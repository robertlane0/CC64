#include "semantic/semantic.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct DeclSpec {
    Type *type;
    StorageClass storage;
    bool inline_specifier;
    bool has_type;
} DeclSpec;

struct Parser {
    Arena *arena;
    DiagnosticSink *diagnostics;
    const Token *tokens;
    size_t count;
    size_t position;
    Scope *global_scope;
    Scope *scope;
    TranslationUnit *unit;
    bool failed;
    Symbol *current_function;
    Type *last_declarator_type;
    unsigned loop_depth;
    unsigned switch_depth;
    unsigned static_local_count;
};

static AstNode *parse_statement(Parser *parser);
static AstNode *parse_expression(Parser *parser);
static AstNode *parse_assignment(Parser *parser);
static AstNode *parse_conditional(Parser *parser);
static AstNode *parse_unary(Parser *parser);
static AstNode *parse_cast(Parser *parser);
static bool parse_declarator(Parser *parser, Type *base, const char **name,
                            Symbol **parameters, size_t *parameter_count,
                            bool *variadic);
static bool parse_decl_specs(Parser *parser, DeclSpec *spec);
static Type *parse_type_name(Parser *parser);

static const Token *peek(const Parser *parser)
{
    return parser->position < parser->count ? &parser->tokens[parser->position] : NULL;
}

static const Token *peek_at(const Parser *parser, size_t offset)
{
    return parser->position + offset < parser->count
               ? &parser->tokens[parser->position + offset] : NULL;
}

static const Token *take(Parser *parser)
{
    const Token *token = peek(parser);
    if (token != NULL) {
        ++parser->position;
    }
    return token;
}

static bool token_text(const Token *token, const char *text)
{
    return token != NULL && strcmp(token->text, text) == 0;
}

static bool accept(Parser *parser, const char *text)
{
    if (!token_text(peek(parser), text)) {
        return false;
    }
    ++parser->position;
    return true;
}

static const Token *expect(Parser *parser, const char *text)
{
    const Token *token = peek(parser);
    if (token_text(token, text)) {
        ++parser->position;
        return token;
    }
    char message[160];
    (void)snprintf(message, sizeof(message), "expected '%s'", text);
    diagnostic_emit(parser->diagnostics, 2001U, DIAG_PARSE,
                    token == NULL ? NULL : token->source,
                    token == NULL ? 0U : token->line,
                    token == NULL ? 0U : token->column, message);
    parser->failed = true;
    return token;
}

static void semantic_error(Parser *parser, unsigned id, const Token *token,
                           const char *message)
{
    diagnostic_emit(parser->diagnostics, id, DIAG_SEMANTIC,
                    token == NULL ? NULL : token->source,
                    token == NULL ? 0U : token->line,
                    token == NULL ? 0U : token->column, message);
    parser->failed = true;
}

static void location_of(const Token *token, SourceLocation *location)
{
    location->source = NULL;
    location->line = 0U;
    location->column = 0U;
    if (token != NULL) {
        location->source = token->source;
        location->line = token->line;
        location->column = token->column;
    }
}

