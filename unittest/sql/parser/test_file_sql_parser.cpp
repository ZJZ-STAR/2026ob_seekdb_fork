#include "sql/parser/ob_parser.h"
#include <gtest/gtest.h>
#include <vector>
using namespace oceanbase::common;
using namespace oceanbase::sql;

TEST(FileSqlParser, ReadFunctionsAliasesNamedOptionsDescribeAndCopy) {
  ObArenaAllocator alloc;
  const char *queries[] = {
      "select * from read_csv('/tmp/a.csv')",
      "select * from read_csv('/tmp/a.csv') f",
      "select * from read_csv('/tmp/a.csv',header=true,sample_size=-1) as f",
      "select * from read_json('/tmp/a.jsonl',format='newline_delimited') j",
      "select * from read_ndjson('/tmp/a.jsonl')",
      "select * from read_parquet('/tmp/a.parquet') p",
      "select * from glob('/tmp/*.csv')",
      "describe select * from read_csv('/tmp/a.csv')",
      "explain select * from read_csv('/tmp/a.csv')",
      "select * from read_csv('/tmp/a.csv') a left join "
      "read_json('/tmp/b.jsonl') b on a.id=b.id",
      "copy (select * from read_csv('/tmp/a.csv')) to '/tmp/out.csv' (format "
      "csv,header,nullstr 'NULL')",
      "copy test_table to '/tmp/out.csv'",
      "copy test_table (a,b) to '/tmp/out.csv' with (header true,delimiter "
      "'|',quote '\"',escape '\"')"};
  for (const char *q : queries) {
    ObParser parser(alloc, SMO_DEFAULT);
    ParseResult result;
    int ret = parser.parse(ObString(q), result);
    EXPECT_EQ(OB_SUCCESS, ret)
        << q << " error=" << (result.error_msg_ ? result.error_msg_ : "");
    if (ret == OB_SUCCESS) {
      ASSERT_NE(nullptr, result.result_tree_);
      std::vector<const ParseNode *> todo{result.result_tree_};
      while (!todo.empty()) {
        const ParseNode *node = todo.back();
        todo.pop_back();
        if (node->type_ == T_FILE_TABLE_EXPRESSION) {
          ASSERT_EQ(3, node->num_child_);
          ASSERT_NE(nullptr, node->children_[1]);
          const ParseNode *args = node->children_[1];
          ASSERT_EQ(T_EXPR_LIST, args->type_);
          ASSERT_GT(args->num_child_, 0);
          EXPECT_EQ(T_VARCHAR,
                    args->children_[0]->type_); // no nested expr-list wrapper
        }
        for (int i = 0; i < node->num_child_; ++i)
          if (node->children_[i])
            todo.push_back(node->children_[i]);
      }
    }
  }
}
TEST(FileSqlParser, OrdinarySqlAndMalformedFileSyntax) {
  ObArenaAllocator alloc;
  for (const char *q :
       {"select 1+2", "select read_csv from t", "describe t",
        "explain select 1", "describe select 1",
        "select * from json_table('[1]','$[*]' columns (v int path '$')) j"}) {
    ObParser parser(alloc, SMO_DEFAULT);
    ParseResult result;
    EXPECT_EQ(OB_SUCCESS, parser.parse(ObString(q), result)) << q;
  }
  for (const char *q :
       {"select * from read_csv()", "select * from read_parquet('/tmp/a',)",
        "copy (select 1) '/tmp/a'"}) {
    ObParser parser(alloc, SMO_DEFAULT);
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString(q), result)) << q;
  }
}
