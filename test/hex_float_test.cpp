// Copyright (c) 2015-2016 The Khronos Group Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "source/util/hex_float.h"
#include "test/unit_spirv.h"

namespace spvtools {
namespace utils {
namespace {

using ::testing::Eq;

// In this file "encode" means converting a number into a string,
// and "decode" means converting a string into a number.

using HexFloatTest =
    ::testing::TestWithParam<std::pair<FloatProxy<float>, std::string>>;
using DecodeHexFloatTest =
    ::testing::TestWithParam<std::pair<std::string, FloatProxy<float>>>;
using HexDoubleTest =
    ::testing::TestWithParam<std::pair<FloatProxy<double>, std::string>>;
using DecodeHexDoubleTest =
    ::testing::TestWithParam<std::pair<std::string, FloatProxy<double>>>;
using RoundTripFloatTest = ::testing::TestWithParam<float>;
using RoundTripDoubleTest = ::testing::TestWithParam<double>;

// Hex-encodes a float value.
template <typename T>
std::string EncodeViaHexFloat(const T& value) {
  std::stringstream ss;
  ss << HexFloat<T>(value);
  return ss.str();
}

// The following two tests can't be DRY because they take different parameter
// types.

TEST_P(HexFloatTest, EncodeCorrectly) {
  EXPECT_THAT(EncodeViaHexFloat(GetParam().first), Eq(GetParam().second));
}

TEST_P(HexDoubleTest, EncodeCorrectly) {
  EXPECT_THAT(EncodeViaHexFloat(GetParam().first), Eq(GetParam().second));
}

// Decodes a hex-float string.
template <typename T>
FloatProxy<T> Decode(const std::string& str) {
  HexFloat<FloatProxy<T>> decoded(0.f);
  EXPECT_TRUE((std::stringstream(str) >> decoded).eof());
  return decoded.value();
}

TEST_P(HexFloatTest, DecodeCorrectly) {
  EXPECT_THAT(Decode<float>(GetParam().second), Eq(GetParam().first));
}

TEST_P(HexDoubleTest, DecodeCorrectly) {
  EXPECT_THAT(Decode<double>(GetParam().second), Eq(GetParam().first));
}

INSTANTIATE_TEST_SUITE_P(
    Float32Tests, HexFloatTest,
    ::testing::ValuesIn(std::vector<std::pair<FloatProxy<float>, std::string>>({
        {0.f, "0x0p+0"},
        {1.f, "0x1p+0"},
        {2.f, "0x1p+1"},
        {3.f, "0x1.8p+1"},
        {0.5f, "0x1p-1"},
        {0.25f, "0x1p-2"},
        {0.75f, "0x1.8p-1"},
        {-0.f, "-0x0p+0"},
        {-1.f, "-0x1p+0"},
        {-0.5f, "-0x1p-1"},
        {-0.25f, "-0x1p-2"},
        {-0.75f, "-0x1.8p-1"},

        // Larger numbers
        {512.f, "0x1p+9"},
        {-512.f, "-0x1p+9"},
        {1024.f, "0x1p+10"},
        {-1024.f, "-0x1p+10"},
        {1024.f + 8.f, "0x1.02p+10"},
        {-1024.f - 8.f, "-0x1.02p+10"},

        // Small numbers
        {1.0f / 512.f, "0x1p-9"},
        {1.0f / -512.f, "-0x1p-9"},
        {1.0f / 1024.f, "0x1p-10"},
        {1.0f / -1024.f, "-0x1p-10"},
        {1.0f / 1024.f + 1.0f / 8.f, "0x1.02p-3"},
        {1.0f / -1024.f - 1.0f / 8.f, "-0x1.02p-3"},

        // lowest non-denorm
        {float(ldexp(1.0f, -126)), "0x1p-126"},
        {float(ldexp(-1.0f, -126)), "-0x1p-126"},

        // Denormalized values
        {float(ldexp(1.0f, -127)), "0x1p-127"},
        {float(ldexp(1.0f, -127) / 2.0f), "0x1p-128"},
        {float(ldexp(1.0f, -127) / 4.0f), "0x1p-129"},
        {float(ldexp(1.0f, -127) / 8.0f), "0x1p-130"},
        {float(ldexp(-1.0f, -127)), "-0x1p-127"},
        {float(ldexp(-1.0f, -127) / 2.0f), "-0x1p-128"},
        {float(ldexp(-1.0f, -127) / 4.0f), "-0x1p-129"},
        {float(ldexp(-1.0f, -127) / 8.0f), "-0x1p-130"},

        {float(ldexp(1.0, -127) + (ldexp(1.0, -127) / 2.0f)), "0x1.8p-127"},
        {float(ldexp(1.0, -127) / 2.0 + (ldexp(1.0, -127) / 4.0f)),
         "0x1.8p-128"},

    })));

INSTANTIATE_TEST_SUITE_P(
    Float32NanTests, HexFloatTest,
    ::testing::ValuesIn(std::vector<std::pair<FloatProxy<float>, std::string>>({
        // Various NAN and INF cases
        {uint32_t(0xFF800000), "-0x1p+128"},         // -inf
        {uint32_t(0x7F800000), "0x1p+128"},          // inf
        {uint32_t(0xFFC00000), "-0x1.8p+128"},       // -nan
        {uint32_t(0xFF800100), "-0x1.0002p+128"},    // -nan
        {uint32_t(0xFF800c00), "-0x1.0018p+128"},    // -nan
        {uint32_t(0xFF80F000), "-0x1.01ep+128"},     // -nan
        {uint32_t(0xFFFFFFFF), "-0x1.fffffep+128"},  // -nan
        {uint32_t(0x7FC00000), "0x1.8p+128"},        // +nan
        {uint32_t(0x7F800100), "0x1.0002p+128"},     // +nan
        {uint32_t(0x7f800c00), "0x1.0018p+128"},     // +nan
        {uint32_t(0x7F80F000), "0x1.01ep+128"},      // +nan
        {uint32_t(0x7FFFFFFF), "0x1.fffffep+128"},   // +nan
    })));

INSTANTIATE_TEST_SUITE_P(
    Float64Tests, HexDoubleTest,
    ::testing::ValuesIn(
        std::vector<std::pair<FloatProxy<double>, std::string>>({
            {0., "0x0p+0"},
            {1., "0x1p+0"},
            {2., "0x1p+1"},
            {3., "0x1.8p+1"},
            {0.5, "0x1p-1"},
            {0.25, "0x1p-2"},
            {0.75, "0x1.8p-1"},
            {-0., "-0x0p+0"},
            {-1., "-0x1p+0"},
            {-0.5, "-0x1p-1"},
            {-0.25, "-0x1p-2"},
            {-0.75, "-0x1.8p-1"},

            // Larger numbers
            {512., "0x1p+9"},
            {-512., "-0x1p+9"},
            {1024., "0x1p+10"},
            {-1024., "-0x1p+10"},
            {1024. + 8., "0x1.02p+10"},
            {-1024. - 8., "-0x1.02p+10"},

            // Large outside the range of normal floats
            {ldexp(1.0, 128), "0x1p+128"},
            {ldexp(1.0, 129), "0x1p+129"},
            {ldexp(-1.0, 128), "-0x1p+128"},
            {ldexp(-1.0, 129), "-0x1p+129"},
            {ldexp(1.0, 128) + ldexp(1.0, 90), "0x1.0000000004p+128"},
            {ldexp(1.0, 129) + ldexp(1.0, 120), "0x1.008p+129"},
            {ldexp(-1.0, 128) + ldexp(1.0, 90), "-0x1.fffffffff8p+127"},
            {ldexp(-1.0, 129) + ldexp(1.0, 120), "-0x1.ffp+128"},

            // Small numbers
            {1.0 / 512., "0x1p-9"},
            {1.0 / -512., "-0x1p-9"},
            {1.0 / 1024., "0x1p-10"},
            {1.0 / -1024., "-0x1p-10"},
            {1.0 / 1024. + 1.0 / 8., "0x1.02p-3"},
            {1.0 / -1024. - 1.0 / 8., "-0x1.02p-3"},

            // Small outside the range of normal floats
            {ldexp(1.0, -128), "0x1p-128"},
            {ldexp(1.0, -129), "0x1p-129"},
            {ldexp(-1.0, -128), "-0x1p-128"},
            {ldexp(-1.0, -129), "-0x1p-129"},
            {ldexp(1.0, -128) + ldexp(1.0, -90), "0x1.0000000004p-90"},
            {ldexp(1.0, -129) + ldexp(1.0, -120), "0x1.008p-120"},
            {ldexp(-1.0, -128) + ldexp(1.0, -90), "0x1.fffffffff8p-91"},
            {ldexp(-1.0, -129) + ldexp(1.0, -120), "0x1.ffp-121"},

            // lowest non-denorm
            {ldexp(1.0, -1022), "0x1p-1022"},
            {ldexp(-1.0, -1022), "-0x1p-1022"},

            // Denormalized values
            {ldexp(1.0, -1023), "0x1p-1023"},
            {ldexp(1.0, -1023) / 2.0, "0x1p-1024"},
            {ldexp(1.0, -1023) / 4.0, "0x1p-1025"},
            {ldexp(1.0, -1023) / 8.0, "0x1p-1026"},
            {ldexp(-1.0, -1024), "-0x1p-1024"},
            {ldexp(-1.0, -1024) / 2.0, "-0x1p-1025"},
            {ldexp(-1.0, -1024) / 4.0, "-0x1p-1026"},
            {ldexp(-1.0, -1024) / 8.0, "-0x1p-1027"},

            {ldexp(1.0, -1023) + (ldexp(1.0, -1023) / 2.0), "0x1.8p-1023"},
            {ldexp(1.0, -1023) / 2.0 + (ldexp(1.0, -1023) / 4.0),
             "0x1.8p-1024"},

        })));

INSTANTIATE_TEST_SUITE_P(
    Float64NanTests, HexDoubleTest,
    ::testing::ValuesIn(std::vector<
                        std::pair<FloatProxy<double>, std::string>>({
        // Various NAN and INF cases
        {uint64_t(0xFFF0000000000000LL), "-0x1p+1024"},                // -inf
        {uint64_t(0x7FF0000000000000LL), "0x1p+1024"},                 // +inf
        {uint64_t(0xFFF8000000000000LL), "-0x1.8p+1024"},              // -nan
        {uint64_t(0xFFF0F00000000000LL), "-0x1.0fp+1024"},             // -nan
        {uint64_t(0xFFF0000000000001LL), "-0x1.0000000000001p+1024"},  // -nan
        {uint64_t(0xFFF0000300000000LL), "-0x1.00003p+1024"},          // -nan
        {uint64_t(0xFFFFFFFFFFFFFFFFLL), "-0x1.fffffffffffffp+1024"},  // -nan
        {uint64_t(0x7FF8000000000000LL), "0x1.8p+1024"},               // +nan
        {uint64_t(0x7FF0F00000000000LL), "0x1.0fp+1024"},              // +nan
        {uint64_t(0x7FF0000000000001LL), "0x1.0000000000001p+1024"},   // -nan
        {uint64_t(0x7FF0000300000000LL), "0x1.00003p+1024"},           // -nan
        {uint64_t(0x7FFFFFFFFFFFFFFFLL), "0x1.fffffffffffffp+1024"},   // -nan
    })));

// Tests that encoding a value and decoding it again restores
// the same value.
TEST_P(RoundTripFloatTest, CanStoreAccurately) {
  std::stringstream ss;
  ss << FloatProxy<float>(GetParam());
  ss.seekg(0);
  FloatProxy<float> res;
  ss >> res;
  EXPECT_THAT(GetParam(), Eq(res.getAsFloat()));
}

TEST_P(RoundTripDoubleTest, CanStoreAccurately) {
  std::stringstream ss;
  ss << FloatProxy<double>(GetParam());
  ss.seekg(0);
  FloatProxy<double> res;
  ss >> res;
  EXPECT_THAT(GetParam(), Eq(res.getAsFloat()));
}

INSTANTIATE_TEST_SUITE_P(
    Float32StoreTests, RoundTripFloatTest,
    ::testing::ValuesIn(std::vector<float>(
        {// Value requiring more than 6 digits of precision to be
         // represented accurately.
         3.0000002f})));

INSTANTIATE_TEST_SUITE_P(
    Float64StoreTests, RoundTripDoubleTest,
    ::testing::ValuesIn(std::vector<double>(
        {// Value requiring more than 15 digits of precision to be
         // represented accurately.
         1.5000000000000002})));

TEST(HexFloatStreamTest, OperatorLeftShiftPreservesFloatAndFill) {
  std::stringstream s;
  s << std::setw(4) << std::oct << std::setfill('x') << 8 << " "
    << FloatProxy<float>(uint32_t(0xFF800100)) << " " << std::setw(4) << 9;
  EXPECT_THAT(s.str(), Eq(std::string("xx10 -0x1.0002p+128 xx11")));
}

TEST(HexDoubleStreamTest, OperatorLeftShiftPreservesFloatAndFill) {
  std::stringstream s;
  s << std::setw(4) << std::oct << std::setfill('x') << 8 << " "
    << FloatProxy<double>(uint64_t(0x7FF0F00000000000LL)) << " " << std::setw(4)
    << 9;
  EXPECT_THAT(s.str(), Eq(std::string("xx10 0x1.0fp+1024 xx11")));
}

TEST_P(DecodeHexFloatTest, DecodeCorrectly) {
  EXPECT_THAT(Decode<float>(GetParam().first), Eq(GetParam().second));
}

TEST_P(DecodeHexDoubleTest, DecodeCorrectly) {
  EXPECT_THAT(Decode<double>(GetParam().first), Eq(GetParam().second));
}

INSTANTIATE_TEST_SUITE_P(
    Float32DecodeTests, DecodeHexFloatTest,
    ::testing::ValuesIn(std::vector<std::pair<std::string, FloatProxy<float>>>({
        {"0x0p+000", 0.f},
        {"0x0p0", 0.f},
        {"0x0p-0", 0.f},

        // flush to zero cases
        {"0x1p-500", 0.f},  // Exponent underflows.
        {"-0x1p-500", -0.f},
        {"0x0.00000000001p-126", 0.f},  // Fraction causes underflow.
        {"-0x0.0000000001p-127", -0.f},
        {"-0x0.01p-142", -0.f},  // Fraction causes additional underflow.
        {"0x0.01p-142", 0.f},

        // Some floats that do not encode the same way as they decode.
        {"0x2p+0", 2.f},
        {"0xFFp+0", 255.f},
        {"0x0.8p+0", 0.5f},
        {"0x0.4p+0", 0.25f},
    })));

INSTANTIATE_TEST_SUITE_P(
    Float32DecodeInfTests, DecodeHexFloatTest,
    ::testing::ValuesIn(std::vector<std::pair<std::string, FloatProxy<float>>>({
        // inf cases
        {"-0x1p+128", uint32_t(0xFF800000)},   // -inf
        {"0x32p+127", uint32_t(0x7F800000)},   // inf
        {"0x32p+500", uint32_t(0x7F800000)},   // inf
        {"-0x32p+127", uint32_t(0xFF800000)},  // -inf
    })));

INSTANTIATE_TEST_SUITE_P(
    Float64DecodeTests, DecodeHexDoubleTest,
    ::testing::ValuesIn(
        std::vector<std::pair<std::string, FloatProxy<double>>>({
            {"0x0p+000", 0.},
            {"0x0p0", 0.},
            {"0x0p-0", 0.},

            // flush to zero cases
            {"0x1p-5000", 0.},  // Exponent underflows.
            {"-0x1p-5000", -0.},
            {"0x0.0000000000000001p-1023", 0.},  // Fraction causes underflow.
            {"-0x0.000000000000001p-1024", -0.},
            {"-0x0.01p-1090", -0.f},  // Fraction causes additional underflow.
            {"0x0.01p-1090", 0.},

            // Some floats that do not encode the same way as they decode.
            {"0x2p+0", 2.},
            {"0xFFp+0", 255.},
            {"0x0.8p+0", 0.5},
            {"0x0.4p+0", 0.25},
        })));

INSTANTIATE_TEST_SUITE_P(
    Float64DecodeInfTests, DecodeHexDoubleTest,
    ::testing::ValuesIn(
        std::vector<std::pair<std::string, FloatProxy<double>>>({
            // inf cases
            {"-0x1p+1024", uint64_t(0xFFF0000000000000)},   // -inf
            {"0x32p+1023", uint64_t(0x7FF0000000000000)},   // inf
            {"0x32p+5000", uint64_t(0x7FF0000000000000)},   // inf
            {"-0x32p+1023", uint64_t(0xFFF0000000000000)},  // -inf
        })));

TEST(FloatProxy, ValidConversion) {
  EXPECT_THAT(FloatProxy<float>(1.f).getAsFloat(), Eq(1.0f));
  EXPECT_THAT(FloatProxy<float>(32.f).getAsFloat(), Eq(32.0f));
  EXPECT_THAT(FloatProxy<float>(-1.f).getAsFloat(), Eq(-1.0f));
  EXPECT_THAT(FloatProxy<float>(0.f).getAsFloat(), Eq(0.0f));
  EXPECT_THAT(FloatProxy<float>(-0.f).getAsFloat(), Eq(-0.0f));
  EXPECT_THAT(FloatProxy<float>(1.2e32f).getAsFloat(), Eq(1.2e32f));

  EXPECT_TRUE(std::isinf(FloatProxy<float>(uint32_t(0xFF800000)).getAsFloat()));
  EXPECT_TRUE(std::isinf(FloatProxy<float>(uint32_t(0x7F800000)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0xFFC00000)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0xFF800100)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0xFF800c00)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0xFF80F000)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0xFFFFFFFF)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0x7FC00000)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0x7F800100)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0x7f800c00)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0x7F80F000)).getAsFloat()));
  EXPECT_TRUE(std::isnan(FloatProxy<float>(uint32_t(0x7FFFFFFF)).getAsFloat()));

  EXPECT_THAT(FloatProxy<float>(uint32_t(0xFF800000)).data(), Eq(0xFF800000u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0x7F800000)).data(), Eq(0x7F800000u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0xFFC00000)).data(), Eq(0xFFC00000u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0xFF800100)).data(), Eq(0xFF800100u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0xFF800c00)).data(), Eq(0xFF800c00u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0xFF80F000)).data(), Eq(0xFF80F000u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0xFFFFFFFF)).data(), Eq(0xFFFFFFFFu));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0x7FC00000)).data(), Eq(0x7FC00000u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0x7F800100)).data(), Eq(0x7F800100u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0x7f800c00)).data(), Eq(0x7f800c00u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0x7F80F000)).data(), Eq(0x7F80F000u));
  EXPECT_THAT(FloatProxy<float>(uint32_t(0x7FFFFFFF)).data(), Eq(0x7FFFFFFFu));
}

