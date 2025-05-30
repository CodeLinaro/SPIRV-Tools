// Copyright (c) 2016 Google Inc.
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

#include "source/opt/fold.h"

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "effcee/effcee.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "source/opt/build_module.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "spirv-tools/libspirv.hpp"

namespace spvtools {
namespace opt {
namespace {

using ::testing::Contains;

std::string Disassemble(const std::string& original, IRContext* context,
                        uint32_t disassemble_options = 0) {
  std::vector<uint32_t> optimized_bin;
  context->module()->ToBinary(&optimized_bin, true);
  spv_target_env target_env = SPV_ENV_UNIVERSAL_1_2;
  SpirvTools tools(target_env);
  std::string optimized_asm;
  EXPECT_TRUE(
      tools.Disassemble(optimized_bin, &optimized_asm, disassemble_options))
      << "Disassembling failed for shader:\n"
      << original << std::endl;
  return optimized_asm;
}

void Match(const std::string& original, IRContext* context,
           uint32_t disassemble_options = 0) {
  std::string disassembly = Disassemble(original, context, disassemble_options);
  auto match_result = effcee::Match(disassembly, original);
  EXPECT_EQ(effcee::Result::Status::Ok, match_result.status())
      << match_result.message() << "\nChecking result:\n"
      << disassembly;
}

template <class ResultType>
struct InstructionFoldingCase {
  InstructionFoldingCase(const std::string& tb, uint32_t id, ResultType result)
      : test_body(tb), id_to_fold(id), expected_result(result) {}

  std::string test_body;
  uint32_t id_to_fold;
  ResultType expected_result;
};

std::tuple<std::unique_ptr<IRContext>, Instruction*> GetInstructionToFold(
    const std::string test_body, const uint32_t id_to_fold,
    spv_target_env spv_env) {
  // Build module.
  std::unique_ptr<IRContext> context =
      BuildModule(spv_env, nullptr, test_body,
                  SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS);
  EXPECT_NE(nullptr, context);
  if (context == nullptr) {
    return {nullptr, nullptr};
  }

  // Fold the instruction to test.
  if (id_to_fold != 0) {
    analysis::DefUseManager* def_use_mgr = context->get_def_use_mgr();
    Instruction* inst = def_use_mgr->GetDef(id_to_fold);
    return {std::move(context), inst};
  }

  // If there is not ID, we get the instruction just before a terminator
  // instruction. That could be a return or abort. This is used for cases where
  // the instruction we want to fold does not have a result id.
  Function* func = &*context->module()->begin();
  for (auto& bb : *func) {
    Instruction* terminator = bb.terminator();
    if (terminator->IsReturnOrAbort()) {
      return {std::move(context), terminator->PreviousNode()};
    }
  }
  return {nullptr, nullptr};
}

std::tuple<std::unique_ptr<IRContext>, Instruction*> FoldInstruction(
    const std::string test_body, const uint32_t id_to_fold,
    spv_target_env spv_env) {
  // Build module.
  std::unique_ptr<IRContext> context;
  Instruction* inst = nullptr;
  std::tie(context, inst) =
      GetInstructionToFold(test_body, id_to_fold, spv_env);

  if (context == nullptr) {
    return {nullptr, nullptr};
  }

  std::unique_ptr<Instruction> original_inst(inst->Clone(context.get()));
  bool succeeded = context->get_instruction_folder().FoldInstruction(inst);
  EXPECT_EQ(inst->result_id(), original_inst->result_id());
  EXPECT_EQ(inst->type_id(), original_inst->type_id());

  if (!succeeded && inst != nullptr) {
    EXPECT_EQ(inst->NumInOperands(), original_inst->NumInOperands());
    for (uint32_t i = 0; i < inst->NumInOperands(); ++i) {
      EXPECT_EQ(inst->GetOperand(i), original_inst->GetOperand(i));
    }
  }

  return {std::move(context), succeeded ? inst : nullptr};
}

template <class ElementType, class Function>
void CheckForExpectedScalarConstant(Instruction* inst,
                                    ElementType expected_result,
                                    Function GetValue) {
  ASSERT_TRUE(inst);

  IRContext* context = inst->context();
  analysis::DefUseManager* def_use_mgr = context->get_def_use_mgr();
  while (inst->opcode() == spv::Op::OpCopyObject) {
    inst = def_use_mgr->GetDef(inst->GetSingleWordInOperand(0));
  }

  // Make sure we have a constant.
  analysis::ConstantManager* const_mrg = context->get_constant_mgr();
  const analysis::Constant* constant = const_mrg->GetConstantFromInst(inst);
  ASSERT_TRUE(constant);

  // Make sure the constant is a scalar.
  const analysis::ScalarConstant* result = constant->AsScalarConstant();
  ASSERT_TRUE(result);

  // Check if the result matches the expected value.
  // If ExpectedType is not a float type, it should cast the value to a double
  // and never get a nan.
  if (!std::isnan(static_cast<double>(expected_result))) {
    EXPECT_EQ(expected_result, GetValue(result));
  } else {
    EXPECT_TRUE(std::isnan(static_cast<double>(GetValue(result))));
  }
}

template <class ElementType, class Function>
void CheckForExpectedVectorConstant(Instruction* inst,
                                    std::vector<ElementType> expected_result,
                                    Function GetValue) {
  ASSERT_TRUE(inst);

  IRContext* context = inst->context();
  EXPECT_EQ(inst->opcode(), spv::Op::OpCopyObject);
  analysis::DefUseManager* def_use_mgr = context->get_def_use_mgr();
  inst = def_use_mgr->GetDef(inst->GetSingleWordInOperand(0));
  std::vector<spv::Op> opcodes = {spv::Op::OpConstantComposite};
  EXPECT_THAT(opcodes, Contains(inst->opcode()));
  analysis::ConstantManager* const_mrg = context->get_constant_mgr();
  const analysis::Constant* result = const_mrg->GetConstantFromInst(inst);
  EXPECT_NE(result, nullptr);
  if (result != nullptr) {
    const std::vector<const analysis::Constant*>& componenets =
        result->AsVectorConstant()->GetComponents();
    EXPECT_EQ(componenets.size(), expected_result.size());
    for (size_t i = 0; i < componenets.size(); i++) {
      EXPECT_EQ(expected_result[i], GetValue(componenets[i]));
    }
  }
}

using IntegerInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<uint32_t>>;

TEST_P(IntegerInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);
  CheckForExpectedScalarConstant(
      inst, tc.expected_result, [](const analysis::Constant* c) {
        return c->AsScalarConstant()->GetU32BitValue();
      });
}

// Returns a common SPIR-V header for all of the test that follow.
#define INT_0_ID 100
#define TRUE_ID 101
#define VEC2_0_ID 102
#define INT_7_ID 103
#define FLOAT_0_ID 104
#define DOUBLE_0_ID 105
#define VEC4_0_ID 106
#define DVEC4_0_ID 106
#define HALF_0_ID 108
#define UINT_0_ID 109
#define INT_NULL_ID 110
#define UINT_NULL_ID 111
const std::string& Header() {
  static const std::string header = R"(OpCapability Shader
OpCapability Float16
OpCapability Float64
OpCapability Int8
OpCapability Int16
OpCapability Int64
OpCapability CooperativeMatrixKHR
OpExtension "SPV_KHR_cooperative_matrix"
%1 = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main"
OpExecutionMode %main OriginUpperLeft
OpSource GLSL 140
OpName %main "main"
%void = OpTypeVoid
%void_func = OpTypeFunction %void
%bool = OpTypeBool
%float = OpTypeFloat 32
%double = OpTypeFloat 64
%half = OpTypeFloat 16
%101 = OpConstantTrue %bool ; Need a def with an numerical id to define id maps.
%true = OpConstantTrue %bool
%false = OpConstantFalse %bool
%bool_null = OpConstantNull %bool
%short = OpTypeInt 16 1
%ushort = OpTypeInt 16 0
%byte = OpTypeInt 8 1
%ubyte = OpTypeInt 8 0
%int = OpTypeInt 32 1
%long = OpTypeInt 64 1
%uint = OpTypeInt 32 0
%ulong = OpTypeInt 64 0
%v2int = OpTypeVector %int 2
%v4int = OpTypeVector %int 4
%v2short = OpTypeVector %short 2
%v2long = OpTypeVector %long 2
%v4long = OpTypeVector %long 4
%v4float = OpTypeVector %float 4
%v4double = OpTypeVector %double 4
%v2uint = OpTypeVector %uint 2
%v2ulong = OpTypeVector %ulong 2
%v2float = OpTypeVector %float 2
%v2double = OpTypeVector %double 2
%v2half = OpTypeVector %half 2
%v2bool = OpTypeVector %bool 2
%m2x2int = OpTypeMatrix %v2int 2
%mat4v2float = OpTypeMatrix %v2float 4
%mat2v4float = OpTypeMatrix %v4float 2
%mat4v4float = OpTypeMatrix %v4float 4
%mat4v4double = OpTypeMatrix %v4double 4
%struct_v2int_int_int = OpTypeStruct %v2int %int %int
%_ptr_int = OpTypePointer Function %int
%_ptr_uint = OpTypePointer Function %uint
%_ptr_bool = OpTypePointer Function %bool
%_ptr_float = OpTypePointer Function %float
%_ptr_double = OpTypePointer Function %double
%_ptr_half = OpTypePointer Function %half
%_ptr_long = OpTypePointer Function %long
%_ptr_ulong = OpTypePointer Function %ulong
%_ptr_v2int = OpTypePointer Function %v2int
%_ptr_v4int = OpTypePointer Function %v4int
%_ptr_v4float = OpTypePointer Function %v4float
%_ptr_v4double = OpTypePointer Function %v4double
%_ptr_struct_v2int_int_int = OpTypePointer Function %struct_v2int_int_int
%_ptr_v2float = OpTypePointer Function %v2float
%_ptr_v2double = OpTypePointer Function %v2double
%int_2 = OpConstant %int 2
%int_arr_2 = OpTypeArray %int %int_2
%short_0 = OpConstant %short 0
%short_2 = OpConstant %short 2
%short_3 = OpConstant %short 3
%short_n5 = OpConstant %short -5
%ubyte_1 = OpConstant %ubyte 1
%byte_n1 = OpConstant %byte -1
%100 = OpConstant %int 0 ; Need a def with an numerical id to define id maps.
%110 = OpConstantNull %int ; Need a def with an numerical id to define id maps.
%103 = OpConstant %int 7 ; Need a def with an numerical id to define id maps.
%int_0 = OpConstant %int 0
%int_1 = OpConstant %int 1
%int_3 = OpConstant %int 3
%int_4 = OpConstant %int 4
%int_10 = OpConstant %int 10
%int_1073741824 = OpConstant %int 1073741824
%int_n1 = OpConstant %int -1
%int_n24 = OpConstant %int -24
%int_n858993459 = OpConstant %int -858993459
%int_min = OpConstant %int -2147483648
%int_max = OpConstant %int 2147483647
%long_0 = OpConstant %long 0
%long_1 = OpConstant %long 1
%long_2 = OpConstant %long 2
%long_3 = OpConstant %long 3
%long_n3 = OpConstant %long -3
%long_7 = OpConstant %long 7
%long_n7 = OpConstant %long -7
%long_10 = OpConstant %long 10
%long_32768 = OpConstant %long 32768
%long_n57344 = OpConstant %long -57344
%long_n4611686018427387904 = OpConstant %long -4611686018427387904
%long_4611686018427387904 = OpConstant %long 4611686018427387904
%long_n1 = OpConstant %long -1
%long_n3689348814741910323 = OpConstant %long -3689348814741910323
%long_min = OpConstant %long -9223372036854775808
%long_max = OpConstant %long 9223372036854775807
%ulong_7 = OpConstant %ulong 7
%ulong_4611686018427387904 = OpConstant %ulong 4611686018427387904
%109 = OpConstant %uint 0 ; Need a def with an numerical id to define id maps.
%111 = OpConstantNull %uint ; Need a def with an numerical id to define id maps.
%uint_0 = OpConstant %uint 0
%uint_1 = OpConstant %uint 1
%uint_2 = OpConstant %uint 2
%uint_3 = OpConstant %uint 3
%uint_4 = OpConstant %uint 4
%uint_32 = OpConstant %uint 32
%uint_42 = OpConstant %uint 42
%uint_2147483649 = OpConstant %uint 2147483649
%uint_max = OpConstant %uint 4294967295
%ulong_0 = OpConstant %ulong 0
%ulong_1 = OpConstant %ulong 1
%ulong_2 = OpConstant %ulong 2
%ulong_9223372036854775809 = OpConstant %ulong 9223372036854775809
%ulong_max = OpConstant %ulong 18446744073709551615
%v2int_undef = OpUndef %v2int
%v2int_0_0 = OpConstantComposite %v2int %int_0 %int_0
%v2int_1_0 = OpConstantComposite %v2int %int_1 %int_0
%v2int_2_2 = OpConstantComposite %v2int %int_2 %int_2
%v2int_2_3 = OpConstantComposite %v2int %int_2 %int_3
%v2int_3_2 = OpConstantComposite %v2int %int_3 %int_2
%v2int_n1_n24 = OpConstantComposite %v2int %int_n1 %int_n24
%v2int_4_4 = OpConstantComposite %v2int %int_4 %int_4
%v2int_min_max = OpConstantComposite %v2int %int_min %int_max
%v2short_2_n5 = OpConstantComposite %v2short %short_2 %short_n5
%v2long_2_2 = OpConstantComposite %v2long %long_2 %long_2
%v2long_2_3 = OpConstantComposite %v2long %long_2 %long_3
%v2bool_null = OpConstantNull %v2bool
%v2bool_true_false = OpConstantComposite %v2bool %true %false
%v2bool_false_true = OpConstantComposite %v2bool %false %true
%struct_v2int_int_int_null = OpConstantNull %struct_v2int_int_int
%v2int_null = OpConstantNull %v2int
%102 = OpConstantComposite %v2int %103 %103
%v4int_undef = OpUndef %v4int
%v4int_0_0_0_0 = OpConstantComposite %v4int %int_0 %int_0 %int_0 %int_0
%m2x2int_undef = OpUndef %m2x2int
%struct_undef_0_0 = OpConstantComposite %struct_v2int_int_int %v2int_undef %int_0 %int_0
%float_n1 = OpConstant %float -1
%104 = OpConstant %float 0 ; Need a def with an numerical id to define id maps.
%float_null = OpConstantNull %float
%float_0 = OpConstant %float 0
%float_n0 = OpConstant %float -0.0
%float_1 = OpConstant %float 1
%float_2 = OpConstant %float 2
%float_3 = OpConstant %float 3
%float_4 = OpConstant %float 4
%float_2049 = OpConstant %float 2049
%float_n2049 = OpConstant %float -2049
%float_0p5 = OpConstant %float 0.5
%float_0p2 = OpConstant %float 0.2
%float_pi = OpConstant %float 1.5555
%float_1e16 = OpConstant %float 1e16
%float_n1e16 = OpConstant %float -1e16
%float_1en16 = OpConstant %float 1e-16
%float_n1en16 = OpConstant %float -1e-16
%v2float_0_0 = OpConstantComposite %v2float %float_0 %float_0
%v2float_2_2 = OpConstantComposite %v2float %float_2 %float_2
%v2float_2_3 = OpConstantComposite %v2float %float_2 %float_3
%v2float_3_2 = OpConstantComposite %v2float %float_3 %float_2
%v2float_4_4 = OpConstantComposite %v2float %float_4 %float_4
%v2float_2_0p5 = OpConstantComposite %v2float %float_2 %float_0p5
%v2float_0p2_0p5 = OpConstantComposite %v2float %float_0p2 %float_0p5
%v2float_null = OpConstantNull %v2float
%double_n1 = OpConstant %double -1
%105 = OpConstant %double 0 ; Need a def with an numerical id to define id maps.
%double_null = OpConstantNull %double
%double_0 = OpConstant %double 0
%double_n0 = OpConstant %double -0.0
%double_1 = OpConstant %double 1
%double_2 = OpConstant %double 2
%double_3 = OpConstant %double 3
%double_4 = OpConstant %double 4
%double_5 = OpConstant %double 5
%double_0p5 = OpConstant %double 0.5
%double_0p2 = OpConstant %double 0.2
%v2double_0_0 = OpConstantComposite %v2double %double_0 %double_0
%v2double_2_2 = OpConstantComposite %v2double %double_2 %double_2
%v2double_2_3 = OpConstantComposite %v2double %double_2 %double_3
%v2double_3_2 = OpConstantComposite %v2double %double_3 %double_2
%v2double_4_4 = OpConstantComposite %v2double %double_4 %double_4
%v2double_2_0p5 = OpConstantComposite %v2double %double_2 %double_0p5
%v2double_null = OpConstantNull %v2double
%108 = OpConstant %half 0
%half_1 = OpConstant %half 1
%half_2 = OpConstant %half 2
%half_0_1 = OpConstantComposite %v2half %108 %half_1
%106 = OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_0
%v4float_0_0_0_0 = OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_0
%v4float_0_0_0_1 = OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_1
%v4float_0_1_0_0 = OpConstantComposite %v4float %float_0 %float_1 %float_null %float_0
%v4float_1_1_1_1 = OpConstantComposite %v4float %float_1 %float_1 %float_1 %float_1
%v4float_1_2_3_4 = OpConstantComposite %v4float %float_1 %float_2 %float_3 %float_4
%v4float_null = OpConstantNull %v4float
%mat2v4float_null = OpConstantNull %mat2v4float
%mat4v4float_null = OpConstantNull %mat4v4float
%mat4v4float_1_2_3_4 = OpConstantComposite %mat4v4float %v4float_1_2_3_4 %v4float_1_2_3_4 %v4float_1_2_3_4 %v4float_1_2_3_4
%mat4v4float_1_2_3_4_null = OpConstantComposite %mat4v4float %v4float_1_2_3_4 %v4float_null %v4float_1_2_3_4 %v4float_null
%107 = OpConstantComposite %v4double %double_0 %double_0 %double_0 %double_0
%v4double_0_0_0_0 = OpConstantComposite %v4double %double_0 %double_0 %double_0 %double_0
%v4double_0_0_0_1 = OpConstantComposite %v4double %double_0 %double_0 %double_0 %double_1
%v4double_0_1_0_0 = OpConstantComposite %v4double %double_0 %double_1 %double_null %double_0
%v4double_1_1_1_1 = OpConstantComposite %v4double %double_1 %double_1 %double_1 %double_1
%v4double_1_2_3_4 = OpConstantComposite %v4double %double_1 %double_2 %double_3 %double_4
%v4double_1_1_1_0p5 = OpConstantComposite %v4double %double_1 %double_1 %double_1 %double_0p5
%v4double_null = OpConstantNull %v4double
%mat4v4double_null = OpConstantNull %mat4v4double
%mat4v4double_1_2_3_4 = OpConstantComposite %mat4v4double %v4double_1_2_3_4 %v4double_1_2_3_4 %v4double_1_2_3_4 %v4double_1_2_3_4
%mat4v4double_1_2_3_4_null = OpConstantComposite %mat4v4double %v4double_1_2_3_4 %v4double_null %v4double_1_2_3_4 %v4double_null
%v4float_n1_2_1_3 = OpConstantComposite %v4float %float_n1 %float_2 %float_1 %float_3
%uint_0x3f800000 = OpConstant %uint 0x3f800000
%uint_0xbf800000 = OpConstant %uint 0xbf800000
%v2uint_0x3f800000_0xbf800000 = OpConstantComposite %v2uint %uint_0x3f800000 %uint_0xbf800000
%long_0xbf8000003f800000 = OpConstant %long 0xbf8000003f800000
%int_0x3FF00000 = OpConstant %int 0x3FF00000
%int_0x00000000 = OpConstant %int 0x00000000
%int_0xC05FD666 = OpConstant %int 0xC05FD666
%int_0x66666666 = OpConstant %int 0x66666666
%v4int_0x3FF00000_0x00000000_0xC05FD666_0x66666666 = OpConstantComposite %v4int %int_0x00000000 %int_0x3FF00000 %int_0x66666666 %int_0xC05FD666
%ushort_0x4400 = OpConstant %ushort 0x4400
%short_0x4400 = OpConstant %short 0x4400
%ushort_0xBC00 = OpConstant %ushort 0xBC00
%short_0xBC00 = OpConstant %short 0xBC00
%int_arr_2_undef = OpUndef %int_arr_2
%int_coop_matrix = OpTypeCooperativeMatrixKHR %int %uint_3 %uint_3 %uint_32 %uint_0
%undef_int_coop_matrix = OpUndef %int_coop_matrix
%uint_coop_matrix = OpTypeCooperativeMatrixKHR %uint %uint_3 %uint_3 %uint_32 %uint_0
%undef_uint_coop_matrix = OpUndef %uint_coop_matrix
%float_coop_matrix = OpTypeCooperativeMatrixKHR %float %uint_3 %uint_3 %uint_32 %uint_0
%undef_float_coop_matrix = OpUndef %float_coop_matrix
)";

  return header;
}

// Returns the header with definitions of float NaN and double NaN. Since FC
// "; CHECK: [[double_n0:%\\w+]] = OpConstant [[double]] -0\n" finds
// %double_nan = OpConstant %double -0x1.8p+1024 instead of
// %double_n0 = OpConstant %double -0,
// we separates those definitions from Header().
const std::string& HeaderWithNaN() {
  static const std::string headerWithNaN =
      Header() +
      R"(%float_nan = OpConstant %float -0x1.8p+128
%double_nan = OpConstant %double -0x1.8p+1024
)";

  return headerWithNaN;
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(TestCase, IntegerInstructionFoldingTest,
                        ::testing::Values(
  // Test case 0: fold 0*n
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
    "%main_lab = OpLabel\n" +
           "%n = OpVariable %_ptr_int Function\n" +
        "%load = OpLoad %int %n\n" +
           "%2 = OpIMul %int %int_0 %load\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
    2, 0),
  // Test case 1: fold n*0
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_int Function\n" +
        "%load = OpLoad %int %n\n" +
        "%2 = OpIMul %int %load %int_0\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 2: fold 0/n (signed)
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_int Function\n" +
        "%load = OpLoad %int %n\n" +
        "%2 = OpSDiv %int %int_0 %load\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
        2, 0),
  // Test case 3: fold n/0 (signed)
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_int Function\n" +
        "%load = OpLoad %int %n\n" +
        "%2 = OpSDiv %int %load %int_0\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 4: fold 0/n (unsigned)
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_uint Function\n" +
        "%load = OpLoad %uint %n\n" +
        "%2 = OpUDiv %uint %uint_0 %load\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 5: fold n/0 (unsigned)
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_int Function\n" +
        "%load = OpLoad %int %n\n" +
        "%2 = OpSDiv %int %load %int_0\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 6: fold 0 remainder n
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_int Function\n" +
        "%load = OpLoad %int %n\n" +
        "%2 = OpSRem %int %int_0 %load\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 7: fold n remainder 0
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_int Function\n" +
        "%load = OpLoad %int %n\n" +
        "%2 = OpSRem %int %load %int_0\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 8: fold 0%n (signed)
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_int Function\n" +
        "%load = OpLoad %int %n\n" +
        "%2 = OpSMod %int %int_0 %load\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 9: fold n%0 (signed)
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_int Function\n" +
        "%load = OpLoad %int %n\n" +
        "%2 = OpSMod %int %load %int_0\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 10: fold 0%n (unsigned)
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_uint Function\n" +
        "%load = OpLoad %uint %n\n" +
        "%2 = OpUMod %uint %uint_0 %load\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 11: fold n%0 (unsigned)
  InstructionFoldingCase<uint32_t>(
    Header() + "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_uint Function\n" +
        "%load = OpLoad %uint %n\n" +
        "%2 = OpUMod %uint %load %uint_0\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, 0),
  // Test case 12: fold n << 32
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpShiftLeftLogical %uint %load %uint_32\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 13: fold n >> 32
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpShiftRightLogical %uint %load %uint_32\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 14: fold n | 0xFFFFFFFF
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
  "%main_lab = OpLabel\n" +
  "%n = OpVariable %_ptr_uint Function\n" +
  "%load = OpLoad %uint %n\n" +
  "%2 = OpBitwiseOr %uint %load %uint_max\n" +
  "OpReturn\n" +
  "OpFunctionEnd",
  2, 0xFFFFFFFF),
  // Test case 15: fold 0xFFFFFFFF | n
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpBitwiseOr %uint %uint_max %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0xFFFFFFFF),
  // Test case 16: fold n & 0
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpBitwiseAnd %uint %load %uint_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 17: fold 1/0 (signed)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSDiv %int %int_1 %int_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 18: fold 1/0 (unsigned)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpUDiv %uint %uint_1 %uint_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 19: fold OpSRem 1 0 (signed)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSRem %int %int_1 %int_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 20: fold 1%0 (signed)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSMod %int %int_1 %int_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 21: fold 1%0 (unsigned)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpUMod %uint %uint_1 %uint_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 22: fold unsigned n >> 42 (undefined, so set to zero).
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpShiftRightLogical %uint %load %uint_42\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 23: fold signed n >> 42 (undefined, so set to zero).
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpShiftRightLogical %int %load %uint_42\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 24: fold n << 42 (undefined, so set to zero).
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpShiftLeftLogical %int %load %uint_42\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 25: fold -24 >> 32 (defined as -1)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpShiftRightArithmetic %int %int_n24 %uint_32\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, -1),
  // Test case 26: fold 2 >> 32 (signed)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpShiftRightArithmetic %int %int_2 %uint_32\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 27: fold 2 >> 32 (unsigned)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpShiftRightLogical %int %int_2 %uint_32\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 28: fold 2 << 32
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpShiftLeftLogical %int %int_2 %uint_32\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
  // Test case 29: fold -INT_MIN
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSNegate %int %int_min\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, std::numeric_limits<int32_t>::min()),
  // Test case 30: fold UMin 3 4
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %uint %1 UMin %uint_3 %uint_4\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 3),
  // Test case 31: fold UMin 4 2
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %uint %1 UMin %uint_4 %uint_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 2),
  // Test case 32: fold SMin 3 4
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %int %1 UMin %int_3 %int_4\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 3),
  // Test case 33: fold SMin 4 2
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %int %1 SMin %int_4 %int_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 2),
  // Test case 34: fold UMax 3 4
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %uint %1 UMax %uint_3 %uint_4\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 4),
  // Test case 35: fold UMax 3 2
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %uint %1 UMax %uint_3 %uint_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 3),
  // Test case 36: fold SMax 3 4
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %int %1 UMax %int_3 %int_4\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 4),
  // Test case 37: fold SMax 3 2
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %int %1 SMax %int_3 %int_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 3),
  // Test case 38: fold UClamp 2 3 4
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %uint %1 UClamp %uint_2 %uint_3 %uint_4\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 3),
  // Test case 39: fold UClamp 2 0 4
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %uint %1 UClamp %uint_2 %uint_0 %uint_4\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 2),
  // Test case 40: fold UClamp 2 0 1
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %uint %1 UClamp %uint_2 %uint_0 %uint_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 1),
  // Test case 41: fold SClamp 2 3 4
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %int %1 SClamp %int_2 %int_3 %int_4\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 3),
  // Test case 42: fold SClamp 2 0 4
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %int %1 SClamp %int_2 %int_0 %int_4\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 2),
  // Test case 43: fold SClamp 2 0 1
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpExtInst %int %1 SClamp %int_2 %int_0 %int_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 1),
  // Test case 44: SClamp 1 2 x
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%undef = OpUndef %int\n" +
          "%2 = OpExtInst %int %1 SClamp %int_1 %int_2 %undef\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 2),
  // Test case 45: SClamp 2 x 1
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%undef = OpUndef %int\n" +
          "%2 = OpExtInst %int %1 SClamp %int_2 %undef %int_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 1),
  // Test case 46: UClamp 1 2 x
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%undef = OpUndef %uint\n" +
          "%2 = OpExtInst %uint %1 UClamp %uint_1 %uint_2 %undef\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 2),
  // Test case 47: UClamp 2 x 1
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%undef = OpUndef %uint\n" +
          "%2 = OpExtInst %uint %1 UClamp %uint_2 %undef %uint_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 1),
    // Test case 48: Bit-cast int 0 to unsigned int
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %uint %int_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 49: Bit-cast int -24 to unsigned int
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %uint %int_n24\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, static_cast<uint32_t>(-24)),
    // Test case 50: Bit-cast float 1.0f to unsigned int
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %uint %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, static_cast<uint32_t>(0x3f800000)),
    // Test case 51: Bit-cast ushort 0xBC00 to ushort
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %ushort %ushort_0xBC00\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0xBC00),
    // Test case 52: Bit-cast short 0xBC00 to ushort
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %ushort %short_0xBC00\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0xBC00),
    // Test case 53: Bit-cast half 1 to ushort
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %ushort %half_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0x3C00),
    // Test case 54: Bit-cast ushort 0xBC00 to short
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %short %ushort_0xBC00\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0xFFFFBC00),
    // Test case 55: Bit-cast short 0xBC00 to short
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %short %short_0xBC00\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0xFFFFBC00),
    // Test case 56: Bit-cast half 1 to short
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %short %half_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0x3C00),
    // Test case 57: Bit-cast ushort 0xBC00 to half
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %half %ushort_0xBC00\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0xBC00),
    // Test case 58: Bit-cast short 0xBC00 to half
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %half %short_0xBC00\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0xFFFFBC00),
    // Test case 59: Bit-cast half 1 to half
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %half %half_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0x3C00),
    // Test case 60: Bit-cast ubyte 1 to byte
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %byte %ubyte_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1),
    // Test case 61: Bit-cast byte -1 to ubyte
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpBitcast %ubyte %byte_n1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0xFF),
    // Test case 62: Negate 2.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpSNegate %int %int_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, -2),
    // Test case 63: Negate negative short.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpSNegate %short %short_0xBC00\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0x4400 /* expected to be sign extended. */),
    // Test case 64: Negate positive short.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpSNegate %short %short_0x4400\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0xFFFFBC00 /* expected to be sign extended. */),
    // Test case 65: Negate a negative short.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpSNegate %ushort %ushort_0xBC00\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0x4400 /* expected to be zero extended. */),
    // Test case 66: Negate positive short.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpSNegate %ushort %ushort_0x4400\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0xBC00 /* expected to be zero extended. */),
    // Test case 67: Fold 2 + 3 (short)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpIAdd %short %short_2 %short_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 5),
    // Test case 68: Fold 2 + -5 (short)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpIAdd %short %short_2 %short_n5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, -3),
  // Test case 69: Fold int(3ll)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSConvert %int %long_3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 3),
  // Test case 70: Fold short(-3ll)
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSConvert %short %long_n3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, -3),
  // Test case 71: Fold short(32768ll) - This should do a sign extend when
  // converting to short.
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSConvert %short %long_32768\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, -32768),
  // Test case 72: Fold short(-57344) - This should do a sign extend when
  // converting to short making the upper bits 0.
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSConvert %short %long_n57344\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 8192),
  // Test case 73: Fold int(-5(short)). The -5 should be interpreted as an unsigned value, and be zero extended to 32-bits.
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpUConvert %uint %short_n5\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 65531),
  // Test case 74: Fold short(-24(int)). The upper bits should be cleared. So 0xFFFFFFE8 should become 0x0000FFE8.
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpUConvert %ushort %int_n24\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 65512)
));
// clang-format on

using LongIntegerInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<uint64_t>>;

TEST_P(LongIntegerInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);
  CheckForExpectedScalarConstant(
      inst, tc.expected_result, [](const analysis::Constant* c) {
        return c->AsScalarConstant()->GetU64BitValue();
      });
}

INSTANTIATE_TEST_SUITE_P(
    TestCase, LongIntegerInstructionFoldingTest,
    ::testing::Values(
        // Test case 0: fold 1+4611686018427387904
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpIAdd %long %long_1 %long_4611686018427387904\n" +
                "OpReturn\n" + "OpFunctionEnd",
            2, 1 + 4611686018427387904),
        // Test case 1: fold 1-4611686018427387904
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpISub %long %long_1 %long_4611686018427387904\n" +
                "OpReturn\n" + "OpFunctionEnd",
            2, 1 - 4611686018427387904),
        // Test case 2: fold 2*4611686018427387904
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpIMul %long %long_2 %long_4611686018427387904\n" +
                "OpReturn\n" + "OpFunctionEnd",
            2, 9223372036854775808ull),
        // Test case 3: fold 4611686018427387904/2 (unsigned)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpUDiv %ulong %ulong_4611686018427387904 %ulong_2\n" +
                "OpReturn\n" + "OpFunctionEnd",
            2, 4611686018427387904 / 2),
        // Test case 4: fold 4611686018427387904/2 (signed)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpSDiv %long %long_4611686018427387904 %long_2\n" +
                "OpReturn\n" + "OpFunctionEnd",
            2, 4611686018427387904 / 2),
        // Test case 5: fold -4611686018427387904/2 (signed)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpSDiv %long %long_n4611686018427387904 %long_2\n" +
                "OpReturn\n" + "OpFunctionEnd",
            2, -4611686018427387904 / 2),
        // Test case 6: fold 4611686018427387904 mod 7 (unsigned)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpUMod %ulong %ulong_4611686018427387904 %ulong_7\n" +
                "OpReturn\n" + "OpFunctionEnd",
            2, 4611686018427387904ull % 7ull),
        // Test case 7: fold 7 mod 3 (signed)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpSMod %long %long_7 %long_3\n" + "OpReturn\n" +
                "OpFunctionEnd",
            2, 1ull),
        // Test case 8: fold 7 rem 3 (signed)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpSRem %long %long_7 %long_3\n" + "OpReturn\n" +
                "OpFunctionEnd",
            2, 1ull),
        // Test case 9: fold 7 mod -3 (signed)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpSMod %long %long_7 %long_n3\n" + "OpReturn\n" +
                "OpFunctionEnd",
            2, -2ll),
        // Test case 10: fold 7 rem 3 (signed)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpSRem %long %long_7 %long_n3\n" + "OpReturn\n" +
                "OpFunctionEnd",
            2, 1ll),
        // Test case 11: fold -7 mod 3 (signed)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpSMod %long %long_n7 %long_3\n" + "OpReturn\n" +
                "OpFunctionEnd",
            2, 2ll),
        // Test case 12: fold -7 rem 3 (signed)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpSRem %long %long_n7 %long_3\n" + "OpReturn\n" +
                "OpFunctionEnd",
            2, -1ll),
        // Test case 13: fold long(-24)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" +
                "%2 = OpSConvert %long %int_n24\n" + "OpReturn\n" +
                "OpFunctionEnd",
            2, -24ll),
        // Test case 14: fold long(-24)
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%n = OpVariable %_ptr_int Function\n" +
                "%load = OpLoad %int %n\n" + "%2 = OpSConvert %long %int_10\n" +
                "OpReturn\n" + "OpFunctionEnd",
            2, 10ll),
        // Test case 15: fold long(-24(short)).
        // The upper bits should be cleared. So 0xFFFFFFE8 should become
        // 0x000000000000FFE8.
        InstructionFoldingCase<uint64_t>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" + "%2 = OpUConvert %ulong %short_n5\n" +
                "OpReturn\n" + "OpFunctionEnd",
            2, 65531ull)));

using UIntVectorInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<std::vector<uint32_t>>>;

TEST_P(UIntVectorInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);
  CheckForExpectedVectorConstant(
      inst, tc.expected_result,
      [](const analysis::Constant* c) { return c->GetU32(); });
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(TestCase, UIntVectorInstructionFoldingTest,
::testing::Values(
    // Test case 0: fold 0*n
    InstructionFoldingCase<std::vector<uint32_t>>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load = OpLoad %int %n\n" +
            "%2 = OpVectorShuffle %v2int %v2int_2_2 %v2int_2_3 0 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, {2,3}),
    InstructionFoldingCase<std::vector<uint32_t>>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpVectorShuffle %v2int %v2int_null %v2int_2_3 0 3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, {0,3}),
    // Test case 4: fold bit-cast int -24 to unsigned int
    InstructionFoldingCase<std::vector<uint32_t>>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpBitcast %v2uint %v2int_min_max\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, {2147483648, 2147483647}),
    // Test case 5: fold SNegate vector of uint
    InstructionFoldingCase<std::vector<uint32_t>>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpSNegate %v2uint %v2uint_0x3f800000_0xbf800000\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, {static_cast<uint32_t>(-0x3f800000), static_cast<uint32_t>(-0xbf800000)}),
    // Test case 6: fold vector components of uint (including integer overflow)
    InstructionFoldingCase<std::vector<uint32_t>>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpIAdd %v2uint %v2uint_0x3f800000_0xbf800000 %v2uint_0x3f800000_0xbf800000\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, {0x7f000000u, 0x7f000000u}),
    // Test case 6: fold vector components of uint
    InstructionFoldingCase<std::vector<uint32_t>>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpSConvert %v2int %v2short_2_n5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, {2,static_cast<uint32_t>(-5)}),
    // Test case 6: fold vector components of uint (incuding integer overflow)
    InstructionFoldingCase<std::vector<uint32_t>>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpUConvert %v2uint %v2short_2_n5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, {2,65531})
));
// clang-format on

using IntVectorInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<std::vector<int32_t>>>;

TEST_P(IntVectorInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);

  CheckForExpectedVectorConstant(
      inst, tc.expected_result,
      [](const analysis::Constant* c) { return c->GetS32(); });
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(TestCase, IntVectorInstructionFoldingTest,
::testing::Values(
    // Test case 0: fold negate of a vector
    InstructionFoldingCase<std::vector<int32_t>>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSNegate %v2int %v2int_2_3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, {-2, -3}),
    // Test case 1: fold negate of a vector containing negative values.
    InstructionFoldingCase<std::vector<int32_t>>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSNegate %v2int %v2int_n1_n24\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, {1, 24}),
    // Test case 2: fold negate of a vector at the limits
    InstructionFoldingCase<std::vector<int32_t>>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSNegate %v2int %v2int_min_max\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, {INT_MIN, -INT_MAX}),
    // Test case 3: fold vector components of int
    InstructionFoldingCase<std::vector<int32_t>>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpIMul %v2int %v2int_2_3 %v2int_2_3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, {4,9})
));
// clang-format on

using LongIntVectorInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<std::vector<uint64_t>>>;

TEST_P(LongIntVectorInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);
  CheckForExpectedVectorConstant(
      inst, tc.expected_result,
      [](const analysis::Constant* c) { return c->GetU64(); });
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(TestCase, LongIntVectorInstructionFoldingTest,
  ::testing::Values(
     // Test case 0: fold {2,2} + {2,3} (Testing that the vector logic works
     // correctly. Scalar tests will check that the 64-bit values are correctly
     // folded.)
     InstructionFoldingCase<std::vector<uint64_t>>(
         Header() + "%main = OpFunction %void None %void_func\n" +
             "%main_lab = OpLabel\n" +
             "%n = OpVariable %_ptr_int Function\n" +
             "%load = OpLoad %int %n\n" +
             "%2 = OpIAdd %v2long %v2long_2_2 %v2long_2_3\n" +
             "OpReturn\n" +
             "OpFunctionEnd",
         2, {4,5}),
      // Test case 0: fold {2,2} / {2,3} (Testing that the vector logic works
      // correctly. Scalar tests will check that the 64-bit values are correctly
      // folded.)
     InstructionFoldingCase<std::vector<uint64_t>>(
         Header() + "%main = OpFunction %void None %void_func\n" +
             "%main_lab = OpLabel\n" +
             "%n = OpVariable %_ptr_int Function\n" +
             "%load = OpLoad %int %n\n" +
             "%2 = OpSDiv %v2long %v2long_2_2 %v2long_2_3\n" +
             "OpReturn\n" +
             "OpFunctionEnd",
         2, {1,0})
  ));
// clang-format on

using DoubleVectorInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<std::vector<double>>>;

TEST_P(DoubleVectorInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);
  CheckForExpectedVectorConstant(
      inst, tc.expected_result,
      [](const analysis::Constant* c) { return c->GetDouble(); });
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(TestCase, DoubleVectorInstructionFoldingTest,
::testing::Values(
   // Test case 0: bit-cast int {0x3FF00000,0x00000000,0xC05FD666,0x66666666}
   //              to double vector
   InstructionFoldingCase<std::vector<double>>(
       Header() + "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%2 = OpBitcast %v2double %v4int_0x3FF00000_0x00000000_0xC05FD666_0x66666666\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       2, {1.0,-127.35}),
   // Test case 1: OpVectorTimesMatrix Non-Zero Zero {{0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}} {1.0, 2.0, 3.0, 4.0} {0.0, 0.0, 0.0, 0.0}
   InstructionFoldingCase<std::vector<double>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpVectorTimesMatrix %v4double %v4double_1_2_3_4 %mat4v4double_null\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {0.0,0.0,0.0,0.0}),
   // Test case 2: OpVectorTimesMatrix Zero Non-Zero {{1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}} {0.0, 0.0, 0.0, 0.0} {0.0, 0.0, 0.0, 0.0}
   InstructionFoldingCase<std::vector<double>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpVectorTimesMatrix %v4double %v4double_null %mat4v4double_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {0.0,0.0,0.0,0.0}),
   // Test case 3: OpVectorTimesMatrix Non-Zero Non-Zero {{1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}} {1.0, 2.0, 3.0, 4.0} {30.0, 30.0, 30.0, 30.0}
   InstructionFoldingCase<std::vector<double>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpVectorTimesMatrix %v4double %v4double_1_2_3_4 %mat4v4double_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {30.0,30.0,30.0,30.0}),
   // Test case 4: OpVectorTimesMatrix Non-Zero Non-Zero {{1.0, 2.0, 3.0, 4.0}, Null, {1.0, 2.0, 3.0, 4.0}, Null} {1.0, 2.0, 3.0, 4.0} {30.0, 0.0, 30.0, 0.0}
   InstructionFoldingCase<std::vector<double>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpVectorTimesMatrix %v4double %v4double_1_2_3_4 %mat4v4double_1_2_3_4_null\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {30.0,0.0,30.0,0.0}),
   // Test case 5: OpMatrixTimesVector Zero Non-Zero {1.0, 2.0, 3.0, 4.0} {{0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}} {0.0, 0.0, 0.0, 0.0}
   InstructionFoldingCase<std::vector<double>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpMatrixTimesVector %v4double %mat4v4double_null %v4double_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {0.0,0.0,0.0,0.0}),
   // Test case 6: OpMatrixTimesVector Non-Zero Zero {{1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}} {0.0, 0.0, 0.0, 0.0} {0.0, 0.0, 0.0, 0.0}
   InstructionFoldingCase<std::vector<double>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpMatrixTimesVector %v4double %mat4v4double_1_2_3_4 %v4double_null\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {0.0,0.0,0.0,0.0}),
   // Test case 7: OpMatrixTimesVector Non-Zero Non-Zero {1.0, 2.0, 3.0, 4.0} {{1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}} {10.0, 20.0, 30.0, 40.0}
   InstructionFoldingCase<std::vector<double>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpMatrixTimesVector %v4double %mat4v4double_1_2_3_4 %v4double_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {10.0,20.0,30.0,40.0}),
   // Test case 8: OpMatrixTimesVector Non-Zero Non-Zero {1.0, 2.0, 3.0, 4.0} {{1.0, 2.0, 3.0, 4.0}, Null, {1.0, 2.0, 3.0, 4.0}, Null} {10.0, 20.0, 30.0, 40.0}
   InstructionFoldingCase<std::vector<double>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpMatrixTimesVector %v4double %mat4v4double_1_2_3_4_null %v4double_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {4.0,8.0,12.0,16.0})
));

using FloatVectorInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<std::vector<float>>>;

TEST_P(FloatVectorInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) = FoldInstruction(tc.test_body, tc.id_to_fold,SPV_ENV_UNIVERSAL_1_1);
  CheckForExpectedVectorConstant(inst, tc.expected_result, [](const analysis::Constant* c){ return c->GetFloat();});
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(TestCase, FloatVectorInstructionFoldingTest,
::testing::Values(
   // Test case 0: FMix {2.0, 2.0}, {2.0, 3.0} {0.2,0.5}
   InstructionFoldingCase<std::vector<float>>(
       Header() + "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%2 = OpExtInst %v2float %1 FMix %v2float_2_3 %v2float_0_0 %v2float_0p2_0p5\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       2, {1.6f,1.5f}),
   // Test case 1: bit-cast unsigned int vector {0x3f800000, 0xbf800000} to
   //              float vector
   InstructionFoldingCase<std::vector<float>>(
       Header() + "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%2 = OpBitcast %v2float %v2uint_0x3f800000_0xbf800000\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       2, {1.0f,-1.0f}),
   // Test case 2: bit-cast long int 0xbf8000003f800000 to float vector
   InstructionFoldingCase<std::vector<float>>(
       Header() + "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%2 = OpBitcast %v2float %long_0xbf8000003f800000\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       2, {1.0f,-1.0f}),
   // Test case 3: OpVectorTimesMatrix Non-Zero Zero {{0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}} {1.0, 2.0, 3.0, 4.0} {0.0, 0.0, 0.0, 0.0}
   InstructionFoldingCase<std::vector<float>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpVectorTimesMatrix %v4float %v4float_1_2_3_4 %mat4v4float_null\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {0.0f,0.0f,0.0f,0.0f}),
   // Test case 4: OpVectorTimesMatrix Non-Zero Non-Zero {{1.0, 2.0, 3.0, 4.0}, Null, {1.0, 2.0, 3.0, 4.0}, Null} {1.0, 2.0, 3.0, 4.0} {30.0, 0.0, 30.0, 0.0}
   InstructionFoldingCase<std::vector<float>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpVectorTimesMatrix %v4float %v4float_1_2_3_4 %mat4v4float_1_2_3_4_null\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {30.0,0.0,30.0,0.0}),
   // Test case 5: OpVectorTimesMatrix Zero Non-Zero {{1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}} {0.0, 0.0, 0.0, 0.0} {0.0, 0.0, 0.0, 0.0}
   InstructionFoldingCase<std::vector<float>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpVectorTimesMatrix %v4float %v4float_null %mat4v4float_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {0.0f,0.0f,0.0f,0.0f}),
   // Test case 6: OpVectorTimesMatrix Non-Zero Non-Zero {{1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}} {1.0, 2.0, 3.0, 4.0} {30.0, 30.0, 30.0, 30.0}
   InstructionFoldingCase<std::vector<float>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpVectorTimesMatrix %v4float %v4float_1_2_3_4 %mat4v4float_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {30.0f,30.0f,30.0f,30.0f}),
   // Test case 7: OpMatrixTimesVector Zero Non-Zero {1.0, 2.0, 3.0, 4.0} {{0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}} {0.0, 0.0, 0.0, 0.0}
   InstructionFoldingCase<std::vector<float>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpMatrixTimesVector %v4float %mat4v4float_null %v4float_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {0.0f,0.0f,0.0f,0.0f}),
   // Test case 8: OpMatrixTimesVector Non-Zero Zero {{1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}} {0.0, 0.0, 0.0, 0.0} {0.0, 0.0, 0.0, 0.0}
   InstructionFoldingCase<std::vector<float>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpMatrixTimesVector %v4float %mat4v4float_1_2_3_4 %v4float_null\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {0.0f,0.0f,0.0f,0.0f}),
   // Test case 9: OpMatrixTimesVector Non-Zero Non-Zero {1.0, 2.0, 3.0, 4.0} {{1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}} {10.0, 20.0, 30.0, 40.0}
   InstructionFoldingCase<std::vector<float>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpMatrixTimesVector %v4float %mat4v4float_1_2_3_4 %v4float_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {10.0f,20.0f,30.0f,40.0f}),
   // Test case 10: OpMatrixTimesVector Non-Zero Non-Zero {1.0, 2.0, 3.0, 4.0} {{1.0, 2.0, 3.0, 4.0}, Null, {1.0, 2.0, 3.0, 4.0}, Null} {10.0, 20.0, 30.0, 40.0}
   InstructionFoldingCase<std::vector<float>>(
       Header() +
       "%main = OpFunction %void None %void_func\n" +
       "%main_lab = OpLabel\n" +
       "%2 = OpMatrixTimesVector %v4float %mat4v4float_1_2_3_4_null %v4float_1_2_3_4\n" +
       "OpReturn\n" +
       "OpFunctionEnd",
       2, {4.0,8.0,12.0,16.0})
));
// clang-format on

using FloatMatrixInstructionFoldingTest = ::testing::TestWithParam<
    InstructionFoldingCase<std::vector<std::vector<float>>>>;

