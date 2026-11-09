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

#include "sql/engine/file_table/file_table_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>

namespace oceanbase
{
namespace sql
{
namespace file_table
{

// ===========================================================================
// Snappy (raw block format)
// ===========================================================================

int snappy_decompress(const uint8_t *src, size_t src_size, std::vector<uint8_t> &dst, size_t /*dst_size*/)
{
  if (nullptr == src || 0 == src_size) {
    return FILE_TABLE_ERR_PARSE;
  }
  size_t pos = 0;
  uint64_t uncompressed_len = 0;
  int shift = 0;
  while (true) {
    if (pos >= src_size) {
      return FILE_TABLE_ERR_PARSE;
    }
    uint8_t byte = src[pos++];
    uncompressed_len |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (0 == (byte & 0x80)) {
      break;
    }
    shift += 7;
    if (shift > 56) {
      return FILE_TABLE_ERR_PARSE;
    }
  }
  dst.clear();
  dst.reserve(static_cast<size_t>(uncompressed_len));
  while (pos < src_size) {
    uint8_t tag = src[pos++];
    const uint8_t type = tag & 0x03;
    if (0 == type) {
      // literal
      uint32_t len = (tag >> 2) + 1;
      if (len > 60) {
        const int extra = static_cast<int>(len) - 60;
        uint64_t value = 0;
        for (int i = 0; i < extra; ++i) {
          if (pos >= src_size) {
            return FILE_TABLE_ERR_PARSE;
          }
          value |= static_cast<uint64_t>(src[pos++]) << (8 * i);
        }
        len = static_cast<uint32_t>(value) + 1;
      }
      if (pos + len > src_size) {
        return FILE_TABLE_ERR_PARSE;
      }
      dst.insert(dst.end(), src + pos, src + pos + len);
      pos += len;
    } else {
      // copy
      uint32_t len = 0;
      uint32_t offset = 0;
      if (1 == type) {
        len = 4 + ((tag >> 2) & 0x07);
        if (pos >= src_size) {
          return FILE_TABLE_ERR_PARSE;
        }
        offset = (static_cast<uint32_t>(tag & 0xe0) << 3) | src[pos++];
      } else if (2 == type) {
        len = (tag >> 2) + 1;
        if (pos + 1 >= src_size) {
          return FILE_TABLE_ERR_PARSE;
        }
        offset = static_cast<uint32_t>(src[pos]) | (static_cast<uint32_t>(src[pos + 1]) << 8);
        pos += 2;
      } else {
        len = (tag >> 2) + 1;
        if (pos + 3 >= src_size) {
          return FILE_TABLE_ERR_PARSE;
        }
        offset = static_cast<uint32_t>(src[pos])
                 | (static_cast<uint32_t>(src[pos + 1]) << 8)
                 | (static_cast<uint32_t>(src[pos + 2]) << 16)
                 | (static_cast<uint32_t>(src[pos + 3]) << 24);
        pos += 4;
      }
      if (0 == offset || offset > dst.size()) {
        return FILE_TABLE_ERR_PARSE;
      }
      const size_t start = dst.size() - offset;
      for (uint32_t i = 0; i < len; ++i) {
        dst.push_back(dst[start + i]);
      }
    }
  }
  if (dst.size() != uncompressed_len) {
    dst.clear();
    return FILE_TABLE_ERR_PARSE;
  }
  return FILE_TABLE_OK;
}

// ===========================================================================
// Thrift compact protocol
// ===========================================================================

ThriftCompactReader::ThriftCompactReader(const uint8_t *data, size_t size)
    : data_(data), size_(size)
{
}

bool ThriftCompactReader::read_byte(uint8_t &value)
{
  if (nullptr == data_ || pos_ >= size_) {
    return false;
  }
  value = data_[pos_++];
  return true;
}

bool ThriftCompactReader::read_varint(uint64_t &value)
{
  value = 0;
  int shift = 0;
  while (true) {
    uint8_t byte = 0;
    if (!read_byte(byte)) {
      return false;
    }
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (0 == (byte & 0x80)) {
      break;
    }
    shift += 7;
    if (shift > 63) {
      return false;
    }
  }
  return true;
}

int64_t ThriftCompactReader::read_zigzag()
{
  uint64_t value = 0;
  if (!read_varint(value)) {
    return 0;
  }
  return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
}

bool ThriftCompactReader::read_field_header(int16_t &field_id, int8_t &type)
{
  uint8_t byte = 0;
  if (!read_byte(byte)) {
    return false;
  }
  if (0 == byte) {
    return false;  // stop
  }
  type = static_cast<int8_t>(byte & 0x0f);
  const int16_t delta = static_cast<int16_t>((byte & 0xf0) >> 4);
  if (0 == delta) {
    // the absolute field id is written as a zigzag encoded varint
    field_id = static_cast<int16_t>(read_zigzag());
  } else {
    field_id = static_cast<int16_t>(last_field_id_ + delta);
  }
  last_field_id_ = field_id;
  return true;
}

bool ThriftCompactReader::read_list_header(int8_t &elem_type, int32_t &size)
{
  uint8_t byte = 0;
  if (!read_byte(byte)) {
    return false;
  }
  elem_type = static_cast<int8_t>(byte & 0x0f);
  uint32_t count = static_cast<uint32_t>((byte & 0xf0) >> 4);
  if (15 == count) {
    uint64_t value = 0;
    if (!read_varint(value)) {
      return false;
    }
    count = static_cast<uint32_t>(value);
  }
  size = static_cast<int32_t>(count);
  return true;
}

std::string ThriftCompactReader::read_binary()
{
  uint64_t length = 0;
  if (!read_varint(length)) {
    return std::string();
  }
  if (pos_ + length > size_) {
    return std::string();
  }
  std::string result(reinterpret_cast<const char *>(data_ + pos_), static_cast<size_t>(length));
  pos_ += static_cast<size_t>(length);
  return result;
}

double ThriftCompactReader::read_double()
{
  uint8_t buf[8];
  for (int i = 0; i < 8; ++i) {
    if (!read_byte(buf[i])) {
      return 0.0;
    }
  }
  double value = 0.0;
  memcpy(&value, buf, sizeof(value));
  return value;
}

void ThriftCompactReader::skip(int8_t type)
{
  switch (type) {
    case THRIFT_BOOLEAN_TRUE:
    case THRIFT_BOOLEAN_FALSE:
      break;
    case THRIFT_BYTE: {
      uint8_t byte = 0;
      read_byte(byte);
      break;
    }
    case THRIFT_I16:
    case THRIFT_I32:
    case THRIFT_I64: {
      uint64_t value = 0;
      read_varint(value);
      break;
    }
    case THRIFT_DOUBLE: {
      read_double();
      break;
    }
    case THRIFT_BINARY: {
      read_binary();
      break;
    }
    case THRIFT_LIST:
    case THRIFT_SET: {
      int8_t elem_type = 0;
      int32_t size = 0;
      if (read_list_header(elem_type, size)) {
        for (int32_t i = 0; i < size; ++i) {
          skip(elem_type);
        }
      }
      break;
    }
    case THRIFT_MAP: {
      uint64_t count = 0;
      if (read_varint(count)) {
        if (count > 0) {
          uint8_t types = 0;
          read_byte(types);
          const int8_t key_type = static_cast<int8_t>((types & 0xf0) >> 4);
          const int8_t value_type = static_cast<int8_t>(types & 0x0f);
          for (uint64_t i = 0; i < count; ++i) {
            skip(key_type);
            skip(value_type);
          }
        }
      }
      break;
    }
    case THRIFT_STRUCT: {
      // a nested struct starts a new field id delta context
      const int16_t saved = field_id_context();
      set_field_id_context(0);
      int16_t field_id = 0;
      int8_t field_type = 0;
      while (read_field_header(field_id, field_type)) {
        skip(field_type);
      }
      set_field_id_context(saved);
      break;
    }
    default:
      break;
  }
}

// ===========================================================================
// Parquet metadata
// ===========================================================================

namespace
{

enum ParquetPhysicalType {
  PQ_BOOLEAN = 0,
  PQ_INT32 = 1,
  PQ_INT64 = 2,
  PQ_INT96 = 3,
  PQ_FLOAT = 4,
  PQ_DOUBLE = 5,
  PQ_BYTE_ARRAY = 6,
  PQ_FIXED_LEN_BYTE_ARRAY = 7
};

enum ParquetPageType {
  PQ_PAGE_DATA = 0,
  PQ_PAGE_INDEX = 1,
  PQ_PAGE_DICTIONARY = 2,
  PQ_PAGE_DATA_V2 = 3
};

enum ParquetCodec {
  PQ_CODEC_UNCOMPRESSED = 0,
  PQ_CODEC_SNAPPY = 1,
  PQ_CODEC_GZIP = 2,
  PQ_CODEC_LZO = 3,
  PQ_CODEC_BROTLI = 4,
  PQ_CODEC_LZ4 = 5,
  PQ_CODEC_ZSTD = 6,
  PQ_CODEC_LZ4_RAW = 7
};

enum ParquetEncoding {
  PQ_ENC_PLAIN = 0,
  PQ_ENC_PLAIN_DICTIONARY = 2,
  PQ_ENC_RLE = 3,
  PQ_ENC_BIT_PACKED = 4,
  PQ_ENC_DELTA_BINARY_PACKED = 5,
  PQ_ENC_DELTA_LENGTH_BYTE_ARRAY = 6,
  PQ_ENC_DELTA_BYTE_ARRAY = 7,
  PQ_ENC_RLE_DICTIONARY = 8
};

struct ParquetSchemaElement {
  int32_t type_ = -1;
  int32_t repetition_ = -1;  // 0 required, 1 optional, 2 repeated
  int32_t num_children_ = 0;
  int32_t converted_type_ = -1;
  int32_t type_length_ = 0;
  std::string name_;
};

struct ParquetColumnMeta {
  int32_t type_ = -1;
  int32_t codec_ = PQ_CODEC_UNCOMPRESSED;
  int32_t type_length_ = 0;
  int64_t num_values_ = 0;
  int64_t data_page_offset_ = 0;
  int64_t dictionary_page_offset_ = 0;
  int64_t total_compressed_size_ = 0;
  int64_t total_uncompressed_size_ = 0;
  std::string path_;
};

struct ParquetRowGroup {
  int64_t num_rows_ = 0;
  std::vector<ParquetColumnMeta> columns_;
};

struct ParquetFileMeta {
  int64_t num_rows_ = 0;
  std::vector<ParquetSchemaElement> schema_;
  std::vector<ParquetRowGroup> row_groups_;
};

struct ParquetPageHeader {
  int32_t type_ = PQ_PAGE_DATA;
  int32_t uncompressed_page_size_ = 0;
  int32_t compressed_page_size_ = 0;
  // data page header
  int32_t num_values_ = 0;
  int32_t encoding_ = PQ_ENC_PLAIN;
  int32_t def_level_encoding_ = PQ_ENC_RLE;
  int32_t rep_level_encoding_ = PQ_ENC_RLE;
  // data page header v2
  int32_t num_nulls_ = 0;
  int32_t num_rows_ = 0;
  int32_t def_levels_byte_length_ = 0;
  int32_t rep_levels_byte_length_ = 0;
  bool is_compressed_ = true;
  // dictionary page header
  int32_t dict_num_values_ = 0;
  int32_t dict_encoding_ = PQ_ENC_PLAIN;
};

// ---------------------------------------------------------------------------
// metadata parsing
// ---------------------------------------------------------------------------

// A nested struct starts a new field id delta context.
class FieldIdContextGuard
{
public:
  explicit FieldIdContextGuard(ThriftCompactReader &reader)
      : reader_(reader), saved_(reader.field_id_context())
  {
    reader_.set_field_id_context(0);
  }
  ~FieldIdContextGuard() { reader_.set_field_id_context(saved_); }
private:
  ThriftCompactReader &reader_;
  int16_t saved_;
};

ParquetSchemaElement parse_schema_element(ThriftCompactReader &reader)
{
  FieldIdContextGuard guard(reader);
  ParquetSchemaElement element;
  int16_t field_id = 0;
  int8_t type = 0;
  while (reader.read_field_header(field_id, type)) {
    switch (field_id) {
      case 1: element.type_ = static_cast<int32_t>(reader.read_zigzag()); break;
      case 2: element.type_length_ = static_cast<int32_t>(reader.read_zigzag()); break;
      case 3: element.repetition_ = static_cast<int32_t>(reader.read_zigzag()); break;
      case 4: element.name_ = reader.read_binary(); break;
      case 5: element.num_children_ = static_cast<int32_t>(reader.read_zigzag()); break;
      case 6: element.converted_type_ = static_cast<int32_t>(reader.read_zigzag()); break;
      default: reader.skip(type); break;
    }
  }
  return element;
}

ParquetColumnMeta parse_column_metadata(ThriftCompactReader &reader)
{
  FieldIdContextGuard guard(reader);
  ParquetColumnMeta meta;
  int16_t field_id = 0;
  int8_t type = 0;
  while (reader.read_field_header(field_id, type)) {
    switch (field_id) {
      case 1: meta.type_ = static_cast<int32_t>(reader.read_zigzag()); break;
      case 2: {
        int8_t elem_type = 0;
        int32_t size = 0;
        if (reader.read_list_header(elem_type, size)) {
          for (int32_t i = 0; i < size; ++i) {
            reader.skip(elem_type);
          }
        }
        break;
      }
      case 3: {
        int8_t elem_type = 0;
        int32_t size = 0;
        if (reader.read_list_header(elem_type, size)) {
          for (int32_t i = 0; i < size; ++i) {
            std::string part = reader.read_binary();
            if (meta.path_.empty()) {
              meta.path_ = part;
            }
          }
        }
        break;
      }
      case 4: meta.codec_ = static_cast<int32_t>(reader.read_zigzag()); break;
      case 5: meta.num_values_ = reader.read_zigzag(); break;
      case 6: meta.total_uncompressed_size_ = reader.read_zigzag(); break;
      case 7: meta.total_compressed_size_ = reader.read_zigzag(); break;
      case 9: meta.data_page_offset_ = reader.read_zigzag(); break;
      case 10: reader.skip(type); break;  // index_page_offset
      case 11: meta.dictionary_page_offset_ = reader.read_zigzag(); break;
      default: reader.skip(type); break;
    }
  }
  return meta;
}

ParquetColumnMeta parse_column_chunk(ThriftCompactReader &reader)
{
  FieldIdContextGuard guard(reader);
  ParquetColumnMeta meta;
  int16_t field_id = 0;
  int8_t type = 0;
  while (reader.read_field_header(field_id, type)) {
    switch (field_id) {
      case 1: reader.skip(type); break;  // file_path
      case 2: reader.skip(type); break;  // file_offset
      case 3: meta = parse_column_metadata(reader); break;
      default: reader.skip(type); break;
    }
  }
  return meta;
}

ParquetRowGroup parse_row_group(ThriftCompactReader &reader)
{
  FieldIdContextGuard guard(reader);
  ParquetRowGroup group;
  int16_t field_id = 0;
  int8_t type = 0;
  while (reader.read_field_header(field_id, type)) {
    switch (field_id) {
      case 1: {
        int8_t elem_type = 0;
        int32_t size = 0;
        if (reader.read_list_header(elem_type, size)) {
          for (int32_t i = 0; i < size; ++i) {
            group.columns_.push_back(parse_column_chunk(reader));
          }
        }
        break;
      }
      case 2: reader.skip(type); break;  // total_byte_size
      case 3: group.num_rows_ = reader.read_zigzag(); break;
      default: reader.skip(type); break;
    }
  }
  return group;
}

int parse_file_metadata(ThriftCompactReader &reader, ParquetFileMeta &meta)
{
  int16_t field_id = 0;
  int8_t type = 0;
  while (reader.read_field_header(field_id, type)) {
    switch (field_id) {
      case 2: {
        int8_t elem_type = 0;
        int32_t size = 0;
        if (reader.read_list_header(elem_type, size)) {
          for (int32_t i = 0; i < size; ++i) {
            meta.schema_.push_back(parse_schema_element(reader));
          }
        }
        break;
      }
      case 3: meta.num_rows_ = reader.read_zigzag(); break;
      case 4: {
        int8_t elem_type = 0;
        int32_t size = 0;
        if (reader.read_list_header(elem_type, size)) {
          for (int32_t i = 0; i < size; ++i) {
            meta.row_groups_.push_back(parse_row_group(reader));
          }
        }
        break;
      }
      default: reader.skip(type); break;
    }
  }
  return FILE_TABLE_OK;
}

ParquetPageHeader parse_page_header(ThriftCompactReader &reader)
{
  FieldIdContextGuard guard(reader);
  ParquetPageHeader header;
  int16_t field_id = 0;
  int8_t type = 0;
  while (reader.read_field_header(field_id, type)) {
    switch (field_id) {
      case 1: header.type_ = static_cast<int32_t>(reader.read_zigzag()); break;
      case 2: header.uncompressed_page_size_ = static_cast<int32_t>(reader.read_zigzag()); break;
      case 3: header.compressed_page_size_ = static_cast<int32_t>(reader.read_zigzag()); break;
      case 5: {
        // DataPageHeader
        FieldIdContextGuard sub_guard(reader);
        int16_t sub_id = 0;
        int8_t sub_type = 0;
        while (reader.read_field_header(sub_id, sub_type)) {
          switch (sub_id) {
            case 1: header.num_values_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 2: header.encoding_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 3: header.def_level_encoding_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 4: header.rep_level_encoding_ = static_cast<int32_t>(reader.read_zigzag()); break;
            default: reader.skip(sub_type); break;
          }
        }
        break;
      }
      case 7: {
        // DictionaryPageHeader
        FieldIdContextGuard sub_guard(reader);
        int16_t sub_id = 0;
        int8_t sub_type = 0;
        while (reader.read_field_header(sub_id, sub_type)) {
          switch (sub_id) {
            case 1: header.dict_num_values_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 2: header.dict_encoding_ = static_cast<int32_t>(reader.read_zigzag()); break;
            default: reader.skip(sub_type); break;
          }
        }
        break;
      }
      case 8: {
        // DataPageHeaderV2
        FieldIdContextGuard sub_guard(reader);
        int16_t sub_id = 0;
        int8_t sub_type = 0;
        while (reader.read_field_header(sub_id, sub_type)) {
          switch (sub_id) {
            case 1: header.num_values_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 2: header.num_nulls_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 3: header.num_rows_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 4: header.encoding_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 5: header.def_levels_byte_length_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 6: header.rep_levels_byte_length_ = static_cast<int32_t>(reader.read_zigzag()); break;
            case 7: header.is_compressed_ = (THRIFT_BOOLEAN_TRUE == sub_type); break;
            default: reader.skip(sub_type); break;
          }
        }
        break;
      }
      default: reader.skip(type); break;
    }
  }
  return header;
}

// ---------------------------------------------------------------------------
// RLE / bit packing hybrid
// ---------------------------------------------------------------------------

// Decode the RLE / bit packing hybrid encoding.  Returns the number of
// decoded values, the buffer is filled with the decoded values.
int decode_rle_hybrid(const uint8_t *data, size_t size, int32_t bit_width,
                      int64_t count, std::vector<int64_t> &out)
{
  out.clear();
  if (bit_width <= 0) {
    return FILE_TABLE_OK;
  }
  const int32_t byte_width = (bit_width + 7) / 8;
  size_t pos = 0;
  while (static_cast<int64_t>(out.size()) < count && pos < size) {
    // varint header
    uint64_t header = 0;
    int shift = 0;
    while (true) {
      if (pos >= size) {
        return FILE_TABLE_ERR_PARSE;
      }
      uint8_t byte = data[pos++];
      header |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if (0 == (byte & 0x80)) {
        break;
      }
      shift += 7;
      if (shift > 56) {
        return FILE_TABLE_ERR_PARSE;
      }
    }
    if (0 == (header & 1)) {
      // rle run
      const int64_t run_length = static_cast<int64_t>(header >> 1);
      if (pos + byte_width > size) {
        return FILE_TABLE_ERR_PARSE;
      }
      int64_t value = 0;
      for (int32_t i = 0; i < byte_width; ++i) {
        value |= static_cast<int64_t>(data[pos + i]) << (8 * i);
      }
      pos += byte_width;
      for (int64_t i = 0; i < run_length && static_cast<int64_t>(out.size()) < count; ++i) {
        out.push_back(value);
      }
    } else {
      // bit packed run
      const int64_t groups = static_cast<int64_t>(header >> 1);
      const int64_t values = groups * 8;
      const size_t bytes = static_cast<size_t>((values * bit_width + 7) / 8);
      if (pos + bytes > size) {
        return FILE_TABLE_ERR_PARSE;
      }
      for (int64_t i = 0; i < values && static_cast<int64_t>(out.size()) < count; ++i) {
        int64_t value = 0;
        for (int32_t b = 0; b < bit_width; ++b) {
          const int64_t bit_index = i * bit_width + b;
          const uint8_t byte = data[pos + bit_index / 8];
          value |= static_cast<int64_t>((byte >> (bit_index % 8)) & 1) << b;
        }
        out.push_back(value);
      }
      pos += bytes;
    }
  }
  return FILE_TABLE_OK;
}

// Read the levels of a data page v1 (both levels carry a 4 byte length
// prefix when they use the RLE encoding).
int decode_levels_v1(const uint8_t *data, size_t size,
                      int32_t level_encoding, int32_t bit_width, int64_t count,
                      size_t &consumed, std::vector<int64_t> &out)
{
  consumed = 0;
  out.clear();
  if (bit_width <= 0) {
    return FILE_TABLE_OK;
  }
  if (PQ_ENC_BIT_PACKED == level_encoding) {
    // the legacy bit packed encoding, the values are packed back to back
    const size_t bytes = static_cast<size_t>((count * bit_width + 7) / 8);
    if (size < bytes) {
      return FILE_TABLE_ERR_PARSE;
    }
    for (int64_t i = 0; i < count; ++i) {
      int64_t value = 0;
      for (int32_t b = 0; b < bit_width; ++b) {
        const int64_t bit_index = i * bit_width + b;
        const uint8_t byte = data[bit_index / 8];
        value |= static_cast<int64_t>((byte >> (bit_index % 8)) & 1) << b;
      }
      out.push_back(value);
    }
    consumed = bytes;
    return FILE_TABLE_OK;
  }
  if (size < 4) {
    return FILE_TABLE_ERR_PARSE;
  }
  const uint32_t length = static_cast<uint32_t>(data[0])
                          | (static_cast<uint32_t>(data[1]) << 8)
                          | (static_cast<uint32_t>(data[2]) << 16)
                          | (static_cast<uint32_t>(data[3]) << 24);
  if (size < 4 + static_cast<size_t>(length)) {
    return FILE_TABLE_ERR_PARSE;
  }
  const int ret = decode_rle_hybrid(data + 4, length, bit_width, count, out);
  if (FILE_TABLE_OK != ret) {
    return ret;
  }
  consumed = 4 + static_cast<size_t>(length);
  return FILE_TABLE_OK;
}

// ---------------------------------------------------------------------------
// plain value decoding
// ---------------------------------------------------------------------------

std::string format_double(double value)
{
  char buf[64];
  for (int precision = 15; precision <= 17; ++precision) {
    snprintf(buf, sizeof(buf), "%.*g", precision, value);
    if (strtod(buf, nullptr) == value) {
      break;
    }
  }
  return std::string(buf);
}

int decode_plain_values(const uint8_t *data, size_t size, int32_t type, int32_t type_length,
                        int64_t count, std::vector<Cell> &out)
{
  size_t pos = 0;
  for (int64_t i = 0; i < count; ++i) {
    Cell cell;
    cell.is_null_ = false;
    switch (type) {
      case PQ_BOOLEAN: {
        if (pos >= size) {
          return FILE_TABLE_ERR_PARSE;
        }
        const uint8_t byte = data[pos++];
        cell.text_ = (0 != (byte & 0x01)) ? "true" : "false";
        break;
      }
      case PQ_INT32: {
        if (pos + 4 > size) {
          return FILE_TABLE_ERR_PARSE;
        }
        int32_t value = 0;
        memcpy(&value, data + pos, 4);
        pos += 4;
        cell.text_ = std::to_string(value);
        break;
      }
      case PQ_INT64: {
        if (pos + 8 > size) {
          return FILE_TABLE_ERR_PARSE;
        }
        int64_t value = 0;
        memcpy(&value, data + pos, 8);
        pos += 8;
        cell.text_ = std::to_string(value);
        break;
      }
      case PQ_INT96: {
        if (pos + 12 > size) {
          return FILE_TABLE_ERR_PARSE;
        }
        // the first 8 bytes are the nanoseconds of the julian day
        int64_t nanos = 0;
        memcpy(&nanos, data + pos, 8);
        uint32_t julian_day = 0;
        memcpy(&julian_day, data + pos + 8, 4);
        pos += 12;
        cell.text_ = std::to_string(static_cast<int64_t>(julian_day) * 8640000000000LL + nanos);
        break;
      }
      case PQ_FLOAT: {
        if (pos + 4 > size) {
          return FILE_TABLE_ERR_PARSE;
        }
        float value = 0.0f;
        memcpy(&value, data + pos, 4);
        pos += 4;
        cell.text_ = format_double(static_cast<double>(value));
        break;
      }
      case PQ_DOUBLE: {
        if (pos + 8 > size) {
          return FILE_TABLE_ERR_PARSE;
        }
        double value = 0.0;
        memcpy(&value, data + pos, 8);
        pos += 8;
        cell.text_ = format_double(value);
        break;
      }
      case PQ_BYTE_ARRAY: {
        if (pos + 4 > size) {
          return FILE_TABLE_ERR_PARSE;
        }
        uint32_t length = 0;
        memcpy(&length, data + pos, 4);
        pos += 4;
        if (pos + length > size) {
          return FILE_TABLE_ERR_PARSE;
        }
        cell.text_.assign(reinterpret_cast<const char *>(data + pos), length);
        pos += length;
        break;
      }
      case PQ_FIXED_LEN_BYTE_ARRAY: {
        const uint32_t length = static_cast<uint32_t>(type_length > 0 ? type_length : 0);
        if (pos + length > size) {
          return FILE_TABLE_ERR_PARSE;
        }
        cell.text_.assign(reinterpret_cast<const char *>(data + pos), length);
        pos += length;
        break;
      }
      default:
        return FILE_TABLE_ERR_FORMAT;
    }
    out.push_back(cell);
  }
  return FILE_TABLE_OK;
}

// ---------------------------------------------------------------------------
// file access
// ---------------------------------------------------------------------------

class ParquetFile
{
public:
  ~ParquetFile() { close(); }