TEST(FloatProxy, Nan) {
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0xFFC00000)).isNan());
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0xFF800100)).isNan());
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0xFF800c00)).isNan());
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0xFF80F000)).isNan());
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0xFFFFFFFF)).isNan());
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0x7FC00000)).isNan());
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0x7F800100)).isNan());
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0x7f800c00)).isNan());
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0x7F80F000)).isNan());
  EXPECT_TRUE(FloatProxy<float>(uint32_t(0x7FFFFFFF)).isNan());
}

TEST(FloatProxy, Negation) {
  EXPECT_THAT((-FloatProxy<float>(1.f)).getAsFloat(), Eq(-1.0f));
  EXPECT_THAT((-FloatProxy<float>(0.f)).getAsFloat(), Eq(-0.0f));

  EXPECT_THAT((-FloatProxy<float>(-1.f)).getAsFloat(), Eq(1.0f));
  EXPECT_THAT((-FloatProxy<float>(-0.f)).getAsFloat(), Eq(0.0f));

  EXPECT_THAT((-FloatProxy<float>(32.f)).getAsFloat(), Eq(-32.0f));
  EXPECT_THAT((-FloatProxy<float>(-32.f)).getAsFloat(), Eq(32.0f));

  EXPECT_THAT((-FloatProxy<float>(1.2e32f)).getAsFloat(), Eq(-1.2e32f));
  EXPECT_THAT((-FloatProxy<float>(-1.2e32f)).getAsFloat(), Eq(1.2e32f));

  EXPECT_THAT(
      (-FloatProxy<float>(std::numeric_limits<float>::infinity())).getAsFloat(),
      Eq(-std::numeric_limits<float>::infinity()));
  EXPECT_THAT((-FloatProxy<float>(-std::numeric_limits<float>::infinity()))
                  .getAsFloat(),
              Eq(std::numeric_limits<float>::infinity()));
}

// Test conversion of FloatProxy values to strings.
//
// In previous cases, we always wrapped the FloatProxy value in a HexFloat
// before conversion to a string.  In the following cases, the FloatProxy
// decides for itself whether to print as a regular number or as a hex float.

using FloatProxyFloatTest =
    ::testing::TestWithParam<std::pair<FloatProxy<float>, std::string>>;
using FloatProxyDoubleTest =
    ::testing::TestWithParam<std::pair<FloatProxy<double>, std::string>>;

// Converts a float value to a string via a FloatProxy.
template <typename T>
std::string EncodeViaFloatProxy(const T& value) {
  std::stringstream ss;
  ss << value;
  return ss.str();
}

// Converts a floating point string so that the exponent prefix
// is 'e', and the exponent value does not have leading zeros.
// The Microsoft runtime library likes to write things like "2.5E+010".
// Convert that to "2.5e+10".
// We don't care what happens to strings that are not floating point
// strings.
std::string NormalizeExponentInFloatString(std::string in) {
  std::string result;
  // Reserve one spot for the terminating null, even when the sscanf fails.
  std::vector<char> prefix(in.size() + 1);
  char e;
  char plus_or_minus;
  int exponent;  // in base 10
  if ((4 == std::sscanf(in.c_str(), "%[-+.0123456789]%c%c%d", prefix.data(), &e,
                        &plus_or_minus, &exponent)) &&
      (e == 'e' || e == 'E') &&
      (plus_or_minus == '-' || plus_or_minus == '+')) {
    // It looks like a floating point value with exponent.
    std::stringstream out;
    out << prefix.data() << 'e' << plus_or_minus << exponent;
    result = out.str();
  } else {
    result = in;
  }
  return result;
}

TEST(NormalizeFloat, Sample) {
  EXPECT_THAT(NormalizeExponentInFloatString(""), Eq(""));
  EXPECT_THAT(NormalizeExponentInFloatString("1e-12"), Eq("1e-12"));
  EXPECT_THAT(NormalizeExponentInFloatString("1E+14"), Eq("1e+14"));
  EXPECT_THAT(NormalizeExponentInFloatString("1e-0012"), Eq("1e-12"));
  EXPECT_THAT(NormalizeExponentInFloatString("1.263E+014"), Eq("1.263e+14"));
}

// The following two tests can't be DRY because they take different parameter
// types.
TEST_P(FloatProxyFloatTest, EncodeCorrectly) {
  EXPECT_THAT(
      NormalizeExponentInFloatString(EncodeViaFloatProxy(GetParam().first)),
      Eq(GetParam().second));
}

TEST_P(FloatProxyDoubleTest, EncodeCorrectly) {
  EXPECT_THAT(
      NormalizeExponentInFloatString(EncodeViaFloatProxy(GetParam().first)),
      Eq(GetParam().second));
}

INSTANTIATE_TEST_SUITE_P(
    Float32Tests, FloatProxyFloatTest,
    ::testing::ValuesIn(std::vector<std::pair<FloatProxy<float>, std::string>>({
        // Zero
        {0.f, "0"},
        // Normal numbers
        {1.f, "1"},
        {-0.25f, "-0.25"},
        {1000.0f, "1000"},

        // Still normal numbers, but with large magnitude exponents.
        {float(ldexp(1.f, 126)), "8.50705917e+37"},
        {float(ldexp(-1.f, -126)), "-1.17549435e-38"},

        // denormalized values are printed as hex floats.
        {float(ldexp(1.0f, -127)), "0x1p-127"},
        {float(ldexp(1.5f, -128)), "0x1.8p-128"},
        {float(ldexp(1.25, -129)), "0x1.4p-129"},
        {float(ldexp(1.125, -130)), "0x1.2p-130"},
        {float(ldexp(-1.0f, -127)), "-0x1p-127"},
        {float(ldexp(-1.0f, -128)), "-0x1p-128"},
        {float(ldexp(-1.0f, -129)), "-0x1p-129"},
        {float(ldexp(-1.5f, -130)), "-0x1.8p-130"},

        // NaNs
        {FloatProxy<float>(uint32_t(0xFFC00000)), "-0x1.8p+128"},
        {FloatProxy<float>(uint32_t(0xFF800100)), "-0x1.0002p+128"},

        {std::numeric_limits<float>::infinity(), "0x1p+128"},
        {-std::numeric_limits<float>::infinity(), "-0x1p+128"},
    })));

INSTANTIATE_TEST_SUITE_P(
    Float64Tests, FloatProxyDoubleTest,
    ::testing::ValuesIn(
        std::vector<std::pair<FloatProxy<double>, std::string>>({
            {0., "0"},
            {1., "1"},
            {-0.25, "-0.25"},
            {1000.0, "1000"},

            // Large outside the range of normal floats
            {ldexp(1.0, 128), "3.4028236692093846e+38"},
            {ldexp(1.5, 129), "1.0208471007628154e+39"},
            {ldexp(-1.0, 128), "-3.4028236692093846e+38"},
            {ldexp(-1.5, 129), "-1.0208471007628154e+39"},

            // Small outside the range of normal floats
            {ldexp(1.5, -129), "2.2040519077917891e-39"},
            {ldexp(-1.5, -129), "-2.2040519077917891e-39"},

            // lowest non-denorm
            {ldexp(1.0, -1022), "2.2250738585072014e-308"},
            {ldexp(-1.0, -1022), "-2.2250738585072014e-308"},

            // Denormalized values
            {ldexp(1.125, -1023), "0x1.2p-1023"},
            {ldexp(-1.375, -1024), "-0x1.6p-1024"},

            // NaNs
            {uint64_t(0x7FF8000000000000LL), "0x1.8p+1024"},
            {uint64_t(0xFFF0F00000000000LL), "-0x1.0fp+1024"},

            // Infinity
            {std::numeric_limits<double>::infinity(), "0x1p+1024"},
            {-std::numeric_limits<double>::infinity(), "-0x1p+1024"},

        })));

// double is used so that unbiased_exponent can be used with the output
// of ldexp directly.
int32_t unbiased_exponent(double f) {
  return HexFloat<FloatProxy<float>>(static_cast<float>(f))
      .getUnbiasedNormalizedExponent();
}

int16_t unbiased_half_exponent(uint16_t f) {
  return HexFloat<FloatProxy<Float16>>(f).getUnbiasedNormalizedExponent();
}
int8_t unbiased_E4M3_exponent(uint8_t f) {
  return HexFloat<FloatProxy<Float8_E4M3>>(f).getUnbiasedNormalizedExponent();
}
int8_t unbiased_E5M2_exponent(uint8_t f) {
  return HexFloat<FloatProxy<Float8_E5M2>>(f).getUnbiasedNormalizedExponent();
}

