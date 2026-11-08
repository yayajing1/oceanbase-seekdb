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

#include "storage/fts/dict/ob_ft_dat_dict.h"

#include "lib/utility/alloc_assist.h"
#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/list/ob_list.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/fts/dict/ob_ft_trie.h"
namespace oceanbase
{
namespace storage
{
template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::init(ObFTTrie<DATA_TYPE> &trie)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    size_t map_size = ObArrayHashMap::estimate_size(trie.node_num());
    // the node array which follows the word map is aligned, so that a node is
    // read with one aligned access
    map_size = (map_size + 7) & ~static_cast<size_t>(7);
    size_t array_size = trie.node_num() * 6; // by experience.
    size_t node_size = array_size * sizeof(ObFTDATNode);
    size_t total_size = sizeof(ObFTDAT) + map_size + node_size;

    if (OB_ISNULL(dat_ = static_cast<ObFTDAT *>(alloc_.alloc(total_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      memset(dat_, 0, total_size);
      dat_->mem_block_size_ = total_size;
      dat_->array_size_ = array_size;
      dat_->base_offset_ = map_size;
      dat_->check_offset_ = 0;
      map_ = dat_->get_map();
      if (OB_FAIL(map_->init(trie.node_num()))) {
      }
    }
  }
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::build_from_trie(ObFTTrie<DATA_TYPE> &trie)
{
  typedef typename ObFTTrieNode<DATA_TYPE>::NodeIndex NodeIndex;

  int ret = OB_SUCCESS;
  if (OB_ISNULL(dat_) || OB_ISNULL(dat_->buff)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObFTDATNode *nodes = reinterpret_cast<ObFTDATNode *>(dat_->buff + dat_->base_offset_);
    nodes[ObFTDAT::FIRST_INDEX].base_ = 1;
    ObArenaAllocator alloc(lib::ObMemAttr("Tmp dfs"));
    ObList<ObFTTrieNode<DATA_TYPE> *, ObIAllocator> dfs_queue(alloc);

    // Start to build the DAT.
    trie.root()->dat_build_info_.state_index_ = ObFTDAT::FIRST_BASE;
    dfs_queue.push_back(trie.root());

    int next_base_offset = 0;
    int level = 1;

    while (OB_SUCC(ret) && !dfs_queue.empty()) {
      ObFTTrieNode<DATA_TYPE> *node = dfs_queue.get_first();
      dfs_queue.pop_front();

      int rootIdx = node->dat_build_info_.state_index_;

      // mark as leaf
      if (node->is_leaf_) {
        nodes[rootIdx].base_ = -1;
      }

      if (level != node->dat_build_info_.level_) {
        level = node->dat_build_info_.level_;
        next_base_offset = 0;
      }

      if (node->is_empty()) {
        // nothing
      } else {
        int try_base = 1;
        try_base += next_base_offset;
        int start = try_base;
        bool need_code = true;
        int try_step = 1;

        for (; try_base < dat_->array_size_; try_base += try_step) {
          bool conflict = false;

          if (node->is_empty()) {
          } else {
            for (typename ObList<NodeIndex, ObIAllocator>::iterator iter = node->children_->begin();
                 iter != node->children_->end();
                 ++iter) {
              ObFTWordCode child_code;
              NodeIndex &child_index = *iter;
              if (need_code) {
                if (OB_FAIL(encode(child_index.word_.get_word(), child_code, true))) {
                  LOG_WARN("Failed to encode");
                  break;
                }
                child_index.child_->dat_build_info_.code_ = child_code;
              }
              child_code = child_index.child_->dat_build_info_.code_;
              if (nodes[try_base + child_code].check_ != 0) {
                conflict = true;
              }
            }
          }

          if (OB_FAIL(ret)) {
          } else {
            need_code = false;

            if (!conflict) {
              if (node->is_leaf_) {
                nodes[rootIdx].base_ = -try_base;
              } else {
                nodes[rootIdx].base_ = try_base;
              }
              break;
            }
            try_step = MAX(1, (try_base - start) / 20);
          }
        }

        if (OB_FAIL(ret) || try_base >= dat_->array_size_) {
          break;
        } else {
          // try_average_.add(try_base + 1 - start);
          if (try_base - start > 100) {
            next_base_offset += try_base - start;
          }

          if (node->is_empty()) {
          } else {
            for (typename ObList<NodeIndex, ObIAllocator>::iterator iter = node->children_->begin();
                 iter != node->children_->end();
                 iter++) {
              typename ObFTTrieNode<DATA_TYPE>::NodeIndex &child_index = *iter;
              int my_index = abs(try_base) + child_index.child_->dat_build_info_.code_;
              // max_used = MAX(max_used, my_index);

              if (my_index >= dat_->array_size_) {
                expand();
                nodes = reinterpret_cast<ObFTDATNode *>(dat_->buff + dat_->base_offset_);
              }
              child_index.child_->dat_build_info_.state_index_ = my_index;
              nodes[my_index].check_ = rootIdx;
              dfs_queue.push_back(child_index.child_);
            }
          }
        }
      }

    } // while

    if (OB_FAIL(ret)) {
      // already logged
    } else if (OB_FAIL(trie.get_start_word(dat_->start_word_))) {
    } else if (OB_FAIL(trie.get_end_word(dat_->end_word_))) {
    }
    LOG_INFO("build dat finished", K(dat_->start_word_.get_word()), K(dat_->end_word_.get_word()));

    dfs_queue.reset();
    alloc.reset();
  }

  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::expand()
{
  int ret = OB_SUCCESS;
  size_t array_size = dat_->array_size_;
  size_t node_inc = array_size * sizeof(ObFTDATNode);
  size_t buffer_size = dat_->mem_block_size_ + node_inc;

  ObFTDAT *new_dat = nullptr;
  if (OB_ISNULL(new_dat = static_cast<ObFTDAT *>(alloc_.alloc(buffer_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    memset(new_dat, 0, buffer_size);
    new_dat->mem_block_size_ = buffer_size;
    new_dat->array_size_ = array_size * 2;
    new_dat->base_offset_ = dat_->base_offset_;
    new_dat->check_offset_ = 0;
    // the word map and the nodes which are already used
    MEMCPY(new_dat->buff, dat_->buff, dat_->base_offset_ + array_size * sizeof(ObFTDATNode));
    alloc_.free(dat_);
    dat_ = new_dat;
    map_ = dat_->get_map();
  }
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::encode(const ObString &word, ObFTWordCode &code, bool add)
{
  int ret = OB_SUCCESS;
  ObFTWordCode code_value;
  ObFTSingleWord single_word;
  single_word.set_word(word.ptr(), word.length());
  ret = map_->find(single_word, code);
  if (OB_ENTRY_NOT_EXIST == ret) {
    if (add) {
      code = next_code_++;
      ret = map_->insert(word, code);
      if (OB_SUCCESS != ret) {
      }
    } else {
      // do nothing
    }
  } else {
    // nothing
  }
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATReader<DATA_TYPE>::find_char_code(const ObString &ft_char,
                                             ObFTWordCode &code,
                                             int32_t &first_idx,
                                             int32_t &first_is_leaf) const
{
  int ret = OB_SUCCESS;
  first_idx = 0;
  first_is_leaf = 0;
  const int32_t char_len = ft_char.length();
  const ObFTDATNode *nodes = reinterpret_cast<const ObFTDATNode *>(dat_->buff + dat_->base_offset_);
  // The first step of the trie starts at the root, so its result only depends
  // on the code of the character.
  auto calc_first_step = [&](const ObFTWordCode the_code) {
    const int32_t idx = abs(nodes[1].base_) + the_code;
    if (nodes[idx].check_ != 1) {
      first_idx = CHAR_CODE_NO_CHILD;
      first_is_leaf = 0;
    } else {
      first_idx = idx;
      first_is_leaf = (nodes[idx].base_ < 0) ? 1 : 0;
    }
  };
  if (char_len <= 0 || char_len > common::ObCharset::MAX_MB_LEN) {
    // Not a normal single character, use the original path.
    ObFTSingleWord word;
    word.set_word(ft_char.ptr(), ft_char.length());
    ret = dat_->get_map()->find(word, code);
    if (OB_SUCC(ret)) {
      calc_first_step(code);
    }
    return ret;
  }

  // The key of the cache is the length of the character and its bytes.  The
  // bytes of a utf8 character are packed in little endian order, so that the
  // key of two different characters never collides.
  uint64_t key = static_cast<uint64_t>(char_len) << 32;
  for (int32_t i = 0; i < char_len; ++i) {
    key |= static_cast<uint64_t>(static_cast<uint8_t>(ft_char.ptr()[i])) << (8 * i);
  }

  const uint64_t slot = ((key * 0x9E3779B97F4A7C15ULL) >> 53) & (CHAR_CODE_CACHE_SIZE - 1);
  const uint64_t bit = 1ULL << (slot & 63);
  const uint64_t bit_word = slot >> 6;
  if (0 != (valid_bits_[bit_word] & bit)) {
    const CharCodeEntry &entry = char_code_cache_[slot];
    if (entry.key_ == key) {
      if (CHAR_CODE_NOT_FOUND == entry.code_) {
        return OB_ENTRY_NOT_EXIST;
      }
      code = entry.code_;
      if (entry.first_idx_ > 0) {
        first_idx = entry.first_idx_ & ~CHAR_CODE_FIRST_IS_LEAF;
        first_is_leaf = (0 != (entry.first_idx_ & CHAR_CODE_FIRST_IS_LEAF)) ? 1 : 0;
      } else {
        first_idx = entry.first_idx_;
        first_is_leaf = 0;
      }
      return OB_SUCCESS;
    }
  }

  // The character is not cached yet, look it up in the word map of the dat
  // and remember the result, so that the next occurrence of the character
  // does not touch the word map again.
  ObFTSingleWord ft_word;
  ft_word.set_word(ft_char.ptr(), ft_char.length());
  ret = dat_->get_map()->find(ft_word, code);
  if (OB_FAIL(ret) && OB_ENTRY_NOT_EXIST != ret) {
    return ret;
  }
  CharCodeEntry &entry = char_code_cache_[slot];
  entry.key_ = key;
  if (OB_SUCC(ret)) {
    // The first step of the trie is remembered as well: it starts at the root
    // of the trie, so its result only depends on the code.
    calc_first_step(code);
    entry.code_ = code;
    entry.first_idx_ = first_idx | (first_is_leaf ? CHAR_CODE_FIRST_IS_LEAF : 0);
  } else {
    entry.code_ = CHAR_CODE_NOT_FOUND;
    entry.first_idx_ = CHAR_CODE_NO_CHILD;
  }
  valid_bits_[bit_word] |= bit;
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATReader<DATA_TYPE>::match_with_hit(const ObString &ft_char,
                                             const ObDATrieHit &last_hit,
                                             ObDATrieHit &hit) const
{
  int ret = OB_SUCCESS;
  ObFTWordCode code = 0;
  int32_t first_idx = 0;
  int32_t first_is_leaf = 0;
  hit.set_unmatch();
  auto *map = dat_->get_map();
  ObFTDATNode *nodes = reinterpret_cast<ObFTDATNode *>(dat_->buff + dat_->base_offset_);

  if (OB_FAIL(find_char_code(ft_char, code, first_idx, first_is_leaf)) && OB_ENTRY_NOT_EXIST != ret) {
  } else if (OB_ENTRY_NOT_EXIST == ret) {
    ret = OB_SUCCESS;
    hit.set_unmatch();
  } else {
    if (&last_hit == &hit) {
      // sameone
    } else {
      hit = last_hit;
    }
    if (0 == hit.base_idx_) {
      // The match starts at the root of the trie.  The result of the first
      // step is taken from the cache of the character, so that the node array
      // of the trie is not read again for every character.
      hit.base_idx_ = 1;
      if (first_idx > 0) {
        hit.base_idx_ = first_idx;
        hit.end_pos_ = hit.end_pos_ + ft_char.length();
        hit.char_cnt_++;
        hit.set_prefix();
        if (0 != first_is_leaf) {
          hit.set_match();
        }
      } else {
        hit.set_unmatch();
      }
    } else {
      // from current place to do check
      int32_t idx = abs(nodes[hit.base_idx_].base_) + code;

      if (nodes[idx].check_ != hit.base_idx_) {
        hit.set_unmatch();
      } else if (nodes[idx].base_ < 0) {
        hit.base_idx_ = idx;
        hit.end_pos_ = hit.end_pos_ + ft_char.length();
        hit.char_cnt_++;
        hit.set_match();
        hit.set_prefix(); // to be checked next time
      } else {
        hit.base_idx_ = idx;
        hit.end_pos_ = hit.end_pos_ + ft_char.length();
        hit.char_cnt_++;
        hit.set_prefix();
      }
    }
  }
  return ret;
}

template class ObFTDATBuilder<void>;
template class ObFTDATReader<void>;

ObArrayHashMap *ObFTDAT::get_map() { return reinterpret_cast<ObArrayHashMap *>(buff); }

int ObArrayHashMap::find(const ObFTSingleWord &word, ObFTWordCode &code) const
{
  int ret = OB_ENTRY_NOT_EXIST;
  uint64_t hash = word.get_word().hash();
  uint64_t index = hash % header_.capacity_;
  while (header_.data[index].used) {
    if (header_.data[index].word == word) {
      code = header_.data[index].code;
      ret = OB_SUCCESS;
      break;
    } else {
      index = (index + 3) % header_.capacity_;
    }
  }
  return ret;
}

int ObArrayHashMap::insert(const ObString &key, ObFTWordCode code)
{
  int ret = OB_SUCCESS;
  uint64_t hash = key.hash();
  uint64_t index = hash % header_.capacity_;
  while (header_.data[index].used) {
    index = (index + 3) % header_.capacity_;
  }

  header_.data[index].code = code;
  header_.data[index].word.set_word(key.ptr(), key.length());
  header_.data[index].used = true;
  header_.count_++;
  return ret;
}

int ObArrayHashMap::init(size_t word_num)
{
  int ret = OB_SUCCESS;
  size_t capacity = estimate_capacity(word_num);
  size_t size = estimate_size(word_num);

  memset(this, 0, size);
  header_.buffer_size_ = size;
  header_.capacity_ = capacity;
  header_.count_ = 0;
  return OB_SUCCESS;
}

size_t ObArrayHashMap::estimate_size(size_t word_num)
{
  size_t capacity = estimate_capacity(word_num);
  size_t size = sizeof(Header) + capacity * sizeof(Entry);
  return size;
}

size_t ObArrayHashMap::estimate_capacity(size_t word_num)
{
  constexpr size_t MIN_CAPACITY = 101;                   // prime
  size_t capacity = MAX(word_num * 4 / 3, MIN_CAPACITY); // 75%
  return capacity;
}
} //  namespace storage
} //  namespace oceanbase
