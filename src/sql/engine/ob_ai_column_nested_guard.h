#pragma once

#include "sql/engine/ob_exec_context.h"

namespace oceanbase { namespace sql {
class AiColumnNestedGuard
{
public:
  explicit AiColumnNestedGuard(ObExecContext &ctx)
    : context_(ctx.get_das_ctx()), saved_(context_.is_ai_column_nested_sql_)
  { context_.is_ai_column_nested_sql_ = true; }
  ~AiColumnNestedGuard() { context_.is_ai_column_nested_sql_ = saved_; }
private:
  ObDASCtx &context_;
  bool saved_;
};
} }
