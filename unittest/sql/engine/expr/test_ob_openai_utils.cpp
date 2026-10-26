#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace
{
class AIProviderEnhancement : public testing::Test
{
protected:
  ObArenaAllocator allocator_;
  ObJsonObject *object(const char *json)
  {
    ObString text(json);
    ObJsonObject *result = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, text, result));
    return result;
  }
};

TEST_F(AIProviderEnhancement, RecoverEmbeddingIndexOrder)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_, object(
      R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})"), result));
  auto array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, array->element_count());
  EXPECT_EQ(1, static_cast<ObJsonArray *>(array->get_value(0))->get_value(0)->get_int());
  EXPECT_EQ(3, static_cast<ObJsonArray *>(array->get_value(1))->get_value(0)->get_int());
}

TEST_F(AIProviderEnhancement, RejectInvalidEmbeddingRecords)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  const char *responses[] = {
    R"({"data":{}})", R"({"data":[]})", R"({"data":[1]})",
    R"({"data":[{"embedding":[1]}]})",
    R"({"data":[{"index":-1,"embedding":[1]}]})",
    R"({"data":[{"index":1,"embedding":[1]}]})",
    R"({"data":[{"index":0.5,"embedding":[1]}]})",
    R"({"data":[{"index":0,"embedding":[]}]})",
    R"({"data":[{"index":0,"embedding":[null]}]})",
    R"({"data":[{"index":0,"embedding":[true]}]})",
    R"({"data":[{"index":0,"embedding":["1"]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":1,"embedding":[2,3]}]})"
  };
  for (const auto response : responses) {
    SCOPED_TRACE(response);
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, object(response), result));
  }
}

TEST_F(AIProviderEnhancement, RejectEmptyOrNonStringCompletion)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  const char *responses[] = {
    R"({"choices":[]})", R"({"choices":[{"message":{"content":""}}]})",
    R"({"choices":[{"message":{"content":null}}]})",
    R"({"choices":[{"message":{"content":[1]}}]})"
  };
  for (const auto response : responses) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, object(response), result));
  }
}

TEST_F(AIProviderEnhancement, OllamaNativeGenerationAndOptions)
{
  ObAIFuncIComplete *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, ObString("oLlAmA"), provider));
  ObString key, model("test-model"), system("system"), content("hello");
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, provider->get_header(allocator_, key, headers));
  ASSERT_EQ(1, headers.count());
  EXPECT_EQ(0, headers.at(0).compare("Content-Type: application/json"));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator_, model, system, content,
      object(R"({"temperature":0.2,"max_tokens":16})"), body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(static_cast<ObJsonBoolean *>(body->get_value("stream"))->value());
  auto options = static_cast<ObJsonObject *>(body->get_value("options"));
  ASSERT_NE(nullptr, options);
  EXPECT_EQ(16, options->get_value("num_predict")->get_int());
  EXPECT_EQ(nullptr, body->get_value("max_tokens"));
}

TEST_F(AIProviderEnhancement, OllamaEmbeddingRequestAndResponse)
{
  ObAIFuncIEmbed *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator_, ObString("OLLAMA"), provider));
  ObString model("test-model");
  ObArray<ObString> inputs;
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("a")));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("b")));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator_, model, inputs, nullptr, body));
  EXPECT_EQ(2, body->get_value("input")->element_count());
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_SUCCESS, provider->parse_output(allocator_, object(R"({"embeddings":[[1,2],[3,4]]})"), result));
  EXPECT_NE(OB_SUCCESS, provider->parse_output(allocator_, object(R"({"embeddings":[[1],[2,3]]})"), result));
  EXPECT_NE(OB_SUCCESS, provider->parse_output(allocator_, object(R"({"embeddings":"bad"})"), result));
}
}
