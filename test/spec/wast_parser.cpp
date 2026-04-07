// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2026 Second State INC

#include "wast_parser.h"
#include "common/hash.h"
#include "common/spdlog.h"
#include "wat/parser.h"
#include "wat/wat_util.h"

#include "tree_sitter.h"

extern "C" const TSLanguage *tree_sitter_wat();

#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace WasmEdge {
namespace Wast {

namespace {
using namespace WasmEdge::WAT; // Node, Cursor, Tree, Parser
using namespace std::literals;

/// Node type enum for the WAT grammar.
/// Mirrors the enum in converter.h — defined locally to avoid depending
/// on WAT converter internals.
enum class NodeType {
  Sexpr,
  Keyword,
  U,
  S,
  F,
  String,
  Id,
  Error,
  Unknown,
};

// --- WastConverter ---
// Walks a WAT grammar parse tree (generic sexpr/keyword nodes) and produces
// a WastScript. Follows the same pattern as the WAT Converter class:
// all semantic interpretation via keyword-string matching.

class WastConverter {
public:
  Expect<WastScript> convert(const Tree &T, std::string Source);

private:
  // --- Helpers (same pattern as WAT Converter) ---

  std::string_view nodeText(Node N) const { return N.text(Source_); }

  NodeType nodeType(Node N) const {
    if (N.isNull()) {
      return NodeType::Unknown;
    }
    auto T = N.type();
    if (T == "sexpr"sv || T == "root"sv)
      return NodeType::Sexpr;
    if (T == "keyword"sv)
      return NodeType::Keyword;
    if (T == "u"sv)
      return NodeType::U;
    if (T == "s"sv)
      return NodeType::S;
    if (T == "f"sv)
      return NodeType::F;
    if (T == "string"sv)
      return NodeType::String;
    if (T == "id"sv)
      return NodeType::Id;
    if (T == "ERROR"sv)
      return NodeType::Error;
    return NodeType::Unknown;
  }

  /// Get the first keyword text from a sexpr node's children.
  std::string_view sexprKeyword(Node N) const {
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      if (nodeType(Child) == NodeType::Keyword) {
        return nodeText(Child);
      }
    }
    return {};
  }

  /// Check if a node is a sexpr whose first keyword matches KW.
  bool sexprMatch(Node N, std::string_view KW) const {
    return nodeType(N) == NodeType::Sexpr && sexprKeyword(N) == KW;
  }

  /// Check if a node is a numeric token (u, s, or f).
  bool isNumeric(Node N) const {
    auto T = nodeType(N);
    return T == NodeType::U || T == NodeType::S || T == NodeType::F;
  }

  /// Check if a node is a NaN pattern token (nan:canonical or nan:arithmetic).
  Result::NaNPattern nanPattern(Node N) const {
    if (nodeType(N) != NodeType::Keyword) {
      return Result::NaNPattern::None;
    }
    auto Text = nodeText(N);
    if (Text == "nan:canonical"sv)
      return Result::NaNPattern::Canonical;
    if (Text == "nan:arithmetic"sv)
      return Result::NaNPattern::Arithmetic;
    return Result::NaNPattern::None;
  }

  // --- Heap type keyword -> TypeCode ---

  TypeCode heapTypeToCode(std::string_view HT) const {
    if (HT == "func"sv)
      return TypeCode::FuncRef;
    if (HT == "extern"sv)
      return TypeCode::ExternRef;
    if (HT == "any"sv)
      return TypeCode::AnyRef;
    if (HT == "eq"sv)
      return TypeCode::EqRef;
    if (HT == "i31"sv)
      return TypeCode::I31Ref;
    if (HT == "struct"sv)
      return TypeCode::StructRef;
    if (HT == "array"sv)
      return TypeCode::ArrayRef;
    if (HT == "none"sv)
      return TypeCode::NullRef;
    if (HT == "noextern"sv)
      return TypeCode::NullExternRef;
    if (HT == "nofunc"sv)
      return TypeCode::NullFuncRef;
    if (HT == "exn"sv)
      return TypeCode::ExnRef;
    if (HT == "noexn"sv)
      return TypeCode::NullExnRef;
    return TypeCode::FuncRef; // default
  }

  // --- Const expression parsing (invoke args) ---