static AstNode *node_new(Parser *parser, NodeKind kind, Type *type,
                         const Token *token)
{
    AstNode *node = arena_alloc(parser->arena, sizeof(*node));
    if (node == NULL) {
        parser->failed = true;
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->type = type;
    location_of(token, &node->location);
    return node;
}

static bool append_node(AstNode ***items, size_t *count, size_t *capacity,
                        AstNode *node)
{
    if (node == NULL) {
        return false;
    }
    if (*count == *capacity) {
        size_t next = *capacity == 0U ? 8U : *capacity * 2U;
        if (next < *capacity || next > SIZE_MAX / sizeof(**items)) {
            return false;
        }
        AstNode **grown = cc64_xrealloc(*items, next * sizeof(**items));
        *items = grown;
        *capacity = next;
    }
    (*items)[(*count)++] = node;
    return true;
}

static bool token_is_keyword(const Token *token, const char *text)
{
    return token != NULL && token->kind == TOKEN_KEYWORD &&
           strcmp(token->text, text) == 0;
}

static bool token_is_identifier(const Token *token)
{
    return token != NULL && token->kind == TOKEN_IDENTIFIER;
}

static bool token_is_type_keyword(const Token *token)
{
    static const char *const names[] = {
        "void", "char", "short", "int", "long", "float", "double", "signed",
        "unsigned", "_Bool", "struct", "union", "enum", "const", "volatile",
        "restrict", "typedef", "extern", "static", "auto", "register",
        "inline", "_Noreturn", "_Alignas", "_Atomic"
    };
    if (token == NULL || token->kind != TOKEN_KEYWORD) {
        return false;
    }
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(token->text, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool token_is_decl_start(Parser *parser, const Token *token)
{
    if (token_is_type_keyword(token)) {
        return true;
    }
    if (!token_is_identifier(token)) {
        return false;
    }
    Symbol *symbol = scope_lookup(parser->scope, token->text);
    return symbol != NULL && symbol->class == SYMBOL_TYPEDEF;
}

static bool token_is_start_of_declaration(Parser *parser)
{
    return token_is_decl_start(parser, peek(parser));
}

static char *copy_name(Parser *parser, const char *name)
{
    (void)parser;
    return cc64_xstrdup(name);
}

static Symbol *symbol_new(Parser *parser, const char *name, Type *type,
                          SymbolClass class)
{
    Symbol *symbol = arena_alloc(parser->arena, sizeof(*symbol));
    if (symbol == NULL) {
        parser->failed = true;
        return NULL;
    }
    memset(symbol, 0, sizeof(*symbol));
    symbol->name = copy_name(parser, name);
    symbol->type = type;
    symbol->class = class;
    return symbol;
}

static bool type_is_function(const Type *type)
{
    return type != NULL && type->kind == TYPE_FUNCTION;
}

static bool is_integer_constant_token(const Token *token)
{
    return token != NULL && token->kind == TOKEN_NUMBER;
}

static bool parse_constant_size(Parser *parser, size_t *value)
{
    const Token *start = peek(parser);
    if (!is_integer_constant_token(start)) {
        semantic_error(parser, 2010U, start, "array size must be an integer constant");
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long raw = strtoull(start->text, &end, 0);
    if (errno == ERANGE || end == start->text || *end != '\0' || raw > SIZE_MAX) {
        semantic_error(parser, 2011U, start, "invalid array size");
        return false;
    }
    *value = (size_t)raw;
    ++parser->position;
    return true;
}

static void complete_struct_type(Parser *parser, Type *type, const Token *token)
{
    size_t offset = 0U;
    size_t alignment = 1U;
    for (Member *member = type->members; member != NULL; member = member->next) {
        if (!type_is_complete(member->type)) {
            semantic_error(parser, 2012U, token, "struct member has incomplete type");
            continue;
        }
        size_t member_alignment = type_alignment(member->type);
        if (member_alignment > alignment) {
            alignment = member_alignment;
        }
        if (type->kind == TYPE_STRUCT) {
            size_t aligned = offset % member_alignment;
            if (aligned != 0U && member_alignment - aligned > SIZE_MAX - offset) {
                semantic_error(parser, 2013U, token, "struct layout overflow");
                offset = SIZE_MAX;
            } else if (aligned != 0U) {
                offset += member_alignment - aligned;
            }
            member->offset = offset;
            if (type_size(member->type) > SIZE_MAX - offset) {
                semantic_error(parser, 2014U, token, "struct layout overflow");
                offset = SIZE_MAX;
            } else {
                offset += type_size(member->type);
            }
        } else {
            member->offset = 0U;
            if (type_size(member->type) > offset) {
                offset = type_size(member->type);
            }
        }
    }
    if (alignment != 0U && offset > SIZE_MAX - (alignment - 1U)) {
        semantic_error(parser, 2015U, token, "struct layout overflow");
    } else if (alignment != 0U) {
        offset = (offset + alignment - 1U) & ~(alignment - 1U);
    }
    type->alignment = alignment;
    type->size = offset;
    type->incomplete = false;
}

static bool parse_struct_members(Parser *parser, Type *type)
{
    Member **tail = &type->members;
    while (!token_text(peek(parser), "}") && peek(parser) != NULL) {
        DeclSpec spec;
        if (!parse_decl_specs(parser, &spec)) {
            return false;
        }
        if (accept(parser, ";")) {
            semantic_error(parser, 2016U, peek(parser), "anonymous struct member is unsupported");
            continue;
        }
        for (;;) {
            const char *name = NULL;
            Symbol *parameters = NULL;
            size_t parameter_count = 0U;
            bool variadic = false;
            Type *member_type = spec.type;
            bool member_ok = parse_declarator(parser, member_type, &name, &parameters,
                                               &parameter_count, &variadic);
            if (!member_ok) {
                semantic_error(parser, 2017U, peek(parser), "struct member declarator is invalid");
                return false;
            }
            member_type = parser->last_declarator_type;
            if (name == NULL) {
                semantic_error(parser, 2017U, peek(parser), "struct member declarator is invalid");
                return false;
            }
            if (accept(parser, ":")) {
                (void)parse_constant_size(parser, &parameter_count);
                semantic_error(parser, 2018U, peek(parser), "bit-fields are deferred");
            }
            if (type_is_function(member_type)) {
                semantic_error(parser, 2019U, peek(parser), "function member is unsupported");
            }
            Member *member = arena_alloc(parser->arena, sizeof(*member));
            if (member == NULL) {
                return false;
            }
            member->name = copy_name(parser, name);
            member->type = member_type;
            member->next = NULL;
            *tail = member;
            tail = &member->next;
            if (!accept(parser, ",")) {
                break;
            }
        }
        if (!accept(parser, ";")) {
            expect(parser, ";");
            return false;
        }
    }
    return expect(parser, "}") != NULL;
}

static Type *lookup_local_tag(const Scope *scope, const char *name)
{
    if (scope == NULL) {
        return NULL;
    }
    for (Binding *binding = scope->bindings; binding != NULL; binding = binding->next) {
        if (binding->tag && strcmp(binding->name, name) == 0) {
            return binding->type;
        }
    }
    return NULL;
}

static Type *parse_tag_specifier(Parser *parser, TypeKind kind)
{
    const Token *tag = token_is_identifier(peek(parser)) ? take(parser) : NULL;
    Type *existing = tag == NULL ? NULL : lookup_local_tag(parser->scope, tag->text);
    if (token_text(peek(parser), "{")) {
        if (existing != NULL && existing->kind != kind) {
            semantic_error(parser, 2021U, tag, "tag used with the wrong kind");
            return existing;
        }
        if (existing != NULL && !existing->incomplete) {
            semantic_error(parser, 2022U, tag, "redefinition of struct or union tag");
            return existing;
        }
        Type *type = existing;
        if (type == NULL) {
            type = type_basic(parser->arena, kind == TYPE_UNION ? TYPE_UNION : TYPE_STRUCT);
            if (type == NULL) {
                return NULL;
            }
            if (tag != NULL) {
                type->tag = copy_name(parser, tag->text);
                (void)scope_add_tag(parser->arena, parser->scope, tag->text, type);
            }
        }
        type->incomplete = true;
        (void)take(parser);
        if (!parse_struct_members(parser, type)) {
            return NULL;
        }
        complete_struct_type(parser, type, tag);
        return type;
    }
    if (tag == NULL) {
        semantic_error(parser, 2020U, peek(parser), "struct or union tag is required here");
        return type_basic(parser->arena, kind);
    }
    existing = scope_lookup_tag(parser->scope, tag->text);
    if (existing != NULL) {
        if (existing->kind != kind) {
            semantic_error(parser, 2021U, tag, "tag used with the wrong kind");
        }
        return existing;
    }
    Type *type = type_basic(parser->arena, kind);
    if (type == NULL) {
        return NULL;
    }
    type->tag = copy_name(parser, tag->text);
    type->incomplete = true;
    (void)scope_add_tag(parser->arena, parser->scope, tag->text, type);
    return type;
}

static bool parse_enum_specifier(Parser *parser, Type **result)
{
    const Token *tag = token_is_identifier(peek(parser)) ? take(parser) : NULL;
    Type *type = NULL;
    if (tag != NULL) {
        Type *existing = scope_lookup_tag(parser->scope, tag->text);
        if (existing != NULL && existing->kind == TYPE_ENUM) {
            type = existing;
        }
    }
    if (type == NULL) {
        type = type_basic(parser->arena, TYPE_ENUM);
        if (type == NULL) return false;
        if (tag != NULL) {
            type->tag = copy_name(parser, tag->text);
            (void)scope_add_tag(parser->arena, parser->scope, tag->text, type);
        }
    }
    if (accept(parser, "{")) {
        int64_t next_value = 0;
        while (!token_text(peek(parser), "}") && peek(parser) != NULL) {
            const Token *name = token_is_identifier(peek(parser)) ? take(parser) : NULL;
            if (name == NULL) {
                semantic_error(parser, 2022U, peek(parser), "enumerator name is required");
                return false;
            }
            if (accept(parser, "=")) {
                const Token *number = peek(parser);
                if (!is_integer_constant_token(number)) {
                    semantic_error(parser, 2023U, number, "enumerator value must be constant");
                } else {
                    char *end = NULL;
                    errno = 0;
                    long long value = strtoll(number->text, &end, 0);
                    if (errno == ERANGE || end == number->text || *end != '\0') {
                        semantic_error(parser, 2024U, number, "invalid enumerator value");
                    } else {
                        next_value = value;
                    }
                    ++parser->position;
                }
            }
            Symbol *constant = symbol_new(parser, name->text, type, SYMBOL_ENUM_CONSTANT);
            if (constant == NULL) {
                return false;
            }
            constant->enum_value = next_value++;
            constant->defined = true;
            if (scope_lookup(parser->scope, name->text) != NULL ||
                !scope_add_symbol(parser->arena, parser->scope, constant)) {
                semantic_error(parser, 2025U, name, "duplicate enumerator");
                return false;
            }
            if (!accept(parser, ",")) {
                break;
            }
        }
        if (!expect(parser, "}")) {
            return false;
        }
    } else if (tag == NULL) {
        semantic_error(parser, 2026U, peek(parser), "enum tag or body is required");
        return false;
    }
    *result = type;
    return true;
}

static bool parse_parameter_list(Parser *parser, Symbol **parameters,
                                 size_t *parameter_count, bool *variadic)
{
    *parameters = NULL;
    *parameter_count = 0U;
    *variadic = false;
    if (accept(parser, ")")) {
        return true;
    }
    if (token_is_keyword(peek(parser), "void") && token_text(peek_at(parser, 1U), ")")) {
        (void)take(parser);
        (void)take(parser);
        return true;
    }
    Symbol *tail = NULL;
    unsigned unnamed = 0U;
    for (;;) {
        if (accept(parser, "...")) {
            *variadic = true;
            if (!expect(parser, ")")) {
                return false;
            }
            break;
        }
        DeclSpec spec;
        if (!parse_decl_specs(parser, &spec)) {
            return false;
        }
        const char *name = NULL;
        Symbol *nested_parameters = NULL;
        size_t nested_count = 0U;
        bool nested_variadic = false;
        Type *type = spec.type;
        bool parameter_ok = parse_declarator(parser, type, &name, &nested_parameters,
                                             &nested_count, &nested_variadic);
        if (!parameter_ok) {
            return false;
        }
        type = parser->last_declarator_type;
        if (type_is_function(type) || type->kind == TYPE_ARRAY) {
            if (type->kind == TYPE_ARRAY) {
                type = type_pointer(parser->arena, type->base, type->qualifiers);
            }
        }
        if (type == NULL || type->kind == TYPE_VOID ||
            type->kind == TYPE_STRUCT || type->kind == TYPE_UNION) {
            semantic_error(parser, 2027U, peek(parser), "invalid parameter type");
            return false;
        }
        char generated[32];
        if (name == NULL) {
            (void)snprintf(generated, sizeof(generated), "__unnamed_%u", unnamed++);
            name = generated;
        }
        Symbol *parameter = symbol_new(parser, name, type, SYMBOL_VARIABLE);
        if (parameter == NULL) {
            return false;
        }
        parameter->is_parameter = true;
        if (tail == NULL) {
            *parameters = parameter;
        } else {
            tail->next = parameter;
        }
        tail = parameter;
        ++*parameter_count;
        if (!accept(parser, ",")) {
            if (!expect(parser, ")")) {
                return false;
            }
            break;
        }
    }
    return true;
}

static bool parse_declarator(Parser *parser, Type *base, const char **name,
                             Symbol **parameters, size_t *parameter_count,
                             bool *variadic)
{
    if (name != NULL) {
        *name = NULL;
    }
    if (parameters != NULL) {
        *parameters = NULL;
    }
    if (parameter_count != NULL) {
        *parameter_count = 0U;
    }
    if (variadic != NULL) {
        *variadic = false;
    }
    Type *type = base;
    while (accept(parser, "*")) {
        TypeQualifiers qualifiers = 0U;
        for (;;) {
            if (accept(parser, "const")) qualifiers |= TYPE_QUAL_CONST;
            else if (accept(parser, "volatile")) qualifiers |= TYPE_QUAL_VOLATILE;
            else if (accept(parser, "restrict")) qualifiers |= TYPE_QUAL_RESTRICT;
            else break;
        }
        type = type_pointer(parser->arena, type, qualifiers);
    }
    if (token_text(peek(parser), "(") && token_text(peek_at(parser, 1U), "*")) {
        (void)take(parser);
        const char *group_name = NULL;
        Symbol *group_parameters = NULL;
        size_t group_count = 0U;
        bool group_variadic = false;
        bool grouped = parse_declarator(parser, type, &group_name,
                                        &group_parameters, &group_count,
                                        &group_variadic);
        if (!grouped || !expect(parser, ")")) {
            return false;
        }
        Type *grouped_type = parser->last_declarator_type;
        if (name != NULL) *name = group_name;
        if (parameters != NULL) *parameters = group_parameters;
        if (parameter_count != NULL) *parameter_count = group_count;
        if (variadic != NULL) *variadic = group_variadic;
        type = grouped_type;
        bool wrapped_pointer = grouped_type != NULL && grouped_type->kind == TYPE_POINTER;
        Type *suffix_base = wrapped_pointer ? grouped_type->base : grouped_type;
        while (accept(parser, "[")) {
            size_t count = 0U;
            if (!parse_constant_size(parser, &count)) return false;
            (void)expect(parser, "]");
            type = type_array(parser->arena, suffix_base, count, true);
            suffix_base = type;
        }
        if (wrapped_pointer && !token_text(peek(parser), "(")) {
            type = type_pointer(parser->arena, type, 0U);
        }
        if (accept(parser, "(")) {
            Symbol *function_parameters = NULL;
            size_t function_count = 0U;
            bool function_variadic = false;
            if (!parse_parameter_list(parser, &function_parameters, &function_count,
                                      &function_variadic)) return false;
            type = type_function(parser->arena, suffix_base, function_parameters,
                                 function_count, function_variadic);
            if (wrapped_pointer) type = type_pointer(parser->arena, type, 0U);
            if (parameters != NULL) *parameters = function_parameters;
            if (parameter_count != NULL) *parameter_count = function_count;
            if (variadic != NULL) *variadic = function_variadic;
        }
        parser->last_declarator_type = type;
        return type != NULL;
    }
    if (token_is_identifier(peek(parser))) {
        if (name != NULL) *name = take(parser)->text;
        else (void)take(parser);
    }
    for (;;) {
        if (accept(parser, "[")) {
            size_t count = 0U;
            if (token_text(peek(parser), "]")) {
                (void)take(parser);
                type = type_array(parser->arena, type, 0U, false);
            } else {
                if (!parse_constant_size(parser, &count) || !expect(parser, "]")) {
                    return false;
                }
                type = type_array(parser->arena, type, count, true);
            }
        } else if (accept(parser, "(")) {
            Symbol *function_parameters = NULL;
            size_t function_count = 0U;
            bool function_variadic = false;
            if (!parse_parameter_list(parser, &function_parameters, &function_count,
                                      &function_variadic)) return false;
            if (type_is_function(type)) {
                semantic_error(parser, 2028U, peek(parser), "function returning function is unsupported");
            }
            if (type->kind == TYPE_ARRAY) {
                semantic_error(parser, 2029U, peek(parser), "function returning array is unsupported");
            }
            type = type_function(parser->arena, type, function_parameters,
                                 function_count, function_variadic);
            if (parameters != NULL) *parameters = function_parameters;
            if (parameter_count != NULL) *parameter_count = function_count;
            if (variadic != NULL) *variadic = function_variadic;
        } else {
            break;
        }
    }
    parser->last_declarator_type = type;
    return type != NULL;
}

static bool parse_decl_specs(Parser *parser, DeclSpec *spec)
{
    spec->type = NULL;
    spec->storage = STORAGE_NONE;
    spec->inline_specifier = false;
    spec->has_type = false;
    bool saw_signed = false;
    bool saw_unsigned = false;
    unsigned short_count = 0U;
    unsigned long_count = 0U;
    bool saw_char = false;
    bool saw_int = false;
    bool saw_float = false;
    bool saw_double = false;
    bool saw_bool = false;
    TypeQualifiers qualifiers = 0U;
    for (;;) {
        const Token *token = peek(parser);
        if (token == NULL) break;
        if (token_is_keyword(token, "typedef")) { spec->storage = STORAGE_TYPEDEF; (void)take(parser); continue; }
        if (token_is_keyword(token, "extern")) { spec->storage = STORAGE_EXTERN; (void)take(parser); continue; }
        if (token_is_keyword(token, "static")) { spec->storage = STORAGE_STATIC; (void)take(parser); continue; }
        if (token_is_keyword(token, "auto")) { spec->storage = STORAGE_AUTO; (void)take(parser); continue; }
        if (token_is_keyword(token, "register")) { spec->storage = STORAGE_REGISTER; (void)take(parser); continue; }
        if (token_is_keyword(token, "inline")) { spec->inline_specifier = true; (void)take(parser); continue; }
        if (token_is_keyword(token, "_Noreturn")) { (void)take(parser); continue; }
        if (token_is_keyword(token, "const")) { qualifiers |= TYPE_QUAL_CONST; (void)take(parser); continue; }
        if (token_is_keyword(token, "volatile")) { qualifiers |= TYPE_QUAL_VOLATILE; (void)take(parser); continue; }
        if (token_is_keyword(token, "restrict")) { qualifiers |= TYPE_QUAL_RESTRICT; (void)take(parser); continue; }
        if (token_is_keyword(token, "_Atomic") || token_is_keyword(token, "_Alignas")) {
            semantic_error(parser, 2030U, token, "deferred type specifier is unsupported");
            (void)take(parser);
            if (token_text(peek(parser), "(")) {
                unsigned depth = 1U;
                while (depth != 0U && peek(parser) != NULL) {
                    if (accept(parser, "(")) ++depth;
                    else if (accept(parser, ")")) --depth;
                    else (void)take(parser);
                }
            }
            continue;
        }
        if (token_is_keyword(token, "struct") || token_is_keyword(token, "union")) {
            if (spec->has_type) break;
            (void)take(parser);
            spec->type = parse_tag_specifier(parser, token_is_keyword(token, "union") ? TYPE_UNION : TYPE_STRUCT);
            spec->has_type = spec->type != NULL;
            continue;
        }
        if (token_is_keyword(token, "enum")) {
            if (spec->has_type) break;
            (void)take(parser);
            if (!parse_enum_specifier(parser, &spec->type)) return false;
            spec->has_type = true;
            continue;
        }
        if (token_is_keyword(token, "void")) { if (!spec->has_type) { spec->type = type_basic(parser->arena, TYPE_VOID); spec->has_type = true; } (void)take(parser); continue; }
        if (token_is_keyword(token, "_Bool")) { saw_bool = true; (void)take(parser); continue; }
        if (token_is_keyword(token, "char")) { saw_char = true; (void)take(parser); continue; }
        if (token_is_keyword(token, "short")) { ++short_count; (void)take(parser); continue; }
        if (token_is_keyword(token, "int")) { saw_int = true; (void)take(parser); continue; }
        if (token_is_keyword(token, "long")) { ++long_count; (void)take(parser); continue; }
        if (token_is_keyword(token, "float")) { saw_float = true; (void)take(parser); continue; }
        if (token_is_keyword(token, "double")) { saw_double = true; (void)take(parser); continue; }
        if (token_is_keyword(token, "signed")) { saw_signed = true; (void)take(parser); continue; }
        if (token_is_keyword(token, "unsigned")) { saw_unsigned = true; (void)take(parser); continue; }
        if (!spec->has_type && token_is_identifier(token)) {
            Symbol *symbol = scope_lookup(parser->scope, token->text);
            if (symbol == NULL || symbol->class != SYMBOL_TYPEDEF) break;
            spec->type = symbol->type;
            spec->has_type = true;
            (void)take(parser);
            continue;
        }
        break;
    }
    if (!spec->has_type) {
        TypeKind kind = TYPE_INT;
        if (saw_bool) kind = TYPE_BOOL;
        else if (saw_float) kind = TYPE_FLOAT;
        else if (saw_double) kind = TYPE_DOUBLE;
        else if (saw_char) kind = saw_unsigned ? TYPE_UNSIGNED_CHAR : (saw_signed ? TYPE_SIGNED_CHAR : TYPE_CHAR);
        else if (short_count != 0U) kind = saw_unsigned ? TYPE_UNSIGNED_SHORT : TYPE_SHORT;
        else if (long_count > 1U) kind = saw_unsigned ? TYPE_UNSIGNED_LONG_LONG : TYPE_LONG_LONG;
        else if (long_count == 1U) kind = saw_unsigned ? TYPE_UNSIGNED_LONG : TYPE_LONG;
        else if (saw_signed || saw_unsigned || saw_int) kind = saw_unsigned ? TYPE_UNSIGNED_INT : TYPE_INT;
        else if (token_text(peek(parser), ";")) kind = TYPE_INT;
        else if (peek(parser) == NULL) kind = TYPE_INT;
        else {
            /* An implicit int is retained only for declaration-shaped input. */
            if (!token_text(peek(parser), "*") && !token_text(peek(parser), "(") &&
                !token_is_identifier(peek(parser))) {
                semantic_error(parser, 2031U, peek(parser), "declaration has no type specifier");
            }
        }
        spec->type = type_basic(parser->arena, kind);
        spec->has_type = true;
    } else if (qualifiers != 0U) {
        spec->type = type_copy(parser->arena, spec->type, qualifiers);
    }
    if (saw_float && (saw_double || saw_int || saw_signed || saw_unsigned)) {
        semantic_error(parser, 2032U, peek(parser), "conflicting floating type specifiers");
    }
    if (saw_char && (saw_float || saw_double || saw_int || short_count != 0U || long_count != 0U)) {
        semantic_error(parser, 2033U, peek(parser), "conflicting integer type specifiers");
    }
    if (saw_int && short_count != 0U) {
        semantic_error(parser, 2034U, peek(parser), "'int' cannot combine with 'short'");
    }
    if (long_count > 2U || short_count > 1U) {
        semantic_error(parser, 2035U, peek(parser), "invalid repeated type specifier");
    }
    return spec->type != NULL;
}

static Type *parse_type_name(Parser *parser)
{
    DeclSpec spec;
    if (!parse_decl_specs(parser, &spec)) return NULL;
    const char *name = NULL;
    Symbol *parameters = NULL;
    size_t parameter_count = 0U;
    bool variadic = false;
    bool declarator_ok = parse_declarator(parser, spec.type, &name, &parameters,
                                         &parameter_count, &variadic);
    Type *type = declarator_ok ? parser->last_declarator_type : NULL;
    if (name != NULL) {
        semantic_error(parser, 2036U, peek(parser), "named type is not allowed here");
    }
    return type;
}

static bool type_is_lvalue(AstNode *node)
{
    if (node == NULL) return false;
    if (node->kind == NODE_IDENTIFIER || node->kind == NODE_DEREFERENCE ||
        node->kind == NODE_INDEX || node->kind == NODE_MEMBER) return true;
    return false;
}

static AstNode *make_cast(Parser *parser, Type *type, AstNode *value,
                          const Token *token)
{
    if (value == NULL || type == NULL) return value;
    if (type_is_void(type)) {
        AstNode *node = node_new(parser, NODE_CAST, type, token);
        if (node != NULL) node->a = value;
        return node;
    }
    if (!type_is_scalar(type) || !type_is_scalar(value->type)) {
        semantic_error(parser, 2040U, token, "invalid scalar conversion");
        return value;
    }
    AstNode *node = node_new(parser, NODE_CAST, type, token);
    if (node != NULL) node->a = value;
    return node;
}

static bool assignment_compatible(Parser *parser, Type *left, Type *right,
                                  const Token *token)
{
    if (left == NULL || right == NULL) return false;
    if (type_is_arithmetic(left) && type_is_arithmetic(right)) return true;
    if (type_is_pointer(left) && type_is_pointer(right)) {
        if (type_is_void(left->base) || type_is_void(right->base) ||
            type_compatible(left->base, right->base)) return true;
        semantic_error(parser, 2041U, token, "pointer assignment target is incompatible");
        return false;
    }
    if (type_is_pointer(left) && right != NULL && right->kind == TYPE_ARRAY) {
        return type_compatible(left->base, right->base) || type_is_void(left->base);
    }
    if (type_is_pointer(left) && right != NULL && right->kind == TYPE_FUNCTION) {
        return type_is_void(left->base);
    }
    if (type_is_pointer(right) && type_is_integer(left)) return true;
    if (type_compatible(type_unqualified(left), type_unqualified(right))) return true;
    semantic_error(parser, 2042U, token, "incompatible assignment");
    return false;
}

static AstNode *implicit_conversion(Parser *parser, Type *target, AstNode *value,
                                    const Token *token)
{
    if (value == NULL || target == NULL) return value;
    if (type_is_arithmetic(target) && type_is_arithmetic(value->type)) {
        Type *common = target;
        if (target->size < value->type->size) common = value->type;
        return make_cast(parser, common == target ? target : common, value, token);
    }
    if (type_is_pointer(target) && value->type != NULL &&
        value->type->kind == TYPE_ARRAY) {
        if (!assignment_compatible(parser, target, value->type, token)) return value;
        AstNode *decay = node_new(parser, NODE_CAST, target, token);
        if (decay != NULL) decay->a = value;
        return decay;
    }
    if (type_is_pointer(target) && type_is_function(value->type)) {
        AstNode *decay = node_new(parser, NODE_CAST, target, token);
        if (decay != NULL) decay->a = value;
        return decay;
    }
    if (type_is_pointer(target) && type_is_pointer(value->type)) {
        if (!assignment_compatible(parser, target, value->type, token)) return value;
        return make_cast(parser, target, value, token);
    }
    if (type_is_pointer(target) && type_is_integer(value->type)) {
        return make_cast(parser, target, value, token);
    }
    if (type_compatible(type_unqualified(target), type_unqualified(value->type))) return value;
    semantic_error(parser, 2043U, token, "implicit conversion is not valid");
    return value;
}

static AstNode *decay_value(Parser *parser, AstNode *value, const Token *token)
{
    if (value == NULL || value->type == NULL) return value;
    if (value->type->kind == TYPE_ARRAY) {
        AstNode *node = node_new(parser, NODE_CAST,
                                 type_pointer(parser->arena, value->type->base, 0U),
                                 token);
        if (node != NULL) node->a = value;
        return node;
    }
    if (value->type->kind == TYPE_FUNCTION) {
        AstNode *node = node_new(parser, NODE_CAST,
                                 type_pointer(parser->arena, value->type, 0U),
                                 token);
        if (node != NULL) node->a = value;
        return node;
    }
    return value;
}

static AstNode *make_binary(Parser *parser, BinaryOperator op, AstNode *left,
                            AstNode *right, const Token *token)
{
    if (left == NULL || right == NULL) return NULL;
    left = decay_value(parser, left, token);
    right = decay_value(parser, right, token);
    AstNode *node;
    Type *type = NULL;
    if (op == BINARY_LOGICAL_AND || op == BINARY_LOGICAL_OR) {
        if (!type_is_scalar(left->type) || !type_is_scalar(right->type)) {
            semantic_error(parser, 2050U, token, "logical operand must be scalar");
        }
        type = type_basic(parser->arena, TYPE_INT);
    } else if (op == BINARY_ADD || op == BINARY_SUBTRACT) {
        if (type_is_arithmetic(left->type) && type_is_arithmetic(right->type)) {
            type = type_usual_arithmetic(parser->arena, type_unqualified(left->type), type_unqualified(right->type));
            left = make_cast(parser, type, left, token);
            right = make_cast(parser, type, right, token);
        } else if (type_is_pointer(left->type) && type_is_integer(right->type)) {
            type = left->type;
            right = make_cast(parser, type_basic(parser->arena, TYPE_LONG), right, token);
        } else if (op == BINARY_ADD && type_is_integer(left->type) && type_is_pointer(right->type)) {
            type = right->type;
            left = make_cast(parser, type_basic(parser->arena, TYPE_LONG), left, token);
        } else if (op == BINARY_SUBTRACT && type_is_pointer(left->type) &&
                   type_is_pointer(right->type)) {
            if (!type_compatible(left->type->base, right->type->base)) {
                semantic_error(parser, 2051U, token, "pointer subtraction is incompatible");
            }
            type = type_basic(parser->arena, TYPE_LONG);
        } else {
            semantic_error(parser, 2051U, token, "invalid pointer arithmetic");
            type = type_basic(parser->arena, TYPE_LONG);
        }
    } else if (op == BINARY_MULTIPLY || op == BINARY_DIVIDE || op == BINARY_REMAINDER ||
               op == BINARY_BITWISE_AND || op == BINARY_BITWISE_XOR || op == BINARY_BITWISE_OR) {
        if (!type_is_integer(left->type) || !type_is_integer(right->type)) {
            semantic_error(parser, 2052U, token, "integer operator requires integer operands");
        }
        type = type_usual_arithmetic(parser->arena, type_unqualified(left->type), type_unqualified(right->type));
        left = make_cast(parser, type, left, token);
        right = make_cast(parser, type, right, token);
    } else if (op == BINARY_LEFT_SHIFT || op == BINARY_RIGHT_SHIFT) {
        if (!type_is_integer(left->type) || !type_is_integer(right->type)) {
            semantic_error(parser, 2053U, token, "shift requires integer operands");
        }
        type = type_integer_promote(parser->arena, type_unqualified(left->type));
        left = make_cast(parser, type, left, token);
        right = make_cast(parser, type_integer_promote(parser->arena, type_unqualified(right->type)), right, token);
    } else {
        if (!type_is_scalar(left->type) || !type_is_scalar(right->type)) {
            semantic_error(parser, 2054U, token, "comparison requires scalar operands");
        }
        if (type_is_pointer(left->type) && type_is_pointer(right->type)) {
            if (!type_is_void(left->type->base) && !type_is_void(right->type->base) &&
                !type_compatible(left->type->base, right->type->base)) {
                semantic_error(parser, 2055U, token, "pointer comparison is incompatible");
            }
        }
        type = type_basic(parser->arena, TYPE_INT);
    }
    node = node_new(parser, NODE_BINARY, type, token);
    if (node != NULL) {
        node->binary = op;
        node->a = left;
        node->b = right;
    }
    return node;
}

static AstNode *make_unary(Parser *parser, UnaryOperator op, AstNode *value,
                           const Token *token)
{
    if (value == NULL) return NULL;
    Type *type = value->type;
    switch (op) {
    case UNARY_PLUS:
        if (!type_is_arithmetic(type)) semantic_error(parser, 2060U, token, "unary '+' requires arithmetic");
        type = type_integer_promote(parser->arena, type_unqualified(type));
        value = make_cast(parser, type, value, token);
        break;
    case UNARY_MINUS:
        if (!type_is_arithmetic(type)) semantic_error(parser, 2061U, token, "unary '-' requires arithmetic");
        type = type_integer_promote(parser->arena, type_unqualified(type));
        value = make_cast(parser, type, value, token);
        break;
    case UNARY_LOGICAL_NOT:
        if (!type_is_scalar(type)) semantic_error(parser, 2062U, token, "logical '!' requires scalar");
        type = type_basic(parser->arena, TYPE_INT);
        break;
    case UNARY_BITWISE_NOT:
        if (!type_is_integer(type)) semantic_error(parser, 2063U, token, "'~' requires integer");
        type = type_integer_promote(parser->arena, type_unqualified(type));
        value = make_cast(parser, type, value, token);
        break;
    case UNARY_PRE_INCREMENT:
    case UNARY_PRE_DECREMENT:
    case UNARY_POST_INCREMENT:
    case UNARY_POST_DECREMENT:
        if (!type_is_lvalue(value) || !type_is_scalar(value->type)) semantic_error(parser, 2064U, token, "increment/decrement requires scalar lvalue");
        break;
    }
    AstNode *node = node_new(parser, NODE_UNARY, type, token);
    if (node != NULL) { node->unary = op; node->a = value; }
    return node;
}

static AstNode *parse_primary(Parser *parser);
static AstNode *parse_postfix(Parser *parser);

static AstNode *parse_number(Parser *parser, const Token *token)
{
    const char *text = token->text;
    char *end = NULL;
    errno = 0;
    bool hexadecimal = text[0] == '0' &&
                       (text[1] == 'x' || text[1] == 'X');
    bool floating = !hexadecimal &&
                    (strpbrk(text, ".eEpP") != NULL ||
                     text[strlen(text) - 1U] == 'f' ||
                     text[strlen(text) - 1U] == 'F');
    if (floating) {
        bool float_suffix = text[strlen(text) - 1U] == 'f' ||
                            text[strlen(text) - 1U] == 'F';
        char *number_end = NULL;
        double value = float_suffix ? (double)strtof(text, &number_end)
                                    : strtod(text, &number_end);
        end = number_end;
        if (errno == ERANGE || end == text ||
            (*end != '\0' && !(float_suffix && (*end == 'f' || *end == 'F')))) {
            semantic_error(parser, 2070U, token, "invalid floating constant");
            value = 0.0;
        }
        AstNode *node = node_new(parser, NODE_FLOAT,
                                 type_basic(parser->arena,
                                             float_suffix ? TYPE_FLOAT : TYPE_DOUBLE),
                                 token);
        if (node != NULL) node->floating = value;
        return node;
    }
    unsigned base = 10U;
    if (hexadecimal) base = 16U;
    else if (text[0] == '0' && text[1] != '\0') base = 8U;
    unsigned long long raw = strtoull(text, &end, (int)base);
    if (errno == ERANGE || end == text) {
        semantic_error(parser, 2071U, token, "invalid integer constant");
        raw = 0U;
    }
    bool unsigned_suffix = false;
    unsigned long_count = 0U;
    while (*end != '\0') {
        if (*end == 'u' || *end == 'U') unsigned_suffix = true;
        else if (*end == 'l' || *end == 'L') ++long_count;
        else { semantic_error(parser, 2072U, token, "invalid integer suffix"); break; }
        ++end;
    }
    if (long_count > 2U) semantic_error(parser, 2073U, token, "integer suffix is too long");
    TypeKind kind = TYPE_INT;
    if (unsigned_suffix && long_count == 0U) kind = TYPE_UNSIGNED_INT;
    else if (unsigned_suffix && long_count == 1U) kind = TYPE_UNSIGNED_LONG;
    else if (unsigned_suffix) kind = TYPE_UNSIGNED_LONG_LONG;
    else if (long_count == 1U) kind = TYPE_LONG;
    else if (long_count >= 2U) kind = TYPE_LONG_LONG;
    AstNode *node = node_new(parser, NODE_INTEGER, type_basic(parser->arena, kind), token);
    if (node != NULL) {
        node->unsigned_integer = (uint64_t)raw;
        node->integer = (int64_t)raw;
    }
    return node;
}

static uint64_t decode_escape(Parser *parser, const char **cursor,
                              const Token *token)
{
    const char *p = *cursor;
    if (*p != '\\') return (unsigned char)*p++;
    ++p;
    uint64_t value = 0U;
    switch (*p) {
    case 'a': value = '\a'; ++p; break;
    case 'b': value = '\b'; ++p; break;
    case 'f': value = '\f'; ++p; break;
    case 'n': value = '\n'; ++p; break;
    case 'r': value = '\r'; ++p; break;
    case 't': value = '\t'; ++p; break;
    case 'v': value = '\v'; ++p; break;
    case '\\': value = '\\'; ++p; break;
    case '\'': value = '\''; ++p; break;
    case '"': value = '"'; ++p; break;
    case 'x':
        ++p;
        if (!isxdigit((unsigned char)*p)) {
            semantic_error(parser, 2074U, token, "invalid hexadecimal escape");
        }
        while (isxdigit((unsigned char)*p)) {
            unsigned digit = (unsigned)(*p >= 'a' ? *p - 'a' + 10 :
                                       *p >= 'A' ? *p - 'A' + 10 : *p - '0');
            value = value * 16U + digit;
            ++p;
        }
        break;
    default:
        if (*p >= '0' && *p <= '7') {
            unsigned count = 0U;
            while (count < 3U && *p >= '0' && *p <= '7') {
                value = value * 8U + (uint64_t)(unsigned)(*p - '0');
                ++p;
                ++count;
            }
        } else {
            semantic_error(parser, 2074U, token, "unknown escape sequence");
            value = (unsigned char)*p++;
        }
        break;
    }
    *cursor = p;
    return value;
}

static AstNode *parse_character(Parser *parser, const Token *token)
{
    const char *p = token->text;
    while (*p != '\0' && *p != '\'') ++p;
    if (*p == '\'') ++p;
    uint64_t value = 0U;
    if (*p == '\0' || *p == '\'') {
        semantic_error(parser, 2074U, token, "empty character constant");
    } else {
        value = decode_escape(parser, &p, token);
    }
    AstNode *node = node_new(parser, NODE_INTEGER, type_basic(parser->arena, TYPE_INT), token);
    if (node != NULL) { node->unsigned_integer = value; node->integer = (int64_t)value; }
    return node;
}

static AstNode *parse_string(Parser *parser, const Token *token)
{
    size_t length = strlen(token->text);
    size_t raw_length = length >= 2U ? length - 2U : 0U;
    char *decoded = cc64_xmalloc(raw_length + 1U);
    size_t decoded_length = 0U;
    const char *p = token->text;
    if (length >= 2U && *p == '"') {
        ++p;
        while (*p != '\0' && *p != '"') {
            if (*p == '\\') decoded[decoded_length++] = (char)decode_escape(parser, &p, token);
            else decoded[decoded_length++] = *p++;
        }
    }
    decoded[decoded_length] = '\0';
    AstNode *node = node_new(parser, NODE_STRING,
                             type_array(parser->arena,
                                         type_basic(parser->arena, TYPE_CHAR),
                                         decoded_length + 1U, true), token);
    if (node != NULL) {
        node->text = decoded;
        node->text_length = decoded_length + 1U;
    } else {
        free(decoded);
    }
    return node;
}

static AstNode *parse_identifier(Parser *parser, const Token *token)
{
    Symbol *symbol = scope_lookup(parser->scope, token->text);
    if (symbol == NULL) {
        semantic_error(parser, 2075U, token, "undeclared identifier");
        symbol = symbol_new(parser, token->text, type_basic(parser->arena, TYPE_INT), SYMBOL_VARIABLE);
        if (symbol != NULL) (void)scope_add_symbol(parser->arena, parser->scope, symbol);
    }
    AstNode *node = node_new(parser, NODE_IDENTIFIER, symbol->type, token);
    if (node != NULL) node->symbol = symbol;
    return node;
}

static AstNode *parse_arguments(Parser *parser, AstNode *call)
{
    AstNode *tail = NULL;
    if (accept(parser, ")")) return tail;
    for (;;) {
        AstNode *argument = parse_assignment(parser);
        if (tail == NULL) call->b = argument;
        else tail->next = argument;
        tail = argument;
        if (!accept(parser, ",")) { (void)expect(parser, ")"); break; }
    }
    return tail;
}

static void convert_call_arguments(Parser *parser, AstNode *call,
                                   Type *function_type, const Token *token)
{
    if (function_type == NULL) return;
    size_t count = 0U;
    for (AstNode *arg = call->b; arg != NULL; arg = arg->next) ++count;
    if ((!function_type->variadic && count != function_type->parameter_count) ||
        (function_type->variadic && count < function_type->parameter_count)) {
        semantic_error(parser, 2115U, token, "call argument count does not match prototype");
        return;
    }
    Symbol *parameter = function_type->parameters;
    AstNode *arg = call->b;
    AstNode **link = &call->b;
    while (arg != NULL) {
        AstNode *next = arg->next;
        Type *target = parameter != NULL ? parameter->type :
                       type_integer_promote(parser->arena, type_unqualified(arg->type));
        if (parameter != NULL || type_is_arithmetic(arg->type)) {
            AstNode *converted = implicit_conversion(parser, target, arg, token);
            if (converted != NULL) {
                converted->next = next;
                *link = converted;
                link = &converted->next;
            }
            if (parameter != NULL) parameter = parameter->next;
        } else {
            *link = arg;
            link = &arg->next;
        }
        arg = next;
    }
}

static AstNode *parse_primary(Parser *parser)
{
    const Token *token = peek(parser);
    if (token == NULL) {
        semantic_error(parser, 2080U, NULL, "expected expression");
        return NULL;
    }
    if (token->kind == TOKEN_NUMBER) { (void)take(parser); return parse_number(parser, token); }
    if (token->kind == TOKEN_CHARACTER) { (void)take(parser); return parse_character(parser, token); }
    if (token->kind == TOKEN_STRING) {
        (void)take(parser);
        AstNode *node = parse_string(parser, token);
        while (peek(parser) != NULL && peek(parser)->kind == TOKEN_STRING) {
            const Token *next = take(parser);
            AstNode *part = parse_string(parser, next);
            if (node == NULL || part == NULL) return node;
            size_t combined = node->text_length - 1U + part->text_length;
            char *text = cc64_xmalloc(combined);
            memcpy(text, node->text, node->text_length - 1U);
            memcpy(text + node->text_length - 1U, part->text, part->text_length);
            node->text = text;
            node->text_length = combined;
            node->type = type_array(parser->arena,
                                    type_basic(parser->arena, TYPE_CHAR),
                                    combined, true);
        }
        return node;
    }
    if (token_text(token, "(")) {
        (void)take(parser);
        if (token_text(peek(parser), "{")) {
            semantic_error(parser, 2081U, token, "compound literals are unsupported");
            while (peek(parser) != NULL && !token_text(peek(parser), "}")) (void)take(parser);
            (void)accept(parser, "}");
            return NULL;
        }
        AstNode *node = parse_expression(parser);
        (void)expect(parser, ")");
        return node;
    }
    if (token_is_identifier(token)) { (void)take(parser); return parse_identifier(parser, token); }
    if (token->kind == TOKEN_KEYWORD && (strcmp(token->text, "true") == 0 || strcmp(token->text, "false") == 0)) {
        (void)take(parser);
        AstNode *node = node_new(parser, NODE_INTEGER, type_basic(parser->arena, TYPE_BOOL), token);
        if (node != NULL) { node->integer = strcmp(token->text, "true") == 0 ? 1 : 0; node->unsigned_integer = (uint64_t)node->integer; }
        return node;
    }
    semantic_error(parser, 2082U, token, "expected primary expression");
    (void)take(parser);
    return NULL;
}

static AstNode *parse_postfix(Parser *parser)
{
    AstNode *node = parse_primary(parser);
    for (;;) {
        const Token *token = peek(parser);
        if (accept(parser, "[")) {
            AstNode *index = parse_expression(parser);
            (void)expect(parser, "]");
            Type *type = NULL;
            if (node != NULL && type_is_pointer(node->type)) type = node->type->base;
            else if (node != NULL && node->type->kind == TYPE_ARRAY) type = node->type->base;
            else if (index != NULL && type_is_pointer(index->type)) type = index->type->base;
            else semantic_error(parser, 2083U, token, "subscript requires pointer operand");
            AstNode *result = node_new(parser, NODE_INDEX, type, token);
            if (result != NULL) { result->a = node; result->b = index; }
            node = result;
        } else if (accept(parser, "(")) {
            const Token *call_token = token;
            AstNode *call = node_new(parser, NODE_CALL, type_basic(parser->arena, TYPE_LONG), call_token);
            if (call != NULL) call->a = node;
            (void)parse_arguments(parser, call);
            bool callable = node != NULL &&
                            (type_is_function(node->type) ||
                             (type_is_pointer(node->type) &&
                              type_is_function(node->type->base)));
            if (!callable) semantic_error(parser, 2084U, call_token, "called object is not a function");
            Type *called_type = NULL;
            if (node != NULL && type_is_function(node->type)) called_type = node->type;
            else if (node != NULL && type_is_pointer(node->type) &&
                     type_is_function(node->type->base)) called_type = node->type->base;
            if (call != NULL) {
                convert_call_arguments(parser, call, called_type, call_token);
                if (called_type != NULL) call->type = called_type->return_type;
            }
            node = call;
        } else if (accept(parser, ".") || accept(parser, "->")) {
            bool arrow = token_text(token, "->");
            const Token *member_token = token_is_identifier(peek(parser)) ? take(parser) : NULL;
            Type *base = node == NULL ? NULL : node->type;
            if (arrow) {
                if (!type_is_pointer(base)) semantic_error(parser, 2085U, token, "'->' requires pointer to struct or union");
                else base = base->base;
            }
            if ((base == NULL || (base->kind != TYPE_STRUCT && base->kind != TYPE_UNION))) {
                semantic_error(parser, 2086U, token, "member access requires struct or union");
            }
            Symbol *field = NULL;
            if (base != NULL && member_token != NULL) {
                for (Member *member = base->members; member != NULL; member = member->next) {
                    if (strcmp(member->name, member_token->text) == 0) { field = symbol_new(parser, member->name, member->type, SYMBOL_VARIABLE); if (field != NULL) field->offset = member->offset; break; }
                }
            }
            if (field == NULL && member_token != NULL) semantic_error(parser, 2087U, member_token, "unknown struct member");
            AstNode *result = node_new(parser, NODE_MEMBER, field == NULL ? type_basic(parser->arena, TYPE_INT) : field->type, token);
            if (result != NULL) { result->a = node; result->field = field; }
            node = result;
        } else if (accept(parser, "++") || accept(parser, "--")) {
            UnaryOperator op = token_text(token, "++") ? UNARY_POST_INCREMENT : UNARY_POST_DECREMENT;
            node = make_unary(parser, op, node, token);
        } else {
            break;
        }
    }
    return node;
}

static AstNode *parse_unary(Parser *parser)
{
    const Token *token = peek(parser);
    if (token == NULL) return NULL;
    if (accept(parser, "++") || accept(parser, "--")) {
        return make_unary(parser, token_text(token, "++") ? UNARY_PRE_INCREMENT : UNARY_PRE_DECREMENT, parse_unary(parser), token);
    }
    if (accept(parser, "&")) {
        AstNode *value = parse_cast(parser);
        if (value != NULL && !type_is_lvalue(value) && !type_is_function(value->type)) semantic_error(parser, 2090U, token, "cannot take address of rvalue");
        AstNode *node = node_new(parser, NODE_ADDRESS, type_pointer(parser->arena, value == NULL ? NULL : value->type, 0U), token);
        if (node != NULL) node->a = value;
        return node;
    }
    if (accept(parser, "*")) {
        AstNode *value = parse_cast(parser);
        if (value != NULL && !type_is_pointer(value->type)) semantic_error(parser, 2091U, token, "dereference requires pointer");
        Type *type = value != NULL && type_is_pointer(value->type) ? value->type->base : type_basic(parser->arena, TYPE_INT);
        AstNode *node = node_new(parser, NODE_DEREFERENCE, type, token);
        if (node != NULL) node->a = value;
        return node;
    }
    if (accept(parser, "+") || accept(parser, "-") || accept(parser, "!") || accept(parser, "~")) {
        UnaryOperator op = token_text(token, "+") ? UNARY_PLUS : token_text(token, "-") ? UNARY_MINUS : token_text(token, "!") ? UNARY_LOGICAL_NOT : UNARY_BITWISE_NOT;
        return make_unary(parser, op, parse_cast(parser), token);
    }
    if (token_is_keyword(token, "sizeof")) {
        (void)take(parser);
        if (accept(parser, "(")) {
            if (token_is_decl_start(parser, peek(parser))) {
                Type *type = parse_type_name(parser);
                (void)expect(parser, ")");
                AstNode *node = node_new(parser, NODE_SIZEOF, type_basic(parser->arena, TYPE_UNSIGNED_LONG), token);
                if (node != NULL) { node->type_operand = type; node->unsigned_integer = type_size(type); node->integer = (int64_t)type_size(type); }
                return node;
            }
            AstNode *value = parse_expression(parser);
            (void)expect(parser, ")");
            AstNode *node = node_new(parser, NODE_SIZEOF, type_basic(parser->arena, TYPE_UNSIGNED_LONG), token);
            if (node != NULL) { node->a = value; node->unsigned_integer = type_size(value == NULL ? NULL : value->type); }
            return node;
        }
        AstNode *value = parse_unary(parser);
        AstNode *node = node_new(parser, NODE_SIZEOF, type_basic(parser->arena, TYPE_UNSIGNED_LONG), token);
        if (node != NULL) { node->a = value; node->unsigned_integer = type_size(value == NULL ? NULL : value->type); }
        return node;
    }
    return parse_postfix(parser);
}

static AstNode *parse_cast(Parser *parser)
{
    const Token *token = peek(parser);
    if (token_text(token, "(") &&
        token_is_decl_start(parser, peek_at(parser, 1U))) {
        (void)take(parser);
        Type *type = parse_type_name(parser);
        (void)expect(parser, ")");
        if (type_is_void(type)) return make_cast(parser, type, parse_cast(parser), token);
        return make_cast(parser, type, parse_cast(parser), token);
    }
    return parse_unary(parser);
}

static unsigned binary_precedence(const Token *token)
{
    if (token_text(token, "*") || token_text(token, "/") || token_text(token, "%")) return 10U;
    if (token_text(token, "+") || token_text(token, "-")) return 9U;
    if (token_text(token, "<<") || token_text(token, ">>")) return 8U;
    if (token_text(token, "<") || token_text(token, ">") || token_text(token, "<=") || token_text(token, ">=")) return 7U;
    if (token_text(token, "==") || token_text(token, "!=")) return 6U;
    if (token_text(token, "&")) return 5U;
    if (token_text(token, "^")) return 4U;
    if (token_text(token, "|")) return 3U;
    if (token_text(token, "&&")) return 2U;
    if (token_text(token, "||")) return 1U;
    return 0U;
}

static BinaryOperator binary_operator(const Token *token)
{
    if (token_text(token, "*")) return BINARY_MULTIPLY;
    if (token_text(token, "/")) return BINARY_DIVIDE;
    if (token_text(token, "%")) return BINARY_REMAINDER;
    if (token_text(token, "+")) return BINARY_ADD;
    if (token_text(token, "-")) return BINARY_SUBTRACT;
    if (token_text(token, "<<")) return BINARY_LEFT_SHIFT;
    if (token_text(token, ">>")) return BINARY_RIGHT_SHIFT;
    if (token_text(token, "<")) return BINARY_LESS;
    if (token_text(token, "<=")) return BINARY_LESS_EQUAL;
    if (token_text(token, ">")) return BINARY_GREATER;
    if (token_text(token, ">=")) return BINARY_GREATER_EQUAL;
    if (token_text(token, "==")) return BINARY_EQUAL;
    if (token_text(token, "!=")) return BINARY_NOT_EQUAL;
    if (token_text(token, "&")) return BINARY_BITWISE_AND;
    if (token_text(token, "^")) return BINARY_BITWISE_XOR;
    if (token_text(token, "|")) return BINARY_BITWISE_OR;
    if (token_text(token, "&&")) return BINARY_LOGICAL_AND;
    return BINARY_LOGICAL_OR;
}

static AstNode *parse_binary(Parser *parser, unsigned minimum)
{
    AstNode *left = parse_cast(parser);
    for (;;) {
        const Token *token = peek(parser);
        unsigned precedence = binary_precedence(token);
        if (precedence == 0U || precedence < minimum) break;
        (void)take(parser);
        AstNode *right = parse_binary(parser, precedence + 1U);
        left = make_binary(parser, binary_operator(token), left, right, token);
    }
    return left;
}

static AstNode *parse_conditional(Parser *parser)
{
    AstNode *condition = parse_binary(parser, 0U);
    if (!accept(parser, "?")) return condition;
    const Token *token = peek(parser);
    AstNode *yes = parse_expression(parser);
    (void)expect(parser, ":");
    AstNode *no = parse_conditional(parser);
    if (condition != NULL && !type_is_scalar(condition->type)) semantic_error(parser, 2092U, token, "conditional condition must be scalar");
    Type *type = NULL;
    if (yes != NULL && no != NULL) {
        if (type_is_arithmetic(yes->type) && type_is_arithmetic(no->type)) type = type_usual_arithmetic(parser->arena, type_unqualified(yes->type), type_unqualified(no->type));
        else if (type_is_pointer(yes->type) && type_is_pointer(no->type)) type = yes->type;
        else if (type_is_pointer(yes->type)) type = yes->type;
        else if (type_is_pointer(no->type)) type = no->type;
        else semantic_error(parser, 2093U, token, "incompatible conditional branches");
    }
    AstNode *node = node_new(parser, NODE_CONDITIONAL, type, token);
    if (node != NULL) { node->a = condition; node->b = yes; node->c = no; }
    return node;
}

static bool assignment_operator(const Token *token, BinaryOperator *binary,
                               UnaryOperator *unary)
{
    if (token_text(token, "=")) { *binary = BINARY_ADD; *unary = UNARY_PLUS; return false; }
    if (token_text(token, "+=")) { *binary = BINARY_ADD; *unary = UNARY_PLUS; return true; }
    if (token_text(token, "-=")) { *binary = BINARY_SUBTRACT; *unary = UNARY_MINUS; return true; }
    if (token_text(token, "*=")) { *binary = BINARY_MULTIPLY; *unary = UNARY_PLUS; return true; }
    if (token_text(token, "/=")) { *binary = BINARY_DIVIDE; *unary = UNARY_PLUS; return true; }
    if (token_text(token, "%=")) { *binary = BINARY_REMAINDER; *unary = UNARY_PLUS; return true; }
    if (token_text(token, "&=")) { *binary = BINARY_BITWISE_AND; *unary = UNARY_PLUS; return true; }
    if (token_text(token, "|=")) { *binary = BINARY_BITWISE_OR; *unary = UNARY_PLUS; return true; }
    if (token_text(token, "^=")) { *binary = BINARY_BITWISE_XOR; *unary = UNARY_PLUS; return true; }
    if (token_text(token, "<<=")) { *binary = BINARY_LEFT_SHIFT; *unary = UNARY_PLUS; return true; }
    if (token_text(token, ">>=")) { *binary = BINARY_RIGHT_SHIFT; *unary = UNARY_PLUS; return true; }
    return false;
}

static AstNode *parse_assignment(Parser *parser)
{
    AstNode *left = parse_conditional(parser);
    const Token *token = peek(parser);
    BinaryOperator binary;
    UnaryOperator unary;
    bool compound = assignment_operator(token, &binary, &unary);
    if (token_text(token, "=") || compound) {
        (void)take(parser);
        if (!type_is_lvalue(left) || type_has_const(left->type)) semantic_error(parser, 2094U, token, "assignment target is not modifiable lvalue");
        AstNode *right = parse_assignment(parser);
        if (token_text(token, "=")) {
            if (right != NULL && !assignment_compatible(parser, left->type, right->type, token)) right = right;
            else if (right != NULL) right = implicit_conversion(parser, left->type, right, token);
        } else {
            AstNode *operation = make_binary(parser, binary, left, right, token);
            right = operation;
        }
        AstNode *node = node_new(parser, NODE_ASSIGNMENT, left == NULL ? NULL : left->type, token);
        if (node != NULL) {
            node->a = left;
            node->b = right;
            if (compound) {
                node->binary = binary;
                node->compound_assignment = true;
            }
        }
        return node;
    }
    return left;
}

static AstNode *parse_expression(Parser *parser)
{
    AstNode *node = parse_assignment(parser);
    while (accept(parser, ",")) {
        const Token *token = peek(parser);
        AstNode *right = parse_assignment(parser);
        AstNode *comma = node_new(parser, NODE_COMMA, right == NULL ? NULL : right->type, token);
        if (comma != NULL) { comma->a = node; comma->b = right; }
        node = comma;
    }
    return node;
}

static AstNode *parse_initializer(Parser *parser, Type *type)
{
    const Token *token = peek(parser);
    if (accept(parser, "{")) {
        AstNode *initializer = node_new(parser, NODE_INITIALIZER, type, token);
        AstNode *tail = NULL;
        if (!token_text(peek(parser), "}")) {
            for (;;) {
                AstNode *value = parse_initializer(parser, type);
                if (tail == NULL) initializer->a = value;
                else tail->next = value;
                tail = value;
                if (!accept(parser, ",")) break;
            }
        }
        (void)expect(parser, "}");
        return initializer;
    }
    AstNode *value = parse_assignment(parser);
    if (value != NULL && type != NULL && type_is_scalar(type)) value = implicit_conversion(parser, type, value, token);
    AstNode *initializer = node_new(parser, NODE_INITIALIZER, type, token);
    if (initializer != NULL) initializer->a = value;
    return initializer;
}

static AstNode *parse_compound(Parser *parser)
{
    const Token *token = peek(parser);
    if (!expect(parser, "{")) return NULL;
    Scope *old_scope = parser->scope;
    parser->scope = scope_create(parser->arena, old_scope);
    AstNode *compound = node_new(parser, NODE_COMPOUND, type_basic(parser->arena, TYPE_VOID), token);
    AstNode *tail = NULL;
    while (!token_text(peek(parser), "}") && peek(parser) != NULL && !parser->failed) {
        AstNode *statement = parse_statement(parser);
        if (statement == NULL) {
            while (peek(parser) != NULL && !token_text(peek(parser), "}")) (void)take(parser);
            break;
        }
        if (tail == NULL) compound->a = statement;
        else tail->next = statement;
        tail = statement;
        /* A declaration statement may carry a chain of nodes, one per
           declarator. Splice the whole chain so that every declarator keeps
           its initializer and its frame slot. */
        while (tail->next != NULL) tail = tail->next;
    }
    (void)expect(parser, "}");
    parser->scope = old_scope;
    return compound;
}

static void complete_initializer_array(Type *type, AstNode *initializer)
{
    if (type == NULL || initializer == NULL || type->kind != TYPE_ARRAY ||
        !type->incomplete || type->base == NULL || type->base->size == 0U) return;
    AstNode *value = initializer->kind == NODE_INITIALIZER ? initializer->a : initializer;
    size_t count = 0U;
    if (value != NULL && value->kind == NODE_STRING) {
        count = value->text_length;
    } else if (value != NULL && value->kind == NODE_INITIALIZER) {
        for (AstNode *item = value; item != NULL; item = item->next) ++count;
    }
    if (count == 0U || count > SIZE_MAX / type->base->size) return;
    type->array_count = count;
    type->size = count * type->base->size;
    type->incomplete = false;
}

static AstNode *parse_local_declaration(Parser *parser)
{
    DeclSpec spec;
    if (!parse_decl_specs(parser, &spec)) return NULL;
    if (accept(parser, ";")) return node_new(parser, NODE_NULL_STATEMENT, NULL, peek(parser));
    AstNode *first = NULL;
    AstNode *tail = NULL;
    for (;;) {
        const char *name = NULL;
        Symbol *parameters = NULL;
        size_t parameter_count = 0U;
        bool variadic = false;
        Type *type = spec.type;
        bool local_declarator_ok = parse_declarator(parser, type, &name,
                                                     &parameters, &parameter_count,
                                                     &variadic);
        type = parser->last_declarator_type;
        if (!local_declarator_ok || name == NULL) {
            semantic_error(parser, 2095U, peek(parser), "declarator requires a name");
            return first;
        }
        if (spec.storage == STORAGE_TYPEDEF) {
            Symbol *symbol = symbol_new(parser, name, type, SYMBOL_TYPEDEF);
            if (symbol == NULL || scope_lookup(parser->scope, name) != NULL || !scope_add_symbol(parser->arena, parser->scope, symbol)) {
                semantic_error(parser, 2096U, peek(parser), "duplicate declaration");
                return first;
            }
        } else if (type_is_function(type)) {
            semantic_error(parser, 2097U, peek(parser), "function declaration inside function is unsupported");
        } else {
            Symbol *symbol = symbol_new(parser, name, type, SYMBOL_VARIABLE);
            if (symbol == NULL) return first;
            char *source_name = symbol->name;
            symbol->storage = spec.storage;
            symbol->linkage = spec.storage == STORAGE_STATIC ? LINKAGE_INTERNAL :
                              (spec.storage == STORAGE_EXTERN ? LINKAGE_EXTERNAL :
                               LINKAGE_NONE);
            if (spec.storage == STORAGE_STATIC) symbol->defined = true;
            if (scope_lookup(parser->scope, name) != NULL) {
                semantic_error(parser, 2098U, peek(parser), "duplicate local declaration");
            } else if (!scope_add_symbol(parser->arena, parser->scope, symbol)) return first;
            if (spec.storage == STORAGE_STATIC) {
                char internal_name[48];
                (void)snprintf(internal_name, sizeof(internal_name),
                               ".Lstatic.%u", parser->static_local_count++);
                symbol->name = cc64_xstrdup(internal_name);
                (void)source_name;
            }
            if (accept(parser, "=")) {
                symbol->initializer = parse_initializer(parser, type);
                complete_initializer_array(type, symbol->initializer);
            }
            AstNode *declaration = node_new(parser, NODE_DECLARATION, type, peek(parser));
            if (declaration != NULL) { declaration->symbol = symbol; declaration->a = symbol->initializer; if (first == NULL) first = declaration; else tail->next = declaration; tail = declaration; }
        }
        if (!accept(parser, ",")) break;
    }
    (void)expect(parser, ";");
    return first;
}

static AstNode *parse_statement(Parser *parser)
{
    const Token *token = peek(parser);
    if (token == NULL) return NULL;
    if (token_text(token, "{")) return parse_compound(parser);
    if (token_text(token, ";")) { (void)take(parser); return node_new(parser, NODE_NULL_STATEMENT, NULL, token); }
    if (token_is_keyword(token, "if")) {
        (void)take(parser); (void)expect(parser, "(");
        AstNode *condition = parse_expression(parser); (void)expect(parser, ")");
        AstNode *then_branch = parse_statement(parser);
        AstNode *else_branch = accept(parser, "else") ? parse_statement(parser) : NULL;
        AstNode *node = node_new(parser, NODE_IF, type_basic(parser->arena, TYPE_VOID), token);
        if (node != NULL) { node->a = condition; node->b = then_branch; node->c = else_branch; }
        return node;
    }
    if (token_is_keyword(token, "while")) {
        (void)take(parser); (void)expect(parser, "("); AstNode *condition = parse_expression(parser); (void)expect(parser, ")"); ++parser->loop_depth; AstNode *body = parse_statement(parser); --parser->loop_depth;
        AstNode *node = node_new(parser, NODE_WHILE, type_basic(parser->arena, TYPE_VOID), token); if (node != NULL) { node->a = condition; node->b = body; } return node;
    }
    if (token_is_keyword(token, "do")) {
        (void)take(parser); ++parser->loop_depth; AstNode *body = parse_statement(parser); --parser->loop_depth; (void)expect(parser, "while"); (void)expect(parser, "("); AstNode *condition = parse_expression(parser); (void)expect(parser, ")"); (void)expect(parser, ";");
        AstNode *node = node_new(parser, NODE_DO, type_basic(parser->arena, TYPE_VOID), token); if (node != NULL) { node->a = condition; node->b = body; } return node;
    }
    if (token_is_keyword(token, "for")) {
        (void)take(parser); (void)expect(parser, "(");
        Scope *old_scope = parser->scope; parser->scope = scope_create(parser->arena, old_scope);
        AstNode *init = NULL;
        if (token_text(peek(parser), ";")) {
            (void)take(parser);
        } else if (token_is_start_of_declaration(parser)) {
            /* parse_local_declaration consumes the terminating semicolon. */
            init = parse_local_declaration(parser);
        } else {
            /* An expression initializer stops before its own semicolon. */
            init = parse_expression(parser);
            (void)expect(parser, ";");
        }
        AstNode *condition = token_text(peek(parser), ";") ? NULL : parse_expression(parser);
        (void)expect(parser, ";");
        AstNode *step = token_text(peek(parser), ")") ? NULL : parse_expression(parser);
        (void)expect(parser, ")"); ++parser->loop_depth; AstNode *body = parse_statement(parser); --parser->loop_depth; parser->scope = old_scope;
        AstNode *node = node_new(parser, NODE_FOR, type_basic(parser->arena, TYPE_VOID), token); if (node != NULL) { node->a = init; node->b = condition; node->c = step; node->d = body; } return node;
    }
    if (token_is_keyword(token, "switch")) {
        (void)take(parser); (void)expect(parser, "("); AstNode *condition = parse_expression(parser); (void)expect(parser, ")"); ++parser->switch_depth; AstNode *body = parse_statement(parser); --parser->switch_depth;
        AstNode *node = node_new(parser, NODE_SWITCH, type_basic(parser->arena, TYPE_VOID), token); if (node != NULL) { node->a = condition; node->b = body; } return node;
    }
    if (token_is_keyword(token, "case")) {
        (void)take(parser); AstNode *value = parse_conditional(parser); (void)expect(parser, ":");
        if (parser->switch_depth == 0U) semantic_error(parser, 2099U, token, "case outside switch");
        AstNode *node = node_new(parser, NODE_CASE, type_basic(parser->arena, TYPE_VOID), token); if (node != NULL) node->a = value; return node;
    }
    if (token_is_keyword(token, "default")) {
        (void)take(parser); (void)expect(parser, ":"); if (parser->switch_depth == 0U) semantic_error(parser, 2100U, token, "default outside switch");
        return node_new(parser, NODE_DEFAULT, type_basic(parser->arena, TYPE_VOID), token);
    }
    if (token_is_keyword(token, "break")) { (void)take(parser); (void)expect(parser, ";"); if (parser->loop_depth == 0U && parser->switch_depth == 0U) semantic_error(parser, 2101U, token, "break outside loop or switch"); return node_new(parser, NODE_BREAK, type_basic(parser->arena, TYPE_VOID), token); }
    if (token_is_keyword(token, "continue")) { (void)take(parser); (void)expect(parser, ";"); if (parser->loop_depth == 0U) semantic_error(parser, 2102U, token, "continue outside loop"); return node_new(parser, NODE_CONTINUE, type_basic(parser->arena, TYPE_VOID), token); }
    if (token_is_keyword(token, "return")) { const Token *return_token = take(parser); AstNode *value = NULL; if (!token_text(peek(parser), ";")) value = parse_expression(parser); (void)expect(parser, ";"); if (parser->current_function == NULL || parser->current_function->type == NULL || parser->current_function->type->return_type == NULL) semantic_error(parser, 2103U, return_token, "return outside function"); AstNode *node = node_new(parser, NODE_RETURN, type_basic(parser->arena, TYPE_VOID), return_token); if (node != NULL) node->a = value; return node; }
    if (token_is_keyword(token, "goto")) {
        (void)take(parser);
        const Token *label = take(parser);
        if (!token_is_identifier(label)) semantic_error(parser, 2106U, label, "goto requires a label identifier");
        (void)expect(parser, ";");
        AstNode *node = node_new(parser, NODE_GOTO, type_basic(parser->arena, TYPE_VOID), token);
        if (node != NULL && label != NULL) node->text = copy_name(parser, label->text);
        return node;
    }
    if (token_is_identifier(token) && token_text(peek_at(parser, 1U), ":")) { const Token *label = take(parser); (void)take(parser); AstNode *node = node_new(parser, NODE_LABEL, type_basic(parser->arena, TYPE_VOID), token); if (node != NULL) node->text = copy_name(parser, label->text); return node; }
    if (token_is_start_of_declaration(parser)) return parse_local_declaration(parser);
    AstNode *expression = parse_expression(parser); (void)expect(parser, ";");
    AstNode *node = node_new(parser, NODE_EXPRESSION_STATEMENT, type_basic(parser->arena, TYPE_VOID), token); if (node != NULL) node->a = expression; return node;
}

static bool append_declaration(TranslationUnit *unit, AstNode *node)
{
    return append_node(&unit->declarations, &unit->count, &unit->capacity, node);
}

static bool define_symbol(Parser *parser, const char *name, Type *type,
                          StorageClass storage, bool function, bool definition,
                          Symbol **result)
{
    Symbol *existing = scope_lookup(parser->global_scope, name);
    Symbol *symbol = existing;
    if (symbol != NULL && symbol->class != (function ? SYMBOL_FUNCTION : SYMBOL_VARIABLE)) {
        semantic_error(parser, 2104U, peek(parser), "symbol redeclared with a different kind");
    }
    if (symbol == NULL) {
        symbol = symbol_new(parser, name, type, function ? SYMBOL_FUNCTION : SYMBOL_VARIABLE);
        if (symbol == NULL || !scope_add_symbol(parser->arena, parser->global_scope, symbol)) return false;
    }
    symbol->type = type;
    symbol->storage = storage;
    symbol->linkage = storage == STORAGE_STATIC ? LINKAGE_INTERNAL : LINKAGE_EXTERNAL;
    if (definition && symbol->defined) semantic_error(parser, 2105U, peek(parser), "duplicate definition");
    if (definition) symbol->defined = true;
    if (result != NULL) *result = symbol;
    return true;
}

static void parse_function_definition(Parser *parser, const DeclSpec *spec,
                                      Type *type, const char *name,
                                      Symbol *parameters, size_t parameter_count)
{
    Symbol *function = NULL;
    if (!define_symbol(parser, name, type, spec->storage, true, true, &function)) return;
    (void)parameter_count;
    Scope *function_scope = scope_create(parser->arena, parser->global_scope);
    for (Symbol *parameter = parameters; parameter != NULL; parameter = parameter->next) {
        if (!scope_add_symbol(parser->arena, function_scope, parameter)) return;
    }
    Scope *old_scope = parser->scope;
    Symbol *old_function = parser->current_function;
    parser->scope = function_scope;
    parser->current_function = function;
    AstNode *body = parse_compound(parser);
    parser->scope = old_scope;
    parser->current_function = old_function;
    AstNode *definition = node_new(parser, NODE_FUNCTION_DEFINITION, type, peek(parser));
    if (definition != NULL) { definition->symbol = function; definition->a = body; if (append_declaration(parser->unit, definition)) { if (strcmp(name, "main") == 0) parser->unit->has_main = true; function->initializer = body; } }
}

static void parse_global_declaration(Parser *parser, const DeclSpec *spec)
{
    if (accept(parser, ";")) return;
    for (;;) {
        const char *name = NULL;
        Symbol *parameters = NULL;
        size_t parameter_count = 0U;
        bool variadic = false;
        Type *type = spec->type;
        bool global_declarator_ok = parse_declarator(parser, type, &name,
                                                      &parameters, &parameter_count,
                                                      &variadic);
        type = parser->last_declarator_type;
        if (!global_declarator_ok || name == NULL) {
            semantic_error(parser, 2106U, peek(parser), "global declarator requires a name");
            return;
        }
        if (spec->storage == STORAGE_TYPEDEF) {
            Symbol *symbol = symbol_new(parser, name, type, SYMBOL_TYPEDEF);
            if (symbol == NULL || scope_lookup(parser->global_scope, name) != NULL || !scope_add_symbol(parser->arena, parser->global_scope, symbol)) semantic_error(parser, 2107U, peek(parser), "duplicate typedef");
        } else if (type_is_function(type) && token_text(peek(parser), "{")) {
            parse_function_definition(parser, spec, type, name, parameters, parameter_count);
            return;
        } else {
            Symbol *symbol = NULL;
            bool is_function = type_is_function(type);
            bool is_definition = !is_function && !token_text(peek(parser), "=") &&
                                 spec->storage != STORAGE_EXTERN;
            if (!define_symbol(parser, name, type, spec->storage, is_function, is_definition, &symbol)) return;
            if (accept(parser, "=")) {
                symbol->initializer = parse_initializer(parser, type);
                complete_initializer_array(type, symbol->initializer);
                symbol->defined = true;
                AstNode *declaration = node_new(parser, NODE_DECLARATION, type, peek(parser));
                if (declaration != NULL) { declaration->symbol = symbol; declaration->a = symbol->initializer; (void)append_declaration(parser->unit, declaration); }
            } else if (is_definition) {
                AstNode *declaration = node_new(parser, NODE_DECLARATION, type, peek(parser));
                if (declaration != NULL) { declaration->symbol = symbol; (void)append_declaration(parser->unit, declaration); }
            }
        }
        if (!accept(parser, ",")) break;
    }
    (void)expect(parser, ";");
}

bool parse_tokens(Arena *arena, TokenList *tokens, DiagnosticSink *diagnostics,
                  TranslationUnit *unit)
{
    memset(unit, 0, sizeof(*unit));
    Token *compact = arena_alloc_array(arena, tokens->count + 1U, sizeof(*compact));
    if (compact == NULL) return false;
    size_t count = 0U;
    for (size_t i = 0U; i < tokens->count; ++i) {
        if (tokens->items[i].kind != TOKEN_NEWLINE && tokens->items[i].kind != TOKEN_EOF) {
            compact[count++] = tokens->items[i];
        }
    }
    Parser parser = {arena, diagnostics, compact, count, 0U, NULL, NULL, unit,
                      false, NULL, NULL, 0U, 0U, 0U};
    parser.global_scope = scope_create(arena, NULL);
    parser.scope = parser.global_scope;
    if (parser.global_scope == NULL) return false;
    while (parser.position < parser.count && !parser.failed) {
        if (accept(&parser, ";")) continue;
        DeclSpec spec;
        if (!parse_decl_specs(&parser, &spec)) {
            while (peek(&parser) != NULL && !token_text(peek(&parser), ";")) (void)take(&parser);
            (void)accept(&parser, ";");
            continue;
        }
        parse_global_declaration(&parser, &spec);
    }
    unit->globals = parser.global_scope->bindings == NULL ? NULL : parser.global_scope->bindings->symbol;
    return !parser.failed;
}