TEST_P(FloatMatrixInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);

  EXPECT_EQ(inst->opcode(), spv::Op::OpCopyObject);
  if (inst->opcode() == spv::Op::OpCopyObject) {
    analysis::DefUseManager* def_use_mgr = context->get_def_use_mgr();
    inst = def_use_mgr->GetDef(inst->GetSingleWordInOperand(0));
    analysis::ConstantManager* const_mgr = context->get_constant_mgr();
    const analysis::Constant* result = const_mgr->GetConstantFromInst(inst);
    EXPECT_NE(result, nullptr);
    if (result != nullptr) {
      std::vector<const analysis::Constant*> matrix =
          result->AsMatrixConstant()->GetComponents();
      EXPECT_EQ(matrix.size(), tc.expected_result.size());
      for (size_t c = 0; c < matrix.size(); c++) {
        if (matrix[c]->AsNullConstant() != nullptr) {
          matrix[c] = const_mgr->GetNullCompositeConstant(matrix[c]->type());
        }
        const analysis::VectorConstant* column_const =
            matrix[c]->AsVectorConstant();
        ASSERT_NE(column_const, nullptr);
        const std::vector<const analysis::Constant*>& column =
            column_const->GetComponents();
        EXPECT_EQ(column.size(), tc.expected_result[c].size());
        for (size_t r = 0; r < column.size(); r++) {
          EXPECT_EQ(tc.expected_result[c][r], column[r]->GetFloat());
        }
      }
    }
  }
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(TestCase, FloatMatrixInstructionFoldingTest,
::testing::Values(
   // Test case 0: OpTranspose square null matrix
   InstructionFoldingCase<std::vector<std::vector<float>>>(
       Header() + "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%2 = OpTranspose %mat4v4float %mat4v4float_null\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       2, {{0.0f, 0.0f, 0.0f, 0.0f},{0.0f, 0.0f, 0.0f, 0.0f},{0.0f, 0.0f, 0.0f, 0.0f},{0.0f, 0.0f, 0.0f, 0.0f}}),
   // Test case 1: OpTranspose rectangular null matrix
   InstructionFoldingCase<std::vector<std::vector<float>>>(
       Header() + "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%2 = OpTranspose %mat4v2float %mat2v4float_null\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       2, {{0.0f, 0.0f},{0.0f, 0.0f},{0.0f, 0.0f},{0.0f, 0.0f}}),
   InstructionFoldingCase<std::vector<std::vector<float>>>(
       Header() + "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%2 = OpTranspose %mat4v4float %mat4v4float_1_2_3_4\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       2, {{1.0f, 1.0f, 1.0f, 1.0f},{2.0f, 2.0f, 2.0f, 2.0f},{3.0f, 3.0f, 3.0f, 3.0f},{4.0f, 4.0f, 4.0f, 4.0f}})
));
// clang-format on

using BooleanInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<bool>>;

TEST_P(BooleanInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);
  CheckForExpectedScalarConstant(
      inst, tc.expected_result,
      [](const analysis::Constant* c) { return c->AsBoolConstant()->value(); });
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(TestCase, BooleanInstructionFoldingTest,
                        ::testing::Values(
  // Test case 0: fold true || n
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_bool Function\n" +
          "%load = OpLoad %bool %n\n" +
          "%2 = OpLogicalOr %bool %true %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 1: fold n || true
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_bool Function\n" +
          "%load = OpLoad %bool %n\n" +
          "%2 = OpLogicalOr %bool %load %true\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 2: fold false && n
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_bool Function\n" +
          "%load = OpLoad %bool %n\n" +
          "%2 = OpLogicalAnd %bool %false %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 3: fold n && false
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_bool Function\n" +
          "%load = OpLoad %bool %n\n" +
          "%2 = OpLogicalAnd %bool %load %false\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 4: fold n < 0 (unsigned)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpULessThan %bool %load %uint_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 5: fold UINT_MAX < n (unsigned)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpULessThan %bool %uint_max %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 6: fold INT_MAX < n (signed)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpSLessThan %bool %int_max %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 7: fold n < INT_MIN (signed)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpSLessThan %bool %load %int_min\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 8: fold 0 > n (unsigned)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpUGreaterThan %bool %uint_0 %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 9: fold n > UINT_MAX (unsigned)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpUGreaterThan %bool %load %uint_max\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 10: fold n > INT_MAX (signed)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpSGreaterThan %bool %load %int_max\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 11: fold INT_MIN > n (signed)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpSGreaterThan %bool %int_min %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 12: fold 0 <= n (unsigned)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpULessThanEqual %bool %uint_0 %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 13: fold n <= UINT_MAX (unsigned)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpULessThanEqual %bool %load %uint_max\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 14: fold INT_MIN <= n (signed)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpSLessThanEqual %bool %int_min %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 15: fold n <= INT_MAX (signed)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpSLessThanEqual %bool %load %int_max\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 16: fold n >= 0 (unsigned)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpUGreaterThanEqual %bool %load %uint_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 17: fold UINT_MAX >= n (unsigned)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_uint Function\n" +
          "%load = OpLoad %uint %n\n" +
          "%2 = OpUGreaterThanEqual %bool %uint_max %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 18: fold n >= INT_MIN (signed)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpSGreaterThanEqual %bool %load %int_min\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 19: fold INT_MAX >= n (signed)
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpSGreaterThanEqual %bool %int_max %load\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true)
));

INSTANTIATE_TEST_SUITE_P(FClampAndCmpLHS, BooleanInstructionFoldingTest,
::testing::Values(
    // Test case 0: fold 0.0 > clamp(n, 0.0, 1.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFOrdGreaterThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 1: fold 0.0 > clamp(n, -1.0, -1.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_n1\n" +
            "%2 = OpFOrdGreaterThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 2: fold 0.0 >= clamp(n, 1, 2)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
            "%2 = OpFOrdGreaterThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 3: fold 0.0 >= clamp(n, -1.0, 0.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFOrdGreaterThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 4: fold 0.0 <= clamp(n, 0.0, 1.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFOrdLessThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 5: fold 0.0 <= clamp(n, -1.0, -1.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_n1\n" +
            "%2 = OpFOrdLessThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 6: fold 0.0 < clamp(n, 1, 2)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
            "%2 = OpFOrdLessThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 7: fold 0.0 < clamp(n, -1.0, 0.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFOrdLessThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 8: fold 0.0 > clamp(n, 0.0, 1.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFUnordGreaterThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 9: fold 0.0 > clamp(n, -1.0, -1.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_n1\n" +
            "%2 = OpFUnordGreaterThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 10: fold 0.0 >= clamp(n, 1, 2)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
            "%2 = OpFUnordGreaterThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 11: fold 0.0 >= clamp(n, -1.0, 0.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFUnordGreaterThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 12: fold 0.0 <= clamp(n, 0.0, 1.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFUnordLessThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 13: fold 0.0 <= clamp(n, -1.0, -1.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_n1\n" +
            "%2 = OpFUnordLessThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 14: fold 0.0 < clamp(n, 1, 2)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
            "%2 = OpFUnordLessThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 15: fold 0.0 < clamp(n, -1.0, 0.0)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFUnordLessThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false)
));

INSTANTIATE_TEST_SUITE_P(FClampAndCmpRHS, BooleanInstructionFoldingTest,
::testing::Values(
    // Test case 0: fold clamp(n, 0.0, 1.0) > 1.0
    InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%n = OpVariable %_ptr_float Function\n" +
      "%ld = OpLoad %float %n\n" +
      "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
      "%2 = OpFOrdGreaterThan %bool %clamp %float_1\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
      2, false),
    // Test case 1: fold clamp(n, 1.0, 1.0) > 0.0
    InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%n = OpVariable %_ptr_float Function\n" +
      "%ld = OpLoad %float %n\n" +
      "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_1\n" +
      "%2 = OpFOrdGreaterThan %bool %clamp %float_0\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
      2, true),
    // Test case 2: fold clamp(n, 1, 2) >= 0.0
    InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%n = OpVariable %_ptr_float Function\n" +
      "%ld = OpLoad %float %n\n" +
      "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
      "%2 = OpFOrdGreaterThanEqual %bool %clamp %float_0\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
      2, true),
    // Test case 3: fold clamp(n, 1.0, 2.0) >= 3.0
    InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%n = OpVariable %_ptr_float Function\n" +
      "%ld = OpLoad %float %n\n" +
      "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
      "%2 = OpFOrdGreaterThanEqual %bool %clamp %float_3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
      2, false),
    // Test case 4: fold clamp(n, 0.0, 1.0) <= 1.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFOrdLessThanEqual %bool %clamp %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 5: fold clamp(n, 1.0, 2.0) <= 0.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
            "%2 = OpFOrdLessThanEqual %bool %clamp %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 6: fold clamp(n, 1, 2) < 3
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
            "%2 = OpFOrdLessThan %bool %clamp %float_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 7: fold clamp(n, -1.0, 0.0) < -1.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFOrdLessThan %bool %clamp %float_n1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 8: fold clamp(n, 0.0, 1.0) > 1.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFUnordGreaterThan %bool %clamp %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 9: fold clamp(n, 1.0, 2.0) > 0.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
            "%2 = OpFUnordGreaterThan %bool %clamp %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 10: fold clamp(n, 1, 2) >= 3.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
            "%2 = OpFUnordGreaterThanEqual %bool %clamp %float_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 11: fold clamp(n, -1.0, 0.0) >= -1.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFUnordGreaterThanEqual %bool %clamp %float_n1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 12: fold clamp(n, 0.0, 1.0) <= 1.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFUnordLessThanEqual %bool %clamp %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 13: fold clamp(n, 1.0, 1.0) <= 0.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_1\n" +
            "%2 = OpFUnordLessThanEqual %bool %clamp %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 14: fold clamp(n, 1, 2) < 3
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_1 %float_2\n" +
            "%2 = OpFUnordLessThan %bool %clamp %float_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 15: fold clamp(n, -1.0, 0.0) < -1.0
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFUnordLessThan %bool %clamp %float_n1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false),
    // Test case 16: fold clamp(n, -1.0, 0.0) < -1.0 (one test for double)
    InstructionFoldingCase<bool>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%ld = OpLoad %double %n\n" +
            "%clamp = OpExtInst %double %1 FClamp %ld %double_n1 %double_0\n" +
            "%2 = OpFUnordLessThan %bool %clamp %double_n1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, false)
));
// clang-format on

using FloatInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<float>>;

TEST_P(FloatInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);

  CheckForExpectedScalarConstant(inst, tc.expected_result,
                                 [](const analysis::Constant* c) {
                                   return c->AsFloatConstant()->GetFloatValue();
                                 });
}

// Not testing NaNs because there are no expectations concerning NaNs according
// to the "Precision and Operation of SPIR-V Instructions" section of the Vulkan
// specification.

// clang-format off
INSTANTIATE_TEST_SUITE_P(FloatConstantFoldingTest, FloatInstructionFoldingTest,
::testing::Values(
    // Test case 0: Fold 2.0 - 1.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFSub %float %float_2 %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1.0),
    // Test case 1: Fold 2.0 + 1.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFAdd %float %float_2 %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3.0),
    // Test case 2: Fold 3.0 * 2.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFMul %float %float_3 %float_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 6.0),
    // Test case 3: Fold 1.0 / 2.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFDiv %float %float_1 %float_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.5),
    // Test case 4: Fold 1.0 / 0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFDiv %float %float_1 %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, std::numeric_limits<float>::infinity()),
    // Test case 5: Fold -1.0 / 0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFDiv %float %float_n1 %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, -std::numeric_limits<float>::infinity()),
    // Test case 6: Fold (2.0, 3.0) dot (2.0, 0.5)
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpDot %float %v2float_2_3 %v2float_2_0p5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 5.5f),
    // Test case 7: Fold (0.0, 0.0) dot v
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%v = OpVariable %_ptr_v2float Function\n" +
            "%2 = OpLoad %v2float %v\n" +
            "%3 = OpDot %float %v2float_0_0 %2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, 0.0f),
    // Test case 8: Fold v dot (0.0, 0.0)
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%v = OpVariable %_ptr_v2float Function\n" +
            "%2 = OpLoad %v2float %v\n" +
            "%3 = OpDot %float %2 %v2float_0_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, 0.0f),
    // Test case 9: Fold Null dot v
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%v = OpVariable %_ptr_v2float Function\n" +
            "%2 = OpLoad %v2float %v\n" +
            "%3 = OpDot %float %v2float_null %2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, 0.0f),
    // Test case 10: Fold v dot Null
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%v = OpVariable %_ptr_v2float Function\n" +
            "%2 = OpLoad %v2float %v\n" +
            "%3 = OpDot %float %2 %v2float_null\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, 0.0f),
    // Test case 11: Fold -2.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFNegate %float %float_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, -2),
    // Test case 12: QuantizeToF16 1.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpQuantizeToF16 %float %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1.0),
    // Test case 13: QuantizeToF16 positive non exact
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpQuantizeToF16 %float %float_2049\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 2048),
    // Test case 14: QuantizeToF16 negative non exact
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpQuantizeToF16 %float %float_n2049\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, -2048),
    // Test case 15: QuantizeToF16 large positive
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpQuantizeToF16 %float %float_1e16\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, std::numeric_limits<float>::infinity()),
    // Test case 16: QuantizeToF16 large negative
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpQuantizeToF16 %float %float_n1e16\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, -std::numeric_limits<float>::infinity()),
    // Test case 17: QuantizeToF16 small positive
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpQuantizeToF16 %float %float_1en16\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.0),
    // Test case 18: QuantizeToF16 small negative
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpQuantizeToF16 %float %float_n1en16\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.0),
    // Test case 19: QuantizeToF16 nan
    InstructionFoldingCase<float>(
        HeaderWithNaN() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpQuantizeToF16 %float %float_nan\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, std::numeric_limits<float>::quiet_NaN()),
    // Test case 20: FMix 1.0 4.0 0.2
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 FMix %float_1 %float_4 %float_0p2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1.6f),
    // Test case 21: FMin 1.0 4.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 FMin %float_1 %float_4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1.0f),
    // Test case 22: FMin 4.0 0.2
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 FMin %float_4 %float_0p2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.2f),
    // Test case 23: FMax 1.0 4.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 FMax %float_1 %float_4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 4.0f),
    // Test case 24: FMax 1.0 0.2
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 FMax %float_1 %float_0p2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1.0f),
    // Test case 25: FClamp 1.0 0.2 4.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 FClamp %float_1 %float_0p2 %float_4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1.0f),
    // Test case 26: FClamp 0.2 2.0 4.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 FClamp %float_0p2 %float_2 %float_4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 2.0f),
    // Test case 27: FClamp 2049.0 2.0 4.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 FClamp %float_2049 %float_2 %float_4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 4.0f),
    // Test case 28: FClamp 1.0 2.0 x
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%undef = OpUndef %float\n" +
            "%2 = OpExtInst %float %1 FClamp %float_1 %float_2 %undef\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 2.0),
    // Test case 29: FClamp 1.0 x 0.5
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%undef = OpUndef %float\n" +
            "%2 = OpExtInst %float %1 FClamp %float_1 %undef %float_0p5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.5),
    // Test case 30: Sin 0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Sin %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.0),
    // Test case 31: Cos 0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Cos %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1.0),
    // Test case 32: Tan 0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Tan %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.0),
    // Test case 33: Asin 0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Asin %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.0),
    // Test case 34: Acos 1.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Acos %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.0),
    // Test case 35: Atan 0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Atan %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.0),
    // Test case 36: Exp 0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Exp %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1.0),
    // Test case 37: Log 1.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Log %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.0),
    // Test case 38: Exp2 2.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Exp2 %float_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 4.0),
    // Test case 39: Log2 4.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Log2 %float_4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 2.0),
    // Test case 40: Sqrt 4.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Sqrt %float_4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 2.0),
    // Test case 41: Atan2 0.0 1.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Atan2 %float_0 %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0.0),
    // Test case 42: Pow 2.0 3.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpExtInst %float %1 Pow %float_2 %float_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 8.0),
    // Test case 43: Fold 1.0 / -0.0.
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFDiv %float %float_1 %float_n0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, -std::numeric_limits<float>::infinity()),
    // Test case 44: Fold -1.0 / -0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFDiv %float %float_n1 %float_n0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, std::numeric_limits<float>::infinity()),
    // Test case 45: Fold 0.0 / 0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFDiv %float %float_0 %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, std::numeric_limits<float>::quiet_NaN()),
    // Test case 46: Fold 0.0 / -0.0
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFDiv %float %float_0 %float_n0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, std::numeric_limits<float>::quiet_NaN())
));
// clang-format on

using DoubleInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<double>>;

TEST_P(DoubleInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);
  CheckForExpectedScalarConstant(
      inst, tc.expected_result, [](const analysis::Constant* c) {
        return c->AsFloatConstant()->GetDoubleValue();
      });
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(DoubleConstantFoldingTest, DoubleInstructionFoldingTest,
::testing::Values(
    // Test case 0: Fold 2.0 - 1.0
    InstructionFoldingCase<double>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFSub %double %double_2 %double_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 1.0),
        // Test case 1: Fold 2.0 + 1.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFAdd %double %double_2 %double_1\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 3.0),
        // Test case 2: Fold 3.0 * 2.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFMul %double %double_3 %double_2\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 6.0),
        // Test case 3: Fold 1.0 / 2.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFDiv %double %double_1 %double_2\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 0.5),
        // Test case 4: Fold 1.0 / 0.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFDiv %double %double_1 %double_0\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, std::numeric_limits<double>::infinity()),
        // Test case 5: Fold -1.0 / 0.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFDiv %double %double_n1 %double_0\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, -std::numeric_limits<double>::infinity()),
        // Test case 6: Fold (2.0, 3.0) dot (2.0, 0.5)
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpDot %double %v2double_2_3 %v2double_2_0p5\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 5.5f),
        // Test case 7: Fold (0.0, 0.0) dot v
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%v = OpVariable %_ptr_v2double Function\n" +
                "%2 = OpLoad %v2double %v\n" +
                "%3 = OpDot %double %v2double_0_0 %2\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            3, 0.0f),
        // Test case 8: Fold v dot (0.0, 0.0)
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%v = OpVariable %_ptr_v2double Function\n" +
                "%2 = OpLoad %v2double %v\n" +
                "%3 = OpDot %double %2 %v2double_0_0\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            3, 0.0f),
        // Test case 9: Fold Null dot v
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%v = OpVariable %_ptr_v2double Function\n" +
                "%2 = OpLoad %v2double %v\n" +
                "%3 = OpDot %double %v2double_null %2\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            3, 0.0f),
        // Test case 10: Fold v dot Null
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%v = OpVariable %_ptr_v2double Function\n" +
                "%2 = OpLoad %v2double %v\n" +
                "%3 = OpDot %double %2 %v2double_null\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            3, 0.0f),
        // Test case 11: Fold -2.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFNegate %double %double_2\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, -2),
        // Test case 12: FMin 1.0 4.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpExtInst %double %1 FMin %double_1 %double_4\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 1.0),
        // Test case 13: FMin 4.0 0.2
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpExtInst %double %1 FMin %double_4 %double_0p2\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 0.2),
        // Test case 14: FMax 1.0 4.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpExtInst %double %1 FMax %double_1 %double_4\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 4.0),
        // Test case 15: FMax 1.0 0.2
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpExtInst %double %1 FMax %double_1 %double_0p2\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 1.0),
        // Test case 16: FClamp 1.0 0.2 4.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpExtInst %double %1 FClamp %double_1 %double_0p2 %double_4\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 1.0),
        // Test case 17: FClamp 0.2 2.0 4.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpExtInst %double %1 FClamp %double_0p2 %double_2 %double_4\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 2.0),
        // Test case 18: FClamp 5.0 2.0 4.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpExtInst %double %1 FClamp %double_5 %double_2 %double_4\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 4.0),
        // Test case 19: FClamp 1.0 2.0 x
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%undef = OpUndef %double\n" +
                "%2 = OpExtInst %double %1 FClamp %double_1 %double_2 %undef\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 2.0),
        // Test case 20: FClamp 1.0 x 0.5
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%undef = OpUndef %double\n" +
                "%2 = OpExtInst %double %1 FClamp %double_1 %undef %double_0p5\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 0.5),
        // Test case 21: Sqrt 4.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%undef = OpUndef %double\n" +
                "%2 = OpExtInst %double %1 Sqrt %double_4\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 2.0),
        // Test case 22: Pow 2.0 3.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%undef = OpUndef %double\n" +
                "%2 = OpExtInst %double %1 Pow %double_2 %double_3\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, 8.0),
        // Test case 23: Fold 1.0 / -0.0.
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFDiv %double %double_1 %double_n0\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, -std::numeric_limits<double>::infinity()),
        // Test case 24: Fold -1.0 / -0.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFDiv %double %double_n1 %double_n0\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, std::numeric_limits<double>::infinity()),
        // Test case 25: Fold 0.0 / 0.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFDiv %double %double_0 %double_0\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, std::numeric_limits<double>::quiet_NaN()),
        // Test case 26: Fold 0.0 / -0.0
        InstructionFoldingCase<double>(
            Header() + "%main = OpFunction %void None %void_func\n" +
                "%main_lab = OpLabel\n" +
                "%2 = OpFDiv %double %double_0 %double_n0\n" +
                "OpReturn\n" +
                "OpFunctionEnd",
            2, std::numeric_limits<double>::quiet_NaN())
));
// clang-format on

// clang-format off
INSTANTIATE_TEST_SUITE_P(DoubleOrderedCompareConstantFoldingTest, BooleanInstructionFoldingTest,
                        ::testing::Values(
  // Test case 0: fold 1.0 == 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdEqual %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 1: fold 1.0 != 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdNotEqual %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 2: fold 1.0 < 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThan %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 3: fold 1.0 > 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThan %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 4: fold 1.0 <= 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThanEqual %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 5: fold 1.0 >= 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThanEqual %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 6: fold 1.0 == 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdEqual %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 7: fold 1.0 != 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdNotEqual %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 8: fold 1.0 < 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThan %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 9: fold 1.0 > 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThan %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 10: fold 1.0 <= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThanEqual %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 11: fold 1.0 >= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThanEqual %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 12: fold 2.0 < 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThan %bool %double_2 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 13: fold 2.0 > 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThan %bool %double_2 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 14: fold 2.0 <= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThanEqual %bool %double_2 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 15: fold 2.0 >= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThanEqual %bool %double_2 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true)
));

INSTANTIATE_TEST_SUITE_P(DoubleUnorderedCompareConstantFoldingTest, BooleanInstructionFoldingTest,
                        ::testing::Values(
  // Test case 0: fold 1.0 == 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordEqual %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 1: fold 1.0 != 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordNotEqual %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 2: fold 1.0 < 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThan %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 3: fold 1.0 > 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThan %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 4: fold 1.0 <= 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThanEqual %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 5: fold 1.0 >= 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThanEqual %bool %double_1 %double_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 6: fold 1.0 == 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordEqual %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 7: fold 1.0 != 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordNotEqual %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 8: fold 1.0 < 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThan %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 9: fold 1.0 > 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThan %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 10: fold 1.0 <= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThanEqual %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 11: fold 1.0 >= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThanEqual %bool %double_1 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 12: fold 2.0 < 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThan %bool %double_2 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 13: fold 2.0 > 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThan %bool %double_2 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 14: fold 2.0 <= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThanEqual %bool %double_2 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 15: fold 2.0 >= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThanEqual %bool %double_2 %double_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true)
));

INSTANTIATE_TEST_SUITE_P(FloatOrderedCompareConstantFoldingTest, BooleanInstructionFoldingTest,
                        ::testing::Values(
  // Test case 0: fold 1.0 == 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdEqual %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 1: fold 1.0 != 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdNotEqual %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 2: fold 1.0 < 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThan %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 3: fold 1.0 > 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThan %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 4: fold 1.0 <= 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThanEqual %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 5: fold 1.0 >= 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThanEqual %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 6: fold 1.0 == 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdEqual %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 7: fold 1.0 != 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdNotEqual %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 8: fold 1.0 < 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThan %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 9: fold 1.0 > 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThan %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 10: fold 1.0 <= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThanEqual %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 11: fold 1.0 >= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThanEqual %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 12: fold 2.0 < 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThan %bool %float_2 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 13: fold 2.0 > 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThan %bool %float_2 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 14: fold 2.0 <= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdLessThanEqual %bool %float_2 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 15: fold 2.0 >= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdGreaterThanEqual %bool %float_2 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true)
));

INSTANTIATE_TEST_SUITE_P(FloatUnorderedCompareConstantFoldingTest, BooleanInstructionFoldingTest,
                        ::testing::Values(
  // Test case 0: fold 1.0 == 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordEqual %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 1: fold 1.0 != 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordNotEqual %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 2: fold 1.0 < 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThan %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 3: fold 1.0 > 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThan %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 4: fold 1.0 <= 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThanEqual %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 5: fold 1.0 >= 2.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThanEqual %bool %float_1 %float_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 6: fold 1.0 == 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordEqual %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 7: fold 1.0 != 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordNotEqual %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 8: fold 1.0 < 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThan %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 9: fold 1.0 > 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThan %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 10: fold 1.0 <= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThanEqual %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 11: fold 1.0 >= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThanEqual %bool %float_1 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 12: fold 2.0 < 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThan %bool %float_2 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 13: fold 2.0 > 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThan %bool %float_2 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 14: fold 2.0 <= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordLessThanEqual %bool %float_2 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 15: fold 2.0 >= 1.0
  InstructionFoldingCase<bool>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordGreaterThanEqual %bool %float_2 %float_1\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true)
));

