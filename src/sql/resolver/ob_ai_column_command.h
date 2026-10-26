#pragma once

#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/parser/ob_parser.h"
#include "share/ai_service/ob_ai_column_sql.h"

namespace oceanbase { namespace sql {
class AiColumnCommand
{
  using SQL = share::AiColumnSQL;
  static common::ObString name(const ParseNode *node)
  { return node == nullptr ? common::ObString() : common::ObString(node->str_len_, node->str_value_); }
  static int replace_status(ParseNode *node, const common::ObString &state, common::ObIAllocator &alloc)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    if (node == nullptr) {}
    else if (node->type_ == T_COLUMN_REF && node->num_child_ == 3) {
      auto *leaf = node->children_[2];
      if (leaf != nullptr && !leaf->is_input_quoted_ && name(leaf).case_compare("status") == 0) {
        ObString stable;
        if (OB_FAIL(ob_write_string(alloc, state, stable))) {}
        else { leaf->str_value_ = stable.ptr(); leaf->str_len_ = stable.length(); }
      }
    } else {
      for (int i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
        ret = replace_status(node->children_[i], state, alloc);
      }
    }
    return ret;
  }
public:
  static int rewrite(ObResolverParams &params, const ParseNode &node, const ParseNode *&output)
  {
    using namespace common;
    using namespace share::schema;
    int ret = OB_SUCCESS;
    const bool retry = node.type_ == T_AI_COLUMN_RETRY;
    const ParseNode *relation = node.children_[0];
    if ((retry && node.num_child_ != 3) || relation == nullptr || relation->num_child_ < 2 ||
        (relation->num_child_ > 2 && relation->children_[2] != nullptr)) {
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
      return OB_NOT_SUPPORTED;
    }
    ObString database = name(relation->children_[0]);
    ObString table_name = name(relation->children_[1]);
    if (database.empty()) { database = params.session_info_->get_database_name(); }
    const ObTableSchema *table = nullptr;
    if (OB_FAIL(params.schema_checker_->get_table_schema(database, table_name, false, table))) { return ret; }
    if (table == nullptr) { return OB_TABLE_NOT_EXIST; }
    std::string sql;
    const ObColumnSchemaV2 *state = nullptr;
    if (retry) {
      const auto *column = table->get_column_schema(name(node.children_[1]));
      if (column == nullptr || !column->is_ai_column()) {
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
        return OB_NOT_SUPPORTED;
      }
      state = table->get_column_schema(column->get_ai_column_number(2));
      if (state == nullptr) { return OB_SCHEMA_ERROR; }
      sql = "UPDATE " + SQL::ident(database) + "." + SQL::ident(table_name) +
        " SET " + SQL::ident(column->get_column_name_str()) + "=NULL";
      if (node.children_[2] != nullptr) { sql += " WHERE " + SQL::text(name(node.children_[2])); }
      params.query_ctx_->is_ai_column_retry_ = true;
    } else {
      sql = "SELECT column_name,normalized_expr,sync_mode,max_retries,dependencies,definition_version "
        "FROM oceanbase.__all_ai_column WHERE table_id=" + std::to_string(table->get_table_id()) + " ORDER BY column_id";
    }
    ObString stable_sql;
    if (OB_FAIL(ob_write_string(*params.allocator_, ObString(sql.size(), sql.data()), stable_sql))) { return ret; }
    ObParser parser(*params.allocator_, params.session_info_->get_sql_mode(), params.session_info_->get_charsets4parser());
    ParseResult result;
    if (OB_FAIL(parser.parse(stable_sql, result))) { return ret; }
    if (result.result_tree_ == nullptr || result.result_tree_->num_child_ != 1) { return OB_ERR_UNEXPECTED; }
    ParseNode *rewritten = result.result_tree_->children_[0];
    if (retry) { ret = replace_status(rewritten->children_[3], state->get_column_name_str(), *params.allocator_); }
    output = rewritten;
    return ret;
  }
};
} }
