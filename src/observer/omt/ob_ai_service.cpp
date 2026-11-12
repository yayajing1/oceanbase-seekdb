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

#define USING_LOG_PREFIX SERVER_OMT
#include "ob_ai_service.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string_buffer.h"
#include "share/ai_service/ob_ai_service_struct.h"

using namespace oceanbase::share;
using namespace oceanbase::common;
namespace oceanbase
{
namespace omt
{

/* ---------------------------------------------------------------------------
 * The worker of the AI columns
 *
 * The tasks of the AI columns are in the table of the tasks, every task belongs
 * to one AI column of one row.  The tasks of the AI columns of one row belong
 * to one request, they are computed by one call of the model and their results
 * are written back to the row by one statement, the results of the AI columns
 * of the row become visible at the same time.
 * ------------------------------------------------------------------------- */

/* the table that holds the tasks of the AI columns */
static const char *const AI_COLUMN_TASK_TABLE = "oceanbase.__all_ai_column_task";
/* the table that holds the definitions of the AI columns */
static const char *const AI_COLUMN_TABLE = "oceanbase.__all_ai_column";
/* the state of a task that waits for the worker */
static const int64_t AI_TASK_STATE_PENDING = 0;
/* the state of a task that failed and waits for the next try */
static const int64_t AI_TASK_STATE_RETRY = 3;
/* the number of tasks that the worker picks up at once */
static const int64_t AI_TASK_BATCH_SIZE = 64;
/* the time between two tries of a task that failed */
static const int64_t AI_TASK_RETRY_DELAY_SECONDS = 2;
/* the value of the state column of an AI column whose result is ready */
static const char *const AI_COLUMN_STATE_READY = "READY";
/* the value of the state column of an AI column whose result failed */
static const char *const AI_COLUMN_STATE_FAILED = "FAILED";
/* the time between two rounds of the worker */
static const int64_t AI_WORKER_PERIOD = 50 * 1000L; /* 50ms */

/* Append the value as a literal of a statement.  The value comes from the
   model, it may hold any character. */
static int append_ai_literal(const ObString &value,
                             ObStringBuffer &buf)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(buf.append("'"))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      const char c = value.ptr()[i];
      switch (c) {
        case '\'': ret = buf.append("''"); break;
        case '\\': ret = buf.append("\\\\"); break;
        case '\n': ret = buf.append("\\n"); break;
        case '\r': ret = buf.append("\\r"); break;
        case '\t': ret = buf.append("\\t"); break;
        case '\032': ret = buf.append("\\Z"); break;
        default: ret = buf.append(&c, 1); break;
      }
    }
    if (OB_SUCC(ret)) {
      ret = buf.append("'");
    }
  }
  return ret;
}

/* The name of the hidden column that holds the state of an AI column. */
static int build_ai_state_column_name(ObIAllocator &allocator,
                                       const ObString &ai_column_name,
                                       ObString &state_column_name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObAiColumnDef::build_hidden_column_name(allocator,
                                                      ObAiColumnDef::AI_STATE_PREFIX,
                                                      ai_column_name,
                                                      state_column_name))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

ObAiColumnWorkerTask::ObAiColumnWorkerTask()
  : sql_proxy_(nullptr), is_inited_(false)
{
}

ObAiColumnWorkerTask::~ObAiColumnWorkerTask()
{
}

int ObAiColumnWorkerTask::init(common::ObMySQLProxy &sql_proxy)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    sql_proxy_ = &sql_proxy;
    /* the call of the model may take a long time */
    disable_timeout_check();
    is_inited_ = true;
  }
  return ret;
}

void ObAiColumnWorkerTask::runTimerTask()
{
  ObCurTraceId::init(GCTX.self_addr());
  do_work(); /* ignore the error */
}

int ObAiColumnWorkerTask::do_work()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;
  ObSEArray<ObAiTask, 16> tasks;
  if (OB_FAIL(collect_tasks(allocator, tasks))) {
    LOG_WARN("cannot collect the tasks of the AI columns", K(ret));
  } else if (tasks.count() > 0) {
    ret = process_tasks(allocator, tasks);
  }
  return ret;
}