INSTANTIATE_TEST_SUITE_P(DoubleNaNCompareConstantFoldingTest, BooleanInstructionFoldingTest,
                        ::testing::Values(
  // Test case 0: fold NaN == 0 (ord)
  InstructionFoldingCase<bool>(
      HeaderWithNaN() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdEqual %bool %double_nan %double_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 1: fold NaN == NaN (unord)
  InstructionFoldingCase<bool>(
      HeaderWithNaN() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordEqual %bool %double_nan %double_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 2: fold NaN != NaN (ord)
  InstructionFoldingCase<bool>(
      HeaderWithNaN() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdNotEqual %bool %double_nan %double_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 3: fold NaN != NaN (unord)
  InstructionFoldingCase<bool>(
      HeaderWithNaN() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordNotEqual %bool %double_nan %double_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true)
));

INSTANTIATE_TEST_SUITE_P(FloatNaNCompareConstantFoldingTest, BooleanInstructionFoldingTest,
                        ::testing::Values(
  // Test case 0: fold NaN == 0 (ord)
  InstructionFoldingCase<bool>(
      HeaderWithNaN() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdEqual %bool %float_nan %float_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 1: fold NaN == NaN (unord)
  InstructionFoldingCase<bool>(
      HeaderWithNaN() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordEqual %bool %float_nan %float_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 2: fold NaN != NaN (ord)
  InstructionFoldingCase<bool>(
      HeaderWithNaN() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFOrdNotEqual %bool %float_nan %float_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, false),
  // Test case 3: fold NaN != NaN (unord)
  InstructionFoldingCase<bool>(
      HeaderWithNaN() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpFUnordNotEqual %bool %float_nan %float_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true)
));
// clang-format on

template <class ResultType>
struct InstructionFoldingCaseWithMap {
  InstructionFoldingCaseWithMap(const std::string& tb, uint32_t id,
                                ResultType result,
                                std::function<uint32_t(uint32_t)> map)
      : test_body(tb), id_to_fold(id), expected_result(result), id_map(map) {}

  std::string test_body;
  uint32_t id_to_fold;
  ResultType expected_result;
  std::function<uint32_t(uint32_t)> id_map;
};

using IntegerInstructionFoldingTestWithMap =
    ::testing::TestWithParam<InstructionFoldingCaseWithMap<uint32_t>>;

TEST_P(IntegerInstructionFoldingTestWithMap, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      GetInstructionToFold(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_5);

  inst = context->get_instruction_folder().FoldInstructionToConstant(inst,
                                                                     tc.id_map);
  EXPECT_NE(inst, nullptr);

  CheckForExpectedScalarConstant(inst, tc.expected_result,
                                 [](const analysis::Constant* c) {
                                   return c->AsIntConstant()->GetU32BitValue();
                                 });
}
// clang-format off

INSTANTIATE_TEST_SUITE_P(TestCase, IntegerInstructionFoldingTestWithMap,
  ::testing::Values(
      // Test case 0: fold %3 = 0; %3 * n
      InstructionFoldingCaseWithMap<uint32_t>(
          Header() + "%main = OpFunction %void None %void_func\n" +
              "%main_lab = OpLabel\n" +
              "%n = OpVariable %_ptr_int Function\n" +
              "%load = OpLoad %int %n\n" +
              "%3 = OpCopyObject %int %int_0\n"
              "%2 = OpIMul %int %3 %load\n" +
              "OpReturn\n" +
              "OpFunctionEnd",
          2, 0, [](uint32_t id) {return (id == 3 ? INT_0_ID : id);})
  ));
// clang-format on

using BooleanInstructionFoldingTestWithMap =
    ::testing::TestWithParam<InstructionFoldingCaseWithMap<bool>>;

TEST_P(BooleanInstructionFoldingTestWithMap, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      GetInstructionToFold(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_5);
  inst = context->get_instruction_folder().FoldInstructionToConstant(inst,
                                                                     tc.id_map);
  ASSERT_NE(inst, nullptr);
  CheckForExpectedScalarConstant(
      inst, tc.expected_result,
      [](const analysis::Constant* c) { return c->AsBoolConstant()->value(); });
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(TestCase, BooleanInstructionFoldingTestWithMap,
  ::testing::Values(
      // Test case 0: fold %3 = true; %3 || n
      InstructionFoldingCaseWithMap<bool>(
          Header() + "%main = OpFunction %void None %void_func\n" +
              "%main_lab = OpLabel\n" +
              "%n = OpVariable %_ptr_bool Function\n" +
              "%load = OpLoad %bool %n\n" +
              "%3 = OpCopyObject %bool %true\n" +
              "%2 = OpLogicalOr %bool %3 %load\n" +
              "OpReturn\n" +
              "OpFunctionEnd",
          2, true, [](uint32_t id) {return (id == 3 ? TRUE_ID : id);})
  ));
// clang-format on

using GeneralInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<uint32_t>>;

TEST_P(GeneralInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);

  EXPECT_TRUE((inst == nullptr) == (tc.expected_result == 0));
  if (inst != nullptr) {
    EXPECT_EQ(inst->opcode(), spv::Op::OpCopyObject);
    EXPECT_EQ(inst->GetSingleWordInOperand(0), tc.expected_result);
  }
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(IntegerArithmeticTestCases, GeneralInstructionFoldingTest,
                        ::testing::Values(
    // Test case 0: Don't fold n * m
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%m = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%load_m = OpLoad %int %m\n" +
            "%2 = OpIMul %int %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 1: Don't fold n / m (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpUDiv %uint %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 2: Don't fold n / m (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%m = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%load_m = OpLoad %int %m\n" +
            "%2 = OpSDiv %int %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 3: Don't fold n remainder m
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%m = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%load_m = OpLoad %int %m\n" +
            "%2 = OpSRem %int %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 4: Don't fold n % m (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%m = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%load_m = OpLoad %int %m\n" +
            "%2 = OpSMod %int %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 5: Don't fold n % m (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpUMod %int %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 6: Don't fold n << m
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpShiftRightLogical %int %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 7: Don't fold n >> m
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpShiftLeftLogical %int %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 8: Don't fold n | m
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpBitwiseOr %int %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 9: Don't fold n & m
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpBitwiseAnd %int %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 10: Don't fold n < m (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpULessThan %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 11: Don't fold n > m (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpUGreaterThan %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 12: Don't fold n <= m (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpULessThanEqual %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 13: Don't fold n >= m (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%m = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%load_m = OpLoad %uint %m\n" +
            "%2 = OpUGreaterThanEqual %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 14: Don't fold n < m (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%m = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%load_m = OpLoad %int %m\n" +
            "%2 = OpULessThan %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 15: Don't fold n > m (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%m = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%load_m = OpLoad %int %m\n" +
            "%2 = OpUGreaterThan %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 16: Don't fold n <= m (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%m = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%load_m = OpLoad %int %m\n" +
            "%2 = OpULessThanEqual %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 17: Don't fold n >= m (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%m = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%load_m = OpLoad %int %m\n" +
            "%2 = OpUGreaterThanEqual %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 18: Don't fold n || m
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_bool Function\n" +
            "%m = OpVariable %_ptr_bool Function\n" +
            "%load_n = OpLoad %bool %n\n" +
            "%load_m = OpLoad %bool %m\n" +
            "%2 = OpLogicalOr %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 19: Don't fold n && m
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_bool Function\n" +
            "%m = OpVariable %_ptr_bool Function\n" +
            "%load_n = OpLoad %bool %n\n" +
            "%load_m = OpLoad %bool %m\n" +
            "%2 = OpLogicalAnd %bool %load_n %load_m\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 20: Don't fold n * 3
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%2 = OpIMul %int %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 21: Don't fold n / 3 (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpUDiv %uint %load_n %uint_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 22: Don't fold n / 3 (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%2 = OpSDiv %int %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 23: Don't fold n remainder 3
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%2 = OpSRem %int %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 24: Don't fold n % 3 (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%2 = OpSMod %int %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 25: Don't fold n % 3 (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpUMod %int %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 26: Don't fold n << 3
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpShiftRightLogical %int %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 27: Don't fold n >> 3
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpShiftLeftLogical %int %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 28: Don't fold n | 3
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpBitwiseOr %int %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 29: Don't fold n & 3
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpBitwiseAnd %uint %load_n %uint_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 30: Don't fold n < 3 (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpULessThan %bool %load_n %uint_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 31: Don't fold n > 3 (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpUGreaterThan %bool %load_n %uint_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 32: Don't fold n <= 3 (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpULessThanEqual %bool %load_n %uint_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 33: Don't fold n >= 3 (unsigned)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%load_n = OpLoad %uint %n\n" +
            "%2 = OpUGreaterThanEqual %bool %load_n %uint_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 34: Don't fold n < 3 (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%2 = OpULessThan %bool %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 35: Don't fold n > 3 (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%2 = OpUGreaterThan %bool %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 36: Don't fold n <= 3 (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%2 = OpULessThanEqual %bool %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 37: Don't fold n >= 3 (signed)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load_n = OpLoad %int %n\n" +
            "%2 = OpUGreaterThanEqual %bool %load_n %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 38: fold 1*n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%3 = OpLoad %int %n\n" +
            "%2 = OpIMul %int %int_1 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 39: fold n*1
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%3 = OpLoad %int %n\n" +
            "%2 = OpIMul %int %3 %int_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 40: Don't fold comparisons of 64-bit types
    // (https://github.com/KhronosGroup/SPIRV-Tools/issues/3343).
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%2 = OpSLessThan %bool %long_0 %long_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
        2, 0),
    // Test case 41: Don't fold OpSNegate for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpSNegate %int_coop_matrix %undef_int_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 42: Don't fold OpIAdd for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpIAdd %int_coop_matrix %undef_int_coop_matrix %undef_int_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 43: Don't fold OpISub for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpISub %int_coop_matrix %undef_int_coop_matrix %undef_int_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 44: Don't fold OpIMul for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpIMul %int_coop_matrix %undef_int_coop_matrix %undef_int_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 45: Don't fold OpSDiv for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpSDiv %int_coop_matrix %undef_int_coop_matrix %undef_int_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 46: Don't fold OpUDiv for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpUDiv %uint_coop_matrix %undef_uint_coop_matrix %undef_uint_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 47: Don't fold OpMatrixTimesScalar for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpMatrixTimesScalar %uint_coop_matrix %undef_uint_coop_matrix %uint_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0)
));

INSTANTIATE_TEST_SUITE_P(CompositeExtractFoldingTest, GeneralInstructionFoldingTest,
::testing::Values(
    // Test case 0: fold Insert feeding extract
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%2 = OpLoad %int %n\n" +
            "%3 = OpCompositeInsert %v4int %2 %v4int_0_0_0_0 0\n" +
            "%4 = OpCompositeInsert %v4int %int_1 %3 1\n" +
            "%5 = OpCompositeInsert %v4int %int_1 %4 2\n" +
            "%6 = OpCompositeInsert %v4int %int_1 %5 3\n" +
            "%7 = OpCompositeExtract %int %6 0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        7, 2),
    // Test case 1: fold Composite construct feeding extract (position 0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%2 = OpLoad %int %n\n" +
            "%3 = OpCompositeConstruct %v4int %2 %int_0 %int_0 %int_0\n" +
            "%4 = OpCompositeExtract %int %3 0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, 2),
    // Test case 2: fold Composite construct feeding extract (position 3)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%2 = OpLoad %int %n\n" +
            "%3 = OpCompositeConstruct %v4int %2 %int_0 %int_0 %100\n" +
            "%4 = OpCompositeExtract %int %3 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, INT_0_ID),
    // Test case 3: fold Composite construct with vectors feeding extract (scalar element)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%2 = OpLoad %int %n\n" +
            "%3 = OpCompositeConstruct %v2int %2 %int_0\n" +
            "%4 = OpCompositeConstruct %v4int %3 %int_0 %100\n" +
            "%5 = OpCompositeExtract %int %4 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, INT_0_ID),
    // Test case 4: fold Composite construct with vectors feeding extract (start of vector element)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%2 = OpLoad %int %n\n" +
            "%3 = OpCompositeConstruct %v2int %2 %int_0\n" +
            "%4 = OpCompositeConstruct %v4int %3 %int_0 %100\n" +
            "%5 = OpCompositeExtract %int %4 0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, 2),
    // Test case 5: fold Composite construct with vectors feeding extract (middle of vector element)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%2 = OpLoad %int %n\n" +
            "%3 = OpCompositeConstruct %v2int %int_0 %2\n" +
            "%4 = OpCompositeConstruct %v4int %3 %int_0 %100\n" +
            "%5 = OpCompositeExtract %int %4 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, 2),
    // Test case 6: fold Composite construct with multiple indices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%2 = OpLoad %int %n\n" +
            "%3 = OpCompositeConstruct %v2int %int_0 %2\n" +
            "%4 = OpCompositeConstruct %struct_v2int_int_int %3 %int_0 %100\n" +
            "%5 = OpCompositeExtract %int %4 0 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, 2),
    // Test case 7: fold constant extract.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeExtract %int %102 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, INT_7_ID),
    // Test case 8: constant struct has OpUndef
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeExtract %int %struct_undef_0_0 0 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 9: Extracting a member of element inserted via Insert
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_struct_v2int_int_int Function\n" +
            "%2 = OpLoad %struct_v2int_int_int %n\n" +
            "%3 = OpCompositeInsert %struct_v2int_int_int %102 %2 0\n" +
            "%4 = OpCompositeExtract %int %3 0 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, 103),
    // Test case 10: Extracting a element that is partially changed by Insert. (Don't fold)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_struct_v2int_int_int Function\n" +
            "%2 = OpLoad %struct_v2int_int_int %n\n" +
            "%3 = OpCompositeInsert %struct_v2int_int_int %int_0 %2 0 1\n" +
            "%4 = OpCompositeExtract %v2int %3 0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, 0),
    // Test case 11: Extracting from result of vector shuffle (first input)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v2int Function\n" +
            "%2 = OpLoad %v2int %n\n" +
            "%3 = OpVectorShuffle %v2int %102 %2 3 0\n" +
            "%4 = OpCompositeExtract %int %3 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, INT_7_ID),
    // Test case 12: Extracting from result of vector shuffle (second input)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v2int Function\n" +
            "%2 = OpLoad %v2int %n\n" +
            "%3 = OpVectorShuffle %v2int %2 %102 2 0\n" +
            "%4 = OpCompositeExtract %int %3 0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, INT_7_ID),
    // Test case 13: https://github.com/KhronosGroup/SPIRV-Tools/issues/2608
    // Out of bounds access.  Do not fold.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpConstantComposite %v4float %float_1 %float_1 %float_1 %float_1\n" +
            "%3 = OpCompositeExtract %float %2 4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, 0),
    // Test case 14: https://github.com/KhronosGroup/SPIRV-Tools/issues/3631
    // Extract the component right after the vector constituent.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeConstruct %v2int %int_0 %int_0\n" +
            "%3 = OpCompositeConstruct %v4int %2 %100 %int_0\n" +
            "%4 = OpCompositeExtract %int %3 2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, INT_0_ID),
    // Test case 15:
    // Don't fold extract fed by construct with vector result if the index is
    // past the last element.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeConstruct %v2int %int_0 %int_0\n" +
            "%3 = OpCompositeConstruct %v4int %2 %100 %int_0\n" +
            "%4 = OpCompositeExtract %int %3 4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, 0)
));

INSTANTIATE_TEST_SUITE_P(CompositeConstructFoldingTest, GeneralInstructionFoldingTest,
::testing::Values(
    // Test case 0: fold Extracts feeding construct
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCopyObject %v4int %v4int_0_0_0_0\n" +
            "%3 = OpCompositeExtract %int %2 0\n" +
            "%4 = OpCompositeExtract %int %2 1\n" +
            "%5 = OpCompositeExtract %int %2 2\n" +
            "%6 = OpCompositeExtract %int %2 3\n" +
            "%7 = OpCompositeConstruct %v4int %3 %4 %5 %6\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        7, 2),
    // Test case 1: Don't fold Extracts feeding construct (Different source)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCopyObject %v4int %v4int_0_0_0_0\n" +
            "%3 = OpCompositeExtract %int %2 0\n" +
            "%4 = OpCompositeExtract %int %2 1\n" +
            "%5 = OpCompositeExtract %int %2 2\n" +
            "%6 = OpCompositeExtract %int %v4int_0_0_0_0 3\n" +
            "%7 = OpCompositeConstruct %v4int %3 %4 %5 %6\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        7, 0),
    // Test case 2: Don't fold Extracts feeding construct (bad indices)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCopyObject %v4int %v4int_0_0_0_0\n" +
            "%3 = OpCompositeExtract %int %2 0\n" +
            "%4 = OpCompositeExtract %int %2 0\n" +
            "%5 = OpCompositeExtract %int %2 2\n" +
            "%6 = OpCompositeExtract %int %2 3\n" +
            "%7 = OpCompositeConstruct %v4int %3 %4 %5 %6\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        7, 0),
    // Test case 3: Don't fold Extracts feeding construct (different type)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCopyObject %struct_v2int_int_int %struct_v2int_int_int_null\n" +
            "%3 = OpCompositeExtract %v2int %2 0\n" +
            "%4 = OpCompositeExtract %int %2 1\n" +
            "%5 = OpCompositeExtract %int %2 2\n" +
            "%7 = OpCompositeConstruct %v4int %3 %4 %5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        7, 0),
    // Test case 4: Fold construct with constants to constant.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeConstruct %v2int %103 %103\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, VEC2_0_ID),
    // Test case 5: Don't segfault when trying to fold an OpCompositeConstruct
    // for an empty struct, and we reached the id limit.
    InstructionFoldingCase<uint32_t>(
        Header() + "%empty_struct = OpTypeStruct\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%4194303 = OpCompositeConstruct %empty_struct\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4194303, 0)
));

INSTANTIATE_TEST_SUITE_P(PhiFoldingTest, GeneralInstructionFoldingTest,
::testing::Values(
  // Test case 0: Fold phi with the same values for all edges.
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "            OpBranchConditional %true %l1 %l2\n" +
          "%l1 = OpLabel\n" +
          "      OpBranch %merge_lab\n" +
          "%l2 = OpLabel\n" +
          "      OpBranch %merge_lab\n" +
          "%merge_lab = OpLabel\n" +
          "%2 = OpPhi %int %100 %l1 %100 %l2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, INT_0_ID),
  // Test case 1: Fold phi in pass through loop.
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "            OpBranch %l1\n" +
          "%l1 = OpLabel\n" +
          "%2 = OpPhi %int %100 %main_lab %2 %l1\n" +
          "      OpBranchConditional %true %l1 %merge_lab\n" +
          "%merge_lab = OpLabel\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, INT_0_ID),
  // Test case 2: Don't Fold phi because of different values.
  InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "            OpBranch %l1\n" +
          "%l1 = OpLabel\n" +
          "%2 = OpPhi %int %int_0 %main_lab %int_3 %l1\n" +
          "      OpBranchConditional %true %l1 %merge_lab\n" +
          "%merge_lab = OpLabel\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0)
));

INSTANTIATE_TEST_SUITE_P(FloatRedundantFoldingTest, GeneralInstructionFoldingTest,
                        ::testing::Values(
    // Test case 0: Don't fold n + 1.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFAdd %float %3 %float_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 1: Don't fold n - 1.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFSub %float %3 %float_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 2: Don't fold n * 2.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFMul %float %3 %float_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 3: Fold n + 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFAdd %float %3 %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 4: Fold 0.0 + n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFAdd %float %float_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 5: Fold n - 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFSub %float %3 %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 6: Fold n * 1.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFMul %float %3 %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 7: Fold 1.0 * n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFMul %float %float_1 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 8: Fold n / 1.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFDiv %float %3 %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 9: Don't fold n % 1.0
    // If `n` is not a whole number, the answer is not 0.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFMod %float %3 %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 10: Fold n * 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFMul %float %3 %104\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, FLOAT_0_ID),
    // Test case 11: Fold 0.0 * n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFMul %float %104 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, FLOAT_0_ID),
    // Test case 12: Fold 0.0 / n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFDiv %float %104 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, FLOAT_0_ID),
    // Test case 13: Fold 0.0 % n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFMod %float %104 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, FLOAT_0_ID),
    // Test case 14: Don't fold mix(a, b, 2.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%a = OpVariable %_ptr_float Function\n" +
            "%b = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %a\n" +
            "%4 = OpLoad %float %b\n" +
            "%2 = OpExtInst %float %1 FMix %3 %4 %float_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 15: Fold mix(a, b, 0.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%a = OpVariable %_ptr_float Function\n" +
            "%b = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %a\n" +
            "%4 = OpLoad %float %b\n" +
            "%2 = OpExtInst %float %1 FMix %3 %4 %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 16: Fold mix(a, b, 1.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%a = OpVariable %_ptr_float Function\n" +
            "%b = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %a\n" +
            "%4 = OpLoad %float %b\n" +
            "%2 = OpExtInst %float %1 FMix %3 %4 %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 4),
    // Test case 17: Fold vector fadd with null
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%a = OpVariable %_ptr_v2float Function\n" +
            "%2 = OpLoad %v2float %a\n" +
            "%3 = OpFAdd %v2float %2 %v2float_null\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, 2),
    // Test case 18: Fold vector fadd with null
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%a = OpVariable %_ptr_v2float Function\n" +
            "%2 = OpLoad %v2float %a\n" +
            "%3 = OpFAdd %v2float %v2float_null %2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, 2),
    // Test case 19: Fold vector fsub with null
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%a = OpVariable %_ptr_v2float Function\n" +
            "%2 = OpLoad %v2float %a\n" +
            "%3 = OpFSub %v2float %2 %v2float_null\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, 2),
    // Test case 20: Fold 0.0(half) * n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_half Function\n" +
            "%3 = OpLoad %half %n\n" +
            "%2 = OpFMul %half %108 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, HALF_0_ID),
    // Test case 21: Don't fold 1.0(half) * n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_half Function\n" +
            "%3 = OpLoad %half %n\n" +
            "%2 = OpFMul %half %half_1 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 22: Don't fold 1.0 * 1.0 (half)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFMul %half %half_1 %half_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 23: Don't fold (0.0, 1.0) * (0.0, 1.0) (half)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFMul %v2half %half_0_1 %half_0_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 24: Don't fold (0.0, 1.0) dotp (0.0, 1.0) (half)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpDot %half %half_0_1 %half_0_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 25: Don't fold 1.0(half) / 2.0(half)
    // We do not have to code to emulate 16-bit float operations. Just make sure we do not crash.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_half Function\n" +
            "%3 = OpLoad %half %n\n" +
            "%2 = OpFDiv %half %half_1 %half_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 26: Don't fold OpFNegate for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFNegate %float_coop_matrix %undef_float_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 27: Don't fold OpIAdd for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFAdd %float_coop_matrix %undef_float_coop_matrix %undef_float_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 28: Don't fold OpISub for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFSub %float_coop_matrix %undef_float_coop_matrix %undef_float_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 29: Don't fold OpIMul for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFMul %float_coop_matrix %undef_float_coop_matrix %undef_float_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 30: Don't fold OpSDiv for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFDiv %float_coop_matrix %undef_float_coop_matrix %undef_float_coop_matrix\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 31: Don't fold OpMatrixTimesScalar for cooperative matrices.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpMatrixTimesScalar %float_coop_matrix %undef_float_coop_matrix %float_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0)
));

INSTANTIATE_TEST_SUITE_P(DoubleRedundantFoldingTest, GeneralInstructionFoldingTest,
                        ::testing::Values(
    // Test case 0: Don't fold n + 1.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFAdd %double %3 %double_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 1: Don't fold n - 1.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFSub %double %3 %double_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 2: Don't fold n * 2.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFMul %double %3 %double_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 3: Fold n + 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFAdd %double %3 %double_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 4: Fold 0.0 + n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFAdd %double %double_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 5: Fold n - 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFSub %double %3 %double_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 6: Fold n * 1.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFMul %double %3 %double_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 7: Fold 1.0 * n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFMul %double %double_1 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 8: Fold n / 1.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFDiv %double %3 %double_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 9: Fold n * 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFMul %double %3 %105\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, DOUBLE_0_ID),
    // Test case 10: Fold 0.0 * n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFMul %double %105 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, DOUBLE_0_ID),
    // Test case 11: Fold 0.0 / n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFDiv %double %105 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, DOUBLE_0_ID),
    // Test case 12: Don't fold mix(a, b, 2.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%a = OpVariable %_ptr_double Function\n" +
            "%b = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %a\n" +
            "%4 = OpLoad %double %b\n" +
            "%2 = OpExtInst %double %1 FMix %3 %4 %double_2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 13: Fold mix(a, b, 0.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%a = OpVariable %_ptr_double Function\n" +
            "%b = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %a\n" +
            "%4 = OpLoad %double %b\n" +
            "%2 = OpExtInst %double %1 FMix %3 %4 %double_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 14: Fold mix(a, b, 1.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%a = OpVariable %_ptr_double Function\n" +
            "%b = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %a\n" +
            "%4 = OpLoad %double %b\n" +
            "%2 = OpExtInst %double %1 FMix %3 %4 %double_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 4)
));

INSTANTIATE_TEST_SUITE_P(FloatVectorRedundantFoldingTest, GeneralInstructionFoldingTest,
                        ::testing::Values(
    // Test case 0: Don't fold a * vec4(0.0, 0.0, 0.0, 1.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4float Function\n" +
            "%3 = OpLoad %v4float %n\n" +
            "%2 = OpFMul %v4float %3 %v4float_0_0_0_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 1: Fold a * vec4(0.0, 0.0, 0.0, 0.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4float Function\n" +
            "%3 = OpLoad %v4float %n\n" +
            "%2 = OpFMul %v4float %3 %106\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, VEC4_0_ID),
    // Test case 2: Fold a * vec4(1.0, 1.0, 1.0, 1.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4float Function\n" +
            "%3 = OpLoad %v4float %n\n" +
            "%2 = OpFMul %v4float %3 %v4float_1_1_1_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3)
));

INSTANTIATE_TEST_SUITE_P(DoubleVectorRedundantFoldingTest, GeneralInstructionFoldingTest,
                        ::testing::Values(
    // Test case 0: Don't fold a * vec4(0.0, 0.0, 0.0, 1.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%2 = OpFMul %v4double %3 %v4double_0_0_0_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 1: Fold a * vec4(0.0, 0.0, 0.0, 0.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%2 = OpFMul %v4double %3 %106\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, DVEC4_0_ID),
    // Test case 2: Fold a + vec4(0.0, 0.0, 0.0, 0.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%2 = OpFAdd %v4double %3 %106\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 3: Fold a - vec4(0.0, 0.0, 0.0, 0.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%2 = OpFSub %v4double %3 %106\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 4: Fold a * vec4(1.0, 1.0, 1.0, 1.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%2 = OpFMul %v4double %3 %v4double_1_1_1_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3)
));

INSTANTIATE_TEST_SUITE_P(IntegerRedundantFoldingTest, GeneralInstructionFoldingTest,
                        ::testing::Values(
    // Test case 0: Don't fold n + 1
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpIAdd %uint %3 %uint_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 1: Don't fold 1 + n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpIAdd %uint %uint_1 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 2: Fold n | 0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpBitwiseOr %uint %3 %uint_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 3: Fold n ^ 0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpBitwiseXor %uint %3 %uint_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 4: Fold n >> 0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpShiftRightLogical %uint %3 %uint_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 5: Fold n >> 0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpShiftRightArithmetic %uint %3 %uint_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 6: Fold n << 0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpShiftLeftLogical %uint %3 %uint_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 7: Fold n + 0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpIAdd %uint %3 %uint_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 8: Fold n - 0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpISub %uint %3 %uint_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 9: Fold 0 | n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpBitwiseOr %uint %uint_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 10: Fold 0 ^ n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpBitwiseXor %uint %uint_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 11: Fold 0 + n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpIAdd %uint %uint_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 12: Don't fold n + (1,0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v2int Function\n" +
            "%3 = OpLoad %v2int %n\n" +
            "%2 = OpIAdd %v2int %3 %v2int_1_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 13: Don't fold (1,0) + n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v2int Function\n" +
            "%3 = OpLoad %v2int %n\n" +
            "%2 = OpIAdd %v2int %v2int_1_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 14: Fold n + (0,0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v2int Function\n" +
            "%3 = OpLoad %v2int %n\n" +
            "%2 = OpIAdd %v2int %3 %v2int_0_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 15: Fold (0,0) + n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v2int Function\n" +
            "%3 = OpLoad %v2int %n\n" +
            "%2 = OpIAdd %v2int %v2int_0_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 16: Fold n | (0,0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v2int Function\n" +
            "%3 = OpLoad %v2int %n\n" +
            "%2 = OpBitwiseOr %v2int %3 %v2int_0_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 17: Fold (0,0) | n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v2int Function\n" +
            "%3 = OpLoad %v2int %n\n" +
            "%2 = OpBitwiseOr %v2int %v2int_0_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 18: Fold 0 >> n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpShiftRightLogical %uint %109 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, UINT_0_ID),
    // Test case 19: Fold 0 >> n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpShiftRightArithmetic %uint %109 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, UINT_0_ID),
    // Test case 20: Fold 0 << n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpShiftLeftLogical %uint %109 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, UINT_0_ID),
    // Test case 21: Fold 0 / n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %int %n\n" +
            "%2 = OpSDiv %int %100 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, INT_0_ID),
    // Test case 22: Fold 0 / n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpUDiv %uint %109 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, UINT_0_ID),
    // Test case 23: Fold 0 % n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpSMod %int %int_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, INT_0_ID),
    // Test case 24: Fold 0 % n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpUMod %uint %109 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, UINT_0_ID),
    // Test case 25: Fold n % 1
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %int %n\n" +
            "%2 = OpSMod %int %3 %int_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, INT_NULL_ID),
    // Test case 26: Fold n % 1
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_uint Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpUMod %uint %3 %uint_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, UINT_NULL_ID),
    // Test case 27: Don't fold because of undefined value. Using 4294967295
    // means that entry is undefined. We do not expect it to ever happen, so
    // not worth folding.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load = OpLoad %int %n\n" +
            "%2 = OpVectorShuffle %v2int %v2int_null %v2int_2_3 4294967295 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 28: Don't fold because of undefined value. Using 4294967295
    // means that entry is undefined. We do not expect it to ever happen, so
    // not worth folding.
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load = OpLoad %int %n\n" +
            "%2 = OpVectorShuffle %v2int %v2int_null %v2int_2_3 0 4294967295 \n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0)
));

INSTANTIATE_TEST_SUITE_P(ClampAndCmpLHS, GeneralInstructionFoldingTest,
::testing::Values(
    // Test case 0: Don't Fold 0.0 < clamp(-1, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFUnordLessThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 1: Don't Fold 0.0 < clamp(-1, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFOrdLessThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 2: Don't Fold 0.0 <= clamp(-1, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFUnordLessThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 3: Don't Fold 0.0 <= clamp(-1, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFOrdLessThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 4: Don't Fold 0.0 > clamp(-1, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFUnordGreaterThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 5: Don't Fold 0.0 > clamp(-1, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFOrdGreaterThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 6: Don't Fold 0.0 >= clamp(-1, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFUnordGreaterThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 7: Don't Fold 0.0 >= clamp(-1, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFOrdGreaterThanEqual %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 8: Don't Fold 0.0 < clamp(0, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFUnordLessThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 9: Don't Fold 0.0 < clamp(0, 1)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFOrdLessThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 10: Don't Fold 0.0 > clamp(-1, 0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFUnordGreaterThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 11: Don't Fold 0.0 > clamp(-1, 0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFOrdGreaterThan %bool %float_0 %clamp\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0)
));

INSTANTIATE_TEST_SUITE_P(ClampAndCmpRHS, GeneralInstructionFoldingTest,
::testing::Values(
    // Test case 0: Don't Fold clamp(-1, 1) < 0.0
    InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_float Function\n" +
          "%ld = OpLoad %float %n\n" +
          "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
          "%2 = OpFUnordLessThan %bool %clamp %float_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
    // Test case 1: Don't Fold clamp(-1, 1) < 0.0
    InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_float Function\n" +
          "%ld = OpLoad %float %n\n" +
          "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
          "%2 = OpFOrdLessThan %bool %clamp %float_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
    // Test case 2: Don't Fold clamp(-1, 1) <= 0.0
    InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_float Function\n" +
          "%ld = OpLoad %float %n\n" +
          "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
          "%2 = OpFUnordLessThanEqual %bool %clamp %float_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
    // Test case 3: Don't Fold clamp(-1, 1) <= 0.0
    InstructionFoldingCase<uint32_t>(
      Header() + "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_float Function\n" +
          "%ld = OpLoad %float %n\n" +
          "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
          "%2 = OpFOrdLessThanEqual %bool %clamp %float_0\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, 0),
    // Test case 4: Don't Fold clamp(-1, 1) > 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFUnordGreaterThan %bool %clamp %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 5: Don't Fold clamp(-1, 1) > 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFOrdGreaterThan %bool %clamp %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 6: Don't Fold clamp(-1, 1) >= 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFUnordGreaterThanEqual %bool %clamp %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 7: Don't Fold clamp(-1, 1) >= 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_1\n" +
            "%2 = OpFOrdGreaterThanEqual %bool %clamp %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 8: Don't Fold clamp(-1, 0) < 0.0
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFUnordLessThan %bool %clamp %float_0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 9: Don't Fold clamp(0, 1) < 1
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_0 %float_1\n" +
            "%2 = OpFOrdLessThan %bool %clamp %float_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 10: Don't Fold clamp(-1, 0) > -1
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFUnordGreaterThan %bool %clamp %float_n1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 11: Don't Fold clamp(-1, 0) > -1
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%ld = OpLoad %float %n\n" +
            "%clamp = OpExtInst %float %1 FClamp %ld %float_n1 %float_0\n" +
            "%2 = OpFOrdGreaterThan %bool %clamp %float_n1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0)
));

INSTANTIATE_TEST_SUITE_P(FToIConstantFoldingTest, IntegerInstructionFoldingTest,
                        ::testing::Values(
    // Test case 0: Fold int(3.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpConvertFToS %int %float_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
    // Test case 1: Fold uint(3.0)
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpConvertFToU %int %float_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3)
));

INSTANTIATE_TEST_SUITE_P(IToFConstantFoldingTest, FloatInstructionFoldingTest,
                        ::testing::Values(
    // Test case 0: Fold float(3)
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpConvertSToF %float %int_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3.0),
    // Test case 1: Fold float(3u)
    InstructionFoldingCase<float>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpConvertUToF %float %uint_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3.0)
));
// clang-format on

using ToNegateFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<uint32_t>>;

TEST_P(ToNegateFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) =
      FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_1);

  EXPECT_TRUE((inst == nullptr) == (tc.expected_result == 0));
  if (inst != nullptr) {
    EXPECT_EQ(inst->opcode(), spv::Op::OpFNegate);
    EXPECT_EQ(inst->GetSingleWordInOperand(0), tc.expected_result);
  }
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(FloatRedundantSubFoldingTest, ToNegateFoldingTest,
                        ::testing::Values(
    // Test case 0: Don't fold 1.0 - n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFSub %float %float_1 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 1: Fold 0.0 - n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_float Function\n" +
            "%3 = OpLoad %float %n\n" +
            "%2 = OpFSub %float %float_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
	// Test case 2: Don't fold (0,0,0,1) - n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4float Function\n" +
            "%3 = OpLoad %v4float %n\n" +
            "%2 = OpFSub %v4float %v4float_0_0_0_1 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
	// Test case 3: Fold (0,0,0,0) - n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4float Function\n" +
            "%3 = OpLoad %v4float %n\n" +
            "%2 = OpFSub %v4float %v4float_0_0_0_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3)
));

INSTANTIATE_TEST_SUITE_P(DoubleRedundantSubFoldingTest, ToNegateFoldingTest,
                        ::testing::Values(
    // Test case 0: Don't fold 1.0 - n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFSub %double %double_1 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
    // Test case 1: Fold 0.0 - n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_double Function\n" +
            "%3 = OpLoad %double %n\n" +
            "%2 = OpFSub %double %double_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3),
	// Test case 2: Don't fold (0,0,0,1) - n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%2 = OpFSub %v4double %v4double_0_0_0_1 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 0),
	// Test case 3: Fold (0,0,0,0) - n
    InstructionFoldingCase<uint32_t>(
        Header() + "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%2 = OpFSub %v4double %v4double_0_0_0_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, 3)
));

using MatchingInstructionFoldingTest =
    ::testing::TestWithParam<InstructionFoldingCase<bool>>;

TEST_P(MatchingInstructionFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) = FoldInstruction(tc.test_body, tc.id_to_fold,SPV_ENV_UNIVERSAL_1_1);

  EXPECT_EQ(inst != nullptr, tc.expected_result);
  if (inst != nullptr) {
    Match(tc.test_body, context.get());
  }
}

INSTANTIATE_TEST_SUITE_P(RedundantIntegerMatching, MatchingInstructionFoldingTest,
::testing::Values(
    // Test case 0: Fold 0 + n (change sign)
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[uint:%\\w+]] = OpTypeInt 32 0\n" +
            "; CHECK: %2 = OpBitcast [[uint]] %3\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%3 = OpLoad %uint %n\n" +
            "%2 = OpIAdd %uint %int_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd\n",
        2, true),
    // Test case 0: Fold 0 + n (change sign)
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK: %2 = OpBitcast [[int]] %3\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%3 = OpLoad %int %n\n" +
            "%2 = OpIAdd %int %uint_0 %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd\n",
        2, true)
));

INSTANTIATE_TEST_SUITE_P(MergeNegateTest, MatchingInstructionFoldingTest,
::testing::Values(
  // Test case 0: fold consecutive fnegate
  // -(-x) = x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float:%\\w+]]\n" +
      "; CHECK: %4 = OpCopyObject [[float]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFNegate %float %2\n" +
      "%4 = OpFNegate %float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 1: fold fnegate(fmul with const).
  // -(x * 2.0) = x * -2.0
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_n2:%\\w+]] = OpConstant [[float]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFMul [[float]] [[ld]] [[float_n2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFMul %float %2 %float_2\n" +
      "%4 = OpFNegate %float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 2: fold fnegate(fmul with const).
  // -(2.0 * x) = x * 2.0
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_n2:%\\w+]] = OpConstant [[float]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFMul [[float]] [[ld]] [[float_n2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFMul %float %float_2 %2\n" +
      "%4 = OpFNegate %float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 3: fold fnegate(fdiv with const).
  // -(x / 2.0) = x * -0.5
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_n0p5:%\\w+]] = OpConstant [[float]] -0.5\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFMul [[float]] [[ld]] [[float_n0p5]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %2 %float_2\n" +
      "%4 = OpFNegate %float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 4: fold fnegate(fdiv with const).
  // -(2.0 / x) = -2.0 / x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_n2:%\\w+]] = OpConstant [[float]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFDiv [[float]] [[float_n2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %float_2 %2\n" +
      "%4 = OpFNegate %float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 5: fold fnegate(fadd with const).
  // -(2.0 + x) = -2.0 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_n2:%\\w+]] = OpConstant [[float]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_n2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %float_2 %2\n" +
      "%4 = OpFNegate %float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 6: fold fnegate(fadd with const).
  // -(x + 2.0) = -2.0 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_n2:%\\w+]] = OpConstant [[float]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_n2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %2 %float_2\n" +
      "%4 = OpFNegate %float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 7: fold fnegate(fsub with const).
  // -(2.0 - x) = x - 2.0
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[ld]] [[float_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %float_2 %2\n" +
      "%4 = OpFNegate %float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 8: fold fnegate(fsub with const).
  // -(x - 2.0) = 2.0 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %2 %float_2\n" +
      "%4 = OpFNegate %float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 9: fold consecutive snegate
  // -(-x) = x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int:%\\w+]]\n" +
      "; CHECK: %4 = OpCopyObject [[int]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpSNegate %int %2\n" +
      "%4 = OpSNegate %int %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 10: fold consecutive vector negate
  // -(-x) = x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[v2float:%\\w+]]\n" +
      "; CHECK: %4 = OpCopyObject [[v2float]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_v2float Function\n" +
      "%2 = OpLoad %v2float %var\n" +
      "%3 = OpFNegate %v2float %2\n" +
      "%4 = OpFNegate %v2float %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 11: fold snegate(iadd with const).
  // -(2 + x) = -2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
      "; CHECK: OpConstant [[int]] -2147483648\n" +
      "; CHECK: [[int_n2:%\\w+]] = OpConstant [[int]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpISub [[int]] [[int_n2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIAdd %int %int_2 %2\n" +
      "%4 = OpSNegate %int %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 12: fold snegate(iadd with const).
  // -(x + 2) = -2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
      "; CHECK: OpConstant [[int]] -2147483648\n" +
      "; CHECK: [[int_n2:%\\w+]] = OpConstant [[int]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpISub [[int]] [[int_n2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIAdd %int %2 %int_2\n" +
      "%4 = OpSNegate %int %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 13: fold snegate(isub with const).
  // -(2 - x) = x - 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
      "; CHECK: [[int_2:%\\w+]] = OpConstant [[int]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpISub [[int]] [[ld]] [[int_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpISub %int %int_2 %2\n" +
      "%4 = OpSNegate %int %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 14: fold snegate(isub with const).
  // -(x - 2) = 2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
      "; CHECK: [[int_2:%\\w+]] = OpConstant [[int]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpISub [[int]] [[int_2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpISub %int %2 %int_2\n" +
      "%4 = OpSNegate %int %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 15: fold snegate(iadd with const).
  // -(x + 2) = -2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64 1\n" +
      "; CHECK: [[long_n2:%\\w+]] = OpConstant [[long]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpISub [[long]] [[long_n2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpIAdd %long %2 %long_2\n" +
      "%4 = OpSNegate %long %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 16: fold snegate(isub with const).
  // -(2 - x) = x - 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64 1\n" +
      "; CHECK: [[long_2:%\\w+]] = OpConstant [[long]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpISub [[long]] [[ld]] [[long_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpISub %long %long_2 %2\n" +
      "%4 = OpSNegate %long %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
  // Test case 17: fold snegate(isub with const).
  // -(x - 2) = 2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64 1\n" +
      "; CHECK: [[long_2:%\\w+]] = OpConstant [[long]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpISub [[long]] [[long_2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpISub %long %2 %long_2\n" +
      "%4 = OpSNegate %long %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    4, true),
    // Test case 18: fold -vec4(-1.0, 2.0, 1.0, 3.0)
    InstructionFoldingCase<bool>(
        Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[v4float:%\\w+]] = OpTypeVector [[float]] 4{{[[:space:]]}}\n" +
      "; CHECK: [[float_n1:%\\w+]] = OpConstant [[float]] -1{{[[:space:]]}}\n" +
      "; CHECK: [[float_1:%\\w+]] = OpConstant [[float]] 1{{[[:space:]]}}\n" +
      "; CHECK: [[float_n2:%\\w+]] = OpConstant [[float]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[float_n3:%\\w+]] = OpConstant [[float]] -3{{[[:space:]]}}\n" +
      "; CHECK: [[v4float_1_n2_n1_n3:%\\w+]] = OpConstantComposite [[v4float]] [[float_1]] [[float_n2]] [[float_n1]] [[float_n3]]\n" +
      "; CHECK: %2 = OpCopyObject [[v4float]] [[v4float_1_n2_n1_n3]]\n" +
        "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFNegate %v4float %v4float_n1_2_1_3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 19: fold vector fnegate with null
    InstructionFoldingCase<bool>(
        Header() +
      "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
      "; CHECK: [[v2double:%\\w+]] = OpTypeVector [[double]] 2\n" +
      "; CHECK: [[double_n0:%\\w+]] = OpConstant [[double]] -0\n" +
      "; CHECK: [[v2double_0_0:%\\w+]] = OpConstantComposite [[v2double]] [[double_n0]] [[double_n0]]\n" +
      "; CHECK: %2 = OpCopyObject [[v2double]] [[v2double_0_0]]\n" +
        "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpFNegate %v2double %v2double_null\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        2, true),
    // Test case 20: fold snegate with OpIMul.
    // -(x * 2) = x * -2
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[long:%\\w+]] = OpTypeInt 64 1\n" +
          "; CHECK: [[long_n2:%\\w+]] = OpConstant [[long]] -2\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
          "; CHECK: %4 = OpIMul [[long]] [[ld]] [[long_n2]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_long Function\n" +
          "%2 = OpLoad %long %var\n" +
          "%3 = OpIMul %long %2 %long_2\n" +
          "%4 = OpSNegate %long %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 21: fold snegate with OpIMul.
    // -(x * 2) = x * -2
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK-DAG: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK-DAG: [[uint:%\\w+]] = OpTypeInt 32 0\n" +
          "; CHECK: [[uint_n2:%\\w+]] = OpConstant [[uint]] 4294967294\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
          "; CHECK: %4 = OpIMul [[int]] [[ld]] [[uint_n2]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_int Function\n" +
          "%2 = OpLoad %int %var\n" +
          "%3 = OpIMul %int %2 %uint_2\n" +
          "%4 = OpSNegate %int %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 22: fold snegate with OpIMul.
    // -(-24 * x) = x * 24
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK-DAG: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[int_24:%\\w+]] = OpConstant [[int]] 24\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
          "; CHECK: %4 = OpIMul [[int]] [[ld]] [[int_24]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_int Function\n" +
          "%2 = OpLoad %int %var\n" +
          "%3 = OpIMul %int %int_n24 %2\n" +
          "%4 = OpSNegate %int %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 23: fold snegate with OpIMul with UINT_MAX
    // -(UINT_MAX * x) = x
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
          "; CHECK: %4 = OpCopyObject [[int]] [[ld]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_int Function\n" +
          "%2 = OpLoad %int %var\n" +
          "%3 = OpIMul %int %uint_max %2\n" +
          "%4 = OpSNegate %int %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 24: fold snegate with OpIMul using -INT_MAX
    // -(x * 2147483649u) = x * 2147483647u
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[uint:%\\w+]] = OpTypeInt 32 0\n" +
          "; CHECK: [[uint_2147483647:%\\w+]] = OpConstant [[uint]] 2147483647\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
          "; CHECK: %4 = OpIMul [[int]] [[ld]] [[uint_2147483647]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_int Function\n" +
          "%2 = OpLoad %int %var\n" +
          "%3 = OpIMul %int %2 %uint_2147483649\n" +
          "%4 = OpSNegate %int %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 25: fold snegate with OpSDiv (long).
    // -(x / 2) = x / -2
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[long:%\\w+]] = OpTypeInt 64 1\n" +
          "; CHECK: [[long_n2:%\\w+]] = OpConstant [[long]] -2\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
          "; CHECK: %4 = OpSDiv [[long]] [[ld]] [[long_n2]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_long Function\n" +
          "%2 = OpLoad %long %var\n" +
          "%3 = OpSDiv %long %2 %long_2\n" +
          "%4 = OpSNegate %long %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 26: fold snegate with OpSDiv (int).
    // -(x / 2) = x / -2
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK-DAG: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK-DAG: [[uint:%\\w+]] = OpTypeInt 32 0\n" +
          "; CHECK: [[uint_n2:%\\w+]] = OpConstant [[uint]] 4294967294\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
          "; CHECK: %4 = OpSDiv [[int]] [[ld]] [[uint_n2]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_int Function\n" +
          "%2 = OpLoad %int %var\n" +
          "%3 = OpSDiv %int %2 %uint_2\n" +
          "%4 = OpSNegate %int %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 27: fold snegate with OpSDiv.
    // -(-24 / x) = 24 / x
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK-DAG: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[int_24:%\\w+]] = OpConstant [[int]] 24\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
          "; CHECK: %4 = OpSDiv [[int]] [[int_24]] [[ld]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_int Function\n" +
          "%2 = OpLoad %int %var\n" +
          "%3 = OpSDiv %int %int_n24 %2\n" +
          "%4 = OpSNegate %int %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 28: fold snegate with OpSDiv with UINT_MAX
    // -(UINT_MAX / x) = (1 / x)
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[uint:%\\w+]] = OpTypeInt 32 0\n" +
          "; CHECK: [[uint_1:%\\w+]] = OpConstant [[uint]] 1\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
          "; CHECK: %4 = OpSDiv [[int]] [[uint_1]] [[ld]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_int Function\n" +
          "%2 = OpLoad %int %var\n" +
          "%3 = OpSDiv %int %uint_max %2\n" +
          "%4 = OpSNegate %int %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 29: fold snegate with OpSDiv using -INT_MAX
    // -(x / 2147483647u) = x / 2147483647
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[uint:%\\w+]] = OpTypeInt 32 0\n" +
          "; CHECK: [[uint_2147483647:%\\w+]] = OpConstant [[uint]] 2147483647\n" +
          "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
          "; CHECK: %4 = OpSDiv [[int]] [[ld]] [[uint_2147483647]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_int Function\n" +
          "%2 = OpLoad %int %var\n" +
          "%3 = OpSDiv %int %2 %uint_2147483649\n" +
          "%4 = OpSNegate %int %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
    // Test case 30: Don't fold snegate int OpUDiv. The operands are interpreted
    // as unsigned, so negating an operand is not the same a negating the
    // result.
  InstructionFoldingCase<bool>(
      Header() +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%var = OpVariable %_ptr_int Function\n" +
          "%2 = OpLoad %int %var\n" +
          "%3 = OpUDiv %int %2 %uint_1\n" +
          "%4 = OpSNegate %int %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, false)
));

INSTANTIATE_TEST_SUITE_P(ReciprocalFDivTest, MatchingInstructionFoldingTest,
::testing::Values(
  // Test case 0: scalar reicprocal
  // x / 0.5 = x * 2.0
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %3 = OpFMul [[float]] [[ld]] [[float_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %2 %float_0p5\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    3, true),
  // Test case 1: Unfoldable
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_0:%\\w+]] = OpConstant [[float]] 0\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %3 = OpFDiv [[float]] [[ld]] [[float_0]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %2 %104\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    3, false),
  // Test case 2: Vector reciprocal
  // x / {2.0, 0.5} = x * {0.5, 2.0}
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[v2float:%\\w+]] = OpTypeVector [[float]] 2\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[float_0p5:%\\w+]] = OpConstant [[float]] 0.5\n" +
      "; CHECK: [[v2float_0p5_2:%\\w+]] = OpConstantComposite [[v2float]] [[float_0p5]] [[float_2]]\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[v2float]]\n" +
      "; CHECK: %3 = OpFMul [[v2float]] [[ld]] [[v2float_0p5_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_v2float Function\n" +
      "%2 = OpLoad %v2float %var\n" +
      "%3 = OpFDiv %v2float %2 %v2float_2_0p5\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    3, true),
  // Test case 3: double reciprocal
  // x / 2.0 = x * 0.5
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
      "; CHECK: [[double_0p5:%\\w+]] = OpConstant [[double]] 0.5\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[double]]\n" +
      "; CHECK: %3 = OpFMul [[double]] [[ld]] [[double_0p5]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_double Function\n" +
      "%2 = OpLoad %double %var\n" +
      "%3 = OpFDiv %double %2 %double_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    3, true),
  // Test case 4: don't fold x / 0.
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_v2float Function\n" +
      "%2 = OpLoad %v2float %var\n" +
      "%3 = OpFDiv %v2float %2 %v2float_null\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    3, false)
));

