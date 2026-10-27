#pragma once

#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "sql/parser/ob_parser.h"
#include "lib/string/ob_sql_string.h"
#include <string>
#include <vector>

namespace oceanbase { namespace sql {

// AI expressions are stored separately from DEFAULT and generated expressions.
// Neither DDL nor the business transaction evaluates an external AI function.
class AiColumnDDL
{
public:
  static common::ObString str(const ParseNode *node)
  {
    return node == nullptr ? common::ObString() :
      common::ObString(static_cast<int32_t>(node->str_len_), node->str_value_);
  }

  static int define(share::schema::ObColumnSchemaV2 &column, const ParseNode &node)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    int64_t retries = 3;
    bool seen_mode = false, seen_retries = false;
    const ParseNode *expr = node.children_[4];
    const ParseNode *options = node.children_[5];
    if (!column.is_collection() && !column.is_json() && !ob_is_string_type(column.get_data_type()) &&
        !ob_is_integer_type(column.get_data_type()) && !ob_is_numeric_type(column.get_data_type())) {
      ret = OB_NOT_SUPPORTED;
    } else if (expr == nullptr || expr->type_ != T_FUN_SYS || expr->num_child_ != 2 ||
        expr->children_[0] == nullptr || expr->children_[1] == nullptr) {
      ret = OB_NOT_SUPPORTED;
    } else {
      ObString name = str(expr->children_[0]);
      const bool embed = name.case_compare("AI_EMBED") == 0;
      const ParseNode *args = expr->children_[1];
      if ((!embed && name.case_compare("AI_COMPLETE") != 0) ||
          args->num_child_ < 2 || args->num_child_ > (embed ? 3 : 2) ||
          args->children_[0] == nullptr || args->children_[0]->type_ != T_VARCHAR ||
          str(args->children_[0]).empty() || embed != column.is_collection()) {
        ret = OB_NOT_SUPPORTED;
      } else if (embed && args->num_child_ == 3) {
        const ParseNode *dim = args->children_[2];
        // VECTOR(N) is resolved as an array collection with N in its type name.
        int64_t dimension = 0;
        if (column.get_extended_type_info().count() == 1) {
          ObString info = column.get_extended_type_info().at(0);
          const char *open = static_cast<const char *>(memchr(info.ptr(), '(', info.length()));
          if (open != nullptr) { dimension = strtoll(open + 1, nullptr, 10); }
        }
        if (dim == nullptr || dim->type_ != T_INT || dim->value_ <= 0 || dim->value_ != dimension) {
          ret = OB_ERR_INVALID_VECTOR_DIM;
          LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(dimension),
            static_cast<uint32_t>(dim == nullptr ? 0 : dim->value_));
        }
      }
    }
    for (int i = 0; OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
      const ParseNode *option = options->children_[i];
      if (option == nullptr || option->num_child_ != 2) { ret = OB_INVALID_ARGUMENT; }
      else {
        ObString key = str(option->children_[0]);
        const ParseNode *value = option->children_[1];
        if (key.case_compare("sync_mode") == 0 && !seen_mode && value != nullptr &&
            value->type_ == T_VARCHAR && str(value).case_compare("async") == 0) {
          seen_mode = true;
        } else if (key.case_compare("max_retries") == 0 && !seen_retries && value != nullptr &&
                   value->type_ == T_INT && value->value_ >= 0 && value->value_ <= INT32_MAX) {
          seen_retries = true;
          retries = value->value_;
        } else { ret = OB_NOT_SUPPORTED; }
      }
    }
    if (OB_SUCC(ret)) {
      ObSqlString metadata;
      if (OB_FAIL(metadata.append_fmt("1|%ld|0|0|0||%.*s", retries, str(expr).length(), str(expr).ptr()))) {
      } else { ret = column.set_ai_column_info(metadata.string()); }
    }
    if (ret == OB_NOT_SUPPORTED) { LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN definition"); }
    return ret;
  }

