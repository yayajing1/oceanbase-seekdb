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

#include <cmath>
#include <cstring>

namespace oceanbase
{
namespace sql
{
namespace file_table
{

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

namespace
{
// The error message is kept per thread so that concurrent scans do not
// overwrite each other.
thread_local std::string g_last_error;
}  // namespace

void set_last_error(const std::string &message)
{
  g_last_error = message;
}

const std::string &get_last_error()
{
  return g_last_error;
}

// ---------------------------------------------------------------------------
// Deterministic value classification
// ---------------------------------------------------------------------------

namespace
{
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline bool is_leap_year(int64_t year)
{
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

inline int days_in_month(int64_t year, int month)
{
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return kDays[month - 1];
}

// Parse a fixed width decimal number, return false on any deviation.
inline bool parse_fixed_digits(const std::string &text, size_t pos, size_t count, int64_t &value)
{
  if (pos + count > text.size()) {
    return false;
  }
  int64_t result = 0;
  for (size_t i = 0; i < count; ++i) {
    const char c = text[pos + i];
    if (!is_digit(c)) {
      return false;
    }
    result = result * 10 + (c - '0');
  }
  value = result;
  return true;
}

// Strict decimal number parser that does not depend on the C locale.
// The mantissa is limited to 19 digits which is enough to detect the value
// range; the result is only used for type classification.
bool parse_decimal(const std::string &text, double &value)
{
  const size_t n = text.size();
  size_t i = 0;
  bool negative = false;
  if (i < n && (text[i] == '+' || text[i] == '-')) {
    negative = (text[i] == '-');
    ++i;
  }
  uint64_t mantissa = 0;
  int significant = 0;
  int scale = 0;
  int64_t exp_adjust = 0;
  bool has_digit = false;
  for (; i < n && is_digit(text[i]); ++i) {
    has_digit = true;
    if (significant < 19) {
      mantissa = mantissa * 10 + (uint64_t)(text[i] - '0');
      ++significant;
    } else {
      // Integer digits beyond the mantissa precision shift the value.
      ++exp_adjust;
    }
  }
  if (i < n && text[i] == '.') {
    ++i;
    for (; i < n && is_digit(text[i]); ++i) {
      has_digit = true;
      if (significant < 19) {
        mantissa = mantissa * 10 + (uint64_t)(text[i] - '0');
        ++significant;
      }
      // Every fraction digit counts towards the scale, dropped digits are
      // removed together with their power of ten.
      ++scale;
    }
  }
  if (!has_digit) {
    return false;
  }
  int64_t exponent = 0;
  if (i < n && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    bool exp_negative = false;
    if (i < n && (text[i] == '+' || text[i] == '-')) {
      exp_negative = (text[i] == '-');
      ++i;
    }
    if (i >= n || !is_digit(text[i])) {
      return false;
    }
    int64_t exp_value = 0;
    for (; i < n && is_digit(text[i]); ++i) {
      if (exp_value < 100000) {
        exp_value = exp_value * 10 + (text[i] - '0');
      }
    }
    exponent = exp_negative ? -exp_value : exp_value;
  }
  if (i != n) {
    return false;
  }
  int64_t power = exponent + exp_adjust - scale;
  double result = (double)mantissa;
  if (power > 0) {
    for (int64_t k = 0; k < power; ++k) {
      result *= 10.0;
    }
  } else {
    for (int64_t k = 0; k > power; --k) {
      result /= 10.0;
    }
  }
  if (!std::isfinite(result)) {
    return false;
  }
  value = negative ? -result : result;
  return true;
}
}  // namespace

bool text_is_int64(const std::string &text, int64_t &value)
{
  const size_t n = text.size();
  if (n == 0) {
    return false;
  }
  size_t i = 0;
  bool negative = false;
  if (text[i] == '+' || text[i] == '-') {
    negative = (text[i] == '-');
    ++i;
  }
  if (i >= n) {
    return false;
  }
  const uint64_t limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
  uint64_t acc = 0;
  for (; i < n; ++i) {
    const char c = text[i];
    if (!is_digit(c)) {
      return false;
    }
    const uint64_t digit = (uint64_t)(c - '0');
    if (acc > limit / 10 || (acc == limit / 10 && digit > limit % 10)) {
      return false;  // out of the int64 range
    }
    acc = acc * 10 + digit;
  }
  value = negative ? (int64_t)(0 - acc) : (int64_t)acc;
  return true;
}

bool text_is_double(const std::string &text, double &value)
{
  return parse_decimal(text, value);
}

bool text_is_boolean(const std::string &text, bool &value)
{
  if (text == "true" || text == "TRUE" || text == "t" || text == "T") {
    value = true;
    return true;
  }
  if (text == "false" || text == "FALSE" || text == "f" || text == "F") {
    value = false;
    return true;
  }
  return false;
}

bool text_is_date(const std::string &text)
{
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return false;
  }
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  if (!parse_fixed_digits(text, 0, 4, year) || !parse_fixed_digits(text, 5, 2, month)
      || !parse_fixed_digits(text, 8, 2, day)) {
    return false;
  }
  if (year < 1 || year > 9999) {
    return false;
  }
  if (month < 1 || month > 12) {
    return false;
  }
  if (day < 1 || day > days_in_month(year, (int)month)) {
    return false;
  }
  return true;
}

bool text_is_datetime(const std::string &text)
{
  // YYYY-MM-DD is always 10 characters, the separator and the time part follow.
  if (text.size() < 16) {
    return false;
  }
  const std::string date_part = text.substr(0, 10);
  if (!text_is_date(date_part)) {
    return false;
  }
  const char separator = text[10];
  if (separator != 'T' && separator != ' ') {
    return false;
  }
  size_t pos = 11;
  int64_t hour = 0;
  int64_t minute = 0;
  if (!parse_fixed_digits(text, pos, 2, hour) || text[pos + 2] != ':') {
    return false;
  }
  pos += 3;
  if (!parse_fixed_digits(text, pos, 2, minute)) {
    return false;
  }
  pos += 2;
  if (hour > 23 || minute > 59) {
    return false;
  }
  if (pos == text.size()) {
    return true;  // HH:MM
  }
  if (text[pos] != ':') {
    return false;
  }
  ++pos;
  int64_t second = 0;
  if (!parse_fixed_digits(text, pos, 2, second)) {
    return false;
  }
  pos += 2;
  if (second > 59) {
    return false;
  }
  if (pos == text.size()) {
    return true;  // HH:MM:SS
  }
  if (text[pos] != '.') {
    return false;
  }
  ++pos;
  size_t fraction_begin = pos;
  while (pos < text.size() && is_digit(text[pos])) {
    ++pos;
  }
  const size_t fraction_digits = pos - fraction_begin;
  if (fraction_digits == 0 || fraction_digits > 9) {
    return false;
  }
  return pos == text.size();
}

ColumnKind classify_text(const std::string &text)
{
  int64_t int_value = 0;
  double double_value = 0.0;
  bool bool_value = false;
  if (text_is_int64(text, int_value)) {
    return ColumnKind::BIGINT;
  }
  if (text_is_double(text, double_value)) {
    return ColumnKind::DOUBLE;
  }
  if (text_is_boolean(text, bool_value)) {
    return ColumnKind::BOOLEAN;
  }
  if (text_is_date(text)) {
    return ColumnKind::DATE;
  }
  if (text_is_datetime(text)) {
    return ColumnKind::DATETIME;
  }
  return ColumnKind::VARCHAR;
}

ColumnKind merge_kinds(ColumnKind lhs, ColumnKind rhs)
{
  if (lhs == rhs) {
    return lhs;
  }
  if (lhs == ColumnKind::UNKNOWN) {
    return rhs;
  }
  if (rhs == ColumnKind::UNKNOWN) {
    return lhs;
  }
  if (lhs == ColumnKind::NULL_ONLY) {
    return rhs;
  }
  if (rhs == ColumnKind::NULL_ONLY) {
    return lhs;
  }
  // Text wins over every other type, a column that mixes a number and a
  // string is a string column.
  if (lhs == ColumnKind::VARCHAR || rhs == ColumnKind::VARCHAR) {
    return ColumnKind::VARCHAR;
  }
  const bool lhs_temporal = (lhs == ColumnKind::DATE || lhs == ColumnKind::DATETIME);
  const bool rhs_temporal = (rhs == ColumnKind::DATE || rhs == ColumnKind::DATETIME);
  if (lhs_temporal || rhs_temporal) {
    if (lhs_temporal && rhs_temporal) {
      return (lhs == ColumnKind::DATETIME || rhs == ColumnKind::DATETIME) ? ColumnKind::DATETIME
                                                                         : ColumnKind::DATE;
    }
    // A timestamp mixed with a number or a boolean cannot be represented.
    return ColumnKind::VARCHAR;
  }
  if (lhs == ColumnKind::DOUBLE || rhs == ColumnKind::DOUBLE) {
    return ColumnKind::DOUBLE;
  }
  if (lhs == ColumnKind::BIGINT || rhs == ColumnKind::BIGINT) {
    return ColumnKind::BIGINT;
  }
  if (lhs == ColumnKind::BOOLEAN || rhs == ColumnKind::BOOLEAN) {
    return ColumnKind::BOOLEAN;
  }
  return ColumnKind::VARCHAR;
}

int32_t text_fraction_digits(const std::string &text)
{
  // The number of digits behind the decimal point of a decimal literal.
  const size_t dot = text.find('.');
  if (dot == std::string::npos) {
    return 0;
  }
  int32_t digits = 0;
  for (size_t i = dot + 1; i < text.size(); ++i) {
    if (!is_digit(text[i])) {
      break;
    }
    ++digits;
  }
  return digits;
}

// ---------------------------------------------------------------------------
// Type inference
// ---------------------------------------------------------------------------

void finalize_column(const ColumnInferenceState &state, int64_t total_rows, ColumnDesc &desc)
{
  desc.kind_ = state.has_value_ ? state.kind_ : ColumnKind::NULL_ONLY;
  // A column is nullable when a NULL value was observed or when the column
  // was missing in at least one of the scanned rows.
  desc.nullable_ = state.has_null_ || state.present_count_ < total_rows;
  desc.length_ = (desc.kind_ == ColumnKind::VARCHAR) ? state.max_length_ : 0;
  desc.precision_ = (desc.kind_ == ColumnKind::DOUBLE) ? state.fraction_digits_ : 0;
}

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

const char *column_kind_name(ColumnKind kind)
{
  switch (kind) {
    case ColumnKind::UNKNOWN: return "UNKNOWN";
    case ColumnKind::BOOLEAN: return "BOOLEAN";
    case ColumnKind::BIGINT: return "BIGINT";
    case ColumnKind::DOUBLE: return "DOUBLE";
    case ColumnKind::VARCHAR: return "VARCHAR";
    case ColumnKind::DATE: return "DATE";
    case ColumnKind::DATETIME: return "DATETIME";
    case ColumnKind::JSON: return "JSON";
    case ColumnKind::NULL_ONLY: return "NULL_ONLY";
  }
  return "UNKNOWN";
}

int validate_sample_size(int64_t sample_size)
{
  // The deterministic inference always scans the whole file.
  if (sample_size != -1) {
    set_last_error("only a full scan (-1) is supported, got: " + std::to_string(sample_size));
    return FILE_TABLE_ERR_PARAM;
  }
  return FILE_TABLE_OK;
}

// ---------------------------------------------------------------------------
// Buffered text input
// ---------------------------------------------------------------------------

namespace
{
const size_t kFileBufferSize = 64 * 1024;
}  // namespace

TextFileInput::TextFileInput() : buf_(kFileBufferSize) {}

TextFileInput::~TextFileInput() { close(); }

int TextFileInput::open(const std::string &path)
{
  close();
  path_ = path;
  fp_ = std::fopen(path.c_str(), "rb");
  if (fp_ == nullptr) {
    set_last_error("cannot open file: " + path);
    return FILE_TABLE_ERR_IO;
  }
  pos_ = 0;
  len_ = 0;
  pushback_ = -1;
  eof_ = false;
  return FILE_TABLE_OK;
}

void TextFileInput::close()
{
  if (fp_ != nullptr) {
    std::fclose(fp_);
    fp_ = nullptr;
  }
  pos_ = 0;
  len_ = 0;
  pushback_ = -1;
  eof_ = false;
}

int TextFileInput::fill()
{
  if (eof_) {
    return FILE_TABLE_OK;
  }
  len_ = std::fread(&buf_[0], 1, buf_.size(), fp_);
  pos_ = 0;
  if (len_ == 0) {
    eof_ = true;
  }
  return FILE_TABLE_OK;
}

int TextFileInput::read_char(int &ch)
{
  if (pushback_ >= 0) {
    ch = pushback_;
    pushback_ = -1;
    return FILE_TABLE_OK;
  }
  if (pos_ >= len_) {
    int rc = fill();
    if (rc != FILE_TABLE_OK) {
      ch = -1;
      return rc;
    }
    if (len_ == 0) {
      ch = -1;
      return FILE_TABLE_OK;
    }
  }
  ch = (int)(unsigned char)buf_[pos_++];
  return FILE_TABLE_OK;
}

int TextFileInput::peek_char(int &ch)
{
  int value = -1;
  int rc = read_char(value);
  if (rc != FILE_TABLE_OK) {
    ch = -1;
    return rc;
  }
  pushback_ = value;
  ch = value;
  return FILE_TABLE_OK;
}

}  // namespace file_table
}  // namespace sql
}  // namespace oceanbase
