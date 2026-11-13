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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ob_resolver.h"
#include "lib/utility/ob_smart_call.h"
#include "lib/charset/ob_charset.h"
#include "lib/stat/ob_diagnostic_info_guard.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/resolver/cmd/ob_alter_system_resolver.h"
#include "sql/resolver/dml/ob_insert_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/resolver/dml/ob_delete_resolver.h"
#include "sql/resolver/tcl/ob_start_trans_resolver.h"
#include "sql/resolver/tcl/ob_end_trans_resolver.h"
#include "sql/resolver/prepare/ob_prepare_resolver.h"
#include "sql/resolver/prepare/ob_execute_resolver.h"
#include "sql/resolver/prepare/ob_deallocate_resolver.h"
#include "sql/resolver/ddl/ob_create_table_resolver.h"
#include "sql/resolver/ddl/ob_rename_table_resolver.h"
#include "sql/resolver/ddl/ob_truncate_table_resolver.h"
#include "sql/resolver/ddl/ob_fork_table_resolver.h"
#include "sql/resolver/ddl/ob_fork_database_resolver.h"
#include "sql/resolver/ddl/ob_create_table_like_resolver.h"
#include "sql/resolver/ddl/ob_alter_table_resolver.h"
#include "sql/resolver/ddl/ob_drop_table_resolver.h"
#include "sql/resolver/ddl/ob_create_index_resolver.h"
#include "sql/resolver/ddl/ob_drop_index_resolver.h"
#include "sql/resolver/ddl/ob_create_database_resolver.h"
#include "sql/resolver/ddl/ob_alter_database_resolver.h"
#include "sql/resolver/ddl/ob_use_database_resolver.h"
#include "sql/resolver/ddl/ob_drop_database_resolver.h"
#include "sql/resolver/ddl/ob_create_view_resolver.h"
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ddl/ob_create_outline_resolver.h"
#include "sql/resolver/ddl/ob_alter_outline_resolver.h"
#include "sql/resolver/ddl/ob_drop_outline_resolver.h"
#include "sql/resolver/ddl/ob_drop_routine_resolver.h"
#include "sql/resolver/ddl/ob_trigger_resolver.h"
#include "sql/resolver/ddl/ob_optimize_resolver.h"
#include "ddl/ob_drop_routine_resolver.h"
#include "ddl/ob_alter_routine_resolver.h"
#include "sql/resolver/ddl/ob_create_package_resolver.h"
#include "sql/resolver/ddl/ob_drop_package_resolver.h"
#include "sql/resolver/ddl/ob_recyclebin_restore_resolver.h"
#include "sql/resolver/ddl/ob_purge_resolver.h"
#include "sql/resolver/ddl/ob_analyze_stmt_resolver.h"
#include "sql/resolver/ddl/ob_purge_resolver.h"
#include "sql/resolver/ddl/ob_set_comment_resolver.h"
#include "sql/resolver/ddl/ob_lock_table_resolver.h"
#include "sql/resolver/dml/ob_insert_resolver.h"
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/resolver/dml/ob_delete_resolver.h"
#include "sql/resolver/dcl/ob_create_user_resolver.h"
#include "sql/resolver/dcl/ob_drop_user_resolver.h"
#include "sql/resolver/dcl/ob_rename_user_resolver.h"
#include "sql/resolver/dcl/ob_set_password_resolver.h"
#include "sql/resolver/dcl/ob_lock_user_resolver.h"
#include "sql/resolver/dcl/ob_grant_resolver.h"
#include "sql/resolver/dcl/ob_revoke_resolver.h"
#include "sql/resolver/dcl/ob_create_role_resolver.h"
#include "sql/resolver/dcl/ob_drop_role_resolver.h"
#include "sql/resolver/dcl/ob_alter_user_role_resolver.h"
#include "sql/resolver/tcl/ob_start_trans_resolver.h"
#include "sql/resolver/tcl/ob_end_trans_resolver.h"
#include "tcl/ob_savepoint_resolver.h"
#include "sql/resolver/cmd/ob_variable_set_resolver.h"
#include "sql/resolver/cmd/ob_show_resolver.h"
#include "sql/resolver/cmd/ob_diff_table_resolver.h"
#include "sql/resolver/cmd/ob_merge_table_resolver.h"
#include "sql/resolver/cmd/ob_kill_resolver.h"
#include "sql/resolver/cmd/ob_set_names_resolver.h"
#include "sql/resolver/cmd/ob_set_transaction_resolver.h"
#include "sql/resolver/cmd/ob_empty_query_resolver.h"
#include "sql/resolver/cmd/ob_anonymous_block_resolver.h"
#include "sql/resolver/cmd/ob_call_procedure_resolver.h"
#include "sql/resolver/cmd/ob_load_data_resolver.h"
#include "sql/resolver/prepare/ob_execute_resolver.h"
#include "sql/resolver/prepare/ob_deallocate_resolver.h"
#include "sql/resolver/ddl/ob_purge_resolver.h"
#include "sql/resolver/ddl/ob_set_comment_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_wrap_enum_set.h"
#include "sql/resolver/cmd/ob_get_diagnostics_resolver.h"
#include "sql/resolver/dcl/ob_alter_role_resolver.h"
#include "sql/pl/ob_pl_package.h"

namespace oceanbase
{
using namespace common;
using namespace share;
namespace sql
{
ObResolver::ObResolver(ObResolverParams &params)
    : params_(params)
{

}

ObResolver::~ObResolver()
{
}

template <typename ResolverType>
int ObResolver::stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  HEAP_VAR(ResolverType, stmt_resolver, params) {
    if (OB_FAIL(SMART_CALL(stmt_resolver.resolve(parse_tree)))) {
    }
    stmt = stmt_resolver.get_basic_stmt();
  }
  return ret;
}

template <typename SelectResolverType>
int ObResolver::select_stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  HEAP_VAR(SelectResolverType, stmt_resolver, params) {
    stmt_resolver.set_calc_found_rows(true);
    stmt_resolver.set_has_top_limit(true);
    if (OB_FAIL(SMART_CALL(stmt_resolver.resolve(parse_tree)))) {
    }
    stmt = stmt_resolver.get_basic_stmt();
  }
  return ret;
}