  int open(const std::string &path)
  {
    close();
    fp_ = fopen(path.c_str(), "rb");
    if (nullptr == fp_) {
      set_last_error("cannot open parquet file: " + path);
      return FILE_TABLE_ERR_IO;
    }
    if (0 != fseek(fp_, 0, SEEK_END)) {
      return FILE_TABLE_ERR_IO;
    }
    const long file_size = ftell(fp_);
    if (file_size < 12) {
      set_last_error("parquet file is too small: " + path);
      return FILE_TABLE_ERR_FORMAT;
    }
    size_ = static_cast<size_t>(file_size);
    if (0 != fseek(fp_, 0, SEEK_SET)) {
      return FILE_TABLE_ERR_IO;
    }
    uint8_t magic[4];
    if (1 != fread(magic, 4, 1, fp_) || 0 != memcmp(magic, "PAR1", 4)) {
      set_last_error("not a parquet file: " + path);
      return FILE_TABLE_ERR_FORMAT;
    }
    // the footer metadata length precedes the trailing magic
    uint8_t tail[8];
    if (0 != fseek(fp_, static_cast<long>(size_ - 8), SEEK_SET)) {
      return FILE_TABLE_ERR_IO;
    }
    if (1 != fread(tail, 8, 1, fp_)) {
      return FILE_TABLE_ERR_IO;
    }
    if (0 != memcmp(tail + 4, "PAR1", 4)) {
      set_last_error("not a parquet file: " + path);
      return FILE_TABLE_ERR_FORMAT;
    }
    const uint32_t meta_len = static_cast<uint32_t>(tail[0])
                               | (static_cast<uint32_t>(tail[1]) << 8)
                               | (static_cast<uint32_t>(tail[2]) << 16)
                               | (static_cast<uint32_t>(tail[3]) << 24);
    if (meta_len + 12 > size_) {
      set_last_error("broken parquet footer: " + path);
      return FILE_TABLE_ERR_FORMAT;
    }
    footer_.resize(meta_len);
    if (0 != fseek(fp_, static_cast<long>(size_ - 8 - meta_len), SEEK_SET)) {
      return FILE_TABLE_ERR_IO;
    }
    if (meta_len > 0 && 1 != fread(&footer_[0], meta_len, 1, fp_)) {
      return FILE_TABLE_ERR_IO;
    }
    ThriftCompactReader reader(footer_.empty() ? nullptr : &footer_[0], footer_.size());
    if (FILE_TABLE_OK != parse_file_metadata(reader, meta_)) {
      return FILE_TABLE_ERR_FORMAT;
    }
    return FILE_TABLE_OK;
  }

