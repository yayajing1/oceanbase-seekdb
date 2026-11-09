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

#ifndef OCEANBASE_SQL_ENGINE_FILE_TABLE_FILE_TABLE_INTERNAL_H_
#define OCEANBASE_SQL_ENGINE_FILE_TABLE_FILE_TABLE_INTERNAL_H_

// Internal helpers shared by the format engines.  Not part of the public API.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sql/engine/file_table/file_table_format.h"

namespace oceanbase
{
namespace sql
{
namespace file_table
{

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

// Store the message of the last failure of the current thread.
void set_last_error(const std::string &message);

// ---------------------------------------------------------------------------
// Deterministic value classification
// ---------------------------------------------------------------------------

// Strict decimal integer parse (no leading blanks, no fraction, no exponent).
// Returns false when the text is not an integer or does not fit into int64.
bool text_is_int64(const std::string &text, int64_t &value);

// Strict floating point parse (decimal / exponent notation only).
bool text_is_double(const std::string &text, double &value);

// Boolean literals: true, false, TRUE, FALSE, t, f, T, F.
bool text_is_boolean(const std::string &text, bool &value);

// Valid calendar date in YYYY-MM-DD notation.
bool text_is_date(const std::string &text);

// Valid timestamp: YYYY-MM-DD ('T' | ' ') HH:MM[:SS[.fraction]].
bool text_is_datetime(const std::string &text);

// Classify a raw text value (CSV semantics).
ColumnKind classify_text(const std::string &text);

// Widening of two column kinds.  The lattice is
//   BOOLEAN < BIGINT < DOUBLE < DATE / DATETIME < VARCHAR
// and a value of a different "family" (text versus number) widens to VARCHAR.
ColumnKind merge_kinds(ColumnKind lhs, ColumnKind rhs);

// Number of fractional digits of a decimal text (used for DOUBLE precision).
int32_t text_fraction_digits(const std::string &text);

// ---------------------------------------------------------------------------
// Type inference
// ---------------------------------------------------------------------------

// State collected while scanning the values of one column.
struct ColumnInferenceState {
  // At least one non NULL value was observed.
  bool has_value_ = false;
  // At least one NULL value was observed.
  bool has_null_ = false;
  // Widened type of all observed values.
  ColumnKind kind_ = ColumnKind::NULL_ONLY;
  // Longest value in bytes (VARCHAR columns).
  int32_t max_length_ = 0;
  // Largest number of fractional digits (DOUBLE columns).
  int32_t fraction_digits_ = 0;
  // Number of rows in which the column carried a value.
  int64_t present_count_ = 0;
};

// Build the final column description.  total_rows is the number of scanned
// rows, it is used to detect columns that are missing in some rows.
void finalize_column(const ColumnInferenceState &state, int64_t total_rows, ColumnDesc &desc);

// ---------------------------------------------------------------------------
// Buffered text input
// ---------------------------------------------------------------------------

// Small buffered forward reader used by the CSV / JSONL engines.  It never
// loads the whole file, only one buffer at a time.
class TextFileInput
{
public:
  TextFileInput();
  ~TextFileInput();

  int open(const std::string &path);
  void close();
  bool is_open() const { return fp_ != nullptr; }
  const std::string &path() const { return path_; }

  // Read one byte, ch is -1 at end of file.
  int read_char(int &ch);
  // Look at the next byte without consuming it.
  int peek_char(int &ch);

private:
  int fill();

  std::FILE *fp_ = nullptr;
  std::vector<char> buf_;
  size_t pos_ = 0;
  size_t len_ = 0;
  int pushback_ = -1;
  bool eof_ = false;
  std::string path_;
};

// ---------------------------------------------------------------------------
// CSV parsing
// ---------------------------------------------------------------------------

// One parsed CSV field.  An unquoted empty field is NULL, a quoted empty
// field ("") is an empty string.
struct CsvField {
  std::string value_;
  bool is_null_ = false;
};

// RFC 4180 like CSV parser: '"' quoting, doubled quotes as escape, embedded
// newlines, LF and CRLF line endings.
class CsvParser
{
public:
  explicit CsvParser(char separator = ',');
  ~CsvParser();