  void parseConstExpr(Node N, std::vector<ValVariant> &Args,
                      std::vector<ValType> &Types) {
    auto KW = sexprKeyword(N);

    if (KW == "i32.const"sv) {
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        if (isNumeric(C)) {
          Args.emplace_back(parseU32(nodeText(C)));
          Types.emplace_back(TypeCode::I32);
        }
      }
    } else if (KW == "i64.const"sv) {
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        if (isNumeric(C)) {
          Args.emplace_back(parseU64(nodeText(C)));
          Types.emplace_back(TypeCode::I64);
        }
      }
    } else if (KW == "f32.const"sv) {
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        if (isNumeric(C)) {
          Args.emplace_back(parseF32Bits(nodeText(C)));
          Types.emplace_back(TypeCode::F32);
        }
      }
    } else if (KW == "f64.const"sv) {
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        if (isNumeric(C)) {
          Args.emplace_back(parseF64Bits(nodeText(C)));
          Types.emplace_back(TypeCode::F64);
        }
      }
    } else if (KW == "v128.const"sv) {
      uint128_t V128{};
      std::string Shape;
      std::vector<std::string_view> Nums;
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        auto CT = nodeType(C);
        if (CT == NodeType::Keyword && nodeText(C) != "v128.const"sv) {
          Shape = std::string(nodeText(C));
        } else if (isNumeric(C)) {
          Nums.push_back(nodeText(C));
        }
      }
      parseV128Lanes(Shape, Nums, V128);
      Args.emplace_back(V128);
      Types.emplace_back(TypeCode::V128);
    } else if (KW == "ref.null"sv) {
      TypeCode Code = TypeCode::FuncRef;
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        if (nodeType(C) == NodeType::Keyword && nodeText(C) != "ref.null"sv) {
          Code = heapTypeToCode(nodeText(C));
        }
      }
      Args.emplace_back(RefVariant(Code));
      Types.emplace_back(Code);
    } else if (KW == "ref.extern"sv) {
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        if (isNumeric(C)) {
          auto Val = parseU32(nodeText(C));
          Args.emplace_back(
              RefVariant(TypeCode::ExternRef,
                         reinterpret_cast<void *>(static_cast<uintptr_t>(Val) +
                                                  0x100000000ULL)));
          Types.emplace_back(TypeCode::ExternRef);
        }
      }
    } else if (KW == "ref.host"sv) {
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        if (isNumeric(C)) {
          auto Val = parseU32(nodeText(C));
          Args.emplace_back(
              RefVariant(TypeCode::AnyRef,
                         reinterpret_cast<void *>(static_cast<uintptr_t>(Val) +
                                                  0x100000000ULL)));
          Types.emplace_back(TypeCode::AnyRef);
        }
      }
    } else if (KW == "ref.func"sv) {
      Args.emplace_back(RefVariant(TypeCode::FuncRef));
      Types.emplace_back(TypeCode::FuncRef);
    }
  }

  // --- V128 lane parsing helper ---

  void parseV128Lanes(const std::string &Shape,
                      const std::vector<std::string_view> &Nums,
                      uint128_t &V128) {
    if (Shape == "i8x16" && Nums.size() == 16) {
      uint8_t Lanes[16];
      for (size_t K = 0; K < 16; ++K)
        Lanes[K] = static_cast<uint8_t>(parseU32(Nums[K]));
      std::memcpy(&V128, Lanes, 16);
    } else if (Shape == "i16x8" && Nums.size() == 8) {
      uint16_t Lanes[8];
      for (size_t K = 0; K < 8; ++K)
        Lanes[K] = static_cast<uint16_t>(parseU32(Nums[K]));
      std::memcpy(&V128, Lanes, 16);
    } else if (Shape == "i32x4" && Nums.size() == 4) {
      uint32_t Lanes[4];
      for (size_t K = 0; K < 4; ++K)
        Lanes[K] = parseU32(Nums[K]);
      std::memcpy(&V128, Lanes, 16);
    } else if (Shape == "i64x2" && Nums.size() == 2) {
      uint64_t Lanes[2];
      for (size_t K = 0; K < 2; ++K)
        Lanes[K] = parseU64(Nums[K]);
      std::memcpy(&V128, Lanes, 16);
    } else if (Shape == "f32x4" && Nums.size() == 4) {
      uint32_t Lanes[4];
      for (size_t K = 0; K < 4; ++K)
        Lanes[K] = parseF32Bits(Nums[K]);
      std::memcpy(&V128, Lanes, 16);
    } else if (Shape == "f64x2" && Nums.size() == 2) {
      uint64_t Lanes[2];
      for (size_t K = 0; K < 2; ++K)
        Lanes[K] = parseF64Bits(Nums[K]);
      std::memcpy(&V128, Lanes, 16);
    }
  }

  // --- Action parsing (invoke/get) ---

  Action parseAction(Node N) {
    Action Act;
    auto KW = sexprKeyword(N);
    Act.Type = (KW == "invoke"sv) ? ActionType::Invoke : ActionType::Get;

    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      auto CT = nodeType(Child);
      if (CT == NodeType::Keyword) {
        // Skip the action keyword itself
        continue;
      } else if (CT == NodeType::Id) {
        Act.ModuleName = parseIdentifier(nodeText(Child));
      } else if (CT == NodeType::String) {
        if (auto S = parseString(nodeText(Child), false)) {
          Act.FieldName = std::move(*S);
        }
      } else if (CT == NodeType::Sexpr) {
        // Const expression argument (for invoke)
        parseConstExpr(Child, Act.Args, Act.ArgTypes);
      }
    }
    return Act;
  }

  // --- Result parsing ---

  Result parseResultConst(Node N) {
    Result R;
    auto KW = sexprKeyword(N);

    if (KW == "i32.const"sv) {
      R.Type = ValType(TypeCode::I32);
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        if (isNumeric(C)) {
          R.Value = parseU32(nodeText(C));
        }
      }
    } else if (KW == "i64.const"sv) {
      R.Type = ValType(TypeCode::I64);
      for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
        auto C = N.namedChild(I);
        if (isNumeric(C)) {
          R.Value = parseU64(nodeText(C));
        }
      }
    } else if (KW == "f32.const"sv) {
      R.Type = ValType(TypeCode::F32);
      parseFloatResult<uint32_t>(N, R, &parseF32Bits);
    } else if (KW == "f64.const"sv) {
      R.Type = ValType(TypeCode::F64);
      parseFloatResult<uint64_t>(N, R, &parseF64Bits);
    } else if (KW == "v128.const"sv) {
      R.Type = ValType(TypeCode::V128);
      parseV128Result(N, R);
    } else if (KW == "ref.null"sv) {
      parseRefNullResult(N, R);
    } else if (KW == "ref.extern"sv) {
      parseRefExternResult(N, R);
    } else if (KW == "ref.func"sv) {
      R.Type = ValType(TypeCode::FuncRef);
      R.Value = RefVariant(TypeCode::FuncRef);
      R.OpaqueRef = true;
    } else if (KW == "ref.any"sv) {
      R.Type = ValType(TypeCode::AnyRef);
      R.Value = RefVariant(TypeCode::AnyRef);
      R.OpaqueRef = true;
    } else if (KW == "ref.eq"sv) {
      R.Type = ValType(TypeCode::EqRef);
      R.Value = RefVariant(TypeCode::EqRef);
      R.OpaqueRef = true;
    } else if (KW == "ref.i31"sv) {
      R.Type = ValType(TypeCode::I31Ref);
      R.Value = RefVariant(TypeCode::I31Ref);
      R.OpaqueRef = true;
    } else if (KW == "ref.struct"sv) {
      R.Type = ValType(TypeCode::StructRef);
      R.Value = RefVariant(TypeCode::StructRef);
      R.OpaqueRef = true;
    } else if (KW == "ref.array"sv) {
      R.Type = ValType(TypeCode::ArrayRef);
      R.Value = RefVariant(TypeCode::ArrayRef);
      R.OpaqueRef = true;
    } else if (KW == "ref.exn"sv) {
      R.Type = ValType(TypeCode::ExnRef);
      R.Value = RefVariant(TypeCode::ExnRef);
      R.OpaqueRef = true;
    } else if (KW == "ref.host"sv) {
      parseRefHostResult(N, R);
    }
    return R;
  }

  // --- Float result helper (handles NaN patterns) ---

  template <typename T>
  void parseFloatResult(Node N, Result &R, T (*ParseFn)(std::string_view)) {
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto C = N.namedChild(I);
      if (isNumeric(C)) {
        R.Value = (*ParseFn)(nodeText(C));
      } else if (auto NaN = nanPattern(C); NaN != Result::NaNPattern::None) {
        R.NaN = NaN;
        R.Value = T(0);
      }
    }
  }

  // --- V128 result parsing ---

  void parseV128Result(Node N, Result &R) {
    uint128_t V128{};
    std::string Shape;
    struct LaneEntry {
      std::string_view Text;
      Result::NaNPattern NaN = Result::NaNPattern::None;
    };
    std::vector<LaneEntry> Lanes;

    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto C = N.namedChild(I);
      auto CT = nodeType(C);
      if (auto NaN = nanPattern(C); NaN != Result::NaNPattern::None) {
        Lanes.push_back({{}, NaN});
      } else if (CT == NodeType::Keyword && nodeText(C) != "v128.const"sv) {
        Shape = std::string(nodeText(C));
      } else if (isNumeric(C)) {
        Lanes.push_back({nodeText(C), Result::NaNPattern::None});
      }
    }

    R.V128Shape = Shape;
    for (const auto &L : Lanes) {
      R.V128LaneNaN.push_back(L.NaN);
    }

    uint8_t *Bytes = reinterpret_cast<uint8_t *>(&V128);
    if (Shape == "i8x16" && Lanes.size() == 16) {
      for (size_t K = 0; K < 16; ++K)
        if (Lanes[K].NaN == Result::NaNPattern::None)
          Bytes[K] = static_cast<uint8_t>(parseU32(Lanes[K].Text));
    } else if (Shape == "i16x8" && Lanes.size() == 8) {
      for (size_t K = 0; K < 8; ++K)
        if (Lanes[K].NaN == Result::NaNPattern::None) {
          uint16_t V = static_cast<uint16_t>(parseU32(Lanes[K].Text));
          std::memcpy(Bytes + K * 2, &V, 2);
        }
    } else if (Shape == "i32x4" && Lanes.size() == 4) {
      for (size_t K = 0; K < 4; ++K)
        if (Lanes[K].NaN == Result::NaNPattern::None) {
          uint32_t V = parseU32(Lanes[K].Text);
          std::memcpy(Bytes + K * 4, &V, 4);
        }
    } else if (Shape == "i64x2" && Lanes.size() == 2) {
      for (size_t K = 0; K < 2; ++K)
        if (Lanes[K].NaN == Result::NaNPattern::None) {
          uint64_t V = parseU64(Lanes[K].Text);
          std::memcpy(Bytes + K * 8, &V, 8);
        }
    } else if (Shape == "f32x4" && Lanes.size() == 4) {
      for (size_t K = 0; K < 4; ++K)
        if (Lanes[K].NaN == Result::NaNPattern::None) {
          uint32_t V = parseF32Bits(Lanes[K].Text);
          std::memcpy(Bytes + K * 4, &V, 4);
        }
    } else if (Shape == "f64x2" && Lanes.size() == 2) {
      for (size_t K = 0; K < 2; ++K)
        if (Lanes[K].NaN == Result::NaNPattern::None) {
          uint64_t V = parseF64Bits(Lanes[K].Text);
          std::memcpy(Bytes + K * 8, &V, 8);
        }
    }
    R.Value = V128;
  }

  // --- Ref result helpers ---

  void parseRefNullResult(Node N, Result &R) {
    TypeCode Code = TypeCode::FuncRef;
    bool HasHeapType = false;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto C = N.namedChild(I);
      auto CT = nodeType(C);
      if (CT == NodeType::Keyword && nodeText(C) != "ref.null"sv) {
        HasHeapType = true;
        Code = heapTypeToCode(nodeText(C));
      } else if (isNumeric(C)) {
        HasHeapType = true;
      }
    }
    if (!HasHeapType) {
      Code = TypeCode::AnyRef;
      R.AnyNullRef = true;
    }
    R.Type = ValType(Code);
    R.Value = RefVariant(Code);
  }

  void parseRefExternResult(Node N, Result &R) {
    bool HasVal = false;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto C = N.namedChild(I);
      if (isNumeric(C)) {
        HasVal = true;
        auto Val = parseU32(nodeText(C));
        R.Value = RefVariant(TypeCode::ExternRef,
                             reinterpret_cast<void *>(
                                 static_cast<uintptr_t>(Val) + 0x100000000ULL));
      }
    }
    if (!HasVal) {
      R.Value = RefVariant(TypeCode::ExternRef);
      R.OpaqueRef = true;
    }
    R.Type = ValType(TypeCode::ExternRef);
  }

  void parseRefHostResult(Node N, Result &R) {
    bool HasVal = false;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto C = N.namedChild(I);
      if (isNumeric(C)) {
        HasVal = true;
        auto Val = parseU32(nodeText(C));
        R.Value = RefVariant(TypeCode::AnyRef,
                             reinterpret_cast<void *>(
                                 static_cast<uintptr_t>(Val) + 0x100000000ULL));
      }
    }
    if (!HasVal) {
      R.Value = RefVariant(TypeCode::AnyRef);
      R.OpaqueRef = true;
    }
    R.Type = ValType(TypeCode::AnyRef);
  }

  // --- Command converters ---

  ScriptCommand convertCommand(Node N) {
    auto KW = sexprKeyword(N);

    if (KW == "module"sv)
      return convertModule(N);
    if (KW == "register"sv)
      return convertRegister(N);
    if (KW == "invoke"sv || KW == "get"sv)
      return convertActionCommand(N);
    if (KW == "assert_return"sv)
      return convertAssertReturn(N);
    if (KW == "assert_trap"sv)
      return convertAssertTrap(N);
    if (KW == "assert_exhaustion"sv)
      return convertAssertExhaustion(N);
    if (KW == "assert_invalid"sv)
      return convertAssertInvalid(N);
    if (KW == "assert_malformed"sv)
      return convertAssertMalformed(N);
    if (KW == "assert_unlinkable"sv)
      return convertAssertUnlinkable(N);
    if (KW == "assert_uninstantiable"sv)
      return convertAssertUninstantiable(N);
    if (KW == "assert_exception"sv)
      return convertAssertException(N);
    if (KW == "thread"sv)
      return convertThread(N);
    if (KW == "wait"sv)
      return convertWait(N);

    // Bare module (sexpr starting with section keyword like "func", "type")
    return convertBareModule(N);
  }

  ScriptCommand convertModule(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::Module;
    Cmd.Line = N.startRow() + 1;
    Cmd.ModType = ModuleType::Text;
    Cmd.ModuleSource = nodeText(N);

    bool IsInstance = false;
    bool IsBinary = false;
    bool IsQuote = false;
    std::vector<std::string_view> Identifiers;

    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      auto CT = nodeType(Child);
      auto Text = nodeText(Child);

      if (CT == NodeType::Id) {
        Identifiers.push_back(parseIdentifier(Text));
      } else if (CT == NodeType::Keyword) {
        if (Text == "binary"sv) {
          IsBinary = true;
        } else if (Text == "quote"sv) {
          IsQuote = true;
        } else if (Text == "definition"sv) {
          Cmd.Type = CommandType::ModuleDefinition;
        } else if (Text == "instance"sv) {
          IsInstance = true;
          Cmd.Type = CommandType::ModuleInstance;
        }
      }
    }

    if (IsBinary) {
      Cmd.ModType = ModuleType::Binary;
    } else if (IsQuote) {
      Cmd.ModType = ModuleType::Quote;
    }

    if (IsInstance) {
      if (Identifiers.size() >= 2) {
        Cmd.ModuleName = Identifiers[0];
        Cmd.DefinitionName = Identifiers[1];
      } else if (Identifiers.size() == 1) {
        Cmd.ModuleName = Identifiers[0];
      }
    } else if (!Identifiers.empty()) {
      Cmd.ModuleName = Identifiers[0];
    }

    return Cmd;
  }

  ScriptCommand convertBareModule(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::Module;
    Cmd.Line = N.startRow() + 1;
    Cmd.ModType = ModuleType::Text;
    Cmd.ModuleSource = nodeText(N);
    return Cmd;
  }

  ScriptCommand convertRegister(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::Register;
    Cmd.Line = N.startRow() + 1;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      auto CT = nodeType(Child);
      if (CT == NodeType::String) {
        Cmd.RegisterName = stripQuotes(nodeText(Child));
      } else if (CT == NodeType::Id) {
        Cmd.ModuleName = parseIdentifier(nodeText(Child));
      }
    }
    return Cmd;
  }

  ScriptCommand convertActionCommand(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::Action;
    Cmd.Line = N.startRow() + 1;
    Cmd.Act = parseAction(N);
    return Cmd;
  }

  ScriptCommand convertAssertReturn(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::AssertReturn;
    Cmd.Line = N.startRow() + 1;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      auto CT = nodeType(Child);
      if (CT == NodeType::Sexpr) {
        auto CKW = sexprKeyword(Child);
        if (CKW == "invoke"sv || CKW == "get"sv) {
          Cmd.Act = parseAction(Child);
        } else if (CKW == "either"sv) {
          ResultOrEither ROE;
          for (uint32_t J = 0; J < Child.namedChildCount(); ++J) {
            auto EChild = Child.namedChild(J);
            if (nodeType(EChild) == NodeType::Sexpr) {
              ROE.Alternatives.push_back(parseResultConst(EChild));
            }
          }
          Cmd.Expected.push_back(std::move(ROE));
        } else {
          // Result const/ref sexpr
          ResultOrEither ROE;
          ROE.Alternatives.push_back(parseResultConst(Child));
          Cmd.Expected.push_back(std::move(ROE));
        }
      }
    }
    return Cmd;
  }

  ScriptCommand convertAssertTrap(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::AssertTrap;
    Cmd.Line = N.startRow() + 1;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      auto CT = nodeType(Child);
      if (CT == NodeType::Sexpr) {
        auto CKW = sexprKeyword(Child);
        if (CKW == "invoke"sv || CKW == "get"sv) {
          Cmd.Act = parseAction(Child);
        } else if (CKW == "module"sv) {
          // Embedded module in assert_trap
          Cmd.ModType = detectModuleType(Child);
          Cmd.ModuleSource = nodeText(Child);
        } else {
          // Bare module (section keyword)
          Cmd.ModType = ModuleType::Text;
          Cmd.ModuleSource = nodeText(Child);
        }
      } else if (CT == NodeType::String) {
        Cmd.ExpectedMessage = stripQuotes(nodeText(Child));
      }
    }
    return Cmd;
  }

  ScriptCommand convertAssertExhaustion(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::AssertExhaustion;
    Cmd.Line = N.startRow() + 1;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      auto CT = nodeType(Child);
      if (CT == NodeType::Sexpr) {
        auto CKW = sexprKeyword(Child);
        if (CKW == "invoke"sv || CKW == "get"sv) {
          Cmd.Act = parseAction(Child);
        }
      } else if (CT == NodeType::String) {
        Cmd.ExpectedMessage = stripQuotes(nodeText(Child));
      }
    }
    return Cmd;
  }

  // Helper for assert_invalid/malformed/unlinkable/uninstantiable
  // (all have the same structure: module + error message string)
  ScriptCommand convertAssertModule(Node N, CommandType Type) {
    ScriptCommand Cmd;
    Cmd.Type = Type;
    Cmd.Line = N.startRow() + 1;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      auto CT = nodeType(Child);
      if (CT == NodeType::Sexpr) {
        auto CKW = sexprKeyword(Child);
        if (CKW == "module"sv) {
          Cmd.ModType = detectModuleType(Child);
          Cmd.ModuleSource = nodeText(Child);
        } else {
          // Bare module
          Cmd.ModType = ModuleType::Text;
          Cmd.ModuleSource = nodeText(Child);
        }
      } else if (CT == NodeType::String) {
        Cmd.ExpectedMessage = stripQuotes(nodeText(Child));
      }
    }
    return Cmd;
  }

  ScriptCommand convertAssertInvalid(Node N) {
    return convertAssertModule(N, CommandType::AssertInvalid);
  }
  ScriptCommand convertAssertMalformed(Node N) {
    return convertAssertModule(N, CommandType::AssertMalformed);
  }
  ScriptCommand convertAssertUnlinkable(Node N) {
    return convertAssertModule(N, CommandType::AssertUnlinkable);
  }
  ScriptCommand convertAssertUninstantiable(Node N) {
    return convertAssertModule(N, CommandType::AssertUninstantiable);
  }

  ScriptCommand convertAssertException(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::AssertException;
    Cmd.Line = N.startRow() + 1;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      if (nodeType(Child) == NodeType::Sexpr) {
        auto CKW = sexprKeyword(Child);
        if (CKW == "invoke"sv || CKW == "get"sv) {
          Cmd.Act = parseAction(Child);
        }
      }
    }
    return Cmd;
  }

  // --- Module type detection for embedded modules ---

  ModuleType detectModuleType(Node N) {
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      auto CT = nodeType(Child);
      auto Text = nodeText(Child);
      if (CT == NodeType::Keyword) {
        if (Text == "binary"sv)
          return ModuleType::Binary;
        if (Text == "quote"sv)
          return ModuleType::Quote;
      }
    }
    return ModuleType::Text;
  }

  // --- Thread/Wait ---

  ScriptCommand convertThread(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::Thread;
    Cmd.Line = N.startRow() + 1;

    std::map<std::string, std::string> RegisterMap;

    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      auto CT = nodeType(Child);

      if (CT == NodeType::Id) {
        Cmd.ModuleName = parseIdentifier(nodeText(Child));
        continue;
      }

      if (CT != NodeType::Sexpr) {
        continue;
      }

      auto CKW = sexprKeyword(Child);

      if (CKW == "shared"sv) {
        // (shared (module $name) ...)
        for (uint32_t J = 0; J < Child.namedChildCount(); ++J) {
          auto Sub = Child.namedChild(J);
          if (sexprMatch(Sub, "module"sv)) {
            for (uint32_t K = 0; K < Sub.namedChildCount(); ++K) {
              auto MC = Sub.namedChild(K);
              if (nodeType(MC) == NodeType::Id) {
                auto ModRef = parseIdentifier(nodeText(MC));
                Cmd.SharedModules.emplace_back(std::string(ModRef),
                                               std::string(ModRef));
              }
            }
          }
        }
      } else if (CKW == "register"sv) {
        std::string_view RegName;
        std::string_view ModRef;
        for (uint32_t J = 0; J < Child.namedChildCount(); ++J) {
          auto Sub = Child.namedChild(J);
          if (nodeType(Sub) == NodeType::String) {
            RegName = stripQuotes(nodeText(Sub));
          } else if (nodeType(Sub) == NodeType::Id) {
            ModRef = parseIdentifier(nodeText(Sub));
          }
        }
        if (!ModRef.empty() && !RegName.empty()) {
          RegisterMap.emplace(std::string(ModRef), std::string(RegName));
        }
        // Also build sub-command
        ScriptCommand Sub;
        Sub.Type = CommandType::Register;
        Sub.Line = Child.startRow() + 1;
        Sub.RegisterName = RegName;
        if (!ModRef.empty()) {
          Sub.ModuleName = ModRef;
        }
        Cmd.SubCommands.push_back(std::move(Sub));
      } else if (CKW == "module"sv) {
        ScriptCommand Sub;
        Sub.Type = CommandType::Module;
        Sub.Line = Child.startRow() + 1;
        Sub.ModType = ModuleType::Text;
        Sub.ModuleSource = nodeText(Child);
        for (uint32_t J = 0; J < Child.namedChildCount(); ++J) {
          auto MC = Child.namedChild(J);
          if (nodeType(MC) == NodeType::Id) {
            Sub.ModuleName = parseIdentifier(nodeText(MC));
          }
        }
        Cmd.SubCommands.push_back(std::move(Sub));
      } else if (CKW == "invoke"sv) {
        ScriptCommand Sub;
        Sub.Type = CommandType::Action;
        Sub.Line = Child.startRow() + 1;
        Action Act;
        Act.Type = ActionType::Invoke;
        for (uint32_t J = 0; J < Child.namedChildCount(); ++J) {
          auto IC = Child.namedChild(J);
          if (nodeType(IC) == NodeType::String) {
            if (auto S = parseString(nodeText(IC), false)) {
              Act.FieldName = std::move(*S);
            }
          } else if (nodeType(IC) == NodeType::Id) {
            Act.ModuleName = parseIdentifier(nodeText(IC));
          }
        }
        Sub.Act = std::move(Act);
        Cmd.SubCommands.push_back(std::move(Sub));
      }
    }

    // Apply register aliases to shared modules
    for (auto &[OrigName, AliasName] : Cmd.SharedModules) {
      if (auto It = RegisterMap.find(OrigName); It != RegisterMap.end()) {
        AliasName = It->second;
      }
    }

    return Cmd;
  }

  ScriptCommand convertWait(Node N) {
    ScriptCommand Cmd;
    Cmd.Type = CommandType::Wait;
    Cmd.Line = N.startRow() + 1;
    for (uint32_t I = 0; I < N.namedChildCount(); ++I) {
      auto Child = N.namedChild(I);
      if (nodeType(Child) == NodeType::Id) {
        Cmd.ThreadName = parseIdentifier(nodeText(Child));
      }
    }
    return Cmd;
  }

  // --- Module source resolution ---

  void resolveModuleSource(ScriptCommand &Cmd) {
    if (Cmd.Type == CommandType::ModuleDefinition &&
        Cmd.ModType == ModuleType::Text) {
      auto Src = Cmd.ModuleSource;
      auto Pos = Src.find("definition");
      if (Pos != std::string_view::npos) {
        auto After = Pos + 10; // strlen("definition")
        while (After < Src.size() && Src[After] == ' ') {
          ++After;
        }
        OwnedStrings_->push_back(std::string(Src.substr(0, Pos)) +
                                 std::string(Src.substr(After)));
        Cmd.ModuleSource = OwnedStrings_->back();
      }
    }
    if (Cmd.ModType == ModuleType::Quote) {
      OwnedStrings_->push_back(resolveQuoteText(Cmd.ModuleSource));
      Cmd.ModuleSource = OwnedStrings_->back();
      Cmd.ModType = ModuleType::Text;
    }
    if (Cmd.ModType == ModuleType::Binary) {
      OwnedStrings_->push_back(resolveBinaryString(Cmd.ModuleSource));
      Cmd.ModuleSource = OwnedStrings_->back();
    }
  }

  // --- String decoding helpers ---

  static size_t findNextString(std::string_view Src, size_t &Pos) {
    while (Pos < Src.size()) {
      if (Src[Pos] == ' ' || Src[Pos] == '\t' || Src[Pos] == '\r' ||
          Src[Pos] == '\n') {
        ++Pos;
        continue;
      }
      if (Pos + 1 < Src.size() && Src[Pos] == ';' && Src[Pos + 1] == ';') {
        auto Eol = Src.find('\n', Pos);
        Pos = (Eol == std::string_view::npos) ? Src.size() : Eol + 1;
        continue;
      }
      if (Pos + 1 < Src.size() && Src[Pos] == '(' && Src[Pos + 1] == ';') {
        auto End = Src.find(";)", Pos + 2);
        Pos = (End == std::string_view::npos) ? Src.size() : End + 2;
        continue;
      }
      if (Src[Pos] == '"') {
        return Pos;
      }
      ++Pos;
    }
    return std::string_view::npos;
  }

  static std::string extractQuotedStrings(std::string_view RawSource) {
    std::string Result;
    size_t Pos = 0;
    while (Pos < RawSource.size()) {
      auto QStart = findNextString(RawSource, Pos);
      if (QStart == std::string_view::npos)
        break;
      auto QEnd = QStart + 1;
      while (QEnd < RawSource.size()) {
        if (RawSource[QEnd] == '\\') {
          QEnd += 2;
        } else if (RawSource[QEnd] == '"') {
          break;
        } else {
          ++QEnd;
        }
      }
      if (QEnd >= RawSource.size())
        break;
      auto StrLit = RawSource.substr(QStart, QEnd - QStart + 1);
      if (auto Str = parseString(StrLit, false)) {
        Result.append(*Str);
      }
      Pos = QEnd + 1;
    }
    return Result;
  }

  static std::string resolveBinaryString(std::string_view RawSource) {
    return extractQuotedStrings(RawSource);
  }

  static std::string resolveQuoteText(std::string_view RawSource) {
    return extractQuotedStrings(RawSource);
  }

  // State
  std::string_view Source_;
  std::deque<std::string> *OwnedStrings_ = nullptr;
};