INSTANTIATE_TEST_SUITE_P(MergeMulTest, MatchingInstructionFoldingTest,
::testing::Values(
  // Test case 0: fold consecutive fmuls
  // (x * 3.0) * 2.0 = x * 6.0
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_6:%\\w+]] = OpConstant [[float]] 6\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFMul [[float]] [[ld]] [[float_6]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFMul %float %2 %float_3\n" +
      "%4 = OpFMul %float %3 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 1: fold consecutive fmuls
  // 2.0 * (x * 3.0) = x * 6.0
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_6:%\\w+]] = OpConstant [[float]] 6\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFMul [[float]] [[ld]] [[float_6]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFMul %float %2 %float_3\n" +
      "%4 = OpFMul %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 2: fold consecutive fmuls
  // (3.0 * x) * 2.0 = x * 6.0
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_6:%\\w+]] = OpConstant [[float]] 6\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFMul [[float]] [[ld]] [[float_6]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFMul %float %float_3 %2\n" +
      "%4 = OpFMul %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 3: fold vector fmul
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[v2float:%\\w+]] = OpTypeVector [[float]] 2\n" +
      "; CHECK: [[float_6:%\\w+]] = OpConstant [[float]] 6\n" +
      "; CHECK: [[v2float_6_6:%\\w+]] = OpConstantComposite [[v2float]] [[float_6]] [[float_6]]\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[v2float]]\n" +
      "; CHECK: %4 = OpFMul [[v2float]] [[ld]] [[v2float_6_6]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_v2float Function\n" +
      "%2 = OpLoad %v2float %var\n" +
      "%3 = OpFMul %v2float %2 %v2float_2_3\n" +
      "%4 = OpFMul %v2float %3 %v2float_3_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 4: fold double fmuls
  // (x * 3.0) * 2.0 = x * 6.0
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
      "; CHECK: [[double_6:%\\w+]] = OpConstant [[double]] 6\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[double]]\n" +
      "; CHECK: %4 = OpFMul [[double]] [[ld]] [[double_6]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_double Function\n" +
      "%2 = OpLoad %double %var\n" +
      "%3 = OpFMul %double %2 %double_3\n" +
      "%4 = OpFMul %double %3 %double_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 5: fold 32 bit imuls
  // (x * 3) * 2 = x * 6
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
      "; CHECK: [[int_6:%\\w+]] = OpConstant [[int]] 6\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpIMul [[int]] [[ld]] [[int_6]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIMul %int %2 %int_3\n" +
      "%4 = OpIMul %int %3 %int_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 6: fold 64 bit imuls
  // (x * 3) * 2 = x * 6
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64\n" +
      "; CHECK: [[long_6:%\\w+]] = OpConstant [[long]] 6\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpIMul [[long]] [[ld]] [[long_6]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpIMul %long %2 %long_3\n" +
      "%4 = OpIMul %long %3 %long_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 7: merge vector integer mults
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
      "; CHECK: [[v2int:%\\w+]] = OpTypeVector [[int]] 2\n" +
      "; CHECK: [[int_6:%\\w+]] = OpConstant [[int]] 6\n" +
      "; CHECK: [[v2int_6_6:%\\w+]] = OpConstantComposite [[v2int]] [[int_6]] [[int_6]]\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[v2int]]\n" +
      "; CHECK: %4 = OpIMul [[v2int]] [[ld]] [[v2int_6_6]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_v2int Function\n" +
      "%2 = OpLoad %v2int %var\n" +
      "%3 = OpIMul %v2int %2 %v2int_2_3\n" +
      "%4 = OpIMul %v2int %3 %v2int_3_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 8: merge fmul of fdiv
  // 2.0 * (2.0 / x) = 4.0 / x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_4:%\\w+]] = OpConstant [[float]] 4\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFDiv [[float]] [[float_4]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %float_2 %2\n" +
      "%4 = OpFMul %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 9: merge fmul of fdiv
  // (2.0 / x) * 2.0 = 4.0 / x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_4:%\\w+]] = OpConstant [[float]] 4\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFDiv [[float]] [[float_4]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %float_2 %2\n" +
      "%4 = OpFMul %float %3 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 10: Do not merge imul of sdiv
  // 4 * (x / 2)
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpSDiv %int %2 %int_2\n" +
      "%4 = OpIMul %int %int_4 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 11: Do not merge imul of sdiv
  // (x / 2) * 4
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpSDiv %int %2 %int_2\n" +
      "%4 = OpIMul %int %3 %int_4\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 12: Do not merge imul of udiv
  // 4 * (x / 2)
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_uint Function\n" +
      "%2 = OpLoad %uint %var\n" +
      "%3 = OpUDiv %uint %2 %uint_2\n" +
      "%4 = OpIMul %uint %uint_4 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 13: Do not merge imul of udiv
  // (x / 2) * 4
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_uint Function\n" +
      "%2 = OpLoad %uint %var\n" +
      "%3 = OpUDiv %uint %2 %uint_2\n" +
      "%4 = OpIMul %uint %3 %uint_4\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 14: Don't fold
  // (x / 3) * 4
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_uint Function\n" +
      "%2 = OpLoad %uint %var\n" +
      "%3 = OpUDiv %uint %2 %uint_3\n" +
      "%4 = OpIMul %uint %3 %uint_4\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 15: merge vector fmul of fdiv
  // (x / {2,2}) * {4,4} = x * {2,2}
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[v2float:%\\w+]] = OpTypeVector [[float]] 2\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[v2float_2_2:%\\w+]] = OpConstantComposite [[v2float]] [[float_2]] [[float_2]]\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[v2float]]\n" +
      "; CHECK: %4 = OpFMul [[v2float]] [[ld]] [[v2float_2_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_v2float Function\n" +
      "%2 = OpLoad %v2float %var\n" +
      "%3 = OpFDiv %v2float %2 %v2float_2_2\n" +
      "%4 = OpFMul %v2float %3 %v2float_4_4\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 16: merge vector imul of snegate
  // (-x) * {2,2} = x * {-2,-2}
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
      "; CHECK: [[v2int:%\\w+]] = OpTypeVector [[int]] 2{{[[:space:]]}}\n" +
      "; CHECK: OpConstant [[int]] -2147483648{{[[:space:]]}}\n" +
      "; CHECK: [[int_n2:%\\w+]] = OpConstant [[int]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[v2int_n2_n2:%\\w+]] = OpConstantComposite [[v2int]] [[int_n2]] [[int_n2]]\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[v2int]]\n" +
      "; CHECK: %4 = OpIMul [[v2int]] [[ld]] [[v2int_n2_n2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_v2int Function\n" +
      "%2 = OpLoad %v2int %var\n" +
      "%3 = OpSNegate %v2int %2\n" +
      "%4 = OpIMul %v2int %3 %v2int_2_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 17: merge vector imul of snegate
  // {2,2} * (-x) = x * {-2,-2}
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
      "; CHECK: [[v2int:%\\w+]] = OpTypeVector [[int]] 2{{[[:space:]]}}\n" +
      "; CHECK: OpConstant [[int]] -2147483648{{[[:space:]]}}\n" +
      "; CHECK: [[int_n2:%\\w+]] = OpConstant [[int]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[v2int_n2_n2:%\\w+]] = OpConstantComposite [[v2int]] [[int_n2]] [[int_n2]]\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[v2int]]\n" +
      "; CHECK: %4 = OpIMul [[v2int]] [[ld]] [[v2int_n2_n2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_v2int Function\n" +
      "%2 = OpLoad %v2int %var\n" +
      "%3 = OpSNegate %v2int %2\n" +
      "%4 = OpIMul %v2int %v2int_2_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 18: Fold OpVectorTimesScalar
  // {4,4} = OpVectorTimesScalar v2float {2,2} 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[v2float:%\\w+]] = OpTypeVector [[float]] 2\n" +
      "; CHECK: [[float_4:%\\w+]] = OpConstant [[float]] 4\n" +
      "; CHECK: [[v2float_4_4:%\\w+]] = OpConstantComposite [[v2float]] [[float_4]] [[float_4]]\n" +
      "; CHECK: %2 = OpCopyObject [[v2float]] [[v2float_4_4]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%2 = OpVectorTimesScalar %v2float %v2float_2_2 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    2, true),
  // Test case 19: Fold OpVectorTimesScalar
  // {0,0} = OpVectorTimesScalar v2float v2float_null -1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[v2float:%\\w+]] = OpTypeVector [[float]] 2\n" +
      "; CHECK: [[v2float_null:%\\w+]] = OpConstantNull [[v2float]]\n" +
      "; CHECK: %2 = OpCopyObject [[v2float]] [[v2float_null]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%2 = OpVectorTimesScalar %v2float %v2float_null %float_n1\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    2, true),
  // Test case 20: Fold OpVectorTimesScalar
  // {4,4} = OpVectorTimesScalar v2double {2,2} 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
      "; CHECK: [[v2double:%\\w+]] = OpTypeVector [[double]] 2\n" +
      "; CHECK: [[double_4:%\\w+]] = OpConstant [[double]] 4\n" +
      "; CHECK: [[v2double_4_4:%\\w+]] = OpConstantComposite [[v2double]] [[double_4]] [[double_4]]\n" +
      "; CHECK: %2 = OpCopyObject [[v2double]] [[v2double_4_4]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%2 = OpVectorTimesScalar %v2double %v2double_2_2 %double_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd",
    2, true),
  // Test case 21: Fold OpVectorTimesScalar
  // {0,0} = OpVectorTimesScalar v2double {0,0} n
  InstructionFoldingCase<bool>(
    Header() +
        "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
        "; CHECK: [[v2double:%\\w+]] = OpTypeVector [[double]] 2\n" +
        "; CHECK: {{%\\w+}} = OpConstant [[double]] 0\n" +
        "; CHECK: [[double_0:%\\w+]] = OpConstant [[double]] 0\n" +
        "; CHECK: [[v2double_0_0:%\\w+]] = OpConstantComposite [[v2double]] [[double_0]] [[double_0]]\n" +
        "; CHECK: %2 = OpCopyObject [[v2double]] [[v2double_0_0]]\n" +
        "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_double Function\n" +
        "%load = OpLoad %double %n\n" +
        "%2 = OpVectorTimesScalar %v2double %v2double_0_0 %load\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, true),
  // Test case 22: Fold OpVectorTimesScalar
  // {0,0} = OpVectorTimesScalar v2double n 0
  InstructionFoldingCase<bool>(
    Header() +
        "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
        "; CHECK: [[v2double:%\\w+]] = OpTypeVector [[double]] 2\n" +
        "; CHECK: [[v2double_null:%\\w+]] = OpConstantNull [[v2double]]\n" +
        "; CHECK: %2 = OpCopyObject [[v2double]] [[v2double_null]]\n" +
        "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%n = OpVariable %_ptr_v2double Function\n" +
        "%load = OpLoad %v2double %n\n" +
        "%2 = OpVectorTimesScalar %v2double %load %double_0\n" +
        "OpReturn\n" +
        "OpFunctionEnd",
    2, true),
  // Test case 23: merge fmul of fdiv
  // x * (y / x) = y
  InstructionFoldingCase<bool>(
    Header() +
        "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
        "; CHECK: [[ldx:%\\w+]] = OpLoad [[float]]\n" +
        "; CHECK: [[ldy:%\\w+]] = OpLoad [[float]] [[y:%\\w+]]\n" +
        "; CHECK: %5 = OpCopyObject [[float]] [[ldy]]\n" +
        "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%x = OpVariable %_ptr_float Function\n" +
        "%y = OpVariable %_ptr_float Function\n" +
        "%2 = OpLoad %float %x\n" +
        "%3 = OpLoad %float %y\n" +
        "%4 = OpFDiv %float %3 %2\n" +
        "%5 = OpFMul %float %2 %4\n" +
        "OpReturn\n" +
        "OpFunctionEnd\n",
    5, true),
  // Test case 24: merge fmul of fdiv
  // (y / x) * x = y
  InstructionFoldingCase<bool>(
    Header() +
        "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
        "; CHECK: [[ldx:%\\w+]] = OpLoad [[float]]\n" +
        "; CHECK: [[ldy:%\\w+]] = OpLoad [[float]] [[y:%\\w+]]\n" +
        "; CHECK: %5 = OpCopyObject [[float]] [[ldy]]\n" +
        "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%x = OpVariable %_ptr_float Function\n" +
        "%y = OpVariable %_ptr_float Function\n" +
        "%2 = OpLoad %float %x\n" +
        "%3 = OpLoad %float %y\n" +
        "%4 = OpFDiv %float %3 %2\n" +
        "%5 = OpFMul %float %4 %2\n" +
        "OpReturn\n" +
        "OpFunctionEnd\n",
    5, true),
  // Test case 25: fold overflowing signed 32 bit imuls
  // (x * 1073741824) * 2 = x * int_min
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32\n" +
      "; CHECK: [[int_min:%\\w+]] = OpConstant [[int]] -2147483648\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpIMul [[int]] [[ld]] [[int_min]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIMul %int %2 %int_1073741824\n" +
      "%4 = OpIMul %int %3 %int_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 26: fold overflowing signed 64 bit imuls
  // (x * 4611686018427387904) * 2 = x * long_min
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64\n" +
      "; CHECK: [[long_min:%\\w+]] = OpConstant [[long]] -9223372036854775808\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpIMul [[long]] [[ld]] [[long_min]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpIMul %long %2 %long_4611686018427387904\n" +
      "%4 = OpIMul %long %3 %long_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 27: fold overflowing 32 bit unsigned imuls
  // (x * 2147483649) * 2 = x * 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[uint:%\\w+]] = OpTypeInt 32 0\n" +
      "; CHECK: [[uint_2:%\\w+]] = OpConstant [[uint]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[uint]]\n" +
      "; CHECK: %4 = OpIMul [[uint]] [[ld]] [[uint_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_uint Function\n" +
      "%2 = OpLoad %uint %var\n" +
      "%3 = OpIMul %uint %2 %uint_2147483649\n" +
      "%4 = OpIMul %uint %3 %uint_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 28: fold overflowing 64 bit unsigned imuls
  // (x * 9223372036854775809) * 2 = x * 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[ulong:%\\w+]] = OpTypeInt 64 0\n" +
      "; CHECK: [[ulong_2:%\\w+]] = OpConstant [[ulong]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[ulong]]\n" +
      "; CHECK: %4 = OpIMul [[ulong]] [[ld]] [[ulong_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_ulong Function\n" +
      "%2 = OpLoad %ulong %var\n" +
      "%3 = OpIMul %ulong %2 %ulong_9223372036854775809\n" +
      "%4 = OpIMul %ulong %3 %ulong_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 29: fold underflowing signed 32 bit imuls
  // (x * (-858993459)) * 10 = x * 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32\n" +
      "; CHECK: [[int_2:%\\w+]] = OpConstant [[int]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpIMul [[int]] [[ld]] [[int_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIMul %int %2 %int_n858993459\n" +
      "%4 = OpIMul %int %3 %int_10\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 30: fold underflowing signed 64 bit imuls
  // (x * (-3689348814741910323)) * 10 = x * 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64\n" +
      "; CHECK: [[long_2:%\\w+]] = OpConstant [[long]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpIMul [[long]] [[ld]] [[long_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpIMul %long %2 %long_n3689348814741910323\n" +
      "%4 = OpIMul %long %3 %long_10\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true)
));

