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

#ifndef OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_
#define OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "common/json_type/ob_json_base.h"
#include "share/schema/ob_schema_struct.h"
#include "share/ai_service/ob_ai_model_info.h"

namespace oceanbase
{
namespace share
{

// Layer-neutral AI service endpoint info from user side.
class ObAiModelEndpointInfo
{
  friend class ObAiServiceProxy;
public:
  ObAiModelEndpointInfo() { reset(); }
  ~ObAiModelEndpointInfo() = default;

  void reset()
  {
    name_.reset();
    scope_ = DEFAULT_SCOPE;
    ai_model_name_.reset();
    url_.reset();
    access_key_.reset();
    provider_.reset();
    request_model_name_.reset();
    parameters_.reset();
    request_transform_fn_.reset();
    response_transform_fn_.reset();
    max_concurrency_ = 0;
    endpoint_id_ = OB_INVALID_ID;
  }

  int parse_from_json_base(common::ObArenaAllocator &allocator,const ObString &name, const common::ObIJsonBase &params_jbase);
  int merge_delta_endpoint(common::ObArenaAllocator &allocator, const ObIJsonBase &delta_endpoint);
  int deep_copy(common::ObIAllocator &allocator, const ObAiModelEndpointInfo &other);
  int check_valid() const;

  const ObString &get_name() const { return name_; }
  const ObString &get_scope() const { return scope_; }
  const ObString &get_url() const { return url_; }
  const ObString &get_encrypted_access_key() const { return access_key_; }
  int get_unencrypted_access_key(common::ObIAllocator &allocator, ObString &unencrypted_access_key) const;
  const ObString &get_ai_model_name() const { return ai_model_name_; }
  const ObString &get_request_model_name() const { return request_model_name_; }
  const ObString &get_provider() const { return provider_; }
  const ObString &get_parameters() const { return parameters_; }
  const ObString &get_request_transform_fn() const { return request_transform_fn_; }
  const ObString &get_response_transform_fn() const { return response_transform_fn_; }
  int64_t get_max_concurrency() const { return max_concurrency_; }
  uint64_t get_endpoint_id() const { return endpoint_id_; }
  void set_endpoint_id(uint64_t endpoint_id) { endpoint_id_ = endpoint_id; }

  TO_STRING_KV(K_(name),
               K_(scope),
               K_(endpoint_id),
               K_(url),
               K_(access_key),
               K_(ai_model_name),
               K_(provider),
               K_(request_model_name),
               K_(parameters),
               K_(request_transform_fn),
               K_(response_transform_fn),
               K_(max_concurrency));
private:
  static const ObString DEFAULT_SCOPE;
  static bool is_valid_provider(const ObString &provider);
  static bool is_ollama_provider(const ObString &provider);
  static bool is_valid_max_concurrency(int64_t max_concurrency);
  int encrypt_access_key_(common::ObIAllocator &allocator, const ObString &access_key, ObString &encrypted_access_key);
  int decrypt_access_key_(common::ObIAllocator &allocator, const ObString &encrypted_access_key, ObString &unencrypted_access_key) const;
private:
  uint64_t endpoint_id_;
  ObString name_;
  ObString scope_;
  ObString ai_model_name_;
  ObString url_;
  ObString access_key_;
  ObString provider_;
  ObString request_model_name_;
  ObString parameters_;
  ObString request_transform_fn_;
  ObString response_transform_fn_;
  int64_t max_concurrency_;
};

// The definition of an AI column.
//
// A column that was declared with "AI COLUMN (...)" carries its definition in
// the comment of the column.  The marker at the beginning of the comment tells
// an AI column apart from an ordinary column, the rest of the comment holds
// the definition itself:
//
//   __ai_column__:<func_type>|<model>|<sync_mode>|<max_retries>|<inputs>|<expr>
//
// The definition is needed by the DDL, the metadata table of the AI columns is
// filled when the table of the column is created.
class ObAiColumnDef
{
public:
  ObAiColumnDef() : func_type_(0), max_retries_(0) {}
  int assign(common::ObIAllocator &allocator,
             int64_t func_type,
             const common::ObString &model_name,
             const common::ObString &sync_mode,
             int64_t max_retries,
             const common::ObString &normalized_expr,
             const common::ObString &input_columns);
  // Build the text that is stored in the comment of the column.
  int build_marker(common::ObIAllocator &allocator, common::ObString &marker) const;
  // True when the comment of a column holds the definition of an AI column.
  static bool is_ai_column_comment(const common::ObString &comment);
  // Read the definition back from the comment of the column.
  int parse_marker(common::ObIAllocator &allocator, const common::ObString &comment);

  int64_t get_func_type() const { return func_type_; }
  int64_t get_max_retries() const { return max_retries_; }
  const common::ObString &get_model_name() const { return model_name_; }
  const common::ObString &get_sync_mode() const { return sync_mode_; }
  const common::ObString &get_normalized_expr() const { return normalized_expr_; }
  const common::ObString &get_input_columns() const { return input_columns_; }
  // The names of the columns that the ai function of the AI column reads, they
  // are the dependencies of the AI column.
  int get_input_column_names(common::ObIAllocator &allocator,
                              common::ObIArray<common::ObString> &input_column_names) const;

  // The marker of the comment of an AI column.
  static const char *const AI_COLUMN_MARKER;
  // The prefixes of the names of the three hidden columns of an AI column.
  static const char *const AI_STATE_PREFIX;
  static const char *const AI_REQUEST_PREFIX;
  static const char *const AI_RESULT_SPEC_PREFIX;
  // The name of the hidden column that holds the given part of an AI column.
  static int build_hidden_column_name(common::ObIAllocator &allocator,
                                       const char *prefix,
                                       const common::ObString &ai_column_name,
                                       common::ObString &hidden_name);
private:
  int64_t func_type_;
  int64_t max_retries_;
  common::ObString model_name_;
  common::ObString sync_mode_;
  common::ObString normalized_expr_;
  common::ObString input_columns_;
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_