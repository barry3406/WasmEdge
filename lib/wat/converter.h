// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2026 Second State INC

#pragma once

#include "tree_sitter.h"

#include "ast/module.h"
#include "common/configure.h"
#include "common/errcode.h"
#include "common/hash.h"
#include "common/types.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace WasmEdge::WAT {

/// Node type enum for the simplified grammar.
/// The grammar produces only S-expression structure and raw token types.
/// All semantic interpretation is handled by keyword string matching.
enum class NodeType {
  // Tree structure nodes (root and sexpr both map here)
  Sexpr,

  // Leaf token types
  Keyword,
  U,
  S,
  F,
  String,
  Id,
  Reserved,

  // Tree-sitter built-in
  Error,

  // Fallback for unrecognized types
  Unknown,
};

class Converter {
public:
  Converter(const Configure &C) : Conf(C) {}

  /// Convert a parsed WAT tree into an AST::Module.
  Expect<AST::Module> convert(const Tree &ParseTree, std::string_view Source);

private:
  // --- Symbol Table ---
  struct SymbolTable {
    std::unordered_map<std::string, uint32_t, Hash::Hash> Types;
    std::unordered_map<std::string, uint32_t, Hash::Hash> Funcs;
    std::unordered_map<std::string, uint32_t, Hash::Hash> Tables;
    std::unordered_map<std::string, uint32_t, Hash::Hash> Memories;
    std::unordered_map<std::string, uint32_t, Hash::Hash> Globals;
    std::unordered_map<std::string, uint32_t, Hash::Hash> Tags;
    std::unordered_map<std::string, uint32_t, Hash::Hash> Elems;
    std::unordered_map<std::string, uint32_t, Hash::Hash> Datas;
    std::unordered_map<std::string, uint32_t, Hash::Hash> Locals;
    std::unordered_map<std::string, std::vector<uint32_t>, Hash::Hash> Labels;
    // Per-type field name -> field index mapping for struct types
    std::unordered_map<uint32_t,
                       std::unordered_map<std::string, uint32_t, Hash::Hash>>
        FieldNames;
    std::vector<std::string> LabelStack;

    uint32_t NextType = 0, NextFunc = 0, NextTable = 0, NextMemory = 0;
    uint32_t NextGlobal = 0, NextTag = 0, NextElem = 0, NextData = 0;
    uint32_t NextLocal = 0;

    void clear();
    void clearLocals();
    void pushLabel(std::string_view Label);
    void popLabel();

    Expect<uint32_t>
    resolveIdx(const std::unordered_map<std::string, uint32_t, Hash::Hash> &Map,
               std::string_view Ref, ErrCode::Value Err) const;

    enum class IndexSpace {
      Func,
      Table,
      Memory,
      Global,
      Tag,
      Elem,
      Data,
      Local,
    };

    Expect<uint32_t> resolveType(std::string_view Ref) const;
    Expect<uint32_t> resolve(IndexSpace Space, std::string_view Ref) const;
    Expect<uint32_t> resolveLabel(std::string_view Ref) const;
    Expect<uint32_t> resolveField(uint32_t TypeIdx, std::string_view Ref) const;
  };

  // --- Pass 1: Collect identifiers and assign indices ---
  Expect<void> collectIndices(Node Root);
  Expect<void> collectModuleField(Node Field);

  // --- Pass 2: Build AST sections ---
  Expect<void> buildModule(Node Root, AST::Module &Mod);
  Expect<void> buildModuleField(Node Field, AST::Module &Mod);

  // Type conversions
  Expect<void> convertTypedef(Node N, AST::Module &Mod);
  Expect<void> convertRecType(Node N, AST::Module &Mod);
  Expect<AST::SubType> convertSubType(Node N, uint32_t TypeIdx);
  Expect<AST::FunctionType> convertFuncType(Node N);
  Expect<ValType> convertValType(Node N);
  Expect<ValType> convertRefType(Node N);
  Expect<ValType> convertRefTypeSexpr(Node N);
  Expect<ValType> convertHeapType(Node N);
  Expect<std::vector<AST::FieldType>> convertStructFields(Node N,
                                                          uint32_t TypeIdx);
  Expect<AST::FieldType> convertArrayField(Node N);
  Expect<ValType> convertStorageType(Node N);
  Expect<AST::FieldType> parseMutField(Node MutNode);