INSTANTIATE_TEST_SUITE_P(MergeDivTest, MatchingInstructionFoldingTest,
::testing::Values(
  // Test case 0: merge consecutive fdiv
  // 4.0 / (2.0 / x) = 2.0 * x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFMul [[float]] [[float_2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %float_2 %2\n" +
      "%4 = OpFDiv %float %float_4 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 1: merge consecutive fdiv
  // 4.0 / (x / 2.0) = 8.0 / x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_8:%\\w+]] = OpConstant [[float]] 8\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFDiv [[float]] [[float_8]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %2 %float_2\n" +
      "%4 = OpFDiv %float %float_4 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 2: merge consecutive fdiv
  // (4.0 / x) / 2.0 = 2.0 / x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFDiv [[float]] [[float_2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %float_4 %2\n" +
      "%4 = OpFDiv %float %3 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 3: Do not merge consecutive sdiv
  // 4 / (2 / x)
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpSDiv %int %int_2 %2\n" +
      "%4 = OpSDiv %int %int_4 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 4: Do not merge consecutive sdiv
  // 4 / (x / 2)
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpSDiv %int %2 %int_2\n" +
      "%4 = OpSDiv %int %int_4 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 5: Do not merge consecutive sdiv
  // (4 / x) / 2
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpSDiv %int %int_4 %2\n" +
      "%4 = OpSDiv %int %3 %int_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 6: Do not merge consecutive sdiv
  // (x / 4) / 2
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpSDiv %int %2 %int_4\n" +
      "%4 = OpSDiv %int %3 %int_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 7: Do not merge sdiv of imul
  // 4 / (2 * x)
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIMul %int %int_2 %2\n" +
      "%4 = OpSDiv %int %int_4 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 8: Do not merge sdiv of imul
  // 4 / (x * 2)
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIMul %int %2 %int_2\n" +
      "%4 = OpSDiv %int %int_4 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 9: Do not merge sdiv of imul
  // (4 * x) / 2
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIMul %int %int_4 %2\n" +
      "%4 = OpSDiv %int %3 %int_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 10: Do not merge sdiv of imul
  // (x * 4) / 2
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIMul %int %2 %int_4\n" +
      "%4 = OpSDiv %int %3 %int_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 11: Do not merge sdiv of snegate.  If %2 is INT_MIN, then the
  // sign of %3 will be the same as %2.  This cannot be accounted for in OpSDiv.
  // Specifically, (-INT_MIN) / 2 != INT_MIN / -2.
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpSNegate %int %2\n" +
      "%4 = OpSDiv %int %3 %int_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 12: Do not merge sdiv of snegate.  If %2 is INT_MIN, then the
  // sign of %3 will be the same as %2.  This cannot be accounted for in OpSDiv.
  // Specifically, 2 / (-INT_MIN) != -2 / INT_MIN.
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpSNegate %int %2\n" +
      "%4 = OpSDiv %int %int_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 13: Don't merge
  // (x / {null}) / {null}
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_v2float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFDiv %float %2 %v2float_null\n" +
      "%4 = OpFDiv %float %3 %v2float_null\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 14: merge fmul of fdiv
  // (y * x) / x = y
  InstructionFoldingCase<bool>(
    Header() +
        "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
        "; CHECK: [[ldx:%\\w+]] = OpLoad [[float]]\n" +
        "; CHECK: [[ldy:%\\w+]] = OpLoad [[float]] [[y:%\\w+]]\n" +
        "; CHECK: %5 = OpCopyObject [[float]] [[ldy]]\n" +
        "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%x = OpVariable %_ptr_float Function\n" +
        "%y = OpVariable %_ptr_float Function\n" +
        "%2 = OpLoad %float %x\n" +
        "%3 = OpLoad %float %y\n" +
        "%4 = OpFMul %float %3 %2\n" +
        "%5 = OpFDiv %float %4 %2\n" +
        "OpReturn\n" +
        "OpFunctionEnd\n",
    5, true),
  // Test case 15: merge fmul of fdiv
  // (x * y) / x = y
  InstructionFoldingCase<bool>(
    Header() +
        "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
        "; CHECK: [[ldx:%\\w+]] = OpLoad [[float]]\n" +
        "; CHECK: [[ldy:%\\w+]] = OpLoad [[float]] [[y:%\\w+]]\n" +
        "; CHECK: %5 = OpCopyObject [[float]] [[ldy]]\n" +
        "%main = OpFunction %void None %void_func\n" +
        "%main_lab = OpLabel\n" +
        "%x = OpVariable %_ptr_float Function\n" +
        "%y = OpVariable %_ptr_float Function\n" +
        "%2 = OpLoad %float %x\n" +
        "%3 = OpLoad %float %y\n" +
        "%4 = OpFMul %float %2 %3\n" +
        "%5 = OpFDiv %float %4 %2\n" +
        "OpReturn\n" +
        "OpFunctionEnd\n",
    5, true),
  // Test case 16: Do not merge udiv of snegate
  // (-x) / 2u
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_uint Function\n" +
      "%2 = OpLoad %uint %var\n" +
      "%3 = OpSNegate %uint %2\n" +
      "%4 = OpUDiv %uint %3 %uint_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false),
  // Test case 17: Do not merge udiv of snegate
  // 2u / (-x)
  InstructionFoldingCase<bool>(
    Header() +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_uint Function\n" +
      "%2 = OpLoad %uint %var\n" +
      "%3 = OpSNegate %uint %2\n" +
      "%4 = OpUDiv %uint %uint_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, false)
));

INSTANTIATE_TEST_SUITE_P(MergeAddTest, MatchingInstructionFoldingTest,
::testing::Values(
  // Test case 0: merge add of negate
  // (-x) + 2 = 2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFNegate %float %2\n" +
      "%4 = OpFAdd %float %3 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 1: merge add of negate
  // 2 + (-x) = 2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpSNegate %float %2\n" +
      "%4 = OpIAdd %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 2: merge add of negate
  // (-x) + 2 = 2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64 1\n" +
      "; CHECK: [[long_2:%\\w+]] = OpConstant [[long]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpISub [[long]] [[long_2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpSNegate %long %2\n" +
      "%4 = OpIAdd %long %3 %long_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 3: merge add of negate
  // 2 + (-x) = 2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64 1\n" +
      "; CHECK: [[long_2:%\\w+]] = OpConstant [[long]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpISub [[long]] [[long_2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpSNegate %long %2\n" +
      "%4 = OpIAdd %long %long_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 4: merge add of subtract
  // (x - 1) + 2 = x + 1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_1:%\\w+]] = OpConstant [[float]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_1]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %2 %float_1\n" +
      "%4 = OpFAdd %float %3 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 5: merge add of subtract
  // (1 - x) + 2 = 3 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_3:%\\w+]] = OpConstant [[float]] 3\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_3]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %float_1 %2\n" +
      "%4 = OpFAdd %float %3 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 6: merge add of subtract
  // 2 + (x - 1) = x + 1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_1:%\\w+]] = OpConstant [[float]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_1]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %2 %float_1\n" +
      "%4 = OpFAdd %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 7: merge add of subtract
  // 2 + (1 - x) = 3 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_3:%\\w+]] = OpConstant [[float]] 3\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_3]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %float_1 %2\n" +
      "%4 = OpFAdd %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 8: merge add of add
  // (x + 1) + 2 = x + 3
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_3:%\\w+]] = OpConstant [[float]] 3\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_3]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %2 %float_1\n" +
      "%4 = OpFAdd %float %3 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 9: merge add of add
  // (1 + x) + 2 = 3 + x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_3:%\\w+]] = OpConstant [[float]] 3\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_3]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %float_1 %2\n" +
      "%4 = OpFAdd %float %3 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 10: merge add of add
  // 2 + (x + 1) = x + 1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_3:%\\w+]] = OpConstant [[float]] 3\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_3]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %2 %float_1\n" +
      "%4 = OpFAdd %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 11: merge add of add
  // 2 + (1 + x) = 3 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_3:%\\w+]] = OpConstant [[float]] 3\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_3]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %float_1 %2\n" +
      "%4 = OpFAdd %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 12: fold overflowing signed 32 bit iadds
  // (x + int_max) + 1 = x + int_min
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32\n" +
      "; CHECK: [[int_min:%\\w+]] = OpConstant [[int]] -2147483648\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpIAdd [[int]] [[ld]] [[int_min]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIAdd %int %2 %int_max\n" +
      "%4 = OpIAdd %int %3 %int_1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 13: fold overflowing signed 64 bit iadds
  // (x + long_max) + 1 = x + long_min
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64\n" +
      "; CHECK: [[long_min:%\\w+]] = OpConstant [[long]] -9223372036854775808\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpIAdd [[long]] [[ld]] [[long_min]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpIAdd %long %2 %long_max\n" +
      "%4 = OpIAdd %long %3 %long_1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 14: fold overflowing 32 bit unsigned iadds
  // (x + uint_max) + 2 = x + 1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[uint:%\\w+]] = OpTypeInt 32 0\n" +
      "; CHECK: [[uint_1:%\\w+]] = OpConstant [[uint]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[uint]]\n" +
      "; CHECK: %4 = OpIAdd [[uint]] [[ld]] [[uint_1]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_uint Function\n" +
      "%2 = OpLoad %uint %var\n" +
      "%3 = OpIAdd %uint %2 %uint_max\n" +
      "%4 = OpIAdd %uint %3 %uint_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 15: fold overflowing 64 bit unsigned iadds
  // (x + ulong_max) + 2 = x + 1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[ulong:%\\w+]] = OpTypeInt 64 0\n" +
      "; CHECK: [[ulong_1:%\\w+]] = OpConstant [[ulong]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[ulong]]\n" +
      "; CHECK: %4 = OpIAdd [[ulong]] [[ld]] [[ulong_1]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_ulong Function\n" +
      "%2 = OpLoad %ulong %var\n" +
      "%3 = OpIAdd %ulong %2 %ulong_max\n" +
      "%4 = OpIAdd %ulong %3 %ulong_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 16: fold underflowing signed 32 bit iadds
  // (x + int_min) + (-1) = x + int_max
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32\n" +
      "; CHECK: [[int_max:%\\w+]] = OpConstant [[int]] 2147483647\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpIAdd [[int]] [[ld]] [[int_max]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpIAdd %int %2 %int_min\n" +
      "%4 = OpIAdd %int %3 %int_n1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 17: fold underflowing signed 64 bit iadds
  // (x + long_min) + (-1) = x + long_max
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64\n" +
      "; CHECK: [[long_max:%\\w+]] = OpConstant [[long]] 9223372036854775807\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpIAdd [[long]] [[ld]] [[long_max]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpIAdd %long %2 %long_min\n" +
      "%4 = OpIAdd %long %3 %long_n1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true)
));

INSTANTIATE_TEST_SUITE_P(MergeGenericAddSub, MatchingInstructionFoldingTest,
::testing::Values(
    // Test case 0: merge of add of sub
    // (a - b) + b => a
    InstructionFoldingCase<bool>(
      Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: %6 = OpCopyObject [[float]] %3\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var0 = OpVariable %_ptr_float Function\n" +
      "%var1 = OpVariable %_ptr_float Function\n" +
      "%3 = OpLoad %float %var0\n" +
      "%4 = OpLoad %float %var1\n" +
      "%5 = OpFSub %float %3 %4\n" +
      "%6 = OpFAdd %float %5 %4\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
      6, true),
  // Test case 1: merge of add of sub
  // b + (a - b) => a
  InstructionFoldingCase<bool>(
    Header() +
    "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
    "; CHECK: %6 = OpCopyObject [[float]] %3\n" +
    "%main = OpFunction %void None %void_func\n" +
    "%main_lab = OpLabel\n" +
    "%var0 = OpVariable %_ptr_float Function\n" +
    "%var1 = OpVariable %_ptr_float Function\n" +
    "%3 = OpLoad %float %var0\n" +
    "%4 = OpLoad %float %var1\n" +
    "%5 = OpFSub %float %3 %4\n" +
    "%6 = OpFAdd %float %4 %5\n" +
    "OpReturn\n" +
    "OpFunctionEnd\n",
    6, true)
));

INSTANTIATE_TEST_SUITE_P(FactorAddMul, MatchingInstructionFoldingTest,
::testing::Values(
    // Test case 0: factor of add of muls
    // (a * b) + (a * c) => a * (b + c)
    InstructionFoldingCase<bool>(
      Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[newadd:%\\w+]] = OpFAdd [[float]] %4 %5\n" +
      "; CHECK: %9 = OpFMul [[float]] %6 [[newadd]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var0 = OpVariable %_ptr_float Function\n" +
      "%var1 = OpVariable %_ptr_float Function\n" +
      "%var2 = OpVariable %_ptr_float Function\n" +
      "%4 = OpLoad %float %var0\n" +
      "%5 = OpLoad %float %var1\n" +
      "%6 = OpLoad %float %var2\n" +
      "%7 = OpFMul %float %6 %4\n" +
      "%8 = OpFMul %float %6 %5\n" +
      "%9 = OpFAdd %float %7 %8\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
      9, true),
  // Test case 1: factor of add of muls
  // (b * a) + (a * c) => a * (b + c)
  InstructionFoldingCase<bool>(
    Header() +
    "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
    "; CHECK: [[newadd:%\\w+]] = OpFAdd [[float]] %4 %5\n" +
    "; CHECK: %9 = OpFMul [[float]] %6 [[newadd]]\n" +
    "%main = OpFunction %void None %void_func\n" +
    "%main_lab = OpLabel\n" +
    "%var0 = OpVariable %_ptr_float Function\n" +
    "%var1 = OpVariable %_ptr_float Function\n" +
    "%var2 = OpVariable %_ptr_float Function\n" +
    "%4 = OpLoad %float %var0\n" +
    "%5 = OpLoad %float %var1\n" +
    "%6 = OpLoad %float %var2\n" +
    "%7 = OpFMul %float %4 %6\n" +
    "%8 = OpFMul %float %6 %5\n" +
    "%9 = OpFAdd %float %7 %8\n" +
    "OpReturn\n" +
    "OpFunctionEnd\n",
    9, true),
  // Test case 2: factor of add of muls
  // (a * b) + (c * a) => a * (b + c)
  InstructionFoldingCase<bool>(
    Header() +
    "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
    "; CHECK: [[newadd:%\\w+]] = OpFAdd [[float]] %4 %5\n" +
    "; CHECK: %9 = OpFMul [[float]] %6 [[newadd]]\n" +
    "%main = OpFunction %void None %void_func\n" +
    "%main_lab = OpLabel\n" +
    "%var0 = OpVariable %_ptr_float Function\n" +
    "%var1 = OpVariable %_ptr_float Function\n" +
    "%var2 = OpVariable %_ptr_float Function\n" +
    "%4 = OpLoad %float %var0\n" +
    "%5 = OpLoad %float %var1\n" +
    "%6 = OpLoad %float %var2\n" +
    "%7 = OpFMul %float %6 %4\n" +
    "%8 = OpFMul %float %5 %6\n" +
    "%9 = OpFAdd %float %7 %8\n" +
    "OpReturn\n" +
    "OpFunctionEnd\n",
    9, true),
  // Test case 3: factor of add of muls
  // (b * a) + (c * a) => a * (b + c)
  InstructionFoldingCase<bool>(
    Header() +
    "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
    "; CHECK: [[newadd:%\\w+]] = OpFAdd [[float]] %4 %5\n" +
    "; CHECK: %9 = OpFMul [[float]] %6 [[newadd]]\n" +
    "%main = OpFunction %void None %void_func\n" +
    "%main_lab = OpLabel\n" +
    "%var0 = OpVariable %_ptr_float Function\n" +
    "%var1 = OpVariable %_ptr_float Function\n" +
    "%var2 = OpVariable %_ptr_float Function\n" +
    "%4 = OpLoad %float %var0\n" +
    "%5 = OpLoad %float %var1\n" +
    "%6 = OpLoad %float %var2\n" +
    "%7 = OpFMul %float %4 %6\n" +
    "%8 = OpFMul %float %5 %6\n" +
    "%9 = OpFAdd %float %7 %8\n" +
    "OpReturn\n" +
    "OpFunctionEnd\n",
    9, true)
));

