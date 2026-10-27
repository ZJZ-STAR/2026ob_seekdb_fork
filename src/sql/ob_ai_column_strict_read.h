#pragma once

#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/ob_ai_column_nested_guard.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "share/ai_service/ob_ai_column_sql.h"

namespace oceanbase { namespace sql {
inline int check_ai_column_strict_read(ObExecContext &ctx, const ObPhysicalPlan &plan)
{
  using namespace common;
  using namespace share::schema;
  int ret = OB_SUCCESS;
  auto *session = ctx.get_my_session();
  if (session->is_inner() && !session->is_user_session()) { return ret; }
  if (plan.get_ai_column_refs().empty()) { return ret; }
  int64_t strict = 0;
  if (OB_FAIL(session->get_sys_variable_by_name(ObString("ai_column_strict_read"), strict))) { return ret; }
  if (!strict) { return ret; }
  common::sqlclient::ObISQLConnectionGuard connection;
  if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, connection))) { return ret; }
  auto *inner = as_inner_sql_connection(connection.get_ptr());
  if (inner == nullptr) { return OB_ERR_UNEXPECTED; }
  ObSQLSessionInfo::StmtSavedValue saved;
  ObIInnerSQLConnection::SavedValue saved_conn;
  AiColumnNestedGuard nested_guard(ctx);
  if (OB_FAIL(inner->begin_nested_session(saved, saved_conn, true))) { return ret; }
  const auto &refs = plan.get_ai_column_refs();
  for (int64_t i = 0; OB_SUCC(ret) && i + 1 < refs.count(); i += 2) {
    const ObTableSchema *table = nullptr;
    const ObDatabaseSchema *database = nullptr;
    auto *guard = ctx.get_sql_ctx()->schema_guard_;
    if (OB_FAIL(guard->get_table_schema(refs.at(i), table))) {}
    else if (table == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(guard->get_database_schema(table->get_database_id(), database))) {}
    else if (database == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else {
      const auto *column = table->get_column_schema(refs.at(i + 1));
      const auto *state = column == nullptr ? nullptr : table->get_column_schema(column->get_ai_column_number(2));
      if (state == nullptr) { ret = OB_SCHEMA_ERROR; }
      else {
        using SQL = share::AiColumnSQL;
        std::string sql = "SELECT 1 FROM " + SQL::ident(database->get_database_name_str()) + "." +
          SQL::ident(table->get_table_name_str()) + " WHERE " + SQL::ident(state->get_column_name_str()) +
          " IS NULL OR " + SQL::ident(state->get_column_name_str()) + "<>'READY' LIMIT 1";
        SMART_VAR(ObMySQLProxy::MySQLResult, result) {
          if (OB_FAIL(inner->execute_read(sql.c_str(), result))) {}
          else if (result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
          else {
            int next = result.get_result()->next();
            if (next == OB_SUCCESS) {
              ret = OB_AI_COLUMN_NOT_READY;
              LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, refs.at(i), refs.at(i + 1));
            } else if (next != OB_ITER_END) { ret = next; }
          }
        }
      }
    }
  }
  int end_ret = inner->end_nested_session(saved, saved_conn);
  return OB_SUCC(ret) ? end_ret : ret;
}
} }