TEST(HexFloatOperationTest, UnbiasedExponent) {
  // Float cases
  EXPECT_EQ(0, unbiased_exponent(ldexp(1.0f, 0)));
  EXPECT_EQ(-32, unbiased_exponent(ldexp(1.0f, -32)));
  EXPECT_EQ(42, unbiased_exponent(ldexp(1.0f, 42)));
  EXPECT_EQ(125, unbiased_exponent(ldexp(1.0f, 125)));

  EXPECT_EQ(128,
            HexFloat<FloatProxy<float>>(std::numeric_limits<float>::infinity())
                .getUnbiasedNormalizedExponent());

  EXPECT_EQ(-100, unbiased_exponent(ldexp(1.0f, -100)));
  EXPECT_EQ(-127, unbiased_exponent(ldexp(1.0f, -127)));  // First denorm
  EXPECT_EQ(-128, unbiased_exponent(ldexp(1.0f, -128)));
  EXPECT_EQ(-129, unbiased_exponent(ldexp(1.0f, -129)));
  EXPECT_EQ(-140, unbiased_exponent(ldexp(1.0f, -140)));
  // Smallest representable number
  EXPECT_EQ(-126 - 23, unbiased_exponent(ldexp(1.0f, -126 - 23)));
  // Should get rounded to 0 first.
  EXPECT_EQ(0, unbiased_exponent(ldexp(1.0f, -127 - 23)));

  // Float16 cases
  // The exponent is represented in the bits 0x7C00
  // The offset is -15
  EXPECT_EQ(0, unbiased_half_exponent(0x3C00));
  EXPECT_EQ(3, unbiased_half_exponent(0x4800));
  EXPECT_EQ(-1, unbiased_half_exponent(0x3800));
  EXPECT_EQ(-14, unbiased_half_exponent(0x0400));
  EXPECT_EQ(16, unbiased_half_exponent(0x7C00));
  EXPECT_EQ(10, unbiased_half_exponent(0x6400));

  // Smallest representable number
  EXPECT_EQ(-24, unbiased_half_exponent(0x0001));

  // E4M3 cases
  // The exponent is represented in the bits 0x78
  // The offset is -7
  EXPECT_EQ(0, unbiased_E4M3_exponent(0x38));
  EXPECT_EQ(3, unbiased_E4M3_exponent(0x50));
  EXPECT_EQ(-1, unbiased_E4M3_exponent(0x30));
  EXPECT_EQ(-6, unbiased_E4M3_exponent(0x08));
  EXPECT_EQ(8, unbiased_E4M3_exponent(0x78));

  // Smallest representable number
  EXPECT_EQ(-9, unbiased_E4M3_exponent(0x01));

  // E5M2 cases
  // The exponent is represented in the bits 0x7C
  // The offset is -15
  EXPECT_EQ(0, unbiased_E5M2_exponent(0x3C));
  EXPECT_EQ(3, unbiased_E5M2_exponent(0x48));
  EXPECT_EQ(-1, unbiased_E5M2_exponent(0x38));
  EXPECT_EQ(-14, unbiased_E5M2_exponent(0x04));
  EXPECT_EQ(16, unbiased_E5M2_exponent(0x7C));
  EXPECT_EQ(10, unbiased_E5M2_exponent(0x64));

  // Smallest representable number
  EXPECT_EQ(-16, unbiased_E5M2_exponent(0x01));
}

// Creates a float that is the sum of 1/(2 ^ fractions[i]) for i in factions
float float_fractions(const std::vector<uint32_t>& fractions) {
  float f = 0;
  for (int32_t i : fractions) {
    f += std::ldexp(1.0f, -i);
  }
  return f;
}

// Returns the normalized significand of a HexFloat<FloatProxy<float>>
// that was created by calling float_fractions with the input fractions,
// raised to the power of exp.
uint32_t normalized_significand(const std::vector<uint32_t>& fractions,
                                uint32_t exp) {
  return HexFloat<FloatProxy<float>>(
             static_cast<float>(ldexp(float_fractions(fractions), exp)))
      .getNormalizedSignificand();
}

// Sets the bits from MSB to LSB of the significand part of a float.
// For example 0 would set the bit 23 (counting from LSB to MSB),
// and 1 would set the 22nd bit.
uint32_t bits_set(const std::vector<uint32_t>& bits) {
  const uint32_t top_bit = 1u << 22u;
  uint32_t val = 0;
  for (uint32_t i : bits) {
    val |= top_bit >> i;
  }
  return val;
}

// The same as bits_set but for a Float16 value instead of 32-bit floating
// point.
uint16_t half_bits_set(const std::vector<uint32_t>& bits) {
  const uint32_t top_bit = 1u << 9u;
  uint32_t val = 0;
  for (uint32_t i : bits) {
    val |= top_bit >> i;
  }
  return static_cast<uint16_t>(val);
}

TEST(HexFloatOperationTest, NormalizedSignificand) {
  // For normalized numbers (the following) it should be a simple matter
  // of getting rid of the top implicit bit
  EXPECT_EQ(bits_set({}), normalized_significand({0}, 0));
  EXPECT_EQ(bits_set({0}), normalized_significand({0, 1}, 0));
  EXPECT_EQ(bits_set({0, 1}), normalized_significand({0, 1, 2}, 0));
  EXPECT_EQ(bits_set({1}), normalized_significand({0, 2}, 0));
  EXPECT_EQ(bits_set({1}), normalized_significand({0, 2}, 32));
  EXPECT_EQ(bits_set({1}), normalized_significand({0, 2}, 126));

  // For denormalized numbers we expect the normalized significand to
  // shift as if it were normalized. This means, in practice that the
  // top_most set bit will be cut off. Looks very similar to above (on purpose)
  EXPECT_EQ(bits_set({}),
            normalized_significand({0}, static_cast<uint32_t>(-127)));
  EXPECT_EQ(bits_set({3}),
            normalized_significand({0, 4}, static_cast<uint32_t>(-128)));
  EXPECT_EQ(bits_set({3}),
            normalized_significand({0, 4}, static_cast<uint32_t>(-127)));
  EXPECT_EQ(bits_set({}),
            normalized_significand({22}, static_cast<uint32_t>(-127)));
  EXPECT_EQ(bits_set({0}),
            normalized_significand({21, 22}, static_cast<uint32_t>(-127)));
}

// Returns the 32-bit floating point value created by
// calling setFromSignUnbiasedExponentAndNormalizedSignificand
// on a HexFloat<FloatProxy<float>>
float set_from_sign(bool negative, int32_t unbiased_exponent,
                    uint32_t significand, bool round_denorm_up) {
  HexFloat<FloatProxy<float>> f(0.f);
  f.setFromSignUnbiasedExponentAndNormalizedSignificand(
      negative, unbiased_exponent, significand, round_denorm_up);
  return f.value().getAsFloat();
}

TEST(HexFloatOperationTests,
     SetFromSignUnbiasedExponentAndNormalizedSignificand) {
  EXPECT_EQ(1.f, set_from_sign(false, 0, 0, false));

  // Tests insertion of various denormalized numbers with and without round up.
  EXPECT_EQ(static_cast<float>(ldexp(1.f, -149)),
            set_from_sign(false, -149, 0, false));
  EXPECT_EQ(static_cast<float>(ldexp(1.f, -149)),
            set_from_sign(false, -149, 0, true));
  EXPECT_EQ(0.f, set_from_sign(false, -150, 1, false));
  EXPECT_EQ(static_cast<float>(ldexp(1.f, -149)),
            set_from_sign(false, -150, 1, true));

  EXPECT_EQ(ldexp(1.0f, -127), set_from_sign(false, -127, 0, false));
  EXPECT_EQ(ldexp(1.0f, -128), set_from_sign(false, -128, 0, false));
  EXPECT_EQ(float_fractions({0, 1, 2, 5}),
            set_from_sign(false, 0, bits_set({0, 1, 4}), false));
  EXPECT_EQ(ldexp(float_fractions({0, 1, 2, 5}), -32),
            set_from_sign(false, -32, bits_set({0, 1, 4}), false));
  EXPECT_EQ(ldexp(float_fractions({0, 1, 2, 5}), -128),
            set_from_sign(false, -128, bits_set({0, 1, 4}), false));

  // The negative cases from above.
  EXPECT_EQ(-1.f, set_from_sign(true, 0, 0, false));
  EXPECT_EQ(-ldexp(1.0, -127), set_from_sign(true, -127, 0, false));
  EXPECT_EQ(-ldexp(1.0, -128), set_from_sign(true, -128, 0, false));
  EXPECT_EQ(-float_fractions({0, 1, 2, 5}),
            set_from_sign(true, 0, bits_set({0, 1, 4}), false));
  EXPECT_EQ(-ldexp(float_fractions({0, 1, 2, 5}), -32),
            set_from_sign(true, -32, bits_set({0, 1, 4}), false));
  EXPECT_EQ(-ldexp(float_fractions({0, 1, 2, 5}), -128),
            set_from_sign(true, -128, bits_set({0, 1, 4}), false));
}

TEST(HexFloatOperationTests, NonRounding) {
  // Rounding from 32-bit hex-float to 32-bit hex-float should be trivial,
  // except in the denorm case which is a bit more complex.
  using HF = HexFloat<FloatProxy<float>>;
  bool carry_bit = false;

  round_direction rounding[] = {round_direction::kToZero,
                                round_direction::kToNearestEven,
                                round_direction::kToPositiveInfinity,
                                round_direction::kToNegativeInfinity};

  // Everything fits, so this should be straight-forward
  for (round_direction round : rounding) {
    EXPECT_EQ(bits_set({}),
              HF(0.f).getRoundedNormalizedSignificand<HF>(round, &carry_bit));
    EXPECT_FALSE(carry_bit);

    EXPECT_EQ(bits_set({0}),
              HF(float_fractions({0, 1}))
                  .getRoundedNormalizedSignificand<HF>(round, &carry_bit));
    EXPECT_FALSE(carry_bit);

    EXPECT_EQ(bits_set({1, 3}),
              HF(float_fractions({0, 2, 4}))
                  .getRoundedNormalizedSignificand<HF>(round, &carry_bit));
    EXPECT_FALSE(carry_bit);

    EXPECT_EQ(
        bits_set({0, 1, 4}),
        HF(static_cast<float>(-ldexp(float_fractions({0, 1, 2, 5}), -128)))
            .getRoundedNormalizedSignificand<HF>(round, &carry_bit));
    EXPECT_FALSE(carry_bit);

    EXPECT_EQ(bits_set({0, 1, 4, 22}),
              HF(static_cast<float>(float_fractions({0, 1, 2, 5, 23})))
                  .getRoundedNormalizedSignificand<HF>(round, &carry_bit));
    EXPECT_FALSE(carry_bit);
  }
}

using RD = round_direction;
struct RoundSignificandCase {
  float source_float;
  std::pair<int16_t, bool> expected_results;
  round_direction round;
};

using HexFloatRoundTest = ::testing::TestWithParam<RoundSignificandCase>;