INSTANTIATE_TEST_SUITE_P(MergeSubTest, MatchingInstructionFoldingTest,
::testing::Values(
  // Test case 0: merge sub of negate
  // (-x) - 2 = -2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_n2:%\\w+]] = OpConstant [[float]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_n2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFNegate %float %2\n" +
      "%4 = OpFSub %float %3 %float_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 1: merge sub of negate
  // 2 - (-x) = x + 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_2:%\\w+]] = OpConstant [[float]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFNegate %float %2\n" +
      "%4 = OpFSub %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 2: merge sub of negate
  // (-x) - 2 = -2 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64 1\n" +
      "; CHECK: [[long_n2:%\\w+]] = OpConstant [[long]] -2{{[[:space:]]}}\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpISub [[long]] [[long_n2]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpSNegate %long %2\n" +
      "%4 = OpISub %long %3 %long_2\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 3: merge sub of negate
  // 2 - (-x) = x + 2
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64 1\n" +
      "; CHECK: [[long_2:%\\w+]] = OpConstant [[long]] 2\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpIAdd [[long]] [[ld]] [[long_2]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpSNegate %long %2\n" +
      "%4 = OpISub %long %long_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 4: merge add of subtract
  // (x + 2) - 1 = x + 1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_1:%\\w+]] = OpConstant [[float]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_1]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %2 %float_2\n" +
      "%4 = OpFSub %float %3 %float_1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 5: merge add of subtract
  // (2 + x) - 1 = x + 1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_1:%\\w+]] = OpConstant [[float]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_1]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %float_2 %2\n" +
      "%4 = OpFSub %float %3 %float_1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 6: merge add of subtract
  // 2 - (x + 1) = 1 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_1:%\\w+]] = OpConstant [[float]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_1]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %2 %float_1\n" +
      "%4 = OpFSub %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 7: merge add of subtract
  // 2 - (1 + x) = 1 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_1:%\\w+]] = OpConstant [[float]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_1]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFAdd %float %float_1 %2\n" +
      "%4 = OpFSub %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 8: merge subtract of subtract
  // (x - 2) - 1 = x - 3
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_3:%\\w+]] = OpConstant [[float]] 3\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[ld]] [[float_3]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %2 %float_2\n" +
      "%4 = OpFSub %float %3 %float_1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 9: merge subtract of subtract
  // (2 - x) - 1 = 1 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_1:%\\w+]] = OpConstant [[float]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_1]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %float_2 %2\n" +
      "%4 = OpFSub %float %3 %float_1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 10: merge subtract of subtract
  // 2 - (x - 1) = 3 - x
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_3:%\\w+]] = OpConstant [[float]] 3\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFSub [[float]] [[float_3]] [[ld]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %2 %float_1\n" +
      "%4 = OpFSub %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 11: merge subtract of subtract
  // 1 - (2 - x) = x + (-1)
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_n1:%\\w+]] = OpConstant [[float]] -1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_n1]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %float_2 %2\n" +
      "%4 = OpFSub %float %float_1 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 12: merge subtract of subtract
  // 2 - (1 - x) = x + 1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
      "; CHECK: [[float_1:%\\w+]] = OpConstant [[float]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[float]]\n" +
      "; CHECK: %4 = OpFAdd [[float]] [[ld]] [[float_1]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_float Function\n" +
      "%2 = OpLoad %float %var\n" +
      "%3 = OpFSub %float %float_1 %2\n" +
      "%4 = OpFSub %float %float_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 13: merge subtract of subtract with mixed types.
  // 2 - (1 - x) = x + 1
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
      "; CHECK: [[int_1:%\\w+]] = OpConstant [[int]] 1\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpIAdd [[int]] [[ld]] [[int_1]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpISub %int %uint_1 %2\n" +
      "%4 = OpISub %int %int_2 %3\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 14: fold overflowing signed 32 bit isubs
  // (x - int_max) - 1 = x - int_min
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[int:%\\w+]] = OpTypeInt 32\n" +
      "; CHECK: [[int_min:%\\w+]] = OpConstant [[int]] -2147483648\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[int]]\n" +
      "; CHECK: %4 = OpISub [[int]] [[ld]] [[int_min]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_int Function\n" +
      "%2 = OpLoad %int %var\n" +
      "%3 = OpISub %int %2 %int_max\n" +
      "%4 = OpISub %int %3 %int_1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true),
  // Test case 15: fold overflowing signed 64 bit isubs
  // (x - long_max) - 1 = x - long_min
  InstructionFoldingCase<bool>(
    Header() +
      "; CHECK: [[long:%\\w+]] = OpTypeInt 64\n" +
      "; CHECK: [[long_min:%\\w+]] = OpConstant [[long]] -9223372036854775808\n" +
      "; CHECK: [[ld:%\\w+]] = OpLoad [[long]]\n" +
      "; CHECK: %4 = OpISub [[long]] [[ld]] [[long_min]]\n" +
      "%main = OpFunction %void None %void_func\n" +
      "%main_lab = OpLabel\n" +
      "%var = OpVariable %_ptr_long Function\n" +
      "%2 = OpLoad %long %var\n" +
      "%3 = OpISub %long %2 %long_max\n" +
      "%4 = OpISub %long %3 %long_1\n" +
      "OpReturn\n" +
      "OpFunctionEnd\n",
    4, true)
));

INSTANTIATE_TEST_SUITE_P(SelectFoldingTest, MatchingInstructionFoldingTest,
::testing::Values(
  // Test case 0: Fold select with the same values for both sides
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[int0:%\\w+]] = OpConstant [[int]] 0\n" +
          "; CHECK: %2 = OpCopyObject [[int]] [[int0]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_bool Function\n" +
          "%load = OpLoad %bool %n\n" +
          "%2 = OpSelect %int %load %100 %100\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 1: Fold select true to left side
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[int0:%\\w+]] = OpConstant [[int]] 0\n" +
          "; CHECK: %2 = OpCopyObject [[int]] [[int0]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %bool %n\n" +
          "%2 = OpSelect %int %true %100 %n\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 2: Fold select false to right side
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[int0:%\\w+]] = OpConstant [[int]] 0\n" +
          "; CHECK: %2 = OpCopyObject [[int]] [[int0]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %bool %n\n" +
          "%2 = OpSelect %int %false %n %100\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 3: Fold select null to right side
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[int0:%\\w+]] = OpConstant [[int]] 0\n" +
          "; CHECK: %2 = OpCopyObject [[int]] [[int0]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_int Function\n" +
          "%load = OpLoad %int %n\n" +
          "%2 = OpSelect %int %bool_null %load %100\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 4: vector null
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[v2int:%\\w+]] = OpTypeVector [[int]] 2\n" +
          "; CHECK: [[int2:%\\w+]] = OpConstant [[int]] 2\n" +
          "; CHECK: [[v2int2_2:%\\w+]] = OpConstantComposite [[v2int]] [[int2]] [[int2]]\n" +
          "; CHECK: %2 = OpCopyObject [[v2int]] [[v2int2_2]]\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%n = OpVariable %_ptr_v2int Function\n" +
          "%load = OpLoad %v2int %n\n" +
          "%2 = OpSelect %v2int %v2bool_null %load %v2int_2_2\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      2, true),
  // Test case 5: vector select
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[v2int:%\\w+]] = OpTypeVector [[int]] 2\n" +
          "; CHECK: %4 = OpVectorShuffle [[v2int]] %2 %3 0 3\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%m = OpVariable %_ptr_v2int Function\n" +
          "%n = OpVariable %_ptr_v2int Function\n" +
          "%2 = OpLoad %v2int %n\n" +
          "%3 = OpLoad %v2int %n\n" +
          "%4 = OpSelect %v2int %v2bool_true_false %2 %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true),
  // Test case 6: vector select
  InstructionFoldingCase<bool>(
      Header() +
          "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
          "; CHECK: [[v2int:%\\w+]] = OpTypeVector [[int]] 2\n" +
          "; CHECK: %4 = OpVectorShuffle [[v2int]] %2 %3 2 1\n" +
          "%main = OpFunction %void None %void_func\n" +
          "%main_lab = OpLabel\n" +
          "%m = OpVariable %_ptr_v2int Function\n" +
          "%n = OpVariable %_ptr_v2int Function\n" +
          "%2 = OpLoad %v2int %n\n" +
          "%3 = OpLoad %v2int %n\n" +
          "%4 = OpSelect %v2int %v2bool_false_true %2 %3\n" +
          "OpReturn\n" +
          "OpFunctionEnd",
      4, true)
));