  void close()
  {
    if (nullptr != fp_) {
      fclose(fp_);
      fp_ = nullptr;
    }
    footer_.clear();
    footer_.shrink_to_fit();
  }

  int read_at(int64_t offset, size_t length, std::vector<uint8_t> &buffer)
  {
    buffer.resize(length);
    if (0 == length) {
      return FILE_TABLE_OK;
    }
    if (offset < 0 || static_cast<size_t>(offset) + length > size_) {
      return FILE_TABLE_ERR_FORMAT;
    }
    if (0 != fseek(fp_, static_cast<long>(offset), SEEK_SET)) {
      return FILE_TABLE_ERR_IO;
    }
    if (1 != fread(&buffer[0], length, 1, fp_)) {
      return FILE_TABLE_ERR_IO;
    }
    return FILE_TABLE_OK;
  }

  const ParquetFileMeta &meta() const { return meta_; }
  size_t size() const { return size_; }

private:
  std::FILE *fp_ = nullptr;
  size_t size_ = 0;
  std::vector<uint8_t> footer_;
  ParquetFileMeta meta_;
};

// Map a parquet physical type onto an inferred column kind.
ColumnKind parquet_type_kind(int32_t type)
{
  switch (type) {
    case PQ_BOOLEAN: return ColumnKind::BOOLEAN;
    case PQ_INT32:
    case PQ_INT64:
    case PQ_INT96: return ColumnKind::BIGINT;
    case PQ_FLOAT:
    case PQ_DOUBLE: return ColumnKind::DOUBLE;
    case PQ_BYTE_ARRAY:
    case PQ_FIXED_LEN_BYTE_ARRAY: return ColumnKind::VARCHAR;
    default: return ColumnKind::VARCHAR;
  }
}

// One column of the scanned relation.
struct ParquetColumn {
  std::string name_;
  int32_t type_ = -1;
  int32_t repetition_ = 1;
  bool selected_ = true;
};

class ParquetReader : public FileScanReader
{
public:
  ParquetReader()
      : row_group_index_(0),
        rows_emitted_(0)
  {}