/* The tasks that are ready to be computed. */
int ObAiColumnWorkerTask::collect_tasks(ObIAllocator &allocator,
                                         ObSEArray<ObAiTask, 16> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObMySQLTransaction trans;
  if (OB_FAIL(sql.assign_fmt("SELECT task_id, table_id, column_id, request_id, rowkey_predicate, retry_count"
                              " FROM %s WHERE eligible_ts <= now(6) ORDER BY task_id LIMIT %ld",
                              AI_COLUMN_TASK_TABLE, AI_TASK_BATCH_SIZE))) {
  } else if (OB_FAIL(trans.start(sql_proxy_))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(trans.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          ObAiTask task;
          ObString request_id;
          ObString rowkey_predicate;
          if (OB_FAIL(result->get_int(static_cast<int64_t>(0), task.task_id_))
              || OB_FAIL(result->get_int(static_cast<int64_t>(1), task.table_id_))
              || OB_FAIL(result->get_int(static_cast<int64_t>(2), task.column_id_))
              || OB_FAIL(result->get_varchar(static_cast<int64_t>(3), request_id))
              || OB_FAIL(result->get_varchar(static_cast<int64_t>(4), rowkey_predicate))
              || OB_FAIL(result->get_int(static_cast<int64_t>(5), task.retry_count_))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(deep_copy_ob_string(allocator, request_id, task.request_id_))
                     || OB_FAIL(deep_copy_ob_string(allocator, rowkey_predicate,
                                                   task.rowkey_predicate_))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(tasks.push_back(task))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
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

/* The tasks of one row are computed together, they are grouped by the request
   they belong to. */
int ObAiColumnWorkerTask::process_tasks(ObIAllocator &allocator,
                                         ObSEArray<ObAiTask, 16> &tasks)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAiTaskGroup, 8> groups;
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    const ObAiTask &task = tasks.at(i);
    ObAiTaskGroup *group = NULL;
    for (int64_t j = 0; OB_ISNULL(group) && j < groups.count(); ++j) {
      ObAiTaskGroup &candidate = groups.at(j);
      if (candidate.table_id_ == task.table_id_
          && 0 == candidate.request_id_.compare(task.request_id_)) {
        group = &candidate;
      }
    }
    if (OB_ISNULL(group)) {
      ObAiTaskGroup candidate;
      candidate.table_id_ = task.table_id_;
      candidate.request_id_ = task.request_id_;
      candidate.rowkey_predicate_ = task.rowkey_predicate_;
      candidate.retry_count_ = task.retry_count_;
      if (OB_FAIL(groups.push_back(candidate))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        group = &groups.at(groups.count() - 1);
      }
    }
    if (!OB_ISNULL(group) && OB_FAIL(group->tasks_.push_back(task))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < groups.count(); ++i) {
    if (OB_FAIL(process_group(allocator, groups.at(i)))) {
      LOG_WARN("cannot process the task of an AI column",
               K(ret), K(groups.at(i).request_id_));
    }
  }
  return ret;
}

/* The definitions of the AI columns of the tasks of the group.  Only the AI
   columns of the tasks are kept, they are in the order of the tasks. */
int ObAiColumnWorkerTask::collect_column_metas(ObIAllocator &allocator,
                                                ObAiTaskGroup &group,
                                                ObSEArray<ObAiColumnMeta, 8> &column_metas)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObMySQLTransaction trans;
  ObSEArray<ObAiColumnMeta, 8> all_metas;
  if (OB_FAIL(sql.assign_fmt("SELECT table_id, column_id, column_name, normalized_expr, max_retries, state_column_id"
                              " FROM %s WHERE table_id = %ld",
                              AI_COLUMN_TABLE, group.table_id_))) {
  } else if (OB_FAIL(trans.start(sql_proxy_))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(trans.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          ObAiColumnMeta meta;
          ObString column_name;
          ObString normalized_expr;
          if (OB_FAIL(result->get_int(static_cast<int64_t>(0), meta.table_id_))
              || OB_FAIL(result->get_int(static_cast<int64_t>(1), meta.column_id_))
              || OB_FAIL(result->get_varchar(static_cast<int64_t>(2), column_name))
              || OB_FAIL(result->get_varchar(static_cast<int64_t>(3), normalized_expr))
              || OB_FAIL(result->get_int(static_cast<int64_t>(4), meta.max_retries_))
              || OB_FAIL(result->get_int(static_cast<int64_t>(5), meta.state_column_id_))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(deep_copy_ob_string(allocator, column_name, meta.column_name_))
                     || OB_FAIL(deep_copy_ob_string(allocator, normalized_expr,
                                                   meta.normalized_expr_))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(all_metas.push_back(meta))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
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
  for (int64_t i = 0; OB_SUCC(ret) && i < group.tasks_.count(); ++i) {
    const int64_t column_id = group.tasks_.at(i).column_id_;
    bool found = false;
    for (int64_t j = 0; OB_SUCC(ret) && j < all_metas.count(); ++j) {
      if (all_metas.at(j).column_id_ == column_id) {
        if (OB_FAIL(column_metas.push_back(all_metas.at(j)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        }
        found = true;
        break;
      }    }
    if (OB_SUCC(ret) && !found) {
      /* the definition of the AI column is gone, the task cannot be computed */
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the definition of an AI column is gone", K(ret), K(column_id),
               K(all_metas.count()), K(group.table_id_));
    }
  }
  return ret;
}

/* The table that owns the AI columns of the group. */
int ObAiColumnWorkerTask::collect_table_meta(ObIAllocator &allocator,
                                              ObAiTaskGroup &group,
                                              ObAiTableMeta &table_meta)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObMySQLTransaction trans;
  if (OB_FAIL(sql.assign_fmt("SELECT d.database_name, t.table_name"
                              " FROM oceanbase.__all_table t, oceanbase.__all_database d"
                              " WHERE t.database_id = d.database_id AND t.table_id = %ld",
                              group.table_id_))) {
  } else if (OB_FAIL(trans.start(sql_proxy_))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(trans.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_TABLE_NOT_EXIST;
        }
      } else {
        ObString database_name;
        ObString table_name;
        if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), database_name))
            || OB_FAIL(result->get_varchar(static_cast<int64_t>(1), table_name))) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(deep_copy_ob_string(allocator, database_name,
                                               table_meta.database_name_))
                   || OB_FAIL(deep_copy_ob_string(allocator, table_name,
                                                  table_meta.table_name_))) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          table_meta.table_id_ = group.table_id_;
        }
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

/* Ask the model for the result of the AI columns of the row.  The row is read
   once, the result of every AI column of the row is computed from the same
   row. */
int ObAiColumnWorkerTask::compute_results(ObIAllocator &allocator,
                                           ObAiTaskGroup &group,
                                           const ObAiTableMeta &table_meta,
                                           ObSEArray<ObAiColumnMeta, 8> &column_metas,
                                           ObSEArray<ObString, 8> &results,
                                           bool &row_exists)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObStringBuffer buf(&allocator);
  ObMySQLTransaction trans;
  row_exists = false;
  if (OB_FAIL(buf.append("SELECT "))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_metas.count(); ++i) {
      if (i > 0 && OB_FAIL(buf.append(", "))) {
      } else if (OB_FAIL(buf.append(column_metas.at(i).normalized_expr_))) {
      }
    }
    if (OB_SUCC(ret)
        && OB_FAIL(buf.append(" FROM `"))) {
    } else if (OB_SUCC(ret)
               && OB_FAIL(buf.append(table_meta.database_name_))) {
    } else if (OB_SUCC(ret)
               && OB_FAIL(buf.append("`.`"))) {
    } else if (OB_SUCC(ret)
               && OB_FAIL(buf.append(table_meta.table_name_))) {
    } else if (OB_SUCC(ret)
               && OB_FAIL(buf.append("` WHERE "))) {
    } else if (OB_SUCC(ret)
               && OB_FAIL(buf.append(group.rowkey_predicate_))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.assign(buf.string()))) {
  } else if (OB_FAIL(trans.start(sql_proxy_))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(trans.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          /* the row is gone, there is nothing to compute */
          ret = OB_SUCCESS;
          row_exists = false;
        }
      } else {
        row_exists = true;
        for (int64_t i = 0; OB_SUCC(ret) && i < column_metas.count(); ++i) {
          ObString value;
          ObString result_value;
          if (OB_FAIL(result->get_varchar(i, value))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(deep_copy_ob_string(allocator, value, result_value))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(results.push_back(result_value))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          }
        }
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

/* Write the result of the AI columns back to the row and take the tasks of the
   group away.  The tasks are taken away in the same transaction, the result
   is not written when the tasks are gone, the result of a task that was
   replaced by a newer one is too old. */
int ObAiColumnWorkerTask::write_back(ObAiTaskGroup &group,
                                      const ObAiTableMeta &table_meta,
                                      ObSEArray<ObAiColumnMeta, 8> &column_metas,
                                      ObSEArray<ObString, 8> &results)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;
  ObStringBuffer buf(&allocator);
  ObSqlString update_sql;
  ObSqlString delete_sql;
  ObMySQLTransaction trans;
  int64_t affected = 0;
  bool is_stale = false;
  if (OB_FAIL(buf.append("UPDATE `"))) {
  } else if (OB_FAIL(buf.append(table_meta.database_name_))) {
  } else if (OB_FAIL(buf.append("`.`"))) {
  } else if (OB_FAIL(buf.append(table_meta.table_name_))) {
  } else if (OB_FAIL(buf.append("` SET "))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_metas.count(); ++i) {
      const ObAiColumnMeta &meta = column_metas.at(i);
      ObString state_column_name;
      if (i > 0 && OB_FAIL(buf.append(", "))) {
      } else if (OB_FAIL(buf.append("`"))) {
      } else if (OB_FAIL(buf.append(meta.column_name_))) {
      } else if (OB_FAIL(buf.append("` = "))) {
      } else if (OB_FAIL(append_ai_literal(results.at(i), buf))) {
      } else if (OB_FAIL(build_ai_state_column_name(*buf.get_allocator(),
                                                    meta.column_name_,
                                                    state_column_name))) {
      } else if (OB_FAIL(buf.append(", `"))) {
      } else if (OB_FAIL(buf.append(state_column_name))) {
      } else if (OB_FAIL(buf.append("` = '"))) {
      } else if (OB_FAIL(buf.append(AI_COLUMN_STATE_READY))) {
      } else if (OB_FAIL(buf.append("'"))) {
      }
    }
    if (OB_SUCC(ret)
        && OB_FAIL(buf.append(" WHERE "))) {
    } else if (OB_SUCC(ret)
               && OB_FAIL(buf.append(group.rowkey_predicate_))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(update_sql.assign(buf.string()))) {
  } else if (OB_SUCC(ret)
             && OB_FAIL(delete_sql.assign_fmt("DELETE FROM %s WHERE request_id = '%.*s'",
                                             AI_COLUMN_TASK_TABLE,
                                             static_cast<int>(group.request_id_.length()),
                                             group.request_id_.ptr()))) {
  } else if (OB_FAIL(trans.start(sql_proxy_))) {
  } else if (OB_FAIL(trans.write(update_sql.ptr(), affected))) {
  } else if (OB_FAIL(trans.write(delete_sql.ptr(), affected))) {
  } else if (affected != group.tasks_.count()) {
    /* the task was replaced by a newer one, the result is too old */
    is_stale = true;
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCC(ret) && !is_stale);
    if (OB_SUCC(ret) && OB_SUCCESS != end_ret) {
      ret = end_ret;
    }
  }
  if (OB_SUCC(ret) && is_stale) {
    LOG_INFO("the result of an AI column is too old, it is dropped", K(group.request_id_));
  }
  return ret;
}

