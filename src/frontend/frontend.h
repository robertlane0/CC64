#ifndef CC64_FRONTEND_H
#define CC64_FRONTEND_H

#include "cc64.h"

#define CC64_PP_MAX_INCLUDE_DEPTH 32U
#define CC64_PP_MAX_MACRO_DEPTH 64U
#define CC64_PP_MAX_TOKENS 1048576U

typedef enum TokenKind {
    TOKEN_EOF,
    TOKEN_NEWLINE,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_CHARACTER,
    TOKEN_STRING,
    TOKEN_PUNCTUATOR,
    TOKEN_KEYWORD
} TokenKind;

typedef struct Hideset {
    size_t count;
    const char **names;
} Hideset;

typedef struct Token {
    TokenKind kind;
    const Source *source;
    size_t start;
    size_t end;
    size_t line;
    size_t column;
    bool at_bol;
    bool has_space;
    char *text;
    Hideset *hideset;
} Token;

typedef struct TokenList {
    Token *items;
    size_t count;
    size_t capacity;
} TokenList;

typedef struct Lexer {
    Arena *arena;
    const Source *source;
    size_t position;
    size_t line;
    size_t column;
    DiagnosticSink *diagnostics;
} Lexer;

typedef struct MacroParameter {
    char *name;
} MacroParameter;

typedef struct Macro {
    char *name;
    bool function_like;
    bool variadic;
    MacroParameter *parameters;
    size_t parameter_count;
    Token *body;
    size_t body_count;
    struct Macro *next;
} Macro;

typedef struct PreprocessorOptions {
    const char **include_paths;
    size_t include_path_count;
    const char **predefines;
    size_t predefine_count;
    bool preserve_newlines;
} PreprocessorOptions;

typedef struct Preprocessor {
    Arena *arena;
    SourceManager *sources;
    DiagnosticSink *diagnostics;
    PreprocessorOptions options;
    Macro *macros;
    size_t active_includes;
    size_t token_count;
    const char **include_stack;
    size_t include_stack_count;
    int64_t line_delta;
    const char *display_path;
} Preprocessor;

void token_list_init(TokenList *list);
void token_list_free(TokenList *list);
bool token_list_push(TokenList *list, const Token *token);
Token *token_list_last(TokenList *list);
const char *token_kind_name(TokenKind kind);
void token_list_classify_keywords(TokenList *list);

void lexer_create(Arena *arena, const Source *source,
                 DiagnosticSink *diagnostics, Lexer *lexer);
bool lexer_next(Lexer *lexer, Token *token);
bool lex_source(Arena *arena, const Source *source,
                DiagnosticSink *diagnostics, TokenList *list);

Preprocessor *preprocessor_create(Arena *arena, SourceManager *sources,
                                  DiagnosticSink *diagnostics,
                                  const PreprocessorOptions *options);
void preprocessor_define_text(Preprocessor *preprocessor, const char *definition);
bool preprocessor_run(Preprocessor *preprocessor, const Source *source,
                      TokenList *output);
bool preprocess_source(Arena *arena, SourceManager *sources,
                       DiagnosticSink *diagnostics, const Source *source,
                       const PreprocessorOptions *options, TokenList *output);
bool write_token_list(FILE *stream, const TokenList *list, bool line_markers);

#endif