TEST_P(HexFloatRoundTest, RoundDownToFP16) {
  using HF = HexFloat<FloatProxy<float>>;
  using HF16 = HexFloat<FloatProxy<Float16>>;

  HF input_value(GetParam().source_float);
  bool carry_bit = false;
  EXPECT_EQ(GetParam().expected_results.first,
            input_value.getRoundedNormalizedSignificand<HF16>(GetParam().round,
                                                              &carry_bit));
  EXPECT_EQ(carry_bit, GetParam().expected_results.second);
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(F32ToF16, HexFloatRoundTest,
  ::testing::ValuesIn(std::vector<RoundSignificandCase>(
  {
    {float_fractions({0}), std::make_pair(half_bits_set({}), false), RD::kToZero},
    {float_fractions({0}), std::make_pair(half_bits_set({}), false), RD::kToNearestEven},
    {float_fractions({0}), std::make_pair(half_bits_set({}), false), RD::kToPositiveInfinity},
    {float_fractions({0}), std::make_pair(half_bits_set({}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1}), std::make_pair(half_bits_set({0}), false), RD::kToZero},

    {float_fractions({0, 1, 11}), std::make_pair(half_bits_set({0}), false), RD::kToZero},
    {float_fractions({0, 1, 11}), std::make_pair(half_bits_set({0, 9}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 11}), std::make_pair(half_bits_set({0}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 11}), std::make_pair(half_bits_set({0}), false), RD::kToNearestEven},

    {float_fractions({0, 1, 10, 11}), std::make_pair(half_bits_set({0, 9}), false), RD::kToZero},
    {float_fractions({0, 1, 10, 11}), std::make_pair(half_bits_set({0, 8}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 10, 11}), std::make_pair(half_bits_set({0, 9}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 10, 11}), std::make_pair(half_bits_set({0, 8}), false), RD::kToNearestEven},

    {float_fractions({0, 1, 11, 12}), std::make_pair(half_bits_set({0}), false), RD::kToZero},
    {float_fractions({0, 1, 11, 12}), std::make_pair(half_bits_set({0, 9}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 11, 12}), std::make_pair(half_bits_set({0}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 11, 12}), std::make_pair(half_bits_set({0, 9}), false), RD::kToNearestEven},

    {-float_fractions({0, 1, 11, 12}), std::make_pair(half_bits_set({0}), false), RD::kToZero},
    {-float_fractions({0, 1, 11, 12}), std::make_pair(half_bits_set({0}), false), RD::kToPositiveInfinity},
    {-float_fractions({0, 1, 11, 12}), std::make_pair(half_bits_set({0, 9}), false), RD::kToNegativeInfinity},
    {-float_fractions({0, 1, 11, 12}), std::make_pair(half_bits_set({0, 9}), false), RD::kToNearestEven},

    {float_fractions({0, 1, 11, 22}), std::make_pair(half_bits_set({0}), false), RD::kToZero},
    {float_fractions({0, 1, 11, 22}), std::make_pair(half_bits_set({0, 9}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 11, 22}), std::make_pair(half_bits_set({0}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 11, 22}), std::make_pair(half_bits_set({0, 9}), false), RD::kToNearestEven},

    // Carries
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(half_bits_set({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), false), RD::kToZero},
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(half_bits_set({}), true), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(half_bits_set({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(half_bits_set({}), true), RD::kToNearestEven},

    // Cases where original number was denorm. Note: this should have no effect
    // the number is pre-normalized.
    {static_cast<float>(ldexp(float_fractions({0, 1, 11, 13}), -128)), std::make_pair(half_bits_set({0}), false), RD::kToZero},
    {static_cast<float>(ldexp(float_fractions({0, 1, 11, 13}), -129)), std::make_pair(half_bits_set({0, 9}), false), RD::kToPositiveInfinity},
    {static_cast<float>(ldexp(float_fractions({0, 1, 11, 13}), -131)), std::make_pair(half_bits_set({0}), false), RD::kToNegativeInfinity},
    {static_cast<float>(ldexp(float_fractions({0, 1, 11, 13}), -130)), std::make_pair(half_bits_set({0, 9}), false), RD::kToNearestEven},
  })));

// clang-format on

// The same as bits_set but for a E4M3 value instead of 32-bit floating
// point.
uint8_t e4m3_bits_set(const std::vector<uint32_t>& bits) {
  const uint32_t top_bit = 1u << 2u;
  uint32_t val = 0;
  for (uint32_t i : bits) {
    val |= top_bit >> i;
  }
  return static_cast<uint8_t>(val);
}

struct RoundSignificandCaseE4M3 {
  float source_float;
  std::pair<int8_t, bool> expected_results;
  round_direction round;
};

using HexFloatRoundTestE4M3 =
    ::testing::TestWithParam<RoundSignificandCaseE4M3>;

TEST_P(HexFloatRoundTestE4M3, RoundDownToFPE4M3) {
  using HF = HexFloat<FloatProxy<float>>;
  using HFE4M3 = HexFloat<FloatProxy<Float8_E4M3>>;

  HF input_value(GetParam().source_float);
  bool carry_bit = false;
  EXPECT_EQ(GetParam().expected_results.first,
            input_value.getRoundedNormalizedSignificand<HFE4M3>(
                GetParam().round, &carry_bit));
  EXPECT_EQ(carry_bit, GetParam().expected_results.second);
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(F32ToE4M3, HexFloatRoundTestE4M3,
  ::testing::ValuesIn(std::vector<RoundSignificandCaseE4M3>(
  {
    {float_fractions({0}), std::make_pair(e4m3_bits_set({}), false), RD::kToZero},
    {float_fractions({0}), std::make_pair(e4m3_bits_set({}), false), RD::kToNearestEven},
    {float_fractions({0}), std::make_pair(e4m3_bits_set({}), false), RD::kToPositiveInfinity},
    {float_fractions({0}), std::make_pair(e4m3_bits_set({}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1}), std::make_pair(e4m3_bits_set({0}), false), RD::kToZero},

    {float_fractions({0, 1, 4}), std::make_pair(e4m3_bits_set({0}), false), RD::kToZero},
    {float_fractions({0, 1, 4}), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 4}), std::make_pair(e4m3_bits_set({0}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 4}), std::make_pair(e4m3_bits_set({0}), false), RD::kToNearestEven},

    {float_fractions({0, 1, 3, 4}), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToZero},
    {float_fractions({0, 1, 3, 4}), std::make_pair(e4m3_bits_set({0, 1}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 3, 4}), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 3, 4}), std::make_pair(e4m3_bits_set({0, 1}), false), RD::kToNearestEven},

    {float_fractions({0, 1, 4, 5}), std::make_pair(e4m3_bits_set({0}), false), RD::kToZero},
    {float_fractions({0, 1, 4, 5}), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 4, 5}), std::make_pair(e4m3_bits_set({0}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 4, 5}), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToNearestEven},

    {-float_fractions({0, 1, 4, 5}), std::make_pair(e4m3_bits_set({0}), false), RD::kToZero},
    {-float_fractions({0, 1, 4, 5}), std::make_pair(e4m3_bits_set({0}), false), RD::kToPositiveInfinity},
    {-float_fractions({0, 1, 4, 5}), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToNegativeInfinity},
    {-float_fractions({0, 1, 4, 5}), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToNearestEven},

    {float_fractions({0, 1, 4, 22}), std::make_pair(e4m3_bits_set({0}), false), RD::kToZero},
    {float_fractions({0, 1, 4, 22}), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 4, 22}), std::make_pair(e4m3_bits_set({0}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 4, 22}), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToNearestEven},

    // Carries
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(e4m3_bits_set({0, 1, 2}), false), RD::kToZero},
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(e4m3_bits_set({}), true), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(e4m3_bits_set({0, 1, 2}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(e4m3_bits_set({}), true), RD::kToNearestEven},

    // Cases where original number was denorm. Note: this should have no effect
    // the number is pre-normalized.
    {static_cast<float>(ldexp(float_fractions({0, 1, 4, 6}), -128)), std::make_pair(e4m3_bits_set({0}), false), RD::kToZero},
    {static_cast<float>(ldexp(float_fractions({0, 1, 4, 6}), -129)), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToPositiveInfinity},
    {static_cast<float>(ldexp(float_fractions({0, 1, 4, 6}), -131)), std::make_pair(e4m3_bits_set({0}), false), RD::kToNegativeInfinity},
    {static_cast<float>(ldexp(float_fractions({0, 1, 4, 6}), -130)), std::make_pair(e4m3_bits_set({0, 2}), false), RD::kToNearestEven},
  })));

// clang-format on
// The same as bits_set but for a E4M3 value instead of 32-bit floating
// point.
uint8_t e5m2_bits_set(const std::vector<uint32_t>& bits) {
  const uint32_t top_bit = 1u << 1u;
  uint32_t val = 0;
  for (uint32_t i : bits) {
    val |= top_bit >> i;
  }
  return static_cast<uint8_t>(val);
}

struct RoundSignificandCaseE5M2 {
  float source_float;
  std::pair<int8_t, bool> expected_results;
  round_direction round;
};

using HexFloatRoundTestE5M2 =
    ::testing::TestWithParam<RoundSignificandCaseE5M2>;

TEST_P(HexFloatRoundTestE5M2, RoundDownToFPE4M3) {
  using HF = HexFloat<FloatProxy<float>>;
  using HFE5M2 = HexFloat<FloatProxy<Float8_E5M2>>;

  HF input_value(GetParam().source_float);
  bool carry_bit = false;
  EXPECT_EQ(GetParam().expected_results.first,
            input_value.getRoundedNormalizedSignificand<HFE5M2>(
                GetParam().round, &carry_bit));
  EXPECT_EQ(carry_bit, GetParam().expected_results.second);
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(F32ToE5M2, HexFloatRoundTestE5M2,
  ::testing::ValuesIn(std::vector<RoundSignificandCaseE5M2>(
  {
    {float_fractions({0}), std::make_pair(e5m2_bits_set({}), false), RD::kToZero},
    {float_fractions({0}), std::make_pair(e5m2_bits_set({}), false), RD::kToNearestEven},
    {float_fractions({0}), std::make_pair(e5m2_bits_set({}), false), RD::kToPositiveInfinity},
    {float_fractions({0}), std::make_pair(e5m2_bits_set({}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1}), std::make_pair(e5m2_bits_set({0}), false), RD::kToZero},

    {float_fractions({0, 1, 4}), std::make_pair(e5m2_bits_set({0}), false), RD::kToZero},
    {float_fractions({0, 1, 4}), std::make_pair(e5m2_bits_set({0, 1}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 4}), std::make_pair(e5m2_bits_set({0}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 4}), std::make_pair(e5m2_bits_set({0}), false), RD::kToNearestEven},

    {float_fractions({0, 3, 4}), std::make_pair(e5m2_bits_set({}), false), RD::kToZero},
    {float_fractions({0, 3, 4}), std::make_pair(e5m2_bits_set({1}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 3, 4}), std::make_pair(e5m2_bits_set({}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 3, 4}), std::make_pair(e5m2_bits_set({1}), false), RD::kToNearestEven},

    {float_fractions({0, 2, 3}), std::make_pair(e5m2_bits_set({1}), false), RD::kToZero},
    {float_fractions({0, 2, 3}), std::make_pair(e5m2_bits_set({0}), false), RD::kToPositiveInfinity},
    {float_fractions({0, 2, 3}), std::make_pair(e5m2_bits_set({1}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 2, 3}), std::make_pair(e5m2_bits_set({0}), false), RD::kToNearestEven},

    {-float_fractions({0, 2, 3}), std::make_pair(e5m2_bits_set({1}), false), RD::kToZero},
    {-float_fractions({0, 2, 3}), std::make_pair(e5m2_bits_set({1}), false), RD::kToPositiveInfinity},
    {-float_fractions({0, 2, 3}), std::make_pair(e5m2_bits_set({0}), false), RD::kToNegativeInfinity},
    {-float_fractions({0, 2, 3}), std::make_pair(e5m2_bits_set({0}), false), RD::kToNearestEven},

    // Carries
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(e5m2_bits_set({0, 1}), false), RD::kToZero},
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(e5m2_bits_set({}), true), RD::kToPositiveInfinity},
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(e5m2_bits_set({0, 1}), false), RD::kToNegativeInfinity},
    {float_fractions({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}), std::make_pair(e5m2_bits_set({}), true), RD::kToNearestEven},

    // Cases where original number was denorm. Note: this should have no effect
    // the number is pre-normalized.
    {static_cast<float>(ldexp(float_fractions({0, 3, 6}), -128)), std::make_pair(e5m2_bits_set({}), false), RD::kToZero},
    {static_cast<float>(ldexp(float_fractions({0, 3, 6}), -129)), std::make_pair(e5m2_bits_set({1}), false), RD::kToPositiveInfinity},
    {static_cast<float>(ldexp(float_fractions({0, 3, 6}), -131)), std::make_pair(e5m2_bits_set({}), false), RD::kToNegativeInfinity},
    {static_cast<float>(ldexp(float_fractions({0, 3, 6}), -130)), std::make_pair(e5m2_bits_set({1}), false), RD::kToNearestEven},
  })));
// clang-format on

struct UpCastSignificandCase {
  uint16_t source_half;
  uint32_t expected_result;
};

using HexFloatRoundUpSignificandTest =
    ::testing::TestWithParam<UpCastSignificandCase>;
TEST_P(HexFloatRoundUpSignificandTest, Widening) {
  using HF = HexFloat<FloatProxy<float>>;
  using HF16 = HexFloat<FloatProxy<Float16>>;
  bool carry_bit = false;

  round_direction rounding[] = {round_direction::kToZero,
                                round_direction::kToNearestEven,
                                round_direction::kToPositiveInfinity,
                                round_direction::kToNegativeInfinity};

  // Everything fits, so everything should just be bit-shifts.
  for (round_direction round : rounding) {
    carry_bit = false;
    HF16 input_value(GetParam().source_half);
    EXPECT_EQ(
        GetParam().expected_result,
        input_value.getRoundedNormalizedSignificand<HF>(round, &carry_bit))
        << std::hex << "0x"
        << input_value.getRoundedNormalizedSignificand<HF>(round, &carry_bit)
        << "  0x" << GetParam().expected_result;
    EXPECT_FALSE(carry_bit);
  }
}

INSTANTIATE_TEST_SUITE_P(
    F16toF32, HexFloatRoundUpSignificandTest,
    // 0xFC00 of the source 16-bit hex value cover the sign and the exponent.
    // They are ignored for this test.
    ::testing::ValuesIn(std::vector<UpCastSignificandCase>({
        {0x3F00, 0x600000},
        {0x0F00, 0x600000},
        {0x0F01, 0x602000},
        {0x0FFF, 0x7FE000},
    })));

struct DownCastTest {
  float source_float;
  uint16_t expected_half;
  std::vector<round_direction> directions;
};

std::string get_round_text(round_direction direction) {
#define CASE(round_direction) \
  case round_direction:       \
    return #round_direction

  switch (direction) {
    CASE(round_direction::kToZero);
    CASE(round_direction::kToPositiveInfinity);
    CASE(round_direction::kToNegativeInfinity);
    CASE(round_direction::kToNearestEven);
  }
#undef CASE
  return "";
}

using HexFloatFP32To16Tests = ::testing::TestWithParam<DownCastTest>;

TEST_P(HexFloatFP32To16Tests, NarrowingCasts) {
  using HF = HexFloat<FloatProxy<float>>;
  using HF16 = HexFloat<FloatProxy<Float16>>;
  HF f(GetParam().source_float);
  for (auto round : GetParam().directions) {
    HF16 half(0);
    f.castTo(half, round);
    EXPECT_EQ(GetParam().expected_half, half.value().getAsFloat().get_value())
        << get_round_text(round) << "  " << std::hex
        << BitwiseCast<uint32_t>(GetParam().source_float)
        << " cast to: " << half.value().getAsFloat().get_value();
  }
}

const uint16_t positive_infinity = 0x7C00;
const uint16_t negative_infinity = 0xFC00;

