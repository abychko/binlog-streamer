/* Copyright (c) 2026, Alexey Bychko.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of binlog-streamer hereby grant you an
   additional permission to link the program and your derivative works
   with the separately licensed software that they have either included
   with the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "protocol/cAuthMoreDataCodec.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(AuthMoreDataCodecTest, RecognizesFastAuthSuccess) {
  const std::vector<std::uint8_t> payload{0x01, 0x03};
  ASSERT_TRUE(AuthMoreDataCodec::IsAuthMoreData(payload));
  AuthMoreDataSignal signal{};
  std::span<const std::uint8_t> data;
  std::string error;
  ASSERT_TRUE(AuthMoreDataCodec::Parse(payload, signal, data, error)) << error;
  EXPECT_EQ(signal, AuthMoreDataSignal::FastAuthSuccess);
}

TEST(AuthMoreDataCodecTest, RecognizesPerformFullAuthentication) {
  const std::vector<std::uint8_t> payload{0x01, 0x04};
  AuthMoreDataSignal signal{};
  std::span<const std::uint8_t> data;
  std::string error;
  ASSERT_TRUE(AuthMoreDataCodec::Parse(payload, signal, data, error));
  EXPECT_EQ(signal, AuthMoreDataSignal::PerformFullAuthentication);
}

TEST(AuthMoreDataCodecTest, ClassifiesLongerPayloadAsOther) {
  const std::vector<std::uint8_t> payload{0x01, '-', '-', '-', '-', '-'};
  AuthMoreDataSignal signal{};
  std::span<const std::uint8_t> data;
  std::string error;
  ASSERT_TRUE(AuthMoreDataCodec::Parse(payload, signal, data, error));
  EXPECT_EQ(signal, AuthMoreDataSignal::Other);
  ASSERT_EQ(data.size(), 5u);
}

TEST(AuthMoreDataCodecTest, RejectsWrongHeaderByte) {
  const std::vector<std::uint8_t> payload{0x02, 0x03};
  EXPECT_FALSE(AuthMoreDataCodec::IsAuthMoreData(payload));
}

TEST(AuthMoreDataCodecTest, EncodesSignalBytesThatParseClassifies) {
  struct Case {
    AuthMoreDataSignal signal;
    std::uint8_t signalByte;
  };
  for (const Case &testCase :
       {Case{AuthMoreDataSignal::FastAuthSuccess, 0x03},
        Case{AuthMoreDataSignal::PerformFullAuthentication, 0x04}}) {
    std::vector<std::uint8_t> out = {0xEE};
    ASSERT_TRUE(AuthMoreDataCodec::EncodeSignal(testCase.signal, out));
    ASSERT_EQ(out,
              (std::vector<std::uint8_t>{0xEE, 0x01, testCase.signalByte}));

    AuthMoreDataSignal parsedSignal = AuthMoreDataSignal::Other;
    std::span<const std::uint8_t> data;
    std::string error;
    ASSERT_TRUE(AuthMoreDataCodec::Parse(
        std::span<const std::uint8_t>(out.data() + 1, out.size() - 1),
        parsedSignal, data, error))
        << error;
    EXPECT_EQ(parsedSignal, testCase.signal);
  }
}

TEST(AuthMoreDataCodecTest, EncodeSignalRefusesOtherAndWritesNothing) {
  std::vector<std::uint8_t> out = {0xEE};
  EXPECT_FALSE(AuthMoreDataCodec::EncodeSignal(AuthMoreDataSignal::Other, out));
  EXPECT_EQ(out, (std::vector<std::uint8_t>{0xEE}));
}

}  // namespace
}  // namespace binlog_streamer