/* COPY (query) TO 'file'
 *
 * The query of the copy is resolved as a select statement, the select into
 * item of the statement carries the target path and the csv options.  The
 * select into operator of the executor writes the rows to the file.
 */
int ObResolver::copy_stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(3 != parse_tree.num_child_)
      || OB_ISNULL(parse_tree.children_[0])
      || OB_ISNULL(parse_tree.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (T_SELECT != parse_tree.children_[0]->type_) {
    /* the table form is rewritten into a select by the parser */
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(select_stmt_resolver_func<ObSelectResolver>(params,
                                                                *parse_tree.children_[0],
                                                                stmt))) {
  } else if (OB_ISNULL(stmt) || !stmt->is_select_stmt()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = resolve_copy_stmt(params, parse_tree, *static_cast<ObSelectStmt *>(stmt));
  }
  return ret;
}

int ObResolver::resolve_copy_stmt(ObResolverParams &params,
                                  const ParseNode &parse_tree,
                                  ObSelectStmt &select_stmt)
{
  int ret = OB_SUCCESS;
  const ParseNode *path_node = parse_tree.children_[1];
  const ParseNode *option_list = parse_tree.children_[2];
  ObIAllocator *allocator = params.allocator_;
  ObSelectIntoItem *into_item = NULL;
  ObString path;
  ObString nullstr;
  ObString delimiter;
  ObString quote;
  ObString escape;
  bool with_header = false;
  if (OB_ISNULL(allocator)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(path_node) || OB_ISNULL(path_node->str_value_) || path_node->str_len_ <= 0) {
    ret = OB_ERR_PARSE_SQL;
    LOG_USER_ERROR(OB_ERR_PARSE_SQL, "copy target path");
  } else {
    path = ObString(path_node->str_len_, path_node->str_value_);
  }
  /* the csv options of the copy */
  if (OB_SUCC(ret) && OB_NOT_NULL(option_list)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < option_list->num_child_; ++i) {
      const ParseNode *option = option_list->children_[i];
      ObString name;
      ObString value;
      if (OB_ISNULL(option) || T_COPY_OPTION != option->type_ || option->num_child_ < 1
          || OB_ISNULL(option->children_[0]) || OB_ISNULL(option->children_[0]->str_value_)) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      name = ObString(option->children_[0]->str_len_, option->children_[0]->str_value_);
      bool bool_value = true;
      bool has_bool_value = false;
      if (option->num_child_ >= 2 && OB_NOT_NULL(option->children_[1])) {
        const ParseNode *value_node = option->children_[1];
        if (T_BOOL == value_node->type_) {
          /* "header = true" and "header = false" */
          bool_value = (0 != value_node->value_);
          has_bool_value = true;
        } else if (OB_NOT_NULL(value_node->str_value_)) {
          value = ObString(value_node->str_len_, value_node->str_value_);
        }
      }
      if (0 == name.case_compare("format")) {
        if (0 != value.case_compare("csv")) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy format");
        }
      } else if (0 == name.case_compare("header")) {
        /* "header" alone means that the header is written, "header = false"
           asks for a file without a header */
        with_header = has_bool_value ? bool_value : true;
      } else if (0 == name.case_compare("nullstr")) {
        if (value.length() <= 0) {
          ret = OB_ERR_PARSE_SQL;
          LOG_USER_ERROR(OB_ERR_PARSE_SQL, "copy nullstr");
        } else {
          nullstr = value;
        }
      } else if (0 == name.case_compare("delimiter")) {
        if (value.length() <= 0) {
          ret = OB_ERR_PARSE_SQL;
          LOG_USER_ERROR(OB_ERR_PARSE_SQL, "copy delimiter");
        } else {
          delimiter = value;
        }
      } else if (0 == name.case_compare("quote")) {
        if (value.length() <= 0) {
          ret = OB_ERR_PARSE_SQL;
          LOG_USER_ERROR(OB_ERR_PARSE_SQL, "copy quote");
        } else {
          quote = value;
        }
      } else if (0 == name.case_compare("escape")) {
        if (value.length() <= 0) {
          ret = OB_ERR_PARSE_SQL;
          LOG_USER_ERROR(OB_ERR_PARSE_SQL, "copy escape");
        } else {
          escape = value;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy option");
      }
    }
  }
  if (OB_SUCC(ret)) {
    into_item = static_cast<ObSelectIntoItem *>(allocator->alloc(sizeof(ObSelectIntoItem)));
    if (OB_ISNULL(into_item)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      new(into_item) ObSelectIntoItem();
      into_item->into_type_ = T_INTO_OUTFILE;
      into_item->is_copy_ = true;
      into_item->with_header_ = with_header;
      /* a copy always writes a single file */
      into_item->is_single_ = true;
      into_item->outfile_name_.set_varchar(path);
      into_item->outfile_name_.set_collation_type(ObCharset::get_system_collation());
      if (nullstr.length() > 0
          && OB_FAIL(ob_write_string(*allocator, nullstr, into_item->nullstr_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (delimiter.length() > 0
                 && OB_FAIL(ob_write_string(*allocator, delimiter, into_item->delimiter_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (quote.length() > 0
                 && OB_FAIL(ob_write_string(*allocator, quote, into_item->quote_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (escape.length() > 0
                 && OB_FAIL(ob_write_string(*allocator, escape, into_item->escape_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      /* the header of the file holds the names of the output columns */
      for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt.get_select_item_size(); ++i) {
        ObString name;
        if (OB_FAIL(ob_write_string(*allocator, select_stmt.get_select_item(i).alias_name_, name))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(into_item->header_names_.push_back(name))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        }
      }
      if (OB_SUCC(ret)) {
        select_stmt.set_select_into(into_item);
      }
    }
  }
  return ret;
}

/* An AI column that a statement reads. */
struct ObAiColumnOfStmt
{
  uint64_t table_id_;
  uint64_t column_id_;
  common::ObString table_name_;
  common::ObString database_name_;
  common::ObString column_name_;
};

/* The name of the database and the name of the table of a table of the
   database.  The names are the names of the schema, the name of the database
   of a table item is empty when the statement does not qualify the name of
   the table. */
static int get_ai_column_table_names(ObSchemaChecker *schema_checker,
                                      const uint64_t table_id,
                                      common::ObString &database_name,
                                      common::ObString &table_name)
{
  int ret = OB_SUCCESS;
  const share::schema::ObTableSchema *table_schema = NULL;
  const share::schema::ObDatabaseSchema *database_schema = NULL;
  if (OB_FAIL(schema_checker->get_table_schema(table_id, table_schema))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_checker->get_database_schema(table_schema->get_database_id(),
                                                         database_schema))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(database_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    database_name = database_schema->get_database_name_str();
    table_name = table_schema->get_table_name();
  }
  return ret;
}

/* The AI columns that the statement and its subqueries read.  The columns of
   a table that is not a table of the statement belong to another statement,
   they are not collected here. */
static int collect_ai_columns_of_stmt(ObDMLStmt *stmt,
                                       ObSchemaChecker *schema_checker,
                                       common::ObSEArray<ObAiColumnOfStmt, 8> &ai_columns)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<ColumnItem> &column_items = stmt->get_column_items();
  /* the columns that the statement writes are not read by the statement, the
     strict read does not check them */
  common::ObSEArray<uint64_t, 8> written_column_ids;
  common::ObSEArray<uint64_t, 8> written_table_ids;
  if (stmt->is_update_stmt()) {
    ObUpdateStmt *update_stmt = static_cast<ObUpdateStmt *>(stmt);
    common::ObIArray<ObUpdateTableInfo*> &table_infos = update_stmt->get_update_table_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < table_infos.count(); ++i) {
      ObUpdateTableInfo *table_info = table_infos.at(i);
      if (OB_ISNULL(table_info)) {
        continue;
      }
      ObAssignments &assignments = table_info->assignments_;
      for (int64_t j = 0; OB_SUCC(ret) && j < assignments.count(); ++j) {
        ObAssignment &assignment = assignments.at(j);
        if (!OB_ISNULL(assignment.column_expr_)) {
          if (OB_FAIL(written_column_ids.push_back(assignment.column_expr_->get_column_id()))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(written_table_ids.push_back(assignment.column_expr_->get_table_id()))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          }
        }
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); ++i) {
    const ColumnItem &column_item = column_items.at(i);
    bool is_written = false;
    for (int64_t j = 0; !is_written && j < written_column_ids.count(); ++j) {
      if (written_column_ids.at(j) == column_item.column_id_
          && written_table_ids.at(j) == column_item.table_id_) {
        is_written = true;
      }
    }
    if (is_written) {
      continue;
    }
    /* the id of the table of a column item is the id of the table of the
       statement, the id of the schema of the table is the id of the base
       table item */
    const TableItem *table_item = stmt->get_table_item_by_id(column_item.table_id_);
    if (OB_ISNULL(table_item) || !table_item->is_basic_table()) {
      /* the column belongs to a table of another statement or to a table that
         has no schema of its own */
      continue;
    }
    const uint64_t table_id = table_item->ref_id_;
    const share::schema::ObColumnSchemaV2 *column_schema = NULL;
    if (OB_FAIL(schema_checker->get_column_schema(table_id,
                                                   column_item.column_id_,
                                                   column_schema))) {
      /* the column is not a column of a table of the database, the strict read
         does not know anything about it */
      ret = OB_SUCCESS;
    } else if (!OB_ISNULL(column_schema)
               && ObAiColumnDef::is_ai_column_comment(column_schema->get_comment_str())) {
      ObAiColumnOfStmt ai_column;
      ai_column.table_id_ = table_id;
      ai_column.column_id_ = column_item.column_id_;
      ai_column.column_name_ = column_schema->get_column_name_str();
      if (OB_FAIL(get_ai_column_table_names(schema_checker, table_id,
                                            ai_column.database_name_,
                                            ai_column.table_name_))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ai_columns.push_back(ai_column))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  common::ObArray<ObSelectStmt*> child_stmts;
  if (OB_SUCC(ret) && OB_FAIL(stmt->get_child_stmts(child_stmts))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
      if (OB_FAIL(collect_ai_columns_of_stmt(child_stmts.at(i), schema_checker, ai_columns))) {
      }
    }
  }
  return ret;
}

/* Whether every row of the table has the result of the AI column.  The check
   is done for the whole column, the rows that the statement selects do not
   matter. */
static int check_ai_column_is_complete(common::ObMySQLProxy *sql_proxy,
                                       const ObAiColumnOfStmt &ai_column,
                                       bool &is_complete)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  common::ObMySQLTransaction trans;
  is_complete = true;
  if (OB_FAIL(sql.assign_fmt("SELECT 1 FROM `%.*s`.`%.*s` WHERE `%.*s` IS NULL LIMIT 1",
                              static_cast<int>(ai_column.database_name_.length()),
                              ai_column.database_name_.ptr(),
                              static_cast<int>(ai_column.table_name_.length()),
                              ai_column.table_name_.ptr(),
                              static_cast<int>(ai_column.column_name_.length()),
                              ai_column.column_name_.ptr()))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(trans.start(sql_proxy))) {
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(trans.read(res, sql.ptr()))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          is_complete = true;
          ret = OB_SUCCESS;
        }
      } else {
        is_complete = false;
      }
      res.reset();
    }
    if (trans.is_started()) {
      const int end_ret = trans.end(OB_SUCC(ret));
      if (OB_SUCC(ret) && OB_SUCCESS != end_ret) {
        ret = end_ret;
      }
    }
  }
  return ret;
}

/* A statement that reads an AI column whose result is not complete fails when
   the strict read is on.  The session of the check is another one, the strict
   read is off in it, the check does not call itself. */
static int check_ai_column_strict_read(ObResolverParams &params, ObStmt *stmt)
{
  int ret = OB_SUCCESS;
  const stmt::StmtType stmt_type = stmt->get_stmt_type();
  if (stmt::T_SELECT != stmt_type
      && stmt::T_UPDATE != stmt_type
      && stmt::T_DELETE != stmt_type) {
    return OB_SUCCESS;
  }
  ObSQLSessionInfo *session = params.session_info_;
  common::ObObj value;
  bool strict_read = false;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, value))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    strict_read = (0 != value.get_int());
  }
  if (OB_SUCC(ret) && strict_read) {
    common::ObSEArray<ObAiColumnOfStmt, 8> ai_columns;
    if (OB_FAIL(collect_ai_columns_of_stmt(static_cast<ObDMLStmt*>(stmt),
                                            params.schema_checker_,
                                            ai_columns))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
        const ObAiColumnOfStmt &ai_column = ai_columns.at(i);
        bool is_complete = true;
        if (OB_FAIL(check_ai_column_is_complete(params.sql_proxy_, ai_column, is_complete))) {
          ret = OB_ERR_UNEXPECTED;
        } else if (!is_complete) {
          LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED_TASKS,
                         static_cast<uint32_t>(ai_column.table_id_),
                         static_cast<uint32_t>(ai_column.column_id_));
          ret = OB_AI_COLUMN_UNFINISHED_TASKS;
        }
      }
    }
  }
  return ret;
}


/* ---------------------------------------------------------------------------
 * The retry of the result of an AI column
 *
 * "alter table t retry ai column c [where expr]" asks the worker of the AI
 * column to compute the result of the column again.  The statement is an
 * update of the AI column, the result of the column is set to null and the
 * worker computes it again.  The status of the column is a pseudo column of
 * the retry, it is the state of the hidden column of the AI column, the state
 * tells whether the result of the column is ready or that the computation of
 * the result failed.
 * ------------------------------------------------------------------------- */

/* A parse node that the resolver builds.  The nodes live as long as the
   statement, the allocator of the resolver is the allocator of the result of
   the statement. */
static ParseNode *new_retry_parse_node(common::ObIAllocator &allocator,
                                       const ObItemType type,
                                       const int32_t num_child)
{
  ParseNode *node = static_cast<ParseNode *>(allocator.alloc(sizeof(ParseNode)));
  if (OB_ISNULL(node)) {
    return NULL;
  }
  MEMSET(node, 0, sizeof(ParseNode));
  node->type_ = type;
  node->num_child_ = num_child;
  if (num_child > 0) {
    ParseNode **children = static_cast<ParseNode **>(allocator.alloc(num_child * sizeof(ParseNode *)));
    if (OB_ISNULL(children)) {
      return NULL;
    }
    for (int32_t i = 0; i < num_child; ++i) {
      children[i] = NULL;
    }
    node->children_ = children;
  }
  return node;
}

/* A copy of a string in the allocator of the resolver. */
static const char *dup_retry_string(common::ObIAllocator &allocator,
                                    const common::ObString &str)
{
  char *buf = static_cast<char *>(allocator.alloc(str.length() + 1));
  if (OB_ISNULL(buf)) {
    return NULL;
  }
  MEMCPY(buf, str.ptr(), str.length());
  buf[str.length()] = '\0';
  return buf;
}

/* The pseudo column of the status is the state of the AI column, the state is
   kept in a hidden column of the AI column.  The name of the pseudo column is
   not quoted, the name of the physical column of the table is quoted. */
static int replace_retry_status_column(ParseNode *node,
                                        common::ObIAllocator &allocator,
                                        const common::ObString &state_column_name)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node) || OB_ISNULL(node->children_)) {
    return OB_SUCCESS;
  }
  if (T_COLUMN_REF == node->type_ && 3 == node->num_child_) {
    ParseNode *col_name = node->children_[2];
    if (!OB_ISNULL(col_name) && !OB_ISNULL(col_name->str_value_) && !col_name->is_input_quoted_
        && 6 == col_name->str_len_ && 0 == strncasecmp(col_name->str_value_, "status", 6)) {
      ParseNode *new_name = new_retry_parse_node(allocator, col_name->type_, 0);
      const char *name = dup_retry_string(allocator, state_column_name);
      const char *ref_name = dup_retry_string(allocator, state_column_name);
      if (OB_ISNULL(new_name) || OB_ISNULL(name) || OB_ISNULL(ref_name)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        new_name->str_value_ = name;
        new_name->str_len_ = state_column_name.length();
        node->str_value_ = ref_name;
        node->str_len_ = state_column_name.length();
        node->children_[2] = new_name;
      }
    }
  }
  for (int32_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
    if (OB_FAIL(replace_retry_status_column(node->children_[i], allocator, state_column_name))) {
    }
  }
  return ret;
}

