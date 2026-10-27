#define USING_LOG_PREFIX SQL_RESV
#include <gtest/gtest.h>
#include "sql/resolver/ddl/ob_ai_column_ddl.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::share::schema;

static const ParseNode *find_node(const ParseNode *node, ObItemType type)
{
  if (node == nullptr || node->type_ == type) { return node; }
  for (int i = 0; i < node->num_child_; ++i) {
    const auto *found = find_node(node->children_[i], type);
    if (found != nullptr) { return found; }
  }
  return nullptr;
}

TEST(AiColumnResolver, AsyncOptionsAndDuplicateValidation)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result;
  ObColumnSchemaV2 column;
  column.set_data_type(ObJsonType);
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("CREATE TABLE t(v JSON AI COLUMN(AI_COMPLETE('m','x')) "
    "WITH(sync_mode='async',max_retries=0))"), result));
  ASSERT_EQ(OB_SUCCESS, AiColumnDDL::define(column, *find_node(result.result_tree_, T_COLUMN_DEFINITION)));
  ASSERT_EQ(0, column.get_ai_column_number(1));
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("CREATE TABLE t(v JSON AI COLUMN(AI_COMPLETE('m','x')) "
    "WITH(sync_mode='sync'))"), result));
  ASSERT_EQ(OB_NOT_SUPPORTED, AiColumnDDL::define(column, *find_node(result.result_tree_, T_COLUMN_DEFINITION)));
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("CREATE TABLE t(v JSON AI COLUMN(AI_COMPLETE('m','x')) "
    "WITH(max_retries=1,max_retries=2))"), result));
  ASSERT_EQ(OB_NOT_SUPPORTED, AiColumnDDL::define(column, *find_node(result.result_tree_, T_COLUMN_DEFINITION)));
}

TEST(AiColumnResolver, RejectsFunctionsAndMismatchedEmbeddingDimension)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result;
  ObColumnSchemaV2 column;
  column.set_data_type(ObVarcharType);
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("CREATE TABLE t(v TEXT AI COLUMN(CONCAT('x','y')))"), result));
  ASSERT_EQ(OB_NOT_SUPPORTED, AiColumnDDL::define(column, *find_node(result.result_tree_, T_COLUMN_DEFINITION)));
  column.set_data_type(ObCollectionSQLType);
  ASSERT_EQ(OB_SUCCESS, column.add_type_info(ObString("VECTOR(4)")));
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("CREATE TABLE t(v VECTOR(4) AI COLUMN(AI_EMBED('m','x',3)))"), result));
  ASSERT_EQ(OB_ERR_INVALID_VECTOR_DIM, AiColumnDDL::define(column, *find_node(result.result_tree_, T_COLUMN_DEFINITION)));
}

TEST(AiColumnResolver, TypedPromptKeepsInputAndUsesSeparateSystemContract)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("CREATE TABLE t(v JSON AI COLUMN(AI_COMPLETE('m',concat('a,b|',body))))"), result));
  const auto *definition = find_node(result.result_tree_, T_COLUMN_DEFINITION);
  ObTableSchema table;
  table.set_table_id(500000);
  ObColumnSchemaV2 body, output;
  body.set_table_id(500000);
  body.set_column_id(16);
  body.set_data_type(ObVarcharType);
  body.set_data_length(200);
  body.set_column_name("body");
  body.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ASSERT_EQ(OB_SUCCESS, table.add_column(body));
  output.set_column_id(17);
  output.set_data_type(ObJsonType);
  ASSERT_EQ(OB_SUCCESS, AiColumnDDL::define(output, *definition));
  ObArray<ObColumnSchemaV2> hidden;
  uint64_t next_id = 17;
  ASSERT_EQ(OB_SUCCESS, AiColumnDDL::finalize(output, definition->children_[4], table, next_id, hidden));
  ASSERT_EQ(3, hidden.count());
  std::string expression(output.get_ai_column_field(6).ptr(), output.get_ai_column_field(6).length());
  ASSERT_NE(std::string::npos, expression.find("concat('a,b|',body)"));
  ASSERT_NE(std::string::npos, expression.find("JSON_OBJECT('__seekdb_ai_column_contract', 'Return exactly one valid JSON value')"));
  ASSERT_EQ(ObString(",16,"), output.get_ai_column_field(5));
}
