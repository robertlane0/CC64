#include "frontend/frontend.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const punctuators[] = {
    "...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=", ">=",
    "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=",
    "##", "+", "-", "*", "/", "%", "=",
    "<", ">", "!", "~", "&", "^", "|", "?", ":", ";", ",", ".",
    "(", ")", "[", "]", "{", "}", "#"
};

static bool is_ident_start(unsigned char byte)
{
    return byte == '_' || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z');
}

static bool is_ident_continue(unsigned char byte)
{
    return is_ident_start(byte) || (byte >= '0' && byte <= '9');
}

static bool is_digit(unsigned char byte)
{
    return byte >= '0' && byte <= '9';
}

static void lexer_emit(Lexer *lexer, unsigned id, size_t line, size_t column,
                       const char *message)
{
    diagnostic_emit(lexer->diagnostics, id, DIAG_LEX, lexer->source, line,
                    column, message);
}

static bool splice_at(Lexer *lexer)
{
    if (lexer->source->bytes[lexer->position] != '\\') {
        return false;
    }
    size_t next = lexer->position + 1U;
    if (next < lexer->source->length && lexer->source->bytes[next] == '\r') {
        ++next;
    }
    if (next >= lexer->source->length || lexer->source->bytes[next] != '\n') {
        return false;
    }
    lexer->position = next + 1U;
    ++lexer->line;
    lexer->column = 1U;
    return true;
}

static void advance_byte(Lexer *lexer, unsigned char byte)
{
    ++lexer->position;
    if (byte == '\n') {
        ++lexer->line;
        lexer->column = 1U;
    } else {
        ++lexer->column;
    }
}

static void skip_splices(Lexer *lexer)
{
    while (lexer->position < lexer->source->length && splice_at(lexer)) {
    }
}

static size_t logical_position(const Source *source, size_t position)
{
    for (;;) {
        if (position >= source->length || source->bytes[position] != '\\') {
            return position;
        }
        size_t next = position + 1U;
        if (next < source->length && source->bytes[next] == '\r') {
            ++next;
        }
        if (next >= source->length || source->bytes[next] != '\n') {
            return position;
        }
        position = next + 1U;
    }
}

static void skip_line_end(Lexer *lexer)
{
    if (lexer->position >= lexer->source->length) {
        return;
    }
    unsigned char byte = lexer->source->bytes[lexer->position];
    if (byte == '\r') {
        ++lexer->position;
        if (lexer->position < lexer->source->length &&
            lexer->source->bytes[lexer->position] == '\n') {
            ++lexer->position;
        }
        ++lexer->line;
        lexer->column = 1U;
    } else if (byte == '\n') {
        advance_byte(lexer, byte);
    }
}

static void skip_space(Lexer *lexer, bool *has_space, bool *at_bol)
{
    *has_space = false;
    *at_bol = lexer->column == 1U;
    skip_splices(lexer);
    while (lexer->position < lexer->source->length) {
            unsigned char byte = lexer->source->bytes[lexer->position];
            /* A splice can reveal the start of a continuation line, and the
               indentation there is whitespace like any other. The loop asks
               again after each splice so the space a continuation line begins
               with is skipped rather than read as a token that is not one. */
            if (splice_at(lexer)) {
                continue;
            }
            if (byte == ' ' || byte == '\t' || byte == '\f' || byte == '\v') {
                *has_space = true;
                advance_byte(lexer, byte);
                continue;
            }
            if (byte == '\r' || byte == '\n') {
                *at_bol = true;
                break;
            }
            if (byte == '/' && lexer->position + 1U < lexer->source->length) {
                unsigned char next = lexer->source->bytes[lexer->position + 1U];
                if (next == '/') {
                    *has_space = true;
                    lexer->position += 2U;
                    lexer->column += 2U;
                    while (lexer->position < lexer->source->length) {
                        skip_splices(lexer);
                        if (lexer->position >= lexer->source->length ||
                            lexer->source->bytes[lexer->position] == '\n' ||
                            lexer->source->bytes[lexer->position] == '\r') {
                            break;
                        }
                        ++lexer->position;
                        ++lexer->column;
                    }
                    continue;
                }
                if (next == '*') {
                    size_t start_line = lexer->line;
                    size_t start_column = lexer->column;
                    lexer->position += 2U;
                    lexer->column += 2U;
                    bool closed = false;
                    while (lexer->position < lexer->source->length) {
                        if (lexer->position + 1U < lexer->source->length &&
                            lexer->source->bytes[lexer->position] == '*' &&
                            lexer->source->bytes[lexer->position + 1U] == '/') {
                            lexer->position += 2U;
                            lexer->column += 2U;
                            closed = true;
                            *has_space = true;
                            break;
                        }
                        advance_byte(lexer, lexer->source->bytes[lexer->position]);
                    }
                    if (!closed) {
                        lexer_emit(lexer, 1001U, start_line, start_column,
                                   "unterminated block comment");
                    }
                    continue;
                }
            }
            break;
        }
}