/* The parse tree of the update that the retry is.  The result of the AI column
   is set to null, the rows that the predicate of the retry selects are the
   rows that the worker of the AI column computes again. */
static ParseNode *build_retry_update_node(common::ObIAllocator &allocator,
                                           const ParseNode *alter_node,
                                           ParseNode *table_factor,
                                           ParseNode *column_name_node,
                                           ParseNode *where_node)
{
  ParseNode *update_node = new_retry_parse_node(allocator, T_UPDATE, 11);
  ParseNode *table_refs = new_retry_parse_node(allocator, T_TABLE_REFERENCES, 1);
  ParseNode *assign_list = new_retry_parse_node(allocator, T_ASSIGN_LIST, 1);
  ParseNode *assign_item = new_retry_parse_node(allocator, T_ASSIGN_ITEM, 2);
  ParseNode *col_ref = new_retry_parse_node(allocator, T_COLUMN_REF, 3);
  ParseNode *null_expr = new_retry_parse_node(allocator, T_NULL, 0);
  const common::ObString column_name(column_name_node->str_len_, column_name_node->str_value_);
  const char *name = dup_retry_string(allocator, column_name);
  if (OB_ISNULL(update_node) || OB_ISNULL(table_refs) || OB_ISNULL(assign_list)
      || OB_ISNULL(assign_item) || OB_ISNULL(col_ref) || OB_ISNULL(null_expr)
      || OB_ISNULL(name)) {
    return NULL;
  }
  col_ref->str_value_ = name;
  col_ref->str_len_ = column_name.length();
  col_ref->children_[2] = column_name_node;
  assign_item->children_[0] = col_ref;
  assign_item->children_[1] = null_expr;
  assign_list->children_[0] = assign_item;
  table_refs->children_[0] = table_factor;
  update_node->children_[ObUpdateResolver::TABLE] = table_refs;
  update_node->children_[ObUpdateResolver::UPDATE_LIST] = assign_list;
  update_node->children_[ObUpdateResolver::WHERE] = where_node;
  update_node->stmt_loc_ = alter_node->stmt_loc_;
  return update_node;
}


