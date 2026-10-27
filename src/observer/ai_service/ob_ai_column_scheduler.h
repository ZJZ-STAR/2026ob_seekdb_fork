#pragma once

#include "share/ob_thread_pool.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/ai_service/ob_ai_column_sql.h"
#include "share/json/ob_json_helper.h"
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <limits>

namespace oceanbase { namespace observer {

class AiColumnScheduler : public share::ObThreadPool
{
  struct Task
  {
    int64_t table_id = 0, column_id = 0, version = 0, attempt = 0, max_retries = 0, state = 0, data_type = 0;
    std::string request, predicate, database, table, column, expression, type, state_column,
      request_column, definition, value;
  };
  using SQL = share::AiColumnSQL;
public:
  static AiColumnScheduler &instance() { static AiColumnScheduler scheduler; return scheduler; }
  int start_service() { set_thread_count(4); return start(); }
  void run1() override
  {
    lib::set_thread_name("AIColumn");
    while (!has_set_stop()) {
      if (GCTX.sql_proxy_ == nullptr || GCTX.in_bootstrap_ || GCTX.start_service_time_ <= 0) {
        usleep(100000);
        continue;
      }
      if (get_thread_idx() == 0 && common::ObTimeUtility::current_time() >= next_reconcile_) {
        reconcile();
        next_reconcile_ = common::ObTimeUtility::current_time() + 1000000;
      }
      const int ret = tick();
      if (ret != common::OB_SUCCESS && ret != common::OB_ITER_END) {
        LOG_WARN("AI column scheduling iteration failed", KR(ret));
      }
      usleep(ret == common::OB_SUCCESS ? 10000 : 20000);
    }
  }

private:
  int64_t next_reconcile_ = 0;
  std::map<std::pair<int64_t, int64_t>, int64_t> reconciled_versions_;
  int reconcile()
  {
    using namespace common;
    int ret = OB_SUCCESS;
    struct Backfill { std::pair<int64_t, int64_t> key; int64_t version; std::string sql; };
    std::vector<Backfill> updates;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      const char *sql = "SELECT d.database_name,t.table_name,a.column_name,c.column_name AS spec_name,"
        "a.table_id,a.column_id,r.schema_version FROM oceanbase.__all_ai_column a JOIN oceanbase.__all_table t ON t.table_id=a.table_id "
        "JOIN oceanbase.__all_database d ON d.database_id=t.database_id JOIN oceanbase.__all_column c "
        "ON c.table_id=a.table_id AND c.column_id=a.result_spec_column_id JOIN oceanbase.__all_column r "
        "ON r.table_id=a.table_id AND r.column_id=a.column_id";
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql))) {}
      else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        auto &r = *res.get_result();
        while (OB_SUCC(ret) && (ret = r.next()) == OB_SUCCESS) {
          ObString database, table, column, spec;
          int64_t table_id = 0, column_id = 0, version = 0;
          OZ(r.get_varchar("database_name", database));
          OZ(r.get_varchar("table_name", table));
          OZ(r.get_varchar("column_name", column));
          OZ(r.get_varchar("spec_name", spec));
          OZ(r.get_int("table_id", table_id));
          OZ(r.get_int("column_id", column_id));
          OZ(r.get_int("schema_version", version));
          if (OB_SUCC(ret)) {
            const auto key = std::make_pair(table_id, column_id);
            auto known = reconciled_versions_.find(key);
            if (known != reconciled_versions_.end() && known->second == version) { continue; }
            updates.push_back({key, version, "ALTER TABLE " + SQL::ident(database) + "." + SQL::ident(table) +
              " RETRY AI COLUMN " + SQL::ident(column) + " WHERE " + SQL::ident(spec) + " IS NULL OR " +
              SQL::ident(spec) + "<>" + SQL::literal(std::to_string(version))});
          }
        }
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      }
    }
    for (const auto &update : updates) {
      int64_t affected = 0;
      int update_ret = GCTX.sql_proxy_->write(update.sql.c_str(), affected);
      if (update_ret != OB_SUCCESS) { ret = update_ret; }
      else { reconciled_versions_[update.key] = update.version; }
    }
    return ret;
  }
  static int extract(common::sqlclient::ObMySQLResult &r, Task &t)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    OZ(r.get_int("table_id", t.table_id));
    OZ(r.get_int("column_id", t.column_id));
    OZ(r.get_int("schema_version", t.version));
    OZ(r.get_int("attempt", t.attempt));
    OZ(r.get_int("max_retries", t.max_retries));
    OZ(r.get_int("task_state", t.state));
    const char *names[] = {"request_id", "rowkey_predicate", "database_name", "table_name", "column_name",
      "expression", "result_type", "state_column_name", "request_column_name", "definition", "result_value"};
    std::string *values[] = {&t.request, &t.predicate, &t.database, &t.table, &t.column,
      &t.expression, &t.type, &t.state_column, &t.request_column, &t.definition, &t.value};
    for (size_t i = 0; OB_SUCC(ret) && i < sizeof(names) / sizeof(names[0]); ++i) {
      ObString value;
      int field_ret = r.get_varchar(names[i], value);
      if (field_ret == OB_ERR_NULL_VALUE && i == 10) { field_ret = OB_SUCCESS; }
      if (field_ret != OB_SUCCESS) { ret = field_ret; }
      else { *values[i] = SQL::text(value); }
    }
    if (OB_SUCC(ret)) {
      auto split = t.type.find('|');
      if (split == std::string::npos) { ret = OB_INVALID_DATA; }
      else { t.data_type = strtoll(t.type.c_str(), nullptr, 10); t.type.erase(0, split + 1); }
    }
    return ret;
  }
  static std::string key(const Task &t)
  {
    return "table_id=" + std::to_string(t.table_id) + " AND request_id=" + SQL::literal(t.request) +
      " AND column_id=" + std::to_string(t.column_id);
  }
  static std::string group_key(const Task &t)
  { return "table_id=" + std::to_string(t.table_id) + " AND request_id=" + SQL::literal(t.request); }
  static std::string qualified(const Task &t) { return SQL::ident(t.database) + "." + SQL::ident(t.table); }
  static int scalar(common::ObISQLClient &client, const std::string &sql, int64_t &value)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(client.read(res, sql.c_str()))) {}
      else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(res.get_result()->next())) {}
      else { ret = res.get_result()->get_int(0L, value); }
    }
    return ret;
  }
  static std::string metadata_guard(const Task &t)
  {
    return "EXISTS(SELECT 1 FROM oceanbase.__all_column c JOIN oceanbase.__all_table tb "
      "ON c.table_id=tb.table_id WHERE c.table_id=" + std::to_string(t.table_id) +
      " AND c.column_id=" + std::to_string(t.column_id) + " AND c.schema_version=" + std::to_string(t.version) +
      " AND c.ai_column_info=" + SQL::literal(t.definition) + ")";
  }
  static int current(const Task &t, bool &valid)
  {
    using namespace common;
    int64_t count = 0;
    int ret = scalar(*GCTX.sql_proxy_, "SELECT COUNT(*) FROM " + qualified(t) + " WHERE (" +
      t.predicate + ") AND " + SQL::ident(t.request_column) + "=" + SQL::literal(t.request) +
      " AND " + metadata_guard(t), count);
    valid = OB_SUCC(ret) && count == 1;
    return ret;
  }
  static int validate(const Task &t, const std::string &value)
  {
    using namespace common;
    if (t.data_type == ObCollectionSQLType) {
      ObArenaAllocator allocator;
      ObJsonNode *root = nullptr;
      int ret = ObJsonParser::get_tree(&allocator, ObString(value.size(), value.data()), root);
      const auto open = t.type.find('(');
      const int64_t dimension = open == std::string::npos ? 0 : strtoll(t.type.c_str() + open + 1, nullptr, 10);
      if (OB_SUCC(ret) && (root == nullptr || root->json_type() != ObJsonNodeType::J_ARRAY ||
          dimension <= 0 || root->element_count() != static_cast<uint64_t>(dimension))) { ret = OB_INVALID_DATA; }
      share::ObJsonReaderHelper reader(allocator);
      for (int64_t i = 0; OB_SUCC(ret) && i < dimension; ++i) {
        ObIJsonBase *element = nullptr;
        float number = 0;
        if (OB_FAIL(root->get_array_element(i, element))) {}
        else if (element == nullptr || !share::ObJsonHelper::is_number_type(element)) { ret = OB_INVALID_DATA; }
        else if (OB_FAIL(reader.get_float_value(element, number))) {}
        else if (!std::isfinite(number)) { ret = OB_INVALID_DATA; }
      }
      return ret;
    }
    // Reject conversions which MySQL would otherwise silently coerce to 0/truncate.
    if (t.type == "SIGNED" || t.type == "UNSIGNED") {
      if (value.empty() || value.find_first_not_of("+-0123456789") != std::string::npos ||
          (value.find_first_of("+-", 1) != std::string::npos) ||
          (t.type == "UNSIGNED" && value[0] == '-')) { return OB_INVALID_DATA; }
      errno = 0;
      char *end = nullptr;
      uint64_t u = 0;
      int64_t s = 0;
      if (t.type == "UNSIGNED") { u = strtoull(value.c_str(), &end, 10); }
      else { s = strtoll(value.c_str(), &end, 10); }
      if (errno != 0 || end == value.c_str() || *end != 0) { return OB_INVALID_DATA; }
      int bits = t.data_type == ObTinyIntType || t.data_type == ObUTinyIntType ? 8 :
        t.data_type == ObSmallIntType || t.data_type == ObUSmallIntType ? 16 :
        t.data_type == ObMediumIntType || t.data_type == ObUMediumIntType ? 24 :
        t.data_type == ObInt32Type || t.data_type == ObUInt32Type ? 32 : 64;
      if (bits < 64 && (t.type == "UNSIGNED" ? u >= (1ULL << bits) :
                       s < -(1LL << (bits - 1)) || s >= (1LL << (bits - 1)))) { return OB_INVALID_DATA; }
    } else if (t.type.compare(0, 7, "DECIMAL") == 0 || t.type == "FLOAT" || t.type == "DOUBLE") {
      size_t pos = !value.empty() && (value[0] == '+' || value[0] == '-') ? 1 : 0;
      size_t digits = 0;
      while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') { ++pos; ++digits; }
      if (pos < value.size() && value[pos] == '.') {
        ++pos;
        while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') { ++pos; ++digits; }
      }
      if (digits == 0) { return OB_INVALID_DATA; }
      if (pos < value.size() && (value[pos] == 'e' || value[pos] == 'E')) {
        ++pos;
        if (pos < value.size() && (value[pos] == '+' || value[pos] == '-')) { ++pos; }
        const size_t exponent = pos;
        while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') { ++pos; }
        if (pos == exponent) { return OB_INVALID_DATA; }
      }
      if (pos != value.size()) { return OB_INVALID_DATA; }
      errno = 0;
      char *end = nullptr;
      long double number = strtold(value.c_str(), &end);
      if (errno != 0 || end == value.c_str() || *end != 0 || !std::isfinite(number)) { return OB_INVALID_DATA; }
      if (ob_is_unsigned_type(static_cast<ObObjType>(t.data_type)) && number < 0) { return OB_INVALID_DATA; }
      if (t.type == "FLOAT" && std::fabs(number) > std::numeric_limits<float>::max()) { return OB_INVALID_DATA; }
      if (t.type == "DOUBLE" && std::fabs(number) > std::numeric_limits<double>::max()) { return OB_INVALID_DATA; }
      if (t.type.compare(0, 7, "DECIMAL") == 0) {
        char *comma = nullptr;
        const int64_t precision = strtoll(t.type.c_str() + 8, &comma, 10);
        const int64_t scale = comma == nullptr || *comma != ',' ? -1 : strtoll(comma + 1, nullptr, 10);
        if (precision <= 0 || scale < 0 || scale > precision) { return OB_INVALID_DATA; }
        const long double unit = std::pow(10.L, -scale);
        const long double limit = std::pow(10.L, precision - scale);
        if (std::fabs(number) >= limit || std::round(std::fabs(number) / unit) * unit >= limit) {
          return OB_INVALID_DATA;
        }
      }
    }
    int64_t valid = 0;
    std::string sql;
    if (t.type == "JSON") { sql = "SELECT JSON_VALID(" + SQL::literal(value) + ")"; }
    else if (t.type.compare(0, 5, "CHAR(") == 0) {
      sql = "SELECT CHAR_LENGTH(" + SQL::literal(value) + ") <= " + t.type.substr(5, t.type.size() - 6);
    } else {
      sql = "SELECT CAST(" + SQL::literal(value) + " AS " + t.type + ") IS NOT NULL";
    }
    int ret = scalar(*GCTX.sql_proxy_, sql, valid);
    return OB_SUCC(ret) && valid != 1 ? OB_INVALID_DATA : ret;
  }
  static int execute_model(const Task &t, std::string &value)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      const std::string sql = "SELECT CAST(ai_output.result AS CHAR) AS result FROM (" + t.expression + ") ai_output";
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.c_str()))) {}
      else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(res.get_result()->next())) {}
      else {
        ObString result;
        if (OB_FAIL(res.get_result()->get_varchar(0L, result))) {}
        else { value = SQL::text(result); }
      }
    }
    if (OB_SUCC(ret)) { ret = validate(t, value); }
    return ret;
  }
  static int publish(const Task &group)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    ObMySQLTransaction tx;
    std::vector<Task> tasks;
    if (OB_FAIL(tx.start(GCTX.sql_proxy_))) { return ret; }
    {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        const std::string sql = "SELECT * FROM oceanbase.__all_ai_column_task WHERE " +
          group_key(group) + " ORDER BY column_id FOR UPDATE";
        if (OB_FAIL(tx.read(res, sql.c_str()))) {}
        else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else {
          auto &r = *res.get_result();
          while (OB_SUCC(ret) && (ret = r.next()) == OB_SUCCESS) {
            Task task;
            if (OB_FAIL(extract(r, task))) {}
            else { tasks.push_back(task); }
          }
          if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        }
      }
    }
    bool terminal = !tasks.empty();
    for (const auto &t : tasks) { terminal &= t.state == 2 || t.state == 3; }
    if (OB_SUCC(ret) && terminal) {
      bool definitions_exist = true;
      for (const auto &t : tasks) {
        int64_t exists = 0;
        if (OB_FAIL(scalar(tx, "SELECT " + metadata_guard(t), exists))) { break; }
        definitions_exist &= exists == 1;
      }
      std::string set_clause, predicate = "(" + group.predicate + ")";
      for (const auto &t : tasks) {
        if (!set_clause.empty()) { set_clause += ","; }
        set_clause += SQL::ident(t.column) + "=" +
          (t.state == 2 ? (t.data_type == ObCollectionSQLType ? SQL::literal(t.value) :
            "CAST(" + SQL::literal(t.value) + " AS " + t.type + ")") : "NULL") + "," +
          SQL::ident(t.state_column) + "=" + SQL::literal(std::string(t.state == 2 ? "READY" : "FAILED"));
        predicate += " AND " + SQL::ident(t.request_column) + "=" + SQL::literal(t.request) +
          " AND " + metadata_guard(t);
      }
      const std::string update = "UPDATE " + qualified(group) + " SET " + set_clause + " WHERE " + predicate;
      int64_t affected = 0;
      if (OB_SUCC(ret) && definitions_exist) { ret = tx.write(update.c_str(), affected); }
      if (OB_SUCC(ret)) {
        const std::string cleanup = "DELETE FROM oceanbase.__all_ai_column_task WHERE " + group_key(group);
        ret = tx.write(cleanup.c_str(), affected);
      }
    }
    const int end_ret = tx.end(OB_SUCC(ret));
    return OB_SUCC(ret) ? end_ret : ret;
  }
  int tick()
  {
    using namespace common;
    int ret = OB_SUCCESS;
    Task task;
    bool found = false;
    {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        const char *sql = "SELECT * FROM oceanbase.__all_ai_column_task WHERE "
          "(task_state=0 AND eligible_ts<=NOW(6)) OR (task_state=1 AND lease_until<NOW(6)) "
          "OR (task_state IN(2,3) AND NOT EXISTS(SELECT 1 FROM oceanbase.__all_ai_column_task g "
          "WHERE g.table_id=__all_ai_column_task.table_id AND g.request_id=__all_ai_column_task.request_id "
          "AND g.task_state IN(0,1))) ORDER BY eligible_ts LIMIT 1";
        if (OB_FAIL(GCTX.sql_proxy_->read(res, sql))) {}
        else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(res.get_result()->next())) {}
        else if (OB_FAIL(extract(*res.get_result(), task))) {}
        else { found = true; }
      }
    }
    if (OB_FAIL(ret) || !found) { return ret; }
    if (task.state == 2 || task.state == 3) { return publish(task); }
    const std::string owner = std::to_string(ObTimeUtility::current_time()) + "-" + std::to_string(get_thread_idx());
    std::string sql = "UPDATE oceanbase.__all_ai_column_task SET task_state=1,owner=" + SQL::literal(owner) +
      ",lease_until=DATE_ADD(NOW(6),INTERVAL 60 SECOND) WHERE " + key(task) +
      " AND ((task_state=0 AND eligible_ts<=NOW(6)) OR (task_state=1 AND lease_until<NOW(6)))";
    int64_t affected = 0;
    if (OB_FAIL(GCTX.sql_proxy_->write(sql.c_str(), affected))) { return ret; }
    // Another worker claimed this candidate. Retry promptly so the remaining
    // workers can reach different queued rows while its model call is in flight.
    if (affected == 0) { return OB_SUCCESS; }
    bool valid = false;
    ret = current(task, valid);
    if (ret != OB_SUCCESS && ret != OB_TABLE_NOT_EXIST && ret != OB_ERR_BAD_FIELD_ERROR) {
      const int read_ret = ret;
      sql = "UPDATE oceanbase.__all_ai_column_task SET task_state=0,owner=NULL,"
        "eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND) WHERE " + key(task) + " AND owner=" + SQL::literal(owner);
      const int reset_ret = GCTX.sql_proxy_->write(sql.c_str(), affected);
      return reset_ret == OB_SUCCESS ? read_ret : reset_ret;
    }
    if (OB_FAIL(ret) || !valid) {
      sql = "DELETE FROM oceanbase.__all_ai_column_task WHERE " + key(task) + " AND owner=" + SQL::literal(owner);
      return GCTX.sql_proxy_->write(sql.c_str(), affected);
    }
    std::string value;
    const int model_ret = execute_model(task, value);
    const bool retry = model_ret != OB_SUCCESS && task.attempt < task.max_retries;
    sql = "UPDATE oceanbase.__all_ai_column_task SET task_state=" + std::to_string(retry ? 0 : model_ret == OB_SUCCESS ? 2 : 3) +
      ",state=" + std::to_string(retry ? 3 : model_ret == OB_SUCCESS ? 2 : 4) +
      ",retry_count=" + std::to_string(model_ret == OB_SUCCESS ? task.attempt : task.attempt + 1) +
      ",attempt=attempt+1,last_error=" + std::to_string(model_ret) +
      ",result_value=" + SQL::literal(value) + ",eligible_ts=DATE_ADD(NOW(6),INTERVAL " +
      std::to_string(std::min<int64_t>(10, 1LL << std::min<int64_t>(task.attempt, 4))) + " SECOND) WHERE " +
      key(task) + " AND task_state=1 AND owner=" + SQL::literal(owner);
    if (OB_FAIL(GCTX.sql_proxy_->write(sql.c_str(), affected))) { return ret; }
    return affected == 0 || retry ? OB_SUCCESS : publish(task);
  }
};
} }