INSTANTIATE_TEST_SUITE_P(
    F32ToF16, HexFloatFP32To16Tests,
    ::testing::ValuesIn(std::vector<DownCastTest>({
        // Exactly representable as half.
        {0.f,
         0x0,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-0.f,
         0x8000,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {1.0f,
         0x3C00,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-1.0f,
         0xBC00,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        {float_fractions({0, 1, 10}),
         0x3E01,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-float_fractions({0, 1, 10}),
         0xBE01,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(ldexp(float_fractions({0, 1, 10}), 3)),
         0x4A01,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(float_fractions({0, 1, 10}), 3)),
         0xCA01,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Underflow
        {static_cast<float>(ldexp(1.0f, -25)),
         0x0,
         {RD::kToZero, RD::kToNegativeInfinity, RD::kToNearestEven}},
        {static_cast<float>(ldexp(1.0f, -25)), 0x1, {RD::kToPositiveInfinity}},
        {static_cast<float>(-ldexp(1.0f, -25)),
         0x8000,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.0f, -25)),
         0x8001,
         {RD::kToNegativeInfinity}},
        {static_cast<float>(ldexp(1.0f, -24)),
         0x1,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Overflow
        {static_cast<float>(ldexp(1.0f, 16)),
         positive_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(ldexp(1.0f, 18)),
         positive_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(ldexp(1.3f, 16)),
         positive_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.0f, 16)),
         negative_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.0f, 18)),
         negative_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.3f, 16)),
         negative_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Transfer of Infinities
        {std::numeric_limits<float>::infinity(),
         positive_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-std::numeric_limits<float>::infinity(),
         negative_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Nans are below because we cannot test for equality.
    })));

using HexFloatFP32ToE4M3Tests = ::testing::TestWithParam<DownCastTest>;

TEST_P(HexFloatFP32ToE4M3Tests, NarrowingCasts) {
  using HF = HexFloat<FloatProxy<float>>;
  using HFE4M3 = HexFloat<FloatProxy<Float8_E4M3>>;
  HF f(GetParam().source_float);
  for (auto round : GetParam().directions) {
    HFE4M3 e4m3(0);
    f.castTo(e4m3, round);
    EXPECT_EQ(GetParam().expected_half, e4m3.value().getAsFloat().get_value())
        << get_round_text(round) << "  " << std::hex
        << BitwiseCast<uint32_t>(GetParam().source_float)
        << " cast to: " << (uint32_t)e4m3.value().getAsFloat().get_value();
  }
}

INSTANTIATE_TEST_SUITE_P(
    F32ToE4M3, HexFloatFP32ToE4M3Tests,
    ::testing::ValuesIn(std::vector<DownCastTest>({
        // Exactly representable as half.
        {0.f,
         0x0,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-0.f,
         0x80,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {1.0f,
         0x38,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-1.0f,
         0xB8,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        {float_fractions({0, 1, 3}),
         0x3D,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-float_fractions({0, 1, 3}),
         0xBD,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(ldexp(float_fractions({0, 1, 3}), 3)),
         0x55,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(float_fractions({0, 1, 3}), 3)),
         0xD5,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Underflow
        {static_cast<float>(ldexp(1.0f, -10)),
         0x0,
         {RD::kToZero, RD::kToNegativeInfinity, RD::kToNearestEven}},
        {static_cast<float>(ldexp(1.0f, -10)), 0x1, {RD::kToPositiveInfinity}},
        {static_cast<float>(-ldexp(1.0f, -10)),
         0x80,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.0f, -9)), 0x81, {RD::kToNegativeInfinity}},
        {static_cast<float>(ldexp(1.0f, -9)),
         0x1,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Overflow
        {static_cast<float>(ldexp(1.0f, 9)),
         Float8_E4M3::max().get_value(),
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(ldexp(1.0f, 10)),
         Float8_E4M3::max().get_value(),
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(ldexp(1.3f, 9)),
         Float8_E4M3::max().get_value(),
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.0f, 9)),
         static_cast<uint16_t>(0x80 | Float8_E4M3::max().get_value()),
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.0f, 10)),
         static_cast<uint16_t>(0x80 | Float8_E4M3::max().get_value()),
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.3f, 9)),
         static_cast<uint16_t>(0x80 | Float8_E4M3::max().get_value()),
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Transfer of Infinities
        {std::numeric_limits<float>::infinity(),
         Float8_E4M3::max().get_value(),
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-std::numeric_limits<float>::infinity(),
         static_cast<uint16_t>(0x80 | Float8_E4M3::max().get_value()),
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Nans are below because we cannot test for equality.
    })));

using HexFloatFP32ToE5M2Tests = ::testing::TestWithParam<DownCastTest>;

TEST_P(HexFloatFP32ToE5M2Tests, NarrowingCasts) {
  using HF = HexFloat<FloatProxy<float>>;
  using HFE5M2 = HexFloat<FloatProxy<Float8_E5M2>>;
  HF f(GetParam().source_float);
  for (auto round : GetParam().directions) {
    HFE5M2 e5m2(0);
    f.castTo(e5m2, round);
    EXPECT_EQ(GetParam().expected_half, e5m2.value().getAsFloat().get_value())
        << get_round_text(round) << "  " << std::hex
        << BitwiseCast<uint32_t>(GetParam().source_float)
        << " cast to: " << (uint32_t)e5m2.value().getAsFloat().get_value();
  }
}

const uint8_t e5m2_positive_infinity = 0x7C;
const uint8_t e5m2_negative_infinity = 0xFC;

INSTANTIATE_TEST_SUITE_P(
    F32ToE5M2, HexFloatFP32ToE5M2Tests,
    ::testing::ValuesIn(std::vector<DownCastTest>({
        // Exactly representable as half.
        {0.f,
         0x0,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-0.f,
         0x80,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {1.0f,
         0x3C,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-1.0f,
         0xBC,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        {float_fractions({0, 1, 2}),
         0x3F,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-float_fractions({0, 1, 2}),
         0xBF,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(ldexp(float_fractions({0, 2}), 3)),
         0x49,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(float_fractions({0, 2}), 3)),
         0xC9,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Underflow
        {static_cast<float>(ldexp(1.0f, -17)),
         0x0,
         {RD::kToZero, RD::kToNegativeInfinity, RD::kToNearestEven}},
        {static_cast<float>(ldexp(1.0f, -17)), 0x1, {RD::kToPositiveInfinity}},
        {static_cast<float>(-ldexp(1.0f, -17)),
         0x80,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.0f, -16)),
         0x81,
         {RD::kToNegativeInfinity}},
        {static_cast<float>(ldexp(1.0f, -16)),
         0x1,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Overflow
        {static_cast<float>(ldexp(1.0f, 16)),
         e5m2_positive_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(ldexp(1.0f, 17)),
         e5m2_positive_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(ldexp(1.3f, 16)),
         e5m2_positive_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.0f, 16)),
         e5m2_negative_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.0f, 17)),
         e5m2_negative_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {static_cast<float>(-ldexp(1.3f, 16)),
         e5m2_negative_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Transfer of Infinities
        {std::numeric_limits<float>::infinity(),
         e5m2_positive_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},
        {-std::numeric_limits<float>::infinity(),
         e5m2_negative_infinity,
         {RD::kToZero, RD::kToPositiveInfinity, RD::kToNegativeInfinity,
          RD::kToNearestEven}},

        // Nans are below because we cannot test for equality.
    })));

struct UpCastCase {
  uint16_t source_half;
  float expected_float;
};

using HexFloatFP16To32Tests = ::testing::TestWithParam<UpCastCase>;
TEST_P(HexFloatFP16To32Tests, WideningCasts) {
  using HF = HexFloat<FloatProxy<float>>;
  using HF16 = HexFloat<FloatProxy<Float16>>;
  HF16 f(GetParam().source_half);

  round_direction rounding[] = {round_direction::kToZero,
                                round_direction::kToNearestEven,
                                round_direction::kToPositiveInfinity,
                                round_direction::kToNegativeInfinity};

  // Everything fits, so everything should just be bit-shifts.
  for (round_direction round : rounding) {
    HF flt(0.f);
    f.castTo(flt, round);
    EXPECT_EQ(GetParam().expected_float, flt.value().getAsFloat())
        << get_round_text(round) << "  " << std::hex
        << BitwiseCast<uint16_t>(GetParam().source_half)
        << " cast to: " << flt.value().getAsFloat();
  }
}

INSTANTIATE_TEST_SUITE_P(
    F16ToF32, HexFloatFP16To32Tests,
    ::testing::ValuesIn(std::vector<UpCastCase>({
        {0x0000, 0.f},
        {0x8000, -0.f},
        {0x3C00, 1.0f},
        {0xBC00, -1.0f},
        {0x3F00, float_fractions({0, 1, 2})},
        {0xBF00, -float_fractions({0, 1, 2})},
        {0x3F01, float_fractions({0, 1, 2, 10})},
        {0xBF01, -float_fractions({0, 1, 2, 10})},

        // denorm
        {0x0001, static_cast<float>(ldexp(1.0, -24))},
        {0x0002, static_cast<float>(ldexp(1.0, -23))},
        {0x8001, static_cast<float>(-ldexp(1.0, -24))},
        {0x8011, static_cast<float>(-ldexp(1.0, -20) + -ldexp(1.0, -24))},

        // inf
        {0x7C00, std::numeric_limits<float>::infinity()},
        {0xFC00, -std::numeric_limits<float>::infinity()},
    })));

TEST(HexFloatOperationTests, NanTests) {
  using HF = HexFloat<FloatProxy<float>>;
  using HF16 = HexFloat<FloatProxy<Float16>>;
  using FE4M3 = HexFloat<FloatProxy<Float8_E4M3>>;
  using FE5M2 = HexFloat<FloatProxy<Float8_E5M2>>;
  round_direction rounding[] = {round_direction::kToZero,
                                round_direction::kToNearestEven,
                                round_direction::kToPositiveInfinity,
                                round_direction::kToNegativeInfinity};

  // Everything fits, so everything should just be bit-shifts.
  for (round_direction round : rounding) {
    HF16 f16(0);
    HF f(0.f);
    FE4M3 fe4m3(0);
    FE5M2 fe5m2(0);
    HF(std::numeric_limits<float>::quiet_NaN()).castTo(f16, round);
    EXPECT_TRUE(f16.value().isNan());
    HF(std::numeric_limits<float>::signaling_NaN()).castTo(f16, round);
    EXPECT_TRUE(f16.value().isNan());

    HF16(0x7C01).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    HF16(0x7C11).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    HF16(0xFC01).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    HF16(0x7C10).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    HF16(0xFF00).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());

    HF(std::numeric_limits<float>::quiet_NaN()).castTo(fe4m3, round);
    EXPECT_TRUE(fe4m3.value().isNan());
    HF(std::numeric_limits<float>::signaling_NaN()).castTo(fe4m3, round);
    EXPECT_TRUE(fe4m3.value().isNan());

    FE4M3(0x7F).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    FE4M3(0xFF).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());

    HF(std::numeric_limits<float>::quiet_NaN()).castTo(fe5m2, round);
    EXPECT_TRUE(fe5m2.value().isNan());
    HF(std::numeric_limits<float>::signaling_NaN()).castTo(fe5m2, round);
    EXPECT_TRUE(fe5m2.value().isNan());

    FE5M2(0x7D).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    FE5M2(0x7E).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    FE5M2(0x7F).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    FE5M2(0xFD).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    FE5M2(0xFE).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
    FE5M2(0xFF).castTo(f, round);
    EXPECT_TRUE(f.value().isNan());
  }
}

// A test case for parsing good and bad HexFloat<FloatProxy<T>> literals.
template <typename T>
struct FloatParseCase {
  std::string literal;
  bool negate_value;
  bool expect_success;
  HexFloat<FloatProxy<T>> expected_value;
};

using ParseNormalFloatTest = ::testing::TestWithParam<FloatParseCase<float>>;

TEST_P(ParseNormalFloatTest, Samples) {
  std::stringstream input(GetParam().literal);
  HexFloat<FloatProxy<float>> parsed_value(0.0f);
  ParseNormalFloat(input, GetParam().negate_value, parsed_value);
  EXPECT_NE(GetParam().expect_success, input.fail())
      << " literal: " << GetParam().literal
      << " negate: " << GetParam().negate_value;
  if (GetParam().expect_success) {
    EXPECT_THAT(parsed_value.value(), Eq(GetParam().expected_value.value()))
        << " literal: " << GetParam().literal
        << " negate: " << GetParam().negate_value;
  }
}

// Returns a FloatParseCase with expected failure.
template <typename T>
FloatParseCase<T> BadFloatParseCase(std::string literal, bool negate_value,
                                    T expected_value) {
  HexFloat<FloatProxy<T>> proxy_expected_value(expected_value);
  return FloatParseCase<T>{literal, negate_value, false, proxy_expected_value};
}

// Returns a FloatParseCase that should successfully parse to a given value.
template <typename T>
FloatParseCase<T> GoodFloatParseCase(std::string literal, bool negate_value,
                                     T expected_value) {
  HexFloat<FloatProxy<T>> proxy_expected_value(expected_value);
  return FloatParseCase<T>{literal, negate_value, true, proxy_expected_value};
}

INSTANTIATE_TEST_SUITE_P(
    FloatParse, ParseNormalFloatTest,
    ::testing::ValuesIn(std::vector<FloatParseCase<float>>{
        // Failing cases due to trivially incorrect syntax.
        BadFloatParseCase("abc", false, 0.0f),
        BadFloatParseCase("abc", true, 0.0f),

        // Valid cases.
        GoodFloatParseCase("0", false, 0.0f),
        GoodFloatParseCase("0.0", false, 0.0f),
        GoodFloatParseCase("-0.0", false, -0.0f),
        GoodFloatParseCase("2.0", false, 2.0f),
        GoodFloatParseCase("-2.0", false, -2.0f),
        GoodFloatParseCase("+2.0", false, 2.0f),
        // Cases with negate_value being true.
        GoodFloatParseCase("0.0", true, -0.0f),
        GoodFloatParseCase("2.0", true, -2.0f),

        // When negate_value is true, we should not accept a
        // leading minus or plus.
        BadFloatParseCase("-0.0", true, 0.0f),
        BadFloatParseCase("-2.0", true, 0.0f),
        BadFloatParseCase("+0.0", true, 0.0f),
        BadFloatParseCase("+2.0", true, 0.0f),

        // Overflow is an error for 32-bit float parsing.
        BadFloatParseCase("1e40", false, FLT_MAX),
        BadFloatParseCase("1e40", true, -FLT_MAX),
        BadFloatParseCase("-1e40", false, -FLT_MAX),
        // We can't have -1e40 and negate_value == true since
        // that represents an original case of "--1e40" which
        // is invalid.
    }));

