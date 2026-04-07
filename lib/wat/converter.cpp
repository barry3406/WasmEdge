// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2026 Second State INC

#include "converter.h"
#include "common/errcode.h"
#include "wat/wat_util.h"

#include <cctype>
#include <limits>
#include <vector>

using namespace std::string_view_literals;

namespace WasmEdge::WAT {

namespace {

// Classify a non-ASCII byte sequence starting at Data[Pos].
// Returns WatIllegalCharacter if the bytes form a valid UTF-8 codepoint
// (legal Unicode, but non-ASCII chars are illegal in WAT outside strings).
// Returns MalformedUTF8 if the bytes are not valid UTF-8.
// Advances Pos past the consumed bytes.
ErrCode::Value classifyNonASCII(const unsigned char *Data, size_t Size,
                                size_t &Pos) {
  unsigned char B = Data[Pos];
  unsigned Expect;
  uint32_t CP;
  if ((B & 0xE0) == 0xC0) {
    Expect = 1;
    CP = B & 0x1F;
  } else if ((B & 0xF0) == 0xE0) {
    Expect = 2;
    CP = B & 0x0F;
  } else if ((B & 0xF8) == 0xF0) {
    Expect = 3;
    CP = B & 0x07;
  } else {
    // Lone continuation byte or invalid leading byte.
    ++Pos;
    return ErrCode::Value::MalformedUTF8;
  }
  if (Pos + Expect >= Size) {
    ++Pos;
    return ErrCode::Value::MalformedUTF8;
  }
  for (unsigned J = 1; J <= Expect; ++J) {
    if ((Data[Pos + J] & 0xC0) != 0x80) {
      ++Pos;
      return ErrCode::Value::MalformedUTF8;
    }
    CP = (CP << 6) | (Data[Pos + J] & 0x3F);
  }
  // Reject overlong encodings and out-of-range codepoints.
  if ((Expect == 1 && CP < 0x80) || (Expect == 2 && CP < 0x800) ||
      (Expect == 3 && CP < 0x10000) || CP > 0x10FFFF ||
      (CP >= 0xD800 && CP <= 0xDFFF)) {
    ++Pos;
    return ErrCode::Value::MalformedUTF8;
  }
  Pos += Expect + 1;
  return ErrCode::Value::WatIllegalCharacter;
}

} // anonymous namespace

void Converter::SymbolTable::clear() {
  Types.clear();
  Funcs.clear();
  Tables.clear();
  Memories.clear();
  Globals.clear();
  Tags.clear();
  Elems.clear();
  Datas.clear();
  Locals.clear();
  Labels.clear();
  LabelStack.clear();
  NextType = NextFunc = NextTable = NextMemory = 0;
  NextGlobal = NextTag = NextElem = NextData = NextLocal = 0;
}

void Converter::SymbolTable::clearLocals() {
  Locals.clear();
  Labels.clear();
  LabelStack.clear();
  NextLocal = 0;
}

void Converter::SymbolTable::pushLabel(std::string_view Label) {
  auto Decoded = decodeIdentifier(Label);
  std::string Name = Decoded ? std::move(*Decoded) : std::string();
  if (!Name.empty()) {
    Labels[Name].push_back(static_cast<uint32_t>(LabelStack.size()));
  }
  LabelStack.push_back(std::move(Name));
}

void Converter::SymbolTable::popLabel() {
  auto L = std::move(LabelStack.back());
  LabelStack.pop_back();
  if (!L.empty()) {
    auto It = Labels.find(L);
    It->second.pop_back();
    if (It->second.empty()) {
      Labels.erase(It);
    }
  }
}

Expect<uint32_t> Converter::SymbolTable::resolveIdx(
    const std::unordered_map<std::string, uint32_t, Hash::Hash> &Map,
    std::string_view Ref, ErrCode::Value Err) const {
  if (Ref.empty()) {
    return Unexpect(Err);
  }
  if (Ref[0] == '$') {
    EXPECTED_TRY(auto Decoded, decodeIdentifier(Ref).map_error([](auto) {
      return ErrCode::Value::WatUnknownId;
    }));
    auto It = Map.find(Decoded);
    if (It == Map.end()) {
      return Unexpect(ErrCode::Value::WatUnknownId);
    }
    return It->second;
  }
  // Numeric index — supports decimal and 0x hex
  uint32_t Idx = 0;
  size_t Pos = 0;
  bool IsHex = false;
  if (Ref.size() >= 2 && Ref[0] == '0' && (Ref[1] == 'x' || Ref[1] == 'X')) {
    IsHex = true;
    Pos = 2;
  }
  for (; Pos < Ref.size(); ++Pos) {
    char C = Ref[Pos];
    if (C == '_') {
      continue;
    }
    int D = IsHex ? hexDigit(C) : (C >= '0' && C <= '9' ? C - '0' : -1);
    if (D < 0) {
      return Unexpect(Err);
    }
    uint32_t Base = IsHex ? 16 : 10;
    if (Idx > std::numeric_limits<uint32_t>::max() / Base ||
        (Idx == std::numeric_limits<uint32_t>::max() / Base &&
         static_cast<uint32_t>(D) >
             std::numeric_limits<uint32_t>::max() % Base)) {
      return Unexpect(Err);
    }
    Idx = Idx * Base + static_cast<uint32_t>(D);
  }
  return Idx;
}

Expect<uint32_t>
Converter::SymbolTable::resolveType(std::string_view Ref) const {
  EXPECTED_TRY(auto Idx,
               resolveIdx(Types, Ref, ErrCode::Value::InvalidFuncTypeIdx));
  // For numeric type indices, check bounds. NextType is updated when
  // implicit types are created by resolveTypeUse.
  if (!Ref.empty() && Ref.front() != '$' && Idx >= NextType) {
    return Unexpect(ErrCode::Value::WatUnknownType);
  }
  return Idx;
}
Expect<uint32_t> Converter::SymbolTable::resolve(IndexSpace Space,
                                                 std::string_view Ref) const {
  switch (Space) {
  case IndexSpace::Func:
    return resolveIdx(Funcs, Ref, ErrCode::Value::InvalidFuncIdx);
  case IndexSpace::Table:
    return resolveIdx(Tables, Ref, ErrCode::Value::InvalidTableIdx);
  case IndexSpace::Memory:
    return resolveIdx(Memories, Ref, ErrCode::Value::InvalidMemoryIdx);
  case IndexSpace::Global:
    return resolveIdx(Globals, Ref, ErrCode::Value::InvalidGlobalIdx);
  case IndexSpace::Tag:
    return resolveIdx(Tags, Ref, ErrCode::Value::InvalidTagIdx);
  case IndexSpace::Elem:
    return resolveIdx(Elems, Ref, ErrCode::Value::InvalidElemIdx);
  case IndexSpace::Data:
    return resolveIdx(Datas, Ref, ErrCode::Value::InvalidDataIdx);
  case IndexSpace::Local:
    return resolveIdx(Locals, Ref, ErrCode::Value::InvalidLocalIdx);
  }
  return Unexpect(ErrCode::Value::WatUnexpectedToken);
}

Expect<uint32_t>
Converter::SymbolTable::resolveLabel(std::string_view Ref) const {
  if (Ref[0] == '$') {
    EXPECTED_TRY(auto Decoded, decodeIdentifier(Ref).map_error([](auto) {
      return ErrCode::Value::WatUnknownLabel;
    }));
    auto It = Labels.find(Decoded);
    if (It == Labels.end()) {
      return Unexpect(ErrCode::Value::WatUnknownLabel);
    }
    return static_cast<uint32_t>(LabelStack.size() - 1 - It->second.back());
  }
  // Numeric label
  uint32_t Idx = 0;
  for (char C : Ref) {
    if (C == '_') {
      continue;
    }
    int D = (C >= '0' && C <= '9') ? C - '0' : -1;
    if (D < 0) {
      return Unexpect(ErrCode::Value::WatUnknownLabel);
    }
    if (Idx > std::numeric_limits<uint32_t>::max() / 10 ||
        (Idx == std::numeric_limits<uint32_t>::max() / 10 &&
         static_cast<uint32_t>(D) >
             std::numeric_limits<uint32_t>::max() % 10)) {
      return Unexpect(ErrCode::Value::WatUnknownLabel);
    }
    Idx = Idx * 10 + static_cast<uint32_t>(D);
  }
  return Idx;
}

Expect<uint32_t>
Converter::SymbolTable::resolveField(uint32_t TypeIdx,
                                     std::string_view Ref) const {
  if (!Ref.empty() && Ref[0] == '$') {
    EXPECTED_TRY(auto Decoded, decodeIdentifier(Ref).map_error([](auto) {
      return ErrCode::Value::WatUnknownId;
    }));
    auto TIt = FieldNames.find(TypeIdx);
    if (TIt == FieldNames.end()) {
      return Unexpect(ErrCode::Value::WatUnknownId);
    }
    auto FIt = TIt->second.find(Decoded);
    if (FIt == TIt->second.end()) {
      return Unexpect(ErrCode::Value::WatUnknownId);
    }
    return FIt->second;
  }
  // Numeric field index: parse decimal integer
  uint64_t Idx = 0;
  for (char C : Ref) {
    if (C == '_') {
      continue;
    }
    int D = (C >= '0' && C <= '9') ? C - '0' : -1;
    if (D < 0) {
      return Unexpect(ErrCode::Value::WatUnknownId);
    }
    Idx = Idx * 10 + static_cast<uint64_t>(D);
  }
  return static_cast<uint32_t>(Idx);
}

std::string_view Converter::nodeText(Node N) const { return N.text(Source); }

NodeType Converter::nodeType(Node N) const {
  if (N.isNull()) {
    return NodeType::Unknown;
  }
  static const std::unordered_map<std::string_view, NodeType, Hash::Hash> Map =
      {
          // Both root and sexpr map to Sexpr. The converter distinguishes
          // them by context: root has no leading keyword; a module sexpr
          // has a leading "module" keyword.
          {"root"sv, NodeType::Sexpr},
          {"sexpr"sv, NodeType::Sexpr},
          {"keyword"sv, NodeType::Keyword},
          {"u"sv, NodeType::U},
          {"s"sv, NodeType::S},
          {"f"sv, NodeType::F},
          {"string"sv, NodeType::String},
          {"id"sv, NodeType::Id},
          {"reserved"sv, NodeType::Reserved},
          {"ERROR"sv, NodeType::Error},
      };
  auto It = Map.find(N.type());
  return It != Map.end() ? It->second : NodeType::Unknown;
}

/// Peek the type of the current node of C
NodeType Converter::peekType(const Cursor &C) const {
  if (!C.valid()) {
    return NodeType::Unknown;
  }
  return nodeType(C.node());
}

bool Converter::sexprMatch(const Cursor &C, std::string_view KW) const {
  if (peekType(C) != NodeType::Sexpr) {
    return false;
  }
  Cursor FC(C.node());
  return peekType(FC) == NodeType::Keyword && nodeText(FC.node()) == KW;
}

bool Converter::sexprUnmatch(const Cursor &C, std::string_view KW) const {
  if (peekType(C) != NodeType::Sexpr) {
    return false;
  }
  Cursor FC(C.node());
  return peekType(FC) == NodeType::Keyword && nodeText(FC.node()) != KW;
}

ErrCode::Value Converter::classifyError(Node Root) const {
  // Walk down to the first ERROR node.
  Node Err = Root;
  while (nodeType(Err) != NodeType::Error) {
    bool Found = false;
    for (uint32_t I = 0; I < Err.childCount(); ++I) {
      Node Child = Err.child(I);
      if (nodeType(Child) == NodeType::Error) {
        Err = Child;
        Found = true;
        break;
      }
      if (Child.hasError()) {
        Err = Child;
        Found = true;
        break;
      }
    }
    if (!Found) {
      return ErrCode::Value::WatUnexpectedToken;
    }
  }

  auto Text = Err.text(Source);

  // Annotation errors: (@id ...) where id must immediately follow @.
  auto AtPos = Text.find('@');
  if (AtPos != std::string_view::npos) {
    auto After = Text.substr(AtPos + 1);
    if (After.empty() || After.front() == ')' || After.front() == '(' ||
        After.front() == ' ' || After.front() == '\t' ||
        After.front() == '\n' || After.front() == '\r') {
      return ErrCode::Value::WatEmptyAnnotationId;
    }
    if (AtPos > 0 && Text[AtPos - 1] == '(') {
      return ErrCode::Value::WatUnclosedAnnotation;
    }
  }

  return ErrCode::Value::WatUnexpectedToken;
}

static void collectLeaves(Node N, std::vector<Node> &Leaves) {
  if (N.childCount() == 0) {
    Leaves.push_back(N);
    return;
  }
  for (uint32_t I = 0; I < N.childCount(); ++I) {
    collectLeaves(N.child(I), Leaves);
  }
}

uint32_t Converter::findOrCreateFuncType(AST::FunctionType FuncTy,
                                         AST::Module &Mod) {
  auto &Types = Mod.getTypeSection().getContent();
  for (uint32_t I = 0; I < Types.size(); ++I) {
    // Skip types in multi-type rec groups: they have a different identity
    // even if the structure matches. Only match standalone types or
    // single-type rec groups.
    if (auto RI = Types[I].getRecursiveInfo();
        RI.has_value() && RI->RecTypeSize > 1) {
      continue;
    }
    if (Types[I].getCompositeType().isFunc() &&
        Types[I].getCompositeType().getFuncType() == FuncTy) {
      return I;
    }
  }
  uint32_t NewIdx = static_cast<uint32_t>(Types.size());
  Types.emplace_back(std::move(FuncTy));
  Types.back().setTypeIndex(NewIdx);
  Syms.NextType = static_cast<uint32_t>(Types.size());
  return NewIdx;
}

// module ::= ( module id? modulefield* )
// Main entry: parse tree → AST::Module. Two passes: collect indices, then
// build.
Expect<AST::Module> Converter::convert(const Tree &ParseTree,
                                       std::string_view Src) {
  Source = Src;
  Syms.clear();
  PendingExports.clear();
  HasFuncDef = false;
  HasTableDef = false;
  HasMemoryDef = false;
  HasGlobalDef = false;
  HasStart = false;

  // Deferred parse error: if the tree has structural errors (extra parens,
  // etc.) but the main structure is still parseable, we attempt conversion
  // first. Semantic errors from conversion take priority.
  ErrCode::Value DeferredParseError = ErrCode::Value::Success;

  // Early scan for illegal characters and malformed UTF-8.
  // Outside strings and comments: control chars, DEL, and non-ASCII are
  // illegal. Inside strings: malformed UTF-8 is still rejected.
  // Comments and strings may contain arbitrary UTF-8.
  {
    auto *Data = reinterpret_cast<const unsigned char *>(Src.data());
    size_t Size = Src.size();
    bool InString = false;
    for (size_t I = 0; I < Size; ++I) {
      auto C = Data[I];
      if (InString) {
        if (C >= 0x80) {
          // Inside strings, valid non-ASCII UTF-8 is allowed,
          // but malformed UTF-8 is not.
          auto Err = classifyNonASCII(Data, Size, I);
          if (Err == ErrCode::Value::MalformedUTF8) {
            return Unexpect(Err);
          }
          // classifyNonASCII advanced I past the sequence; adjust for ++I.
          --I;
          continue;
        }
        if (C == '"') {
          // Count consecutive preceding backslashes.
          // Even count (including 0) means quote is real.
          size_t Backslashes = 0;
          while (Backslashes < I && Src[I - 1 - Backslashes] == '\\') {
            ++Backslashes;
          }
          if (Backslashes % 2 == 0) {
            InString = false;
          }
        }
        continue;
      }
      // Skip line comments: ;; to end of line.
      if (C == ';' && I + 1 < Size && Data[I + 1] == ';') {
        I += 2;
        while (I < Size && Data[I] != '\n') {
          ++I;
        }
        continue;
      }
      // Skip block comments: (; ... ;) with nesting.
      if (C == '(' && I + 1 < Size && Data[I + 1] == ';') {
        I += 2;
        unsigned Depth = 1;
        while (I < Size && Depth > 0) {
          if (Data[I] == '(' && I + 1 < Size && Data[I + 1] == ';') {
            ++Depth;
            ++I;
          } else if (Data[I] == ';' && I + 1 < Size && Data[I + 1] == ')') {
            --Depth;
            ++I;
          }
          ++I;
        }
        --I; // adjust for ++I in outer loop
        continue;
      }
      if (C == '"') {
        InString = true;
        continue;
      }
      if (C >= 0x80) {
        return Unexpect(classifyNonASCII(Data, Size, I));
      }
      if (C < 0x20 && C != '\t' && C != '\n' && C != '\r') {
        return Unexpect(ErrCode::Value::WatIllegalCharacter);
      }
      if (C == 0x7F) {
        return Unexpect(ErrCode::Value::WatIllegalCharacter);
      }
    }
  }

  Node Root = ParseTree.rootNode();
  if (Root.isNull()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  {
    std::vector<Node> Leaves;
    collectLeaves(Root, Leaves);

    // Check for empty identifiers (bare '$' or '$""') and empty annotation
    // ids before error classification, since they may cause secondary parse
    // errors.
    for (const auto &Leaf : Leaves) {
      if (!Leaf.isNamed()) {
        continue;
      }
      auto Text = Leaf.text(Source);
      auto Type = nodeType(Leaf);
      if ((Type == NodeType::Reserved && Text == "$"sv) ||
          (Type == NodeType::Id && Text == "$\"\""sv)) {
        return Unexpect(ErrCode::Value::WatEmptyIdentifier);
      }
      // Any other reserved token indicates adjacent tokens without
      // whitespace or a malformed numeric literal.  Skip '@'-prefixed
      // reserved tokens only when the tree has errors — those come from
      // failed annotation parsing and are handled by classifyError.
      if (Type == NodeType::Reserved) {
        if (!Text.empty() && Text.front() == '@') {
          if (Root.hasError()) {
            continue;
          }
          // @-prefixed reserved in a source with unclosed string after (@
          // → the annotation scanner failed due to unclosed string
          if (Source.find("(@"sv) != std::string_view::npos) {
            // Check for bare '"' without closing in the source
            auto AtPos = Source.find("(@"sv);
            auto QuotePos = Source.find('"', AtPos);
            if (QuotePos != std::string_view::npos) {
              auto CloseQuote = Source.find('"', QuotePos + 1);
              if (CloseQuote == std::string_view::npos) {
                return Unexpect(ErrCode::Value::WatUnclosedString);
              }
            }
          }
          return Unexpect(ErrCode::Value::WatUnknownOperator);
        }
        // Bare '"' or unclosed string literal → unclosed string
        if (!Text.empty() && Text.front() == '"') {
          auto Close = Text.find('"', 1);
          if (Close == std::string_view::npos) {
            return Unexpect(ErrCode::Value::WatUnclosedString);
          }
        }
        return Unexpect(ErrCode::Value::WatUnknownOperator);
      }
    }

    // Check (@"") — empty quoted annotation id
    // Check (@"\n") etc — control char in quoted annotation id
    // Note: (@ ...) with bare @ is caught by the leaf adjacency scan above.
    {
      auto Pos = Source.find("(@\""sv);
      while (Pos != std::string_view::npos) {
        size_t J = Pos + 3; // after (@"
        // Check for empty string: (@"")
        if (J < Source.size() && Source[J] == '"') {
          return Unexpect(ErrCode::Value::WatEmptyAnnotationId);
        }
        // Check for control chars and malformed UTF-8 inside quoted
        // annotation id
        while (J < Source.size() && Source[J] != '"') {
          auto C = static_cast<unsigned char>(Source[J]);
          if (C < 0x20 || C == 0x7F) {
            return Unexpect(ErrCode::Value::WatEmptyAnnotationId);
          }
          if (C >= 0x80) {
            // Validate UTF-8 sequence
            auto *Data = reinterpret_cast<const unsigned char *>(Source.data());
            auto Len = Source.size();
            uint32_t Needed = 0;
            if ((C & 0xE0) == 0xC0) {
              Needed = 2;
            } else if ((C & 0xF0) == 0xE0) {
              Needed = 3;
            } else if ((C & 0xF8) == 0xF0) {
              Needed = 4;
            } else {
              return Unexpect(ErrCode::Value::MalformedUTF8);
            }
            if (J + Needed > Len) {
              return Unexpect(ErrCode::Value::MalformedUTF8);
            }
            for (uint32_t K = 1; K < Needed; ++K) {
              if ((Data[J + K] & 0xC0) != 0x80) {
                return Unexpect(ErrCode::Value::MalformedUTF8);
              }
            }
            J += Needed;
            continue;
          }
          if (Source[J] == '\\' && J + 1 < Source.size()) {
            // Check \HH hex escapes for non-ASCII bytes
            if (J + 2 < Source.size()) {
              int D1 = hexDigit(Source[J + 1]);
              int D2 = hexDigit(Source[J + 2]);
              if (D1 >= 0 && D2 >= 0) {
                unsigned Byte =
                    static_cast<unsigned>(D1) * 16 + static_cast<unsigned>(D2);
                if (Byte >= 0x80) {
                  return Unexpect(ErrCode::Value::MalformedUTF8);
                }
                J += 2; // skip HH (outer ++J handles the \)
              }
            }
            ++J; // skip escape
          }
          ++J;
        }
        Pos = Source.find("(@\""sv, Pos + 1);
      }
    }

    // Check for bare empty annotation ids: (@), (@ x), (@( etc.
    // The annotation scanner may consume these without producing errors.
    // Only check when the tree has no module content — inside annotations,
    // these patterns are just data content and not actual annotation syntax.
    // With annotations visible, check that all named children are either
    // annotation or ERROR nodes (no real module content).
    {
      bool HasModuleContent = false;
      {
        Cursor RC(Root);
        while (RC.valid()) {
          if (nodeType(RC.node()) != NodeType::Error) {
            HasModuleContent = true;
            break;
          }
          RC.next();
        }
      }
      if (!HasModuleContent) {
        auto Pos = Source.find("(@"sv);
        while (Pos != std::string_view::npos) {
          size_t J = Pos + 2; // after (@
          if (J < Source.size()) {
            auto C = Source[J];
            if (C == ')' || C == ' ' || C == '\t' || C == '\n' || C == '\r' ||
                C == '(') {
              return Unexpect(ErrCode::Value::WatEmptyAnnotationId);
            }
          } else {
            // (@  at end of source
            return Unexpect(ErrCode::Value::WatEmptyAnnotationId);
          }
          Pos = Source.find("(@"sv, J);
        }
      }
    }

    if (Root.hasError()) {
      // Before classifying the error, check if adjacent non-paren tokens
      // exist in the source. These produce ERROR nodes but the real issue
      // is missing whitespace -> "unknown operator".
      for (size_t I = 1; I < Leaves.size(); ++I) {
        if (Leaves[I - 1].endByte() != Leaves[I].startByte()) {
          continue;
        }
        auto PrevText = Leaves[I - 1].text(Source);
        auto CurrText = Leaves[I].text(Source);
        if (PrevText == "("sv || PrevText == ")"sv || CurrText == "("sv ||
            CurrText == ")"sv) {
          continue;
        }
        // Check if one of the adjacent tokens is a string literal
        // (starts with '"'). Adjacent strings indicate missing whitespace.
        // Exception: '$' adjacent to '"' is an identifier issue, not
        // spacing.
        if ((!PrevText.empty() && PrevText.front() == '"') ||
            (!CurrText.empty() && CurrText.front() == '"')) {
          if (PrevText == "$"sv || CurrText == "$"sv) {
            // Let classifyError handle $"..." patterns
          } else {
            return Unexpect(ErrCode::Value::WatUnknownOperator);
          }
        }
      }
      // Defer the parse error: attempt conversion first so that semantic
      // errors (e.g. "i8 constant out of range" in a shuffle with a float
      // lane) take priority over structural parse errors (extra parens).
      DeferredParseError = classifyError(Root);
    }

    // Validate that all tokens are properly separated by whitespace.
    // Tree-sitter may accept adjacent tokens without spaces (e.g. "i32.const0"
    // as "i32.const" + "0"), but the WAT spec requires whitespace.
    // Skip if we already have a deferred parse error (tree has structural
    // issues; the deferred error is more specific).
    if (DeferredParseError == ErrCode::Value::Success)
      for (size_t I = 1; I < Leaves.size(); ++I) {
        if (Leaves[I - 1].endByte() != Leaves[I].startByte()) {
          continue;
        }
        // Two adjacent leaf tokens with no gap.
        // Exception: '(' ')' can be adjacent to anything.
        auto PrevText = Leaves[I - 1].text(Source);
        auto CurrText = Leaves[I].text(Source);
        if (PrevText == "("sv || PrevText == ")"sv || CurrText == "("sv ||
            CurrText == ")"sv) {
          continue;
        }
        // Two non-paren tokens directly adjacent -> malformed.
        // If the adjacent tokens form a float-like pattern (e.g., "1" + ".5"
        // or "15" + ".0") used where an integer is expected (SIMD lane),
        // it's "unexpected token" rather than "unknown operator".
        if ((!PrevText.empty() &&
             std::isdigit(static_cast<unsigned char>(PrevText.back()))) &&
            (!CurrText.empty() && CurrText.front() == '.')) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        if ((!PrevText.empty() && PrevText.back() == '.') &&
            (!CurrText.empty() &&
             std::isdigit(static_cast<unsigned char>(CurrText.front())))) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        return Unexpect(ErrCode::Value::WatUnknownOperator);
      }
  } // Leaves scope

  // Pass 1: collect all identifiers and assign indices
  EXPECTED_TRY(collectIndices(Root));

  // Pass 2: build AST::Module
  AST::Module Mod;
  Mod.getMagic() = {0x00, 0x61, 0x73, 0x6D};
  Mod.getVersion() = {0x01, 0x00, 0x00, 0x00};
  CurMod = &Mod;
  EXPECTED_TRY(buildModule(Root, Mod));

  // Set TypeIndex on each type in the type section.
  // The binary loader does this during loading; we must do it here.
  {
    auto &Types = Mod.getTypeSection().getContent();
    for (uint32_t I = 0; I < Types.size(); ++I) {
      Types[I].setTypeIndex(I);
    }
  }

  // Set DefType pointers on tag types (both in tag section and import section).
  // The binary loader does this in setTagFunctionType(); we must do it here.
  {
    auto &TypeVec = Mod.getTypeSection().getContent();
    for (auto &TgType : Mod.getTagSection().getContent()) {
      auto TypeIdx = TgType.getTypeIdx();
      if (TypeIdx < TypeVec.size()) {
        TgType.setDefType(&TypeVec[TypeIdx]);
      }
    }
    for (auto &Desc : Mod.getImportSection().getContent()) {
      if (Desc.getExternalType() == ExternalType::Tag) {
        auto &TgType = Desc.getExternalTagType();
        auto TypeIdx = TgType.getTypeIdx();
        if (TypeIdx < TypeVec.size()) {
          TgType.setDefType(&TypeVec[TypeIdx]);
        }
      }
    }
  }

  // Emit deferred inline exports
  for (const auto &Exp : PendingExports) {
    auto &Exports = Mod.getExportSection().getContent();
    Exports.emplace_back();
    Exports.back().setExternalName(Exp.Name);
    Exports.back().setExternalType(Exp.Type);
    Exports.back().setExternalIndex(Exp.Index);
  }

  // Set data count section if data segments exist
  auto &DataSec = Mod.getDataSection().getContent();
  if (!DataSec.empty()) {
    Mod.getDataCountSection().setContent(static_cast<uint32_t>(DataSec.size()));
  }

  // If conversion succeeded but there was a deferred parse error, return it.
  if (DeferredParseError != ErrCode::Value::Success) {
    return Unexpect(DeferredParseError);
  }

  return Mod;
}

// Pass 1: walk module fields to assign indices for all named definitions.
Expect<void> Converter::collectIndices(Node Root) {
  // Root is either a root node (containing module sexprs) or already a
  // sexpr. With the simplified grammar, root maps to Sexpr.
  // Check if it contains a module sexpr, or is bare module fields.
  Cursor C(Root);
  while (C.valid()) {
    Node Child = C.node();
    C.next();
    if (nodeType(Child) == NodeType::Sexpr) {
      Cursor FC(Child);
      if (peekType(FC) != NodeType::Keyword) {
        continue;
      }
      auto KW = nodeText(FC.node());
      if (KW == "module"sv) {
        // Recurse into module's children (skip the "module" keyword)
        Cursor IC(Child);
        while (IC.valid()) {
          Node GC = IC.node();
          IC.next();
          if (nodeType(GC) == NodeType::Sexpr) {
            EXPECTED_TRY(collectModuleField(GC));
          }
        }
      } else {
        // Bare module field (no wrapping module sexpr)
        EXPECTED_TRY(collectModuleField(Child));
      }
    }
  }
  return {};
}

// modulefield ::= type | import | func | table | memory | global | export
//               | start | elem | data | tag
// Register identifiers and assign index-space positions for one field.
Expect<void> Converter::collectModuleField(Node Field) {
  // Field should be a sexpr with a leading keyword.
  Cursor FC(Field);
  if (peekType(FC) != NodeType::Keyword) {
    return {}; // Skip non-keyword sexprs
  }
  auto KW = nodeText(FC.node());

  // Extract identifier (Id child) immediately after the leading keyword.
  // In WAT, the optional identifier always comes right after the keyword.
  auto GetId = [&](Node N) -> Expect<std::string> {
    // Skip leading keyword(s), then check for Id.
    Cursor GC(N);
    while (GC.valid()) {
      auto Type = nodeType(GC.node());
      if (Type == NodeType::Id) {
        return decodeIdentifier(nodeText(GC.node()));
      }
      if (Type != NodeType::Keyword) {
        break; // Id must come before any non-keyword child
      }
      GC.next();
    }
    return std::string();
  };

  auto CheckDup = [](auto &Map, const std::string &Id, uint32_t Val,
                     ErrCode::Value Err) -> Expect<void> {
    if (!Id.empty()) {
      if (!Map.emplace(Id, Val).second) {
        return Unexpect(Err);
      }
    }
    return {};
  };

  if (KW == "type"sv) {
    EXPECTED_TRY(auto Id, GetId(Field));
    if (!Id.empty()) {
      Syms.Types[Id] = Syms.NextType;
    }
    Syms.NextType++;
  } else if (KW == "rec"sv) {
    // Recursive type: each child (type ...) sexpr gets an index
    Cursor RC(Field);
    while (RC.valid()) {
      Node Child = RC.node();
      RC.next();
      Cursor FC(Child);
      auto ChildKW =
          peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
      if (nodeType(Child) == NodeType::Sexpr && ChildKW == "type"sv) {
        EXPECTED_TRY(auto Id, GetId(Child));
        if (!Id.empty()) {
          Syms.Types[Id] = Syms.NextType;
        }
        Syms.NextType++;
      }
    }
  } else if (KW == "import"sv) {
    // Imports consume indices in their respective spaces.
    // Find the descriptor sexpr (func/table/memory/global/tag) inside
    Cursor IC(Field);
    while (IC.valid()) {
      Node Child = IC.node();
      IC.next();
      if (nodeType(Child) != NodeType::Sexpr) {
        continue;
      }
      Cursor FC(Child);
      auto DescKW =
          peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
      if (DescKW != "func"sv && DescKW != "table"sv && DescKW != "memory"sv &&
          DescKW != "global"sv && DescKW != "tag"sv) {
        continue;
      }
      EXPECTED_TRY(auto Id, GetId(Child));
      if (DescKW == "func"sv) {
        EXPECTED_TRY(CheckDup(Syms.Funcs, Id, Syms.NextFunc,
                              ErrCode::Value::WatDuplicateFunc));
        Syms.NextFunc++;
      } else if (DescKW == "table"sv) {
        EXPECTED_TRY(CheckDup(Syms.Tables, Id, Syms.NextTable,
                              ErrCode::Value::WatDuplicateTable));
        Syms.NextTable++;
      } else if (DescKW == "memory"sv) {
        EXPECTED_TRY(CheckDup(Syms.Memories, Id, Syms.NextMemory,
                              ErrCode::Value::WatDuplicateMemory));
        Syms.NextMemory++;
      } else if (DescKW == "global"sv) {
        EXPECTED_TRY(CheckDup(Syms.Globals, Id, Syms.NextGlobal,
                              ErrCode::Value::WatDuplicateGlobal));
        Syms.NextGlobal++;
      } else if (DescKW == "tag"sv) {
        if (!Id.empty()) {
          Syms.Tags[Id] = Syms.NextTag;
        }
        Syms.NextTag++;
      }
    }
  } else if (KW == "func"sv) {
    EXPECTED_TRY(auto Id, GetId(Field));
    EXPECTED_TRY(CheckDup(Syms.Funcs, Id, Syms.NextFunc,
                          ErrCode::Value::WatDuplicateFunc));
    Syms.NextFunc++;
  } else if (KW == "table"sv) {
    EXPECTED_TRY(auto Id, GetId(Field));
    EXPECTED_TRY(CheckDup(Syms.Tables, Id, Syms.NextTable,
                          ErrCode::Value::WatDuplicateTable));
    Syms.NextTable++;
  } else if (KW == "memory"sv) {
    EXPECTED_TRY(auto Id, GetId(Field));
    EXPECTED_TRY(CheckDup(Syms.Memories, Id, Syms.NextMemory,
                          ErrCode::Value::WatDuplicateMemory));
    Syms.NextMemory++;
  } else if (KW == "global"sv) {
    EXPECTED_TRY(auto Id, GetId(Field));
    EXPECTED_TRY(CheckDup(Syms.Globals, Id, Syms.NextGlobal,
                          ErrCode::Value::WatDuplicateGlobal));
    Syms.NextGlobal++;
  } else if (KW == "tag"sv) {
    EXPECTED_TRY(auto Id, GetId(Field));
    if (!Id.empty()) {
      Syms.Tags[Id] = Syms.NextTag;
    }
    Syms.NextTag++;
  } else if (KW == "elem"sv) {
    EXPECTED_TRY(auto Id, GetId(Field));
    if (!Id.empty()) {
      Syms.Elems[Id] = Syms.NextElem;
    }
    Syms.NextElem++;
  } else if (KW == "data"sv) {
    EXPECTED_TRY(auto Id, GetId(Field));
    if (!Id.empty()) {
      Syms.Datas[Id] = Syms.NextData;
    }
    Syms.NextData++;
  }
  // export, start, etc. don't define names — skip.
  return {};
}

// Pass 2: convert each module field into AST sections.
// Types are processed first, then all other fields in source order.
Expect<void> Converter::buildModule(Node Root, AST::Module &Mod) {
  // Collect module fields (sexpr children).
  std::vector<Node> Fields;
  {
    Cursor C(Root);
    while (C.valid()) {
      Node Child = C.node();
      C.next();
      if (nodeType(Child) == NodeType::Sexpr) {
        Cursor FC(Child);
        if (peekType(FC) != NodeType::Keyword) {
          continue;
        }
        auto KW = nodeText(FC.node());
        if (KW == "module"sv) {
          // Recurse into module children
          Cursor IC(Child);
          while (IC.valid()) {
            Node GC = IC.node();
            IC.next();
            if (nodeType(GC) == NodeType::Sexpr) {
              Fields.push_back(GC);
            }
          }
        } else {
          Fields.push_back(Child);
        }
      }
    }
  }

  // Process type definitions first so that explicit types get their correct
  // indices before implicit function types are created.
  for (auto &Field : Fields) {
    Cursor FC(Field);
    auto KW = peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
    if (KW == "type"sv || KW == "rec"sv) {
      EXPECTED_TRY(buildModuleField(Field, Mod));
    }
  }

  // Then process all other fields in source order (skip types already done).
  for (auto &Field : Fields) {
    Cursor FC(Field);
    auto KW = peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
    if (KW == "type"sv || KW == "rec"sv) {
      continue;
    }
    EXPECTED_TRY(buildModuleField(Field, Mod));
  }

  return {};
}

// Dispatch a single module field to the appropriate converter by keyword.
Expect<void> Converter::buildModuleField(Node Field, AST::Module &Mod) {
  Cursor FC(Field);
  if (peekType(FC) != NodeType::Keyword) {
    return {};
  }
  auto KW = nodeText(FC.node());

  // Check if a func/table/memory/global node has an inline import, e.g.
  // (func (import "m" "f") ...). These are imports, not definitions.
  auto hasInlineImport = [this](Node N) -> bool {
    Cursor HC(N);
    while (HC.valid()) {
      Node Child = HC.node();
      HC.next();
      Cursor FC(Child);
      auto KW = peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
      if (nodeType(Child) == NodeType::Sexpr && KW == "import"sv) {
        return true;
      }
    }
    return false;
  };

  // Check import ordering: ALL imports must come before ANY definitions.
  auto checkImportOrder = [this]() -> Expect<void> {
    if (HasFuncDef) {
      return Unexpect(ErrCode::Value::WatImportAfterFunc);
    } else if (HasTableDef) {
      return Unexpect(ErrCode::Value::WatImportAfterTable);
    } else if (HasMemoryDef) {
      return Unexpect(ErrCode::Value::WatImportAfterMemory);
    } else if (HasGlobalDef) {
      return Unexpect(ErrCode::Value::WatImportAfterGlobal);
    }
    return {};
  };

  if (KW == "type"sv) {
    return convertTypedef(Field, Mod);
  } else if (KW == "rec"sv) {
    return convertRecType(Field, Mod);
  } else if (KW == "import"sv) {
    EXPECTED_TRY(checkImportOrder());
    return convertImport(Field, Mod);
  } else if (KW == "export"sv) {
    return convertExport(Field, Mod);
  } else if (KW == "func"sv) {
    if (hasInlineImport(Field)) {
      EXPECTED_TRY(checkImportOrder());
    } else {
      HasFuncDef = true;
    }
    return convertFunc(Field, Mod);
  } else if (KW == "table"sv) {
    if (hasInlineImport(Field)) {
      EXPECTED_TRY(checkImportOrder());
    } else {
      HasTableDef = true;
    }
    return convertTable(Field, Mod);
  } else if (KW == "memory"sv) {
    if (hasInlineImport(Field)) {
      EXPECTED_TRY(checkImportOrder());
    } else {
      HasMemoryDef = true;
    }
    return convertMemory(Field, Mod);
  } else if (KW == "global"sv) {
    if (hasInlineImport(Field)) {
      EXPECTED_TRY(checkImportOrder());
    } else {
      HasGlobalDef = true;
    }
    return convertGlobal(Field, Mod);
  } else if (KW == "start"sv) {
    if (HasStart) {
      return Unexpect(ErrCode::Value::WatMultipleStart);
    }
    HasStart = true;
    return convertStart(Field, Mod);
  } else if (KW == "elem"sv) {
    return convertElem(Field, Mod);
  } else if (KW == "data"sv) {
    return convertData(Field, Mod);
  } else if (KW == "tag"sv) {
    return convertTag(Field, Mod);
  }
  // Unknown keywords silently ignored (annotations handled as extras).
  return {};
}

} // namespace WasmEdge::WAT
