#include <gtest/gtest.h>
#include "share/schema/ob_column_schema.h"
#include "lib/allocator/ob_allocator.h"
#include <vector>

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

TEST(AiColumnSchema, CopyOwnsExpressionStorage)
{
  ObColumnSchemaV2 original, copy;
  original.set_column_id(18);
  ASSERT_EQ(OB_SUCCESS, original.set_ai_column_info(ObString("1|2|19|20|21|,17,|AI_COMPLETE('model', concat(content, '|'))")));
  ASSERT_EQ(OB_SUCCESS, copy.assign(original));
  ASSERT_TRUE(copy.is_ai_column());
  ASSERT_EQ(original.get_ai_column_info(), copy.get_ai_column_info());
  ASSERT_NE(original.get_ai_column_info().ptr(), copy.get_ai_column_info().ptr());
  ASSERT_EQ(ObString("AI_COMPLETE('model', concat(content, '|'))"), copy.get_ai_column_field(6));
  original.reset();
  ASSERT_EQ(2, copy.get_ai_column_number(1));
  ASSERT_EQ(19, copy.get_ai_column_number(2));
}

TEST(AiColumnSchema, SerializationPreservesConfiguration)
{
  ObColumnSchemaV2 original, restored;
  ASSERT_EQ(OB_SUCCESS, original.set_ai_column_info(ObString("1|0|19|20|21|,17,|AI_EMBED('model', body)")));
  std::vector<char> bytes(original.get_serialize_size());
  int64_t written = 0, read = 0;
  ASSERT_EQ(OB_SUCCESS, original.serialize(bytes.data(), bytes.size(), written));
  ASSERT_EQ(OB_SUCCESS, restored.deserialize(bytes.data(), written, read));
  ASSERT_EQ(written, read);
  ASSERT_EQ(original.get_ai_column_info(), restored.get_ai_column_info());
  ASSERT_EQ(0, restored.get_ai_column_number(1));
}

TEST(AiColumnSchema, RemapsHiddenAndDependencyColumnIds)
{
  ObColumnSchemaV2 column;
  column.set_column_id(18);
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_info(ObString("1|3|19|20|21|,16,,17,|AI_COMPLETE('model', concat(a,b))")));
  oceanbase::common::hash::ObHashMap<uint64_t, uint64_t> mapping;
  ASSERT_EQ(OB_SUCCESS, mapping.create(16, "AiColumnTest"));
  for (uint64_t id = 16; id <= 21; ++id) {
    ASSERT_EQ(OB_SUCCESS, mapping.set_refactored(id, id + 100));
  }
  ASSERT_EQ(OB_SUCCESS, column.convert_column_id(mapping));
  ASSERT_EQ(118, column.get_column_id());
  ASSERT_EQ(119, column.get_ai_column_number(2));
  ASSERT_EQ(120, column.get_ai_column_number(3));
  ASSERT_EQ(121, column.get_ai_column_number(4));
  ASSERT_EQ(ObString(",116,,117,"), column.get_ai_column_field(5));
}

TEST(AiColumnSchema, OrdinaryColumnsAndResetHaveNoAiMetadata)
{
  ObColumnSchemaV2 column;
  ASSERT_FALSE(column.is_ai_column());
  ASSERT_TRUE(column.get_ai_column_field(6).empty());
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_info(ObString("1|1|19|20|21||AI_COMPLETE('model','x')")));
  column.reset();
  ASSERT_FALSE(column.is_ai_column());
}
