// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2026 Second State INC
//
// External scanner for WAT grammar.
// Handles block comments (;...;) which are nestable and conflict with
// the '(' token in the sexpr rule. Also handles annotations (@id ...).

#include "tree_sitter/parser.h"

enum TokenType {
  BLOCK_COMMENT,
  ANNOTATION,
};

void *tree_sitter_wat_external_scanner_create(void) { return NULL; }

void tree_sitter_wat_external_scanner_destroy(void *payload) { (void)payload; }

unsigned tree_sitter_wat_external_scanner_serialize(void *payload, char *buffer) {
  (void)payload;
  (void)buffer;
  return 0;
}

void tree_sitter_wat_external_scanner_deserialize(void *payload,
                                                   const char *buffer,
                                                   unsigned length) {
  (void)payload;
  (void)buffer;
  (void)length;
}

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static void skip_ws(TSLexer *lexer) { lexer->advance(lexer, true); }

bool tree_sitter_wat_external_scanner_scan(void *payload, TSLexer *lexer,
                                            const bool *valid_symbols) {
  (void)payload;

  // Skip whitespace before checking for '(' opener.
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
         lexer->lookahead == '\n' || lexer->lookahead == '\r') {
    skip_ws(lexer);
  }

  if (lexer->lookahead != '(') {
    return false;
  }

  lexer->mark_end(lexer);
  advance(lexer);

  // '(;' — block comment opener
  if (lexer->lookahead == ';' && valid_symbols[BLOCK_COMMENT]) {
    advance(lexer);
    int depth = 1;
    while (depth > 0 && !lexer->eof(lexer)) {
      if (lexer->lookahead == '(') {
        advance(lexer);
        if (lexer->lookahead == ';') { advance(lexer); depth++; }
      } else if (lexer->lookahead == ';') {
        advance(lexer);
        if (lexer->lookahead == ')') { advance(lexer); depth--; }
      } else {
        advance(lexer);
      }
    }
    if (depth == 0) {
      lexer->mark_end(lexer);
      lexer->result_symbol = BLOCK_COMMENT;
      return true;
    }
    return false;
  }

  // '(@' — annotation opener: scan balanced parens to closing ')'
  if (lexer->lookahead == '@' && valid_symbols[ANNOTATION]) {
    advance(lexer);
    // Consume annotation id (keyword or quoted string after @)
    if (lexer->lookahead == '"') {
      advance(lexer);
      while (!lexer->eof(lexer) && lexer->lookahead != '"') {
        if (lexer->lookahead == '\\') advance(lexer); // skip escape
        advance(lexer);
      }
      if (lexer->lookahead == '"') advance(lexer);
    } else {
      while (!lexer->eof(lexer) && lexer->lookahead != ' ' &&
             lexer->lookahead != '\t' && lexer->lookahead != '\n' &&
             lexer->lookahead != '\r' && lexer->lookahead != '(' &&
             lexer->lookahead != ')') {
        advance(lexer);
      }
    }
    // Scan balanced content until matching ')'.
    // Handle block comments (;...;), line comments ;;, and strings "...".
    int depth = 1;
    while (depth > 0 && !lexer->eof(lexer)) {
      if (lexer->lookahead == '(') {
        advance(lexer);
        if (lexer->lookahead == ';') {
          // Block comment (;...;) — skip nestable
          advance(lexer);
          int cdepth = 1;
          while (cdepth > 0 && !lexer->eof(lexer)) {
            if (lexer->lookahead == '(') {
              advance(lexer);
              if (lexer->lookahead == ';') { advance(lexer); cdepth++; }
            } else if (lexer->lookahead == ';') {
              advance(lexer);
              if (lexer->lookahead == ')') { advance(lexer); cdepth--; }
            } else {
              advance(lexer);
            }
          }
        } else {
          depth++;
        }
      } else if (lexer->lookahead == ')') {
        advance(lexer); depth--;
      } else if (lexer->lookahead == ';') {
        advance(lexer);
        if (lexer->lookahead == ';') {
          // Line comment ;; — skip to end of line
          advance(lexer);
          while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
            advance(lexer);
          }
        }
      } else if (lexer->lookahead == '"') {
        advance(lexer);
        while (!lexer->eof(lexer) && lexer->lookahead != '"') {
          if (lexer->lookahead == '\\') advance(lexer);
          advance(lexer);
        }
        if (lexer->lookahead == '"') advance(lexer);
      } else {
        advance(lexer);
      }
    }
    if (depth == 0) {
      lexer->mark_end(lexer);
      lexer->result_symbol = ANNOTATION;
      return true;
    }
    return false;
  }

  return false;
}
