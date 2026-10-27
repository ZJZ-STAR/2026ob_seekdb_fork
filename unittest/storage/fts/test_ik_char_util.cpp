/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/dict/ob_ft_dict_hub.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using Type = ObFTCharUtil::CharType;

class IKCharUtil : public testing::Test
{
protected:
  void SetUp() override
  {
    static const int result = ObCharset::init_charset();
    ASSERT_EQ(OB_SUCCESS, result);
  }
};

// Retain the pre-optimization repeated-decode control flow as the oracle.
template <ObCharsetType CS, typename Predicate>
int legacy_check(const char *input, uint8_t len, Predicate predicate, bool &found)
{
  if (nullptr == input) {
    return OB_INVALID_ARGUMENT;
  }
  ob_wc_t code = 0;
  const auto *begin = reinterpret_cast<const unsigned char *>(input);
  int size = oceanbase::common::ob_charset_decode_unicode<CS>(begin, begin + len, code);
  if (size < 0) {
    return OB_UNEXPECT_INTERNAL_ERROR;
  }
  found = predicate(code);
  return OB_SUCCESS;
}

template <ObCharsetType CS>
int legacy_classify(const char *input, uint8_t len, Type &type)
{
  int ret = OB_SUCCESS;
  bool found = false;
  type = Type::USELESS;
  if (OB_FAIL(legacy_check<CS>(input, len, ObUnicodeBlockUtils::is_alpha, found))) {
  } else if (found) {
    type = Type::ENGLISH_LETTER;
  } else if (OB_FAIL(legacy_check<CS>(input, len, ObUnicodeBlockUtils::is_arabic, found))) {
  } else if (found) {
    type = Type::ARABIC_LETTER;
  } else if (OB_FAIL(legacy_check<CS>(input, len, ObUnicodeBlockUtils::is_chinese, found))) {
  } else if (found) {
    type = Type::CHINESE;
  } else if (OB_FAIL(legacy_check<CS>(input, len, ObUnicodeBlockUtils::is_other_cjk, found))) {
  } else if (found) {
    type = Type::OTHER_CJK;
  } else if (CS == CHARSET_UTF16 || CS == CHARSET_UTF16LE) {
    if (OB_FAIL(legacy_check<CS>(input, len, ObUnicodeBlockUtils::check_high_surrogate, found))) {
    } else if (found) {
      type = Type::SURROGATE_HIGH;
    } else if (OB_FAIL(legacy_check<CS>(input, len, ObUnicodeBlockUtils::check_low_surrogate, found))) {
    } else if (found) {
      type = Type::SURROGATE_LOW;
    }
  }
  return ret;
}

TEST_F(IKCharUtil, AllUtf8ScalarsMatchLegacy)
{
  for (uint32_t cp = 0; cp <= 0x10ffff; ++cp) {
    if (cp >= 0xd800 && cp <= 0xdfff) { continue; }
    char bytes[4];
    uint8_t len = 0;
    if (cp < 0x80) {
      bytes[0] = cp; len = 1;
    } else if (cp < 0x800) {
      bytes[0] = 0xc0 | (cp >> 6); bytes[1] = 0x80 | (cp & 63); len = 2;
    } else if (cp < 0x10000) {
      bytes[0] = 0xe0 | (cp >> 12); bytes[1] = 0x80 | ((cp >> 6) & 63);
      bytes[2] = 0x80 | (cp & 63); len = 3;
    } else {
      bytes[0] = 0xf0 | (cp >> 18); bytes[1] = 0x80 | ((cp >> 12) & 63);
      bytes[2] = 0x80 | ((cp >> 6) & 63); bytes[3] = 0x80 | (cp & 63); len = 4;
    }
    Type expected = Type::USELESS, actual = Type::USELESS;
    ASSERT_EQ(legacy_classify<CHARSET_UTF8MB4>(bytes, len, expected),
              ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, bytes, len, actual)) << cp;
    ASSERT_EQ(expected, actual) << cp;
  }
}

TEST_F(IKCharUtil, UnsupportedCharsetsKeepTheirError)
{
  // This competition branch supports utf8mb4 and binary only. The IK
  // dispatcher must not start accepting the legacy UTF-16/latin1 enum values.
  const ObCollationType collations[] = {CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN,
                                       CS_TYPE_LATIN1_BIN, CS_TYPE_GBK_BIN, CS_TYPE_BINARY};
  for (ObCollationType collation : collations) {
    Type type = Type::USELESS;
    ASSERT_EQ(OB_NOT_SUPPORTED, ObFTCharUtil::classify_first_char(collation, "x", 1, type));
  }
}

TEST_F(IKCharUtil, InvalidAndTruncatedUtf8MatchLegacy)
{
  const unsigned char bytes[][4] = {{0x80}, {0xc0, 0xaf}, {0xe4, 0xb8},
      {0xf4, 0x90, 0x80, 0x80}, {0xed, 0xa0, 0x80}, {0xff}, {0xf0, 0x9f, 0x98, 0x80}};
  for (const auto &input : bytes) {
    for (uint8_t len = 0; len <= 4; ++len) {
      Type expected = Type::USELESS, actual = Type::USELESS;
      const char *ptr = reinterpret_cast<const char *>(input);
      ASSERT_EQ(legacy_classify<CHARSET_UTF8MB4>(ptr, len, expected),
                ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, ptr, len, actual));
      ASSERT_EQ(expected, actual);
    }
  }
  Type type;
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, nullptr, 1, type));
  ASSERT_EQ(OB_NOT_SUPPORTED, ObFTCharUtil::classify_first_char(CS_TYPE_LATIN1_BIN, "x", 1, type));
}

TEST(FTDictHub, UninitializedSnapshotIsNotPublished)
{
  ObFTDictHub hub;
  ObIFTDict *main = nullptr, *quan = nullptr, *stop = nullptr;
  ASSERT_EQ(OB_NOT_INIT, hub.get_builtin_ik_dicts(main, quan, stop));
  ASSERT_EQ(nullptr, main);
  ASSERT_EQ(nullptr, quan);
  ASSERT_EQ(nullptr, stop);
  ASSERT_EQ(OB_SUCCESS, hub.destroy());
  ASSERT_EQ(OB_SUCCESS, hub.destroy());
}