  ~ParquetReader() { close(); }

  int open(const std::string &path, const std::vector<std::string> &selected)
  {
    int ret = file_.open(path);
    if (FILE_TABLE_OK != ret) {
      return ret;
    }
    const ParquetFileMeta &meta = file_.meta();
    // the first schema element is the root, the leaves follow
    for (size_t i = 1; i < meta.schema_.size(); ++i) {
      const ParquetSchemaElement &element = meta.schema_.at(i);
      if (element.num_children_ > 0) {
        continue;  // a group node, nested structures are not supported
      }
      ParquetColumn column;
      column.name_ = element.name_;
      column.type_ = element.type_;
      column.repetition_ = (0 == element.repetition_) ? 0 : 1;
      column.selected_ = true;
      if (!selected.empty()) {
        column.selected_ = false;
        for (size_t k = 0; k < selected.size(); ++k) {
          if (selected.at(k) == column.name_) {
            column.selected_ = true;
            break;
          }
        }
      }
      parquet_columns_.push_back(column);
    }
    // build the public schema, only the selected columns are exposed
    for (size_t i = 0; i < parquet_columns_.size(); ++i) {
      if (!parquet_columns_.at(i).selected_) {
        continue;
      }
      ColumnDesc desc;
      desc.name_ = parquet_columns_.at(i).name_;
      desc.kind_ = parquet_type_kind(parquet_columns_.at(i).type_);
      desc.nullable_ = (0 != parquet_columns_.at(i).repetition_);
      if (ColumnKind::VARCHAR == desc.kind_) {
        desc.length_ = 65535;
      }
      columns_.push_back(desc);
    }
    return FILE_TABLE_OK;
  }