static void scratch_begin(Lexer *lexer)
{
    lexer->scratch_length = 0U;
}

static void append_text(Lexer *lexer, unsigned char byte)
{
    if (lexer->scratch_length + 1U >= lexer->scratch_capacity) {
        size_t next = lexer->scratch_capacity == 0U ? 64U
                                                     : lexer->scratch_capacity * 2U;
        char *grown = cc64_xrealloc(lexer->scratch, next);
        lexer->scratch = grown;
        lexer->scratch_capacity = next;
    }
    lexer->scratch[lexer->scratch_length++] = (char)byte;
}

static void append_spliced(Lexer *lexer)
{
    skip_splices(lexer);
    if (lexer->position < lexer->source->length) {
        append_text(lexer, lexer->source->bytes[lexer->position]);
        advance_byte(lexer, lexer->source->bytes[lexer->position]);
    }
}

/* A token's positions are 32-bit, which bounds a source at four gigabytes. The
   limit is diagnosed rather than truncated, because a truncated position would
   silently misplace a later diagnostic. */
static void finish_token(Lexer *lexer, Token *token, size_t start,
                         size_t line, size_t column, bool at_bol,
                         bool has_space, TokenKind kind)
{
    if (start > UINT32_MAX || lexer->position > UINT32_MAX || line > UINT32_MAX ||
        column > UINT32_MAX) {
        lexer_emit(lexer, 1004U, line, column, "source is too large for 32-bit token positions");
        return;
    }
    size_t length = lexer->scratch_length;
    char *text = arena_alloc(lexer->arena, length + 1U);
    if (text == NULL) {
        lexer_emit(lexer, 1004U, line, column, "cannot store token text");
        return;
    }
    memcpy(text, lexer->scratch, length);
    text[length] = '\0';
    token->kind = kind;
    token->source = lexer->source;
    token->start = (uint32_t)start;
    token->end = (uint32_t)lexer->position;
    token->line = (uint32_t)line;
    token->column = (uint32_t)column;
    token->flags = (at_bol ? TOKEN_FLAG_BOL : 0U) |
                   (has_space ? TOKEN_FLAG_SPACE : 0U);
    token->text = text;
    token->hideset = NULL;
}

static bool lex_quoted(Lexer *lexer, Token *token, size_t start,
                       size_t line, size_t column, bool at_bol,
                       bool has_space, unsigned char quote,
                       TokenKind kind)
{
    scratch_begin(lexer);
    append_text(lexer, quote);
    advance_byte(lexer, quote);
    bool closed = false;
    while (lexer->position < lexer->source->length) {
        skip_splices(lexer);
        if (lexer->position >= lexer->source->length) {
            break;
        }
        unsigned char byte = lexer->source->bytes[lexer->position];
        if (byte == '\r' || byte == '\n') {
            break;
        }
        if (byte == '\\') {
            append_text(lexer, byte);
            advance_byte(lexer, byte);
            skip_splices(lexer);
            if (lexer->position < lexer->source->length &&
                lexer->source->bytes[lexer->position] != '\r' &&
                lexer->source->bytes[lexer->position] != '\n') {
                append_spliced(lexer);
            }
            continue;
        }
        append_text(lexer, byte);
        advance_byte(lexer, byte);
        if (byte == quote) {
            closed = true;
            break;
        }
    }
    if (!closed) {
        const char *message = "unterminated character literal";
        if (kind == TOKEN_STRING) {
            message = "unterminated string literal";
        }
        lexer_emit(lexer, 1002U, line, column, message);
    }
    finish_token(lexer, token, start, line, column, at_bol, has_space, kind);
    return closed;
}

void lexer_create(Arena *arena, const Source *source,
                 DiagnosticSink *diagnostics, Lexer *lexer)
{
    lexer->arena = arena;
    lexer->source = source;
    lexer->position = 0U;
    lexer->line = 1U;
    lexer->column = 1U;
    lexer->diagnostics = diagnostics;
    lexer->scratch = NULL;
    lexer->scratch_length = 0U;
    lexer->scratch_capacity = 0U;
    lexer->keep_newlines = true;
}

