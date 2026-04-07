// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2026 Second State INC

#include "common/errcode.h"
#include "converter.h"
#include "wat/wat_util.h"

#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::string_view_literals;

namespace WasmEdge::WAT {

enum class SimdShape { I8x16, I16x8, I32x4, I64x2, F32x4, F64x2 };

/// Map instruction keyword text to OpCode using enum.inc macros.
static const std::unordered_map<std::string_view, OpCode, Hash::Hash> &
keywordToOpCode() {
  static const std::unordered_map<std::string_view, OpCode, Hash::Hash> Map = {
#define UseOpCode
#define Line(NAME, STRING, HEX) {STRING##sv, OpCode::NAME},
#define Line_FB(NAME, STRING, B1, B2) {STRING##sv, OpCode::NAME},
#define Line_FC(NAME, STRING, B1, B2) {STRING##sv, OpCode::NAME},
#define Line_FD(NAME, STRING, B1, B2) {STRING##sv, OpCode::NAME},
#define Line_FE(NAME, STRING, B1, B2) {STRING##sv, OpCode::NAME},
#include "common/enum.inc"
#undef Line
#undef Line_FB
#undef Line_FC
#undef Line_FD
#undef Line_FE
#undef UseOpCode
      // Manual entries: enum.inc strings contain spaces, won't match WAT
      // keywords
      {"ref.test"sv, OpCode::Ref__test},
      {"ref.cast"sv, OpCode::Ref__cast},
  };
  return Map;
}

static uint32_t naturalAlign(OpCode Code) {
  switch (Code) {
  case OpCode::I32__load:
  case OpCode::I32__store:
  case OpCode::F32__load:
  case OpCode::F32__store:
  case OpCode::I32__atomic__load:
  case OpCode::I32__atomic__store:
  case OpCode::I32__atomic__rmw__add:
  case OpCode::I32__atomic__rmw__sub:
  case OpCode::I32__atomic__rmw__and:
  case OpCode::I32__atomic__rmw__or:
  case OpCode::I32__atomic__rmw__xor:
  case OpCode::I32__atomic__rmw__xchg:
  case OpCode::I32__atomic__rmw__cmpxchg:
  case OpCode::V128__load32_splat:
  case OpCode::V128__load32_zero:
  case OpCode::Memory__atomic__notify:
  case OpCode::Memory__atomic__wait32:
    return 2;

  case OpCode::I64__load:
  case OpCode::I64__store:
  case OpCode::F64__load:
  case OpCode::F64__store:
  case OpCode::I64__atomic__load:
  case OpCode::I64__atomic__store:
  case OpCode::I64__atomic__rmw__add:
  case OpCode::I64__atomic__rmw__sub:
  case OpCode::I64__atomic__rmw__and:
  case OpCode::I64__atomic__rmw__or:
  case OpCode::I64__atomic__rmw__xor:
  case OpCode::I64__atomic__rmw__xchg:
  case OpCode::I64__atomic__rmw__cmpxchg:
  case OpCode::V128__load64_splat:
  case OpCode::V128__load64_zero:
  case OpCode::Memory__atomic__wait64:
  case OpCode::V128__load8x8_s:
  case OpCode::V128__load8x8_u:
  case OpCode::V128__load16x4_s:
  case OpCode::V128__load16x4_u:
  case OpCode::V128__load32x2_s:
  case OpCode::V128__load32x2_u:
    return 3;

  case OpCode::V128__load:
  case OpCode::V128__store:
    return 4;

  case OpCode::I32__load16_s:
  case OpCode::I32__load16_u:
  case OpCode::I64__load16_s:
  case OpCode::I64__load16_u:
  case OpCode::I32__store16:
  case OpCode::I64__store16:
  case OpCode::I32__atomic__load16_u:
  case OpCode::I64__atomic__load16_u:
  case OpCode::I32__atomic__store16:
  case OpCode::I64__atomic__store16:
  case OpCode::I32__atomic__rmw16__add_u:
  case OpCode::I64__atomic__rmw16__add_u:
  case OpCode::I32__atomic__rmw16__sub_u:
  case OpCode::I64__atomic__rmw16__sub_u:
  case OpCode::I32__atomic__rmw16__and_u:
  case OpCode::I64__atomic__rmw16__and_u:
  case OpCode::I32__atomic__rmw16__or_u:
  case OpCode::I64__atomic__rmw16__or_u:
  case OpCode::I32__atomic__rmw16__xor_u:
  case OpCode::I64__atomic__rmw16__xor_u:
  case OpCode::I32__atomic__rmw16__xchg_u:
  case OpCode::I64__atomic__rmw16__xchg_u:
  case OpCode::I32__atomic__rmw16__cmpxchg_u:
  case OpCode::I64__atomic__rmw16__cmpxchg_u:
  case OpCode::V128__load16_splat:
    return 1;

  case OpCode::I64__load32_s:
  case OpCode::I64__load32_u:
  case OpCode::I64__store32:
  case OpCode::I64__atomic__load32_u:
  case OpCode::I64__atomic__store32:
  case OpCode::I64__atomic__rmw32__add_u:
  case OpCode::I64__atomic__rmw32__sub_u:
  case OpCode::I64__atomic__rmw32__and_u:
  case OpCode::I64__atomic__rmw32__or_u:
  case OpCode::I64__atomic__rmw32__xor_u:
  case OpCode::I64__atomic__rmw32__xchg_u:
  case OpCode::I64__atomic__rmw32__cmpxchg_u:
    return 2;

  // 8-bit loads/stores
  case OpCode::I32__load8_s:
  case OpCode::I32__load8_u:
  case OpCode::I64__load8_s:
  case OpCode::I64__load8_u:
  case OpCode::I32__store8:
  case OpCode::I64__store8:
  case OpCode::I32__atomic__load8_u:
  case OpCode::I64__atomic__load8_u:
  case OpCode::I32__atomic__store8:
  case OpCode::I64__atomic__store8:
  case OpCode::I32__atomic__rmw8__add_u:
  case OpCode::I64__atomic__rmw8__add_u:
  case OpCode::I32__atomic__rmw8__sub_u:
  case OpCode::I64__atomic__rmw8__sub_u:
  case OpCode::I32__atomic__rmw8__and_u:
  case OpCode::I64__atomic__rmw8__and_u:
  case OpCode::I32__atomic__rmw8__or_u:
  case OpCode::I64__atomic__rmw8__or_u:
  case OpCode::I32__atomic__rmw8__xor_u:
  case OpCode::I64__atomic__rmw8__xor_u:
  case OpCode::I32__atomic__rmw8__xchg_u:
  case OpCode::I64__atomic__rmw8__xchg_u:
  case OpCode::I32__atomic__rmw8__cmpxchg_u:
  case OpCode::I64__atomic__rmw8__cmpxchg_u:
  case OpCode::V128__load8_splat:
    return 0;

  // SIMD lane load/store
  case OpCode::V128__load8_lane:
  case OpCode::V128__store8_lane:
    return 0;
  case OpCode::V128__load16_lane:
  case OpCode::V128__store16_lane:
    return 1;
  case OpCode::V128__load32_lane:
  case OpCode::V128__store32_lane:
    return 2;
  case OpCode::V128__load64_lane:
  case OpCode::V128__store64_lane:
    return 3;

  default:
    return 0;
  }
}

static uint8_t maxLaneForOp(OpCode Code) {
  switch (Code) {
  case OpCode::V128__load8_lane:
  case OpCode::V128__store8_lane:
    return 16;
  case OpCode::V128__load16_lane:
  case OpCode::V128__store16_lane:
    return 8;
  case OpCode::V128__load32_lane:
  case OpCode::V128__store32_lane:
    return 4;
  case OpCode::V128__load64_lane:
  case OpCode::V128__store64_lane:
    return 2;
  default:
    return 255;
  }
}

// Check if opcode is a SIMD lane load/store that also needs a lane byte.
static bool isSimdLaneMemOp(OpCode Code) {
  switch (Code) {
  case OpCode::V128__load8_lane:
  case OpCode::V128__load16_lane:
  case OpCode::V128__load32_lane:
  case OpCode::V128__load64_lane:
  case OpCode::V128__store8_lane:
  case OpCode::V128__store16_lane:
  case OpCode::V128__store32_lane:
  case OpCode::V128__store64_lane:
    return true;
  default:
    return false;
  }
}

static Expect<uint64_t> parseOffset(std::string_view Text) {
  if (Text.size() > 7 && Text.substr(0, 7) == "offset="sv) {
    Text = Text.substr(7);
  }
  // Negative offset is not valid — treat as unknown operator.
  if (!Text.empty() && Text.front() == '-') {
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
  std::string Cleaned;
  Cleaned.reserve(Text.size());
  for (char C : Text) {
    if (C != '_') {
      Cleaned.push_back(C);
    }
  }
  char *End = nullptr;
  errno = 0;
  uint64_t Val = std::strtoull(Cleaned.c_str(), &End, 0);
  if (errno == ERANGE || End != Cleaned.c_str() + Cleaned.size()) {
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
  return Val;
}

static Expect<uint64_t> checkOffsetU32(uint64_t Off, bool Enforce) {
  if (Enforce &&
      Off > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return Unexpect(ErrCode::Value::WatI32Constant);
  }
  return Off;
}

static Expect<uint32_t> parseAlign(std::string_view Text) {
  if (Text.size() > 6 && Text.substr(0, 6) == "align="sv) {
    Text = Text.substr(6);
  }
  // Negative alignment is not valid — treat as unknown operator.
  if (!Text.empty() && Text.front() == '-') {
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
  std::string Cleaned;
  Cleaned.reserve(Text.size());
  for (char C : Text) {
    if (C != '_') {
      Cleaned.push_back(C);
    }
  }
  char *End = nullptr;
  errno = 0;
  uint64_t Val = std::strtoull(Cleaned.c_str(), &End, 0);
  if (errno == ERANGE || End != Cleaned.c_str() + Cleaned.size()) {
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
  if (Val == 0 || (Val & (Val - 1)) != 0) {
    return Unexpect(ErrCode::Value::WatAlignmentPow2);
  }
  uint32_t Log2 = 0;
  uint64_t V = Val;
  while (V > 1) {
    V >>= 1;
    ++Log2;
  }
  return Log2;
}

void Converter::fixupJumps(AST::InstrVec &Instrs) {
  std::vector<std::pair<OpCode, uint32_t>> BlockStack;

  for (uint32_t I = 0; I < Instrs.size(); ++I) {
    auto Code = Instrs[I].getOpCode();

    if (Code == OpCode::Block || Code == OpCode::Loop || Code == OpCode::If ||
        Code == OpCode::Try_table) {
      BlockStack.emplace_back(Code, I);
    } else if (Code == OpCode::Else) {
      if (!BlockStack.empty()) {
        auto &[BackOp, Pos] = BlockStack.back();
        if (BackOp == OpCode::If) {
          Instrs[Pos].setJumpElse(I - Pos);
        }
      }
    } else if (Code == OpCode::End) {
      if (!BlockStack.empty()) {
        Instrs[I].setExprLast(false);
        auto &[BackOp, Pos] = BlockStack.back();
        if (BackOp == OpCode::Block || BackOp == OpCode::Loop ||
            BackOp == OpCode::If) {
          Instrs[I].setTryBlockLast(false);
          Instrs[Pos].setJumpEnd(I - Pos);
          if (BackOp == OpCode::If) {
            if (Instrs[Pos].getJumpElse() == 0) {
              Instrs[Pos].setJumpElse(I - Pos);
            } else {
              uint32_t ElsePos = Pos + Instrs[Pos].getJumpElse();
              Instrs[ElsePos].setJumpEnd(I - ElsePos);
            }
          }
        } else if (BackOp == OpCode::Try_table) {
          Instrs[I].setTryBlockLast(true);
          Instrs[Pos].getTryCatch().JumpEnd = I - Pos;
        }
        BlockStack.pop_back();
      } else {
        Instrs[I].setExprLast(true);
      }
    }
  }
}

bool Converter::isImmediate(Node Child) const {
  // Sexpr children that are instructions (not structural) are NOT immediates.
  return nodeType(Child) != NodeType::Sexpr;
}

/// Advance cursor past sexprs, returning the next immediate (non-sexpr) node.
/// Returns null if no more immediates remain.
Node Converter::nextImmediate(Cursor &C) const {
  while (C.valid()) {
    Node Child = C.node();
    C.next();
    if (isImmediate(Child)) {
      return Child;
    }
  }
  return Node{};
}

// expr ::= instr*  (appends implicit end)
Expect<void> Converter::convertExpression(Cursor &C, AST::Expression &Expr) {
  auto &Instrs = Expr.getInstrs();
  EXPECTED_TRY(convertMixedInstrSeq(C, Instrs));
  Instrs.emplace_back(OpCode::End);
  Instrs.back().setExprLast(true);
  fixupJumps(Instrs);
  return {};
}

// instr ::= plaininstr | blockinstr | foldedplaininstr | foldedblockinstr
// Processes a mixed sequence of flat (keyword) and folded (sexpr) instructions.
Expect<void> Converter::convertMixedInstrSeq(Cursor &C, AST::InstrVec &Instrs) {
  auto &Map = keywordToOpCode();
  // C iterates named children. They are keywords (flat instructions),
  // numbers/ids (operands), and sexprs (folded instructions or structural).
  // We process both folded (sexpr) and flat (keyword + operand tokens) forms.
  //
  // Block tracking for flat instructions:
  std::vector<size_t> BlockStack; // indices of block/loop/if instrs
  while (C.valid()) {
    Node Child = C.node();
    auto Type = nodeType(Child);
    if (Type == NodeType::Sexpr) {
      C.next();
      Cursor FC(Child);
      if (peekType(FC) == NodeType::Keyword) {
        EXPECTED_TRY(convertFoldedInstr(Child, Instrs));
      }
    } else if (Type == NodeType::Keyword) {
      auto Text = nodeText(Child);
      C.next(); // consume keyword
      // Check if this keyword is a flat instruction opcode.
      // Skip structural keywords and the leading "func"/"global"/etc. keyword.
      static const std::unordered_set<std::string_view, Hash::Hash>
          SkipKeywords = {"func"sv,   "global"sv, "module"sv,
                          "offset"sv, "item"sv,   "then"sv};
      if (SkipKeywords.count(Text)) {
        continue;
      }
      // Handle flat block instructions
      if (Text == "block"sv || Text == "loop"sv) {
        // Optional label: peek at next child
        std::string_view Label;
        if (peekType(C) == NodeType::Id) {
          Label = nodeText(C.node());
          C.next(); // consume label
        }
        Syms.pushLabel(Label);
        OpCode Code = (Text == "block"sv) ? OpCode::Block : OpCode::Loop;
        Instrs.emplace_back(Code);
        auto BlockIdx = Instrs.size() - 1;
        BlockStack.push_back(BlockIdx);
        EXPECTED_TRY(Instrs[BlockIdx].getBlockType(), parseBlockType(C));
        continue;
      }
      if (Text == "if"sv) {
        std::string_view Label;
        if (peekType(C) == NodeType::Id) {
          Label = nodeText(C.node());
          C.next();
        }
        Syms.pushLabel(Label);
        Instrs.emplace_back(OpCode::If);
        auto BlockIdx = Instrs.size() - 1;
        BlockStack.push_back(BlockIdx);
        EXPECTED_TRY(Instrs[BlockIdx].getBlockType(), parseBlockType(C));
        continue;
      }
      if (Text == "else"sv) {
        std::string_view Label;
        if (peekType(C) == NodeType::Id) {
          Label = nodeText(C.node());
          C.next();
        }
        if (!Label.empty()) {
          if (auto Idx = Syms.resolveLabel(Label); !Idx || *Idx != 0) {
            return Unexpect(ErrCode::Value::WatMismatchingLabel);
          }
        }
        Instrs.emplace_back(OpCode::Else);
        continue;
      }
      if (Text == "end"sv) {
        std::string_view Label;
        if (peekType(C) == NodeType::Id) {
          Label = nodeText(C.node());
          C.next();
        }
        if (!Label.empty()) {
          if (auto Idx = Syms.resolveLabel(Label); !Idx || *Idx != 0) {
            return Unexpect(ErrCode::Value::WatMismatchingLabel);
          }
        }
        Instrs.emplace_back(OpCode::End);
        if (!BlockStack.empty()) {
          BlockStack.pop_back();
          Syms.popLabel();
        }
        continue;
      }

      auto It = Map.find(Text);
      if (It == Map.end()) {
        return Unexpect(ErrCode::Value::WatUnknownOperator);
      }
      OpCode Code = It->second;

      // Dispatch to shared handlers.
      // Keyword already consumed; C points to first immediate.
      EXPECTED_TRY(convertPlainInstr(C, Instrs, Code));
    } else {
      break;
    }
  }
  return {};
}

// plaininstr ::= keyword immediate*
// Shared dispatch: both flat and folded paths call this with appropriate
// Cursor. C points to the first immediate after the opcode keyword.
Expect<void> Converter::convertPlainInstr(Cursor &C, AST::InstrVec &Instrs,
                                          OpCode Code) {
  switch (Code) {

  // --- ref.test / ref.cast ---
  case OpCode::Ref__test:
  case OpCode::Ref__cast:
    return convertRefOp(C, Instrs, Code);

  // --- select ---
  case OpCode::Select:
  case OpCode::Select_t:
    return convertSelectOp(C, Instrs);

  // --- Const ops ---
  case OpCode::I32__const:
  case OpCode::I64__const:
  case OpCode::F32__const:
  case OpCode::F64__const:
    if (!C.valid()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    return convertConstOp(C, Instrs, Code);

  // --- Variable ops ---
  case OpCode::Local__get:
  case OpCode::Local__set:
  case OpCode::Local__tee:
  case OpCode::Global__get:
  case OpCode::Global__set:
    if (!C.valid()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    return convertVarOp(C, Instrs, Code);

  // --- Branch ops ---
  case OpCode::Br:
  case OpCode::Br_if:
  case OpCode::Br_on_null:
  case OpCode::Br_on_non_null:
  case OpCode::Br_table:
    return convertBranchOp(C, Instrs, Code);

  // --- Call ops ---
  case OpCode::Call:
  case OpCode::Return_call:
  case OpCode::Call_indirect:
  case OpCode::Return_call_indirect:
  case OpCode::Call_ref:
  case OpCode::Return_call_ref:
    return convertCallOp(C, Instrs, Code);

  // --- Ref ops ---
  case OpCode::Ref__null:
  case OpCode::Ref__func:
  case OpCode::Ref__i31:
  case OpCode::Br_on_cast:
  case OpCode::Br_on_cast_fail:
    return convertRefOp(C, Instrs, Code);

  // --- Table ops ---
  case OpCode::Table__get:
  case OpCode::Table__set:
  case OpCode::Table__size:
  case OpCode::Table__grow:
  case OpCode::Table__fill:
  case OpCode::Table__copy:
  case OpCode::Table__init:
  case OpCode::Elem__drop:
    return convertTableOp(C, Instrs, Code);

  // --- Memory control ---
  case OpCode::Memory__size:
  case OpCode::Memory__grow:
  case OpCode::Memory__fill:
  case OpCode::Memory__copy:
  case OpCode::Memory__init:
  case OpCode::Data__drop:
    return convertMemControlOp(C, Instrs, Code);

  // --- GC ops ---
  case OpCode::Struct__new:
  case OpCode::Struct__new_default:
  case OpCode::Array__new:
  case OpCode::Array__new_default:
  case OpCode::Array__get:
  case OpCode::Array__get_s:
  case OpCode::Array__get_u:
  case OpCode::Array__set:
  case OpCode::Array__fill:
  case OpCode::Struct__get:
  case OpCode::Struct__get_s:
  case OpCode::Struct__get_u:
  case OpCode::Struct__set:
  case OpCode::Array__new_fixed:
  case OpCode::Array__new_elem:
  case OpCode::Array__new_data:
  case OpCode::Array__init_data:
  case OpCode::Array__init_elem:
  case OpCode::Array__copy:
  case OpCode::Throw:
    return convertGCOp(C, Instrs, Code);

  // --- SIMD const ops ---
  case OpCode::V128__const:
  case OpCode::I8x16__shuffle:
    return convertSimdConstOp(C, Instrs, Code);

  // --- Memory load/store ops (including SIMD and atomic) ---
  case OpCode::I32__load:
  case OpCode::I64__load:
  case OpCode::F32__load:
  case OpCode::F64__load:
  case OpCode::I32__load8_s:
  case OpCode::I32__load8_u:
  case OpCode::I32__load16_s:
  case OpCode::I32__load16_u:
  case OpCode::I64__load8_s:
  case OpCode::I64__load8_u:
  case OpCode::I64__load16_s:
  case OpCode::I64__load16_u:
  case OpCode::I64__load32_s:
  case OpCode::I64__load32_u:
  case OpCode::I32__store:
  case OpCode::I64__store:
  case OpCode::F32__store:
  case OpCode::F64__store:
  case OpCode::I32__store8:
  case OpCode::I32__store16:
  case OpCode::I64__store8:
  case OpCode::I64__store16:
  case OpCode::I64__store32:
  case OpCode::V128__load:
  case OpCode::V128__store:
  case OpCode::V128__load8x8_s:
  case OpCode::V128__load8x8_u:
  case OpCode::V128__load16x4_s:
  case OpCode::V128__load16x4_u:
  case OpCode::V128__load32x2_s:
  case OpCode::V128__load32x2_u:
  case OpCode::V128__load8_splat:
  case OpCode::V128__load16_splat:
  case OpCode::V128__load32_splat:
  case OpCode::V128__load64_splat:
  case OpCode::V128__load32_zero:
  case OpCode::V128__load64_zero:
  case OpCode::V128__load8_lane:
  case OpCode::V128__load16_lane:
  case OpCode::V128__load32_lane:
  case OpCode::V128__load64_lane:
  case OpCode::V128__store8_lane:
  case OpCode::V128__store16_lane:
  case OpCode::V128__store32_lane:
  case OpCode::V128__store64_lane:
  case OpCode::Memory__atomic__notify:
  case OpCode::Memory__atomic__wait32:
  case OpCode::Memory__atomic__wait64:
  case OpCode::I32__atomic__load:
  case OpCode::I64__atomic__load:
  case OpCode::I32__atomic__load8_u:
  case OpCode::I32__atomic__load16_u:
  case OpCode::I64__atomic__load8_u:
  case OpCode::I64__atomic__load16_u:
  case OpCode::I64__atomic__load32_u:
  case OpCode::I32__atomic__store:
  case OpCode::I64__atomic__store:
  case OpCode::I32__atomic__store8:
  case OpCode::I32__atomic__store16:
  case OpCode::I64__atomic__store8:
  case OpCode::I64__atomic__store16:
  case OpCode::I64__atomic__store32:
  case OpCode::I32__atomic__rmw__add:
  case OpCode::I64__atomic__rmw__add:
  case OpCode::I32__atomic__rmw8__add_u:
  case OpCode::I32__atomic__rmw16__add_u:
  case OpCode::I64__atomic__rmw8__add_u:
  case OpCode::I64__atomic__rmw16__add_u:
  case OpCode::I64__atomic__rmw32__add_u:
  case OpCode::I32__atomic__rmw__sub:
  case OpCode::I64__atomic__rmw__sub:
  case OpCode::I32__atomic__rmw8__sub_u:
  case OpCode::I32__atomic__rmw16__sub_u:
  case OpCode::I64__atomic__rmw8__sub_u:
  case OpCode::I64__atomic__rmw16__sub_u:
  case OpCode::I64__atomic__rmw32__sub_u:
  case OpCode::I32__atomic__rmw__and:
  case OpCode::I64__atomic__rmw__and:
  case OpCode::I32__atomic__rmw8__and_u:
  case OpCode::I32__atomic__rmw16__and_u:
  case OpCode::I64__atomic__rmw8__and_u:
  case OpCode::I64__atomic__rmw16__and_u:
  case OpCode::I64__atomic__rmw32__and_u:
  case OpCode::I32__atomic__rmw__or:
  case OpCode::I64__atomic__rmw__or:
  case OpCode::I32__atomic__rmw8__or_u:
  case OpCode::I32__atomic__rmw16__or_u:
  case OpCode::I64__atomic__rmw8__or_u:
  case OpCode::I64__atomic__rmw16__or_u:
  case OpCode::I64__atomic__rmw32__or_u:
  case OpCode::I32__atomic__rmw__xor:
  case OpCode::I64__atomic__rmw__xor:
  case OpCode::I32__atomic__rmw8__xor_u:
  case OpCode::I32__atomic__rmw16__xor_u:
  case OpCode::I64__atomic__rmw8__xor_u:
  case OpCode::I64__atomic__rmw16__xor_u:
  case OpCode::I64__atomic__rmw32__xor_u:
  case OpCode::I32__atomic__rmw__xchg:
  case OpCode::I64__atomic__rmw__xchg:
  case OpCode::I32__atomic__rmw8__xchg_u:
  case OpCode::I32__atomic__rmw16__xchg_u:
  case OpCode::I64__atomic__rmw8__xchg_u:
  case OpCode::I64__atomic__rmw16__xchg_u:
  case OpCode::I64__atomic__rmw32__xchg_u:
  case OpCode::I32__atomic__rmw__cmpxchg:
  case OpCode::I64__atomic__rmw__cmpxchg:
  case OpCode::I32__atomic__rmw8__cmpxchg_u:
  case OpCode::I32__atomic__rmw16__cmpxchg_u:
  case OpCode::I64__atomic__rmw8__cmpxchg_u:
  case OpCode::I64__atomic__rmw16__cmpxchg_u:
  case OpCode::I64__atomic__rmw32__cmpxchg_u:
    return convertMemLoadStoreOp(C, Instrs, Code);

  // --- SIMD lane ops (extract/replace) ---
  case OpCode::I8x16__extract_lane_s:
  case OpCode::I8x16__extract_lane_u:
  case OpCode::I8x16__replace_lane:
  case OpCode::I16x8__extract_lane_s:
  case OpCode::I16x8__extract_lane_u:
  case OpCode::I16x8__replace_lane:
  case OpCode::I32x4__extract_lane:
  case OpCode::I32x4__replace_lane:
  case OpCode::I64x2__extract_lane:
  case OpCode::I64x2__replace_lane:
  case OpCode::F32x4__extract_lane:
  case OpCode::F32x4__replace_lane:
  case OpCode::F64x2__extract_lane:
  case OpCode::F64x2__replace_lane:
    return convertSimdLaneOp(C, Instrs, Code);

  // --- Atomic fence ---
  case OpCode::Atomic__fence:
    Instrs.emplace_back(Code);
    return {};

  // --- Default: no immediates ---
  default:
    Instrs.emplace_back(Code);
    return {};
  }
}

// --- convertSelectOp ---
// select | select (result valtype+)
Expect<void> Converter::convertSelectOp(Cursor &C, AST::InstrVec &Instrs) {
  std::vector<ValType> ResultTypes;
  // Consume consecutive (result ...) sexprs
  while (C.valid()) {
    Node Child = C.node();
    if (!sexprMatch(C, "result"sv)) {
      break;
    }
    Cursor RC(Child);
    RC.next(); // skip "result" keyword
    while (RC.valid()) {
      EXPECTED_TRY(auto VT, convertValType(RC.node()));
      ResultTypes.push_back(VT);
      RC.next();
    }
    C.next();
  }
  if (ResultTypes.empty()) {
    Instrs.emplace_back(OpCode::Select);
  } else {
    Instrs.emplace_back(OpCode::Select_t);
    auto &Instr = Instrs.back();
    Instr.setValTypeListSize(static_cast<uint32_t>(ResultTypes.size()));
    auto List = Instr.getValTypeList();
    for (uint32_t I = 0; I < ResultTypes.size(); ++I) {
      List[I] = ResultTypes[I];
    }
  }
  return {};
}

// --- convertConstOp ---
// i32.const i32 | i64.const i64 | f32.const f32 | f64.const f64
Expect<void> Converter::convertConstOp(Cursor &C, AST::InstrVec &Instrs,
                                       OpCode Code) {
  auto Child = C.node();
  C.next();
  auto Type = nodeType(Child);
  Instrs.emplace_back(Code);
  switch (Code) {
  case OpCode::I32__const: {
    if (Type != NodeType::U && Type != NodeType::S) {
      if (Type == WasmEdge::WAT::NodeType::Keyword) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
    EXPECTED_TRY(auto Val, parseInt(nodeText(Child)));
    if (Val < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        Val > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
      return Unexpect(ErrCode::Value::WatConstantOutOfRange);
    }
    Instrs.back().setNum(
        ValVariant(static_cast<uint32_t>(static_cast<int32_t>(Val))));
    return {};
  }
  case OpCode::I64__const: {
    if (Type != NodeType::U && Type != NodeType::S) {
      if (Type == WasmEdge::WAT::NodeType::Keyword) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
    EXPECTED_TRY(auto Val, parseInt(nodeText(Child)));
    Instrs.back().setNum(ValVariant(static_cast<uint64_t>(Val)));
    return {};
  }
  case OpCode::F32__const: {
    if (Type != NodeType::U && Type != NodeType::S && Type != NodeType::F) {
      if (nodeText(Child) == "nan:canonical"sv ||
          nodeText(Child) == "nan:arithmetic"sv) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
    EXPECTED_TRY(auto Val, parseF32(nodeText(Child)));
    Instrs.back().setNum(ValVariant(Val));
    return {};
  }
  case OpCode::F64__const: {
    if (Type != NodeType::U && Type != NodeType::S && Type != NodeType::F) {
      if (Type == WasmEdge::WAT::NodeType::Keyword) {
        if (nodeText(Child) == "nan:canonical"sv ||
            nodeText(Child) == "nan:arithmetic"sv) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
      }
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
    EXPECTED_TRY(auto Val, parseF64(nodeText(Child)));
    Instrs.back().setNum(ValVariant(Val));
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
}

// --- convertVarOp ---
// local.get localidx | local.set localidx | local.tee localidx
// global.get globalidx | global.set globalidx
Expect<void> Converter::convertVarOp(Cursor &C, AST::InstrVec &Instrs,
                                     OpCode Code) {
  Instrs.emplace_back(Code);
  auto Child = C.node();
  C.next();
  switch (Code) {
  case OpCode::Local__get:
  case OpCode::Local__set:
  case OpCode::Local__tee: {
    EXPECTED_TRY(auto Idx,
                 Syms.resolve(SymbolTable::IndexSpace::Local, nodeText(Child)));
    Instrs.back().getTargetIndex() = Idx;
    return {};
  }
  case OpCode::Global__get:
  case OpCode::Global__set: {
    EXPECTED_TRY(auto Idx, Syms.resolve(SymbolTable::IndexSpace::Global,
                                        nodeText(Child)));
    Instrs.back().getTargetIndex() = Idx;
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
}

// --- convertBranchOp ---
// br labelidx | br_if labelidx | br_on_null labelidx | br_on_non_null labelidx
// br_table labelidx+ labelidx
Expect<void> Converter::convertBranchOp(Cursor &C, AST::InstrVec &Instrs,
                                        OpCode Code) {
  switch (Code) {
  // br labelidx | br_if labelidx | br_on_null labelidx
  case OpCode::Br:
  case OpCode::Br_if:
  case OpCode::Br_on_null:
  case OpCode::Br_on_non_null: {
    Node Imm = nextImmediate(C);
    if (Imm.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    Instrs.emplace_back(Code);
    EXPECTED_TRY(auto Idx, Syms.resolveLabel(nodeText(Imm)));
    Instrs.back().getJump().TargetIndex = Idx;
    return {};
  }
  // br_table labelidx+ labelidx
  case OpCode::Br_table: {
    std::vector<uint32_t> Labels;
    for (Node Imm = nextImmediate(C); !Imm.isNull(); Imm = nextImmediate(C)) {
      EXPECTED_TRY(auto Idx, Syms.resolveLabel(nodeText(Imm)));
      Labels.push_back(Idx);
    }
    Instrs.emplace_back(Code);
    auto &Instr = Instrs.back();
    Instr.setLabelListSize(static_cast<uint32_t>(Labels.size()));
    auto List = Instr.getLabelList();
    for (uint32_t I = 0; I < Labels.size(); ++I) {
      List[I].TargetIndex = Labels[I];
    }
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
}

// --- convertCallOp ---
// call funcidx | return_call funcidx
// call_indirect tableidx? typeuse | return_call_indirect tableidx? typeuse
// call_ref typeidx | return_call_ref typeidx
Expect<void> Converter::convertCallOp(Cursor &C, AST::InstrVec &Instrs,
                                      OpCode Code) {
  switch (Code) {
  case OpCode::Call:
  case OpCode::Return_call: {
    Node Imm = nextImmediate(C);
    if (Imm.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    Instrs.emplace_back(Code);
    EXPECTED_TRY(auto Idx,
                 Syms.resolve(SymbolTable::IndexSpace::Func, nodeText(Imm)));
    Instrs.back().getTargetIndex() = Idx;
    return {};
  }
  case OpCode::Call_indirect:
  case OpCode::Return_call_indirect: {
    uint32_t TableIdx = 0;
    if (auto Type = peekType(C); Type == NodeType::Id || Type == NodeType::U) {
      // Look ahead: if the next node after this is a type-use sexpr,
      // then this token is a table index.
      auto Ahead = C.copy();
      Ahead.next();
      bool IsTable = false;
      if (peekType(Ahead) == NodeType::Sexpr) {
        Cursor FC(Ahead.node());
        auto KW =
            peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
        if (KW == "type"sv || KW == "param"sv || KW == "result"sv) {
          IsTable = true;
        }
      }
      if (IsTable) {
        EXPECTED_TRY(TableIdx, Syms.resolve(SymbolTable::IndexSpace::Table,
                                            nodeText(C.node())));
        C.next();
      }
    }
    EXPECTED_TRY(auto TypeIdx,
                 resolveTypeUse(C, *CurMod, true, /*AcceptParamId=*/false));
    Instrs.emplace_back(Code);
    Instrs.back().getTargetIndex() = TypeIdx;
    Instrs.back().getSourceIndex() = TableIdx;
    return {};
  }
  case OpCode::Call_ref:
  case OpCode::Return_call_ref: {
    Node Child = nextImmediate(C);
    if (Child.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    Instrs.emplace_back(Code);
    EXPECTED_TRY(auto Idx, Syms.resolveType(nodeText(Child)));
    Instrs.back().getTargetIndex() = Idx;
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
}

// --- convertRefOp ---
// ref.test heaptype | ref.cast heaptype
// ref.null heaptype | ref.func funcidx | ref.i31
// br_on_cast labelidx reftype reftype
// br_on_cast_fail labelidx reftype reftype
Expect<void> Converter::convertRefOp(Cursor &C, AST::InstrVec &Instrs,
                                     OpCode Code) {
  // Helper: check if a ValType encodes a nullable reference.
  auto isNullableRef = [](ValType VT) -> bool {
    auto TC = VT.getCode();
    return TC != TypeCode::Ref;
  };

  // ref.test reftype | ref.cast reftype
  if (Code == OpCode::Ref__test || Code == OpCode::Ref__cast) {
    // Find the type operand — first non-opcode child (keyword, sexpr(ref), or
    // id)
    Node Imm;
    auto Scan = C.copy();
    while (Scan.valid()) {
      Node Child = Scan.node();
      auto CT = nodeType(Child);
      if (sexprMatch(Scan, "ref"sv)) {
        Imm = Child;
        break;
      }
      if (CT == NodeType::Keyword || CT == NodeType::Id || CT == NodeType::U) {
        Imm = Child;
        break;
      }
      Scan.next();
    }
    if (Imm.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    EXPECTED_TRY(auto VT, convertRefType(Imm));
    bool IsNull = isNullableRef(VT);
    OpCode RC;
    if (Code == OpCode::Ref__test) {
      RC = IsNull ? OpCode::Ref__test_null : OpCode::Ref__test;
    } else {
      RC = IsNull ? OpCode::Ref__cast_null : OpCode::Ref__cast;
    }
    Instrs.emplace_back(RC);
    Instrs.back().setValType(VT);
    return {};
  }

  switch (Code) {
  // ref.null heaptype
  case OpCode::Ref__null: {
    Node Imm = nextImmediate(C);
    if (Imm.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    Instrs.emplace_back(Code);
    EXPECTED_TRY(auto VT, convertHeapType(Imm));
    Instrs.back().setValType(VT);
    return {};
  }
  // ref.func funcidx
  case OpCode::Ref__func: {
    Node Imm = nextImmediate(C);
    if (Imm.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    Instrs.emplace_back(Code);
    EXPECTED_TRY(auto Idx,
                 Syms.resolve(SymbolTable::IndexSpace::Func, nodeText(Imm)));
    Instrs.back().getTargetIndex() = Idx;
    return {};
  }
  // ref.i31
  case OpCode::Ref__i31: {
    Instrs.emplace_back(Code);
    return {};
  }
  // br_on_cast labelidx reftype reftype
  // br_on_cast_fail labelidx reftype reftype
  case OpCode::Br_on_cast:
  case OpCode::Br_on_cast_fail: {
    // Collect non-sub-instruction immediates: labelidx + 2 reftypes
    // reftypes can be keywords, (ref ...) sexprs, or type indices
    uint32_t LabelIdx = 0;
    ValType RT1{}, RT2{};
    int RefTypeCount = 0;
    auto Scan = C.copy();
    while (Scan.valid()) {
      Node Child = Scan.node();
      Scan.next();
      auto CType = nodeType(Child);
      // Skip sub-expression sexprs (folded operands)
      if (CType == NodeType::Sexpr) {
        Cursor FC(Child);
        auto KW =
            peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
        if (KW == "ref"sv) {
          // (ref null? heaptype) — type operand
          EXPECTED_TRY(auto VT, convertRefType(Child));
          (RefTypeCount == 0 ? RT1 : RT2) = VT;
          ++RefTypeCount;
        }
        continue; // skip all other sexprs (sub-instructions)
      }
      if ((CType == NodeType::Id || CType == NodeType::U) &&
          RefTypeCount == 0) {
        EXPECTED_TRY(LabelIdx, Syms.resolveLabel(nodeText(Child)));
      } else if (CType == NodeType::Keyword) {
        EXPECTED_TRY(auto VT, convertRefType(Child));
        (RefTypeCount == 0 ? RT1 : RT2) = VT;
        ++RefTypeCount;
      }
    }
    Instrs.emplace_back(Code);
    Instrs.back().setBrCast(LabelIdx);
    Instrs.back().getBrCast().RType1 = RT1;
    Instrs.back().getBrCast().RType2 = RT2;
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
}

// --- convertTableOp ---
// table.get tableidx? | table.set tableidx? | table.size tableidx?
// table.grow tableidx? | table.fill tableidx?
// table.copy tableidx? tableidx? | table.init tableidx? elemidx
// elem.drop elemidx
Expect<void> Converter::convertTableOp(Cursor &C, AST::InstrVec &Instrs,
                                       OpCode Code) {
  switch (Code) {
  // table.{get,set,size,grow,fill} tableidx?
  case OpCode::Table__get:
  case OpCode::Table__set:
  case OpCode::Table__size:
  case OpCode::Table__grow:
  case OpCode::Table__fill: {
    uint32_t TableIdx = 0;
    if (Node Imm = nextImmediate(C); !Imm.isNull()) {
      EXPECTED_TRY(TableIdx,
                   Syms.resolve(SymbolTable::IndexSpace::Table, nodeText(Imm)));
    }
    Instrs.emplace_back(Code);
    Instrs.back().getTargetIndex() = TableIdx;
    return {};
  }
  // table.copy tableidx? tableidx?
  case OpCode::Table__copy: {
    uint32_t DstIdx = 0, SrcIdx = 0;
    Node Imm0 = nextImmediate(C);
    Node Imm1 = nextImmediate(C);
    if (!Imm0.isNull() && !Imm1.isNull()) {
      EXPECTED_TRY(
          DstIdx, Syms.resolve(SymbolTable::IndexSpace::Table, nodeText(Imm0)));
      EXPECTED_TRY(
          SrcIdx, Syms.resolve(SymbolTable::IndexSpace::Table, nodeText(Imm1)));
    } else if (!Imm0.isNull()) {
      EXPECTED_TRY(
          DstIdx, Syms.resolve(SymbolTable::IndexSpace::Table, nodeText(Imm0)));
    }
    Instrs.emplace_back(Code);
    Instrs.back().getTargetIndex() = DstIdx;
    Instrs.back().getSourceIndex() = SrcIdx;
    return {};
  }
  // table.init tableidx? elemidx
  case OpCode::Table__init: {
    uint32_t TableIdx = 0, ElemIdx = 0;
    Node Imm0 = nextImmediate(C);
    Node Imm1 = nextImmediate(C);
    if (!Imm0.isNull() && !Imm1.isNull()) {
      EXPECTED_TRY(TableIdx, Syms.resolve(SymbolTable::IndexSpace::Table,
                                          nodeText(Imm0)));
      EXPECTED_TRY(ElemIdx,
                   Syms.resolve(SymbolTable::IndexSpace::Elem, nodeText(Imm1)));
    } else if (!Imm0.isNull()) {
      EXPECTED_TRY(ElemIdx,
                   Syms.resolve(SymbolTable::IndexSpace::Elem, nodeText(Imm0)));
    }
    Instrs.emplace_back(Code);
    Instrs.back().getSourceIndex() = ElemIdx;
    Instrs.back().getTargetIndex() = TableIdx;
    return {};
  }
  case OpCode::Elem__drop: {
    Instrs.emplace_back(Code);
    if (Node Imm = nextImmediate(C); !Imm.isNull()) {
      EXPECTED_TRY(auto Idx,
                   Syms.resolve(SymbolTable::IndexSpace::Elem, nodeText(Imm)));
      Instrs.back().getTargetIndex() = Idx;
    }
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
}

// --- convertMemControlOp ---
// memory.size memidx? | memory.grow memidx? | memory.fill memidx?
// memory.copy memidx? memidx? | memory.init memidx? dataidx
// data.drop dataidx
Expect<void> Converter::convertMemControlOp(Cursor &C, AST::InstrVec &Instrs,
                                            OpCode Code) {
  switch (Code) {
  // memory.{size,grow,fill} memidx?
  case OpCode::Memory__size:
  case OpCode::Memory__grow:
  case OpCode::Memory__fill: {
    Instrs.emplace_back(Code);
    if (Node Imm = nextImmediate(C); !Imm.isNull()) {
      EXPECTED_TRY(auto Idx, Syms.resolve(SymbolTable::IndexSpace::Memory,
                                          nodeText(Imm)));
      Instrs.back().getTargetIndex() = Idx;
    }
    return {};
  }
  // memory.copy memidx? memidx?
  case OpCode::Memory__copy: {
    Instrs.emplace_back(Code);
    if (Node Imm = nextImmediate(C); !Imm.isNull()) {
      EXPECTED_TRY(auto Idx, Syms.resolve(SymbolTable::IndexSpace::Memory,
                                          nodeText(Imm)));
      Instrs.back().getTargetIndex() = Idx;
    }
    if (Node Imm = nextImmediate(C); !Imm.isNull()) {
      EXPECTED_TRY(auto Idx, Syms.resolve(SymbolTable::IndexSpace::Memory,
                                          nodeText(Imm)));
      Instrs.back().getSourceIndex() = Idx;
    }
    return {};
  }
  // memory.init memidx? dataidx
  case OpCode::Memory__init: {
    uint32_t MemIdx = 0, DataIdx = 0;
    Node Imm0 = nextImmediate(C);
    Node Imm1 = nextImmediate(C);
    if (!Imm0.isNull() && !Imm1.isNull()) {
      EXPECTED_TRY(MemIdx, Syms.resolve(SymbolTable::IndexSpace::Memory,
                                        nodeText(Imm0)));
      EXPECTED_TRY(DataIdx,
                   Syms.resolve(SymbolTable::IndexSpace::Data, nodeText(Imm1)));
    } else if (!Imm0.isNull()) {
      EXPECTED_TRY(DataIdx,
                   Syms.resolve(SymbolTable::IndexSpace::Data, nodeText(Imm0)));
    }
    Instrs.emplace_back(Code);
    Instrs.back().getSourceIndex() = DataIdx;
    Instrs.back().getTargetIndex() = MemIdx;
    return {};
  }
  // data.drop dataidx
  case OpCode::Data__drop: {
    Instrs.emplace_back(Code);
    if (Node Imm = nextImmediate(C); !Imm.isNull()) {
      EXPECTED_TRY(auto Idx,
                   Syms.resolve(SymbolTable::IndexSpace::Data, nodeText(Imm)));
      Instrs.back().getTargetIndex() = Idx;
    }
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
}

// --- convertGCOp ---
// struct.new typeidx | struct.new_default typeidx
// struct.get typeidx fieldidx | struct.set typeidx fieldidx
// array.new typeidx | array.new_default typeidx | array.new_fixed typeidx u32
// array.new_elem typeidx elemidx | array.new_data typeidx dataidx
// array.init_elem typeidx elemidx | array.init_data typeidx dataidx
// array.get typeidx | array.set typeidx | array.fill typeidx
// array.copy typeidx typeidx | throw tagidx
Expect<void> Converter::convertGCOp(Cursor &C, AST::InstrVec &Instrs,
                                    OpCode Code) {
  switch (Code) {
  // {struct,array}.{new,new_default,get,get_s,get_u,set,fill} typeidx
  case OpCode::Struct__new:
  case OpCode::Struct__new_default:
  case OpCode::Array__new:
  case OpCode::Array__new_default:
  case OpCode::Array__get:
  case OpCode::Array__get_s:
  case OpCode::Array__get_u:
  case OpCode::Array__set:
  case OpCode::Array__fill: {
    Node Imm = nextImmediate(C);
    EXPECTED_TRY(auto Idx, Syms.resolveType(nodeText(Imm)));
    Instrs.emplace_back(Code);
    Instrs.back().getTargetIndex() = Idx;
    return {};
  }
  // struct.get{_s,_u} typeidx fieldidx | struct.set typeidx fieldidx
  case OpCode::Struct__get:
  case OpCode::Struct__get_s:
  case OpCode::Struct__get_u:
  case OpCode::Struct__set: {
    Node Imm0 = nextImmediate(C);
    Node Imm1 = nextImmediate(C);
    if (Imm0.isNull() || Imm1.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    EXPECTED_TRY(auto TypeIdx, Syms.resolveType(nodeText(Imm0)));
    EXPECTED_TRY(auto FieldIdx, Syms.resolveField(TypeIdx, nodeText(Imm1)));
    Instrs.emplace_back(Code);
    Instrs.back().getTargetIndex() = TypeIdx;
    Instrs.back().getSourceIndex() = FieldIdx;
    return {};
  }
  // array.new_fixed typeidx u32
  case OpCode::Array__new_fixed: {
    Node Imm0 = nextImmediate(C);
    Node Imm1 = nextImmediate(C);
    if (Imm0.isNull() || Imm1.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    EXPECTED_TRY(auto TypeIdx, Syms.resolveType(nodeText(Imm0)));
    EXPECTED_TRY(auto Count, parseUint(nodeText(Imm1)));
    Instrs.emplace_back(Code);
    Instrs.back().getTargetIndex() = TypeIdx;
    Instrs.back().getSourceIndex() = static_cast<uint32_t>(Count);
    return {};
  }
  // array.new_elem typeidx elemidx | array.init_elem typeidx elemidx
  case OpCode::Array__new_elem:
  case OpCode::Array__init_elem: {
    Node Imm0 = nextImmediate(C);
    Node Imm1 = nextImmediate(C);
    if (Imm0.isNull() || Imm1.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    EXPECTED_TRY(auto TypeIdx, Syms.resolveType(nodeText(Imm0)));
    EXPECTED_TRY(auto ElemIdx,
                 Syms.resolve(SymbolTable::IndexSpace::Elem, nodeText(Imm1)));
    Instrs.emplace_back(Code);
    Instrs.back().getTargetIndex() = TypeIdx;
    Instrs.back().getSourceIndex() = ElemIdx;
    return {};
  }
  // array.new_data typeidx dataidx | array.init_data typeidx dataidx
  case OpCode::Array__new_data:
  case OpCode::Array__init_data: {
    Node Imm0 = nextImmediate(C);
    Node Imm1 = nextImmediate(C);
    if (Imm0.isNull() || Imm1.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    EXPECTED_TRY(auto TypeIdx, Syms.resolveType(nodeText(Imm0)));
    EXPECTED_TRY(auto DataIdx,
                 Syms.resolve(SymbolTable::IndexSpace::Data, nodeText(Imm1)));
    Instrs.emplace_back(Code);
    Instrs.back().getTargetIndex() = TypeIdx;
    Instrs.back().getSourceIndex() = DataIdx;
    return {};
  }
  // array.copy typeidx typeidx
  case OpCode::Array__copy: {
    Node Imm0 = nextImmediate(C);
    Node Imm1 = nextImmediate(C);
    if (Imm0.isNull() || Imm1.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    EXPECTED_TRY(auto DstTypeIdx, Syms.resolveType(nodeText(Imm0)));
    EXPECTED_TRY(auto SrcTypeIdx, Syms.resolveType(nodeText(Imm1)));
    Instrs.emplace_back(Code);
    Instrs.back().getTargetIndex() = DstTypeIdx;
    Instrs.back().getSourceIndex() = SrcTypeIdx;
    return {};
  }
  // throw tagidx
  case OpCode::Throw: {
    Node Imm = nextImmediate(C);
    Instrs.emplace_back(Code);
    EXPECTED_TRY(auto Idx,
                 Syms.resolve(SymbolTable::IndexSpace::Tag, nodeText(Imm)));
    Instrs.back().getTargetIndex() = Idx;
    return {};
  }
  default:
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
}

// --- convertSimdConstOp ---
// v128.const shape lane+ | i8x16.shuffle laneidx{16}
Expect<void> Converter::convertSimdConstOp(Cursor &C, AST::InstrVec &Instrs,
                                           OpCode Code) {
  switch (Code) {
  // v128.const shape lane+
  case OpCode::V128__const: {
    Instrs.emplace_back(Code);

    // First immediate is the shape keyword (i8x16, i16x8, etc.)
    static const std::unordered_map<std::string_view, SimdShape, Hash::Hash>
        ShapeMap = {
            {"i8x16"sv, SimdShape::I8x16}, {"i16x8"sv, SimdShape::I16x8},
            {"i32x4"sv, SimdShape::I32x4}, {"i64x2"sv, SimdShape::I64x2},
            {"f32x4"sv, SimdShape::F32x4}, {"f64x2"sv, SimdShape::F64x2},
        };
    Node ShapeNode = nextImmediate(C);
    if (ShapeNode.isNull()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    auto ShapeIt = ShapeMap.find(nodeText(ShapeNode));
    if (ShapeIt == ShapeMap.end()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    SimdShape Shape = ShapeIt->second;

    // Remaining immediates are lane values (continue from Cursor)
    std::vector<std::string_view> Nums;
    for (Node Imm = nextImmediate(C); !Imm.isNull(); Imm = nextImmediate(C)) {
      auto CType = nodeType(Imm);
      if (CType == NodeType::U || CType == NodeType::S ||
          CType == NodeType::F) {
        Nums.push_back(nodeText(Imm));
      } else {
        return Unexpect(ErrCode::Value::WatUnknownOperator);
      }
    }

    uint128_t Val{};
    uint8_t *Bytes = reinterpret_cast<uint8_t *>(&Val);

    switch (Shape) {
    case SimdShape::I8x16:
      if (Nums.size() != 16) {
        return Unexpect(ErrCode::Value::WatWrongLaneCount);
      }
      for (uint32_t I = 0; I < 16; ++I) {
        EXPECTED_TRY(auto V, parseInt(Nums[I]));
        if (V < -128 || V > 255) {
          return Unexpect(ErrCode::Value::WatConstantOutOfRange);
        }
        Bytes[I] = static_cast<uint8_t>(V);
      }
      break;
    case SimdShape::I16x8:
      if (Nums.size() != 8) {
        return Unexpect(ErrCode::Value::WatWrongLaneCount);
      }
      for (uint32_t I = 0; I < 8; ++I) {
        EXPECTED_TRY(auto V, parseInt(Nums[I]));
        if (V < -32768 || V > 65535) {
          return Unexpect(ErrCode::Value::WatConstantOutOfRange);
        }
        uint16_t U = static_cast<uint16_t>(V);
        std::memcpy(Bytes + I * 2, &U, 2);
      }
      break;
    case SimdShape::I32x4:
      if (Nums.size() != 4) {
        return Unexpect(ErrCode::Value::WatWrongLaneCount);
      }
      for (uint32_t I = 0; I < 4; ++I) {
        EXPECTED_TRY(auto V, parseInt(Nums[I]));
        if (V < -2147483648LL || V > 4294967295LL) {
          return Unexpect(ErrCode::Value::WatConstantOutOfRange);
        }
        uint32_t U = static_cast<uint32_t>(V);
        std::memcpy(Bytes + I * 4, &U, 4);
      }
      break;
    case SimdShape::I64x2:
      if (Nums.size() != 2) {
        return Unexpect(ErrCode::Value::WatWrongLaneCount);
      }
      for (uint32_t I = 0; I < 2; ++I) {
        if (auto R = parseInt(Nums[I])) {
          uint64_t U = static_cast<uint64_t>(*R);
          std::memcpy(Bytes + I * 8, &U, 8);
        } else if (auto R2 = parseUint(Nums[I])) {
          uint64_t U = *R2;
          std::memcpy(Bytes + I * 8, &U, 8);
        } else {
          return Unexpect(R.error());
        }
      }
      break;
    case SimdShape::F32x4:
      if (Nums.size() != 4) {
        return Unexpect(ErrCode::Value::WatWrongLaneCount);
      }
      for (uint32_t I = 0; I < 4; ++I) {
        EXPECTED_TRY(auto V, parseF32(Nums[I]));
        std::memcpy(Bytes + I * 4, &V, 4);
      }
      break;
    case SimdShape::F64x2:
      if (Nums.size() != 2) {
        return Unexpect(ErrCode::Value::WatWrongLaneCount);
      }
      for (uint32_t I = 0; I < 2; ++I) {
        EXPECTED_TRY(auto V, parseF64(Nums[I]));
        std::memcpy(Bytes + I * 8, &V, 8);
      }
      break;
    }

    Instrs.back().setNum(ValVariant(Val));
    return {};
  }

  // i8x16.shuffle laneidx{16}
  case OpCode::I8x16__shuffle: {
    Instrs.emplace_back(Code);
    uint128_t Val{};
    uint8_t *Bytes = reinterpret_cast<uint8_t *>(&Val);
    for (uint32_t Idx = 0; Idx < 16; ++Idx) {
      Node Imm = nextImmediate(C);
      if (Imm.isNull()) {
        return Unexpect(ErrCode::Value::WatInvalidLaneLength);
      }
      auto ImmType = nodeType(Imm);
      if (ImmType == NodeType::F || ImmType == NodeType::S) {
        return Unexpect(ErrCode::Value::WatI8ConstOutOfRange);
      }
      EXPECTED_TRY(auto R, parseUint(nodeText(Imm)));
      if (R > 255) {
        return Unexpect(ErrCode::Value::WatI8ConstOutOfRange);
      }
      if (R > 31) {
        return Unexpect(ErrCode::Value::InvalidLaneIdx);
      }
      Bytes[Idx] = static_cast<uint8_t>(R);
    }
    // Check for extra lane indices after the expected 16.
    if (!nextImmediate(C).isNull()) {
      return Unexpect(ErrCode::Value::WatInvalidLaneLength);
    }
    Instrs.back().setNum(ValVariant(Val));
    return {};
  }

  default:
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
}

// --- convertMemLoadStoreOp ---
// {i32,i64,f32,f64,v128}.{load,store}* memidx? offset=u? align=u? laneidx?
Expect<void> Converter::convertMemLoadStoreOp(Cursor &C, AST::InstrVec &Instrs,
                                              OpCode Code) {
  uint32_t MemIdx = 0;
  uint64_t Offset = 0;
  uint32_t Align = naturalAlign(Code);
  uint8_t Lane = 0;

  // Collect immediates: memidx?, offset=N?, align=N?, laneidx?
  // Stop at keywords that are instruction opcodes (flat form boundary).
  std::vector<Node> Imms;
  while (C.valid()) {
    Node Child = C.node();
    if (nodeType(Child) == NodeType::Sexpr) {
      break; // sub-instruction or non-immediate
    }
    if (nodeType(Child) == NodeType::Keyword) {
      auto Text = nodeText(Child);
      if (Text.substr(0, 7) != "offset="sv && Text.substr(0, 6) != "align="sv) {
        break; // instruction keyword, not our immediate
      }
    }
    Imms.push_back(Child);
    C.next();
  }

  // Separate offset=/align= keywords from index operands
  std::vector<Node> IndexImms;
  for (auto &Imm : Imms) {
    auto CText = nodeText(Imm);
    if (nodeType(Imm) == NodeType::Keyword) {
      if (CText.substr(0, 7) == "offset="sv) {
        EXPECTED_TRY(auto Off, parseOffset(CText));
        EXPECTED_TRY(
            Offset, checkOffsetU32(Off, !Conf.hasProposal(Proposal::Memory64)));
      } else if (CText.substr(0, 6) == "align="sv) {
        EXPECTED_TRY(auto Al, parseAlign(CText));
        Align = Al;
      }
    } else {
      IndexImms.push_back(Imm);
    }
  }

  if (isSimdLaneMemOp(Code)) {
    // SIMD lane ops: last index is lane, preceding index (if any) is memidx
    if (IndexImms.size() >= 2) {
      EXPECTED_TRY(auto Idx, Syms.resolve(SymbolTable::IndexSpace::Memory,
                                          nodeText(IndexImms[0])));
      MemIdx = Idx;
      EXPECTED_TRY(auto L, parseUint(nodeText(IndexImms[1])));
      if (L >= maxLaneForOp(Code)) {
        return Unexpect(ErrCode::Value::InvalidLaneIdx);
      }
      Lane = static_cast<uint8_t>(L);
    } else if (IndexImms.size() == 1) {
      EXPECTED_TRY(auto L, parseUint(nodeText(IndexImms[0])));
      if (L >= maxLaneForOp(Code)) {
        return Unexpect(ErrCode::Value::InvalidLaneIdx);
      }
      Lane = static_cast<uint8_t>(L);
    }
  } else if (!IndexImms.empty()) {
    EXPECTED_TRY(auto Idx, Syms.resolve(SymbolTable::IndexSpace::Memory,
                                        nodeText(IndexImms[0])));
    MemIdx = Idx;
  }

  Instrs.emplace_back(Code);
  auto &Instr = Instrs.back();
  Instr.getMemoryOffset() = Offset;
  Instr.getMemoryAlign() = Align;
  Instr.getTargetIndex() = MemIdx;
  if (isSimdLaneMemOp(Code)) {
    Instr.getMemoryLane() = Lane;
  }
  return {};
}

// --- convertSimdLaneOp ---
// {i8x16,i16x8,...}.{extract_lane,replace_lane} laneidx
Expect<void> Converter::convertSimdLaneOp(Cursor &C, AST::InstrVec &Instrs,
                                          OpCode Code) {
  Instrs.emplace_back(Code);
  Node Child = nextImmediate(C);
  if (Child.isNull()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  auto ChildType = nodeType(Child);
  if (ChildType != NodeType::U) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  auto LaneText = nodeText(Child);
  // Signed lane index (with + or -) is "unexpected token" per spec.
  if (!LaneText.empty() &&
      (LaneText.front() == '-' || LaneText.front() == '+')) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  EXPECTED_TRY(auto V, parseUint(LaneText));
  uint8_t MaxLane = [&]() -> uint8_t {
    switch (Code) {
    case OpCode::I8x16__extract_lane_s:
    case OpCode::I8x16__extract_lane_u:
    case OpCode::I8x16__replace_lane:
      return 16;
    case OpCode::I16x8__extract_lane_s:
    case OpCode::I16x8__extract_lane_u:
    case OpCode::I16x8__replace_lane:
      return 8;
    case OpCode::I32x4__extract_lane:
    case OpCode::I32x4__replace_lane:
    case OpCode::F32x4__extract_lane:
    case OpCode::F32x4__replace_lane:
      return 4;
    default:
      return 2;
    }
  }();
  if (V > 255) {
    return Unexpect(ErrCode::Value::WatI8ConstOutOfRange);
  }
  if (V >= MaxLane) {
    return Unexpect(ErrCode::Value::InvalidLaneIdx);
  }
  Instrs.back().getMemoryLane() = static_cast<uint8_t>(V);
  return {};
}

// blocktype ::= (type typeidx)? (param valtype*)* (result valtype*)*
// For flat instructions: consumes type annotation sexprs from Cursor onward.
Expect<BlockType> Converter::parseBlockType(Cursor &C) {
  AST::FunctionType BlockFT;
  bool HasTypeUse = false;
  uint32_t TypeIdx = 0;
  bool HasParam = false;
  bool HasResult = false;

  while (C.valid()) {
    Node Next = C.node();
    if (nodeType(Next) != NodeType::Sexpr) {
      break;
    }
    Cursor FC(Next);
    auto KW = peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
    if (KW == "type"sv) {
      if (HasParam || HasResult) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      Cursor TC(Next);
      TC.next(); // skip "type" keyword
      if (!TC.valid()) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      HasTypeUse = true;
      Node TChild = TC.node();
      if (nodeType(TChild) == NodeType::Id || nodeType(TChild) == NodeType::U) {
        EXPECTED_TRY(TypeIdx, Syms.resolveType(nodeText(TChild)));
      }
      C.next();
    } else if (KW == "param"sv) {
      if (HasResult) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      HasParam = true;
      Cursor PC(Next);
      PC.next(); // skip "param" keyword
      while (PC.valid()) {
        if (nodeType(PC.node()) == NodeType::Id) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        EXPECTED_TRY(auto VT, convertValType(PC.node()));
        BlockFT.getParamTypes().push_back(VT);
        PC.next();
      }
      C.next();
    } else if (KW == "result"sv) {
      HasResult = true;
      Cursor RC(Next);
      RC.next(); // skip "result" keyword
      while (RC.valid()) {
        EXPECTED_TRY(auto VT, convertValType(RC.node()));
        BlockFT.getReturnTypes().push_back(VT);
        RC.next();
      }
      C.next();
    } else {
      break; // Not a block type annotation, stop consuming
    }
  }

  BlockType BType;
  if (HasTypeUse) {
    if (HasParam || HasResult) {
      uint32_t TIdx = findOrCreateFuncType(std::move(BlockFT), *CurMod);
      if (TIdx != TypeIdx) {
        return Unexpect(ErrCode::Value::WatInlineFuncType);
      }
    }
    BType.setData(TypeIdx);
  } else if (!HasParam && BlockFT.getReturnTypes().size() <= 1) {
    if (BlockFT.getReturnTypes().size() == 1) {
      BType.setData(BlockFT.getReturnTypes()[0]);
    } else {
      BType.setEmpty();
    }
  } else {
    // Multi-value or has params: create function type
    if (CurMod) {
      uint32_t TIdx = findOrCreateFuncType(std::move(BlockFT), *CurMod);
      BType.setData(TIdx);
    }
  }
  return BType;
}

// blockinstr ::= block label blocktype instr* end id?
//              | loop  label blocktype instr* end id?
//              | if    label blocktype instr* (else id? instr*)? end id?
//              | try_table label blocktype catch* instr* end id?
// Folded form: '(' blockinstr ... ')'
Expect<void> Converter::convertBlockInstr(Node N, AST::InstrVec &Instrs) {
  Cursor C(N);
  OpCode Code;
  if (peekType(C) != NodeType::Keyword) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  auto KW = nodeText(C.node());
  C.next();
  if (KW == "block"sv) {
    Code = OpCode::Block;
  } else if (KW == "loop"sv) {
    Code = OpCode::Loop;
  } else if (KW == "if"sv) {
    Code = OpCode::If;
  } else if (KW == "try_table"sv) {
    Code = OpCode::Try_table;
  } else {
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }

  // Parse optional label identifier (first Id child)
  std::string_view Label;
  if (peekType(C) == NodeType::Id) {
    Label = nodeText(C.node());
    C.next();
  }

  // For folded if: first emit condition folded instructions
  if (Code == OpCode::If) {
    // Check for (then ...) child to identify condition instructions
    // Scan ahead using a copy to find (then ...)
    bool FoundThen = false;
    auto Scan = C.copy();
    // Collect all nodes before "then" (including non-sexprs for error checking)
    std::vector<Node> CondNodes;
    while (Scan.valid()) {
      Node Child = Scan.node();
      if (sexprMatch(Scan, "then"sv)) {
        FoundThen = true;
        break;
      }
      CondNodes.push_back(Child);
      Scan.next();
    }
    if (FoundThen) {
      for (auto &Child : CondNodes) {
        if (nodeType(Child) != NodeType::Sexpr) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        Cursor FC(Child);
        if (peekType(FC) != NodeType::Keyword) {
          continue;
        }
        auto CKW = nodeText(FC.node());
        if (CKW != "type"sv && CKW != "param"sv && CKW != "result"sv) {
          EXPECTED_TRY(convertFoldedInstr(Child, Instrs));
        }
      }
    }
  }

  if (Code != OpCode::Try_table) {
    Syms.pushLabel(Label);
  }

  Instrs.emplace_back(Code);
  auto BlockIdx = Instrs.size() - 1;

  if (Code == OpCode::Try_table) {
    Instrs[BlockIdx].setTryCatch();
  }

  {
    auto &BType = (Code == OpCode::Try_table)
                      ? Instrs[BlockIdx].getTryCatch().ResType
                      : Instrs[BlockIdx].getBlockType();
    EXPECTED_TRY(BType, parseBlockType(C));
  }

  // For try_table, handle catch clauses
  if (Code == OpCode::Try_table) {
    auto &TryDesc = Instrs[BlockIdx].getTryCatch();

    while (C.valid()) {
      Node Child = C.node();
      if (nodeType(Child) != NodeType::Sexpr) {
        break;
      }
      Cursor FC(Child);
      auto CKW = peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
      if (CKW != "catch"sv && CKW != "catch_ref"sv && CKW != "catch_all"sv &&
          CKW != "catch_all_ref"sv) {
        break;
      }
      C.next();
      AST::Instruction::CatchDescriptor Catch{};
      std::vector<std::string_view> CatchArgs;
      Cursor CC(Child);
      while (CC.valid()) {
        if (nodeType(CC.node()) == NodeType::Keyword) {
          CC.next();
          continue; // skip catch/catch_ref/catch_all/catch_all_ref keyword
        }
        CatchArgs.push_back(nodeText(CC.node()));
        CC.next();
      }

      if (CKW == "catch"sv && CatchArgs.size() >= 2) {
        Catch.IsAll = false;
        Catch.IsRef = false;
        EXPECTED_TRY(Catch.TagIndex,
                     Syms.resolve(SymbolTable::IndexSpace::Tag, CatchArgs[0]));
        EXPECTED_TRY(Catch.LabelIndex, Syms.resolveLabel(CatchArgs[1]));
      } else if (CKW == "catch_ref"sv && CatchArgs.size() >= 2) {
        Catch.IsAll = false;
        Catch.IsRef = true;
        EXPECTED_TRY(Catch.TagIndex,
                     Syms.resolve(SymbolTable::IndexSpace::Tag, CatchArgs[0]));
        EXPECTED_TRY(Catch.LabelIndex, Syms.resolveLabel(CatchArgs[1]));
      } else if (CKW == "catch_all"sv && CatchArgs.size() >= 1) {
        Catch.IsAll = true;
        Catch.IsRef = false;
        EXPECTED_TRY(Catch.LabelIndex, Syms.resolveLabel(CatchArgs[0]));
      } else if (CKW == "catch_all_ref"sv && CatchArgs.size() >= 1) {
        Catch.IsAll = true;
        Catch.IsRef = true;
        EXPECTED_TRY(Catch.LabelIndex, Syms.resolveLabel(CatchArgs[0]));
      }

      TryDesc.Catch.push_back(Catch);
    }
    Syms.pushLabel(Label);
  }

  // Convert body instructions.
  // For 'if', handle (then ...) and (else ...) sub-sexprs specially.
  if (Code == OpCode::If) {
    bool InThen = false;
    while (C.valid()) {
      Node Child = C.node();
      auto CType = nodeType(Child);
      C.next();

      if (CType == NodeType::Sexpr) {
        Cursor FC(Child);
        if (peekType(FC) != NodeType::Keyword) {
          continue;
        }
        auto CKW = nodeText(FC.node());
        if (CKW == "then"sv) {
          InThen = true;
          // Process then body using convertMixedInstrSeq
          Cursor TC(Child);
          TC.next(); // skip "then" keyword
          EXPECTED_TRY(convertMixedInstrSeq(TC, Instrs));
          continue;
        }
        if (CKW == "else"sv) {
          InThen = false;
          Instrs.emplace_back(OpCode::Else);
          Cursor EC(Child);
          EC.next(); // skip "else" keyword
          EXPECTED_TRY(convertMixedInstrSeq(EC, Instrs));
          continue;
        }
        if (InThen) {
          EXPECTED_TRY(convertFoldedInstr(Child, Instrs));
        }
      } else if (CType == NodeType::Id) {
        continue; // label
      }
    }
  } else {
    // For block/loop/try_table: convertMixedInstrSeq will skip structural
    // sexprs and the block keyword.
    EXPECTED_TRY(convertMixedInstrSeq(C, Instrs));
  }

  Instrs.emplace_back(OpCode::End);
  Syms.popLabel();
  return {};
}

// foldedinstr ::= ( plaininstr foldedinstr* )
//               | ( blockinstr )
// Recursively converts nested sub-instructions, then emits this instruction.
Expect<void> Converter::convertFoldedInstr(Node N, AST::InstrVec &Instrs) {
  Cursor FC(N);
  if (peekType(FC) != NodeType::Keyword) {
    return Unexpect(ErrCode::Value::IllegalGrammar);
  }
  auto KW = nodeText(FC.node());

  // Block-like folded instructions
  if (KW == "block"sv || KW == "loop"sv || KW == "if"sv ||
      KW == "try_table"sv) {
    return convertBlockInstr(N, Instrs);
  }

  // Structural keywords appearing where an instruction is expected
  if (KW == "local"sv || KW == "param"sv || KW == "result"sv ||
      KW == "type"sv || KW == "catch"sv || KW == "catch_ref"sv ||
      KW == "catch_all"sv || KW == "catch_all_ref"sv || KW == "then"sv ||
      KW == "else"sv) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  // Plain folded instruction: (op nested-instrs... immediates...)
  // First, recursively convert nested sexpr children as sub-instructions
  FC.next(); // skip opcode keyword
  while (FC.valid()) {
    Node Child = FC.node();
    FC.next();
    if (nodeType(Child) == NodeType::Sexpr) {
      Cursor CC(Child);
      if (peekType(CC) == NodeType::Keyword) {
        auto CKW = nodeText(CC.node());
        // Skip structural sexprs which are immediates, not instructions
        if (CKW == "type"sv || CKW == "param"sv || CKW == "result"sv ||
            CKW == "offset"sv || CKW == "ref"sv || CKW == "item"sv ||
            CKW == "mut"sv) {
          continue;
        }
        EXPECTED_TRY(convertFoldedInstr(Child, Instrs));
      }
    }
  }

  // Then emit this instruction itself
  auto &Map = keywordToOpCode();
  auto It = Map.find(KW);
  if (It == Map.end()) {
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
  Cursor IC(N);
  IC.next(); // skip opcode keyword
  return convertPlainInstr(IC, Instrs, It->second);
}

} // namespace WasmEdge::WAT