using ParseNormalFloat16Test =
    ::testing::TestWithParam<FloatParseCase<Float16>>;

TEST_P(ParseNormalFloat16Test, Samples) {
  std::stringstream input(GetParam().literal);
  HexFloat<FloatProxy<Float16>> parsed_value(0);
  ParseNormalFloat(input, GetParam().negate_value, parsed_value);
  EXPECT_NE(GetParam().expect_success, input.fail())
      << " literal: " << GetParam().literal
      << " negate: " << GetParam().negate_value;
  if (GetParam().expect_success) {
    EXPECT_THAT(parsed_value.value(), Eq(GetParam().expected_value.value()))
        << " literal: " << GetParam().literal
        << " negate: " << GetParam().negate_value;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Float16Parse, ParseNormalFloat16Test,
    ::testing::ValuesIn(std::vector<FloatParseCase<Float16>>{
        // Failing cases due to trivially incorrect syntax.
        BadFloatParseCase<Float16>("abc", false, uint16_t{0}),
        BadFloatParseCase<Float16>("abc", true, uint16_t{0}),

        // Valid cases.
        GoodFloatParseCase<Float16>("0", false, uint16_t{0}),
        GoodFloatParseCase<Float16>("0.0", false, uint16_t{0}),
        GoodFloatParseCase<Float16>("-0.0", false, uint16_t{0x8000}),
        GoodFloatParseCase<Float16>("2.0", false, uint16_t{0x4000}),
        GoodFloatParseCase<Float16>("-2.0", false, uint16_t{0xc000}),
        GoodFloatParseCase<Float16>("+2.0", false, uint16_t{0x4000}),
        // Cases with negate_value being true.
        GoodFloatParseCase<Float16>("0.0", true, uint16_t{0x8000}),
        GoodFloatParseCase<Float16>("2.0", true, uint16_t{0xc000}),

        // When negate_value is true, we should not accept a leading minus or
        // plus.
        BadFloatParseCase<Float16>("-0.0", true, uint16_t{0}),
        BadFloatParseCase<Float16>("-2.0", true, uint16_t{0}),
        BadFloatParseCase<Float16>("+0.0", true, uint16_t{0}),
        BadFloatParseCase<Float16>("+2.0", true, uint16_t{0}),
    }));

using ParseNormalFloatE4M3Test =
    ::testing::TestWithParam<FloatParseCase<Float8_E4M3>>;

TEST_P(ParseNormalFloatE4M3Test, Samples) {
  std::stringstream input(GetParam().literal);
  HexFloat<FloatProxy<Float8_E4M3>> parsed_value(0);
  ParseNormalFloat(input, GetParam().negate_value, parsed_value);
  EXPECT_NE(GetParam().expect_success, input.fail())
      << " literal: " << GetParam().literal
      << " negate: " << GetParam().negate_value;
  if (GetParam().expect_success) {
    EXPECT_THAT(parsed_value.value(), Eq(GetParam().expected_value.value()))
        << " literal: " << GetParam().literal
        << " negate: " << GetParam().negate_value;
  }
}

INSTANTIATE_TEST_SUITE_P(
    FloatE4M3Parse, ParseNormalFloatE4M3Test,
    ::testing::ValuesIn(std::vector<FloatParseCase<Float8_E4M3>>{
        // Failing cases due to trivially incorrect syntax.
        BadFloatParseCase<Float8_E4M3>("abc", false, uint8_t{0}),
        BadFloatParseCase<Float8_E4M3>("abc", true, uint8_t{0}),

        // Valid cases.
        GoodFloatParseCase<Float8_E4M3>("0", false, uint8_t{0}),
        GoodFloatParseCase<Float8_E4M3>("0.0", false, uint8_t{0}),
        GoodFloatParseCase<Float8_E4M3>("-0.0", false, uint8_t{0x80}),
        GoodFloatParseCase<Float8_E4M3>("2.0", false, uint8_t{0x40}),
        GoodFloatParseCase<Float8_E4M3>("-2.0", false, uint8_t{0xc0}),
        GoodFloatParseCase<Float8_E4M3>("+2.0", false, uint8_t{0x40}),
        // Cases with negate_value being true.
        GoodFloatParseCase<Float8_E4M3>("0.0", true, uint8_t{0x80}),
        GoodFloatParseCase<Float8_E4M3>("2.0", true, uint8_t{0xc0}),

        // When negate_value is true, we should not accept a leading minus or
        // plus.
        BadFloatParseCase<Float8_E4M3>("-0.0", true, uint8_t{0}),
        BadFloatParseCase<Float8_E4M3>("-2.0", true, uint8_t{0}),
        BadFloatParseCase<Float8_E4M3>("+0.0", true, uint8_t{0}),
        BadFloatParseCase<Float8_E4M3>("+2.0", true, uint8_t{0}),
    }));

using ParseNormalFloatE5M2Test =
    ::testing::TestWithParam<FloatParseCase<Float8_E5M2>>;

TEST_P(ParseNormalFloatE5M2Test, Samples) {
  std::stringstream input(GetParam().literal);
  HexFloat<FloatProxy<Float8_E5M2>> parsed_value(0);
  ParseNormalFloat(input, GetParam().negate_value, parsed_value);
  EXPECT_NE(GetParam().expect_success, input.fail())
      << " literal: " << GetParam().literal
      << " negate: " << GetParam().negate_value;
  if (GetParam().expect_success) {
    EXPECT_THAT(parsed_value.value(), Eq(GetParam().expected_value.value()))
        << " literal: " << GetParam().literal
        << " negate: " << GetParam().negate_value;
  }
}

INSTANTIATE_TEST_SUITE_P(
    FloatE5M2Parse, ParseNormalFloatE5M2Test,
    ::testing::ValuesIn(std::vector<FloatParseCase<Float8_E5M2>>{
        // Failing cases due to trivially incorrect syntax.
        BadFloatParseCase<Float8_E5M2>("abc", false, uint8_t{0}),
        BadFloatParseCase<Float8_E5M2>("abc", true, uint8_t{0}),

        // Valid cases.
        GoodFloatParseCase<Float8_E5M2>("0", false, uint8_t{0}),
        GoodFloatParseCase<Float8_E5M2>("0.0", false, uint8_t{0}),
        GoodFloatParseCase<Float8_E5M2>("-0.0", false, uint8_t{0x80}),
        GoodFloatParseCase<Float8_E5M2>("2.0", false, uint8_t{0x40}),
        GoodFloatParseCase<Float8_E5M2>("-2.0", false, uint8_t{0xc0}),
        GoodFloatParseCase<Float8_E5M2>("+2.0", false, uint8_t{0x40}),
        // Cases with negate_value being true.
        GoodFloatParseCase<Float8_E5M2>("0.0", true, uint8_t{0x80}),
        GoodFloatParseCase<Float8_E5M2>("2.0", true, uint8_t{0xc0}),

        // When negate_value is true, we should not accept a leading minus or
        // plus.
        BadFloatParseCase<Float8_E5M2>("-0.0", true, uint8_t{0}),
        BadFloatParseCase<Float8_E5M2>("-2.0", true, uint8_t{0}),
        BadFloatParseCase<Float8_E5M2>("+0.0", true, uint8_t{0}),
        BadFloatParseCase<Float8_E5M2>("+2.0", true, uint8_t{0}),
    }));

// A test case for detecting infinities.
template <typename T>
struct OverflowParseCase {
  std::string input;
  bool expect_success;
  T expected_value;
};

using FloatProxyParseOverflowFloatTest =
    ::testing::TestWithParam<OverflowParseCase<float>>;

TEST_P(FloatProxyParseOverflowFloatTest, Sample) {
  std::istringstream input(GetParam().input);
  HexFloat<FloatProxy<float>> value(0.0f);
  input >> value;
  EXPECT_NE(GetParam().expect_success, input.fail());
  if (GetParam().expect_success) {
    EXPECT_THAT(value.value().getAsFloat(), GetParam().expected_value);
  }
}

INSTANTIATE_TEST_SUITE_P(
    FloatOverflow, FloatProxyParseOverflowFloatTest,
    ::testing::ValuesIn(std::vector<OverflowParseCase<float>>({
        {"0", true, 0.0f},
        {"0.0", true, 0.0f},
        {"1.0", true, 1.0f},
        {"1e38", true, 1e38f},
        {"-1e38", true, -1e38f},
        {"1e40", false, FLT_MAX},
        {"-1e40", false, -FLT_MAX},
        {"1e400", false, FLT_MAX},
        {"-1e400", false, -FLT_MAX},
    })));

using FloatProxyParseOverflowDoubleTest =
    ::testing::TestWithParam<OverflowParseCase<double>>;

TEST_P(FloatProxyParseOverflowDoubleTest, Sample) {
  std::istringstream input(GetParam().input);
  HexFloat<FloatProxy<double>> value(0.0);
  input >> value;
  EXPECT_NE(GetParam().expect_success, input.fail());
  if (GetParam().expect_success) {
    EXPECT_THAT(value.value().getAsFloat(), Eq(GetParam().expected_value));
  }
}

INSTANTIATE_TEST_SUITE_P(
    DoubleOverflow, FloatProxyParseOverflowDoubleTest,
    ::testing::ValuesIn(std::vector<OverflowParseCase<double>>({
        {"0", true, 0.0},
        {"0.0", true, 0.0},
        {"1.0", true, 1.0},
        {"1e38", true, 1e38},
        {"-1e38", true, -1e38},
        {"1e40", true, 1e40},
        {"-1e40", true, -1e40},
        {"1e400", false, DBL_MAX},
        {"-1e400", false, -DBL_MAX},
    })));

using FloatProxyParseOverflowFloat16Test =
    ::testing::TestWithParam<OverflowParseCase<uint16_t>>;

TEST_P(FloatProxyParseOverflowFloat16Test, Sample) {
  std::istringstream input(GetParam().input);
  HexFloat<FloatProxy<Float16>> value(0);
  input >> value;
  EXPECT_NE(GetParam().expect_success, input.fail())
      << " literal: " << GetParam().input;
  if (GetParam().expect_success) {
    EXPECT_THAT(value.value().data(), Eq(GetParam().expected_value))
        << " literal: " << GetParam().input;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Float16Overflow, FloatProxyParseOverflowFloat16Test,
    ::testing::ValuesIn(std::vector<OverflowParseCase<uint16_t>>({
        {"0", true, uint16_t{0}},
        {"0.0", true, uint16_t{0}},
        {"1.0", true, uint16_t{0x3c00}},
        // Overflow for 16-bit float is an error, and returns max or
        // lowest value.
        {"1e38", false, uint16_t{0x7bff}},
        {"1e40", false, uint16_t{0x7bff}},
        {"1e400", false, uint16_t{0x7bff}},
        {"-1e38", false, uint16_t{0xfbff}},
        {"-1e40", false, uint16_t{0xfbff}},
        {"-1e400", false, uint16_t{0xfbff}},
    })));

using FloatProxyParseOverflowFloatE4M3Test =
    ::testing::TestWithParam<OverflowParseCase<uint8_t>>;

TEST_P(FloatProxyParseOverflowFloatE4M3Test, Sample) {
  std::istringstream input(GetParam().input);
  HexFloat<FloatProxy<Float8_E4M3>> value(0);
  input >> value;
  EXPECT_NE(GetParam().expect_success, input.fail())
      << " literal: " << GetParam().input;
  if (GetParam().expect_success) {
    EXPECT_THAT(value.value().data(), Eq(GetParam().expected_value))
        << " literal: " << GetParam().input;
  }
}

INSTANTIATE_TEST_SUITE_P(
    FloatE4M3Overflow, FloatProxyParseOverflowFloatE4M3Test,
    ::testing::ValuesIn(std::vector<OverflowParseCase<uint8_t>>({
        {"0", true, uint8_t{0}},
        {"0.0", true, uint8_t{0}},
        {"1.0", true, uint8_t{0x38}},
        // Overflow for E4M3 float is an error, and returns max or
        // lowest value.
        {"1e38", false, uint8_t{0x7e}},
        {"1e40", false, uint8_t{0x7e}},
        {"1e400", false, uint8_t{0x7e}},
        {"-1e38", false, uint8_t{0xfe}},
        {"-1e40", false, uint8_t{0xfe}},
        {"-1e400", false, uint8_t{0xfe}},
    })));

using FloatProxyParseOverflowFloatE5M2Test =
    ::testing::TestWithParam<OverflowParseCase<uint8_t>>;

TEST_P(FloatProxyParseOverflowFloatE5M2Test, Sample) {
  std::istringstream input(GetParam().input);
  HexFloat<FloatProxy<Float8_E5M2>> value(0);
  input >> value;
  EXPECT_NE(GetParam().expect_success, input.fail())
      << " literal: " << GetParam().input;
  if (GetParam().expect_success) {
    EXPECT_THAT(value.value().data(), Eq(GetParam().expected_value))
        << " literal: " << GetParam().input;
  }
}

INSTANTIATE_TEST_SUITE_P(
    FloatE5M2Overflow, FloatProxyParseOverflowFloatE5M2Test,
    ::testing::ValuesIn(std::vector<OverflowParseCase<uint8_t>>({
        {"0", true, uint8_t{0}},
        {"0.0", true, uint8_t{0}},
        {"1.0", true, uint8_t{0x3c}},
        // Overflow for E5M2 float is an error, and returns max or
        // lowest value.
        {"1e38", false, uint8_t{0x7b}},
        {"1e40", false, uint8_t{0x7b}},
        {"1e400", false, uint8_t{0x7b}},
        {"-1e38", false, uint8_t{0xfb}},
        {"-1e40", false, uint8_t{0xfb}},
        {"-1e400", false, uint8_t{0xfb}},
    })));

TEST(FloatProxy, Max) {
  EXPECT_THAT(FloatProxy<Float16>::max().getAsFloat().get_value(),
              Eq(uint16_t{0x7bff}));
  EXPECT_THAT(FloatProxy<float>::max().getAsFloat(),
              Eq(std::numeric_limits<float>::max()));
  EXPECT_THAT(FloatProxy<double>::max().getAsFloat(),
              Eq(std::numeric_limits<double>::max()));
}

TEST(FloatProxy, Lowest) {
  EXPECT_THAT(FloatProxy<Float16>::lowest().getAsFloat().get_value(),
              Eq(uint16_t{0xfbff}));
  EXPECT_THAT(FloatProxy<float>::lowest().getAsFloat(),
              Eq(std::numeric_limits<float>::lowest()));
  EXPECT_THAT(FloatProxy<double>::lowest().getAsFloat(),
              Eq(std::numeric_limits<double>::lowest()));
}

template <typename T>
struct StreamParseCase {
  StreamParseCase(const std::string& lit, bool succ, const std::string& suffix,
                  T value)
      : literal(lit),
        expect_success(succ),
        expected_suffix(suffix),
        expected_value(HexFloat<FloatProxy<T>>(value)) {}

  std::string literal;
  bool expect_success;
  std::string expected_suffix;
  HexFloat<FloatProxy<T>> expected_value;
};

template <typename T>
std::ostream& operator<<(std::ostream& os, const StreamParseCase<T>& fspc) {
  os << "StreamParseCase(" << fspc.literal
     << ", expect_success:" << int(fspc.expect_success) << ","
     << fspc.expected_suffix << "," << fspc.expected_value << ")";
  return os;
}

using Float32StreamParseTest = ::testing::TestWithParam<StreamParseCase<float>>;
using Float16StreamParseTest =
    ::testing::TestWithParam<StreamParseCase<Float16>>;
using FloatE4M3StreamParseTest =
    ::testing::TestWithParam<StreamParseCase<Float8_E4M3>>;
using FloatE5M2StreamParseTest =
    ::testing::TestWithParam<StreamParseCase<Float8_E5M2>>;

TEST_P(Float32StreamParseTest, Samples) {
  std::stringstream input(GetParam().literal);
  HexFloat<FloatProxy<float>> parsed_value(0.0f);
  // Hex floats must be read with the stream input operator.
  input >> parsed_value;
  if (GetParam().expect_success) {
    EXPECT_FALSE(input.fail());
    std::string suffix;
    input >> suffix;
    // EXPECT_EQ(suffix, GetParam().expected_suffix);
    EXPECT_EQ(parsed_value.value().getAsFloat(),
              GetParam().expected_value.value().getAsFloat());
  } else {
    EXPECT_TRUE(input.fail());
  }
}

// Returns a Float16 constructed from its sign bit, unbiased exponent, and
// mantissa.
Float16 makeF16(int sign_bit, int unbiased_exp, int mantissa) {
  EXPECT_LE(0, sign_bit);
  EXPECT_LE(sign_bit, 1);
  // Exponent is 5 bits, with bias of 15.
  EXPECT_LE(-15, unbiased_exp);  // -15 means zero or subnormal
  EXPECT_LE(unbiased_exp, 16);   // 16 means infinity or NaN
  EXPECT_LE(0, mantissa);
  EXPECT_LE(mantissa, 0x3ff);
  const unsigned biased_exp = 15 + unbiased_exp;
  const uint32_t as_bits = sign_bit << 15 | (biased_exp << 10) | mantissa;
  EXPECT_LE(as_bits, 0xffffu);
  return Float16(static_cast<uint16_t>(as_bits));
}

TEST_P(Float16StreamParseTest, Samples) {
  std::stringstream input(GetParam().literal);
  HexFloat<FloatProxy<Float16>> parsed_value(makeF16(0, 0, 0));
  // Hex floats must be read with the stream input operator.
  input >> parsed_value;
  if (GetParam().expect_success) {
    EXPECT_FALSE(input.fail());
    std::string suffix;
    input >> suffix;
    const auto got = parsed_value.value();
    const auto expected = GetParam().expected_value.value();
    EXPECT_EQ(got.data(), expected.data())
        << "got: " << got << " expected: " << expected;
  } else {
    EXPECT_TRUE(input.fail());
  }
}

INSTANTIATE_TEST_SUITE_P(
    HexFloat32FillSignificantDigits, Float32StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<float>>{
        {"0x123456p0", true, "", ldexpf(0x123456, 0)},
        // Patterns that fill all mantissa bits
        {"0x1.fffffep+23", true, "", ldexpf(0x1fffffe, -1)},
        {"0x1f.ffffep+19", true, "", ldexpf(0x1fffffe, -1)},
        {"0x1ff.fffep+15", true, "", ldexpf(0x1fffffe, -1)},
        {"0x1fff.ffep+11", true, "", ldexpf(0x1fffffe, -1)},
        {"0x1ffff.fep+7", true, "", ldexpf(0x1fffffe, -1)},
        {"0x1fffff.ep+3", true, "", ldexpf(0x1fffffe, -1)},
        {"0x1fffffe.p-1", true, "", ldexpf(0x1fffffe, -1)},
        {"0xffffff.p+0", true, "", ldexpf(0x1fffffe, -1)},
        {"0xffffff.p+0", true, "", ldexpf(0xffffff, 0)},
        // Now drop some bits in the middle
        {"0xa5a5a5.p+0", true, "", ldexpf(0xa5a5a5, 0)},
        {"0x5a5a5a.p+0", true, "", ldexpf(0x5a5a5a, 0)}}));

INSTANTIATE_TEST_SUITE_P(
    HexFloat32ExcessSignificantDigits, Float32StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<float>>{
        // Base cases
        {"0x1.fffffep0", true, "", ldexpf(0xffffff, -23)},
        {"0xa5a5a5p0", true, "", ldexpf(0xa5a5a5, 0)},
        {"0xa.5a5a5p+9", true, "", ldexpf(0xa5a5a5, -11)},
        {"0x5a5a5ap0", true, "", ldexpf(0x5a5a5a, 0)},
        {"0x5.a5a5ap+9", true, "", ldexpf(0x5a5a5a, -11)},
        // Truncate extra bits: zeroes
        {"0x1.fffffe0p0", true, "", ldexpf(0xffffff, -23)},
        {"0xa5a5a5000p0", true, "", ldexpf(0xa5a5a5, 12)},
        {"0xa.5a5a5000p+9", true, "", ldexpf(0xa5a5a5, -11)},
        {"0x5a5a5a000p0", true, "", ldexpf(0x5a5a5a, 12)},
        {"0x5.a5a5a000p+9", true, "", ldexpf(0x5a5a5a, -11)},
        // Truncate extra bits: ones
        {"0x1.ffffffp0",  // Extra bits in the last nibble
         true, "", ldexpf(0xffffff, -23)},
        {"0x1.fffffffp0", true, "", ldexpf(0xffffff, -23)},
        {"0xa5a5a5fffp0", true, "", ldexpf(0xa5a5a5, 12)},
        {"0xa.5a5a5fffp+9", true, "", ldexpf(0xa5a5a5, -11)},
        {"0x5a5a5afffp0",
         // The 5 nibble (0101), leads with 0, so the result can fit a leading
         // 1 bit , yielding 8 (1000).
         true, "", ldexpf(0x5a5a5a8, 8)},
        {"0x5.a5a5afffp+9", true, "", ldexpf(0x5a5a5a8, 8 - 32 + 9)}}));

