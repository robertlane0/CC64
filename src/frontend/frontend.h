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

/* A token is the front end's largest per-word structure and the preprocessed
   list of a large unit holds tens of thousands of them, so the positions are
   32-bit: a source position, an offset into a source, and two flags all fit,
   and a 64-bit field for each would cost a third of the record. */
typedef struct Token {
    TokenKind kind;
    uint32_t flags;              /* bit 0 at beginning of line, bit 1 preceded by space */
    const Source *source;
    uint32_t start;
    uint32_t end;
    uint32_t line;
    uint32_t column;
    char *text;
    Hideset *hideset;
} Token;

#define TOKEN_FLAG_BOL 1U
#define TOKEN_FLAG_SPACE 2U

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
    bool keep_newlines;
    /* One reused buffer builds a token's text, which is then copied into the
       arena. A token that owned a heap block would cost the target one block
       header per token, and a large unit has tens of thousands of tokens. */
    char *scratch;
    size_t scratch_length;
    size_t scratch_capacity;
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
    /* Set when a definition supplied on the command line or built in is one the
       preprocessor will not accept. The definitions are read before the run
       starts, so the failure cannot be returned from where it is found; it is
       held here and the run reports it. */
    bool rejected_definition;
} Preprocessor;

void token_list_init(TokenList *list);
bool token_list_reserve(TokenList *list, size_t capacity);
void token_list_free(TokenList *list);
bool token_list_push(TokenList *list, const Token *token);
Token *token_list_last(TokenList *list);
const char *token_kind_name(TokenKind kind);
void token_list_classify_keywords(TokenList *list);

void lexer_create(Arena *arena, const Source *source,
                 DiagnosticSink *diagnostics, Lexer *lexer);
void lexer_destroy(Lexer *lexer);
bool lexer_next(Lexer *lexer, Token *token);
bool lex_source(Arena *arena, const Source *source,
                DiagnosticSink *diagnostics, TokenList *list);
/* Newline tokens are only produced when the caller needs them: a caller that
   drops them afterwards would hold a third more tokens than it uses. */
bool lex_source_marked(Arena *arena, const Source *source,
                       DiagnosticSink *diagnostics, TokenList *list,
                       bool keep_newlines);

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