int ObResolver::resolve_ai_column_retry(const ParseNode &parse_tree,
                                         ObStmt *&stmt,
                                         bool &is_retry)
{
  int ret = OB_SUCCESS;
  is_retry = false;
  ParseNode *action_list = (parse_tree.num_child_ > 1) ? parse_tree.children_[1] : NULL;
  if (OB_ISNULL(action_list) || OB_ISNULL(action_list->children_)) {
    return OB_SUCCESS;
  }
  ParseNode *retry_node = NULL;
  int64_t action_count = 0;
  for (int32_t i = 0; i < action_list->num_child_; ++i) {
    ParseNode *action = action_list->children_[i];
    if (OB_ISNULL(action)) {
      continue;
    }
    ++action_count;
    if (T_ALTER_TABLE_RETRY_AI_COLUMN == action->type_) {
      retry_node = action;
    }
  }
  if (OB_ISNULL(retry_node)) {
    /* the statement does not retry an AI column */
    return OB_SUCCESS;
  }
  is_retry = true;
  /* the retry of an AI column is an update of the AI column */
  params_.resolver_scope_stmt_type_ = T_UPDATE;
  params_.session_info_->set_stmt_type(ObResolverUtils::get_stmt_type_by_item_type(T_UPDATE));
  if (action_count > 1) {
    /* the retry of an AI column is the only action of the alter table */
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
    ret = OB_NOT_SUPPORTED;
  } else {
    ret = resolve_ai_column_retry_update(parse_tree, *retry_node, stmt);
  }
  return ret;
}

