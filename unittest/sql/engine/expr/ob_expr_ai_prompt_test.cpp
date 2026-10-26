#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace
{
class AIPromptEnhancement : public testing::Test
{
protected:
  ObArenaAllocator allocator_;
  int render(const char *json, std::string &result)
  {
    ObString text(json);
    ObJsonObject *object = nullptr;
    ObString rendered;
    int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator_, text, object);
    if (ret == OB_SUCCESS) {
      ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, object, rendered);
    }
    if (ret == OB_SUCCESS) { result.assign(rendered.ptr(), rendered.length()); }
    return ret;
  }
};

TEST_F(AIPromptEnhancement, RepeatedArgumentsGrowTheBuffer)
{
  std::string result;
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{0}/{0}/{0}","args":["abcdefghij"]})", result));
  EXPECT_EQ("abcdefghij/abcdefghij/abcdefghij", result);
}

TEST_F(AIPromptEnhancement, ArraysObjectsAndNestedPrompt)
{
  std::string result;
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{0}|{1}|{2}","args":[[1,true],{"k":2},{"template":"你好{0}","args":["世界"]}]})", result));
  EXPECT_EQ("[1, true]|{\"k\": 2}|你好世界", result);
}

TEST_F(AIPromptEnhancement, PreserveEscapesAndEmbeddedZero)
{
  std::string result;
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{0}/{0}","args":["\"\\\n\u0000"]})", result));
  const std::string value("\"\\\n\0", 4);
  EXPECT_EQ(value + "/" + value, result);
}

TEST_F(AIPromptEnhancement, RejectOutOfRangeAndOverflowingIndices)
{
  std::string result;
  EXPECT_NE(OB_SUCCESS, render(R"({"template":"{1}","args":["x"]})", result));
  EXPECT_NE(OB_SUCCESS, render(R"({"template":"{99999999999999999999999999999999}","args":["x"]})", result));
  EXPECT_NE(OB_SUCCESS, render(R"({"template":"{18446744073709551616}","args":["x"]})", result));
}

TEST_F(AIPromptEnhancement, LiteralBracesRemainLiteral)
{
  std::string result;
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{} {name} {-1}","args":[]})", result));
  EXPECT_EQ("{} {name} {-1}", result);
}

TEST_F(AIPromptEnhancement, RejectMalformedPromptShape)
{
  std::string result;
  EXPECT_NE(OB_SUCCESS, render(R"({"template":[],"args":[]})", result));
  EXPECT_NE(OB_SUCCESS, render(R"({"template":"{0}","args":{}})", result));
}
}