  // Section conversions
  Expect<void> convertImport(Node N, AST::Module &Mod);
  Expect<void> convertExport(Node N, AST::Module &Mod);
  Expect<void> convertFunc(Node N, AST::Module &Mod);
  Expect<void> convertTable(Node N, AST::Module &Mod);
  Expect<void> convertMemory(Node N, AST::Module &Mod);
  Expect<void> convertGlobal(Node N, AST::Module &Mod);
  Expect<void> convertStart(Node N, AST::Module &Mod);
  Expect<void> convertElem(Node N, AST::Module &Mod);
  Expect<void> convertData(Node N, AST::Module &Mod);
  Expect<void> convertTag(Node N, AST::Module &Mod);

  // Instruction conversions
  static void fixupJumps(AST::InstrVec &Instrs);
  Expect<void> convertExpression(Cursor &C, AST::Expression &Expr);
  Expect<void> convertMixedInstrSeq(Cursor &C, AST::InstrVec &Instrs);

  Expect<void> convertPlainInstr(Cursor &C, AST::InstrVec &Instrs, OpCode Code);
  Expect<void> convertBlockInstr(Node N, AST::InstrVec &Instrs);
  Expect<void> convertFoldedInstr(Node N, AST::InstrVec &Instrs);

  // Category handlers — shared by flat (convertMixedInstrSeq) and folded
  // (convertPlainInstr) paths. Cursor points to the first immediate after
  // the opcode keyword.
  Expect<void> convertSelectOp(Cursor &C, AST::InstrVec &Instrs);
  Expect<void> convertConstOp(Cursor &C, AST::InstrVec &Instrs, OpCode Code);
  Expect<void> convertVarOp(Cursor &C, AST::InstrVec &Instrs, OpCode Code);
  Expect<void> convertBranchOp(Cursor &C, AST::InstrVec &Instrs, OpCode Code);
  Expect<void> convertCallOp(Cursor &C, AST::InstrVec &Instrs, OpCode Code);
  Expect<void> convertRefOp(Cursor &C, AST::InstrVec &Instrs, OpCode Code);
  Expect<void> convertTableOp(Cursor &C, AST::InstrVec &Instrs, OpCode Code);
  Expect<void> convertMemControlOp(Cursor &C, AST::InstrVec &Instrs,
                                   OpCode Code);
  Expect<void> convertGCOp(Cursor &C, AST::InstrVec &Instrs, OpCode Code);
  Expect<void> convertSimdConstOp(Cursor &C, AST::InstrVec &Instrs,
                                  OpCode Code);
  Expect<void> convertMemLoadStoreOp(Cursor &C, AST::InstrVec &Instrs,
                                     OpCode Code);
  Expect<void> convertSimdLaneOp(Cursor &C, AST::InstrVec &Instrs, OpCode Code);

  // ERROR node classification
  ErrCode::Value classifyError(Node Root) const;

  // Helpers
  std::string_view nodeText(Node N) const;
  NodeType nodeType(Node N) const;

  /// Peek the type of the current node of C
  NodeType peekType(const Cursor &C) const;

  /// Check if C is at a Sexpr whose first keyword child matches KW.
  bool sexprMatch(const Cursor &C, std::string_view KW) const;
  /// Check if C is at a Sexpr whose first keyword child does not match KW.
  bool sexprUnmatch(const Cursor &C, std::string_view KW) const;

  /// Check if a child node is an immediate operand (not a folded
  /// sub-instruction sexpr).
  bool isImmediate(Node Child) const;
  /// Advance cursor to the next immediate (non-sexpr) named child.
  /// Returns the immediate node, or null if none remain.
  Node nextImmediate(Cursor &C) const;

  Expect<uint32_t> resolveTypeUse(Cursor &C, AST::Module &Mod,
                                  bool CheckMismatch,
                                  bool AcceptParamId = true);
  Expect<AST::Limit> parseLimits(Cursor &C);
  Expect<AST::TableType> parseTabletype(Cursor &C);
  Expect<AST::GlobalType> parseGlobaltype(Cursor &C);
  Expect<BlockType> parseBlockType(Cursor &C);

  /// Find an existing function type matching FuncTy, or create a new one.
  /// Returns the type index.
  uint32_t findOrCreateFuncType(AST::FunctionType FuncTy, AST::Module &Mod);

  // State
  SymbolTable Syms;
  std::string_view Source;
  AST::Module *CurMod = nullptr;
  const Configure Conf;

  // Import ordering state
  bool HasFuncDef = false;
  bool HasTableDef = false;
  bool HasMemoryDef = false;
  bool HasGlobalDef = false;
  bool HasStart = false;

  // Deferred inline exports (collected in pass 1, emitted in pass 2)
  struct InlineExport {
    std::string Name;
    ExternalType Type;
    uint32_t Index;
  };
  std::vector<InlineExport> PendingExports;

  // Deferred inline imports
  struct InlineImport {
    std::string ModName;
    std::string ExtName;
  };
};

} // namespace WasmEdge::WAT