/* The task waits for the next try. */
int ObAiColumnWorkerTask::mark_retry(ObAiTaskGroup &group,
                                      const int64_t retry_count,
                                      const int error)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObMySQLTransaction trans;
  int64_t affected = 0;
  char error_buf[64];
  databuff_printf(error_buf, sizeof(error_buf), "%d", error);
  if (OB_FAIL(sql.assign_fmt("UPDATE %s SET state = %ld, retry_count = %ld,"
                              " eligible_ts = now(6) + INTERVAL %ld SECOND, last_error = '%s'"
                              " WHERE request_id = '%.*s'",
                              AI_COLUMN_TASK_TABLE,
                              AI_TASK_STATE_RETRY,
                              retry_count,
                              AI_TASK_RETRY_DELAY_SECONDS,
                              error_buf,
                              static_cast<int>(group.request_id_.length()),
                              group.request_id_.ptr()))) {
  } else if (OB_FAIL(trans.start(sql_proxy_))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret) && OB_SUCCESS != end_ret) {
      ret = end_ret;
    }
  }
  return ret;
}

/* The task will not be tried again, the result of the AI column stays null
   and the state of the AI column tells that the result failed. */
int ObAiColumnWorkerTask::finish_failed(ObAiTaskGroup &group,
                                         ObSEArray<ObAiColumnMeta, 8> &column_metas,
                                         const ObAiTableMeta *table_meta)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;
  ObStringBuffer buf(&allocator);
  ObSqlString update_sql;
  ObSqlString delete_sql;
  ObMySQLTransaction trans;
  int64_t affected = 0;
  if (!OB_ISNULL(table_meta) && column_metas.count() > 0) {
    if (OB_FAIL(buf.append("UPDATE `"))) {
    } else if (OB_FAIL(buf.append(table_meta->database_name_))) {
    } else if (OB_FAIL(buf.append("`.`"))) {
    } else if (OB_FAIL(buf.append(table_meta->table_name_))) {
    } else if (OB_FAIL(buf.append("` SET "))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_metas.count(); ++i) {
        ObString state_column_name;
        if (i > 0 && OB_FAIL(buf.append(", "))) {
        } else if (OB_FAIL(build_ai_state_column_name(*buf.get_allocator(),
                                                      column_metas.at(i).column_name_,
                                                      state_column_name))) {
        } else if (OB_FAIL(buf.append("`"))) {
        } else if (OB_FAIL(buf.append(state_column_name))) {
        } else if (OB_FAIL(buf.append("` = '"))) {
        } else if (OB_FAIL(buf.append(AI_COLUMN_STATE_FAILED))) {
        } else if (OB_FAIL(buf.append("'"))) {
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(buf.append(" WHERE "))) {
      } else if (OB_SUCC(ret)
                 && OB_FAIL(buf.append(group.rowkey_predicate_))) {
      }
    }
  }
  if (OB_SUCC(ret)
      && OB_FAIL(delete_sql.assign_fmt("DELETE FROM %s WHERE request_id = '%.*s'",
                                       AI_COLUMN_TASK_TABLE,
                                       static_cast<int>(group.request_id_.length()),
                                       group.request_id_.ptr()))) {
  } else if (OB_FAIL(trans.start(sql_proxy_))) {
  } else {
    if (buf.length() > 0) {
      if (OB_FAIL(update_sql.assign(buf.string()))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(trans.write(update_sql.ptr(), affected))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(trans.write(delete_sql.ptr(), affected))) {
    }
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret) && OB_SUCCESS != end_ret) {
      ret = end_ret;
    }
  }
  return ret;
}