void lexer_destroy(Lexer *lexer)
{
    if (lexer == NULL) return;
    free(lexer->scratch);
    lexer->scratch = NULL;
    lexer->scratch_length = 0U;
    lexer->scratch_capacity = 0U;
}

bool lexer_next(Lexer *lexer, Token *token)
{
    bool has_space;
    bool at_bol;
    skip_space(lexer, &has_space, &at_bol);
    skip_splices(lexer);
    size_t start = lexer->position;
    size_t line = lexer->line;
    size_t column = lexer->column;
    if (lexer->position >= lexer->source->length) {
        scratch_begin(lexer);
        finish_token(lexer, token, start, line, column, at_bol, has_space,
                     TOKEN_EOF);
        return true;
    }
    unsigned char byte = lexer->source->bytes[lexer->position];
    if (byte == '\r' || byte == '\n') {
        skip_line_end(lexer);
        if (!lexer->keep_newlines) return lexer_next(lexer, token);
        scratch_begin(lexer);
        append_text(lexer, (unsigned char)'\n');
        finish_token(lexer, token, start, line, column, true, has_space,
                     TOKEN_NEWLINE);
        return true;
    }
    if (is_ident_start(byte)) {
        scratch_begin(lexer);
        while (lexer->position < lexer->source->length) {
            skip_splices(lexer);
            if (lexer->position >= lexer->source->length ||
                !is_ident_continue(lexer->source->bytes[lexer->position])) {
                break;
            }
            append_spliced(lexer);
        }
        finish_token(lexer, token, start, line, column, at_bol, has_space,
                     TOKEN_IDENTIFIER);
        return true;
    }
    if (is_digit(byte) || (byte == '.' && lexer->position + 1U < lexer->source->length &&
                           is_digit(lexer->source->bytes[lexer->position + 1U]))) {
        scratch_begin(lexer);
        while (lexer->position < lexer->source->length) {
            skip_splices(lexer);
            if (lexer->position >= lexer->source->length) {
                break;
            }
            unsigned char current = lexer->source->bytes[lexer->position];
            if (!is_ident_continue(current) && current != '.') {
                if ((current == '+' || current == '-') &&
                    lexer->scratch_length != 0U) {
                    char previous = lexer->scratch[lexer->scratch_length - 1U];
                    if (previous == 'e' || previous == 'E' || previous == 'p' ||
                        previous == 'P') {
                        append_spliced(lexer);
                        continue;
                    }
                }
                break;
            }
            append_spliced(lexer);
        }
        finish_token(lexer, token, start, line, column, at_bol, has_space,
                     TOKEN_NUMBER);
        return true;
    }
    if (byte == '\'' || byte == '"') {
        return lex_quoted(lexer, token, start, line, column, at_bol, has_space,
                          byte, byte == '"' ? TOKEN_STRING : TOKEN_CHARACTER);
    }
    for (size_t i = 0U; i < sizeof(punctuators) / sizeof(punctuators[0]); ++i) {
        size_t length = strlen(punctuators[i]);
        size_t probe = lexer->position;
        bool matches = true;
        for (size_t j = 0U; j < length; ++j) {
            probe = logical_position(lexer->source, probe);
            if (probe >= lexer->source->length ||
                lexer->source->bytes[probe] !=
                    (unsigned char)punctuators[i][j]) {
                matches = false;
                break;
            }
            ++probe;
        }
        if (matches) {
            scratch_begin(lexer);
            for (size_t j = 0U; j < length; ++j) {
                append_text(lexer, (unsigned char)punctuators[i][j]);
                skip_splices(lexer);
                if (lexer->position >= lexer->source->length) {
                    break;
                }
                advance_byte(lexer, lexer->source->bytes[lexer->position]);
            }
            finish_token(lexer, token, start, line, column, at_bol, has_space,
                         TOKEN_PUNCTUATOR);
            return true;
        }
    }
    lexer_emit(lexer, 1003U, line, column, "invalid input byte");
    scratch_begin(lexer);
    append_spliced(lexer);
    finish_token(lexer, token, start, line, column, at_bol, has_space,
                 TOKEN_PUNCTUATOR);
    return false;
}

const char *token_kind_name(TokenKind kind)
{
    switch (kind) {
    case TOKEN_EOF: return "eof";
    case TOKEN_NEWLINE: return "newline";
    case TOKEN_IDENTIFIER: return "identifier";
    case TOKEN_NUMBER: return "number";
    case TOKEN_CHARACTER: return "character";
    case TOKEN_STRING: return "string";
    case TOKEN_PUNCTUATOR: return "punctuator";
    case TOKEN_KEYWORD: return "keyword";
    }
    return "unknown";
}

