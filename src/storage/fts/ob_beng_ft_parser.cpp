/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_struct.h"

#include "lib/allocator/page_arena.h"
#include "lib/thread_local/ob_tsi_factory.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{
namespace
{
// The per-thread pool of the reusable basic english parsers.
//
// Scope: strictly per thread (the thread specific information storage of the
// server), so a pooled parser is never touched by two threads at once and no
// lock is needed on the hot path. The pool holds a single parser: the
// tokenization of a document is not re-entrant, so a nested request of the
// same thread falls back to the legacy per-call path.
//
// Lifetime: the pool is destroyed when the thread exits, which destroys the
// parser and its analysis pipeline.
struct ObBEngParserPool
{
  ObBEngParserPool()
      : obj_alloc_(lib::ObMemAttr("FTBengPoolMeta")), parser_(nullptr), in_use_(false)
  {
  }

  ~ObBEngParserPool()
  {
    if (OB_NOT_NULL(parser_)) {
      // Destroy the parser explicitly, so that the analysis pipeline is
      // released before the arena is released.
      parser_->~ObBEngFTParser();
      parser_ = nullptr;
    }
  }

  // the storage of the objects which live as long as the parser does
  ObArenaAllocator obj_alloc_;
  ObBEngFTParser *parser_;
  bool in_use_;
};

ObBEngParserPool *get_beng_parser_pool()
{
  // GET_TSI returns the instance of the current thread, it is destroyed when
  // the thread exits.
  ObBEngParserPool *pool = GET_TSI(ObBEngParserPool);
  // The thread specific information storage uses 0x1 as a placeholder while
  // the instance is being created, so a non-null pointer is not enough.
  if (OB_ISNULL(pool) || reinterpret_cast<uint64_t>(pool) == 1) {
    pool = nullptr;
  }
  return pool;
}

// Prepare the pooled parser of the current thread for the given document.
// It returns OB_SUCCESS with pooled == true when the parser is ready to be
// used. When the pool cannot serve the request, pooled stays false and the
// caller uses the legacy per-call path.
int acquire_beng_parser(ObFTParserParam *param, ObBEngFTParser *&parser, bool &pooled)
{
  int ret = OB_SUCCESS;
  parser = nullptr;
  pooled = false;
  ObBEngParserPool *pool = get_beng_parser_pool();
  if (OB_ISNULL(pool) || pool->in_use_) {
    // No free pooled parser of this thread: the caller uses the legacy path.
  } else {
    pool->in_use_ = true;
    if (OB_ISNULL(pool->parser_)) {
      ObBEngFTParser *new_parser = OB_NEWx(ObBEngFTParser, &pool->obj_alloc_, pool->obj_alloc_);
      if (OB_ISNULL(new_parser)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        new_parser->set_pooled();
        pool->parser_ = new_parser;
      }
    }
    if (OB_SUCC(ret)) {
      int tmp_ret = pool->parser_->reuse_parser(param);
      if (OB_SUCCESS != tmp_ret) {
        ret = tmp_ret;
      } else {
        parser = pool->parser_;
        pooled = true;
      }
    }
    if (OB_FAIL(ret)) {
      // The pooled parser stays in the pool, but it is not in use anymore.
      pool->in_use_ = false;
      parser = nullptr;
    }
  }
  return ret;
}

// Let the pooled parser sleep in its slot until the next document of this
// thread.
void release_beng_parser()
{
  ObBEngParserPool *pool = get_beng_parser_pool();
  if (OB_NOT_NULL(pool)) {
    if (OB_NOT_NULL(pool->parser_)) {
      pool->parser_->release_to_pool();
    }
    pool->in_use_ = false;
  }
}
} // anonymous namespace

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  ObDatum token;
  int64_t token_freq = 0;
  char *buf = nullptr;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(token_stream_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(token_stream_->get_next(token, token_freq))) {
    if (OB_ITER_END != ret) {
    }
  } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 >= token.len_ || 0 >= token_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator_->alloc(token.len_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, token.ptr_, token.len_);
    word = buf;
    word_len = token.len_;
    char_len = token.len_;
    word_freq = token_freq;
    LOG_DEBUG("succeed to add word", K(ObString(word_len, word)), K(word_freq));
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    allocator_ = param->allocator_;
    doc_.set_string(param->fulltext_, param->ft_length_);
    analysis_ctx_.cs_ = param->cs_;
    analysis_ctx_.filter_stopword_ = false;
    analysis_ctx_.need_grouping_ = false;
    if (OB_FAIL(english_analyzer_.init(analysis_ctx_, obj_alloc_))) {
    } else if (OB_FAIL(segment(doc_, token_stream_))) {
    } else if (OB_ISNULL(token_stream_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
      built_ = true;
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObBEngFTParser::reuse_parser(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!built_)) {
    // The analysis pipeline was never built (or it was torn down by a failed
    // init): build it through the regular init path.
    ret = init(param);
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    allocator_ = param->allocator_;
    doc_.set_string(param->fulltext_, param->ft_length_);
    analysis_ctx_.cs_ = param->cs_;
    analysis_ctx_.filter_stopword_ = false;
    analysis_ctx_.need_grouping_ = false;
    // The analysis pipeline is reused as it is, only the document is
    // re-opened on it.
    if (OB_FAIL(segment(doc_, token_stream_))) {
    } else if (OB_ISNULL(token_stream_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
    }
  }
  return ret;
}

void ObBEngFTParser::release_to_pool()
{
  if (OB_UNLIKELY(!pooled_)) {
    // The legacy per-call parser is destroyed by free_token_iter: there is
    // nothing to recycle here.
    return;
  }
  // The position of the analysis pipeline is reset when the next document is
  // opened on it, so only the inited flag and the reference to the allocator
  // of the caller have to be dropped here.
  is_inited_ = false;
  allocator_ = nullptr;
}

int ObBEngFTParser::segment(
    const common::ObDatum &doc,
    share::ObITokenStream *&token_stream)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(doc.ptr_) || OB_UNLIKELY(0 >= doc.len_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < doc.len_)) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(english_analyzer_.analyze(doc, token_stream))) {
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  analysis_ctx_.reset();
  english_analyzer_.reset();
  doc_.reset();
  token_stream_ = nullptr;
  allocator_ = nullptr;
  is_inited_ = false;
  built_ = false;
}

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObBEngFTParser *parser = nullptr;
  bool pooled = false;
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(acquire_beng_parser(param, parser, pooled))) {
    // The pooled parser could not be prepared. The legacy path is used as a
    // fallback, so that the reuse of a parser can never break the
    // tokenization.
    parser = nullptr;
    pooled = false;
    ret = OB_SUCCESS;
  }

  if (OB_SUCC(ret) && !pooled) {
    if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, param->allocator_, *(param->allocator_)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(parser->init(param))) {
    } else {
      // pass
    }
  }

  if (OB_SUCC(ret)) {
    iter = parser;
  } else if (!pooled) {
    OB_DELETEx(ObBEngFTParser, param->allocator_, parser);
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter)) {
    ObBEngFTParser *parser = static_cast<ObBEngFTParser *>(iter);
    if (parser->is_pooled()) {
      // The parser belongs to the per-thread pool: it is not destroyed here,
      // it only drops the state of the document which has been tokenized.
      release_beng_parser();
    } else {
      abort_unless(nullptr != param);
      abort_unless(nullptr != param->allocator_);
      iter->~ObITokenIterator();
      param->allocator_->free(iter);
    }
    iter = nullptr;
  }
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
