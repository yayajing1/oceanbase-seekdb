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

#ifndef _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_
#define _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include "ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_dict.h"

#include <cstddef>
#include <cstring>

/* File Description:
 * @brief tools to build a double array trie dictionary.
 */

namespace oceanbase
{
namespace storage
{
/* @class ObArrayHashMap
 * @description: a fixed size hash map, used to store words and their code.
 */
class ObArrayHashMap final
{
public:
  struct Entry
  {
    ObFTSingleWord word;
    ObFTWordCode code;
    bool used = false;
  } __attribute__((packed));

  struct Header
  {
    uint32_t buffer_size_;
    uint32_t capacity_;
    uint32_t count_;
    Entry data[1];
  } __attribute__((packed));

  static size_t estimate_capacity(size_t word_num);

  static size_t estimate_size(size_t word_num);

  size_t capacity() const { return header_.capacity_; }

  int init(size_t word_num);

  // no duplicate should be inserted
  int insert(const ObString &key, ObFTWordCode code);

  int find(const ObFTSingleWord &word, ObFTWordCode &code) const;

private:
  struct Header header_;
} __attribute__((packed));

/**
 * @class ObFTDATNode
 * @brief One node of the double array trie.
 *
 * The base value and the check value of a node are always read together, so
 * they are stored together.  The node array is large and every character of
 * every document reads a node of it, so a node which holds both values is
 * touched by one cache line instead of two: with two separate arrays every
 * step of the trie costs two random accesses into a multi megabyte array.
 */
struct ObFTDATNode
{
  int32_t base_;
  int32_t check_;
};

/**
 * @class ObFTDAT
 * @brief ObFTDATrie data structure, which is a memory block, which contains
 *          the code map(optional), the base and check array, and the data block(optional).
 */
struct ObFTDAT
{
public:
  static constexpr int32_t FIRST_BASE = 1;
  static constexpr int32_t FIRST_INDEX = 1;
  // all size in bytes
  int64_t mem_block_size_ = 0;
  // node_num_: number of nodes in the trie.
  size_t node_num_ = 0;
  // word_num_: number of words in the trie.
  size_t word_num_ = 0;
  // array_size_ for the base and check array.
  size_t array_size_ = 0;

  // start offset of every data block
  size_t map_offset_ = 0;
  size_t base_offset_ = 0;
  size_t check_offset_ = 0;

  // use if offset is not 0, it means the data is in the data block
  size_t data_index_offset_ = 0;
  // use if offset is not 0
  size_t data_offset_ = 0;
  // a buffer to store dat
  ObFTSingleWord start_word_;
  ObFTSingleWord end_word_;
  char buff[1] = {0};

public:
  ObArrayHashMap *get_map();
} __attribute__((packed));

template <typename DataType>
class ObFTTrie;

/**
 * @class OBFTDATBuilder
 * @brief oceanbase double array trie builder, build a trie and then a dat.
 */
template <typename DATA_TYPE = void>
class ObFTDATBuilder
{
public:
  ObFTDATBuilder(ObIAllocator &alloc)
      : alloc_(alloc), dat_(nullptr), map_(nullptr), is_inited_(false), next_code_(1)
  {
  }

  ~ObFTDATBuilder() {}

  int init(ObFTTrie<DATA_TYPE> &trie);

  int build_from_trie(ObFTTrie<DATA_TYPE> &trie);

  // get and put to kv_cache
  int get_mem_block(ObFTDAT *&mem, size_t &mem_len)
  {
    mem = dat_;
    if (nullptr == dat_) {
      mem_len = 0;
    } else {
      mem_len = dat_->mem_block_size_;
    }
    return OB_SUCCESS;
  }

private:
  int expand();

  int encode(const ObString &word, ObFTWordCode &code, bool add);

private:
  common::ObIAllocator &alloc_;
  ObFTDAT *dat_;
  ObArrayHashMap *map_;

  bool is_inited_;
  int32_t next_code_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTDATBuilder);
};

/**
 * @class ObFTDATReader
 * @brief Reader to do match on a mem block dat.
 */
template <typename DataType = void>
class ObFTDATReader
{
public:
  ObFTDATReader(ObFTDAT *dat) : dat_(dat)
  {
    clear_char_code_cache();
  }

  // Match single word with hit.
  int match_with_hit(const ObString &single_word,
                     const ObDATrieHit &last_hit,
                     ObDATrieHit &hit) const;

private:
  /**
   * The code of a character is looked up in the word map of the dat, and the
   * map is a hash map whose key is the character itself.  The hash of the key
   * is a murmur hash, which is a lot of work for a key of one to four bytes,
   * and the hash is computed again for every single character of every
   * document.  The code of a character never changes, so the result of the
   * lookup is remembered here.  The cache is a direct mapped table of the
   * character bytes, and it is private to the reader, so no synchronization is
   * needed: a reader belongs to one dictionary of one parser, and a parser is
   * owned by one thread.
   *
   * The first step of the double array trie is remembered as well.  It starts
   * at the root of the trie, so its result only depends on the code of the
   * character, and it is the step which every character of every document
   * needs.
   */
  int find_char_code(const ObString &ft_char,
                     ObFTWordCode &code,
                     int32_t &first_idx,
                     int32_t &first_is_leaf) const;

  void clear_char_code_cache()
  {
    // Only the validity of the slots is reset, the entries themselves are
    // read only when their slot is valid.  Clearing the whole table would
    // cost more than the lookups which it saves, because a parser is built
    // for every document.
    memset(valid_bits_, 0, sizeof(valid_bits_));
  }

  // The cache is a direct mapped table.  It is much smaller than the word
  // map of the dat, so it stays in the cpu cache while the word map does
  // not, which is the real win: the lookup of the word map misses the cache
  // on nearly every character.
  static constexpr int64_t CHAR_CODE_CACHE_SIZE = 2048; // power of two
  static constexpr int32_t CHAR_CODE_NOT_FOUND = -1;
  // the value of first_idx_ when the character has no child in the trie
  static constexpr int32_t CHAR_CODE_NO_CHILD = -1;
  // the child of the root is a leaf of the trie
  static constexpr int32_t CHAR_CODE_FIRST_IS_LEAF = 0x40000000;

  struct CharCodeEntry
  {
    uint64_t key_;
    int32_t code_;
    // the index of the child of the root, CHAR_CODE_NO_CHILD when the
    // character starts no word of the dictionary, the flag
    // CHAR_CODE_FIRST_IS_LEAF is set when that child is a leaf of the trie
    int32_t first_idx_;
  } __attribute__((packed));

  static constexpr int64_t CHAR_CODE_VALID_WORDS = CHAR_CODE_CACHE_SIZE / 64;

  mutable CharCodeEntry char_code_cache_[CHAR_CODE_CACHE_SIZE];
  mutable uint64_t valid_bits_[CHAR_CODE_VALID_WORDS];

private:
  ObFTDAT *dat_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTDATReader);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_