  static int dependencies(const ParseNode *node, const share::schema::ObTableSchema &table,
                          common::ObSqlString &ids, int depth = 0)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    if (depth > 128) { ret = OB_SIZE_OVERFLOW; }
    else if (node == nullptr) {}
    else if (node->type_ == T_COLUMN_REF) {
      const ParseNode *name = node->children_[node->num_child_ - 1];
      const auto *dep = table.get_column_schema(str(name));
      if (dep == nullptr) { ret = OB_ERR_BAD_FIELD_ERROR; }
      else if (dep->is_ai_column() || dep->is_generated_column() || dep->is_hidden()) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("AI COLUMN dependency on an AI, generated or hidden column", KR(ret));
      } else { ret = ids.append_fmt(",%lu,", dep->get_column_id()); }
    } else if (node->type_ == T_SELECT || node->type_ == T_QUESTIONMARK ||
               node->type_ == T_USER_VARIABLE_IDENTIFIER || node->type_ == T_SYSTEM_VARIABLE) {
      ret = OB_NOT_SUPPORTED;
    } else {
      if (node->type_ == T_FUN_SYS && depth > 0 && node->num_child_ > 0) {
        ObString function = str(node->children_[0]);
        const char *allowed[] = {"CONCAT", "CONCAT_WS", "COALESCE", "IFNULL", "NULLIF", "LOWER", "UPPER",
          "LCASE", "UCASE", "TRIM", "LTRIM", "RTRIM", "SUBSTR", "SUBSTRING", "REPLACE", "LEFT", "RIGHT",
          "JSON_OBJECT", "JSON_ARRAY", "JSON_EXTRACT", "JSON_UNQUOTE", "AI_PROMPT"};
        bool valid = false;
        for (const char *name : allowed) { valid |= function.case_compare(name) == 0; }
        if (!valid) { ret = OB_NOT_SUPPORTED; }
      }
      for (int i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
        ret = dependencies(node->children_[i], table, ids, depth + 1);
      }
    }
    return ret;
  }

  static int finalize(share::schema::ObColumnSchemaV2 &column, const ParseNode *expr,
                      share::schema::ObTableSchema &table, uint64_t &next_id,
                      common::ObIArray<share::schema::ObColumnSchemaV2> &hidden)
  {
    using namespace common;
    using namespace share::schema;
    int ret = OB_SUCCESS;
    ObSqlString ids, metadata;
    if (OB_FAIL(dependencies(expr, table, ids))) { return ret; }
    const uint64_t state_id = ++next_id, request_id = ++next_id, spec_id = ++next_id;
    ObString expression = column.get_ai_column_field(6);
    // Preserve the prompt verbatim; pass its result contract in a separate config.
    std::string normalized(expression.ptr(), expression.length());
    const char *hint = column.is_json() ? "Return exactly one valid JSON value" :
      (ob_is_integer_type(column.get_data_type()) ? "Return exactly one base-10 integer" :
       (ob_is_numeric_type(column.get_data_type()) ? "Return exactly one number" : nullptr));
    if (hint != nullptr) {
      size_t comma = find_argument_separator(normalized);
      if (comma == std::string::npos || normalized.empty()) { return OB_INVALID_ARGUMENT; }
      normalized.insert(normalized.size() - 1,
        std::string(", JSON_OBJECT('__seekdb_ai_column_contract', '") + hint + "')");
    }
    if (OB_FAIL(metadata.append_fmt("1|%ld|%lu|%lu|%lu|%.*s|%s",
        column.get_ai_column_number(1), state_id, request_id, spec_id,
        ids.length(), ids.ptr(), normalized.c_str()))) {
    } else if (OB_FAIL(column.set_ai_column_info(metadata.string()))) {}
    const char *kinds[] = {"state", "request", "spec"};
    const uint64_t col_ids[] = {state_id, request_id, spec_id};
    for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
      ObColumnSchemaV2 c;
      ObSqlString name;
      if (OB_FAIL(name.append_fmt("__ai_%s_%lu", kinds[i], column.get_column_id()))) {}
      else if (OB_FAIL(c.set_column_name(name.string()))) {}
      else {
        c.set_column_id(col_ids[i]);
        c.set_table_id(table.get_table_id());
        c.set_is_hidden(true);
        c.set_data_type(ObVarcharType);
        c.set_data_length(i == 2 ? 4096 : 64);
        c.set_charset_type(CHARSET_UTF8MB4);
        c.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        c.set_nullable(true);
        ObObj value;
        if (i == 0) { value.set_varchar("PENDING"); }
        value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        if (OB_FAIL(c.set_orig_default_value(value))) {}
        else if (OB_FAIL(c.set_cur_default_value(value, false))) {}
        else { ret = hidden.push_back(c); }
      }
    }
    return ret;
  }

private:
  static size_t find_argument_separator(const std::string &text)
  {
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      char c = text[i];
      if (quote != 0) {
        if (c == '\\') { ++i; }
        else if (c == quote) {
          if (i + 1 < text.size() && text[i + 1] == quote) { ++i; }
          else { quote = 0; }
        }
      } else if (c == '\'' || c == '"' || c == '`') { quote = c; }
      else if (c == '(') { ++depth; }
      else if (c == ')') { --depth; }
      else if (c == ',' && depth == 1) { return i; }
    }
    return std::string::npos;
  }
};
} }