// --- WastConverter::convert (out-of-line, after all helpers are declared) ---

Expect<WastScript> WastConverter::convert(const Tree &T, std::string Source) {
  WastScript Script;
  Script.Source = std::move(Source);
  Source_ = Script.Source;
  OwnedStrings_ = &Script.OwnedStrings;

  Node Root = T.rootNode();
  if (Root.hasError()) {
    spdlog::warn("WAST tree-sitter parse had errors"sv);
  }

  // Walk root children — each named child is a sexpr (command).
  for (uint32_t I = 0; I < Root.namedChildCount(); ++I) {
    Node Child = Root.namedChild(I);
    auto Type = nodeType(Child);
    if (Type != NodeType::Sexpr) {
      continue;
    }
    Script.Commands.push_back(convertCommand(Child));
    resolveModuleSource(Script.Commands.back());
  }

  return Script;
}

} // anonymous namespace

// --- Public API ---

Expect<WastScript> parseWast(const std::filesystem::path &Path) {
  std::ifstream File(Path, std::ios::binary | std::ios::ate);
  if (!File.is_open()) {
    spdlog::error("Failed to open WAST file: {}"sv, Path.u8string());
    return Unexpect(ErrCode::Value::IllegalPath);
  }
  auto Size = File.tellg();
  File.seekg(0, std::ios::beg);

  std::string Source;
  Source.resize(static_cast<size_t>(Size));
  File.read(Source.data(), Size);
  File.close();

  Parser P(tree_sitter_wat);
  Tree T = P.parse(Source);
  if (T.rootNode().isNull()) {
    spdlog::error("Failed to parse WAST file: {}"sv, Path.u8string());
    return Unexpect(ErrCode::Value::IllegalPath);
  }

  WastConverter Conv;
  return Conv.convert(T, std::move(Source));
}

} // namespace Wast
} // namespace WasmEdge
