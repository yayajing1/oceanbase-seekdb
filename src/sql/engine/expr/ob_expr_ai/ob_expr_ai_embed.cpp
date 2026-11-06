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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // null model id or null content gives a null result
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

bool ObExprAIEmbed::can_batch_eval_ai_embed(const ObRawExpr &raw_expr)
{
  const ObRawExpr *model_expr = raw_expr.get_param_expr(MODEL_IDX);
  const ObRawExpr *content_expr = raw_expr.get_param_expr(CONTENT_IDX);
  if (OB_ISNULL(model_expr) || OB_ISNULL(content_expr)) {
    return false;
  }
  // the model id and the dimension have to be the same for every row
  if (model_expr->is_vectorize_result()) {
    return false;
  }
  if (raw_expr.get_param_count() > DIM_IDX) {
    const ObRawExpr *dim_expr = raw_expr.get_param_expr(DIM_IDX);
    if (OB_ISNULL(dim_expr) || dim_expr->is_vectorize_result()) {
      return false;
    }
  }
  // the content has to be evaluated as a batch
  return content_expr->is_vectorize_result();
}

int ObExprAIEmbed::eval_ai_embed_batch(BATCH_EVAL_FUNC_ARG_DECL)
{
  int ret = OB_SUCCESS;
  ObDatum *model_datum = nullptr;
  ObDatum *dim_datum = nullptr;
  ObExpr &content_expr = *expr.args_[CONTENT_IDX];
  ObDatum *res_datums = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObArray<ObString> contents;
  ObArray<int64_t> row_indexes;
  bool has_null_row = false;

  if (OB_FAIL(expr.args_[MODEL_IDX]->eval(ctx, model_datum))) {
  } else if (expr.arg_cnt_ == 3 && OB_FAIL(expr.args_[DIM_IDX]->eval(ctx, dim_datum))) {
  } else if (OB_UNLIKELY(!content_expr.is_batch_result())) {
    // the content is not evaluated as a batch, evaluate every row on its own
    ObEvalCtx::BatchInfoScopeGuard batch_info_guard(ctx);
    for (int64_t i = 0; OB_SUCC(ret) && i < size; i++) {
      if (skip.at(i)) {
        continue;
      }
      batch_info_guard.set_batch_idx(i);
      if (OB_FAIL(eval_ai_embed(expr, ctx, res_datums[i]))) {
      } else {
        eval_flags.set(i);
      }
    }
  } else if (OB_FAIL(content_expr.eval_batch(ctx, skip, size))) {
  } else {
    const ObDatum *content_datums = content_expr.locate_batch_datums(ctx);
    for (int64_t i = 0; OB_SUCC(ret) && i < size; i++) {
      if (skip.at(i)) {
        continue;
      }
      if (OB_UNLIKELY(model_datum->is_null() || content_datums[i].is_null())) {
        // a null model id or a null content gives a null result, such a row
        // never reaches the model and never shifts the other rows
        res_datums[i].set_null();
        eval_flags.set(i);
        has_null_row = true;
      } else {
        ObString content = content_datums[i].get_string();
        if (OB_UNLIKELY(content.empty())) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
        } else if (OB_FAIL(contents.push_back(content))) {
        } else if (OB_FAIL(row_indexes.push_back(i))) {
        }
      }
    }
  }

  if (OB_SUCC(ret) && contents.count() > 0) {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    ObJsonObject *config = nullptr;
    ObJsonInt *dim_json = nullptr;
    int64_t dim = 0;
    if (OB_NOT_NULL(dim_datum) && !dim_datum->is_null()) {
      dim = dim_datum->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_SUCC(ret)) {
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      ObString model_id = model_datum->get_string();
      if (model_id.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     model_id, temp_allocator, resolved_endpoint))) {
      } else {
        ObAIFuncModel model(temp_allocator, *info, resolved_endpoint);
        ObArray<ObString> merged_results;
        ObArray<ObString> row_results;
        ObArray<ObString> *final_results = nullptr;
        if (OB_SUCC(model.call_dense_embedding_batch(contents, config, merged_results))) {
          final_results = &merged_results;
        } else {
          // the endpoint may not answer a merged request, retry row by row so
          // that the result of every row is still correct
          LOG_WARN("failed to embed the merged request, retry row by row", K(ret));
          ret = OB_SUCCESS;
          for (int64_t i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
            ObArray<ObString> one_content;
            ObArray<ObString> one_result;
            if (OB_FAIL(one_content.push_back(contents[i]))) {
            } else if (OB_FAIL(model.call_dense_embedding_vector_v2(one_content, config, one_result))) {
            } else if (one_result.count() != 1) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_FAIL(row_results.push_back(one_result[0]))) {
            }
          }
          if (OB_SUCC(ret)) {
            final_results = &row_results;
          }
        }
        if (OB_SUCC(ret) && OB_NOT_NULL(final_results)) {
          ObEvalCtx::BatchInfoScopeGuard batch_info_guard(ctx);
          const int64_t result_count = final_results->count();
          for (int64_t i = 0; OB_SUCC(ret) && i < result_count && i < row_indexes.count(); i++) {
            const int64_t row = row_indexes[i];
            ObString result_str = (*final_results)[i];
            batch_info_guard.set_batch_idx(row);
            if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res_datums[row], result_str))) {
            } else {
              eval_flags.set(row);
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && has_null_row) {
    expr.get_eval_info(ctx).notnull_ = false;
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    // the rows of a batch are merged into one request
    if (can_batch_eval_ai_embed(raw_expr)) {
      rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
