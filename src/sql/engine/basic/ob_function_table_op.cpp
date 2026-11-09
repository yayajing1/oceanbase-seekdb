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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"

#include <cstdlib>
#include <cerrno>
#include <cstring>

namespace oceanbase
{
using namespace common;
namespace sql
{

// number of rows fetched from the file in one go, the file is never
// materialized in memory as a whole
static constexpr int64_t FILE_TABLE_BATCH_ROWS = 1024;

// case insensitive comparison of a scanned text with a lower case literal
static bool text_iequal(const std::string &text, const char *literal)
{
  const size_t len = strlen(literal);
  if (text.size() != len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    char ch = text.at(i);
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (ch != literal[i]) {
      return false;
    }
  }
  return true;
}

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    file_type_, file_header_mode_, file_path_, file_describe_mode_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (0 != MY_SPEC.file_type_) {
    // "file as database" table, the rows are streamed out of the file
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
    if (MY_SPEC.file_describe_mode_) {
      // the table answers the schema of the file instead of its content
      if (OB_FAIL(infer_file_desc_columns())) {
        LOG_WARN("fail to infer file table schema", K(ret), K(MY_SPEC.file_path_));
      }
    } else if (OB_FAIL(open_file_reader())) {
      LOG_WARN("fail to open file table", K(ret), K(MY_SPEC.file_path_));
    }
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::infer_file_desc_columns()
{
  int ret = OB_SUCCESS;
  const common::ObString &path = MY_SPEC.file_path_;
  int file_ret = file_table::FILE_TABLE_OK;
  file_desc_columns_.clear();
  file_desc_idx_ = 0;
  /* file_type_ is stored with an offset of one, zero means "not a file
     table", see ObFileTableDef. */
  switch (MY_SPEC.file_type_ - 1) {
    case 0:
      file_ret = file_table::infer_csv_schema(path.ptr(), MY_SPEC.file_header_mode_, file_desc_columns_);
      break;
    case 1:
      file_ret = file_table::infer_jsonl_schema(path.ptr(), file_desc_columns_);
      break;
    case 2:
      file_ret = file_table::infer_parquet_schema(path.ptr(), file_desc_columns_);
      break;
    case 3: {
      std::vector<std::vector<file_table::Cell> > rows;
      file_ret = file_table::list_glob(path.ptr(), file_desc_columns_, rows);
      break;
    }
    default:
      file_ret = file_table::FILE_TABLE_ERR_FORMAT;
      break;
  }
  if (file_table::FILE_TABLE_OK != file_ret) {
    if (file_table::FILE_TABLE_ERR_IO == file_ret) {
      ret = OB_FILE_NOT_EXIST;
      LOG_USER_ERROR(OB_FILE_NOT_EXIST, "cannot read file table");
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "cannot read file table");
    }
  }
  return ret;
}

int ObFunctionTableOp::open_file_reader()
{
  int ret = OB_SUCCESS;
  const common::ObString &path = MY_SPEC.file_path_;
  int file_ret = file_table::FILE_TABLE_OK;
  if (OB_ISNULL(file_reader_)) {
    /* file_type_ is stored with an offset of one, zero means "not a file
       table", see ObFileTableDef. */
    switch (MY_SPEC.file_type_ - 1) {
      case 0:
        file_ret = file_table::create_csv_reader(path.ptr(), MY_SPEC.file_header_mode_, file_reader_);
        break;
      case 1:
        file_ret = file_table::create_jsonl_reader(path.ptr(), file_reader_);
        break;
      case 2:
        file_ret = file_table::create_parquet_reader(path.ptr(), file_reader_);
        break;
      case 3:
        file_ret = file_table::create_glob_reader(path.ptr(), file_reader_);
        break;
      default:
        file_ret = file_table::FILE_TABLE_ERR_FORMAT;
        break;
    }
    if (file_table::FILE_TABLE_OK != file_ret) {
      if (file_table::FILE_TABLE_ERR_IO == file_ret) {
        ret = OB_FILE_NOT_EXIST;
        LOG_USER_ERROR(OB_FILE_NOT_EXIST, "cannot read file table");
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "cannot read file table");
      }
    } else if (OB_ISNULL(file_reader_)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (0 != MY_SPEC.file_type_) {
      file_rows_.clear();
      file_row_idx_ = 0;
      file_desc_idx_ = 0;
      if (OB_NOT_NULL(file_reader_) && OB_FAIL(file_reader_->rescan())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to rescan file table", K(ret), K(MY_SPEC.file_path_));
      }
    } else if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  file_rows_.clear();
  file_row_idx_ = 0;
  file_desc_columns_.clear();
  file_desc_idx_ = 0;
  if (OB_NOT_NULL(file_reader_)) {
    int file_ret = file_reader_->close();
    delete file_reader_;
    file_reader_ = NULL;
    if (file_table::FILE_TABLE_OK != file_ret) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to close file table", K(ret));
    }
  }
  return ret;
}

void ObFunctionTableOp::destroy()
{
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

// The memory of the file reader is released as soon as the next batch is
// read, so the text of a string column has to be copied into the arena of
// the expression before it is handed to the datum.
int ObFunctionTableOp::set_string_datum(ObExpr *expr, const char *text, uint32_t len)
{
  int ret = OB_SUCCESS;
  char *buf = expr->get_str_res_mem(eval_ctx_, len);
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (len > 0) {
      MEMCPY(buf, text, len);
    }
    expr->locate_datum_for_write(eval_ctx_).set_string(buf, len);
  }
  return ret;
}

// "DESCRIBE SELECT ... FROM read_csv(...)" answers one row per column of the
// file: column_name, column_type, null, key, default and extra.
int ObFunctionTableOp::inner_get_next_row_file_desc()
{
  int ret = OB_SUCCESS;
  if (file_desc_idx_ >= static_cast<int64_t>(file_desc_columns_.size())) {
    ret = OB_ITER_END;
  } else {
    const file_table::ColumnDesc &col = file_desc_columns_.at(file_desc_idx_++);
    const char *type_name = file_table::column_kind_name(col.kind_);
    if (file_table::ColumnKind::NULL_ONLY == col.kind_) {
      // a column that only holds NULL values is reported as NULL
      type_name = "NULL";
    }
    const char *nullable = col.nullable_ ? "YES" : "NO";
    const int64_t column_cnt = MY_SPEC.column_exprs_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < column_cnt; ++i) {
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      switch (i) {
        case 0:
          ret = set_string_datum(expr, col.name_.c_str(), static_cast<uint32_t>(col.name_.length()));
          break;
        case 1:
          ret = set_string_datum(expr, type_name, static_cast<uint32_t>(strlen(type_name)));
          break;
        case 2:
          ret = set_string_datum(expr, nullable, static_cast<uint32_t>(strlen(nullable)));
          break;
        default:
          // key, default and extra are not set for a file column
          expr->locate_datum_for_write(eval_ctx_).set_null();
          break;
      }
      if (OB_SUCC(ret)) {
        expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(ctx_.check_status())) {
  } else if (MY_SPEC.file_describe_mode_) {
    ret = inner_get_next_row_file_desc();
  } else if (OB_ISNULL(file_reader_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // fetch the next batch of rows, the whole file is never materialized
    while (OB_SUCC(ret) && file_row_idx_ >= static_cast<int64_t>(file_rows_.size())) {
      file_rows_.clear();
      file_row_idx_ = 0;
      int file_ret = file_reader_->get_next_batch(file_rows_, FILE_TABLE_BATCH_ROWS);
      if (file_table::FILE_TABLE_OK != file_ret) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "cannot read file table");
      } else if (file_rows_.empty()) {
        ret = OB_ITER_END;
      }
    }
    if (OB_SUCC(ret)) {
      const std::vector<file_table::Cell> &row = file_rows_.at(file_row_idx_++);
      const std::vector<file_table::ColumnDesc> &schema = file_reader_->get_columns();
      const int64_t column_cnt = MY_SPEC.column_exprs_.count();
      for (int64_t i = 0; OB_SUCC(ret) && i < column_cnt; ++i) {
        ObExpr *expr = MY_SPEC.column_exprs_.at(i);
        ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
        if (i >= static_cast<int64_t>(row.size()) || row.at(i).is_null_) {
          datum.set_null();
        } else {
          file_table::ColumnKind kind = file_table::ColumnKind::VARCHAR;
          if (i < static_cast<int64_t>(schema.size())) {
            kind = schema.at(i).kind_;
          }
          const std::string &text = row.at(i).text_;
          switch (kind) {
            case file_table::ColumnKind::BIGINT: {
              char *end = NULL;
              errno = 0;
              long long value = strtoll(text.c_str(), &end, 10);
              if (NULL == end || end == text.c_str() || 0 != errno) {
                datum.set_null();
              } else {
                datum.set_int(static_cast<int64_t>(value));
              }
              break;
            }
            case file_table::ColumnKind::DOUBLE: {
              char *end = NULL;
              double value = strtod(text.c_str(), &end);
              if (NULL == end || end == text.c_str()) {
                datum.set_null();
              } else {
                datum.set_double(value);
              }
              break;
            }
            case file_table::ColumnKind::BOOLEAN: {
              if (text_iequal(text, "true") || text_iequal(text, "t")
                  || text_iequal(text, "yes") || text_iequal(text, "on")) {
                datum.set_bool(true);
              } else if (text_iequal(text, "false") || text_iequal(text, "f")
                         || text_iequal(text, "no") || text_iequal(text, "off")) {
                datum.set_bool(false);
              } else {
                char *end = NULL;
                long long value = strtoll(text.c_str(), &end, 10);
                if (NULL == end || end == text.c_str()) {
                  datum.set_null();
                } else {
                  datum.set_bool(0 != value);
                }
              }
              break;
            }
            case file_table::ColumnKind::DATETIME: {
              // the iso "T" separator is normalized to a blank so that the
              // value is printed like an ordinary datetime
              file_value_buf_.assign(text);
              for (int64_t k = 0; k < static_cast<int64_t>(file_value_buf_.size()); ++k) {
                if ('T' == file_value_buf_[k]) {
                  file_value_buf_[k] = ' ';
                }
              }
              ret = set_string_datum(expr, file_value_buf_.c_str(),
                                     static_cast<uint32_t>(file_value_buf_.size()));
              break;
            }
            default: {
              ret = set_string_datum(expr, text.c_str(), static_cast<uint32_t>(text.size()));
              break;
            }
          }
        }
        if (OB_SUCC(ret)) {
          expr->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
