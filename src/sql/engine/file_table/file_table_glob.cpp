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
#include <filesystem>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_table
{

// ---------------------------------------------------------------------------
// Glob pattern matching
// ---------------------------------------------------------------------------

namespace
{

// Match a file name against a glob pattern.  The pattern supports '*' (any
// sequence of characters), '?' (exactly one character) and the '[...]'
// character class with an optional range and an optional '!' or '^' negation.
bool glob_match(const char *pattern, const char *name)
{
  while (*pattern != '\0') {
    if (*pattern == '*') {
      while (*pattern == '*') {
        ++pattern;
      }
      if (*pattern == '\0') {
        return true;
      }
      for (const char *candidate = name;; ++candidate) {
        if (glob_match(pattern, candidate)) {
          return true;
        }
        if (*candidate == '\0') {
          return false;
        }
      }
    }
    if (*name == '\0') {
      return false;
    }
    if (*pattern == '?') {
      ++pattern;
      ++name;
      continue;
    }
    if (*pattern == '[') {
      const char *cursor = pattern + 1;
      bool negated = false;
      if (*cursor == '!' || *cursor == '^') {
        negated = true;
        ++cursor;
      }
      bool matched = false;
      bool first = true;
      while (*cursor != '\0' && (*cursor != ']' || first)) {
        first = false;
        const char low = *cursor++;
        if (*cursor == '-' && cursor[1] != '\0' && cursor[1] != ']') {
          const char high = cursor[1];
          if ((unsigned char)*name >= (unsigned char)low
              && (unsigned char)*name <= (unsigned char)high) {
            matched = true;
          }
          cursor += 2;
        } else {
          if ((unsigned char)*name == (unsigned char)low) {
            matched = true;
          }
        }
      }
      if (*cursor == ']') {
        if (matched == negated) {
          return false;
        }
        pattern = cursor + 1;
        ++name;
        continue;
      }
      // An unterminated character class, the '[' is a literal character.
      if (*name != '[') {
        return false;
      }
      ++pattern;
      ++name;
      continue;
    }
    if (*pattern != *name) {
      return false;
    }
    ++pattern;
    ++name;
  }
  return *name == '\0';
}

// Split a pattern into the directory part and the name pattern.  Both the
// '/' and the '\' separator are accepted.
void split_pattern(const std::string &pattern, std::string &directory, std::string &name_pattern)
{
  const size_t pos = pattern.find_last_of("/\\");
  if (pos == std::string::npos) {
    directory.clear();
    name_pattern = pattern;
    return;
  }
  directory = pattern.substr(0, pos + 1);
  name_pattern = pattern.substr(pos + 1);
}

bool has_wildcard(const std::string &text)
{
  return text.find_first_of("*?[") != std::string::npos;
}

// Enumerate the files that match the pattern, the result is sorted by path.
int glob_collect(const std::string &pattern, std::vector<std::string> &files)
{
  files.clear();
  std::string directory;
  std::string name_pattern;
  split_pattern(pattern, directory, name_pattern);
  if (!has_wildcard(name_pattern)) {
    // The pattern is a plain path, it must be an existing regular file.
    const std::string path = directory + name_pattern;
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(std::filesystem::path(path), ec);
    if (ec || !std::filesystem::exists(status)) {
      set_last_error("file does not exist: " + path);
      return FILE_TABLE_ERR_IO;
    }
    if (!std::filesystem::is_regular_file(status)) {
      set_last_error("not a regular file: " + path);
      return FILE_TABLE_ERR_IO;
    }
    files.push_back(path);
    return FILE_TABLE_OK;
  }
  // The pattern holds a wildcard, the directory part must exist.
  const std::filesystem::path directory_path =
      directory.empty() ? std::filesystem::path(".") : std::filesystem::path(directory);
  std::error_code ec;
  if (!std::filesystem::is_directory(directory_path, ec) || ec) {
    set_last_error("directory does not exist: " + directory);
    return FILE_TABLE_ERR_IO;
  }
  std::filesystem::directory_iterator iterator(directory_path, ec);
  if (ec) {
    set_last_error("cannot read directory: " + directory);
    return FILE_TABLE_ERR_IO;
  }
  // The iterator is advanced with an error code so that no exception can
  // escape from the enumeration.
  const std::filesystem::directory_iterator end_iterator;
  for (; iterator != end_iterator; iterator.increment(ec)) {
    if (ec) {
      break;
    }
    std::error_code entry_ec;
    if (!std::filesystem::is_regular_file(iterator->path(), entry_ec) || entry_ec) {
      continue;  // only regular files are listed
    }
    const std::string name = iterator->path().filename().string();
    if (glob_match(name_pattern.c_str(), name.c_str())) {
      files.push_back(directory + name);
    }
  }
  std::sort(files.begin(), files.end());
  return FILE_TABLE_OK;
}

// Build the description of the single 'file' column.
void build_glob_columns(const std::vector<std::string> &files, std::vector<ColumnDesc> &columns)
{
  ColumnInferenceState state;
  state.has_value_ = true;
  state.present_count_ = (int64_t)files.size();
  state.kind_ = ColumnKind::VARCHAR;
  for (size_t i = 0; i < files.size(); ++i) {
    state.max_length_ = std::max(state.max_length_, (int32_t)files[i].size());
  }
  columns.clear();
  columns.resize(1);
  columns[0].name_ = "file";
  finalize_column(state, (int64_t)files.size(), columns[0]);
  // The column always holds a path.
  columns[0].kind_ = ColumnKind::VARCHAR;
}

// ---------------------------------------------------------------------------
// Glob reader
// ---------------------------------------------------------------------------

class GlobReader : public FileScanReader
{
public:
  GlobReader(const std::vector<std::string> &files, const std::vector<ColumnDesc> &columns)
  {
    files_ = files;
    columns_ = columns;
  }

  ~GlobReader() override { close(); }

  int get_next_batch(std::vector<std::vector<Cell>> &rows, int64_t max_rows) override
  {
    rows.clear();
    if (max_rows <= 0) {
      return FILE_TABLE_OK;
    }
    const int64_t remain = (int64_t)files_.size() - (int64_t)pos_;
    const int64_t count = std::min(max_rows, remain);
    for (int64_t i = 0; i < count; ++i) {
      std::vector<Cell> row(1);
      row[0].is_null_ = false;
      row[0].text_ = files_[(size_t)(pos_ + i)];
      rows.push_back(row);
    }
    pos_ += (size_t)count;
    return FILE_TABLE_OK;
  }

  int rescan() override
  {
    pos_ = 0;
    return FILE_TABLE_OK;
  }

  int close() override
  {
    pos_ = 0;
    return FILE_TABLE_OK;
  }

private:
  std::vector<std::string> files_;
  size_t pos_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

int create_glob_reader(const std::string &pattern, FileScanReader *&reader)
{
  reader = nullptr;
  std::vector<std::string> files;
  int rc = glob_collect(pattern, files);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  std::vector<ColumnDesc> columns;
  build_glob_columns(files, columns);
  reader = new GlobReader(files, columns);
  return FILE_TABLE_OK;
}

int list_glob(const std::string &pattern,
              std::vector<ColumnDesc> &columns,
              std::vector<std::vector<Cell>> &rows)
{
  columns.clear();
  rows.clear();
  std::vector<std::string> files;
  int rc = glob_collect(pattern, files);
  if (rc != FILE_TABLE_OK) {
    return rc;
  }
  build_glob_columns(files, columns);
  for (size_t i = 0; i < files.size(); ++i) {
    std::vector<Cell> row(1);
    row[0].is_null_ = false;
    row[0].text_ = files[i];
    rows.push_back(row);
  }
  return FILE_TABLE_OK;
}

}  // namespace file_table
}  // namespace sql
}  // namespace oceanbase