  int rescan()
  {
    row_group_index_ = 0;
    rows_emitted_ = 0;
    streams_.clear();
    return FILE_TABLE_OK;
  }

  int close()
  {
    streams_.clear();
    file_.close();
    return FILE_TABLE_OK;
  }

  int get_next_batch(std::vector<std::vector<Cell> > &rows, int64_t max_rows)
  {
    rows.clear();
    const ParquetFileMeta &meta = file_.meta();
    while (static_cast<int64_t>(rows.size()) < max_rows) {
      if (row_group_index_ >= meta.row_groups_.size()) {
        break;
      }
      if (streams_.empty()) {
        int ret = start_row_group(meta.row_groups_.at(row_group_index_));
        if (FILE_TABLE_OK != ret) {
          return ret;
        }
      }
      const size_t before = rows.size();
      int ret = emit_rows(rows, max_rows - static_cast<int64_t>(before));
      if (FILE_TABLE_OK != ret) {
        return ret;
      }
      if (rows.size() == before) {
        // the row group is exhausted
        streams_.clear();
        ++row_group_index_;
      }
    }
    return FILE_TABLE_OK;
  }

private:
  struct Stream {
    ParquetColumnMeta meta_;
    int64_t file_pos_ = 0;
    int64_t values_read_ = 0;
    int64_t values_total_ = 0;
    int32_t def_level_bit_width_ = 0;
    std::vector<Cell> dictionary_;
    std::deque<Cell> pending_;
    bool has_dictionary_ = false;
  };

