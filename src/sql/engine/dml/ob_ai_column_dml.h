#pragma once

#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/ob_ai_column_nested_guard.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/ai_service/ob_ai_column_sql.h"
#include "sql/ob_sql_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include <string>
#include <vector>

namespace oceanbase { namespace sql {
class AiColumnDML
{
public:
  static int prepare(const ObDMLBaseCtDef &def, ObTableModifyOp &op, bool deleting)
  {
    using namespace common;
    using namespace share;
    using namespace share::schema;
    int ret = OB_SUCCESS;
    auto &ctx = op.get_exec_ctx();
    auto *session = ctx.get_my_session();
    if (!def.is_primary_index_) { return ret; }
    if (common::is_inner_table(def.das_base_ctdef_.index_tid_)) { return ret; }
    const ObTableSchema *table = nullptr;
    auto *guard = ctx.get_sql_ctx()->schema_guard_;
    if (guard == nullptr) { return OB_ERR_UNEXPECTED; }
    if (OB_FAIL(guard->get_table_schema(def.das_base_ctdef_.index_tid_, table))) { return ret; }
    if (table == nullptr) { return OB_ERR_UNEXPECTED; }
    bool has_ai = false;
    for (int64_t i = 0; i < table->get_column_count(); ++i) {
      has_ai |= table->get_column_schema_by_idx(i)->is_ai_column();
    }
    if (!has_ai) { return ret; }
    const ObDatabaseSchema *database = nullptr;
    if (OB_FAIL(guard->get_database_schema(table->get_database_id(), database))) { return ret; }
    if (database == nullptr) { return OB_ERR_UNEXPECTED; }
    const auto &row = deleting ? def.old_row_ : def.new_row_;
    const bool inserting = def.old_row_.empty();
    std::string predicate, old_predicate;
    if (OB_FAIL(rowkey(def, row, *table, op, predicate))) {
      LOG_ERROR("AI column rowkey snapshot failed", KR(ret), K(def.column_ids_), K(row.count())); return ret;
    }
    if (!inserting && OB_FAIL(rowkey(def, def.old_row_, *table, op, old_predicate))) { return ret; }
    char request_uuid[UuidCommon::LENGTH_UUID];
    if (OB_FAIL(ObExprUuid::gen_server_uuid(request_uuid, sizeof(request_uuid)))) { return ret; }
    const std::string request(request_uuid, sizeof(request_uuid));
    if (OB_FAIL(op.open_inner_conn())) { LOG_ERROR("AI column open nested connection failed", KR(ret)); return ret; }
    AiColumnNestedGuard nested_guard(ctx);
    if (OB_FAIL(op.begin_nested_session(true))) { LOG_ERROR("AI column begin nested session failed", KR(ret)); return ret; }
    if (deleting || (!inserting && predicate != old_predicate)) {
      std::string sql = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=" +
        std::to_string(table->get_table_id()) + " AND rowkey_predicate=" + AiColumnSQL::literal(old_predicate);
      ret = op.execute_write(sql.c_str());
    }
    const char *stage = "input_snapshot";
    for (int64_t c = 0; OB_SUCC(ret) && !deleting && c < table->get_column_count(); ++c) {
      const auto *ai = table->get_column_schema_by_idx(c);
      if (!ai->is_ai_column()) { continue; }
      const int64_t request_idx = index(def, ai->get_ai_column_number(3));
      const int64_t result_idx = index(def, ai->get_column_id());
      if (request_idx < 0 || result_idx < 0 || request_idx >= row.count()) { continue; }
      // An implicit request assignment exists only for affected AI columns.
      // Compare expressions, since a constant assignment datum is reused by a
      // batch and cannot be used to decide whether the next row is affected.
      if (!inserting && request_idx < def.old_row_.count() &&
          row.at(request_idx) == def.old_row_.at(request_idx)) { continue; }
      ObString stable_request;
      if (OB_FAIL(ob_write_string(ctx.get_allocator(), ObString(request.size(), request.data()), stable_request))) { break; }
      row.at(request_idx)->locate_datum_for_write(op.get_eval_ctx()).set_string(stable_request);
      row.at(request_idx)->get_eval_info(op.get_eval_ctx()).evaluated_ = true;
      row.at(result_idx)->locate_datum_for_write(op.get_eval_ctx()).set_null();
      row.at(result_idx)->get_eval_info(op.get_eval_ctx()).evaluated_ = true;
      const int64_t spec_idx = index(def, ai->get_ai_column_number(4));
      if (spec_idx >= 0 && spec_idx < row.count()) {
        ObString stable_spec;
        const std::string version = std::to_string(ai->get_schema_version());
        if (OB_FAIL(ob_write_string(ctx.get_allocator(), ObString(version.size(), version.data()), stable_spec))) { break; }
        row.at(spec_idx)->locate_datum_for_write(op.get_eval_ctx()).set_string(stable_spec);
        row.at(spec_idx)->get_eval_info(op.get_eval_ctx()).evaluated_ = true;
      }
      const auto *state_col = table->get_column_schema(ai->get_ai_column_number(2));
      const auto *request_col = table->get_column_schema(ai->get_ai_column_number(3));
      if (state_col == nullptr || request_col == nullptr) { ret = OB_ERR_UNEXPECTED; break; }
      int64_t state_idx = index(def, state_col->get_column_id());
      if (state_idx >= 0 && state_idx < row.count()) {
        row.at(state_idx)->locate_datum_for_write(op.get_eval_ctx()).set_string(ObString("PENDING"));
        row.at(state_idx)->get_eval_info(op.get_eval_ctx()).evaluated_ = true;
      }
      std::string snapshot;
      for (int64_t d = 0; OB_SUCC(ret) && d < table->get_column_count(); ++d) {
        const auto *dep = table->get_column_schema_by_idx(d);
        if (dep->is_hidden() || dep->is_ai_column()) { continue; }
        const int64_t idx = index(def, dep->get_column_id());
        if (idx < 0 || idx >= row.count()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("AI column input missing from DML row", KR(ret), K(idx), K(row.count()), K(def.column_ids_));
          break;
        }
        std::string value;
        if (OB_FAIL(value_sql(row.at(idx), op, value))) { break; }
        if (!snapshot.empty()) { snapshot += ","; }
        snapshot += value + " AS " + AiColumnSQL::ident(dep->get_column_name_str());
      }
      if (OB_FAIL(ret)) { break; }
      const std::string expression = "SELECT " + AiColumnSQL::text(ai->get_ai_column_field(6)) +
        " AS result FROM (SELECT " + snapshot + ") AS ai_input";
      std::string type = ai->is_json() ? "JSON" : ai->is_collection() ?
        AiColumnSQL::text(ai->get_extended_type_info().at(0)) :
        ob_is_integer_type(ai->get_data_type()) ? (ob_is_unsigned_type(ai->get_data_type()) ? "UNSIGNED" : "SIGNED") :
        ob_is_float_type(ai->get_data_type()) ? "FLOAT" : ob_is_double_type(ai->get_data_type()) ? "DOUBLE" :
        ob_is_numeric_type(ai->get_data_type()) ? "DECIMAL(" +
          std::to_string(ai->get_data_precision() > 0 ? ai->get_data_precision() : 65) + "," +
          std::to_string(ai->get_data_scale() >= 0 ? ai->get_data_scale() : 30) + ")" :
          "CHAR(" + std::to_string(ai->get_data_length()) + ")";
      type = std::to_string(ai->get_data_type()) + "|" + type;
      std::string sql = "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=" +
        std::to_string(table->get_table_id()) + " AND column_id=" + std::to_string(ai->get_column_id()) +
        " AND rowkey_predicate=" + AiColumnSQL::literal(predicate);
      stage = "remove_obsolete_task";
      // A duplicate ignored INSERT must not cancel a generation belonging to
      // the existing row. The request guard discards the unused new task.
      if (!inserting && OB_FAIL(op.execute_write(sql.c_str()))) { break; }
      sql = "INSERT INTO oceanbase.__all_ai_column_task(table_id,request_id,column_id,schema_version,"
        "rowkey_predicate,database_name,table_name,column_name,expression,result_type,state_column_name,"
        "request_column_name,definition,task_state,attempt,max_retries,eligible_ts) VALUES(" +
        std::to_string(table->get_table_id()) + "," + AiColumnSQL::literal(request) + "," +
        std::to_string(ai->get_column_id()) + "," + std::to_string(ai->get_schema_version()) + "," +
        AiColumnSQL::literal(predicate) + "," + AiColumnSQL::literal(database->get_database_name_str()) + "," +
        AiColumnSQL::literal(table->get_table_name_str()) + "," + AiColumnSQL::literal(ai->get_column_name_str()) + "," +
        AiColumnSQL::literal(expression) + "," + AiColumnSQL::literal(type) + "," +
        AiColumnSQL::literal(state_col->get_column_name_str()) + "," +
        AiColumnSQL::literal(request_col->get_column_name_str()) + "," +
        AiColumnSQL::literal(ai->get_ai_column_info()) + ",0,0," +
        std::to_string(ai->get_ai_column_number(1)) + ",NOW(6))";
      stage = "insert_task";
      ret = op.execute_write(sql.c_str());
    }
    const int end_ret = op.end_nested_session();
    if (OB_SUCC(ret)) { ret = end_ret; }
    if (OB_FAIL(ret)) { LOG_ERROR("AI column transactional task preparation failed", KR(ret), K(end_ret), K(stage)); }
    return ret;
  }

private:
  static int64_t index(const ObDMLBaseCtDef &def, uint64_t id)
  {
    for (int64_t i = 0; i < def.column_ids_.count(); ++i) {
      if (def.column_ids_.at(i) == id) { return i; }
    }
    return -1;
  }
  static int value_sql(ObExpr *expr, ObTableModifyOp &op, std::string &out)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    ObDatum *datum = nullptr;
    ObObj obj;
    if (OB_FAIL(expr->eval(op.get_eval_ctx(), datum))) {}
    else if (OB_FAIL(datum->to_obj(obj, expr->obj_meta_))) {}
    else if (!obj.is_null() && obj.is_lob_storage()) {
      ObString data = datum->get_string();
      if (OB_FAIL(ObTextStringHelper::read_real_string_data(op.get_exec_ctx(), &op.get_exec_ctx().get_allocator(),
          expr->datum_meta_.type_, expr->datum_meta_.cs_type_, expr->obj_meta_.has_lob_header(), data))) {}
      else { out = share::AiColumnSQL::literal(data); }
    } else {
      char *buf = nullptr;
      int64_t size = 0, pos = 0;
      if (OB_FAIL(obj.print_sql_literal(buf, size, pos, op.get_exec_ctx().get_allocator(), op.get_obj_print_params()))) {}
      else { out.assign(buf, pos); }
    }
    return ret;
  }
  static int rowkey(const ObDMLBaseCtDef &def, const ExprFixedArray &row,
                    const share::schema::ObTableSchema &table, ObTableModifyOp &op, std::string &out)
  {
    int ret = common::OB_SUCCESS;
    for (int64_t i = 0; OB_SUCC(ret) && i < table.get_rowkey_info().get_size(); ++i) {
      uint64_t id = common::OB_INVALID_ID;
      if (OB_FAIL(table.get_rowkey_info().get_column_id(i, id))) { break; }
      const int64_t idx = index(def, id);
      const auto *col = table.get_column_schema(id);
      if (idx < 0 || idx >= row.count() || col == nullptr) { ret = common::OB_ERR_UNEXPECTED; break; }
      std::string value;
      if (OB_FAIL(value_sql(row.at(idx), op, value))) { break; }
      if (!out.empty()) { out += " AND "; }
      out += share::AiColumnSQL::ident(col->get_column_name_str()) + " = " + value;
    }
    return ret;
  }
};
} }