int ObResolver::resolve_ai_column_retry_update(const ParseNode &parse_tree,
                                                const ParseNode &retry_node,
                                                ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  common::ObIAllocator &allocator = *params_.allocator_;
  ParseNode *column_name_node = retry_node.children_[0];
  ParseNode *where_node = retry_node.children_[1];
  ParseNode *table_factor = (parse_tree.num_child_ > 0) ? parse_tree.children_[0] : NULL;
  ParseNode *update_node = NULL;
  common::ObString state_column_name;
  if (OB_ISNULL(column_name_node) || OB_ISNULL(column_name_node->str_value_) || OB_ISNULL(table_factor)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAiColumnDef::build_hidden_column_name(allocator,
                                                             ObAiColumnDef::AI_STATE_PREFIX,
                                                             common::ObString(column_name_node->str_len_,
                                                                              column_name_node->str_value_),
                                                             state_column_name))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(replace_retry_status_column(where_node, allocator, state_column_name))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(update_node = build_retry_update_node(allocator,
                                                             &parse_tree,
                                                             table_factor,
                                                             column_name_node,
                                                             where_node))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(stmt_resolver_func<ObUpdateResolver>(params_, *update_node, stmt))) {
  } else {
    ret = check_ai_column_retry(stmt, column_name_node);
  }
  return ret;
}

int ObResolver::check_ai_column_retry(ObStmt *stmt, const ParseNode *column_name_node)
{
  int ret = OB_SUCCESS;
  ObUpdateStmt *update_stmt = NULL;
  const ObTableSchema *table_schema = NULL;
  const ObColumnSchemaV2 *column_schema = NULL;
  bool is_ai_column = false;
  const common::ObString column_name(column_name_node->str_len_, column_name_node->str_value_);
  if (OB_ISNULL(stmt) || OB_ISNULL(update_stmt = static_cast<ObUpdateStmt *>(stmt))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const TableItem *table_item = NULL;
    common::ObIArray<TableItem*> &table_items = update_stmt->get_table_items();
    if (table_items.count() > 0) {
      table_item = table_items.at(0);
    }
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(params_.schema_checker_->get_table_schema(table_item->ref_id_, table_schema))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      column_schema = table_schema->get_column_schema(column_name);
      is_ai_column = (!OB_ISNULL(column_schema)
                      && ObAiColumnDef::is_ai_column_comment(column_schema->get_comment_str()));
    }
  }
  if (OB_SUCC(ret) && !is_ai_column) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
    ret = OB_NOT_SUPPORTED;
  } else if (OB_SUCC(ret)) {
    /* the assignment of the AI column is an implicit one, the worker of the AI
       column computes the result of the column again */
    common::ObIArray<ObUpdateTableInfo*> &table_infos = update_stmt->get_update_table_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < table_infos.count(); ++i) {
      ObUpdateTableInfo *table_info = table_infos.at(i);
      if (OB_ISNULL(table_info)) {
        continue;
      }
      ObAssignments &assignments = table_info->assignments_;
      for (int64_t j = 0; OB_SUCC(ret) && j < assignments.count(); ++j) {
        ObAssignment &assignment = assignments.at(j);
        if (!OB_ISNULL(assignment.column_expr_)
            && assignment.column_expr_->get_column_id() == column_schema->get_column_id()) {
          assignment.is_implicit_ = true;
        }
      }
    }
  }
  return ret;
}

