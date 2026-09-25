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

static void append_text(char **text, size_t *length, size_t *capacity,
                        unsigned char byte)
{
    if (*length + 1U >= *capacity) {
        size_t next = *capacity == 0U ? 32U : *capacity * 2U;
        char *grown = cc64_xrealloc(*text, next);
        *text = grown;
        *capacity = next;
    }
    (*text)[(*length)++] = (char)byte;
}

static void append_spliced(Lexer *lexer, char **text, size_t *length,
                           size_t *capacity)
{
    skip_splices(lexer);
    if (lexer->position < lexer->source->length) {
        append_text(text, length, capacity, lexer->source->bytes[lexer->position]);
        advance_byte(lexer, lexer->source->bytes[lexer->position]);
    }
}

static void finish_token(Lexer *lexer, Token *token, size_t start,
                         size_t line, size_t column, bool at_bol,
                         bool has_space, TokenKind kind, char *text,
                         size_t length)
{
    text[length] = '\0';
    token->kind = kind;
    token->source = lexer->source;
    token->start = start;
    token->end = lexer->position;
    token->line = line;
    token->column = column;
    token->at_bol = at_bol;
    token->has_space = has_space;
    token->text = text;
    token->hideset = NULL;
}

static bool lex_quoted(Lexer *lexer, Token *token, size_t start,
                       size_t line, size_t column, bool at_bol,
                       bool has_space, unsigned char quote,
                       TokenKind kind)
{
    char *text = NULL;
    size_t length = 0U;
    size_t capacity = 0U;
    append_text(&text, &length, &capacity, quote);
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
            append_text(&text, &length, &capacity, byte);
            advance_byte(lexer, byte);
            skip_splices(lexer);
            if (lexer->position < lexer->source->length &&
                lexer->source->bytes[lexer->position] != '\r' &&
                lexer->source->bytes[lexer->position] != '\n') {
                append_spliced(lexer, &text, &length, &capacity);
            }
            continue;
        }
        append_text(&text, &length, &capacity, byte);
        advance_byte(lexer, byte);
        if (byte == quote) {
            closed = true;
            break;
        }
    }
    if (!closed) {
        lexer_emit(lexer, 1002U, line, column,
                   kind == TOKEN_STRING ? "unterminated string literal"
                                        : "unterminated character literal");
    }
    finish_token(lexer, token, start, line, column, at_bol, has_space, kind,
                 text, length);
    return closed;
}

Lexer lexer_create(Arena *arena, const Source *source, DiagnosticSink *diagnostics)
{
    Lexer lexer = {arena, source, 0U, 1U, 1U, diagnostics};
    return lexer;
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
        finish_token(lexer, token, start, line, column, at_bol, has_space,
                     TOKEN_EOF, cc64_xstrdup(""), 0U);
        return true;
    }
    unsigned char byte = lexer->source->bytes[lexer->position];
    if (byte == '\r' || byte == '\n') {
        skip_line_end(lexer);
        finish_token(lexer, token, start, line, column, true, has_space,
                     TOKEN_NEWLINE, cc64_xstrdup("\n"), 1U);
        return true;
    }
    if (is_ident_start(byte)) {
        char *text = NULL;
        size_t length = 0U;
        size_t capacity = 0U;
        while (lexer->position < lexer->source->length) {
            skip_splices(lexer);
            if (lexer->position >= lexer->source->length ||
                !is_ident_continue(lexer->source->bytes[lexer->position])) {
                break;
            }
            append_spliced(lexer, &text, &length, &capacity);
        }
        finish_token(lexer, token, start, line, column, at_bol, has_space,
                     TOKEN_IDENTIFIER, text, length);
        return true;
    }
    if (is_digit(byte) || (byte == '.' && lexer->position + 1U < lexer->source->length &&
                           is_digit(lexer->source->bytes[lexer->position + 1U]))) {
        char *text = NULL;
        size_t length = 0U;
        size_t capacity = 0U;
        while (lexer->position < lexer->source->length) {
            skip_splices(lexer);
            if (lexer->position >= lexer->source->length) {
                break;
            }
            unsigned char current = lexer->source->bytes[lexer->position];
            if (!is_ident_continue(current) && current != '.') {
                if ((current == '+' || current == '-') && length != 0U) {
                    char previous = text[length - 1U];
                    if (previous == 'e' || previous == 'E' || previous == 'p' ||
                        previous == 'P') {
                        append_spliced(lexer, &text, &length, &capacity);
                        continue;
                    }
                }
                break;
            }
            append_spliced(lexer, &text, &length, &capacity);
        }
        finish_token(lexer, token, start, line, column, at_bol, has_space,
                     TOKEN_NUMBER, text, length);
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
            char *text = cc64_xstrdup(punctuators[i]);
            for (size_t j = 0U; j < length; ++j) {
                skip_splices(lexer);
                if (lexer->position >= lexer->source->length) {
                    break;
                }
                advance_byte(lexer, lexer->source->bytes[lexer->position]);
            }
            finish_token(lexer, token, start, line, column, at_bol, has_space,
                         TOKEN_PUNCTUATOR, text, length);
            return true;
        }
    }
    lexer_emit(lexer, 1003U, line, column, "invalid input byte");
    char *text = NULL;
    size_t length = 0U;
    size_t capacity = 0U;
    append_spliced(lexer, &text, &length, &capacity);
    finish_token(lexer, token, start, line, column, at_bol, has_space,
                 TOKEN_PUNCTUATOR, text, length);
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
        "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
        "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn",
        "_Static_assert", "_Thread_local"
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

bool token_list_push(TokenList *list, Token token)
{
    if (list->count == list->capacity) {
        size_t next = list->capacity == 0U ? 32U : list->capacity * 2U;
        if (next > SIZE_MAX / sizeof(*list->items)) {
            return false;
        }
        Token *items = cc64_xrealloc(list->items, next * sizeof(*items));
        list->items = items;
        list->capacity = next;
    }
    list->items[list->count++] = token;
    return true;
}

Token *token_list_last(TokenList *list)
{
    return list->count == 0U ? NULL : &list->items[list->count - 1U];
}

TokenList lex_source(Arena *arena, const Source *source, DiagnosticSink *diagnostics)
{
    TokenList list;
    token_list_init(&list);
    Lexer lexer = lexer_create(arena, source, diagnostics);
    for (;;) {
        Token token;
        bool good = lexer_next(&lexer, &token);
        if (!token_list_push(&list, token)) {
            break;
        }
        if (!good || token.kind == TOKEN_EOF) {
            break;
        }
    }
    return list;
}