INSTANTIATE_TEST_SUITE_P(CompositeExtractOrInsertMatchingTest, MatchingInstructionFoldingTest,
::testing::Values(
    // Test case 0: Extracting from result of consecutive shuffles of differing
    // size.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK: %5 = OpCompositeExtract [[int]] %2 2\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4int Function\n" +
            "%2 = OpLoad %v4int %n\n" +
            "%3 = OpVectorShuffle %v2int %2 %2 2 3\n" +
            "%4 = OpVectorShuffle %v4int %2 %3 0 4 2 5\n" +
            "%5 = OpCompositeExtract %int %4 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, true),
    // Test case 1: Extracting from result of vector shuffle of differing
    // input and result sizes.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK: %4 = OpCompositeExtract [[int]] %2 2\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4int Function\n" +
            "%2 = OpLoad %v4int %n\n" +
            "%3 = OpVectorShuffle %v2int %2 %2 2 3\n" +
            "%4 = OpCompositeExtract %int %3 0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, true),
    // Test case 2: Extracting from result of vector shuffle of differing
    // input and result sizes.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK: %4 = OpCompositeExtract [[int]] %2 3\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4int Function\n" +
            "%2 = OpLoad %v4int %n\n" +
            "%3 = OpVectorShuffle %v2int %2 %2 2 3\n" +
            "%4 = OpCompositeExtract %int %3 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, true),
    // Test case 3: Using fmix feeding extract with a 1 in the a position.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: [[v4double:%\\w+]] = OpTypeVector [[double]] 4\n" +
            "; CHECK: [[ptr_v4double:%\\w+]] = OpTypePointer Function [[v4double]]\n" +
            "; CHECK: [[m:%\\w+]] = OpVariable [[ptr_v4double]] Function\n" +
            "; CHECK: [[n:%\\w+]] = OpVariable [[ptr_v4double]] Function\n" +
            "; CHECK: [[ld:%\\w+]] = OpLoad [[v4double]] [[n]]\n" +
            "; CHECK: %5 = OpCompositeExtract [[double]] [[ld]] 1\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%m = OpVariable %_ptr_v4double Function\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%2 = OpLoad %v4double %m\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%4 = OpExtInst %v4double %1 FMix %2 %3 %v4double_0_1_0_0\n" +
            "%5 = OpCompositeExtract %double %4 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, true),
    // Test case 4: Using fmix feeding extract with a 0 in the a position.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: [[v4double:%\\w+]] = OpTypeVector [[double]] 4\n" +
            "; CHECK: [[ptr_v4double:%\\w+]] = OpTypePointer Function [[v4double]]\n" +
            "; CHECK: [[m:%\\w+]] = OpVariable [[ptr_v4double]] Function\n" +
            "; CHECK: [[n:%\\w+]] = OpVariable [[ptr_v4double]] Function\n" +
            "; CHECK: [[ld:%\\w+]] = OpLoad [[v4double]] [[m]]\n" +
            "; CHECK: %5 = OpCompositeExtract [[double]] [[ld]] 2\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%m = OpVariable %_ptr_v4double Function\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%2 = OpLoad %v4double %m\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%4 = OpExtInst %v4double %1 FMix %2 %3 %v4double_0_1_0_0\n" +
            "%5 = OpCompositeExtract %double %4 2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, true),
    // Test case 5: Using fmix feeding extract with a null for the alpha
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: [[v4double:%\\w+]] = OpTypeVector [[double]] 4\n" +
            "; CHECK: [[ptr_v4double:%\\w+]] = OpTypePointer Function [[v4double]]\n" +
            "; CHECK: [[m:%\\w+]] = OpVariable [[ptr_v4double]] Function\n" +
            "; CHECK: [[n:%\\w+]] = OpVariable [[ptr_v4double]] Function\n" +
            "; CHECK: [[ld:%\\w+]] = OpLoad [[v4double]] [[m]]\n" +
            "; CHECK: %5 = OpCompositeExtract [[double]] [[ld]] 0\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%m = OpVariable %_ptr_v4double Function\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%2 = OpLoad %v4double %m\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%4 = OpExtInst %v4double %1 FMix %2 %3 %v4double_null\n" +
            "%5 = OpCompositeExtract %double %4 0\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, true),
    // Test case 6: Don't fold: Using fmix feeding extract with 0.5 in the a
    // position.
    InstructionFoldingCase<bool>(
        Header() +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%m = OpVariable %_ptr_v4double Function\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%2 = OpLoad %v4double %m\n" +
            "%3 = OpLoad %v4double %n\n" +
            "%4 = OpExtInst %v4double %1 FMix %2 %3 %v4double_1_1_1_0p5\n" +
            "%5 = OpCompositeExtract %double %4 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, false),
    // Test case 7: Extracting the undefined literal value from a vector
    // shuffle.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK: %4 = OpUndef [[int]]\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4int Function\n" +
            "%2 = OpLoad %v4int %n\n" +
            "%3 = OpVectorShuffle %v2int %2 %2 2 4294967295\n" +
            "%4 = OpCompositeExtract %int %3 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, true),
    // Test case 8: Inserting every element of a vector turns into a composite construct.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK-DAG: [[v4:%\\w+]] = OpTypeVector [[int]] 4\n" +
            "; CHECK-DAG: [[int1:%\\w+]] = OpConstant [[int]] 1\n" +
            "; CHECK-DAG: [[int2:%\\w+]] = OpConstant [[int]] 2\n" +
            "; CHECK-DAG: [[int3:%\\w+]] = OpConstant [[int]] 3\n" +
            "; CHECK: [[construct:%\\w+]] = OpCompositeConstruct [[v4]] %100 [[int1]] [[int2]] [[int3]]\n" +
            "; CHECK: %5 = OpCopyObject [[v4]] [[construct]]\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeInsert %v4int %100 %v4int_undef 0\n" +
            "%3 = OpCompositeInsert %v4int %int_1 %2 1\n" +
            "%4 = OpCompositeInsert %v4int %int_2 %3 2\n" +
            "%5 = OpCompositeInsert %v4int %int_3 %4 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, true),
    // Test case 9: Inserting every element of a vector turns into a composite construct in a different order.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK-DAG: [[v4:%\\w+]] = OpTypeVector [[int]] 4\n" +
            "; CHECK-DAG: [[int1:%\\w+]] = OpConstant [[int]] 1\n" +
            "; CHECK-DAG: [[int2:%\\w+]] = OpConstant [[int]] 2\n" +
            "; CHECK-DAG: [[int3:%\\w+]] = OpConstant [[int]] 3\n" +
            "; CHECK: [[construct:%\\w+]] = OpCompositeConstruct [[v4]] %100 [[int1]] [[int2]] [[int3]]\n" +
            "; CHECK: %5 = OpCopyObject [[v4]] [[construct]]\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeInsert %v4int %100 %v4int_undef 0\n" +
            "%4 = OpCompositeInsert %v4int %int_2 %2 2\n" +
            "%3 = OpCompositeInsert %v4int %int_1 %4 1\n" +
            "%5 = OpCompositeInsert %v4int %int_3 %3 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, true),
    // Test case 10: Check multiple inserts to the same position are handled correctly.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK-DAG: [[v4:%\\w+]] = OpTypeVector [[int]] 4\n" +
            "; CHECK-DAG: [[int1:%\\w+]] = OpConstant [[int]] 1\n" +
            "; CHECK-DAG: [[int2:%\\w+]] = OpConstant [[int]] 2\n" +
            "; CHECK-DAG: [[int3:%\\w+]] = OpConstant [[int]] 3\n" +
            "; CHECK: [[construct:%\\w+]] = OpCompositeConstruct [[v4]] %100 [[int1]] [[int2]] [[int3]]\n" +
            "; CHECK: %6 = OpCopyObject [[v4]] [[construct]]\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeInsert %v4int %100 %v4int_undef 0\n" +
            "%3 = OpCompositeInsert %v4int %int_2 %2 2\n" +
            "%4 = OpCompositeInsert %v4int %int_4 %3 1\n" +
            "%5 = OpCompositeInsert %v4int %int_1 %4 1\n" +
            "%6 = OpCompositeInsert %v4int %int_3 %5 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        6, true),
    // Test case 11: The last indexes are 0 and 1, but they have different first indexes.  This should not be folded.
    InstructionFoldingCase<bool>(
        Header() +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeInsert %m2x2int %100 %m2x2int_undef 0 0\n" +
            "%3 = OpCompositeInsert %m2x2int %int_1 %2 1 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, false),
    // Test case 12: Don't fold when there is a partial insertion.
    InstructionFoldingCase<bool>(
        Header() +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeInsert %m2x2int %v2int_1_0 %m2x2int_undef 0\n" +
            "%3 = OpCompositeInsert %m2x2int %int_4 %2 0 0\n" +
            "%4 = OpCompositeInsert %m2x2int %v2int_2_3 %3 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, false),
    // Test case 13: Insert into a column of a matrix
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK-DAG: [[v2:%\\w+]] = OpTypeVector [[int]] 2\n" +
            "; CHECK: [[m2x2:%\\w+]] = OpTypeMatrix [[v2]] 2\n" +
            "; CHECK-DAG: [[m2x2_undef:%\\w+]] = OpUndef [[m2x2]]\n" +
            "; CHECK-DAG: [[int1:%\\w+]] = OpConstant [[int]] 1\n" +
// We keep this insert in the chain.  DeadInsertElimPass should remove it.
            "; CHECK: [[insert:%\\w+]] = OpCompositeInsert [[m2x2]] %100 [[m2x2_undef]] 0 0\n" +
            "; CHECK: [[construct:%\\w+]] = OpCompositeConstruct [[v2]] %100 [[int1]]\n" +
            "; CHECK: %3 = OpCompositeInsert [[m2x2]] [[construct]] [[insert]] 0\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeInsert %m2x2int %100 %m2x2int_undef 0 0\n" +
            "%3 = OpCompositeInsert %m2x2int %int_1 %2 0 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, true),
    // Test case 14: Insert all elements of the matrix.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK-DAG: [[v2:%\\w+]] = OpTypeVector [[int]] 2\n" +
            "; CHECK: [[m2x2:%\\w+]] = OpTypeMatrix [[v2]] 2\n" +
            "; CHECK-DAG: [[m2x2_undef:%\\w+]] = OpUndef [[m2x2]]\n" +
            "; CHECK-DAG: [[int1:%\\w+]] = OpConstant [[int]] 1\n" +
            "; CHECK-DAG: [[int2:%\\w+]] = OpConstant [[int]] 2\n" +
            "; CHECK-DAG: [[int3:%\\w+]] = OpConstant [[int]] 3\n" +
            "; CHECK: [[c0:%\\w+]] = OpCompositeConstruct [[v2]] %100 [[int1]]\n" +
            "; CHECK: [[c1:%\\w+]] = OpCompositeConstruct [[v2]] [[int2]] [[int3]]\n" +
            "; CHECK: [[matrix:%\\w+]] = OpCompositeConstruct [[m2x2]] [[c0]] [[c1]]\n" +
            "; CHECK: %5 = OpCopyObject [[m2x2]] [[matrix]]\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpCompositeConstruct %v2int %100 %int_1\n" +
            "%3 = OpCompositeInsert %m2x2int %2 %m2x2int_undef 0\n" +
            "%4 = OpCompositeInsert %m2x2int %int_2 %3 1 0\n" +
            "%5 = OpCompositeInsert %m2x2int %int_3 %4 1 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, true),
    // Test case 15: Replace construct with extract when reconstructing a member
    // of another object.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK: [[v2:%\\w+]] = OpTypeVector [[int]] 2\n" +
            "; CHECK: [[m2x2:%\\w+]] = OpTypeMatrix [[v2]] 2\n" +
            "; CHECK: [[m2x2_undef:%\\w+]] = OpUndef [[m2x2]]\n" +
            "; CHECK: %5 = OpCompositeExtract [[v2]] [[m2x2_undef]]\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%3 = OpCompositeExtract %int %m2x2int_undef 1 0\n" +
            "%4 = OpCompositeExtract %int %m2x2int_undef 1 1\n" +
            "%5 = OpCompositeConstruct %v2int %3 %4\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, true),
    // Test case 16: Don't fold when type cannot be deduced to a constant.
    InstructionFoldingCase<bool>(
        Header() +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%4 = OpCompositeInsert %struct_v2int_int_int %int_1 %struct_v2int_int_int_null 2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, false),
    // Test case 17: Don't fold when index into composite is out of bounds.
    InstructionFoldingCase<bool>(
	Header() +
            "%main = OpFunction %void None %void_func\n" +
	    "%main_lab = OpLabel\n" +
	    "%4 = OpCompositeExtract %int %struct_v2int_int_int 3\n" +
	    "OpReturn\n" +
	    "OpFunctionEnd",
	4, false),
    // Test case 18: Fold when every element of an array is inserted.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK: [[int2:%\\w+]] = OpConstant [[int]] 2\n" +
            "; CHECK-DAG: [[arr_type:%\\w+]] = OpTypeArray [[int]] [[int2]]\n" +
            "; CHECK-DAG: [[int10:%\\w+]] = OpConstant [[int]] 10\n" +
            "; CHECK-DAG: [[int1:%\\w+]] = OpConstant [[int]] 1\n" +
            "; CHECK: [[construct:%\\w+]] = OpCompositeConstruct [[arr_type]] [[int10]] [[int1]]\n" +
            "; CHECK: %5 = OpCopyObject [[arr_type]] [[construct]]\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%4 = OpCompositeInsert %int_arr_2 %int_10 %int_arr_2_undef 0\n" +
            "%5 = OpCompositeInsert %int_arr_2 %int_1 %4 1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        5, true),
    // Test case 19: Don't fold for isomorphic structs
    InstructionFoldingCase<bool>(
        Header() +
            "%structA = OpTypeStruct %ulong\n" +
            "%structB = OpTypeStruct %ulong\n" +
            "%structC = OpTypeStruct %structB\n" +
            "%struct_a_undef = OpUndef %structA\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%3 = OpCompositeExtract %ulong %struct_a_undef 0\n" +
            "%4 = OpCompositeConstruct %structB %3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        4, false)
));

INSTANTIATE_TEST_SUITE_P(DotProductMatchingTest, MatchingInstructionFoldingTest,
::testing::Values(
    // Test case 0: Using OpDot to extract last element.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
            "; CHECK: %3 = OpCompositeExtract [[float]] %2 3\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4float Function\n" +
            "%2 = OpLoad %v4float %n\n" +
            "%3 = OpDot %float %2 %v4float_0_0_0_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, true),
    // Test case 1: Using OpDot to extract last element.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
            "; CHECK: %3 = OpCompositeExtract [[float]] %2 3\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4float Function\n" +
            "%2 = OpLoad %v4float %n\n" +
            "%3 = OpDot %float %v4float_0_0_0_1 %2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, true),
    // Test case 2: Using OpDot to extract second element.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[float:%\\w+]] = OpTypeFloat 32\n" +
            "; CHECK: %3 = OpCompositeExtract [[float]] %2 1\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4float Function\n" +
            "%2 = OpLoad %v4float %n\n" +
            "%3 = OpDot %float %v4float_0_1_0_0 %2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, true),
    // Test case 3: Using OpDot to extract last element.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: %3 = OpCompositeExtract [[double]] %2 3\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%2 = OpLoad %v4double %n\n" +
            "%3 = OpDot %double %2 %v4double_0_0_0_1\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, true),
    // Test case 4: Using OpDot to extract last element.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: %3 = OpCompositeExtract [[double]] %2 3\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%2 = OpLoad %v4double %n\n" +
            "%3 = OpDot %double %v4double_0_0_0_1 %2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, true),
    // Test case 5: Using OpDot to extract second element.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: %3 = OpCompositeExtract [[double]] %2 1\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%2 = OpLoad %v4double %n\n" +
            "%3 = OpDot %double %v4double_0_1_0_0 %2\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, true)
));

INSTANTIATE_TEST_SUITE_P(VectorShuffleMatchingTest, MatchingInstructionFoldingTest,
::testing::Values(
    // Test case 0: Using OpDot to extract last element.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[int:%\\w+]] = OpTypeInt 32 1\n" +
            "; CHECK: [[v2int:%\\w+]] = OpTypeVector [[int]] 2{{[[:space:]]}}\n" +
            "; CHECK: [[null:%\\w+]] = OpConstantNull [[v2int]]\n" +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: %3 = OpVectorShuffle [[v2int]] [[null]] {{%\\w+}} 4294967295 2\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_int Function\n" +
            "%load = OpLoad %int %n\n" +
            "%2 = OpVectorShuffle %v2int %v2int_null %v2int_2_3 3 0xFFFFFFFF \n" +
            "%3 = OpVectorShuffle %v2int %2 %v2int_2_3 1 2 \n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        3, true)
 ));

// Issue #5658: The Adreno compiler does not handle 16-bit FMA instructions well.
// We want to avoid this by not generating FMA. We decided to never generate
// FMAs because, from a SPIR-V perspective, it is neutral. The ICD can generate
// the FMA if it wants. The simplest code is no code.
INSTANTIATE_TEST_SUITE_P(FmaGenerationMatchingTest, MatchingInstructionFoldingTest,
::testing::Values(
   // Test case 0: Don't fold (x * y) + a
   InstructionFoldingCase<bool>(
       Header() +
           "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%x = OpVariable %_ptr_float Function\n" +
           "%y = OpVariable %_ptr_float Function\n" +
           "%a = OpVariable %_ptr_float Function\n" +
           "%lx = OpLoad %float %x\n" +
           "%ly = OpLoad %float %y\n" +
           "%mul = OpFMul %float %lx %ly\n" +
           "%la = OpLoad %float %a\n" +
           "%3 = OpFAdd %float %mul %la\n" +
           "OpStore %a %3\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       3, false),
    // Test case 1: Don't fold a + (x * y)
   InstructionFoldingCase<bool>(
       Header() +
           "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%x = OpVariable %_ptr_float Function\n" +
           "%y = OpVariable %_ptr_float Function\n" +
           "%a = OpVariable %_ptr_float Function\n" +
           "%lx = OpLoad %float %x\n" +
           "%ly = OpLoad %float %y\n" +
           "%mul = OpFMul %float %lx %ly\n" +
           "%la = OpLoad %float %a\n" +
           "%3 = OpFAdd %float %la %mul\n" +
           "OpStore %a %3\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       3, false),
   // Test case 2: Don't fold (x * y) + a with vectors
   InstructionFoldingCase<bool>(
       Header() +
           "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%x = OpVariable %_ptr_v4float Function\n" +
           "%y = OpVariable %_ptr_v4float Function\n" +
           "%a = OpVariable %_ptr_v4float Function\n" +
           "%lx = OpLoad %v4float %x\n" +
           "%ly = OpLoad %v4float %y\n" +
           "%mul = OpFMul %v4float %lx %ly\n" +
           "%la = OpLoad %v4float %a\n" +
           "%3 = OpFAdd %v4float %mul %la\n" +
           "OpStore %a %3\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       3,false),
    // Test case 3: Don't fold a + (x * y) with vectors
   InstructionFoldingCase<bool>(
       Header() +
           "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%x = OpVariable %_ptr_float Function\n" +
           "%y = OpVariable %_ptr_float Function\n" +
           "%a = OpVariable %_ptr_float Function\n" +
           "%lx = OpLoad %float %x\n" +
           "%ly = OpLoad %float %y\n" +
           "%mul = OpFMul %float %lx %ly\n" +
           "%la = OpLoad %float %a\n" +
           "%3 = OpFAdd %float %la %mul\n" +
           "OpStore %a %3\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       3, false),
   // Test 4: Don't fold if the multiple is marked no contract.
   InstructionFoldingCase<bool>(
       std::string() +
           "OpCapability Shader\n" +
           "OpMemoryModel Logical GLSL450\n" +
           "OpEntryPoint Fragment %main \"main\"\n" +
           "OpExecutionMode %main OriginUpperLeft\n" +
           "OpSource GLSL 140\n" +
           "OpName %main \"main\"\n" +
           "OpDecorate %mul NoContraction\n" +
           "%void = OpTypeVoid\n" +
           "%void_func = OpTypeFunction %void\n" +
           "%bool = OpTypeBool\n" +
           "%float = OpTypeFloat 32\n" +
           "%_ptr_float = OpTypePointer Function %float\n" +
           "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%x = OpVariable %_ptr_float Function\n" +
           "%y = OpVariable %_ptr_float Function\n" +
           "%a = OpVariable %_ptr_float Function\n" +
           "%lx = OpLoad %float %x\n" +
           "%ly = OpLoad %float %y\n" +
           "%mul = OpFMul %float %lx %ly\n" +
           "%la = OpLoad %float %a\n" +
           "%3 = OpFAdd %float %mul %la\n" +
           "OpStore %a %3\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       3, false),
       // Test 5: Don't fold if the add is marked no contract.
       InstructionFoldingCase<bool>(
           std::string() +
               "OpCapability Shader\n" +
               "OpMemoryModel Logical GLSL450\n" +
               "OpEntryPoint Fragment %main \"main\"\n" +
               "OpExecutionMode %main OriginUpperLeft\n" +
               "OpSource GLSL 140\n" +
               "OpName %main \"main\"\n" +
               "OpDecorate %3 NoContraction\n" +
               "%void = OpTypeVoid\n" +
               "%void_func = OpTypeFunction %void\n" +
               "%bool = OpTypeBool\n" +
               "%float = OpTypeFloat 32\n" +
               "%_ptr_float = OpTypePointer Function %float\n" +
               "%main = OpFunction %void None %void_func\n" +
               "%main_lab = OpLabel\n" +
               "%x = OpVariable %_ptr_float Function\n" +
               "%y = OpVariable %_ptr_float Function\n" +
               "%a = OpVariable %_ptr_float Function\n" +
               "%lx = OpLoad %float %x\n" +
               "%ly = OpLoad %float %y\n" +
               "%mul = OpFMul %float %lx %ly\n" +
               "%la = OpLoad %float %a\n" +
               "%3 = OpFAdd %float %mul %la\n" +
               "OpStore %a %3\n" +
               "OpReturn\n" +
               "OpFunctionEnd",
           3, false),
    // Test case 6: Don't fold (x * y) - a
    InstructionFoldingCase<bool>(
       Header() +
           "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%x = OpVariable %_ptr_float Function\n" +
           "%y = OpVariable %_ptr_float Function\n" +
           "%a = OpVariable %_ptr_float Function\n" +
           "%lx = OpLoad %float %x\n" +
           "%ly = OpLoad %float %y\n" +
           "%mul = OpFMul %float %lx %ly\n" +
           "%la = OpLoad %float %a\n" +
           "%3 = OpFSub %float %mul %la\n" +
           "OpStore %a %3\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       3, false),
   // Test case 7: Don't fold a - (x * y)
   InstructionFoldingCase<bool>(
       Header() +
           "%main = OpFunction %void None %void_func\n" +
           "%main_lab = OpLabel\n" +
           "%x = OpVariable %_ptr_float Function\n" +
           "%y = OpVariable %_ptr_float Function\n" +
           "%a = OpVariable %_ptr_float Function\n" +
           "%lx = OpLoad %float %x\n" +
           "%ly = OpLoad %float %y\n" +
           "%mul = OpFMul %float %lx %ly\n" +
           "%la = OpLoad %float %a\n" +
           "%3 = OpFSub %float %la %mul\n" +
           "OpStore %a %3\n" +
           "OpReturn\n" +
           "OpFunctionEnd",
       3, false)
));

using MatchingInstructionWithNoResultFoldingTest =
::testing::TestWithParam<InstructionFoldingCase<bool>>;

// Test folding instructions that do not have a result.  The instruction
// that will be folded is the last instruction before the return.  If there
// are multiple returns, there is not guarantee which one is used.
TEST_P(MatchingInstructionWithNoResultFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) = FoldInstruction(tc.test_body, tc.id_to_fold,SPV_ENV_UNIVERSAL_1_1);

  // Find the instruction to test.
  EXPECT_EQ(inst != nullptr, tc.expected_result);
  if (inst != nullptr) {
    Match(tc.test_body, context.get());
  }
}

INSTANTIATE_TEST_SUITE_P(StoreMatchingTest, MatchingInstructionWithNoResultFoldingTest,
::testing::Values(
    // Test case 0: Remove store of undef.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: OpLabel\n" +
            "; CHECK-NOT: OpStore\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%undef = OpUndef %v4double\n" +
            "OpStore %n %undef\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        0 /* OpStore */, true),
    // Test case 1: Keep volatile store.
    InstructionFoldingCase<bool>(
        Header() +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%n = OpVariable %_ptr_v4double Function\n" +
            "%undef = OpUndef %v4double\n" +
            "OpStore %n %undef Volatile\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        0 /* OpStore */, false)
));

INSTANTIATE_TEST_SUITE_P(VectorShuffleMatchingTest, MatchingInstructionWithNoResultFoldingTest,
::testing::Values(
    // Test case 0: Basic test 1
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %7 %5 2 3 6 7\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v4double %5 %6 2 3 4 5\n" +
            "%9 = OpVectorShuffle %v4double %7 %8 2 3 4 5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 1: Basic test 2
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %6 %7 0 1 4 5\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v4double %5 %6 2 3 4 5\n" +
            "%9 = OpVectorShuffle %v4double %8 %7 2 3 4 5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 2: Basic test 3
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %5 %7 3 2 4 5\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v4double %5 %6 2 3 4 5\n" +
            "%9 = OpVectorShuffle %v4double %8 %7 1 0 4 5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 3: Basic test 4
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %7 %6 2 3 5 4\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v4double %5 %6 2 3 4 5\n" +
            "%9 = OpVectorShuffle %v4double %7 %8 2 3 7 6\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 4: Don't fold, need both operands of the feeder.
    InstructionFoldingCase<bool>(
        Header() +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v4double %5 %6 2 3 4 5\n" +
            "%9 = OpVectorShuffle %v4double %7 %8 2 3 7 5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, false),
    // Test case 5: Don't fold, need both operands of the feeder.
    InstructionFoldingCase<bool>(
        Header() +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v4double %5 %6 2 3 4 5\n" +
            "%9 = OpVectorShuffle %v4double %8 %7 2 0 7 5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, false),
    // Test case 6: Fold, need both operands of the feeder, but they are the same.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %5 %7 0 2 7 5\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v4double %5 %5 2 3 4 5\n" +
            "%9 = OpVectorShuffle %v4double %8 %7 2 0 7 5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 7: Fold, need both operands of the feeder, but they are the same.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %7 %5 2 0 5 7\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v4double %5 %5 2 3 4 5\n" +
            "%9 = OpVectorShuffle %v4double %7 %8 2 0 7 5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 8: Replace first operand with a smaller vector.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %5 %7 0 0 5 3\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v2double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v2double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v4double %5 %5 0 1 2 3\n" +
            "%9 = OpVectorShuffle %v4double %8 %7 2 0 7 5\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 9: Replace first operand with a larger vector.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %5 %7 3 0 7 5\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v2double %5 %5 0 3\n" +
            "%9 = OpVectorShuffle %v4double %8 %7 1 0 5 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 10: Replace unused operand with null.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: [[v4double:%\\w+]] = OpTypeVector [[double]] 2\n" +
            "; CHECK: [[null:%\\w+]] = OpConstantNull [[v4double]]\n" +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} [[null]] %7 4 2 5 3\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v2double %5 %5 0 3\n" +
            "%9 = OpVectorShuffle %v4double %8 %7 4 2 5 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 11: Replace unused operand with null.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: [[v4double:%\\w+]] = OpTypeVector [[double]] 2\n" +
            "; CHECK: [[null:%\\w+]] = OpConstantNull [[v4double]]\n" +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} [[null]] %5 2 2 5 5\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%8 = OpVectorShuffle %v2double %5 %5 0 3\n" +
            "%9 = OpVectorShuffle %v4double %8 %8 2 2 3 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 12: Replace unused operand with null.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: [[v4double:%\\w+]] = OpTypeVector [[double]] 2\n" +
            "; CHECK: [[null:%\\w+]] = OpConstantNull [[v4double]]\n" +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %7 [[null]] 2 0 1 3\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v2double %5 %5 0 3\n" +
            "%9 = OpVectorShuffle %v4double %7 %8 2 0 1 3\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 13: Shuffle with undef literal.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: [[v4double:%\\w+]] = OpTypeVector [[double]] 2\n" +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %7 {{%\\w+}} 2 0 1 4294967295\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v2double %5 %5 0 1\n" +
            "%9 = OpVectorShuffle %v4double %7 %8 2 0 1 4294967295\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true),
    // Test case 14: Shuffle with undef literal and change size of first input vector.
    InstructionFoldingCase<bool>(
        Header() +
            "; CHECK: [[double:%\\w+]] = OpTypeFloat 64\n" +
            "; CHECK: [[v4double:%\\w+]] = OpTypeVector [[double]] 2\n" +
            "; CHECK: OpVectorShuffle\n" +
            "; CHECK: OpVectorShuffle {{%\\w+}} %5 %7 0 1 4 4294967295\n" +
            "; CHECK: OpReturn\n" +
            "%main = OpFunction %void None %void_func\n" +
            "%main_lab = OpLabel\n" +
            "%2 = OpVariable %_ptr_v4double Function\n" +
            "%3 = OpVariable %_ptr_v4double Function\n" +
            "%4 = OpVariable %_ptr_v4double Function\n" +
            "%5 = OpLoad %v4double %2\n" +
            "%6 = OpLoad %v4double %3\n" +
            "%7 = OpLoad %v4double %4\n" +
            "%8 = OpVectorShuffle %v2double %5 %5 0 1\n" +
            "%9 = OpVectorShuffle %v4double %8 %7 0 1 2 4294967295\n" +
            "OpReturn\n" +
            "OpFunctionEnd",
        9, true)
));

using EntryPointFoldingTest =
::testing::TestWithParam<InstructionFoldingCase<bool>>;

TEST_P(EntryPointFoldingTest, Case) {
  const auto& tc = GetParam();

  // Build module.
  std::unique_ptr<IRContext> context =
      BuildModule(SPV_ENV_UNIVERSAL_1_1, nullptr, tc.test_body,
                  SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS);
  ASSERT_NE(nullptr, context);

  // Find the first entry point. That is the instruction we want to fold.
  Instruction* inst = nullptr;
  ASSERT_FALSE(context->module()->entry_points().empty());
  inst = &*context->module()->entry_points().begin();
  assert(inst && "Invalid test.  Could not find entry point instruction to fold.");
  std::unique_ptr<Instruction> original_inst(inst->Clone(context.get()));
  bool succeeded = context->get_instruction_folder().FoldInstruction(inst);
  EXPECT_EQ(succeeded, tc.expected_result);
  if (succeeded) {
    Match(tc.test_body, context.get());
  }
}

INSTANTIATE_TEST_SUITE_P(OpEntryPointFoldingTest, EntryPointFoldingTest,
::testing::Values(
    // Test case 0: Basic test 1
    InstructionFoldingCase<bool>(std::string() +
                    "; CHECK: OpEntryPoint Fragment %2 \"main\" %3\n" +
                             "OpCapability Shader\n" +
                        "%1 = OpExtInstImport \"GLSL.std.450\"\n" +
                             "OpMemoryModel Logical GLSL450\n" +
                             "OpEntryPoint Fragment %2 \"main\" %3 %3 %3\n" +
                             "OpExecutionMode %2 OriginUpperLeft\n" +
                             "OpSource GLSL 430\n" +
                             "OpDecorate %3 Location 0\n" +
                     "%void = OpTypeVoid\n" +
                        "%5 = OpTypeFunction %void\n" +
                    "%float = OpTypeFloat 32\n" +
                  "%v4float = OpTypeVector %float 4\n" +
      "%_ptr_Output_v4float = OpTypePointer Output %v4float\n" +
                        "%3 = OpVariable %_ptr_Output_v4float Output\n" +
                      "%int = OpTypeInt 32 1\n" +
                    "%int_0 = OpConstant %int 0\n" +
"%_ptr_PushConstant_v4float = OpTypePointer PushConstant %v4float\n" +
                        "%2 = OpFunction %void None %5\n" +
                       "%12 = OpLabel\n" +
                             "OpReturn\n" +
                             "OpFunctionEnd\n",
        9, true),
    InstructionFoldingCase<bool>(std::string() +
                    "; CHECK: OpEntryPoint Fragment %2 \"main\" %3 %4\n" +
                             "OpCapability Shader\n" +
                        "%1 = OpExtInstImport \"GLSL.std.450\"\n" +
                             "OpMemoryModel Logical GLSL450\n" +
                             "OpEntryPoint Fragment %2 \"main\" %3 %4 %3\n" +
                             "OpExecutionMode %2 OriginUpperLeft\n" +
                             "OpSource GLSL 430\n" +
                             "OpDecorate %3 Location 0\n" +
                     "%void = OpTypeVoid\n" +
                        "%5 = OpTypeFunction %void\n" +
                    "%float = OpTypeFloat 32\n" +
                  "%v4float = OpTypeVector %float 4\n" +
      "%_ptr_Output_v4float = OpTypePointer Output %v4float\n" +
                        "%3 = OpVariable %_ptr_Output_v4float Output\n" +
                        "%4 = OpVariable %_ptr_Output_v4float Output\n" +
                      "%int = OpTypeInt 32 1\n" +
                    "%int_0 = OpConstant %int 0\n" +
"%_ptr_PushConstant_v4float = OpTypePointer PushConstant %v4float\n" +
                        "%2 = OpFunction %void None %5\n" +
                       "%12 = OpLabel\n" +
                             "OpReturn\n" +
                             "OpFunctionEnd\n",
        9, true),
    InstructionFoldingCase<bool>(std::string() +
                    "; CHECK: OpEntryPoint Fragment %2 \"main\" %4 %3\n" +
                             "OpCapability Shader\n" +
                        "%1 = OpExtInstImport \"GLSL.std.450\"\n" +
                             "OpMemoryModel Logical GLSL450\n" +
                             "OpEntryPoint Fragment %2 \"main\" %4 %4 %3\n" +
                             "OpExecutionMode %2 OriginUpperLeft\n" +
                             "OpSource GLSL 430\n" +
                             "OpDecorate %3 Location 0\n" +
                     "%void = OpTypeVoid\n" +
                        "%5 = OpTypeFunction %void\n" +
                    "%float = OpTypeFloat 32\n" +
                  "%v4float = OpTypeVector %float 4\n" +
      "%_ptr_Output_v4float = OpTypePointer Output %v4float\n" +
                        "%3 = OpVariable %_ptr_Output_v4float Output\n" +
                        "%4 = OpVariable %_ptr_Output_v4float Output\n" +
                      "%int = OpTypeInt 32 1\n" +
                    "%int_0 = OpConstant %int 0\n" +
"%_ptr_PushConstant_v4float = OpTypePointer PushConstant %v4float\n" +
                        "%2 = OpFunction %void None %5\n" +
                       "%12 = OpLabel\n" +
                             "OpReturn\n" +
                             "OpFunctionEnd\n",
        9, true)
));

using SPV14FoldingTest =
::testing::TestWithParam<InstructionFoldingCase<bool>>;

TEST_P(SPV14FoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) = FoldInstruction(tc.test_body, tc.id_to_fold,SPV_ENV_UNIVERSAL_1_4);

  EXPECT_EQ(inst != nullptr, tc.expected_result);
  if (inst !=  nullptr) {
    Match(tc.test_body, context.get());
  }
}

INSTANTIATE_TEST_SUITE_P(SPV14FoldingTest, SPV14FoldingTest,
::testing::Values(
    // Test case 0: select vectors with scalar condition.
    InstructionFoldingCase<bool>(std::string() +
"; CHECK-NOT: OpSelect\n" +
"; CHECK: %3 = OpCopyObject {{%\\w+}} %1\n" +
"OpCapability Shader\n" +
"OpCapability Linkage\n" +
"%void = OpTypeVoid\n" +
"%bool = OpTypeBool\n" +
"%true = OpConstantTrue %bool\n" +
"%int = OpTypeInt 32 0\n" +
"%int4 = OpTypeVector %int 4\n" +
"%int_0 = OpConstant %int 0\n" +
"%int_1 = OpConstant %int 1\n" +
"%1 = OpUndef %int4\n" +
"%2 = OpUndef %int4\n" +
"%void_fn = OpTypeFunction %void\n" +
"%func = OpFunction %void None %void_fn\n" +
"%entry = OpLabel\n" +
"%3 = OpSelect %int4 %true %1 %2\n" +
"OpReturn\n" +
"OpFunctionEnd\n"
,
                                 3, true),
    // Test case 1: select struct with scalar condition.
    InstructionFoldingCase<bool>(std::string() +
"; CHECK-NOT: OpSelect\n" +
"; CHECK: %3 = OpCopyObject {{%\\w+}} %2\n" +
"OpCapability Shader\n" +
"OpCapability Linkage\n" +
"%void = OpTypeVoid\n" +
"%bool = OpTypeBool\n" +
"%true = OpConstantFalse %bool\n" +
"%int = OpTypeInt 32 0\n" +
"%struct = OpTypeStruct %int %int %int %int\n" +
"%int_0 = OpConstant %int 0\n" +
"%int_1 = OpConstant %int 1\n" +
"%1 = OpUndef %struct\n" +
"%2 = OpUndef %struct\n" +
"%void_fn = OpTypeFunction %void\n" +
"%func = OpFunction %void None %void_fn\n" +
"%entry = OpLabel\n" +
"%3 = OpSelect %struct %true %1 %2\n" +
"OpReturn\n" +
"OpFunctionEnd\n"
,
                                 3, true),
    // Test case 1: select array with scalar condition.
    InstructionFoldingCase<bool>(std::string() +
"; CHECK-NOT: OpSelect\n" +
"; CHECK: %3 = OpCopyObject {{%\\w+}} %2\n" +
"OpCapability Shader\n" +
"OpCapability Linkage\n" +
"%void = OpTypeVoid\n" +
"%bool = OpTypeBool\n" +
"%true = OpConstantFalse %bool\n" +
"%int = OpTypeInt 32 0\n" +
"%int_0 = OpConstant %int 0\n" +
"%int_1 = OpConstant %int 1\n" +
"%int_4 = OpConstant %int 4\n" +
"%array = OpTypeStruct %int %int %int %int\n" +
"%1 = OpUndef %array\n" +
"%2 = OpUndef %array\n" +
"%void_fn = OpTypeFunction %void\n" +
"%func = OpFunction %void None %void_fn\n" +
"%entry = OpLabel\n" +
"%3 = OpSelect %array %true %1 %2\n" +
"OpReturn\n" +
"OpFunctionEnd\n"
,
                                 3, true)
));

std::string FloatControlsHeader(const std::string& capabilities) {
  std::string header = R"(
OpCapability Shader
)" + capabilities + R"(
%void = OpTypeVoid
%float = OpTypeFloat 32
%float_0 = OpConstant %float 0
%float_1 = OpConstant %float 1
%void_fn = OpTypeFunction %void
%func = OpFunction %void None %void_fn
%entry = OpLabel
)";

  return header;
}

using FloatControlsFoldingTest =
::testing::TestWithParam<InstructionFoldingCase<bool>>;

TEST_P(FloatControlsFoldingTest, Case) {
  const auto& tc = GetParam();

  std::unique_ptr<IRContext> context;
  Instruction* inst;
  std::tie(context, inst) = FoldInstruction(tc.test_body, tc.id_to_fold, SPV_ENV_UNIVERSAL_1_4);

  EXPECT_EQ(inst != nullptr, tc.expected_result);
  if (inst != nullptr) {
    Match(tc.test_body, context.get());
  }
}

INSTANTIATE_TEST_SUITE_P(FloatControlsFoldingTest, FloatControlsFoldingTest,
::testing::Values(
    // Test case 0: no folding with DenormPreserve
    InstructionFoldingCase<bool>(FloatControlsHeader("OpCapability DenormPreserve") +
                                 "%1 = OpFAdd %float %float_0 %float_1\n" +
                                 "OpReturn\n" +
                                 "OpFunctionEnd\n"
,
                                 1, false),
    // Test case 1: no folding with DenormFlushToZero
    InstructionFoldingCase<bool>(FloatControlsHeader("OpCapability DenormFlushToZero") +
                                 "%1 = OpFAdd %float %float_0 %float_1\n" +
                                 "OpReturn\n" +
                                 "OpFunctionEnd\n"
,
                                 1, false),
    // Test case 2: no folding with SignedZeroInfNanPreserve
    InstructionFoldingCase<bool>(FloatControlsHeader("OpCapability SignedZeroInfNanPreserve") +
                                 "%1 = OpFAdd %float %float_0 %float_1\n" +
                                 "OpReturn\n" +
                                 "OpFunctionEnd\n"
,
                                 1, false),
    // Test case 3: no folding with RoundingModeRTE
    InstructionFoldingCase<bool>(FloatControlsHeader("OpCapability RoundingModeRTE") +
                                 "%1 = OpFAdd %float %float_0 %float_1\n" +
                                 "OpReturn\n" +
                                 "OpFunctionEnd\n"
,
                                 1, false),
    // Test case 4: no folding with RoundingModeRTZ
    InstructionFoldingCase<bool>(FloatControlsHeader("OpCapability RoundingModeRTZ") +
                                 "%1 = OpFAdd %float %float_0 %float_1\n" +
                                 "OpReturn\n" +
                                 "OpFunctionEnd\n"
,
                                 1, false)
));

std::string ImageOperandsTestBody(const std::string& image_instruction) {
  std::string body = R"(
               OpCapability Shader
               OpCapability ImageGatherExtended
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main"
               OpExecutionMode %main OriginUpperLeft
               OpDecorate %Texture DescriptorSet 0
               OpDecorate %Texture Binding 0
        %int = OpTypeInt 32 1
     %int_n1 = OpConstant %int -1
          %5 = OpConstant %int 0
      %float = OpTypeFloat 32
    %float_0 = OpConstant %float 0
%type_2d_image = OpTypeImage %float 2D 2 0 0 1 Unknown
%type_sampled_image = OpTypeSampledImage %type_2d_image
%type_sampler = OpTypeSampler
%_ptr_UniformConstant_type_sampler = OpTypePointer UniformConstant %type_sampler
%_ptr_UniformConstant_type_2d_image = OpTypePointer UniformConstant %type_2d_image
   %_ptr_int = OpTypePointer Function %int
      %v2int = OpTypeVector %int 2
         %10 = OpTypeVector %float 4
       %void = OpTypeVoid
         %22 = OpTypeFunction %void
    %v2float = OpTypeVector %float 2
      %v3int = OpTypeVector %int 3
    %Texture = OpVariable %_ptr_UniformConstant_type_2d_image UniformConstant
   %gSampler = OpVariable %_ptr_UniformConstant_type_sampler UniformConstant
        %110 = OpConstantComposite %v2int %5 %5
        %101 = OpConstantComposite %v2int %int_n1 %int_n1
         %20 = OpConstantComposite %v2float %float_0 %float_0
       %main = OpFunction %void None %22
         %23 = OpLabel
        %var = OpVariable %_ptr_int Function
         %88 = OpLoad %type_2d_image %Texture
        %val = OpLoad %int %var
    %sampler = OpLoad %type_sampler %gSampler
         %26 = OpSampledImage %type_sampled_image %88 %sampler
)" + image_instruction + R"(
               OpReturn
               OpFunctionEnd
)";

  return body;
}

INSTANTIATE_TEST_SUITE_P(ImageOperandsBitmaskFoldingTest, MatchingInstructionWithNoResultFoldingTest,
::testing::Values(
    // Test case 0: OpImageFetch without Offset
    InstructionFoldingCase<bool>(ImageOperandsTestBody(
        "%89 = OpImageFetch %10 %88 %101 Lod %5 \n")
        , 89, false),
    // Test case 1: OpImageFetch with non-const offset
    InstructionFoldingCase<bool>(ImageOperandsTestBody(
        "%89 = OpImageFetch %10 %88 %101 Lod|Offset %5 %val \n")
        , 89, false),
    // Test case 2: OpImageFetch with Lod and Offset
    InstructionFoldingCase<bool>(ImageOperandsTestBody(
      "         %89 = OpImageFetch %10 %88 %101 Lod|Offset %5 %101      \n"
      "; CHECK: %89 = OpImageFetch %10 %88 %101 Lod|ConstOffset %5 %101 \n")
      , 89, true),
    // Test case 3: OpImageFetch with Bias and Offset
    InstructionFoldingCase<bool>(ImageOperandsTestBody(
      "         %89 = OpImageFetch %10 %88 %101 Bias|Offset %5 %101      \n"
      "; CHECK: %89 = OpImageFetch %10 %88 %101 Bias|ConstOffset %5 %101 \n")
      , 89, true),
    // Test case 4: OpImageFetch with Grad and Offset.
    // Grad adds 2 operands to the instruction.
    InstructionFoldingCase<bool>(ImageOperandsTestBody(
      "         %89 = OpImageFetch %10 %88 %101 Grad|Offset %5 %5 %101      \n"
      "; CHECK: %89 = OpImageFetch %10 %88 %101 Grad|ConstOffset %5 %5 %101 \n")
      , 89, true),
    // Test case 5: OpImageFetch with Offset and MinLod.
    // This is an example of a case where the bitmask bit-offset is larger than
    // that of the Offset.
    InstructionFoldingCase<bool>(ImageOperandsTestBody(
      "         %89 = OpImageFetch %10 %88 %101 Offset|MinLod %101 %5      \n"
      "; CHECK: %89 = OpImageFetch %10 %88 %101 ConstOffset|MinLod %101 %5 \n")
      , 89, true),
    // Test case 6: OpImageGather with constant Offset
    InstructionFoldingCase<bool>(ImageOperandsTestBody(
      "         %89 = OpImageGather %10 %26 %20 %5 Offset %101      \n"
      "; CHECK: %89 = OpImageGather %10 %26 %20 %5 ConstOffset %101 \n")
      , 89, true),
    // Test case 7: OpImageWrite with constant Offset
    InstructionFoldingCase<bool>(ImageOperandsTestBody(
      "         OpImageWrite %88 %5 %101 Offset %101      \n"
      "; CHECK: OpImageWrite %88 %5 %101 ConstOffset %101 \n")
      , 0 /* No result-id */, true),
    // Test case 8: OpImageFetch with zero constant Offset
    InstructionFoldingCase<bool>(ImageOperandsTestBody(
        "         %89 = OpImageFetch %10 %88 %101 Lod|Offset %5 %110      \n"
        "; CHECK: %89 = OpImageFetch %10 %88 %101 Lod %5 \n")
        , 89, true)
));

}  // namespace
}  // namespace opt
}  // namespace spvtools