INSTANTIATE_TEST_SUITE_P(
    HexFloat32ExponentMissingDigits, Float32StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<float>>{
        {"0x1.0p1", true, "", 2.0f},
        {"0x1.0p1a", true, "a", 2.0f},
        {"-0x1.0p1f", true, "f", -2.0f},
        {"0x1.0p", false, "", 0.0f},
        {"0x1.0pa", false, "", 0.0f},
        {"0x1.0p!", false, "", 0.0f},
        {"0x1.0p+", false, "", 0.0f},
        {"0x1.0p+a", false, "", 0.0f},
        {"0x1.0p+!", false, "", 0.0f},
        {"0x1.0p-", false, "", 0.0f},
        {"0x1.0p-a", false, "", 0.0f},
        {"0x1.0p-!", false, "", 0.0f},
        {"0x1.0p++", false, "", 0.0f},
        {"0x1.0p+-", false, "", 0.0f},
        {"0x1.0p-+", false, "", 0.0f},
        {"0x1.0p--", false, "", 0.0f}}));

INSTANTIATE_TEST_SUITE_P(
    HexFloat32ExponentTrailingSign, Float32StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<float>>{
        // Don't consume a sign after the binary exponent digits.
        {"0x1.0p1", true, "", 2.0f},
        {"0x1.0p1+", true, "+", 2.0f},
        {"0x1.0p1-", true, "-", 2.0f}}));

INSTANTIATE_TEST_SUITE_P(
    HexFloat32PositiveExponentOverflow, Float32StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<float>>{
        // Positive exponents
        {"0x1.0p1", true, "", 2.0f},       // fine, a normal number
        {"0x1.0p15", true, "", 32768.0f},  // fine, a normal number
        {"0x1.0p127", true, "", float(ldexp(1.0f, 127))},   // good large number
        {"0x0.8p128", true, "", float(ldexp(1.0f, 127))},   // good large number
        {"0x0.1p131", true, "", float(ldexp(1.0f, 127))},   // good large number
        {"0x0.01p135", true, "", float(ldexp(1.0f, 127))},  // good large number
        {"0x1.0p128", true, "", float(ldexp(1.0f, 128))},   // infinity
        {"0x1.0p4294967295", true, "", float(ldexp(1.0f, 128))},  // infinity
        {"0x1.0p5000000000", true, "", float(ldexp(1.0f, 128))},  // infinity
        {"0x0.0p5000000000", true, "", 0.0f},  // zero mantissa, zero result
    }));

INSTANTIATE_TEST_SUITE_P(
    HexFloat32NegativeExponentOverflow, Float32StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<float>>{
        // Positive results, digits before '.'
        {"0x1.0p-126", true, "",
         float(ldexp(1.0f, -126))},  // fine, a small normal number
        {"0x1.0p-127", true, "", float(ldexp(1.0f, -127))},  // denorm number
        {"0x1.0p-149", true, "",
         float(ldexp(1.0f, -149))},  // smallest positive denormal
        {"0x0.8p-148", true, "",
         float(ldexp(1.0f, -149))},  // smallest positive denormal
        {"0x0.1p-145", true, "",
         float(ldexp(1.0f, -149))},  // smallest positive denormal
        {"0x0.01p-141", true, "",
         float(ldexp(1.0f, -149))},  // smallest positive denormal

        // underflow rounds down to zero
        {"0x1.0p-150", true, "", 0.0f},
        {"0x1.0p-4294967296", true, "",
         0.0f},  // avoid exponent overflow in parser
        {"0x1.0p-5000000000", true, "",
         0.0f},  // avoid exponent overflow in parser
        {"0x0.0p-5000000000", true, "", 0.0f},  // zero mantissa, zero result
    }));

INSTANTIATE_TEST_SUITE_P(
    HexFloat16ExcessSignificantDigits, Float16StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<Float16>>{
        // Zero
        {"0x1.c00p0", true, "", makeF16(0, 0, 0x300)},
        {"0x0p0", true, "", makeF16(0, -15, 0x0)},
        {"0x000.0000p0", true, "", makeF16(0, -15, 0x0)},
        // All leading 1s
        {"0x1p0", true, "", makeF16(0, 0, 0x0)},
        {"0x1.8p0", true, "", makeF16(0, 0, 0x200)},
        {"0x1.cp0", true, "", makeF16(0, 0, 0x300)},
        {"0x1.ep0", true, "", makeF16(0, 0, 0x380)},
        {"0x1.fp0", true, "", makeF16(0, 0, 0x3c0)},
        {"0x1.f8p0", true, "", makeF16(0, 0, 0x3e0)},
        {"0x1.fcp0", true, "", makeF16(0, 0, 0x3f0)},
        {"0x1.fep0", true, "", makeF16(0, 0, 0x3f8)},
        {"0x1.ffp0", true, "", makeF16(0, 0, 0x3fc)},
        // Fill trailing zeros to all significant places
        // that might be used for significant digits.
        {"0x1.ff8p0", true, "", makeF16(0, 0, 0x3fe)},
        {"0x1.ffcp0", true, "", makeF16(0, 0, 0x3ff)},
        {"0x1.800p0", true, "", makeF16(0, 0, 0x200)},
        {"0x1.c00p0", true, "", makeF16(0, 0, 0x300)},
        {"0x1.e00p0", true, "", makeF16(0, 0, 0x380)},
        {"0x1.f00p0", true, "", makeF16(0, 0, 0x3c0)},
        {"0x1.f80p0", true, "", makeF16(0, 0, 0x3e0)},
        {"0x1.fc0p0", true, "", makeF16(0, 0, 0x3f0)},
        {"0x1.fe0p0", true, "", makeF16(0, 0, 0x3f8)},
        {"0x1.ff0p0", true, "", makeF16(0, 0, 0x3fc)},
        {"0x1.ff8p0", true, "", makeF16(0, 0, 0x3fe)},
        {"0x1.ffcp0", true, "", makeF16(0, 0, 0x3ff)},
        // Add several trailing zeros
        {"0x1.c00000p0", true, "", makeF16(0, 0, 0x300)},
        {"0x1.e00000p0", true, "", makeF16(0, 0, 0x380)},
        {"0x1.f00000p0", true, "", makeF16(0, 0, 0x3c0)},
        {"0x1.f80000p0", true, "", makeF16(0, 0, 0x3e0)},
        {"0x1.fc0000p0", true, "", makeF16(0, 0, 0x3f0)},
        {"0x1.fe0000p0", true, "", makeF16(0, 0, 0x3f8)},
        {"0x1.ff0000p0", true, "", makeF16(0, 0, 0x3fc)},
        {"0x1.ff8000p0", true, "", makeF16(0, 0, 0x3fe)},
        {"0x1.ffcp0000", true, "", makeF16(0, 0, 0x3ff)},
        // Samples that drop out bits in the middle.
        //   5 = 0101    4 = 0100
        //   a = 1010    8 = 1000
        {"0x1.5a4p0", true, "", makeF16(0, 0, 0x169)},
        {"0x1.a58p0", true, "", makeF16(0, 0, 0x296)},
        // Samples that drop out bits *and* truncate significant bits
        // that can't be represented.
        {"0x1.5a40000p0", true, "", makeF16(0, 0, 0x169)},
        {"0x1.5a7ffffp0", true, "", makeF16(0, 0, 0x169)},
        {"0x1.a580000p0", true, "", makeF16(0, 0, 0x296)},
        {"0x1.a5bffffp0", true, "", makeF16(0, 0, 0x296)},
        // Try some negations.
        {"-0x0p0", true, "", makeF16(1, -15, 0x0)},
        {"-0x000.0000p0", true, "", makeF16(1, -15, 0x0)},
        {"-0x1.5a40000p0", true, "", makeF16(1, 0, 0x169)},
        {"-0x1.5a7ffffp0", true, "", makeF16(1, 0, 0x169)},
        {"-0x1.a580000p0", true, "", makeF16(1, 0, 0x296)},
        {"-0x1.a5bffffp0", true, "", makeF16(1, 0, 0x296)}}));