  int start_row_group(const ParquetRowGroup &group)
  {
    streams_.clear();
    for (size_t i = 0; i < parquet_columns_.size(); ++i) {
      if (!parquet_columns_.at(i).selected_) {
        continue;
      }
      if (i >= group.columns_.size()) {
        continue;
      }
      Stream stream;
      stream.meta_ = group.columns_.at(i);
      stream.values_total_ = stream.meta_.num_values_;
      if (stream.meta_.dictionary_page_offset_ > 0) {
        stream.file_pos_ = stream.meta_.dictionary_page_offset_;
      } else {
        stream.file_pos_ = stream.meta_.data_page_offset_;
      }
      // the definition level bit width is 1 for an optional column
      stream.def_level_bit_width_ = (0 != parquet_columns_.at(i).repetition_) ? 1 : 0;
      streams_.push_back(stream);
    }
    return FILE_TABLE_OK;
  }

  int read_next_page(Stream &stream, std::vector<Cell> &values)
  {
    std::vector<uint8_t> header_buf;
    // read the page header, the thrift compact reader needs the whole header
    int ret = read_page_header(stream.file_pos_, header_buf, stream.file_pos_);
    if (FILE_TABLE_OK != ret) {
      return ret;
    }
    const ParquetPageHeader header = parse_page_header_from(header_buf);
    std::vector<uint8_t> page_data;
    ret = file_.read_at(stream.file_pos_, static_cast<size_t>(header.compressed_page_size_), page_data);
    if (FILE_TABLE_OK != ret) {
      return ret;
    }
    stream.file_pos_ += static_cast<int64_t>(header.compressed_page_size_);
    if (PQ_PAGE_DICTIONARY == header.type_) {
      std::vector<uint8_t> plain;
      ret = decompress_page(stream, header, page_data, plain);
      if (FILE_TABLE_OK != ret) {
        return ret;
      }
      stream.has_dictionary_ = true;
      return decode_plain_values(plain.data(), plain.size(), stream.meta_.type_,
                                  stream.meta_.type_length_, header.dict_num_values_,
                                  stream.dictionary_);
    }
    if (PQ_PAGE_DATA_V2 == header.type_) {
      return decode_page_v2(stream, header, page_data, values);
    }
    if (PQ_PAGE_DATA == header.type_) {
      return decode_page_v1(stream, header, page_data, values);
    }
    return FILE_TABLE_OK;  // index pages are skipped
  }

  int read_page_header(int64_t offset, std::vector<uint8_t> &buffer, int64_t &next_offset)
  {
    // the page header is a thrift compact struct, read a chunk that is large
    // enough for any realistic header
    size_t chunk = 4096;
    const size_t remaining = file_.size() > static_cast<size_t>(offset)
                             ? file_.size() - static_cast<size_t>(offset) : 0;
    if (chunk > remaining) {
      chunk = remaining;
    }
    if (0 == chunk) {
      return FILE_TABLE_ERR_FORMAT;
    }
    std::vector<uint8_t> raw;
    int ret = file_.read_at(offset, chunk, raw);
    if (FILE_TABLE_OK != ret) {
      return ret;
    }
    ThriftCompactReader reader(raw.data(), raw.size());
    // find the end of the struct
    size_t end = 0;
    {
      int16_t field_id = 0;
      int8_t type = 0;
      while (reader.read_field_header(field_id, type)) {
        reader.skip(type);
      }
      end = reader.pos();
    }
    buffer.assign(raw.begin(), raw.begin() + static_cast<long>(end));
    next_offset = offset + static_cast<int64_t>(end);
    return FILE_TABLE_OK;
  }

  static ParquetPageHeader parse_page_header_from(const std::vector<uint8_t> &buffer)
  {
    ThriftCompactReader reader(buffer.data(), buffer.size());
    return parse_page_header(reader);
  }

