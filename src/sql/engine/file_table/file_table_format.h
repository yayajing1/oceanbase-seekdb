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

#ifndef OCEANBASE_SQL_ENGINE_FILE_TABLE_FILE_TABLE_FORMAT_H_
#define OCEANBASE_SQL_ENGINE_FILE_TABLE_FILE_TABLE_FORMAT_H_

// Pure C++17 file format engine for the "file as database" feature.
//
// This layer is intentionally free of any OceanBase dependency: it only uses
// the C++ standard library so that it can be compiled and unit tested in
// isolation.  The SQL layer (parser / executor) lives on top of this API.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_table
{

// Supported physical file formats.
enum class FileFormat { CSV = 0, JSONL = 1, PARQUET = 2, GLOB = 3 };

// Logical column types produced by the deterministic type inference.
//
//   UNKNOWN   : not inferred yet
//   BOOLEAN   : true / false literals
//   BIGINT    : signed 64 bit integer
//   DOUBLE    : floating point number
//   VARCHAR   : free text
//   DATE      : YYYY-MM-DD
//   DATETIME  : YYYY-MM-DD ('T' | ' ') HH:MM[:SS[.fraction]]
//   JSON      : reserved for nested documents (not produced by the scanners)
//   NULL_ONLY : every observed value is NULL
enum class ColumnKind {
  UNKNOWN = 0,
  BOOLEAN = 1,
  BIGINT = 2,
  DOUBLE = 3,
  VARCHAR = 4,
  DATE = 5,
  DATETIME = 6,
  JSON = 7,
  NULL_ONLY = 8
};

// Description of one inferred column.
struct ColumnDesc {
  // Column name.  For CSV files with duplicated header names the k-th
  // occurrence (k >= 2) is renamed to "<name>__<k>".
  std::string name_;
  // Inferred logical type.
  ColumnKind kind_ = ColumnKind::UNKNOWN;
  // True when at least one NULL value was observed in the column.
  bool nullable_ = true;
  // Maximum value length in bytes (VARCHAR columns only).
  int32_t length_ = 0;
  // Number of retained fractional digits (DOUBLE columns only), 0 means unknown.
  int32_t precision_ = 0;
};

// One materialized cell.  The raw text of the value is kept verbatim so that
// the SQL layer can apply its own cast rules.
struct Cell {
  bool is_null_ = true;
  std::string text_;
};

// Error codes returned by every entry point of this layer.
constexpr int FILE_TABLE_OK         = 0;
constexpr int FILE_TABLE_ERR_IO     = -1;  // file does not exist / cannot be opened
constexpr int FILE_TABLE_ERR_FORMAT = -2;  // unsupported format
constexpr int FILE_TABLE_ERR_PARSE  = -3;  // malformed content
constexpr int FILE_TABLE_ERR_PARAM  = -4;  // illegal argument
constexpr int FILE_TABLE_ERR_NESTED = -5;  // nested object / array found in JSONL

// Streaming scan reader.
//
// The reader never materializes a whole file: get_next_batch() returns at
// most max_rows rows and the file is re-opened from the beginning by
// rescan().  A return value of FILE_TABLE_OK together with an empty row
// vector means "end of file".
class FileScanReader
{
public:
  virtual ~FileScanReader() {}
  // Fetch the next batch of at most max_rows rows.
  virtual int get_next_batch(std::vector<std::vector<Cell>> &rows, int64_t max_rows) = 0;
  // Restart the scan from the first row.
  virtual int rescan() = 0;
  // Release all resources held by the reader.
  virtual int close() = 0;
  // Schema of the scanned relation.
  const std::vector<ColumnDesc> &get_columns() const { return columns_; }

protected:
  std::vector<ColumnDesc> columns_;
};

// ---------------------------------------------------------------------------
// Reader factories
// ---------------------------------------------------------------------------

// header_mode: -1 = auto detect the header row, 0 = no header row, 1 = header row.
int create_csv_reader(const std::string &path, int header_mode, FileScanReader *&reader);
int create_jsonl_reader(const std::string &path, FileScanReader *&reader);
int create_parquet_reader(const std::string &path, FileScanReader *&reader);
int create_glob_reader(const std::string &pattern, FileScanReader *&reader);

// ---------------------------------------------------------------------------
// Type inference (standalone full scan, the result is thrown away afterwards)
// ---------------------------------------------------------------------------

int infer_csv_schema(const std::string &path, int header_mode, std::vector<ColumnDesc> &columns);
int infer_jsonl_schema(const std::string &path, std::vector<ColumnDesc> &columns);
int infer_parquet_schema(const std::string &path, std::vector<ColumnDesc> &columns);

// ---------------------------------------------------------------------------
// One shot full scans (small files and tests only)
// ---------------------------------------------------------------------------

int scan_csv(const std::string &path,
             int header_mode,
             std::vector<ColumnDesc> &columns,
             std::vector<std::vector<Cell>> &rows);
int scan_jsonl(const std::string &path,
               std::vector<ColumnDesc> &columns,
               std::vector<std::vector<Cell>> &rows);
int scan_parquet(const std::string &path,
                 std::vector<ColumnDesc> &columns,
                 std::vector<std::vector<Cell>> &rows);
int list_glob(const std::string &pattern,
              std::vector<ColumnDesc> &columns,
              std::vector<std::vector<Cell>> &rows);

// ---------------------------------------------------------------------------
// CSV writer (used by COPY ... TO)
// ---------------------------------------------------------------------------

// Write rows as CSV.  An empty field in `rows` denotes NULL and is written as
// `nullstr`.  Values are quoted with '"' when they contain the separator, a
// quote character, a line break or leading / trailing blanks; an embedded
// quote is doubled.
int write_csv(const std::string &path,
              const std::vector<std::string> &headers,
              const std::vector<std::vector<std::string>> &rows,
              const std::string &field_sep,
              const std::string &nullstr,
              bool with_header);

// ---------------------------------------------------------------------------
// Extensions (not part of the core API above, provided for convenience)
// ---------------------------------------------------------------------------

// Message of the last failed call of the current thread.
const std::string &get_last_error();

// Only -1 (full scan) is supported by the deterministic inference.
int validate_sample_size(int64_t sample_size);

// Human readable name of a column kind ("BIGINT", "VARCHAR", ...).
const char *column_kind_name(ColumnKind kind);

// Same as write_csv() but every non NULL value is quoted.
int write_csv_ex(const std::string &path,
                 const std::vector<std::string> &headers,
                 const std::vector<std::vector<std::string>> &rows,
                 const std::string &field_sep,
                 const std::string &nullstr,
                 bool with_header,
                 bool quote_all);

// ---------------------------------------------------------------------------
// Streaming CSV writer (used by COPY ... TO 'file')
// ---------------------------------------------------------------------------

// Streaming counterpart of write_csv().  The escaping rules are the ones of
// write_csv(), but the rows are written one by one so that a large result set
// never has to be kept in memory.
//
// A NULL field is written as `nullstr`, an empty string is written as an
// explicitly quoted empty field so that it is not read back as NULL.
class CsvWriter
{
public:
  CsvWriter();
  ~CsvWriter();
  // Open the target file.  `field_sep` is the field separator, a comma by
  // default, `nullstr` is the text of a NULL field, `quote_cht` is the
  // character a field is quoted with (a double quote by default) and
  // `escape_cht` is the character a quote is escaped with (the quote itself
  // by default, a doubled quote).
  int open(const std::string &path,
           const std::string &field_sep,
           const std::string &nullstr,
           const std::string &quote_cht = std::string(),
           const std::string &escape_cht = std::string());
  // Write the header row, the fields of a header are never NULL.
  int write_header(const std::vector<std::string> &headers);
  // Write one row, null_flags[i] != 0 marks the i-th field as NULL.
  int write_row(const std::vector<std::string> &values, const std::vector<char> &null_flags);
  int close();
  bool is_open() const { return fp_ != nullptr; }
  const std::string &path() const { return path_; }

private:
  int flush_buffer();

  std::FILE *fp_;
  std::string path_;
  std::string separator_;
  std::string nullstr_;
  std::string quote_;
  std::string escape_;
  std::string buffer_;
};

// True when the given path names an existing regular file.
bool file_exists(const std::string &path);
// Rename `from` to `to`.  An existing `to` is replaced.
int rename_file(const std::string &from, const std::string &to);
// Remove the given file, a missing file is not an error.
int remove_file(const std::string &path);

// Parquet reader restricted to the given columns.  Columns that are not
// selected are not decoded at all (column pruning).
int create_parquet_reader_with_columns(const std::string &path,
                                        const std::vector<std::string> &columns,
                                        FileScanReader *&reader);

}  // namespace file_table
}  // namespace sql
}  // namespace oceanbase

#endif  // OCEANBASE_SQL_ENGINE_FILE_TABLE_FILE_TABLE_FORMAT_H_