INSTANTIATE_TEST_SUITE_P(
    HexFloat16IncreasingExponentsAndMantissa, Float16StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<Float16>>{
        // Zero
        {"0x0p0", true, "", makeF16(0, -15, 0x0)},
        {"0x0p5000000000000", true, "", makeF16(0, -15, 0x0)},
        {"-0x0p5000000000000", true, "", makeF16(1, -15, 0x0)},
        // Leading 1
        {"0x1p0", true, "", makeF16(0, 0, 0x0)},
        {"0x1p1", true, "", makeF16(0, 1, 0x0)},
        {"0x1p16", true, "", makeF16(0, 16, 0x0)},
        {"0x1p-1", true, "", makeF16(0, -1, 0x0)},
        {"0x1p-14", true, "", makeF16(0, -14, 0x0)},
        // Leading 2
        {"0x2p0", true, "", makeF16(0, 1, 0x0)},
        {"0x2p1", true, "", makeF16(0, 2, 0x0)},
        {"0x2p15", true, "", makeF16(0, 16, 0x0)},
        {"0x2p-1", true, "", makeF16(0, 0, 0x0)},
        {"0x2p-15", true, "", makeF16(0, -14, 0x0)},
        // Leading 8
        {"0x8p0", true, "", makeF16(0, 3, 0x0)},
        {"0x8p1", true, "", makeF16(0, 4, 0x0)},
        {"0x8p13", true, "", makeF16(0, 16, 0x0)},
        {"0x8p-3", true, "", makeF16(0, 0, 0x0)},
        {"0x8p-17", true, "", makeF16(0, -14, 0x0)},
        // Leading 10
        {"0x10.0p0", true, "", makeF16(0, 4, 0x0)},
        {"0x10.0p1", true, "", makeF16(0, 5, 0x0)},
        {"0x10.0p12", true, "", makeF16(0, 16, 0x0)},
        {"0x10.0p-5", true, "", makeF16(0, -1, 0x0)},
        {"0x10.0p-18", true, "", makeF16(0, -14, 0x0)},
        // Samples that drop out bits *and* truncate significant bits
        // that can't be represented.
        // Progressively increase the leading digit.
        {"0x1.5a40000p0", true, "", makeF16(0, 0, 0x169)},
        {"0x1.5a7ffffp0", true, "", makeF16(0, 0, 0x169)},
        {"0x2.5a40000p0", true, "", makeF16(0, 1, 0x0b4)},
        {"0x2.5a7ffffp0", true, "", makeF16(0, 1, 0x0b4)},
        {"0x4.5a40000p0", true, "", makeF16(0, 2, 0x05a)},
        {"0x4.5a7ffffp0", true, "", makeF16(0, 2, 0x05a)},
        {"0x8.5a40000p0", true, "", makeF16(0, 3, 0x02d)},
        {"0x8.5a7ffffp0", true, "", makeF16(0, 3, 0x02d)}}));

// Returns a E4M3 constructed from its sign bit, unbiased exponent, and
// mantissa.
Float8_E4M3 makeE4M3(int sign_bit, int unbiased_exp, int mantissa) {
  EXPECT_LE(0, sign_bit);
  EXPECT_LE(sign_bit, 1);
  // Exponent is 4 bits, with bias of 7.
  EXPECT_LE(-7, unbiased_exp);  // -7 means zero or subnormal
  EXPECT_LE(unbiased_exp, 8);
  EXPECT_LE(0, mantissa);
  EXPECT_LE(mantissa, 0x7);
  const unsigned biased_exp = 7 + unbiased_exp;
  const uint32_t as_bits = sign_bit << 7 | (biased_exp << 3) | mantissa;
  EXPECT_LE(as_bits, 0xffu);
  return Float8_E4M3(static_cast<uint8_t>(as_bits));
}

TEST_P(FloatE4M3StreamParseTest, Samples) {
  std::stringstream input(GetParam().literal);
  HexFloat<FloatProxy<Float8_E4M3>> parsed_value(makeE4M3(0, 0, 0));
  // Hex floats must be read with the stream input operator.
  input >> parsed_value;
  if (GetParam().expect_success) {
    EXPECT_FALSE(input.fail());
    std::string suffix;
    input >> suffix;
    const auto got = parsed_value.value();
    const auto expected = GetParam().expected_value.value();
    EXPECT_EQ(got.data(), expected.data())
        << "got: " << got << " expected: " << expected;
  } else {
    EXPECT_TRUE(input.fail());
  }
}

INSTANTIATE_TEST_SUITE_P(
    HexFloatE4M3IncreasingExponentsAndMantissa, FloatE4M3StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<Float8_E4M3>>{
        // Zero
        {"0x0p0", true, "", makeE4M3(0, -7, 0x0)},
        {"0x0p5000000000000", true, "", makeE4M3(0, -7, 0x0)},
        {"-0x0p5000000000000", true, "", makeE4M3(1, -7, 0x0)},
        // Leading 1
        {"0x1p0", true, "", makeE4M3(0, 0, 0x0)},
        {"0x1p1", true, "", makeE4M3(0, 1, 0x0)},
        {"0x1p8", true, "", makeE4M3(0, 8, 0x0)},
        {"0x1p-1", true, "", makeE4M3(0, -1, 0x0)},
        {"0x1p-6", true, "", makeE4M3(0, -6, 0x0)},
        // Leading 2
        {"0x2p0", true, "", makeE4M3(0, 1, 0x0)},
        {"0x2p1", true, "", makeE4M3(0, 2, 0x0)},
        {"0x2p7", true, "", makeE4M3(0, 8, 0x0)},
        {"0x2p-1", true, "", makeE4M3(0, 0, 0x0)},
        {"0x2p-7", true, "", makeE4M3(0, -6, 0x0)},
        // Leading 8
        {"0x8p0", true, "", makeE4M3(0, 3, 0x0)},
        {"0x8p1", true, "", makeE4M3(0, 4, 0x0)},
        {"0x8p5", true, "", makeE4M3(0, 8, 0x0)},
        {"0x8p-3", true, "", makeE4M3(0, 0, 0x0)},
        {"0x8p-9", true, "", makeE4M3(0, -6, 0x0)},
        // Leading 10
        {"0x10.0p0", true, "", makeE4M3(0, 4, 0x0)},
        {"0x10.0p1", true, "", makeE4M3(0, 5, 0x0)},
        {"0x10.0p4", true, "", makeE4M3(0, 8, 0x0)},
        {"0x10.0p-5", true, "", makeE4M3(0, -1, 0x0)},
        {"0x10.0p-10", true, "", makeE4M3(0, -6, 0x0)},
        // Samples that drop out bits *and* truncate significant bits
        // that can't be represented.
        // Progressively increase the leading digit.
        {"0x1.5a40000p0", true, "", makeE4M3(0, 0, 0x2)},
        {"0x1.5a7ffffp0", true, "", makeE4M3(0, 0, 0x2)},
        {"0x2.5a40000p0", true, "", makeE4M3(0, 1, 0x1)},
        {"0x2.5a7ffffp0", true, "", makeE4M3(0, 1, 0x1)},
        {"0x4.5a40000p0", true, "", makeE4M3(0, 2, 0x0)},
        {"0x4.5a7ffffp0", true, "", makeE4M3(0, 2, 0x0)},
        {"0x8.5a40000p0", true, "", makeE4M3(0, 3, 0x0)},
        {"0x8.5a7ffffp0", true, "", makeE4M3(0, 3, 0x0)}}));

// Returns a E5M2 constructed from its sign bit, unbiased exponent, and
// mantissa.
Float8_E5M2 makeE5M2(int sign_bit, int unbiased_exp, int mantissa) {
  EXPECT_LE(0, sign_bit);
  EXPECT_LE(sign_bit, 1);
  // Exponent is 5 bits, with bias of 15.
  EXPECT_LE(-15, unbiased_exp);  // -15 means zero or subnormal
  EXPECT_LE(unbiased_exp, 16);
  EXPECT_LE(0, mantissa);
  EXPECT_LE(mantissa, 0x3);
  const unsigned biased_exp = 15 + unbiased_exp;
  const uint32_t as_bits = sign_bit << 7 | (biased_exp << 2) | mantissa;
  EXPECT_LE(as_bits, 0xffu);
  return Float8_E5M2(static_cast<uint8_t>(as_bits));
}

TEST_P(FloatE5M2StreamParseTest, Samples) {
  std::stringstream input(GetParam().literal);
  HexFloat<FloatProxy<Float8_E5M2>> parsed_value(makeE5M2(0, 0, 0));
  // Hex floats must be read with the stream input operator.
  input >> parsed_value;
  if (GetParam().expect_success) {
    EXPECT_FALSE(input.fail());
    std::string suffix;
    input >> suffix;
    const auto got = parsed_value.value();
    const auto expected = GetParam().expected_value.value();
    EXPECT_EQ(got.data(), expected.data())
        << "got: " << got << " expected: " << expected;
  } else {
    EXPECT_TRUE(input.fail());
  }
}

INSTANTIATE_TEST_SUITE_P(
    HexFloatE5M2IncreasingExponentsAndMantissa, FloatE5M2StreamParseTest,
    ::testing::ValuesIn(std::vector<StreamParseCase<Float8_E5M2>>{
        // Zero
        {"0x0p0", true, "", makeE5M2(0, -15, 0x0)},
        {"0x0p5000000000000", true, "", makeE5M2(0, -15, 0x0)},
        {"-0x0p5000000000000", true, "", makeE5M2(1, -15, 0x0)},
        // Leading 1
        {"0x1p0", true, "", makeE5M2(0, 0, 0x0)},
        {"0x1p1", true, "", makeE5M2(0, 1, 0x0)},
        {"0x1p16", true, "", makeE5M2(0, 16, 0x0)},
        {"0x1p-1", true, "", makeE5M2(0, -1, 0x0)},
        {"0x1p-14", true, "", makeE5M2(0, -14, 0x0)},
        // Leading 2
        {"0x2p0", true, "", makeE5M2(0, 1, 0x0)},
        {"0x2p1", true, "", makeE5M2(0, 2, 0x0)},
        {"0x2p15", true, "", makeE5M2(0, 16, 0x0)},
        {"0x2p-1", true, "", makeE5M2(0, 0, 0x0)},
        {"0x2p-15", true, "", makeE5M2(0, -14, 0x0)},
        // Leading 8
        {"0x8p0", true, "", makeE5M2(0, 3, 0x0)},
        {"0x8p1", true, "", makeE5M2(0, 4, 0x0)},
        {"0x8p13", true, "", makeE5M2(0, 16, 0x0)},
        {"0x8p-3", true, "", makeE5M2(0, 0, 0x0)},
        {"0x8p-17", true, "", makeE5M2(0, -14, 0x0)},
        // Leading 10
        {"0x10.0p0", true, "", makeE5M2(0, 4, 0x0)},
        {"0x10.0p1", true, "", makeE5M2(0, 5, 0x0)},
        {"0x10.0p12", true, "", makeE5M2(0, 16, 0x0)},
        {"0x10.0p-5", true, "", makeE5M2(0, -1, 0x0)},
        {"0x10.0p-18", true, "", makeE5M2(0, -14, 0x0)},
        // Samples that drop out bits *and* truncate significant bits
        // that can't be represented.
        // Progressively increase the leading digit.
        {"0x1.aa40000p0", true, "", makeE5M2(0, 0, 0x2)},
        {"0x1.aa7ffffp0", true, "", makeE5M2(0, 0, 0x2)},
        {"0x2.aa40000p0", true, "", makeE5M2(0, 1, 0x1)},
        {"0x2.aa7ffffp0", true, "", makeE5M2(0, 1, 0x1)},
        {"0x4.aa40000p0", true, "", makeE5M2(0, 2, 0x0)},
        {"0x4.aa7ffffp0", true, "", makeE5M2(0, 2, 0x0)},
        {"0x8.aa40000p0", true, "", makeE5M2(0, 3, 0x0)},
        {"0x8.aa7ffffp0", true, "", makeE5M2(0, 3, 0x0)}}));

}  // namespace
}  // namespace utils
}  // namespace spvtools