  int decompress_page(Stream &stream, const ParquetPageHeader &header,
                      const std::vector<uint8_t> &page_data, std::vector<uint8_t> &plain)
  {
    if (PQ_CODEC_UNCOMPRESSED == stream.meta_.codec_) {
      plain = page_data;
      return FILE_TABLE_OK;
    }
    if (PQ_CODEC_SNAPPY != stream.meta_.codec_) {
      set_last_error("unsupported parquet compression codec");
      return FILE_TABLE_ERR_FORMAT;
    }
    const int ret = snappy_decompress(page_data.data(), page_data.size(), plain,
                                      static_cast<size_t>(header.uncompressed_page_size_));
    if (FILE_TABLE_OK != ret) {
      set_last_error("cannot decompress parquet page");
    }
    return ret;
  }

  int decode_page_v1(Stream &stream, const ParquetPageHeader &header,
                     const std::vector<uint8_t> &page_data, std::vector<Cell> &values)
  {
    std::vector<uint8_t> plain;
    int ret = decompress_page(stream, header, page_data, plain);
    if (FILE_TABLE_OK != ret) {
      return ret;
    }
    size_t pos = 0;
    std::vector<int64_t> def_levels;
    if (stream.def_level_bit_width_ > 0) {
      size_t consumed = 0;
      ret = decode_levels_v1(plain.data() + pos, plain.size() - pos,
                              header.def_level_encoding_, stream.def_level_bit_width_,
                              header.num_values_, consumed, def_levels);
      if (FILE_TABLE_OK != ret) {
        return ret;
      }
      pos += consumed;
    }
    const int64_t present = header.num_values_ - count_zeros(def_levels, header.num_values_);
    std::vector<Cell> decoded;
    ret = decode_values(stream, header, plain.data() + pos, plain.size() - pos, present, decoded);
    if (FILE_TABLE_OK != ret) {
      return ret;
    }
    // merge the values with the definition levels
    size_t value_index = 0;
    for (int64_t i = 0; i < header.num_values_; ++i) {
      Cell cell;
      const bool is_null = (stream.def_level_bit_width_ > 0)
                           && (i < static_cast<int64_t>(def_levels.size()) && 0 == def_levels.at(i));
      if (is_null) {
        cell.is_null_ = true;
      } else if (value_index < decoded.size()) {
        cell = decoded.at(value_index++);
      }
      values.push_back(cell);
    }
    stream.values_read_ += header.num_values_;
    return FILE_TABLE_OK;
  }

  int decode_page_v2(Stream &stream, const ParquetPageHeader &header,
                     const std::vector<uint8_t> &page_data, std::vector<Cell> &values)
  {
    const size_t rep_len = static_cast<size_t>(header.rep_levels_byte_length_);
    const size_t def_len = static_cast<size_t>(header.def_levels_byte_length_);
    if (rep_len + def_len > page_data.size()) {
      return FILE_TABLE_ERR_PARSE;
    }
    std::vector<int64_t> def_levels;
    if (stream.def_level_bit_width_ > 0 && def_len > 0) {
      const int ret = decode_rle_hybrid(page_data.data() + rep_len, def_len,
                                        stream.def_level_bit_width_, header.num_values_, def_levels);
      if (FILE_TABLE_OK != ret) {
        return ret;
      }
    }
    const uint8_t *values_data = page_data.data() + rep_len + def_len;
    size_t values_size = page_data.size() - rep_len - def_len;
    std::vector<uint8_t> plain;
    if (header.is_compressed_) {
      // the compressed page size may or may not include the level bytes,
      // try both interpretations and keep the one that decodes
      int ret = decompress_values(stream, header, values_data, values_size, plain);
      if (FILE_TABLE_OK != ret) {
        // the second interpretation: the compressed size covers the values
        // only, the page data is longer than the compressed page size
        std::vector<uint8_t> extended;
        const int64_t total = static_cast<int64_t>(rep_len) + static_cast<int64_t>(def_len)
                              + static_cast<int64_t>(header.compressed_page_size_);
        if (FILE_TABLE_OK != file_.read_at(stream.file_pos_ - static_cast<int64_t>(header.compressed_page_size_),
                                           static_cast<size_t>(total), extended)) {
          return ret;
        }
        const uint8_t *extended_values = extended.data() + rep_len + def_len;
        const size_t extended_size = extended.size() - rep_len - def_len;
        if (FILE_TABLE_OK != decompress_values(stream, header, extended_values, extended_size, plain)) {
          return ret;
        }
      }
    } else {
      plain.assign(values_data, values_data + values_size);
    }
    const int64_t present = header.num_values_ - count_zeros(def_levels, header.num_values_);
    std::vector<Cell> decoded;
    int ret = decode_values(stream, header, plain.data(), plain.size(), present, decoded);
    if (FILE_TABLE_OK != ret) {
      return ret;
    }
    size_t value_index = 0;
    for (int64_t i = 0; i < header.num_values_; ++i) {
      Cell cell;
      const bool is_null = (stream.def_level_bit_width_ > 0)
                           && (i < static_cast<int64_t>(def_levels.size()) && 0 == def_levels.at(i));
      if (is_null) {
        cell.is_null_ = true;
      } else if (value_index < decoded.size()) {
        cell = decoded.at(value_index++);
      }
      values.push_back(cell);
    }
    stream.values_read_ += header.num_values_;
    return FILE_TABLE_OK;
  }

  int decompress_values(Stream &stream, const ParquetPageHeader &header, const uint8_t *data,
                        size_t size, std::vector<uint8_t> &plain)
  {
    if (PQ_CODEC_UNCOMPRESSED == stream.meta_.codec_) {
      plain.assign(data, data + size);
      return FILE_TABLE_OK;
    }
    if (PQ_CODEC_SNAPPY != stream.meta_.codec_) {
      set_last_error("unsupported parquet compression codec");
      return FILE_TABLE_ERR_FORMAT;
    }
    std::vector<uint8_t> candidate;
    int ret = snappy_decompress(data, size, candidate, 0);
    if (FILE_TABLE_OK == ret) {
      plain.swap(candidate);
      return FILE_TABLE_OK;
    }
    set_last_error("cannot decompress parquet page");
    return FILE_TABLE_ERR_PARSE;
  }

