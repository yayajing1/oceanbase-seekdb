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

#ifndef OCEANBASE_SQL_RESOLVER_OB_RESOLVER
#define OCEANBASE_SQL_RESOLVER_OB_RESOLVER

#include "sql/resolver/ob_stmt_resolver.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;
class ObSelectStmt;

/// the interface of this module
class ObResolver
{
public:
  enum IsPrepared
  {
    IS_PREPARED_STMT,
    IS_NOT_PREPARED_STMT
  };

  explicit ObResolver(ObResolverParams &params);
  virtual ~ObResolver();

  virtual int resolve(IsPrepared if_prepared, const ParseNode &parse_tree, ObStmt *&stmt);
  ObResolverParams &get_params() { return params_; }

private:
  template <typename ResolverType>
  int stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt);

  template <typename SelectResolverType>
  int select_stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt);

  /* COPY (query) TO 'file' */
  int copy_stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt);
  /* The retry of the result of an AI column, it is an update of the AI
     column. */
  int resolve_ai_column_retry(const ParseNode &parse_tree, ObStmt *&stmt, bool &is_retry);
  int resolve_ai_column_retry_update(const ParseNode &parse_tree,
                                      const ParseNode &retry_node,
                                      ObStmt *&stmt);
  int check_ai_column_retry(ObStmt *stmt, const ParseNode *column_name_node);
  int resolve_copy_stmt(ObResolverParams &params, const ParseNode &parse_tree, ObSelectStmt &select_stmt);
private:
  // data members
  ObResolverParams params_;
private:
  // disallow copy
  DISALLOW_COPY_AND_ASSIGN(ObResolver);
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_SQL_RESOLVER_OB_RESOLVER */