int ObResolver::resolve(IsPrepared if_prepared, const ParseNode &parse_tree, ObStmt *&stmt)
{
#define REGISTER_STMT_RESOLVER(name)                                   \
  do {                                                                 \
    ret = stmt_resolver_func<Ob##name##Resolver>(params_, *real_parse_tree, stmt); \
  } while (0)

#define REGISTER_SELECT_STMT_RESOLVER(name)                                   \
  do {                                                                        \
    ret = select_stmt_resolver_func<Ob##name##Resolver>(params_, parse_tree, stmt); \
  } while (0)
  ACTIVE_SESSION_FLAG_SETTER_GUARD(in_resolve);
  int ret = OB_SUCCESS;
  const ParseNode *real_parse_tree = NULL;
  int64_t questionmark_count = 0;
  UNUSED(if_prepared);
  if (OB_ISNULL(params_.allocator_)
      || OB_ISNULL(params_.schema_checker_)
      || OB_ISNULL(params_.session_info_)
      || OB_ISNULL(params_.expr_factory_)
      || OB_ISNULL(params_.query_ctx_)) {
    ret = OB_NOT_INIT;
  } else {
    real_parse_tree = &parse_tree;
  }

  if (OB_SUCC(ret)) {
    /* "DESCRIBE SELECT ..." over a file table is answered by a select, the
       statement is scoped as a plain select.  "COPY (query) TO 'file'" is a
       select as well, the copy only adds the target of the result. */
    const ObItemType scope_stmt_type = (T_DESCRIBE_SELECT == parse_tree.type_
                                        || T_COPY_STMT == parse_tree.type_)
                                       ? T_SELECT : parse_tree.type_;
    params_.resolver_scope_stmt_type_ = scope_stmt_type;
    params_.session_info_->set_stmt_type(
      ObResolverUtils::get_stmt_type_by_item_type(scope_stmt_type));
    ObExecContext *exec_ctx = params_.session_info_->get_cur_exec_ctx();
    if (exec_ctx != nullptr && exec_ctx->get_sql_ctx() != nullptr) {
      //save the current stmt type on my context,
      //some other modules may need to get the current SQL stmt type through its context
      ObSqlCtx *sql_ctx = exec_ctx->get_sql_ctx();
      sql_ctx->stmt_type_ = ObResolverUtils::get_stmt_type_by_item_type(scope_stmt_type);
    }

    switch (real_parse_tree->type_) {
      case T_CREATE_TABLE: {
        REGISTER_STMT_RESOLVER(CreateTable);
        break;
      }
      case T_ALTER_TABLE: {
        bool is_ai_column_retry = false;
        if (OB_FAIL(resolve_ai_column_retry(*real_parse_tree, stmt, is_ai_column_retry))) {
        } else if (!is_ai_column_retry) {
          REGISTER_STMT_RESOLVER(AlterTable);
        }
        break;
      }
      case T_CREATE_INDEX: {
        REGISTER_STMT_RESOLVER(CreateIndex);
        break;
      }
      case T_CREATE_VIEW: {
        REGISTER_STMT_RESOLVER(CreateView);
        break;
      }
      case T_ALTER_VIEW: {
#if 0
        ObAlterViewResolver stmt_resolver(params_);
        if (OB_FAIL(stmt_resolver.resolve(*node))) {
        }

        stmt = stmt_resolver.get_basic_stmt();
#endif
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("not support to alter view");
	      LOG_USER_ERROR(OB_NOT_SUPPORTED, "alter view");
        break;
      }
      case T_DROP_VIEW:
      case T_DROP_TABLE: {
        REGISTER_STMT_RESOLVER(DropTable);
        break;
      }
      case T_DROP_INDEX: {
        REGISTER_STMT_RESOLVER(DropIndex);
        break;
      }
      case T_CREATE_TABLE_LIKE: {
        REGISTER_STMT_RESOLVER(CreateTableLike);
        break;
      }
      case T_FORK_TABLE: {
        REGISTER_STMT_RESOLVER(ForkTable);
        break;
      }
      case T_FORK_DATABASE: {
        REGISTER_STMT_RESOLVER(ForkDatabase);
        break;
      }
      case T_DIFF_TABLE: {
        REGISTER_STMT_RESOLVER(DiffTable);
        break;
      }
      case T_MERGE_TABLE: {
        REGISTER_STMT_RESOLVER(MergeTable);
        break;
      }
      case T_SELECT: {
        REGISTER_SELECT_STMT_RESOLVER(Select);
        break;
      }
      case T_COPY_STMT: {
        /* COPY (query) TO 'file', the query is resolved as a select statement */
        ret = copy_stmt_resolver_func(params_, *real_parse_tree, stmt);
        break;
      }
      case T_DESCRIBE_SELECT: {
        /* DESCRIBE SELECT ... over a file table, the file table exposes the
           six description columns and the statement behaves like a select */
        if (OB_UNLIKELY(1 != real_parse_tree->num_child_
                        || OB_ISNULL(real_parse_tree->children_)
                        || OB_ISNULL(real_parse_tree->children_[0]))) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          params_.resolver_scope_stmt_type_ = T_SELECT;
          params_.is_file_describe_ = true;
          ret = select_stmt_resolver_func<ObSelectResolver>(params_, *real_parse_tree->children_[0], stmt);
          params_.is_file_describe_ = false;
        }
        break;
      }
      case T_INSERT: {
        REGISTER_STMT_RESOLVER(Insert);
        break;
      }
      case T_UPDATE: {
        REGISTER_STMT_RESOLVER(Update);
        break;
      }
      case T_DELETE: {
        REGISTER_STMT_RESOLVER(Delete);
        break;
      }
      case T_BEGIN: {
        REGISTER_STMT_RESOLVER(StartTrans);
        break;
      }
      case T_ROLLBACK:
        //fall through
      case T_COMMIT: {
        REGISTER_STMT_RESOLVER(EndTrans);
        break;
      }
      case T_FREEZE: {
        REGISTER_STMT_RESOLVER(Freeze);
        break;
      }
      case T_FLUSH_CACHE: {
        REGISTER_STMT_RESOLVER(FlushCache);
        break;
      }
      case T_FLUSH_KVCACHE: {
        REGISTER_STMT_RESOLVER(FlushKVCache);
        break;
      }
      case T_FLUSH_ILOGCACHE: {
        REGISTER_STMT_RESOLVER(FlushIlogCache);
        break;
      }
      case T_FLUSH_DAG_WARNINGS: {
        REGISTER_STMT_RESOLVER(FlushDagWarnings);
        break;
      }
      case T_MERGE_CONTROL: {
        REGISTER_STMT_RESOLVER(AdminMerge);
        break;
      }
      case T_CANCEL_TASK: {
        REGISTER_STMT_RESOLVER(CancelTask);
        break;
      }
      case T_REFRESH_MEMORY_STAT: {
        REGISTER_STMT_RESOLVER(RefreshMemStat);
        break;
      }
      case T_REFRESH_IO_CALIBRATION: {
        REGISTER_STMT_RESOLVER(RefreshIOCalibration);
        break;
      }
      case T_SWITCHOVER_TO_STANDBY:
      case T_SWITCHOVER_TO_PRIMARY:
      case T_ACTIVATE_STANDBY: {
        REGISTER_STMT_RESOLVER(SwitchRole);
        break;
      }
      case T_ALTER_SYSTEM_SET_PARAMETER: {
        REGISTER_STMT_RESOLVER(SetConfig);
        break;
      }
      case T_ALTER_SYSTEM_SETTP: {
        REGISTER_STMT_RESOLVER(SetTP);
        break;
      }
      case T_CLEAR_MERGE_ERROR: {
        REGISTER_STMT_RESOLVER(ClearMergeError);
        break;
      }
      case T_CREATE_DATABASE: {
        REGISTER_STMT_RESOLVER(CreateDatabase);
        break;
      }
      case T_USE_DATABASE: {
        REGISTER_STMT_RESOLVER(UseDatabase);
        break;
      }
      case T_ALTER_DATABASE: {
        REGISTER_STMT_RESOLVER(AlterDatabase);
        break;
      }
      case T_DROP_DATABASE: {
        REGISTER_STMT_RESOLVER(DropDatabase);
        break;
      }
      case T_RENAME_TABLE: {
        REGISTER_STMT_RESOLVER(RenameTable);
        break;
      }
      case T_TRUNCATE_TABLE: {
        REGISTER_STMT_RESOLVER(TruncateTable);
        break;
      }
      case T_RECYCLEBIN_RESTORE_TABLE: {
        REGISTER_STMT_RESOLVER(RecyclebinRestoreTable);
        break;
      }
      case T_RECYCLEBIN_RESTORE_DATABASE: {
        REGISTER_STMT_RESOLVER(RecyclebinRestoreDatabase);
        break;
      }
      case T_PURGE_TABLE: {
        REGISTER_STMT_RESOLVER(PurgeTable);
        break;
      }
      case T_PURGE_INDEX: {
        REGISTER_STMT_RESOLVER(PurgeIndex);
        break;
      }
      case T_PURGE_DATABASE: {
        REGISTER_STMT_RESOLVER(PurgeDatabase);
        break;
      }
      case T_PURGE_RECYCLEBIN: {
        REGISTER_STMT_RESOLVER(PurgeRecycleBin);
        break;
      }
      case T_OPTIMIZE_TABLE: {
        REGISTER_STMT_RESOLVER(OptimizeTable);
        break;
      }
      case T_PREPARE: {
        if (params_.is_prepare_protocol_) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          REGISTER_STMT_RESOLVER(Prepare);
        }
        break;
      }
      case T_EXECUTE: {
        if (params_.is_prepare_protocol_) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          REGISTER_STMT_RESOLVER(Execute);
        }
        break;
      }
      case T_DEALLOCATE: {
        if (params_.is_prepare_protocol_) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          REGISTER_STMT_RESOLVER(Deallocate);
        }
        break;
      }
      case T_VARIABLE_SET: {
        REGISTER_STMT_RESOLVER(VariableSet);
        break;
      }
      case T_TRANSACTION: {
        REGISTER_STMT_RESOLVER(SetTransaction);
        break;
      }
      case T_SHOW_TABLES:
      case T_SHOW_DATABASES:
      case T_SHOW_VARIABLES:
      case T_SHOW_COLUMNS:
      case T_SHOW_SCHEMA:
      case T_SHOW_CREATE_DATABASE:
      case T_SHOW_CREATE_TABLE:
      case T_SHOW_CREATE_VIEW:
      case T_SHOW_CREATE_PROCEDURE:
      case T_SHOW_CREATE_FUNCTION:
      case T_SHOW_TABLE_STATUS:
      case T_SHOW_PARAMETERS:
      case T_SHOW_INDEXES:
      case T_SHOW_PROCESSLIST:
      case T_SHOW_WARNINGS:
      case T_SHOW_ERRORS:
      case T_SHOW_TRACE:
      case T_SHOW_ENGINES:
      case T_SHOW_PROFILE:
      case T_SHOW_ENGINE:
      case T_SHOW_OPEN_TABLES:
      case T_SHOW_PRIVILEGES:
      case T_SHOW_CHARSET:
      case T_SHOW_COLLATION:
      case T_SHOW_GRANTS:
      case T_SHOW_RECYCLEBIN:
      case T_SHOW_PROCEDURE_STATUS:
      case T_SHOW_FUNCTION_STATUS:
      case T_SHOW_TRIGGERS:
      case T_SHOW_STATUS:
      case T_SHOW_CREATE_TRIGGER:
      case T_SHOW_CHECK_TABLE:
      case T_SHOW_CREATE_USER: {
        REGISTER_STMT_RESOLVER(Show);
        break;
      }
      case T_CREATE_USER: {
        REGISTER_STMT_RESOLVER(CreateUser);
        break;
      }
      case T_DROP_USER: {
        REGISTER_STMT_RESOLVER(DropUser);
        break;
      }
      case T_RENAME_USER: {
        REGISTER_STMT_RESOLVER(RenameUser);
        break;
      }
      case T_SET_PASSWORD: {
        REGISTER_STMT_RESOLVER(SetPassword);
        break;
      }
      case T_LOCK_USER: {
        REGISTER_STMT_RESOLVER(LockUser);
        break;
      }
      case T_ALTER_USER_DEFAULT_ROLE:
      case T_SET_ROLE: {
        REGISTER_STMT_RESOLVER(AlterUserRole);
        break;
      }

      case T_GRANT_ROLE:
      case T_SYSTEM_GRANT:
      case T_GRANT: {
        REGISTER_STMT_RESOLVER(Grant);
        break;
      }
      case T_SYSTEM_REVOKE:
      case T_REVOKE:
      case T_REVOKE_ALL: {
        REGISTER_STMT_RESOLVER(Revoke);
        break;
      }
      case T_EXPLAIN: {
        REGISTER_STMT_RESOLVER(Explain);
        break;
      }
      case T_ALTER_SYSTEM_SET: {
        REGISTER_STMT_RESOLVER(AlterSystemSet);
        break;
      }
      case T_ALTER_SYSTEM_KILL: {
        REGISTER_STMT_RESOLVER(Kill);
        break;
      }
      case T_ALTER_SESSION_SET: {
        REGISTER_STMT_RESOLVER(AlterSessionSet);
        break;
      }
      case T_SET_NAMES:
        // fall through
      case T_SET_CHARSET: {
        REGISTER_STMT_RESOLVER(SetNames);
        break;
      }
      case T_KILL: {
        REGISTER_STMT_RESOLVER(Kill);
        break;
      }
      case T_EMPTY_QUERY: {
        REGISTER_STMT_RESOLVER(EmptyQuery);
        break;
      }
      case T_FLUSH_PRIVILEGES: {
        REGISTER_STMT_RESOLVER(EmptyQuery);
        break;
      }
      case T_LOCK_TABLE: {
        REGISTER_STMT_RESOLVER(LockTable);
        break;
      }
      case T_CREATE_OUTLINE: {
        REGISTER_STMT_RESOLVER(CreateOutline);
        break;
      }
      case T_ALTER_OUTLINE: {
        REGISTER_STMT_RESOLVER(AlterOutline);
        break;
      }
      case T_DROP_OUTLINE: {
        REGISTER_STMT_RESOLVER(DropOutline);
        break;
      }
      case T_SP_CREATE: {
        REGISTER_STMT_RESOLVER(CreateProcedure);
        break;
      }
      case T_SP_ALTER: {
        REGISTER_STMT_RESOLVER(AlterProcedure);
        break;
      }
      case T_SP_CALL_STMT: {
        REGISTER_STMT_RESOLVER(CallProcedure);
        break;
      }
      case T_SF_CREATE: {
        REGISTER_STMT_RESOLVER(CreateFunction);
        break;
      }
      case T_SF_ALTER: {
        REGISTER_STMT_RESOLVER(AlterFunction);
        break;
      }
      case T_SP_DROP: {
        REGISTER_STMT_RESOLVER(DropProcedure);
        break;
      }
      case T_SF_DROP: {
        REGISTER_STMT_RESOLVER(DropFunction);
        break;
      }
      case T_SP_ANONYMOUS_BLOCK: {
        REGISTER_STMT_RESOLVER(AnonymousBlock);
        break;
      }
      case T_PACKAGE_CREATE: {
        REGISTER_STMT_RESOLVER(CreatePackage);
        break;
      }
      case T_PACKAGE_CREATE_BODY: {
        REGISTER_STMT_RESOLVER(CreatePackageBody);
        break;
      }
      case T_PACKAGE_DROP: {
        REGISTER_STMT_RESOLVER(DropPackage);
        break;
      }
      case T_MYSQL_UPDATE_HISTOGRAM:
      case T_MYSQL_DROP_HISTOGRAM:
      case T_MYSQL_ANALYZE: {
        REGISTER_STMT_RESOLVER(AnalyzeStmt);
        break;
      }
      case T_LOAD_DATA_URL:
      case T_LOAD_DATA: {
        REGISTER_STMT_RESOLVER(LoadData);
        break;
      }
      case T_SET_TABLE_COMMENT:
      case T_SET_COLUMN_COMMENT: {
        REGISTER_STMT_RESOLVER(SetComment);
        break;
      }
      case T_CREATE_ROLE: {
        REGISTER_STMT_RESOLVER(CreateRole);
        break;
      }
      case T_DROP_ROLE: {
        REGISTER_STMT_RESOLVER(DropRole);
        break;
      }
      case T_ALTER_ROLE: {
        REGISTER_STMT_RESOLVER(AlterRole);
        break;
      }
      case T_CREATE_SAVEPOINT:
      case T_ROLLBACK_SAVEPOINT:
      case T_RELEASE_SAVEPOINT: {
        REGISTER_STMT_RESOLVER(SavePoint);
        break;
      }
      case T_TG_CREATE: {
        REGISTER_STMT_RESOLVER(Trigger);
        break;
      }
      case T_TG_DROP: {
        REGISTER_STMT_RESOLVER(Trigger);
        break;
      }
      case T_TG_ALTER: {
        REGISTER_STMT_RESOLVER(Trigger);
        break;
      }
      case T_DIAGNOSTICS: {
        REGISTER_STMT_RESOLVER(GetDiagnostics);
        break;
      }
      case T_ALTER_SYSTEM_RESET_PARAMETER: {
        REGISTER_STMT_RESOLVER(ResetConfig);
        break;
      }
      case T_ALTER_SYSTEM_RESET: {
        REGISTER_STMT_RESOLVER(AlterSystemReset);
        break;
      }
      default: {
        ret = OB_NOT_SUPPORTED;
        const char *type_name = get_type_name(parse_tree.type_);
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "statement type");
        break;
      }
    }  // end switch
    if (OB_SUCC(ret) && stmt->is_dml_write_stmt()) {
      // todo yanli:check leader-follower database
    }
    
    if (OB_SUCC(ret) && stmt->is_dml_stmt() && !params_.session_info_->is_varparams_sql_prepare()) {
      ObDMLStmt *dml_stmt = static_cast<ObDMLStmt*>(stmt);
      ObRawExprWrapEnumSet enum_set_wrapper(*params_.expr_factory_, params_.session_info_);
      if (OB_FAIL(enum_set_wrapper.wrap_enum_set(*dml_stmt))) {
      }
    }

    if (OB_SUCC(ret) && stmt->is_dml_stmt() && !stmt->is_explain_stmt()) {
      if (OB_FAIL(params_.query_ctx_->query_hint_.init_query_hint(params_.allocator_,
                                                                  params_.session_info_,
                                                                  static_cast<ObDMLStmt*>(stmt)))) {
      } else if (OB_FAIL(params_.query_ctx_->query_hint_.check_and_set_params_from_hint(params_,
                                                         *static_cast<ObDMLStmt*>(stmt)))) {
      }
    }

    if (OB_SUCC(ret) && stmt->is_dml_stmt() && !stmt->is_explain_stmt()) {
      bool is_contain_inner_table = false;
      bool is_contain_select_for_update = false;
      ObDMLStmt *dml_stmt = static_cast<ObDMLStmt*>(stmt);
      if (OB_FAIL(dml_stmt->check_if_contain_inner_table(is_contain_inner_table))) {
      } else if (OB_FAIL(dml_stmt->check_if_contain_select_for_update(
                  is_contain_select_for_update))) {
      } else {
        params_.query_ctx_->is_contain_inner_table_ = is_contain_inner_table;
        params_.query_ctx_->is_contain_select_for_update_ = is_contain_select_for_update;
        params_.query_ctx_->has_dml_write_stmt_ = dml_stmt->is_dml_write_stmt();
      }

    }
    if (OB_SUCC(ret)) {
      stmt::StmtType stmt_type = stmt->get_stmt_type();
      if (ObStmt::is_ddl_stmt(stmt_type, stmt->has_global_variable()) || ObStmt::is_dcl_stmt(stmt_type)) {
        ObDDLStmt *ddl_stmt = static_cast<ObDDLStmt*>(stmt);
        obcall::ObDDLArg &ddl_arg = ddl_stmt->get_ddl_arg();

        if (OB_ISNULL(params_.query_ctx_)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ddl_arg.ddl_stmt_str_ = params_.query_ctx_->get_sql_stmt();
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(check_ai_column_strict_read(params_, stmt))) {
    }
  }  // end if
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