  int decode_values(Stream &stream, const ParquetPageHeader &header, const uint8_t *data,
                     size_t size, int64_t count, std::vector<Cell> &out)
  {
    if (0 == count) {
      return FILE_TABLE_OK;
    }
    if (PQ_ENC_PLAIN == header.encoding_) {
      return decode_plain_values(data, size, stream.meta_.type_, stream.meta_.type_length_,
                                 count, out);
    }
    if (PQ_ENC_PLAIN_DICTIONARY == header.encoding_ || PQ_ENC_RLE_DICTIONARY == header.encoding_) {
      if (!stream.has_dictionary_ || stream.dictionary_.empty()) {
        return FILE_TABLE_ERR_FORMAT;
      }
      if (PQ_ENC_PLAIN_DICTIONARY == header.encoding_) {
        // the dictionary indexes are plain encoded int32 values
        if (size < static_cast<size_t>(count) * 4) {
          return FILE_TABLE_ERR_PARSE;
        }
        for (int64_t i = 0; i < count; ++i) {
          int32_t index = 0;
          memcpy(&index, data + i * 4, 4);
          out.push_back(dictionary_cell(stream, index));
        }
        return FILE_TABLE_OK;
      }
      // rle dictionary: a 1 byte bit width followed by the rle encoded indexes
      if (size < 1) {
        return FILE_TABLE_ERR_PARSE;
      }
      const int32_t bit_width = data[0];
      std::vector<int64_t> indexes;
      const int ret = decode_rle_hybrid(data + 1, size - 1, bit_width, count, indexes);
      if (FILE_TABLE_OK != ret) {
        return ret;
      }
      for (size_t i = 0; i < indexes.size(); ++i) {
        out.push_back(dictionary_cell(stream, static_cast<int32_t>(indexes.at(i))));
      }
      return FILE_TABLE_OK;
    }
    set_last_error("unsupported parquet encoding");
    return FILE_TABLE_ERR_FORMAT;
  }

  Cell dictionary_cell(Stream &stream, int32_t index)
  {
    Cell cell;
    if (index >= 0 && index < static_cast<int32_t>(stream.dictionary_.size())) {
      cell = stream.dictionary_.at(index);
    } else {
      cell.is_null_ = true;
    }
    return cell;
  }

  static int64_t count_zeros(const std::vector<int64_t> &levels, int64_t count)
  {
    int64_t zeros = 0;
    const int64_t total = (count > 0 && count < static_cast<int64_t>(levels.size()))
                          ? count : static_cast<int64_t>(levels.size());
    for (int64_t i = 0; i < total; ++i) {
      if (0 == levels.at(i)) {
        ++zeros;
      }
    }
    return zeros;
  }

  int emit_rows(std::vector<std::vector<Cell> > &rows, int64_t max_rows)
  {
    // make sure every stream has at least one buffered value
    for (size_t i = 0; i < streams_.size(); ++i) {
      Stream &stream = streams_.at(i);
      while (stream.pending_.empty() && stream.values_read_ < stream.values_total_) {
        std::vector<Cell> values;
        const int ret = read_next_page(stream, values);
        if (FILE_TABLE_OK != ret) {
          return ret;
        }
        for (size_t k = 0; k < values.size(); ++k) {
          stream.pending_.push_back(values.at(k));
        }
        if (values.empty() && stream.values_read_ >= stream.values_total_) {
          break;
        }
      }
    }
    if (streams_.empty()) {
      return FILE_TABLE_OK;
    }
    int64_t available = -1;
    for (size_t i = 0; i < streams_.size(); ++i) {
      const int64_t size = static_cast<int64_t>(streams_.at(i).pending_.size());
      if (available < 0 || size < available) {
        available = size;
      }
    }
    if (available <= 0) {
      return FILE_TABLE_OK;
    }
    if (available > max_rows) {
      available = max_rows;
    }
    for (int64_t r = 0; r < available; ++r) {
      std::vector<Cell> row;
      row.reserve(streams_.size());
      for (size_t i = 0; i < streams_.size(); ++i) {
        Stream &stream = streams_.at(i);
        row.push_back(stream.pending_.front());
        stream.pending_.pop_front();
      }
      rows.push_back(row);
      ++rows_emitted_;
    }
    return FILE_TABLE_OK;
  }

private:
  ParquetFile file_;
  std::vector<ParquetColumn> parquet_columns_;
  std::vector<Stream> streams_;
  size_t row_group_index_ = 0;
  int64_t rows_emitted_ = 0;
};

int create_parquet_reader_impl(const std::string &path, const std::vector<std::string> &selected,
                                FileScanReader *&reader)
{
  reader = nullptr;
  ParquetReader *parquet_reader = new ParquetReader();
  const int ret = parquet_reader->open(path, selected);
  if (FILE_TABLE_OK != ret) {
    delete parquet_reader;
    return ret;
  }
  reader = parquet_reader;
  return FILE_TABLE_OK;
}

}  // namespace

int create_parquet_reader(const std::string &path, FileScanReader *&reader)
{
  std::vector<std::string> selected;
  return create_parquet_reader_impl(path, selected, reader);
}

int create_parquet_reader_with_columns(const std::string &path,
                                        const std::vector<std::string> &columns,
                                        FileScanReader *&reader)
{
  return create_parquet_reader_impl(path, columns, reader);
}

int infer_parquet_schema(const std::string &path, std::vector<ColumnDesc> &columns)
{
  columns.clear();
  FileScanReader *reader = nullptr;
  const int ret = create_parquet_reader(path, reader);
  if (FILE_TABLE_OK != ret) {
    return ret;
  }
  columns = reader->get_columns();
  reader->close();
  delete reader;
  return FILE_TABLE_OK;
}

int scan_parquet(const std::string &path,
                 std::vector<ColumnDesc> &columns,
                 std::vector<std::vector<Cell> > &rows)
{
  columns.clear();
  rows.clear();
  FileScanReader *reader = nullptr;
  int ret = create_parquet_reader(path, reader);
  if (FILE_TABLE_OK != ret) {
    return ret;
  }
  columns = reader->get_columns();
  std::vector<std::vector<Cell> > batch;
  while (FILE_TABLE_OK == ret) {
    ret = reader->get_next_batch(batch, 1024);
    if (FILE_TABLE_OK != ret) {
      break;
    }
    for (size_t i = 0; i < batch.size(); ++i) {
      rows.push_back(batch.at(i));
    }
    if (batch.empty()) {
      break;
    }
  }
  reader->close();
  delete reader;
  return ret;
}

}  // namespace file_table
}  // namespace sql
}  // namespace oceanbase
