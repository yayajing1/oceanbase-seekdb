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

#ifndef OCEANBASE_OBSERVER_OMT_OB_AI_SERVICE_H_
#define OCEANBASE_OBSERVER_OMT_OB_AI_SERVICE_H_

#include "share/ai_service/ob_ai_service_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/lock/ob_tc_rwlock.h"
#include "lib/container/ob_se_array.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "share/ob_server_struct.h"
#include "lib/worker.h"
#include "lib/task/ob_timer.h"
#include "common/mysqlclient/ob_mysql_proxy.h"

namespace oceanbase
{
namespace omt
{

class ObAiServiceGuard
{
public:
  ObAiServiceGuard();
  ~ObAiServiceGuard();
  int get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info);
  int get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check = true);
private:
  int check_access_privilege();
  ObArenaAllocator local_allocator_;
  DISALLOW_COPY_AND_ASSIGN(ObAiServiceGuard);
};

/* A task of the table of the tasks of the AI columns. */
struct ObAiTask
{
  int64_t task_id_;
  int64_t table_id_;
  int64_t column_id_;
  common::ObString request_id_;
  common::ObString rowkey_predicate_;
  int64_t retry_count_;
  ObAiTask()
    : task_id_(0), table_id_(0), column_id_(0), request_id_(), rowkey_predicate_(), retry_count_(0) {}
};

/* The definition of an AI column. */
struct ObAiColumnMeta
{
  int64_t table_id_;
  int64_t column_id_;
  common::ObString column_name_;
  common::ObString normalized_expr_;
  int64_t max_retries_;
  int64_t state_column_id_;
  ObAiColumnMeta()
    : table_id_(0), column_id_(0), column_name_(), normalized_expr_(), max_retries_(0), state_column_id_(0) {}
};

/* The table that owns the AI columns of a group of tasks. */
struct ObAiTableMeta
{
  int64_t table_id_;
  common::ObString database_name_;
  common::ObString table_name_;
  ObAiTableMeta() : table_id_(0), database_name_(), table_name_() {}
};

/* The tasks of the AI columns of one row, they are computed together. */
struct ObAiTaskGroup
{
  int64_t table_id_;
  common::ObString request_id_;
  common::ObString rowkey_predicate_;
  int64_t retry_count_;
  common::ObSEArray<ObAiTask, 8> tasks_;
  ObAiTaskGroup() : table_id_(0), request_id_(), rowkey_predicate_(), retry_count_(0) {}
};

/**
 * The worker of the AI columns.
 *
 * A row of a table with an AI column needs the result of the ai function of
 * the column.  The statement that wrote the row leaves a task in the table of
 * the tasks, the worker picks the task up, asks the model for the result and
 * writes the result back to the row.  The tasks of the AI columns of one row
 * belong to one request, they are computed together and their results become
 * visible at the same time.
 */
class ObAiColumnWorkerTask : public common::ObTimerTask
{
public:
  ObAiColumnWorkerTask();
  ~ObAiColumnWorkerTask();
  int init(common::ObMySQLProxy &sql_proxy);
  virtual void runTimerTask() override;
  void destroy() {}
private:
  int do_work();
  int collect_tasks(common::ObIAllocator &allocator, common::ObSEArray<ObAiTask, 16> &tasks);
  int process_tasks(common::ObIAllocator &allocator, common::ObSEArray<ObAiTask, 16> &tasks);
  int process_group(common::ObIAllocator &allocator, ObAiTaskGroup &group);
  int collect_column_metas(common::ObIAllocator &allocator,
                           ObAiTaskGroup &group,
                           common::ObSEArray<ObAiColumnMeta, 8> &column_metas);
  int collect_table_meta(common::ObIAllocator &allocator,
                         ObAiTaskGroup &group,
                         ObAiTableMeta &table_meta);
  int compute_results(common::ObIAllocator &allocator,
                       ObAiTaskGroup &group,
                       const ObAiTableMeta &table_meta,
                       common::ObSEArray<ObAiColumnMeta, 8> &column_metas,
                       common::ObSEArray<common::ObString, 8> &results,
                       bool &row_exists);
  int write_back(ObAiTaskGroup &group,
                 const ObAiTableMeta &table_meta,
                 common::ObSEArray<ObAiColumnMeta, 8> &column_metas,
                 common::ObSEArray<common::ObString, 8> &results);
  int mark_retry(ObAiTaskGroup &group, const int64_t retry_count, const int error);
  int finish_failed(ObAiTaskGroup &group,
                    common::ObSEArray<ObAiColumnMeta, 8> &column_metas,
                    const ObAiTableMeta *table_meta);
private:
  common::ObMySQLProxy *sql_proxy_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnWorkerTask);
};

class ObAiService
{
public:
  ObAiService();
  virtual ~ObAiService();
  static int server_module_init(ObAiService* &ai_service);
  int init();
  int start();
  void stop();
  void wait();
  void destroy();
public:
  int get_ai_service_guard(ObAiServiceGuard &ai_service_guard);
private:
  bool is_inited_;
  common::ObTimer timer_;
  ObAiColumnWorkerTask worker_task_;
  DISALLOW_COPY_AND_ASSIGN(ObAiService);
};

} // namespace omt
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_OMT_OB_AI_SERVICE_H_
