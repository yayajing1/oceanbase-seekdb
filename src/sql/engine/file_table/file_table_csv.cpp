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
#include <cstring>
#include <map>

namespace oceanbase
{
namespace sql
{
namespace file_table
{

// ---------------------------------------------------------------------------
// CSV parser
// ---------------------------------------------------------------------------

CsvParser::CsvParser(char separator) : separator_(separator) {}

CsvParser::~CsvParser() { close(); }

int CsvParser::open(const std::string &path)
{
  close();
  int rc = input_.open(path);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  prefix_.clear();
  prefix_pos_ = 0;
  // Skip an UTF-8 byte order mark.
  int c0 = -1;
  int c1 = -1;
  int c2 = -1;
  input_.read_char(c0);
  bool is_bom = (c0 == 0xEF);
  if (is_bom) {
    input_.read_char(c1);
    if (c1 != 0xBB) {
      is_bom = false;
    } else {
      input_.read_char(c2);
      if (c2 != 0xBF) {
        is_bom = false;
      }
    }
  }
  if (!is_bom) {
    if (c0 >= 0) {
      prefix_.push_back((char)c0);
    }
    if (c1 >= 0) {
      prefix_.push_back((char)c1);
    }
    if (c2 >= 0) {
      prefix_.push_back((char)c2);
    }
  }
  return FILE_TABLE_OK;
}

void CsvParser::close() { input_.close(); }

int CsvParser::peek_char(int &ch)
{
  if (prefix_pos_ < prefix_.size()) {
    ch = (int)(unsigned char)prefix_[prefix_pos_];
    return FILE_TABLE_OK;
  }
  return input_.peek_char(ch);
}

int CsvParser::read_char(int &ch)
{
  if (prefix_pos_ < prefix_.size()) {
    ch = (int)(unsigned char)prefix_[prefix_pos_++];
    return FILE_TABLE_OK;
  }
  return input_.read_char(ch);
}

int CsvParser::next_row(std::vector<CsvField> &row, bool &eof)
{
  row.clear();
  eof = false;
  int ch = -1;
  int rc = peek_char(ch);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  if (ch < 0) {
    eof = true;
    return FILE_TABLE_OK;
  }
  for (;;) {
    CsvField field;
    bool quoted = false;
    if (ch == '"') {
      quoted = true;
      read_char(ch);  // consume the opening quote
      std::string value;
      bool closed = false;
      for (;;) {
        int c = -1;
        rc = read_char(c);
        if (rc != FILE_TABLE_OK) {
          return rc;
        }
        if (c < 0) {
          break;  // end of file inside a quoted field
        }
        if (c == '"') {
          int next = -1;
          rc = peek_char(next);
          if (rc != FILE_TABLE_OK) {
            return rc;
          }
          if (next == '"') {
            read_char(next);
            value.push_back('"');
            continue;
          }
          closed = true;
          break;
        }
        value.push_back((char)c);
      }
      if (!closed) {
        set_last_error("unterminated quoted field in CSV file");
        return FILE_TABLE_ERR_PARSE;
      }
      field.value_.swap(value);
      field.is_null_ = false;
    } else {
      std::string value;
      for (;;) {
        int c = -1;
        rc = peek_char(c);
        if (rc != FILE_TABLE_OK) {
          return rc;
        }
        if (c < 0 || c == separator_ || c == '\n' || c == '\r') {
          break;
        }
        value.push_back((char)c);
        read_char(c);  // consume the character
      }
      field.value_.swap(value);
      // An unquoted empty field is NULL.
      field.is_null_ = field.value_.empty();
    }
    row.push_back(field);

    int next = -1;
    rc = peek_char(next);
    if (rc != FILE_TABLE_OK) {
      return rc;
    }
    if (next == separator_) {
      read_char(next);  // consume the separator
      rc = peek_char(ch);
      if (rc != FILE_TABLE_OK) {
        return rc;
      }
      if (ch < 0) {
        // A trailing separator produces one more (NULL) field.
        CsvField tail;
        tail.is_null_ = true;
        row.push_back(tail);
        // the row is complete, the end of the file is reported by the next
        // call so that the last row of the file is not lost
        return FILE_TABLE_OK;
      }
      continue;
    }
    if (next == '\n' || next == '\r') {
      read_char(next);
      if (next == '\r') {
        int after = -1;
        rc = peek_char(after);
        if (rc != FILE_TABLE_OK) {
          return rc;
        }
        if (after == '\n') {
          read_char(after);
        }
      }
    } else if (next >= 0 && quoted) {
      set_last_error("unexpected character after a quoted field in CSV file");
      return FILE_TABLE_ERR_PARSE;
    }
    // The row is complete.  The end of the file is reported by the next call
    // of next_row(), otherwise the last row of the file would be lost.
    return FILE_TABLE_OK;
  }
}

// ---------------------------------------------------------------------------
// Schema helpers
// ---------------------------------------------------------------------------

namespace
{
// Build the column names of a header row.  The k-th occurrence (k >= 2) of a
// duplicated name is renamed to "<name>__<k>".
void build_header_names(const std::vector<CsvField> &header, std::vector<std::string> &names)
{
  names.clear();
  std::map<std::string, int32_t> occurrences;
  for (size_t i = 0; i < header.size(); ++i) {
    std::string name = header[i].is_null_ ? std::string() : header[i].value_;
    if (name.empty()) {
      name = "column" + std::to_string(i);
    }
    int32_t &count = occurrences[name];
    ++count;
    if (count > 1) {
      name = name + "__" + std::to_string(count);
    }
    names.push_back(name);
  }
}

void build_default_names(size_t count, std::vector<std::string> &names)
{
  names.clear();
  for (size_t i = 0; i < count; ++i) {
    names.push_back("column" + std::to_string(i));
  }
}

// State collected while scanning the values of one column.
using ColumnState = ColumnInferenceState;
}  // namespace

// ---------------------------------------------------------------------------
// CSV reader
// ---------------------------------------------------------------------------

namespace
{
class CsvReader : public FileScanReader
{
public:
  CsvReader(const std::string &path, bool has_header, const std::vector<ColumnDesc> &columns)
      : path_(path), has_header_(has_header)
  {
    columns_ = columns;
  }

  ~CsvReader() override { close(); }

  int get_next_batch(std::vector<std::vector<Cell>> &rows, int64_t max_rows) override
  {
    rows.clear();
    if (max_rows <= 0) {
      return FILE_TABLE_OK;
    }
    std::vector<CsvField> row;
    bool eof = false;
    while ((int64_t)rows.size() < max_rows) {
      int rc = parser_.next_row(row, eof);
      if (rc != FILE_TABLE_OK) {
        return rc;
      }
      if (eof) {
        break;
      }
      ++row_no_;
      std::vector<Cell> cells(columns_.size());
      for (size_t i = 0; i < columns_.size(); ++i) {
        if (i < row.size()) {
          cells[i].is_null_ = row[i].is_null_;
          cells[i].text_ = row[i].is_null_ ? std::string() : row[i].value_;
        } else {
          // A short row is padded with NULL values.
          cells[i].is_null_ = true;
        }
      }
      rows.push_back(std::move(cells));
    }
    return FILE_TABLE_OK;
  }

  int rescan() override { return reopen(); }

  int close() override
  {
    parser_.close();
    row_no_ = 0;
    return FILE_TABLE_OK;
  }

  int open() { return reopen(); }

private:
  int reopen()
  {
    parser_.close();
    int rc = parser_.open(path_);
    if (rc != FILE_TABLE_OK) {
      return rc;
    }
    row_no_ = 0;
    if (has_header_) {
      std::vector<CsvField> header;
      bool eof = false;
      rc = parser_.next_row(header, eof);
      if (rc != FILE_TABLE_OK) {
        return rc;
      }
    }
    return FILE_TABLE_OK;
  }