/* Compute the result of the AI columns of the row of the group. */
int ObAiColumnWorkerTask::process_group(ObIAllocator &allocator,
                                         ObAiTaskGroup &group)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAiColumnMeta, 8> column_metas;
  ObAiTableMeta table_meta;
  ObSEArray<ObString, 8> results;
  bool has_table_meta = false;
  bool row_exists = false;
  int compute_ret = collect_column_metas(allocator, group, column_metas);
  int64_t max_retries = 0;
  if (OB_SUCC(compute_ret)) {
    /* the retry policy belongs to the definition of the AI column, it must be
       read before the result is computed */
    for (int64_t i = 0; OB_SUCC(ret) && i < column_metas.count(); ++i) {
      max_retries = (i == 0) ? column_metas.at(i).max_retries_
                              : std::max(max_retries, column_metas.at(i).max_retries_);
    }
    compute_ret = collect_table_meta(allocator, group, table_meta);
    has_table_meta = (OB_SUCCESS == compute_ret);
  }
  if (OB_SUCC(compute_ret)) {
    compute_ret = compute_results(allocator, group, table_meta, column_metas, results, row_exists);
  }
  if (OB_SUCC(compute_ret)) {
    if (!row_exists) {
      /* the row is gone, the task is not needed any more */
      ret = finish_failed(group, column_metas, NULL);
    } else {
      ret = write_back(group, table_meta, column_metas, results);
    }
  } else {
    /* the model did not answer, the task waits for the next try */
    const int64_t retry_count = group.retry_count_ + 1;
    LOG_INFO("the model did not answer", K(group.request_id_), K(compute_ret),
             K(max_retries), K(retry_count), K(column_metas.count()), K(group.tasks_.count()));
    if (retry_count > max_retries) {
      LOG_INFO("the result of an AI column failed", K(group.request_id_), K(compute_ret));
      ret = finish_failed(group, column_metas, has_table_meta ? &table_meta : NULL);
    } else {
      ret = mark_retry(group, retry_count, compute_ret);
    }
  }
  return ret;
}

ObAiService::ObAiService()
  : is_inited_(false), timer_(), worker_task_()
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(timer_.init("AiColumnWorker", common::ObMemAttr("AiColumnWorker")))) {
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(worker_task_.init(*GCTX.sql_proxy_))) {
  } else if (OB_FAIL(timer_.schedule(worker_task_, AI_WORKER_PERIOD, true))) {
    LOG_WARN("cannot schedule the worker of the AI columns", K(ret));
  } else {
    LOG_INFO("the worker of the AI columns is started");
  }
  return ret;
}

void ObAiService::stop()
{
  if (timer_.inited()) {
    timer_.stop();
  }
}

void ObAiService::wait()
{
  if (timer_.inited()) {
    timer_.wait();
  }
}

void ObAiService::destroy()
{
  if (timer_.inited()) {
    timer_.destroy();
  }
  is_inited_ = false;
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase
