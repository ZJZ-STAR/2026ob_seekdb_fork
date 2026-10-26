#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
#include "lib/allocator/ob_allocator.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

TEST(AiColumnParser, CreateAndAlterUseDistinctAiDefinitions)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("CREATE TABLE docs(id BIGINT PRIMARY KEY, content TEXT, "
    "result JSON AI COLUMN(AI_COMPLETE('model', content)) WITH(sync_mode='async',max_retries=2))"), result));
  ASSERT_EQ(T_CREATE_TABLE, result.result_tree_->children_[0]->type_);
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("ALTER TABLE docs ADD COLUMN emb VECTOR(4) "
    "AI COLUMN(AI_EMBED('embed',content))"), result));
  ASSERT_EQ(T_ALTER_TABLE, result.result_tree_->children_[0]->type_);
}

TEST(AiColumnParser, RetryPreservesQuotedStatusAndPredicate)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("ALTER TABLE docs RETRY AI COLUMN result "
    "WHERE status='FAILED' AND `status`='active' AND id IN(1,3)"), result));
  const auto *retry = result.result_tree_->children_[0];
  ASSERT_EQ(T_AI_COLUMN_RETRY, retry->type_);
  ASSERT_EQ(3, retry->num_child_);
  const auto *predicate = retry->children_[2];
  ASSERT_EQ(ObString("status='FAILED' AND `status`='active' AND id IN(1,3)"),
    ObString(predicate->str_len_, predicate->str_value_));
}

TEST(AiColumnParser, ShowAndIllegalMixedRetry)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("SHOW AI COLUMNS FROM docs"), result));
  ASSERT_EQ(T_SHOW_AI_COLUMNS, result.result_tree_->children_[0]->type_);
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString("ALTER TABLE docs RETRY AI COLUMN result WHERE id=1, ADD COLUMN x INT"), result));
  ASSERT_EQ(T_AI_COLUMN_RETRY, result.result_tree_->children_[0]->type_);
  ASSERT_EQ(4, result.result_tree_->children_[0]->num_child_);
}