  CsvParser parser_;
  std::string path_;
  bool has_header_ = false;
  int64_t row_no_ = 0;
};
}  // namespace

// ---------------------------------------------------------------------------
// Header detection and type inference
// ---------------------------------------------------------------------------

namespace
{
// A header row is detected when the file has at least one data row and all
// fields of the first row are non empty and cannot be parsed as a number.
bool looks_like_header(const std::vector<CsvField> &row)
{
  if (row.empty()) {
    return false;
  }
  for (size_t i = 0; i < row.size(); ++i) {
    if (row[i].is_null_ || row[i].value_.empty()) {
      return false;
    }
    int64_t int_value = 0;
    double double_value = 0.0;
    if (text_is_int64(row[i].value_, int_value) || text_is_double(row[i].value_, double_value)) {
      return false;
    }
  }
  return true;
}

int resolve_csv_header_mode(const std::string &path, int header_mode, int &effective_mode)
{
  effective_mode = 0;
  if (header_mode >= 0) {
    effective_mode = header_mode;
    return FILE_TABLE_OK;
  }
  CsvParser parser;
  int rc = parser.open(path);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  std::vector<CsvField> first;
  bool eof = false;
  rc = parser.next_row(first, eof);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  if (eof) {
    return FILE_TABLE_OK;  // empty file, no header and no data
  }
  std::vector<CsvField> second;
  rc = parser.next_row(second, eof);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  if (eof) {
    return FILE_TABLE_OK;  // a single row is always a data row
  }
  if (looks_like_header(first)) {
    effective_mode = 1;
  }
  return FILE_TABLE_OK;
}

int infer_csv_schema_impl(const std::string &path,
                           int header_mode,
                           std::vector<ColumnDesc> &columns,
                           int *effective_mode)
{
  columns.clear();
  int effective = 0;
  int rc = resolve_csv_header_mode(path, header_mode, effective);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  if (effective_mode != nullptr) {
    *effective_mode = effective;
  }
  CsvParser parser;
  rc = parser.open(path);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  std::vector<CsvField> row;
  bool eof = false;
  int64_t row_count = 0;
  std::vector<std::string> names;
  std::vector<ColumnState> states;
  if (effective == 1) {
    rc = parser.next_row(row, eof);
    if (rc != FILE_TABLE_OK) {
      return rc;
    }
    if (eof) {
      return FILE_TABLE_OK;  // empty file
    }
    build_header_names(row, names);
    // The column set is known even when the file has no data row.
    states.resize(names.size());
  }
  while (true) {
    rc = parser.next_row(row, eof);
    if (rc != FILE_TABLE_OK) {
      return rc;
    }
    if (eof) {
      break;
    }
    if (states.empty()) {
      states.resize(row.size());
      build_default_names(states.size(), names);
    }
    ++row_count;
    for (size_t i = 0; i < states.size(); ++i) {
      ColumnState &state = states[i];
      if (i >= row.size()) {
        state.has_null_ = true;
        continue;
      }
      const CsvField &field = row[i];
      if (field.is_null_) {
        state.has_null_ = true;
        continue;
      }
      state.has_value_ = true;
      ++state.present_count_;
      const ColumnKind kind = classify_text(field.value_);
      state.kind_ = merge_kinds(state.kind_, kind);
      state.fraction_digits_ = std::max(state.fraction_digits_, text_fraction_digits(field.value_));
      if (kind == ColumnKind::VARCHAR) {
        state.max_length_ = std::max(state.max_length_, (int32_t)field.value_.size());
      }
    }
  }
  const int64_t total_rows = (int64_t)row_count;
  columns.resize(states.size());
  for (size_t i = 0; i < states.size(); ++i) {
    columns[i].name_ = (i < names.size()) ? names[i] : ("column" + std::to_string(i));
    finalize_column(states[i], total_rows, columns[i]);
  }
  return FILE_TABLE_OK;
}
}  // namespace

int infer_csv_schema(const std::string &path, int header_mode, std::vector<ColumnDesc> &columns)
{
  if (header_mode < -1 || header_mode > 1) {
    set_last_error("invalid header mode: " + std::to_string(header_mode));
    return FILE_TABLE_ERR_PARAM;
  }
  return infer_csv_schema_impl(path, header_mode, columns, nullptr);
}

int create_csv_reader(const std::string &path, int header_mode, FileScanReader *&reader)
{
  reader = nullptr;
  if (header_mode < -1 || header_mode > 1) {
    set_last_error("invalid header mode: " + std::to_string(header_mode));
    return FILE_TABLE_ERR_PARAM;
  }
  std::vector<ColumnDesc> columns;
  int effective = 0;
  int rc = infer_csv_schema_impl(path, header_mode, columns, &effective);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  CsvReader *csv_reader = new CsvReader(path, effective == 1, columns);
  int open_rc = csv_reader->open();
  if (open_rc != FILE_TABLE_OK) {
    delete csv_reader;
    return open_rc;
  }
  reader = csv_reader;
  return FILE_TABLE_OK;
}

int scan_csv(const std::string &path,
             int header_mode,
             std::vector<ColumnDesc> &columns,
             std::vector<std::vector<Cell>> &rows)
{
  rows.clear();
  FileScanReader *reader = nullptr;
  int rc = create_csv_reader(path, header_mode, reader);
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

// ---------------------------------------------------------------------------
// CSV writer
// ---------------------------------------------------------------------------

namespace
{
// Append one field.  A NULL field is written as the null string, an empty
// value is written as an explicitly quoted empty field so that it is not read
// back as NULL.  A value that contains the separator, a quote character, a
// line break or leading / trailing blanks is quoted, an embedded quote is
// escaped with the escape character.
void append_csv_field(std::string &out,
                      const std::string &value,
                      const std::string &separator,
                      const std::string &nullstr,
                      bool quote_all,
                      bool is_null,
                      const std::string &quote_cht,
                      const std::string &escape_cht)
{
  if (is_null) {
    out += nullstr;
    return;
  }
  const char quote_char = quote_cht.empty() ? '"' : quote_cht[0];
  const char escape_char = escape_cht.empty() ? quote_char : escape_cht[0];
  if (value.empty()) {
    out.push_back(quote_char);
    out.push_back(quote_char);
    return;
  }
  bool need_quote = quote_all || value.find(separator) != std::string::npos
                    || value.find(quote_char) != std::string::npos
                    || value.find('\n') != std::string::npos
                    || value.find('\r') != std::string::npos;
  if (!need_quote && escape_char != quote_char
      && value.find(escape_char) != std::string::npos) {
    /* the escape character is doubled, the field has to be quoted to survive
       a round trip */
    need_quote = true;
  }
  if (!need_quote) {
    const char first = value[0];
    const char last = value[value.size() - 1];
    if (first == ' ' || first == '\t' || last == ' ' || last == '\t') {
      need_quote = true;
    }
  }
  if (!need_quote) {
    out += value;
    return;
  }
  out.push_back(quote_char);
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == quote_char) {
      if (escape_char == quote_char) {
        out.push_back(quote_char);
        out.push_back(quote_char);
      } else {
        out.push_back(escape_char);
        out.push_back(quote_char);
      }
    } else if (escape_char != quote_char && c == escape_char) {
      out.push_back(escape_char);
      out.push_back(c);
    } else {
      out.push_back(c);
    }
  }
  out.push_back(quote_char);
}

// Append one row.  The NULL flags of the row are given explicitly, a flag of
// zero means that the field carries a value.
void append_csv_row_ex(std::string &out,
                       const std::vector<std::string> &row,
                       const std::vector<char> &null_flags,
                       const std::string &separator,
                       const std::string &nullstr,
                       bool quote_all,
                       const std::string &quote_cht,
                       const std::string &escape_cht)
{
  for (size_t i = 0; i < row.size(); ++i) {
    if (i > 0) {
      out += separator;
    }
    const bool is_null = (i < null_flags.size()) && (0 != null_flags[i]);
    append_csv_field(out, row[i], separator, nullstr, quote_all, is_null, quote_cht, escape_cht);
  }
  out.push_back('\n');
}

void append_csv_row(std::string &out,
                    const std::vector<std::string> &row,
                    const std::string &separator,
                    const std::string &nullstr,
                    bool quote_all)
{
  // An empty field of the batch writer denotes NULL.
  std::vector<char> null_flags(row.size(), 0);
  for (size_t i = 0; i < row.size(); ++i) {
    null_flags[i] = row[i].empty() ? 1 : 0;
  }
  append_csv_row_ex(out, row, null_flags, separator, nullstr, quote_all,
                    std::string(), std::string());
}
}  // namespace

int write_csv_ex(const std::string &path,
                 const std::vector<std::string> &headers,
                 const std::vector<std::vector<std::string>> &rows,
                 const std::string &field_sep,
                 const std::string &nullstr,
                 bool with_header,
                 bool quote_all)
{
  const std::string separator = field_sep.empty() ? std::string(",") : field_sep;
  std::FILE *fp = std::fopen(path.c_str(), "wb");
  if (fp == nullptr) {
    set_last_error("cannot create file: " + path);
    return FILE_TABLE_ERR_IO;
  }
  std::string buffer;
  if (with_header) {
    append_csv_row(buffer, headers, separator, nullstr, quote_all);
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    append_csv_row(buffer, rows[i], separator, nullstr, quote_all);
  }
  const size_t written = std::fwrite(buffer.data(), 1, buffer.size(), fp);
  const bool ok = (written == buffer.size());
  std::fclose(fp);
  if (!ok) {
    set_last_error("cannot write file: " + path);
    return FILE_TABLE_ERR_IO;
  }
  return FILE_TABLE_OK;
}

int write_csv(const std::string &path,
              const std::vector<std::string> &headers,
              const std::vector<std::vector<std::string>> &rows,
              const std::string &field_sep,
              const std::string &nullstr,
              bool with_header)
{
  return write_csv_ex(path, headers, rows, field_sep, nullstr, with_header, false);
}

// ---------------------------------------------------------------------------
// Streaming CSV writer
// ---------------------------------------------------------------------------

CsvWriter::CsvWriter() : fp_(nullptr) {}

CsvWriter::~CsvWriter() { close(); }

int CsvWriter::open(const std::string &path,
                    const std::string &field_sep,
                    const std::string &nullstr,
                    const std::string &quote_cht,
                    const std::string &escape_cht)
{
  int ret = FILE_TABLE_OK;
  close();
  if (path.empty()) {
    set_last_error("cannot create file: empty path");
    return FILE_TABLE_ERR_PARAM;
  }
  if (nullptr == (fp_ = std::fopen(path.c_str(), "wb"))) {
    set_last_error("cannot create file: " + path);
    return FILE_TABLE_ERR_IO;
  }
  path_ = path;
  separator_ = field_sep.empty() ? std::string(",") : field_sep;
  nullstr_ = nullstr;
  quote_ = quote_cht.empty() ? std::string("\"") : quote_cht;
  escape_ = escape_cht.empty() ? quote_ : escape_cht;
  buffer_.clear();
  return ret;
}

int CsvWriter::flush_buffer()
{
  int ret = FILE_TABLE_OK;
  if (!buffer_.empty()) {
    if (nullptr == fp_) {
      set_last_error("csv writer is not open");
      ret = FILE_TABLE_ERR_IO;
    } else {
      const size_t written = std::fwrite(buffer_.data(), 1, buffer_.size(), fp_);
      if (written != buffer_.size()) {
        set_last_error("cannot write file: " + path_);
        ret = FILE_TABLE_ERR_IO;
      }
      buffer_.clear();
    }
  }
  return ret;
}

int CsvWriter::write_header(const std::vector<std::string> &headers)
{
  int ret = FILE_TABLE_OK;
  if (nullptr == fp_) {
    set_last_error("csv writer is not open");
    return FILE_TABLE_ERR_IO;
  }
  std::vector<char> null_flags(headers.size(), 0);
  append_csv_row_ex(buffer_, headers, null_flags, separator_, nullstr_, false, quote_, escape_);
  return ret;
}

int CsvWriter::write_row(const std::vector<std::string> &values, const std::vector<char> &null_flags)
{
  int ret = FILE_TABLE_OK;
  if (nullptr == fp_) {
    set_last_error("csv writer is not open");
    return FILE_TABLE_ERR_IO;
  }
  append_csv_row_ex(buffer_, values, null_flags, separator_, nullstr_, false, quote_, escape_);
  return ret;
}

int CsvWriter::close()
{
  int ret = FILE_TABLE_OK;
  if (nullptr != fp_) {
    if (FILE_TABLE_OK != flush_buffer()) {
      ret = FILE_TABLE_ERR_IO;
    }
    if (0 != std::fclose(fp_)) {
      set_last_error("cannot close file: " + path_);
      ret = FILE_TABLE_ERR_IO;
    }
    fp_ = nullptr;
  }
  buffer_.clear();
  return ret;
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

bool file_exists(const std::string &path)
{
  if (path.empty()) {
    return false;
  }
  std::FILE *fp = std::fopen(path.c_str(), "rb");
  if (nullptr == fp) {
    return false;
  }
  std::fclose(fp);
  return true;
}

int rename_file(const std::string &from, const std::string &to)
{
  if (from.empty() || to.empty()) {
    set_last_error("cannot rename file: empty path");
    return FILE_TABLE_ERR_PARAM;
  }
  // The target must not be replaced silently, the caller decides whether an
  // existing target is an error.
  if (file_exists(to)) {
    set_last_error("file already exists: " + to);
    return FILE_TABLE_ERR_IO;
  }
  if (0 != std::rename(from.c_str(), to.c_str())) {
    set_last_error("cannot rename file: " + from + " -> " + to);
    return FILE_TABLE_ERR_IO;
  }
  return FILE_TABLE_OK;
}

int remove_file(const std::string &path)
{
  if (path.empty()) {
    return FILE_TABLE_OK;
  }
  if (0 != std::remove(path.c_str())) {
    // A file that is already gone is not an error.
    if (!file_exists(path)) {
      return FILE_TABLE_OK;
    }
    set_last_error("cannot remove file: " + path);
    return FILE_TABLE_ERR_IO;
  }
  return FILE_TABLE_OK;
}

}  // namespace file_table
}  // namespace sql
}  // namespace oceanbase