  int open(const std::string &path);
  void close();
  // Read the next row.  eof is set when the file is exhausted.
  int next_row(std::vector<CsvField> &row, bool &eof);

private:
  int peek_char(int &ch);
  int read_char(int &ch);

  TextFileInput input_;
  char separator_;
  // Characters read while probing an UTF-8 byte order mark.
  std::string prefix_;
  size_t prefix_pos_ = 0;
};

// ---------------------------------------------------------------------------
// JSON line parsing
// ---------------------------------------------------------------------------

// One member of a flat JSON object.
struct JsonMember {
  std::string key_;
  // "true" / "false" for booleans, the raw literal for numbers, the decoded
  // content for strings.
  std::string text_;
  // True when the value was a JSON string.
  bool is_string_ = false;
  // True when the value was an integer literal that fits into int64.
  bool is_integer_ = false;
  // True when the value was a JSON boolean.
  bool is_boolean_ = false;
  // True when the value was the JSON literal null.
  bool is_null_ = false;
};

// Parser for one flat JSON object per line.  Nested objects and arrays are
// rejected with FILE_TABLE_ERR_NESTED.
class JsonLineParser
{
public:
  // line_no is only used to build the error message.
  int parse_line(const std::string &line, int64_t line_no, std::vector<JsonMember> &members);
};

// ---------------------------------------------------------------------------
// Thrift compact protocol (used by the Parquet footer and page headers)
// ---------------------------------------------------------------------------

// Thrift compact protocol types.
enum ThriftType {
  THRIFT_STOP = 0,
  THRIFT_BOOLEAN_TRUE = 1,
  THRIFT_BOOLEAN_FALSE = 2,
  THRIFT_BYTE = 3,
  THRIFT_I16 = 4,
  THRIFT_I32 = 5,
  THRIFT_I64 = 6,
  THRIFT_DOUBLE = 7,
  THRIFT_BINARY = 8,
  THRIFT_LIST = 9,
  THRIFT_SET = 10,
  THRIFT_MAP = 11,
  THRIFT_STRUCT = 12
};

// Minimal reader for the Thrift compact protocol.
class ThriftCompactReader
{
public:
  ThriftCompactReader(const uint8_t *data, size_t size);

  // Read the next field header of the current struct.
  bool read_field_header(int16_t &field_id, int8_t &type);
  // List header of the current list.
  bool read_list_header(int8_t &elem_type, int32_t &size);
  bool read_varint(uint64_t &value);
  int64_t read_zigzag();
  std::string read_binary();
  double read_double();
  // Skip a value of the given type.
  void skip(int8_t type);

  size_t pos() const { return pos_; }
  void set_pos(size_t pos) { pos_ = pos; }
  bool at_end() const { return pos_ >= size_; }
  // the field id delta is relative to the previous field of the current
  // struct, nested structs therefore start a new delta context
  int16_t field_id_context() const { return last_field_id_; }
  void set_field_id_context(int16_t context) { last_field_id_ = context; }

private:
  bool read_byte(uint8_t &value);

  const uint8_t *data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
  int16_t last_field_id_ = 0;
};

// ---------------------------------------------------------------------------
// Snappy
// ---------------------------------------------------------------------------

// Decompress a raw (framing free) Snappy block.
int snappy_decompress(const uint8_t *src, size_t src_size, std::vector<uint8_t> &dst, size_t dst_size);

}  // namespace file_table
}  // namespace sql
}  // namespace oceanbase

#endif  // OCEANBASE_SQL_ENGINE_FILE_TABLE_FILE_TABLE_INTERNAL_H_
