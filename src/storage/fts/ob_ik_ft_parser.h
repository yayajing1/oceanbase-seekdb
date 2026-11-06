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

#ifndef _OCEANBASE_STORAGE_FTS_OB_IK_FT_PARSER_H_
#define _OCEANBASE_STORAGE_FTS_OB_IK_FT_PARSER_H_

#include "lib/allocator/ob_allocator.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "data_plane/fts/ob_fts_parser.h"

#include <cstdint>
namespace oceanbase
{
namespace storage
{
class ObFTDictHub;

class ObIKFTParser final : public ObITokenIterator
{
public:
  /**
   * @param[in] scratch_alloc, the allocator of the per-document scratch
   *            (the token containers). It is recycled after every document.
   * @param[in] obj_alloc, the allocator of the objects which live as long as
   *            the parser does (the tokenize context, the segmenters and the
   *            pinned dictionaries).
   * @param[in] hub, the global dictionary hub.
   */
  ObIKFTParser(ObIAllocator &scratch_alloc, ObIAllocator &obj_alloc, ObFTDictHub *hub)
      : allocator_(scratch_alloc),
        obj_alloc_(obj_alloc),
        is_inited_(false),
        pooled_(false),
        built_(false),
        coll_type_(ObCollationType::CS_TYPE_INVALID),
        ctx_(nullptr),
        hub_(hub),
        segmenters_(obj_alloc),
        cache_main_(obj_alloc),
        cache_quan_(obj_alloc),
        cache_stop_(obj_alloc),
        dict_main_(nullptr),
        dict_quan_(nullptr),
        dict_stop_(nullptr)
  {
  }

  virtual ~ObIKFTParser() { reset(); }

  int init(const ObFTParserParam &param);

  /**
   * Re-bind an already built parser to a new document. The dictionaries, the
   * tokenize context and the segmenters are reused as they are, only the
   * per-document state is re-applied, so that the produced tokens are exactly
   * the ones of a freshly built parser.
   */
  int reuse_parser(const ObFTParserParam &param);

  /**
   * Drop the per-document state of the parser, so that it can be reused for
   * the next document. The long-lived objects (the dictionaries, the context
   * and the segmenters) are kept.
   */
  void release_to_pool();

  bool is_built() const { return built_; }
  bool is_pooled() const { return pooled_; }
  void set_pooled() { pooled_ = true; }

  int get_next_token(const char *&word,
                     int64_t &word_len,
                     int64_t &char_cnt,
                     int64_t &word_freq) override;

  VIRTUAL_TO_STRING_KV(K(is_inited_));

private:
  int produce();

  int process_next_batch();

  int process_one_char(TokenizeContext &ctx,
                       const char *ch,
                       const uint8_t char_len,
                       const ObFTCharUtil::CharType type);

private:
  int init_dict(const ObFTParserParam &param);

  int init_single_dict(ObFTDictDesc desc, ObFTCacheRangeContainer &container);

  int init_segmenter(const ObFTParserParam &param);

  int init_ctx(const ObFTParserParam &param);

  void reset();

  bool should_read_newest_table() const;

  int build_dict_from_cache(const ObFTDictDesc &desc,
                            ObFTCacheRangeContainer &container,
                            ObIFTDict *&dict);

private:
  static constexpr int SEGMENT_LIMIT = 1000;
  // the per-document scratch
  ObIAllocator &allocator_;
  // the storage of the objects which live as long as the parser does
  ObIAllocator &obj_alloc_;
  bool is_inited_;
  // the parser is owned by a per-thread pool and must not be destroyed by
  // the caller of the parser descriptor
  bool pooled_;
  // the long-lived objects are built and usable
  bool built_;

  ObCollationType coll_type_;
  TokenizeContext *ctx_;
  ObFTDictHub *hub_;
  ObList<ObIIKProcessor *, ObIAllocator> segmenters_;

  // For now there's no change of dict in one query, so we can pin dict this level.
  ObFTCacheRangeContainer cache_main_;
  ObFTCacheRangeContainer cache_quan_;
  ObFTCacheRangeContainer cache_stop_;

  ObIFTDict *dict_main_;
  ObIFTDict *dict_quan_;
  ObIFTDict *dict_stop_;

  DISABLE_COPY_ASSIGN(ObIKFTParser);
};

class ObIKFTParserDesc final : public ObIFTParserDesc
{
public:
  ObIKFTParserDesc() {}
  virtual ~ObIKFTParserDesc() = default;
  virtual int segment(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual void free_token_iter(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual int get_add_word_flag(ObAddWordFlag &flag) const override;
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_OB_IK_FT_PARSER_H_
