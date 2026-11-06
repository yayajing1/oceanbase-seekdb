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

#ifndef OB_BENG_FT_PARSER_H_
#define OB_BENG_FT_PARSER_H_

#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/ob_print_utils.h"
#include "share/text_analysis/ob_text_analyzer.h"
#include "data_plane/fts/ob_fts_parser.h"

namespace oceanbase
{
namespace storage
{

class ObBEngFTParser final : public ObITokenIterator
{
public:
  static const int64_t FT_MIN_WORD_LEN = 3;
  static const int64_t FT_MAX_WORD_LEN = 84;
public:
  /**
   * @param[in] obj_alloc, the allocator of the objects which live as long as
   *            the parser does (the analysis pipeline).
   */
  explicit ObBEngFTParser(common::ObIAllocator &obj_alloc)
    : allocator_(nullptr),
      obj_alloc_(obj_alloc),
      analysis_ctx_(),
      english_analyzer_(),
      doc_(),
      token_stream_(nullptr),
      is_inited_(false),
      pooled_(false),
      built_(false)
  {}
  ~ObBEngFTParser() { reset(); }

  int init(ObFTParserParam *param);
  /**
   * Re-bind an already built parser to a new document. The analysis
   * pipeline is reused as it is, only the document is re-opened, so that the
   * produced tokens are exactly the ones of a freshly built parser.
   */
  int reuse_parser(ObFTParserParam *param);
  /**
   * Drop the per-document state of the parser, so that it can be reused for
   * the next document.
   */
  void release_to_pool();
  bool is_built() const { return built_; }
  bool is_pooled() const { return pooled_; }
  void set_pooled() { pooled_ = true; }
  void reset();
  virtual int get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq) override;

  VIRTUAL_TO_STRING_KV(K_(analysis_ctx), K_(english_analyzer), KP_(token_stream), K_(is_inited));
private:
  int segment(
      const common::ObDatum &doc,
      share::ObITokenStream *&token_stream);
private:
  // the allocator of the caller of the current document, it owns the copies
  // of the produced tokens
  common::ObIAllocator *allocator_;
  // the storage of the objects which live as long as the parser does
  common::ObIAllocator &obj_alloc_;
  share::ObTextAnalysisCtx analysis_ctx_;
  share::ObEnglishTextAnalyzer english_analyzer_;
  common::ObDatum doc_;
  share::ObITokenStream *token_stream_;
  bool is_inited_;
  // the parser is owned by a per-thread pool and must not be destroyed by
  // the caller of the parser descriptor
  bool pooled_;
  // the long-lived objects are built and usable
  bool built_;

  DISALLOW_COPY_AND_ASSIGN(ObBEngFTParser);
};

class ObBasicEnglishFTParserDesc final : public ObIFTParserDesc
{
public:
  ObBasicEnglishFTParserDesc();
  virtual ~ObBasicEnglishFTParserDesc() = default;
  virtual int segment(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual void free_token_iter(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual int get_add_word_flag(ObAddWordFlag &flag) const override;
};

} // end namespace storage
} // end namespace oceanbase

#endif // OB_BENG_FT_PARSER_H_
