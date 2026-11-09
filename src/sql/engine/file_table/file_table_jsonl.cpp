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
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_table
{

// ---------------------------------------------------------------------------
// Minimal JSON scanner
//
// Only the subset needed by the JSONL engine is implemented: a flat object
// per line.  The scanner understands the complete JSON grammar but a nested
// object or array is rejected with FILE_TABLE_ERR_NESTED.
// ---------------------------------------------------------------------------

namespace
{

// Cursor over the text of a single JSON line.
class JsonCursor
{
public:
  explicit JsonCursor(const std::string &text) : text_(text) {}

  bool eof() const { return pos_ >= text_.size(); }
  char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  char get() { return text_[pos_++]; }

  // Skip the white space between two JSON tokens.
  void skip_ws()
  {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
        continue;
      }
      break;
    }
  }

  bool starts_with(const char *prefix) const
  {
    return text_.compare(pos_, std::strlen(prefix), prefix) == 0;
  }

  void skip(size_t count) { pos_ += count; }

private:
  const std::string &text_;
  size_t pos_ = 0;
};

inline bool is_hex_digit(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline uint32_t hex_value(char c)
{
  if (c >= '0' && c <= '9') {
    return (uint32_t)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (uint32_t)(c - 'a' + 10);
  }
  return (uint32_t)(c - 'A' + 10);
}

// Append the UTF-8 encoding of a code point.
void append_utf8(uint32_t code_point, std::string &out)
{
  if (code_point < 0x80) {
    out.push_back((char)code_point);
  } else if (code_point < 0x800) {
    out.push_back((char)(0xC0 | (code_point >> 6)));
    out.push_back((char)(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out.push_back((char)(0xE0 | (code_point >> 12)));
    out.push_back((char)(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back((char)(0xF0 | (code_point >> 18)));
    out.push_back((char)(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back((char)(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (code_point & 0x3F)));
  }
}

// Read the four hexadecimal digits of a \uXXXX escape.
bool read_hex4(JsonCursor &cursor, uint32_t &value)
{
  if (cursor.eof()) {
    return false;
  }
  // The cursor does not expose the remaining size, the string is scanned
  // through the peek / get interface.
  uint32_t result = 0;
  for (int i = 0; i < 4; ++i) {
    if (cursor.eof() || !is_hex_digit(cursor.peek())) {
      return false;
    }
    result = (result << 4) | hex_value(cursor.get());
  }
  value = result;
  return true;
}

// Parse a JSON string.  The surrounding quotes are consumed, the escape
// sequences are decoded and the UTF-8 bytes are kept verbatim.
int parse_json_string(JsonCursor &cursor, std::string &out)
{
  out.clear();
  if (cursor.eof() || cursor.peek() != '"') {
    return FILE_TABLE_ERR_PARSE;
  }
  cursor.get();
  for (;;) {
    if (cursor.eof()) {
      return FILE_TABLE_ERR_PARSE;  // unterminated string
    }
    const unsigned char c = (unsigned char)cursor.peek();
    if (c == '"') {
      cursor.get();
      return FILE_TABLE_OK;
    }
    if (c < 0x20) {
      return FILE_TABLE_ERR_PARSE;  // control character in a string
    }
    if (c != '\\') {
      out.push_back(cursor.get());
      continue;
    }
    cursor.get();  // consume the backslash
    if (cursor.eof()) {
      return FILE_TABLE_ERR_PARSE;
    }
    const char escape = cursor.get();
    switch (escape) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'u': {
        uint32_t code_point = 0;
        if (!read_hex4(cursor, code_point)) {
          return FILE_TABLE_ERR_PARSE;
        }
        if (code_point >= 0xD800 && code_point <= 0xDBFF) {
          // A high surrogate, it must be followed by a low surrogate.
          if (cursor.peek() == '\\') {
            cursor.get();
            if (cursor.peek() == 'u') {
              cursor.get();
              uint32_t low = 0;
              if (!read_hex4(cursor, low)) {
                return FILE_TABLE_ERR_PARSE;
              }
              if (low >= 0xDC00 && low <= 0xDFFF) {
                code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
              } else {
                append_utf8(code_point, out);
                code_point = low;
              }
            } else {
              return FILE_TABLE_ERR_PARSE;
            }
          }
        }
        append_utf8(code_point, out);
        break;
      }
      default:
        return FILE_TABLE_ERR_PARSE;  // unknown escape sequence
    }
  }
}

// Parse a JSON number, the raw literal is returned unchanged.
int parse_json_number(JsonCursor &cursor, std::string &out)
{
  out.clear();
  if (cursor.peek() == '-') {
    out.push_back(cursor.get());
  }
  if (cursor.eof()) {
    return FILE_TABLE_ERR_PARSE;
  }
  if (cursor.peek() == '0') {
    out.push_back(cursor.get());
  } else if (cursor.peek() >= '1' && cursor.peek() <= '9') {
    while (!cursor.eof() && cursor.peek() >= '0' && cursor.peek() <= '9') {
      out.push_back(cursor.get());
    }
  } else {
    return FILE_TABLE_ERR_PARSE;
  }
  if (!cursor.eof() && cursor.peek() == '.') {
    out.push_back(cursor.get());
    if (cursor.eof() || cursor.peek() < '0' || cursor.peek() > '9') {
      return FILE_TABLE_ERR_PARSE;
    }
    while (!cursor.eof() && cursor.peek() >= '0' && cursor.peek() <= '9') {
      out.push_back(cursor.get());
    }
  }
  if (!cursor.eof() && (cursor.peek() == 'e' || cursor.peek() == 'E')) {
    out.push_back(cursor.get());
    if (!cursor.eof() && (cursor.peek() == '+' || cursor.peek() == '-')) {
      out.push_back(cursor.get());
    }
    if (cursor.eof() || cursor.peek() < '0' || cursor.peek() > '9') {
      return FILE_TABLE_ERR_PARSE;
    }
    while (!cursor.eof() && cursor.peek() >= '0' && cursor.peek() <= '9') {
      out.push_back(cursor.get());
    }
  }
  return FILE_TABLE_OK;
}

}  // namespace

// ---------------------------------------------------------------------------
// JSON line parsing
// ---------------------------------------------------------------------------

int JsonLineParser::parse_line(const std::string &line, int64_t line_no, std::vector<JsonMember> &members)
{
  members.clear();
  const std::string prefix = "line " + std::to_string(line_no) + ": ";
  JsonCursor cursor(line);
  cursor.skip_ws();
  if (cursor.eof() || cursor.peek() != '{') {
    set_last_error(prefix + "expected a JSON object");
    return FILE_TABLE_ERR_PARSE;
  }
  cursor.get();
  cursor.skip_ws();
  if (!cursor.eof() && cursor.peek() == '}') {
    cursor.get();
  } else {
    for (;;) {
      cursor.skip_ws();
      if (cursor.eof() || cursor.peek() != '"') {
        set_last_error(prefix + "expected a member name");
        return FILE_TABLE_ERR_PARSE;
      }
      JsonMember member;
      int rc = parse_json_string(cursor, member.key_);
      if (rc != FILE_TABLE_OK) {
        set_last_error(prefix + "invalid member name");
        return FILE_TABLE_ERR_PARSE;
      }
      cursor.skip_ws();
      if (cursor.eof() || cursor.peek() != ':') {
        set_last_error(prefix + "expected ':' after the member name '" + member.key_ + "'");
        return FILE_TABLE_ERR_PARSE;
      }
      cursor.get();
      cursor.skip_ws();
      if (cursor.eof()) {
        set_last_error(prefix + "unexpected end of line");
        return FILE_TABLE_ERR_PARSE;
      }
      const char value_char = cursor.peek();
      if (value_char == '{' || value_char == '[') {
        set_last_error(prefix + "a nested object or array is not supported");
        return FILE_TABLE_ERR_NESTED;
      }
      if (value_char == '"') {
        member.is_string_ = true;
        rc = parse_json_string(cursor, member.text_);
        if (rc != FILE_TABLE_OK) {
          set_last_error(prefix + "invalid string value of member '" + member.key_ + "'");
          return FILE_TABLE_ERR_PARSE;
        }
      } else if (value_char == '-' || (value_char >= '0' && value_char <= '9')) {
        rc = parse_json_number(cursor, member.text_);
        if (rc != FILE_TABLE_OK) {
          set_last_error(prefix + "invalid number value of member '" + member.key_ + "'");
          return FILE_TABLE_ERR_PARSE;
        }
        int64_t int_value = 0;
        member.is_integer_ = text_is_int64(member.text_, int_value);
      } else if (cursor.starts_with("true")) {
        cursor.skip(4);
        member.is_boolean_ = true;
        member.text_ = "true";
      } else if (cursor.starts_with("false")) {
        cursor.skip(5);
        member.is_boolean_ = true;
        member.text_ = "false";
      } else if (cursor.starts_with("null")) {
        cursor.skip(4);
        member.is_null_ = true;
      } else {
        set_last_error(prefix + "unexpected character '" + std::string(1, value_char)
                       + "' in member '" + member.key_ + "'");
        return FILE_TABLE_ERR_PARSE;
      }
      members.push_back(member);
      cursor.skip_ws();
      if (cursor.eof()) {
        set_last_error(prefix + "unexpected end of line");
        return FILE_TABLE_ERR_PARSE;
      }
      const char separator = cursor.peek();
      if (separator == ',') {
        cursor.get();
        continue;
      }
      if (separator == '}') {
        cursor.get();
        break;
      }
      set_last_error(prefix + "expected ',' or '}' after the member '" + members.back().key_ + "'");
      return FILE_TABLE_ERR_PARSE;
    }
  }
  cursor.skip_ws();
  if (!cursor.eof()) {
    set_last_error(prefix + "unexpected content after the JSON object");
    return FILE_TABLE_ERR_PARSE;
  }
  return FILE_TABLE_OK;
}

// ---------------------------------------------------------------------------
// JSONL line input
// ---------------------------------------------------------------------------

namespace
{

// Read the lines of a JSONL file one by one, the file is never loaded as a
// whole.
class JsonlLineReader
{
public:
  int open(const std::string &path)
  {
    input_.close();
    int rc = input_.open(path);
    if (rc != FILE_TABLE_OK) {
      return rc;
    }
    line_no_ = 0;
    return FILE_TABLE_OK;
  }

  void close()
  {
    input_.close();
    line_no_ = 0;
  }

  int64_t line_no() const { return line_no_; }

  // Read the next non empty line.  eof is set when the file is exhausted.
  int next_line(std::string &line, bool &eof)
  {
    line.clear();
    eof = false;
    for (;;) {
      std::string raw;
      bool raw_eof = false;
      int rc = next_raw_line(raw, raw_eof);
      if (rc != FILE_TABLE_OK) {
        return rc;
      }
      if (raw_eof) {
        eof = true;
        return FILE_TABLE_OK;
      }
      ++line_no_;
      if (line_no_ == 1 && raw.size() >= 3 && (unsigned char)raw[0] == 0xEF
          && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
        raw.erase(0, 3);  // skip an UTF-8 byte order mark
      }
      if (is_blank(raw)) {
        continue;  // skip empty lines
      }
      line.swap(raw);
      return FILE_TABLE_OK;
    }
  }

private:
  static bool is_blank(const std::string &text)
  {
    for (size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      if (c != ' ' && c != '\t' && c != '\r') {
        return false;
      }
    }
    return true;
  }

  // Read one physical line, eof is set when nothing was read at all.
  int next_raw_line(std::string &raw, bool &eof)
  {
    raw.clear();
    eof = false;
    int ch = -1;
    for (;;) {
      int rc = input_.read_char(ch);
      if (rc != FILE_TABLE_OK) {
        return rc;
      }
      if (ch < 0) {
        eof = raw.empty();
        break;
      }
      if (ch == '\n') {
        break;
      }
      raw.push_back((char)ch);
    }
    if (!raw.empty() && raw[raw.size() - 1] == '\r') {
      raw.erase(raw.size() - 1);
    }
    return FILE_TABLE_OK;
  }

  TextFileInput input_;
  int64_t line_no_ = 0;
};

// ---------------------------------------------------------------------------
// Schema inference
// ---------------------------------------------------------------------------

// Collect the inference state of every column of a JSONL file.
class JsonlSchemaBuilder
{
public:
  // Merge the values of one line into the inference state.
  void add_row(const std::vector<JsonMember> &members)
  {
    // The last occurrence of a duplicated key wins.
    std::map<std::string, const JsonMember *> values;
    for (size_t i = 0; i < members.size(); ++i) {
      values[members[i].key_] = &members[i];
    }
    // The columns are ordered by the first appearance of their key.
    for (size_t i = 0; i < members.size(); ++i) {
      ensure_column(members[i].key_);
    }
    for (std::map<std::string, const JsonMember *>::const_iterator it = values.begin();
         it != values.end(); ++it) {
      merge_value(it->first, *it->second);
    }
    // A column that is missing in this row carries a NULL value.
    for (size_t i = 0; i < key_order_.size(); ++i) {
      if (values.find(key_order_[i]) == values.end()) {
        states_[i].has_null_ = true;
      }
    }
  }

  void build(std::vector<ColumnDesc> &columns, int64_t total_rows) const
  {
    columns.resize(key_order_.size());
    for (size_t i = 0; i < key_order_.size(); ++i) {
      columns[i].name_ = key_order_[i];
      finalize_column(states_[i], total_rows, columns[i]);
    }
  }

private:
  // Register a column, the columns keep the order of their first appearance.
  void ensure_column(const std::string &key)
  {
    if (key_index_.find(key) != key_index_.end()) {
      return;
    }
    key_index_[key] = states_.size();
    key_order_.push_back(key);
    states_.push_back(ColumnInferenceState());
  }

  void merge_value(const std::string &key, const JsonMember &member)
  {
    ensure_column(key);
    ColumnInferenceState &state = states_[key_index_[key]];
    if (member.is_null_) {
      state.has_null_ = true;
      return;
    }
    state.has_value_ = true;
    ++state.present_count_;
    ColumnKind kind = ColumnKind::VARCHAR;
    if (member.is_boolean_) {
      kind = ColumnKind::BOOLEAN;
    } else if (!member.is_string_) {
      // A number, an integer literal is a BIGINT candidate and a number with
      // a fraction or an exponent is a DOUBLE candidate.
      int64_t int_value = 0;
      kind = (member.is_integer_ && text_is_int64(member.text_, int_value)) ? ColumnKind::BIGINT
                                                                           : ColumnKind::DOUBLE;
      state.fraction_digits_ = std::max(state.fraction_digits_, text_fraction_digits(member.text_));
    } else {
      // A string is a text candidate, it is only a DATE candidate when every
      // non NULL string of the column is a date.
      if (text_is_date(member.text_)) {
        kind = ColumnKind::DATE;
      } else {
        kind = ColumnKind::VARCHAR;
      }
      state.max_length_ = std::max(state.max_length_, (int32_t)member.text_.size());
    }
    state.kind_ = merge_kinds(state.kind_, kind);
  }

  // The column names in the order of their first appearance.
  std::vector<std::string> key_order_;
  // The index of a column in the state vector.
  std::map<std::string, size_t> key_index_;
  // The inference state of every column.
  std::vector<ColumnInferenceState> states_;
};

}  // namespace

// ---------------------------------------------------------------------------
// JSONL reader
// ---------------------------------------------------------------------------

namespace
{

class JsonlReader : public FileScanReader
{
public:
  JsonlReader(const std::string &path, const std::vector<ColumnDesc> &columns) : path_(path)
  {
    columns_ = columns;
  }

  ~JsonlReader() override { close(); }

  int get_next_batch(std::vector<std::vector<Cell>> &rows, int64_t max_rows) override
  {
    rows.clear();
    if (max_rows <= 0) {
      return FILE_TABLE_OK;
    }
    std::string line;
    bool eof = false;
    std::vector<JsonMember> members;
    while ((int64_t)rows.size() < max_rows) {
      int rc = reader_.next_line(line, eof);
      if (rc != FILE_TABLE_OK) {
        return rc;
      }
      if (eof) {
        break;
      }
      rc = parser_.parse_line(line, reader_.line_no(), members);
      if (rc != FILE_TABLE_OK) {
        return rc;
      }
      rows.push_back(build_row(members));
    }
    return FILE_TABLE_OK;
  }

  int rescan() override { return reopen(); }

  int close() override
  {
    reader_.close();
    return FILE_TABLE_OK;
  }

  int open() { return reopen(); }

private:
  int reopen()
  {
    reader_.close();
    int rc = reader_.open(path_);
    if (rc != FILE_TABLE_OK) {
      return rc;
    }
    return FILE_TABLE_OK;
  }

  // Build the cells of one row, a missing member is a NULL value.
  std::vector<Cell> build_row(const std::vector<JsonMember> &members)
  {
    std::map<std::string, const JsonMember *> values;
    for (size_t i = 0; i < members.size(); ++i) {
      values[members[i].key_] = &members[i];
    }
    std::vector<Cell> row(columns_.size());
    for (size_t i = 0; i < columns_.size(); ++i) {
      std::map<std::string, const JsonMember *>::const_iterator it =
          values.find(columns_[i].name_);
      if (it == values.end() || it->second->is_null_) {
        row[i].is_null_ = true;
        continue;
      }
      row[i].is_null_ = false;
      row[i].text_ = it->second->text_;
    }
    return row;
  }

  JsonlLineReader reader_;
  JsonLineParser parser_;
  std::string path_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

int infer_jsonl_schema(const std::string &path, std::vector<ColumnDesc> &columns)
{
  columns.clear();
  JsonlLineReader reader;
  int rc = reader.open(path);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  JsonLineParser parser;
  JsonlSchemaBuilder builder;
  std::string line;
  bool eof = false;
  int64_t row_count = 0;
  std::vector<JsonMember> members;
  for (;;) {
    rc = reader.next_line(line, eof);
    if (rc != FILE_TABLE_OK) {
      return rc;
    }
    if (eof) {
      break;
    }
    rc = parser.parse_line(line, reader.line_no(), members);
    if (rc != FILE_TABLE_OK) {
      return rc;
    }
    ++row_count;
    builder.add_row(members);
  }
  builder.build(columns, row_count);
  return FILE_TABLE_OK;
}

int create_jsonl_reader(const std::string &path, FileScanReader *&reader)
{
  reader = nullptr;
  std::vector<ColumnDesc> columns;
  int rc = infer_jsonl_schema(path, columns);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  JsonlReader *jsonl_reader = new JsonlReader(path, columns);
  int open_rc = jsonl_reader->open();
  if (open_rc != FILE_TABLE_OK) {
    delete jsonl_reader;
    return open_rc;
  }
  reader = jsonl_reader;
  return FILE_TABLE_OK;
}

int scan_jsonl(const std::string &path,
               std::vector<ColumnDesc> &columns,
               std::vector<std::vector<Cell>> &rows)
{
  rows.clear();
  FileScanReader *reader = nullptr;
  int rc = create_jsonl_reader(path, reader);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  std::vector<std::vector<Cell>> batch;
  for (;;) {
    rc = reader->get_next_batch(batch, 1024);
    if (rc != FILE_TABLE_OK) {
      reader->close();
      delete reader;
      return rc;
    }
    if (batch.empty()) {
      break;
    }
    rows.insert(rows.end(), batch.begin(), batch.end());
  }
  columns = reader->get_columns();
  reader->close();
  delete reader;
  return FILE_TABLE_OK;
}

}  // namespace file_table
}  // namespace sql
}  // namespace oceanbase
