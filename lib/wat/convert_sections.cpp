// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2026 Second State INC

#include "common/errcode.h"
#include "converter.h"
#include "wat/wat_util.h"

#include <algorithm>
#include <limits>
#include <string>

using namespace std::string_view_literals;

namespace WasmEdge::WAT {

// typeuse ::= (type typeidx) (param valtype*)* (result valtype*)*
// Resolves an explicit (type $idx) or inline param/result list to a type index.
Expect<uint32_t> Converter::resolveTypeUse(Cursor &C, AST::Module &Mod,
                                           bool CheckMismatch,
                                           bool AcceptParamId) {
  // First, check for (type $idx) sexpr
  AST::FunctionType FuncTy;
  uint32_t TypeUseIdx = 0;
  bool HasType = false;
  bool HasParam = false;
  bool HasResult = false;
  while (C.valid()) {
    Node Child = C.node();
    if (nodeType(Child) == NodeType::Sexpr) {
      Cursor FC(Child);
      auto Keyword =
          peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
      if (Keyword == "type"sv) {
        HasType = true;
        if (HasParam || HasResult) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        // Find the type index (Id or Number child)
        Cursor TC(Child);
        TC.next(); // skip keyword
        if (!TC.valid()) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        Node TChild = TC.node();
        auto TType = nodeType(TChild);
        if (TType != NodeType::Id && TType != NodeType::U) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        EXPECTED_TRY(TypeUseIdx, Syms.resolveType(nodeText(TChild)));
      } else if (Keyword == "param"sv) {
        HasParam = true;
        if (HasResult) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        Cursor PC(Child);
        PC.next(); // skip "param" keyword
        while (PC.valid()) {
          if (nodeType(PC.node()) == NodeType::Id) {
            if (!AcceptParamId) {
              return Unexpect(ErrCode::Value::WatUnexpectedToken);
            }
            PC.next();
            continue;
          }
          EXPECTED_TRY(auto VT, convertValType(PC.node()));
          FuncTy.getParamTypes().push_back(VT);
          PC.next();
        }
      } else if (Keyword == "result"sv) {
        HasResult = true;
        Cursor RC(Child);
        RC.next(); // skip "result" keyword
        while (RC.valid()) {
          EXPECTED_TRY(auto VT, convertValType(RC.node()));
          FuncTy.getReturnTypes().push_back(VT);
          RC.next();
        }
      } else {
        break;
      }
      C.next();
    } else {
      break;
    }
  }
  // If both type_use and inline type, check for mismatch
  if (CheckMismatch && HasType && (HasParam || HasResult)) {
    auto &Types = Mod.getTypeSection().getContent();
    if (TypeUseIdx < Types.size()) {
      const auto &RefTy = Types[TypeUseIdx].getCompositeType().getFuncType();
      if (FuncTy.getParamTypes() != RefTy.getParamTypes() ||
          FuncTy.getReturnTypes() != RefTy.getReturnTypes()) {
        return Unexpect(ErrCode::Value::WatInlineFuncType);
      }
    }
  }
  // If only type_use, return its index
  if (HasType) {
    return TypeUseIdx;
  }
  // Only inline type: find or create
  return findOrCreateFuncType(std::move(FuncTy), Mod);
}

// limits ::= u32 u32? | i32 u32 u32? shared? | i64 u32 u32? shared?
Expect<AST::Limit> Converter::parseLimits(Cursor &C) {
  // With the simplified grammar, limits are just number tokens as children
  // of the parent sexpr. We also check for "shared" and address type keywords.
  bool Is64 = false;
  bool IsShared = false;
  std::vector<uint64_t> Nats;

  while (C.valid()) {
    Node Child = C.node();
    auto CType = nodeType(Child);
    auto CText = nodeText(Child);
    if (CType == NodeType::U && Nats.size() < 2) {
      EXPECTED_TRY(auto Val, parseUint(CText));
      Nats.push_back(Val);
    } else if (CType == NodeType::Keyword) {
      if (CText == "i64"sv) {
        Is64 = true;
      } else if (CText == "shared"sv) {
        IsShared = true;
      } else {
        break;
      }
    } else {
      break;
    }
    C.next();
  }

  if (!Is64 && !Conf.hasProposal(Proposal::Memory64)) {
    for (auto V : Nats) {
      if (V > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return Unexpect(ErrCode::Value::WatI32ConstOutOfRange);
      }
    }
  }
  if (Nats.size() == 2) {
    return AST::Limit(Nats[0], Nats[1], Is64, IsShared);
  } else if (Nats.size() == 1) {
    if (IsShared) {
      AST::Limit Lim(Nats[0], Is64);
      Lim.setType(Is64 ? AST::Limit::LimitType::I64SharedNoMax
                       : AST::Limit::LimitType::SharedNoMax);
      return Lim;
    }
    return AST::Limit(Nats[0], Is64);
  } else {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
}

// tabletype ::= limits reftype
Expect<AST::TableType> Converter::parseTabletype(Cursor &C) {
  // Table type: addr_type? limits reftype
  EXPECTED_TRY(auto Lim, parseLimits(C));
  ValType RefTy(TypeCode::FuncRef);
  if (!C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  EXPECTED_TRY(RefTy, convertRefType(C.node()));
  C.next();
  return AST::TableType(RefTy, Lim);
}

// globaltype ::= valtype | ( mut valtype )
Expect<AST::GlobalType> Converter::parseGlobaltype(Cursor &C) {
  // With simplified grammar, could be a keyword or a (mut valtype) sexpr
  auto GChild = C.node();
  Cursor MC(GChild);
  auto KW = peekType(MC) == NodeType::Keyword ? nodeText(MC.node()) : ""sv;
  if (nodeType(GChild) == NodeType::Sexpr && KW == "mut"sv) {
    // (mut valtype)
    MC.next(); // skip "mut" keyword
    while (MC.valid()) {
      Node Child = MC.node();
      if (nodeType(Child) == NodeType::Keyword && nodeText(Child) != "mut"sv) {
        EXPECTED_TRY(auto VT, convertValType(Child));
        C.next();
        return AST::GlobalType(VT, ValMut::Var);
      } else if (nodeType(Child) == NodeType::Sexpr) {
        EXPECTED_TRY(auto VT, convertValType(Child));
        C.next();
        return AST::GlobalType(VT, ValMut::Var);
      }
      MC.next();
    }
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  // Plain valtype
  EXPECTED_TRY(auto VT, convertValType(GChild));
  C.next();
  return AST::GlobalType(VT, ValMut::Const);
}

// import ::= ( import name name importdesc )
// importdesc ::= ( func id? typeuse )
//              | ( table id? tabletype )
//              | ( memory id? memtype )
//              | ( global id? globaltype )
//              | ( tag id? typeuse )
Expect<void> Converter::convertImport(Node N, AST::Module &Mod) {
  AST::ImportDesc Desc;
  Cursor C(N);

  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "import"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }

  if (peekType(C) != NodeType::String) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  auto RawModName = nodeText(C.node());
  C.next();
  EXPECTED_TRY(auto ModName, parseString(RawModName));
  Desc.setModuleName(ModName);

  if (peekType(C) != NodeType::String) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  auto RawExtName = nodeText(C.node());
  C.next();
  EXPECTED_TRY(auto ExtName, parseString(RawExtName));
  Desc.setExternalName(ExtName);

  auto Child = C.node();
  if (nodeType(Child) != NodeType::Sexpr) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  C.next();
  if (C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  Cursor CC(Child);
  auto KW = peekType(CC) == NodeType::Keyword ? nodeText(CC.node()) : ""sv;
  CC.next(); // skip keyword
  if (peekType(CC) == NodeType::Id) {
    CC.next();
  }

  if (KW == "func"sv) {
    Desc.setExternalType(ExternalType::Function);
    EXPECTED_TRY(auto Idx, resolveTypeUse(CC, Mod, false));
    Desc.setExternalFuncTypeIdx(Idx);
  } else if (KW == "table"sv) {
    Desc.setExternalType(ExternalType::Table);
    EXPECTED_TRY(auto TT, parseTabletype(CC));
    Desc.getExternalTableType() = TT;
  } else if (KW == "memory"sv) {
    Desc.setExternalType(ExternalType::Memory);
    EXPECTED_TRY(auto Lim, parseLimits(CC));
    Desc.getExternalMemoryType() = AST::MemoryType(Lim);
  } else if (KW == "global"sv) {
    Desc.setExternalType(ExternalType::Global);
    if (!CC.valid()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    EXPECTED_TRY(auto GT, parseGlobaltype(CC));
    Desc.getExternalGlobalType() = GT;
  } else if (KW == "tag"sv) {
    Desc.setExternalType(ExternalType::Tag);
    EXPECTED_TRY(auto Idx, resolveTypeUse(CC, Mod, false));
    Desc.getExternalTagType().setTypeIdx(Idx);
  } else {
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }

  Mod.getImportSection().getContent().push_back(std::move(Desc));
  return {};
}

// export ::= ( export name exportdesc )
// exportdesc ::= ( func funcidx ) | ( table tableidx )
//              | ( memory memidx ) | ( global globalidx )
Expect<void> Converter::convertExport(Node N, AST::Module &Mod) {
  AST::ExportDesc Desc;

  Cursor C(N);
  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "export"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }

  if (peekType(C) != NodeType::String) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  auto RawName = nodeText(C.node());
  C.next();
  EXPECTED_TRY(auto Name, parseString(RawName));
  Desc.setExternalName(Name);

  auto Child = C.node();
  if (nodeType(Child) != NodeType::Sexpr) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  C.next();

  if (C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  Cursor IC(Child);
  auto KW = peekType(IC) == NodeType::Keyword ? nodeText(IC.node()) : ""sv;
  ExternalType ExtType;
  SymbolTable::IndexSpace Space;
  if (KW == "func"sv) {
    ExtType = ExternalType::Function;
    Space = SymbolTable::IndexSpace::Func;
  } else if (KW == "table"sv) {
    ExtType = ExternalType::Table;
    Space = SymbolTable::IndexSpace::Table;
  } else if (KW == "memory"sv) {
    ExtType = ExternalType::Memory;
    Space = SymbolTable::IndexSpace::Memory;
  } else if (KW == "global"sv) {
    ExtType = ExternalType::Global;
    Space = SymbolTable::IndexSpace::Global;
  } else if (KW == "tag"sv) {
    ExtType = ExternalType::Tag;
    Space = SymbolTable::IndexSpace::Tag;
  } else {
    return Unexpect(ErrCode::Value::WatUnknownOperator);
  }
  Desc.setExternalType(ExtType);

  // Find the index (Id or Number child)
  IC.next(); // skip keyword
  Node IdxNode = IC.valid() ? IC.node() : Node{};
  auto IT = nodeType(IdxNode);
  if (IT != NodeType::Id && IT != NodeType::U) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  EXPECTED_TRY(auto Idx, Syms.resolve(Space, nodeText(IdxNode)));
  Desc.setExternalIndex(Idx);

  Mod.getExportSection().getContent().push_back(std::move(Desc));
  return {};
}

// func ::= ( func id? (( export name ))* (( import name name ))?
//          typeuse local* expr )
// local ::= ( local id? valtype )
// expr  ::= instr*  (implicit end)
Expect<void> Converter::convertFunc(Node N, AST::Module &Mod) {
  // Calculate function index
  uint32_t ImportedFuncs = 0;
  for (const auto &Imp : Mod.getImportSection().getContent()) {
    if (Imp.getExternalType() == ExternalType::Function) {
      ImportedFuncs++;
    }
  }
  uint32_t FuncIdx =
      ImportedFuncs +
      static_cast<uint32_t>(Mod.getFunctionSection().getContent().size());

  Cursor C(N);
  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "func"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }

  if (peekType(C) == NodeType::Id) {
    C.next();
  }

  // Check for inline import: (import "mod" "name") child sexpr
  bool HasInlineImport = false;
  Node InlineImportNode;
  while (peekType(C) == NodeType::Sexpr) {
    Node Child = C.node();
    Cursor FC(Child);
    auto Keyword =
        peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
    if (Keyword == "import"sv) {
      HasInlineImport = true;
      InlineImportNode = Child;
      C.next();
    } else if (Keyword == "export"sv) {
      Cursor C2(Child);
      C2.next(); // skip keyword at position 0
      if (peekType(C2) != NodeType::String) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto RawName = nodeText(C2.node());
      C2.next();
      EXPECTED_TRY(auto Name, parseString(RawName));
      PendingExports.push_back(
          {std::string(Name), ExternalType::Function, FuncIdx});
      C.next();
    } else {
      break;
    }
  }

  if (HasInlineImport) {
    AST::ImportDesc Desc;
    Desc.setExternalType(ExternalType::Function);
    Cursor C2(InlineImportNode);
    C2.next(); // skip keyword at position 0
    uint32_t NameCount = 0;
    while (C2.valid()) {
      if (peekType(C2) != NodeType::String) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto RawName = nodeText(C2.node());
      C2.next();
      EXPECTED_TRY(auto Name, parseString(RawName));
      if (NameCount == 0) {
        Desc.setModuleName(Name);
      } else {
        Desc.setExternalName(Name);
      }
      ++NameCount;
    }
    EXPECTED_TRY(auto Idx, resolveTypeUse(C, Mod, true));
    Desc.setExternalFuncTypeIdx(Idx);
    Mod.getImportSection().getContent().push_back(std::move(Desc));
    return {};
  }

  // Regular function definition
  auto TypeUseC = C.copy();
  EXPECTED_TRY(auto TypeIdx, resolveTypeUse(C, Mod, true));

  // Clear per-function locals
  Syms.clearLocals();

  // Register param identifiers as locals
  while (TypeUseC.valid()) {
    Node Child = TypeUseC.node();
    if (sexprMatch(TypeUseC, "param"sv)) {
      for (Cursor PC(Child); PC.next();) {
        Node PChild = PC.node();
        if (nodeType(PChild) == NodeType::Id) {
          EXPECTED_TRY(auto Id, decodeIdentifier(nodeText(PChild)));
          if (!Syms.Locals.emplace(std::move(Id), Syms.NextLocal).second) {
            return Unexpect(ErrCode::Value::WatDuplicateLocal);
          }
        } else {
          Syms.NextLocal++;
        }
      }
    } else {
      break;
    }
    TypeUseC.next();
  }

  // Ensure NextLocal accounts for all params from the resolved type
  {
    auto &Types = Mod.getTypeSection().getContent();
    if (TypeIdx >= static_cast<uint32_t>(Types.size())) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    uint32_t NumParams = static_cast<uint32_t>(
        Types[TypeIdx].getCompositeType().getFuncType().getParamTypes().size());
    if (Syms.NextLocal < NumParams) {
      Syms.NextLocal = NumParams;
    }
  }

  // Parse local declarations and build CodeSegment
  AST::CodeSegment CodeSeg;

  while (peekType(C) == NodeType::Sexpr) {
    Node Child = C.node();
    if (sexprMatch(C, "local"sv)) {
      std::string LocalId;
      for (Cursor LC(Child); LC.next();) {
        Node LChild = LC.node();
        auto Type = nodeType(LChild);
        if (Type == NodeType::Id) {
          EXPECTED_TRY(LocalId, decodeIdentifier(nodeText(LChild)));
        } else if (Type == NodeType::Keyword || Type == NodeType::Sexpr) {
          EXPECTED_TRY(auto VT, convertValType(LChild));
          if (!LocalId.empty()) {
            if (!Syms.Locals.emplace(std::move(LocalId), Syms.NextLocal)
                     .second) {
              return Unexpect(ErrCode::Value::WatDuplicateLocal);
            }
            LocalId = {};
          }
          CodeSeg.getLocals().emplace_back(1, VT);
          Syms.NextLocal++;
        }
      }
    } else {
      break;
    }
    C.next();
  }

  // Parse body instructions
  EXPECTED_TRY(convertExpression(C, CodeSeg.getExpr()));

  if (C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  Mod.getFunctionSection().getContent().push_back(TypeIdx);
  Mod.getCodeSection().getContent().push_back(std::move(CodeSeg));
  return {};
}

// table ::= ( table id? (( export name ))* (( import name name ))? tabletype )
//        | ( table id? reftype ( elem ... ) )
Expect<void> Converter::convertTable(Node N, AST::Module &Mod) {
  uint32_t TableIdx = 0;
  for (const auto &Imp : Mod.getImportSection().getContent()) {
    if (Imp.getExternalType() == ExternalType::Table) {
      TableIdx++;
    }
  }
  TableIdx += static_cast<uint32_t>(Mod.getTableSection().getContent().size());

  Cursor C(N);
  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "table"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }

  if (peekType(C) == NodeType::Id) {
    C.next();
  }

  bool HasInlineImport = false;
  Node InlineImportNode;
  while (peekType(C) == NodeType::Sexpr) {
    Node Child = C.node();
    Cursor FC(Child);
    auto Keyword =
        peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
    if (Keyword == "import"sv) {
      HasInlineImport = true;
      InlineImportNode = Child;
      C.next();
    } else if (Keyword == "export"sv) {
      Cursor C2(Child);
      C2.next(); // skip keyword at position 0
      if (peekType(C2) != NodeType::String) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto RawName = nodeText(C2.node());
      C2.next();
      EXPECTED_TRY(auto Name, parseString(RawName));
      PendingExports.push_back(
          {std::string(Name), ExternalType::Table, TableIdx});
      C.next();
    } else {
      break;
    }
  }

  if (HasInlineImport) {
    AST::ImportDesc Desc;
    Desc.setExternalType(ExternalType::Table);
    Cursor C2(InlineImportNode);
    C2.next(); // skip keyword at position 0
    uint32_t NameCount = 0;
    while (C2.valid()) {
      if (peekType(C2) != NodeType::String) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto RawName = nodeText(C2.node());
      C2.next();
      EXPECTED_TRY(auto Name, parseString(RawName));
      if (NameCount == 0) {
        Desc.setModuleName(Name);
      } else {
        Desc.setExternalName(Name);
      }
      ++NameCount;
    }
    EXPECTED_TRY(auto TT, parseTabletype(C));
    Desc.getExternalTableType() = TT;
    Mod.getImportSection().getContent().push_back(std::move(Desc));
    return {};
  }

  // '(' 'table' addr_type? limits reftype expr ')'
  // '(' 'table' addr_type? limits reftype ')'
  // '(' 'table' addr_type? reftype '(' 'elem' ... ')' ')'

  // Peek at addr_type keyword without consuming, so parseTabletype can see it
  bool Is64 = false;
  bool HasAddrType = false;
  if (peekType(C) == NodeType::Keyword) {
    auto Text = nodeText(C.node());
    if (Text == "i64"sv) {
      Is64 = true;
      HasAddrType = true;
    } else if (Text == "i32"sv) {
      HasAddrType = true;
    }
  }

  // Check if this is a limits path: peek past optional addr_type for a number
  {
    auto Ahead = C.copy();
    if (HasAddrType) {
      Ahead.next();
    }
    if (peekType(Ahead) == NodeType::U) {
      AST::TableSegment Seg;
      // Don't consume addr_type here; parseTabletype -> parseLimits handles it
      EXPECTED_TRY(auto TT, parseTabletype(C));
      Seg.getTableType() = TT;

      // Check for init expression (instruction sexprs after the table type)
      if (peekType(C) == NodeType::Sexpr) {
        auto Child = C.node();
        EXPECTED_TRY(convertFoldedInstr(Child, Seg.getExpr().getInstrs()));
        Seg.getExpr().getInstrs().emplace_back(OpCode::End);
        Seg.getExpr().getInstrs().back().setExprLast(true);
        C.next();
      }
      if (C.valid()) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      Mod.getTableSection().getContent().push_back(std::move(Seg));
    } else {
      // Inline elem: addr_type? reftype (elem elem_item*)
      // Consume addr_type if present (parseLimits won't be called here)
      if (HasAddrType) {
        C.next();
      }
      ValType RefTy(TypeCode::FuncRef);
      {
        if (!C.valid()) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        EXPECTED_TRY(RefTy, convertRefType(C.node()));
        C.next();
      }

      std::vector<AST::Expression> InitExprs;
      {
        if (!C.valid()) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        if (!sexprMatch(C, "elem"sv)) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
        Node EChild = C.node();
        // Inline elem: (elem $func1 $func2 ...) or (elem (ref.func $f) ...)
        for (Cursor EC(EChild); EC.next();) {
          Node EEChild = EC.node();
          auto EType = nodeType(EEChild);
          if (EType == NodeType::Id || EType == NodeType::U) {
            EXPECTED_TRY(auto Idx, Syms.resolve(SymbolTable::IndexSpace::Func,
                                                nodeText(EEChild)));
            AST::Expression Expr;
            Expr.getInstrs().emplace_back(OpCode::Ref__func);
            Expr.getInstrs().back().getTargetIndex() = Idx;
            Expr.getInstrs().emplace_back(OpCode::End);
            Expr.getInstrs().back().setExprLast(true);
            InitExprs.push_back(std::move(Expr));
          } else if (EType == NodeType::Sexpr) {
            Cursor TC(EEChild);
            // Could be (ref.func $f) or (item ...) expression
            AST::Expression Expr;
            EXPECTED_TRY(convertExpression(TC, Expr));
            InitExprs.push_back(std::move(Expr));
          } else {
            return Unexpect(ErrCode::Value::WatUnexpectedToken);
          }
        }
        C.next();
      }

      uint32_t ElemCount = static_cast<uint32_t>(InitExprs.size());

      AST::TableSegment TSeg;
      AST::Limit Lim(ElemCount, ElemCount, Is64);
      TSeg.getTableType() = AST::TableType(RefTy, Lim);
      Mod.getTableSection().getContent().push_back(std::move(TSeg));

      AST::ElementSegment ESeg;
      ESeg.setMode(AST::ElementSegment::ElemMode::Active);
      ESeg.setRefType(RefTy);
      ESeg.setIdx(TableIdx);
      if (Is64) {
        ESeg.getExpr().getInstrs().emplace_back(OpCode::I64__const);
        ESeg.getExpr().getInstrs().back().setNum(
            static_cast<uint128_t>(static_cast<uint64_t>(0)));
      } else {
        ESeg.getExpr().getInstrs().emplace_back(OpCode::I32__const);
        ESeg.getExpr().getInstrs().back().setNum(
            static_cast<uint128_t>(static_cast<uint32_t>(0)));
      }
      ESeg.getExpr().getInstrs().emplace_back(OpCode::End);
      ESeg.getExpr().getInstrs().back().setExprLast(true);
      ESeg.getInitExprs() = std::move(InitExprs);
      Mod.getElementSection().getContent().push_back(std::move(ESeg));
    }
  }

  if (C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  return {};
}

// memory ::= ( memory id? (( export name ))* (( import name name ))? memtype )
//         | ( memory id? ( data datastring ) )
Expect<void> Converter::convertMemory(Node N, AST::Module &Mod) {
  uint32_t MemIdx = 0;
  for (const auto &Imp : Mod.getImportSection().getContent()) {
    if (Imp.getExternalType() == ExternalType::Memory) {
      MemIdx++;
    }
  }
  MemIdx += static_cast<uint32_t>(Mod.getMemorySection().getContent().size());

  Cursor C(N);
  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "memory"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }

  if (peekType(C) == NodeType::Id) {
    C.next();
  }

  bool HasInlineImport = false;
  Node InlineImportNode;
  while (peekType(C) == NodeType::Sexpr) {
    Node Child = C.node();
    Cursor FC(Child);
    auto Keyword =
        peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
    if (Keyword == "import"sv) {
      HasInlineImport = true;
      InlineImportNode = Child;
      C.next();
    } else if (Keyword == "export"sv) {
      Cursor C2(Child);
      C2.next(); // skip keyword at position 0
      if (peekType(C2) != NodeType::String) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto RawName = nodeText(C2.node());
      C2.next();
      EXPECTED_TRY(auto Name, parseString(RawName));
      PendingExports.push_back(
          {std::string(Name), ExternalType::Memory, MemIdx});
      C.next();
    } else {
      break;
    }
  }

  if (HasInlineImport) {
    AST::ImportDesc Desc;
    Desc.setExternalType(ExternalType::Memory);
    Cursor C2(InlineImportNode);
    C2.next(); // skip keyword at position 0
    uint32_t NameCount = 0;
    while (C2.valid()) {
      if (peekType(C2) != NodeType::String) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto RawName = nodeText(C2.node());
      C2.next();
      EXPECTED_TRY(auto Name, parseString(RawName));
      if (NameCount == 0) {
        Desc.setModuleName(Name);
      } else if (NameCount == 1) {
        Desc.setExternalName(Name);
      } else {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      ++NameCount;
    }
    EXPECTED_TRY(auto Lim, parseLimits(C));
    Desc.getExternalMemoryType() = AST::MemoryType(Lim);
    Mod.getImportSection().getContent().push_back(std::move(Desc));
    return {};
  }

  // Check if next tokens form limits (number, or i32/i64 followed by number)
  bool IsLimits = false;
  bool Is64 = false;
  {
    auto PeekT = peekType(C);
    if (PeekT == NodeType::U) {
      IsLimits = true;
    } else if (PeekT == NodeType::Keyword) {
      auto Text = nodeText(C.node());
      if (Text == "i64"sv || Text == "i32"sv) {
        // Only treat as limits if followed by a number
        auto Ahead = C.copy();
        Ahead.next();
        auto NextT = peekType(Ahead);
        if (NextT == NodeType::U) {
          IsLimits = true;
          Is64 = (Text == "i64"sv);
        } else {
          // Bare i64/i32 before inline data -- consume and pass to data path
          Is64 = (Text == "i64"sv);
          C.next();
        }
      }
    }
  }

  if (IsLimits) {
    EXPECTED_TRY(auto Lim, parseLimits(C));
    Mod.getMemorySection().getContent().emplace_back(Lim);
  } else {
    // Inline data
    std::vector<Byte> DataBytes;
    while (C.valid()) {
      Node Child = C.node();
      if (sexprMatch(C, "data"sv)) {
        // (data "...") form -- collect strings from inside
        for (Cursor DC(Child); DC.valid(); DC.next()) {
          Node DChild = DC.node();
          if (nodeType(DChild) == NodeType::String) {
            EXPECTED_TRY(auto Str, parseString(nodeText(DChild), false));
            DataBytes.insert(
                DataBytes.end(), reinterpret_cast<const Byte *>(Str.data()),
                reinterpret_cast<const Byte *>(Str.data() + Str.size()));
          }
        }
      } else if (nodeType(Child) == NodeType::String) {
        EXPECTED_TRY(auto Str, parseString(nodeText(Child), false));
        DataBytes.insert(
            DataBytes.end(), reinterpret_cast<const Byte *>(Str.data()),
            reinterpret_cast<const Byte *>(Str.data() + Str.size()));
      }
      C.next();
    }

    const uint64_t PageSize = 65536;
    uint64_t DataSize = DataBytes.size();
    uint64_t Pages = (DataSize + PageSize - 1) / PageSize;

    Mod.getMemorySection().getContent().emplace_back(
        AST::Limit(Pages, Pages, Is64));

    AST::DataSegment DSeg;
    DSeg.setMode(AST::DataSegment::DataMode::Active);
    DSeg.setIdx(MemIdx);
    if (Is64) {
      DSeg.getExpr().getInstrs().emplace_back(OpCode::I64__const);
      DSeg.getExpr().getInstrs().back().setNum(
          static_cast<uint128_t>(static_cast<uint64_t>(0)));
    } else {
      DSeg.getExpr().getInstrs().emplace_back(OpCode::I32__const);
      DSeg.getExpr().getInstrs().back().setNum(
          static_cast<uint128_t>(static_cast<uint32_t>(0)));
    }
    DSeg.getExpr().getInstrs().emplace_back(OpCode::End);
    DSeg.getExpr().getInstrs().back().setExprLast(true);
    DSeg.getData() = std::move(DataBytes);
    Mod.getDataSection().getContent().push_back(std::move(DSeg));
  }

  if (C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  return {};
}

// global ::= ( global id? (( export name ))* (( import name name ))?
//             globaltype expr )
Expect<void> Converter::convertGlobal(Node N, AST::Module &Mod) {
  uint32_t GlobalIdx = 0;
  for (const auto &Imp : Mod.getImportSection().getContent()) {
    if (Imp.getExternalType() == ExternalType::Global) {
      GlobalIdx++;
    }
  }
  GlobalIdx +=
      static_cast<uint32_t>(Mod.getGlobalSection().getContent().size());

  Cursor C(N);
  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "global"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }

  if (peekType(C) == NodeType::Id) {
    C.next();
  }

  bool HasInlineImport = false;
  Node InlineImportNode;
  while (peekType(C) == NodeType::Sexpr) {
    Node Child = C.node();
    Cursor FC(Child);
    auto Keyword =
        peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
    if (Keyword == "import"sv) {
      HasInlineImport = true;
      InlineImportNode = Child;
      C.next();
    } else if (Keyword == "export"sv) {
      Cursor C2(Child);
      C2.next(); // skip keyword at position 0
      if (peekType(C2) != NodeType::String) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto RawName = nodeText(C2.node());
      C2.next();
      EXPECTED_TRY(auto Name, parseString(RawName));
      PendingExports.push_back(
          {std::string(Name), ExternalType::Global, GlobalIdx});
      C.next();
    } else {
      break;
    }
  }

  if (HasInlineImport) {
    AST::ImportDesc Desc;
    Desc.setExternalType(ExternalType::Global);
    Cursor C2(InlineImportNode);
    C2.next(); // skip keyword at position 0
    uint32_t NameCount = 0;
    while (C2.valid()) {
      if (peekType(C2) != NodeType::String) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto RawName = nodeText(C2.node());
      C2.next();
      EXPECTED_TRY(auto Name, parseString(RawName));
      if (NameCount == 0) {
        Desc.setModuleName(Name);
      } else if (NameCount == 1) {
        Desc.setExternalName(Name);
      } else {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      ++NameCount;
    }
    if (!C.valid()) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    EXPECTED_TRY(auto GT, parseGlobaltype(C));
    Desc.getExternalGlobalType() = GT;
    Mod.getImportSection().getContent().push_back(std::move(Desc));
    return {};
  }

  AST::GlobalSegment Seg;
  if (!C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  EXPECTED_TRY(auto GT, parseGlobaltype(C));
  Seg.getGlobalType() = GT;
  EXPECTED_TRY(convertExpression(C, Seg.getExpr()));
  Mod.getGlobalSection().getContent().push_back(std::move(Seg));

  if (C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  return {};
}

// start ::= ( start funcidx )
Expect<void> Converter::convertStart(Node N, AST::Module &Mod) {
  Cursor C(N);
  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "start"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }
  if (auto Type = peekType(C); Type != NodeType::Id && Type != NodeType::U) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }
  EXPECTED_TRY(auto Idx,
               Syms.resolve(SymbolTable::IndexSpace::Func, nodeText(C.node())));
  Mod.getStartSection().setContent(Idx);
  C.next();

  if (C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  return {};
}

// elem ::= ( elem id? elemlist )
//        | ( elem id? ( table tableidx ) ( offset expr ) elemlist )
//        | ( elem id? declare elemlist )
//        | ( elem id? ( offset expr ) elemlist )
//        | ( elem id? tableidx ( offset expr ) func funcidx* )
// elemlist ::= reftype elemexpr* | func funcidx*
// elemexpr ::= ( item expr ) | ( instr )
Expect<void> Converter::convertElem(Node N, AST::Module &Mod) {
  AST::ElementSegment Seg;
  Seg.setMode(AST::ElementSegment::ElemMode::Passive);
  Seg.setRefType(ValType(TypeCode::Ref, TypeCode::FuncRef));

  Cursor C(N);
  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "elem"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }

  if (auto Type = peekType(C); Type == NodeType::Id || Type == NodeType::U) {
    auto MaybeIdx =
        Syms.resolve(SymbolTable::IndexSpace::Table, nodeText(C.node()));
    if (MaybeIdx) {
      Seg.setIdx(*MaybeIdx);
    }
    C.next();
  }

  if (peekType(C) == NodeType::Keyword && nodeText(C.node()) == "declare"sv) {
    Seg.setMode(AST::ElementSegment::ElemMode::Declarative);
    C.next();
  } else if (peekType(C) == NodeType::Sexpr) {
    uint32_t Idx = 0;
    if (sexprMatch(C, "table"sv)) {
      Cursor FC(C.node());
      FC.next(); // skip keyword
      auto TType = peekType(FC);
      if (TType != NodeType::Id && TType != NodeType::U) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      EXPECTED_TRY(Idx, Syms.resolve(SymbolTable::IndexSpace::Table,
                                     nodeText(FC.node())));
      C.next();
    }
    if (sexprMatch(C, "offset"sv)) {
      Seg.setMode(AST::ElementSegment::ElemMode::Active);
      Seg.setIdx(Idx);
      Cursor OC(C.node());
      OC.next(); // skip "offset" keyword
      EXPECTED_TRY(convertExpression(OC, Seg.getExpr()));
      if (OC.valid()) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      C.next();
    }
    if (sexprUnmatch(C, "ref"sv)) {
      Seg.setMode(AST::ElementSegment::ElemMode::Active);
      Seg.setIdx(Idx);
      Cursor OC(C.node());
      EXPECTED_TRY(convertExpression(OC, Seg.getExpr()));
      if (OC.valid()) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      C.next();
    }
  }

  bool SeenRefType = false;
  bool SeenFunc = false;
  if (auto PType = peekType(C); PType == NodeType::Keyword) {
    auto Text = nodeText(C.node());
    if (Text == "func"sv) {
      SeenFunc = true;
    } else if (auto RefTy = convertRefType(C.node()); RefTy) {
      Seg.setRefType(*RefTy);
    }
    SeenRefType = true;
    C.next();
  } else if (PType == NodeType::Sexpr) {
    if (sexprMatch(C, "ref"sv)) {
      EXPECTED_TRY(auto RefTy, convertRefType(C.node()));
      Seg.setRefType(RefTy);
      SeenRefType = true;
      C.next();
    }
  }

  while (C.valid()) {
    Node Child = C.node();
    auto Type = nodeType(Child);
    if (Type == NodeType::Sexpr) {
      AST::Expression Expr;
      if (sexprMatch(C, "item"sv)) {
        // (item expr) = elemexpr
        Cursor IC(Child);
        IC.next(); // skip "item" keyword
        EXPECTED_TRY(convertExpression(IC, Expr));
        if (IC.valid()) {
          return Unexpect(ErrCode::Value::WatUnexpectedToken);
        }
      } else {
        auto &Instrs = Expr.getInstrs();
        EXPECTED_TRY(convertFoldedInstr(Child, Instrs));
        Instrs.emplace_back(OpCode::End);
        Instrs.back().setExprLast(true);
        fixupJumps(Instrs);
      }
      Seg.getInitExprs().push_back(std::move(Expr));
    } else if (Type == NodeType::Id || Type == NodeType::U) {
      if (SeenRefType && !SeenFunc) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      EXPECTED_TRY(auto Idx, Syms.resolve(SymbolTable::IndexSpace::Func,
                                          nodeText(Child)));
      AST::Expression Expr;
      auto &Instrs = Expr.getInstrs();
      Instrs.emplace_back(OpCode::Ref__func);
      Instrs.back().getTargetIndex() = Idx;
      Instrs.emplace_back(OpCode::End);
      Instrs.back().setExprLast(true);
      Seg.getInitExprs().push_back(std::move(Expr));
    } else {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    C.next();
  }

  // For active elem segments without explicit ref type, infer from table
  if (Seg.getMode() == AST::ElementSegment::ElemMode::Active && !SeenRefType) {
    uint32_t TblIdx = Seg.getIdx();
    uint32_t ImportedTables = 0;
    for (const auto &Imp : Mod.getImportSection().getContent()) {
      if (Imp.getExternalType() == ExternalType::Table) {
        if (ImportedTables == TblIdx) {
          Seg.setRefType(Imp.getExternalTableType().getRefType());
          break;
        }
        ++ImportedTables;
      }
    }
    if (TblIdx >= ImportedTables) {
      uint32_t LocalIdx = TblIdx - ImportedTables;
      const auto &Tables = Mod.getTableSection().getContent();
      if (LocalIdx < Tables.size()) {
        Seg.setRefType(Tables[LocalIdx].getTableType().getRefType());
      }
    }
  }

  if (C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  Mod.getElementSection().getContent().push_back(std::move(Seg));
  return {};
}

// data ::= ( data id? datastring )
//        | ( data id? ( memory memidx ) ( offset expr ) datastring )
//        | ( data id? memidx ( offset expr ) datastring )
// datastring ::= string*
Expect<void> Converter::convertData(Node N, AST::Module &Mod) {
  AST::DataSegment Seg;
  Seg.setMode(AST::DataSegment::DataMode::Passive);

  Cursor C(N);
  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "data"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }

  if (peekType(C) == NodeType::Id) {
    auto MaybeIdx =
        Syms.resolve(SymbolTable::IndexSpace::Memory, nodeText(C.node()));
    if (MaybeIdx) {
      Seg.setIdx(*MaybeIdx);
    }
    C.next();
  }

  bool HasMemUse = false;
  if (auto PType = peekType(C); PType == NodeType::Sexpr) {
    HasMemUse = true;
    Seg.setMode(AST::DataSegment::DataMode::Active);
    Node MChild = C.node();
    Cursor MC(MChild);
    auto KW = peekType(MC) == NodeType::Keyword ? nodeText(MC.node()) : ""sv;
    if (KW == "memory"sv) {
      MC.next(); // skip keyword
      Node MId = MC.valid() ? MC.node() : Node{};
      if (MId.isNull()) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto MType = nodeType(MId);
      if (MType != NodeType::Id && MType != NodeType::U) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      EXPECTED_TRY(auto MIdx, Syms.resolve(SymbolTable::IndexSpace::Memory,
                                           nodeText(MId)));
      Seg.setIdx(MIdx);
      C.next();
    }
  } else if (PType == NodeType::U) {
    HasMemUse = true;
    Seg.setMode(AST::DataSegment::DataMode::Active);
    // Bare nat as abbreviated memory index.
    Node IChild = C.node();
    EXPECTED_TRY(auto Idx, Syms.resolve(SymbolTable::IndexSpace::Memory,
                                        nodeText(IChild)));
    Seg.setIdx(Idx);
    C.next();
  }
  if (HasMemUse) {
    if (peekType(C) != NodeType::Sexpr) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    Node OChild = C.node();
    Cursor OC(OChild);
    auto KW = peekType(OC) == NodeType::Keyword ? nodeText(OC.node()) : ""sv;
    if (KW != "offset"sv) {
      // Could be a folded instruction as offset expression
      AST::Expression OffExpr;
      auto &Instrs = OffExpr.getInstrs();
      EXPECTED_TRY(convertFoldedInstr(OChild, Instrs));
      Instrs.emplace_back(OpCode::End);
      Instrs.back().setExprLast(true);
      fixupJumps(Instrs);
      Seg.getExpr() = std::move(OffExpr);
    } else {
      OC.next(); // skip "offset" keyword
      EXPECTED_TRY(convertExpression(OC, Seg.getExpr()));
    }
    C.next();
  }

  while (C.valid()) {
    if (peekType(C) != NodeType::String) {
      return Unexpect(ErrCode::Value::WatUnexpectedToken);
    }
    auto RawStr = nodeText(C.node());
    C.next();
    EXPECTED_TRY(auto Str, parseString(RawStr, false));
    auto &Data = Seg.getData();
    Data.insert(Data.end(), reinterpret_cast<const Byte *>(Str.data()),
                reinterpret_cast<const Byte *>(Str.data() + Str.size()));
  }

  if (C.valid()) {
    return Unexpect(ErrCode::Value::WatUnexpectedToken);
  }

  Mod.getDataSection().getContent().push_back(std::move(Seg));
  return {};
}

// tag ::= ( tag id? (( export name ))* (( import name name ))? typeuse )
Expect<void> Converter::convertTag(Node N, AST::Module &Mod) {
  uint32_t TagIdx = 0;
  for (const auto &Imp : Mod.getImportSection().getContent()) {
    if (Imp.getExternalType() == ExternalType::Tag) {
      TagIdx++;
    }
  }
  TagIdx += static_cast<uint32_t>(Mod.getTagSection().getContent().size());

  Cursor C(N);
  if (peekType(C) == NodeType::Keyword) {
    auto KW = nodeText(C.node());
    C.next();
    if (KW != "tag"sv) {
      return Unexpect(ErrCode::Value::WatUnknownOperator);
    }
  }
  if (peekType(C) == NodeType::Id) {
    C.next();
  }
  bool HasInlineImport = false;
  Node InlineImportNode;
  while (peekType(C) == NodeType::Sexpr) {
    Node Child = C.node();
    Cursor FC(Child);
    auto Keyword =
        peekType(FC) == NodeType::Keyword ? nodeText(FC.node()) : ""sv;
    if (Keyword == "import"sv) {
      HasInlineImport = true;
      InlineImportNode = Child;
      C.next();
    } else if (Keyword == "export"sv) {
      Cursor C2(Child);
      C2.next(); // skip keyword at position 0
      if (peekType(C2) != NodeType::String) {
        return Unexpect(ErrCode::Value::WatUnexpectedToken);
      }
      auto RawName = nodeText(C2.node());
      C2.next();
      EXPECTED_TRY(auto Name, parseString(RawName));
      PendingExports.push_back({std::string(Name), ExternalType::Tag, TagIdx});
      C.next();
    } else {
      break;
    }
  }

  auto checkTagResultEmpty = [&](uint32_t Idx) -> Expect<void> {
    if (Idx < Mod.getTypeSection().getContent().size()) {
      const auto &SubTy = Mod.getTypeSection().getContent()[Idx];
      if (SubTy.getCompositeType().isFunc() &&
          !SubTy.getCompositeType().getFuncType().getReturnTypes().empty()) {
        return Unexpect(ErrCode::Value::WatNonEmptyTagResult);
      }
    }
    return {};
  };

  if (HasInlineImport) {
    AST::ImportDesc Desc;
    Desc.setExternalType(ExternalType::Tag);
    uint32_t NameCount = 0;
    for (Cursor IIC(InlineImportNode); IIC.valid(); IIC.next()) {
      Node Child = IIC.node();
      if (nodeType(Child) == NodeType::String) {
        EXPECTED_TRY(auto Name, parseString(nodeText(Child)));
        if (NameCount == 0) {
          Desc.setModuleName(Name);
        } else {
          Desc.setExternalName(Name);
        }
        NameCount++;
      }
    }
    EXPECTED_TRY(auto Idx, resolveTypeUse(C, Mod, true));
    EXPECTED_TRY(checkTagResultEmpty(Idx));
    Desc.getExternalTagType().setTypeIdx(Idx);
    Mod.getImportSection().getContent().push_back(std::move(Desc));
    return {};
  }

  AST::TagType Tag;
  EXPECTED_TRY(auto Idx, resolveTypeUse(C, Mod, true));
  EXPECTED_TRY(checkTagResultEmpty(Idx));
  Tag.setTypeIdx(Idx);
  Mod.getTagSection().getContent().push_back(std::move(Tag));
  return {};
}

} // namespace WasmEdge::WAT