void token_list_classify_keywords(TokenList *list)
{
    static const char *const keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "restrict", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while", "true", "false", "_Alignas", "_Alignof",
        "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn",
        "_Static_assert", "_Thread_local", "static_assert", "__uint128_t"
    };
    for (size_t i = 0U; i < list->count; ++i) {
        Token *token = &list->items[i];
        if (token->kind != TOKEN_IDENTIFIER) {
            continue;
        }
        for (size_t k = 0U; k < sizeof(keywords) / sizeof(keywords[0]); ++k) {
            if (strcmp(token->text, keywords[k]) == 0) {
                token->kind = TOKEN_KEYWORD;
                break;
            }
        }
    }
}

void token_list_init(TokenList *list)
{
    list->items = NULL;
    list->count = 0U;
    list->capacity = 0U;
}

void token_list_free(TokenList *list)
{
    free(list->items);
    token_list_init(list);
}

/* Reserving up front keeps a large source's list from being copied repeatedly
   on the way up, which is the expensive part on a small heap. */
/* A reservation is bounded, however large the estimate is. The estimate from a
   source's length is a good upper bound on how many tokens it produces, and
   taking all of it in one request is what a first-fit heap cannot promise: a
   large unit asks for a couple of megabytes of one run while the rest of the
   compiler's memory is already committed around it, and the request fails even
   though the same memory taken in steps would fit. A bounded reservation lets
   the list grow by the step below, which every allocation the target can
   satisfy, and costs a few more copies. */
#define CC64_RESERVE_CAP 1365U

bool token_list_reserve(TokenList *list, size_t capacity)
{
    if (list == NULL) return false;
    if (capacity > CC64_RESERVE_CAP) capacity = CC64_RESERVE_CAP;
    if (capacity <= list->capacity) return true;
    if (capacity > SIZE_MAX / sizeof(*list->items)) return false;
    Token *items = cc64_xrealloc(list->items, capacity * sizeof(*list->items));
    list->items = items;
    list->capacity = capacity;
    return true;
}

/* A growing list adds a fixed number of entries once it is large instead of
   doubling. A doubling step has to hold the old and the new array at the same
   time, and for a large unit that is several megabytes of contiguous space,
   which the target's first-fit heap cannot always provide in one piece. A
   fixed step keeps every request small enough to satisfy, at the cost of a few
   more copies. */
#define CC64_LIST_STEP 4096U

bool token_list_push(TokenList *list, const Token *token)
{
    if (list->count == list->capacity) {
        size_t next = list->capacity == 0U ? 32U
                       : list->capacity < 4096U ? list->capacity * 2U
                       : list->capacity + CC64_LIST_STEP;
        if (next > SIZE_MAX / sizeof(*list->items)) {
            return false;
        }
        Token *items = cc64_xrealloc(list->items, next * sizeof(*items));
        list->items = items;
        list->capacity = next;
    }
    Token *slot = &list->items[list->count++];
    slot->kind = token->kind;
    slot->source = token->source;
    slot->start = token->start;
    slot->end = token->end;
    slot->line = token->line;
    slot->column = token->column;
    slot->flags = token->flags;
    slot->text = token->text;
    slot->hideset = token->hideset;
    return true;
}

Token *token_list_last(TokenList *list)
{
    return list->count == 0U ? NULL : &list->items[list->count - 1U];
}

bool lex_source(Arena *arena, const Source *source,
                DiagnosticSink *diagnostics, TokenList *list)
{
    return lex_source_marked(arena, source, diagnostics, list, true);
}

bool lex_source_marked(Arena *arena, const Source *source,
                       DiagnosticSink *diagnostics, TokenList *list,
                       bool keep_newlines)
{
    token_list_init(list);
    /* A source of n bytes produces well under n/3 tokens for ordinary C, so
       the list is sized from the source once and rarely has to grow. */
    if (!token_list_reserve(list, source->length / 3U + 64U)) return false;
    Lexer lexer;
    lexer_create(arena, source, diagnostics, &lexer);
    lexer.keep_newlines = keep_newlines;
    for (;;) {
        Token token;
        bool good = lexer_next(&lexer, &token);
        if (!token_list_push(list, &token)) {
            lexer_destroy(&lexer);
            return false;
        }
        if (!good || token.kind == TOKEN_EOF) {
            lexer_destroy(&lexer);
            return true;
        }
    }
}
