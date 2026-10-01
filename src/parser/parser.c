#include "semantic/semantic.h"
#include "frontend/numeric.h"

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
    Arena *node_arena;
    DiagnosticSink *diagnostics;
    Token *tokens;
    size_t count;
    size_t position;
    Scope *global_scope;
    Scope *scope;
    TranslationUnit *unit;
    /* `errored` records that the declaration being parsed now is wrong and
       the next one may be parsed after it; `failed` records that the stream
       cannot be parsed further. Whether the unit as a whole was accepted is
       decided from the diagnostics the sink holds, not from either flag, so
       a per-declaration flag can be cleared without losing an earlier
       declaration's diagnostic. */
    bool errored;
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
    /* A missing token is recoverable: the caller skips the declaration and
       the next one is parsed. A stream that has already run out is not. */
    if (token == NULL) parser->failed = true;
    else parser->errored = true;
    return token;
}

/* A diagnostic records that the unit is wrong but does not stop the parse:
   a program with several independent mistakes should report all of them, so
   the parser recovers at the next declaration and the caller decides from
   the diagnostic count whether the unit was accepted. Only a failure that
   leaves the stream unusable sets `failed`. */
static void semantic_error(Parser *parser, unsigned id, const Token *token,
                           const char *message)
{
    diagnostic_emit(parser->diagnostics, id, DIAG_SEMANTIC,
                    token == NULL ? NULL : token->source,
                    token == NULL ? 0U : token->line,
                    token == NULL ? 0U : token->column, message);
    parser->errored = true;
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
    /* Nodes come from the node arena, which the caller releases once a
       declaration has been lowered. Names, types, and literal text stay in the
       durable arena, because the back end refers to them after lowering. */
    AstNode *node = arena_alloc(parser->node_arena, sizeof(*node));
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

/* A copy of a node tree in the durable arena, for the few values the back end
   still needs after the tree they were parsed from has been released. The
   `next` chain is a sibling list, so it is copied as well. */
static AstNode *copy_tree(Parser *parser, AstNode *node)
{
    if (node == NULL) return NULL;
    AstNode *copy = arena_alloc(parser->arena, sizeof(*copy));
    if (copy == NULL) {
        parser->failed = true;
        return NULL;
    }
    *copy = *node;
    copy->a = copy_tree(parser, node->a);
    copy->b = copy_tree(parser, node->b);
    copy->c = copy_tree(parser, node->c);
    copy->d = copy_tree(parser, node->d);
    copy->next = copy_tree(parser, node->next);
    return copy;
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
        "inline", "_Noreturn", "_Alignas", "_Atomic", "__uint128_t"
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

/* Evaluates an integer constant expression over a parsed tree. Array bounds,
   case labels, and enumerators all need the same arithmetic, and accepting a
   bare literal only would reject ordinary spellings such as [4 * 2 + 1] or a
   size computed from a macro. A value the evaluator cannot prove constant is
   rejected rather than folded. */
static bool const_eval(AstNode *node, uint64_t *result)
{
    if (node == NULL) return false;
    if (node->kind == NODE_INTEGER || node->kind == NODE_SIZEOF) {
        *result = node->unsigned_integer;
        return true;
    }
    if (node->kind == NODE_CAST) return const_eval(node->a, result);
    if (node->kind == NODE_IDENTIFIER && node->symbol != NULL &&
        node->symbol->class == SYMBOL_ENUM_CONSTANT) {
        *result = (uint64_t)node->symbol->enum_value;
        return true;
    }
    if (node->kind == NODE_UNARY) {
        uint64_t operand = 0UL;
        if (!const_eval(node->a, &operand)) return false;
        switch (node->unary) {
        case UNARY_PLUS: *result = operand; return true;
        case UNARY_MINUS: *result = 0UL - operand; return true;
        case UNARY_BITWISE_NOT: *result = ~operand; return true;
        case UNARY_LOGICAL_NOT: *result = operand == 0UL ? 1UL : 0UL; return true;
        default: return false;
        }
    }
    if (node->kind == NODE_CONDITIONAL) {
        uint64_t condition = 0UL;
        if (!const_eval(node->a, &condition)) return false;
        return const_eval(condition != 0UL ? node->b : node->c, result);
    }
    if (node->kind != NODE_BINARY) return false;
    uint64_t left = 0UL;
    uint64_t right = 0UL;
    if (!const_eval(node->a, &left) || !const_eval(node->b, &right)) return false;
    bool is_signed = node->type == NULL || type_is_signed(node->type);
    switch (node->binary) {
    case BINARY_ADD: *result = left + right; return true;
    case BINARY_SUBTRACT: *result = left - right; return true;
    case BINARY_MULTIPLY: *result = left * right; return true;
    case BINARY_DIVIDE:
        if (right == 0UL) return false;
        *result = is_signed ? (uint64_t)((int64_t)left / (int64_t)right)
                            : left / right;
        return true;
    case BINARY_REMAINDER:
        if (right == 0UL) return false;
        *result = is_signed ? (uint64_t)((int64_t)left % (int64_t)right)
                            : left % right;
        return true;
    case BINARY_LEFT_SHIFT: *result = left << (right & 63UL); return true;
    case BINARY_RIGHT_SHIFT:
        *result = is_signed ? (uint64_t)((int64_t)left >> (right & 63UL))
                            : left >> (right & 63UL);
        return true;
    case BINARY_BITWISE_AND: *result = left & right; return true;
    case BINARY_BITWISE_OR: *result = left | right; return true;
    case BINARY_BITWISE_XOR: *result = left ^ right; return true;
    case BINARY_LOGICAL_AND: *result = left != 0UL && right != 0UL ? 1UL : 0UL; return true;
    case BINARY_LOGICAL_OR: *result = left != 0UL || right != 0UL ? 1UL : 0UL; return true;
    case BINARY_LESS: *result = left < right ? 1UL : 0UL; return true;
    case BINARY_LESS_EQUAL: *result = left <= right ? 1UL : 0UL; return true;
    case BINARY_GREATER: *result = left > right ? 1UL : 0UL; return true;
    case BINARY_GREATER_EQUAL: *result = left >= right ? 1UL : 0UL; return true;
    case BINARY_EQUAL: *result = left == right ? 1UL : 0UL; return true;
    case BINARY_NOT_EQUAL: *result = left != right ? 1UL : 0UL; return true;
    default: return false;
    }
}

static bool parse_constant_size(Parser *parser, size_t *value)
{
    AstNode *node = parse_conditional(parser);
    uint64_t raw = 0UL;
    if (node == NULL) {
        semantic_error(parser, 2010U, peek(parser),
                       "array size must be an integer constant expression");
        return false;
    }
    if (!const_eval(node, &raw)) {
        semantic_error(parser, 2010U, NULL,
                       "array size must be an integer constant expression");
        return false;
    }
    if (raw > (uint64_t)SIZE_MAX) {
        semantic_error(parser, 2011U, NULL, "invalid array size");
        return false;
    }
    *value = (size_t)raw;
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
        /* A struct or union is a valid parameter: it arrives in registers, and
           the ABI revision documents how its eightbytes are classified. Only
           `void` and a type that is not yet known are rejected here. */
        if (type == NULL || type->kind == TYPE_VOID || type->incomplete) {
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
    /* Array suffixes are written outermost first, so they are collected in
       source order and applied in reverse: in "a[2][3]" the element type is an
       array of three, which is what the inner suffix describes. Applying them
       as they are read builds the opposite shape, which compiles and then
       silently misplaces every initializer. */
    struct ArraySuffix { size_t count; bool known; } suffixes[8];
    size_t suffix_count = 0U;
    for (;;) {
        if (accept(parser, "[")) {
            size_t count = 0U;
            bool known = true;
            if (token_text(peek(parser), "]")) {
                (void)take(parser);
                known = false;
            } else {
                if (!parse_constant_size(parser, &count) || !expect(parser, "]")) {
                    return false;
                }
            }
            if (suffix_count == sizeof(suffixes) / sizeof(suffixes[0])) {
                semantic_error(parser, 2030U, peek(parser),
                               "array declarator has too many dimensions");
                return false;
            }
            suffixes[suffix_count].count = count;
            suffixes[suffix_count].known = known;
            ++suffix_count;
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
    for (size_t i = suffix_count; i > 0U; --i) {
        type = type_array(parser->arena, type, suffixes[i - 1U].count,
                          suffixes[i - 1U].known);
    }
    parser->last_declarator_type = type;
    return type != NULL;
}

/* skip_balanced consumes a parenthesized argument list, including nested
   parentheses, so a rejected declaration continues from after its own
   specifier instead of being re-parsed as the next declaration's. */
static void skip_balanced(Parser *parser)
{
    if (!token_text(peek(parser), "(")) return;
    unsigned depth = 0U;
    while (peek(parser) != NULL) {
        if (accept(parser, "(")) ++depth;
        else if (accept(parser, ")")) { --depth; if (depth == 0U) return; }
        else (void)take(parser);
    }
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
            if (token_text(peek(parser), "(")) skip_balanced(parser);
            continue;
        }
        /* The remaining deferred specifiers each get their own diagnostic, so a
           program that uses one is told which construct is out of contract
           rather than receiving the generic "no type specifier" error and a
           cascade of follow-on failures. */
        if (token_is_keyword(token, "_Complex") || token_is_keyword(token, "_Imaginary")) {
            semantic_error(parser, 2037U, token, "complex and imaginary types are deferred");
            (void)take(parser);
            continue;
        }
        if (token_is_keyword(token, "_Thread_local")) {
            semantic_error(parser, 2038U, token, "thread-local storage is deferred");
            (void)take(parser);
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
        if (token_is_keyword(token, "__uint128_t")) {
            /* The one integer type wider than the machine's own register. It
               is spelled as one name because it is not built from the integer
               specifiers, and a program that reaches for it says so. */
            if (spec->has_type) {
                semantic_error(parser, 2033U, token,
                               "'__uint128_t' cannot be combined with another type specifier");
                (void)take(parser);
                continue;
            }
            spec->type = type_basic(parser->arena, TYPE_UINT128);
            spec->has_type = true;
            (void)take(parser);
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
    /* `long double` is a distinct C type; the ABI gives binary64 to `double`
       only, so accepting the spelling would silently give a program a
       narrower type than it asked for. */
    if (saw_double && long_count != 0U) {
        semantic_error(parser, 2039U, peek(parser), "'long double' is deferred");
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
    /* A compound literal names an unnamed object rather than a value, so it can
       be assigned through, have its address taken, and be indexed exactly as a
       declared object of the same type can. */
    if (node->kind == NODE_COMPOUND_LITERAL) return true;
    return false;
}

/* An array or function operand converts to a pointer, so a cast of a string
   literal or a function name to a pointer type is a scalar conversion. */
static bool type_decays_to_pointer(const Type *type)
{
    return type_is_scalar(type) ||
           (type != NULL && (type->kind == TYPE_ARRAY || type->kind == TYPE_FUNCTION));
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
    if (!type_is_scalar(type) || !type_decays_to_pointer(value->type)) {
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
    if (type_is_integer(target) && value->type != NULL &&
        type_is_pointer(value->type)) {
        /* Turning a pointer into a number is how a program tests one against
           a null or carries one across an interface that has no pointer. The
           target's pointers and its widest integers are the same width, so the
           conversion is exact when the integer can hold a pointer, and a
           narrower one is refused rather than silently losing the address. */
        Type *pointer = type_pointer(parser->arena, value->type->base, 0U);
        if (type_size(target) < type_size(pointer)) {
            semantic_error(parser, 2043U, token,
                           "implicit conversion is not valid");
            return value;
        }
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
    } else if (op == BINARY_MULTIPLY || op == BINARY_DIVIDE) {
        /* Multiplication and division are the arithmetic operators, so they
           accept the floating types; the remainder and the bitwise operators
           are defined only for integers. */
        if (!type_is_arithmetic(left->type) || !type_is_arithmetic(right->type)) {
            semantic_error(parser, 2052U, token, "arithmetic operator requires arithmetic operands");
        }
        type = type_usual_arithmetic(parser->arena, type_unqualified(left->type), type_unqualified(right->type));
        left = make_cast(parser, type, left, token);
        right = make_cast(parser, type, right, token);
    } else if (op == BINARY_REMAINDER || op == BINARY_BITWISE_AND ||
               op == BINARY_BITWISE_XOR || op == BINARY_BITWISE_OR) {
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
static AstNode *parse_postfix_from(Parser *parser, AstNode *node);

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
        /* The conversion is project code so that a self-hosted build and the
           bootstrap build produce identical constants. */
        size_t length = strlen(text);
        char digits[64];
        if (length >= sizeof(digits)) {
            semantic_error(parser, 2070U, token, "invalid floating constant");
            digits[0] = '\0';
        } else {
            size_t out = 0U;
            for (size_t i = 0U; i < length; ++i) {
                char byte = text[i];
                if (float_suffix && (byte == 'f' || byte == 'F')) break;
                digits[out] = byte;
                ++out;
            }
            digits[out] = '\0';
        }
        uint64_t bits = 0UL;
        double value = 0.0;
        if (cc64_decimal_to_double(digits, &bits)) {
            if (float_suffix) {
                /* The rounding to binary32 is project code, so a bootstrap
                   build and a self-hosted build round the same text the same
                   way. The rounded encoding is then widened back to the
                   binary64 the instruction carries, because copying the four
                   binary32 bytes into an eight-byte value left the high half
                   of the constant zero and every float constant in a program
                   was a denormal close to zero. */
                uint64_t widened = cc64_float_to_double(cc64_double_to_float(bits));
                memcpy(&value, &widened, sizeof(widened));
            } else {
                memcpy(&value, &bits, sizeof(bits));
            }
        } else {
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

/* The operators that may follow a primary expression. The loop takes the
   operand it is given rather than reading one, because a compound literal is
   also a primary expression but is reached from the cast level instead. */
static AstNode *parse_postfix_from(Parser *parser, AstNode *node)
{
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

static AstNode *parse_postfix(Parser *parser)
{
    return parse_postfix_from(parser, parse_primary(parser));
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
    /* `_Alignof` is `sizeof` restricted to a type name, so it reuses the same
       result node and only differs in the operand it accepts. */
    if (token_is_keyword(token, "_Alignof")) {
        (void)take(parser);
        (void)expect(parser, "(");
        Type *type = parse_type_name(parser);
        (void)expect(parser, ")");
        AstNode *node = node_new(parser, NODE_SIZEOF, type_basic(parser->arena, TYPE_UNSIGNED_LONG), token);
        if (node != NULL) { node->type_operand = type; node->unsigned_integer = type_alignment(type); node->integer = (int64_t)type_alignment(type); }
        return node;
    }
    if (token_is_keyword(token, "_Generic")) {
        (void)take(parser);
        semantic_error(parser, 2042U, token, "generic selection is deferred");
        (void)expect(parser, "(");
        while (peek(parser) != NULL && !token_text(peek(parser), ")")) (void)take(parser);
        (void)expect(parser, ")");
        return NULL;
    }
    return parse_postfix(parser);
}

static AstNode *parse_compound_literal(Parser *parser, Type *type,
                                       const Token *token);
static void complete_initializer_array(Type *type, AstNode *initializer);

static AstNode *parse_cast(Parser *parser)
{
    const Token *token = peek(parser);
    if (token_text(token, "(") &&
        token_is_decl_start(parser, peek_at(parser, 1U))) {
        (void)take(parser);
        Type *type = parse_type_name(parser);
        (void)expect(parser, ")");
        /* A type name in parentheses followed by a braced list is a compound
           literal: an unnamed object the list initializes, whose value is the
           object. Anywhere else in that position the parenthesized type name
           is a cast. The literal is an operand, so a member selection or an
           index applied to it selects from the object and not from whatever
           the braced list produced. */
        if (token_text(peek(parser), "{")) {
            return parse_postfix_from(parser,
                                      parse_compound_literal(parser, type, token));
        }
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
    /* Both operands undergo the usual conversions, so an array or function
       operand is converted to a pointer before the result type is chosen. */
    yes = decay_value(parser, yes, token);
    no = decay_value(parser, no, token);
    Type *type = NULL;
    if (yes != NULL && no != NULL) {
        if (type_is_void(yes->type) && type_is_void(no->type)) {
            /* Both branches do nothing. The form is how a statement macro that
               has to work without a statement of its own is written, so the
               conditional is void rather than an error. */
            type = type_basic(parser->arena, TYPE_VOID);
        } else if (type_is_arithmetic(yes->type) && type_is_arithmetic(no->type)) type = type_usual_arithmetic(parser->arena, type_unqualified(yes->type), type_unqualified(no->type));
        else if (type_is_pointer(yes->type) && type_is_pointer(no->type)) type = yes->type;
        else if (type_is_pointer(yes->type)) type = yes->type;
        else if (type_is_pointer(no->type)) type = no->type;
        else if (type_compatible(type_unqualified(yes->type),
                                 type_unqualified(no->type)) &&
                 (yes->type->kind == TYPE_STRUCT ||
                  yes->type->kind == TYPE_UNION)) {
            /* Two branches of the same record type are that type, the way two
               branches of the same arithmetic type are their common type. */
            type = yes->type;
        } else semantic_error(parser, 2093U, token, "incompatible conditional branches");
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

/* The type of the element an initializer list is at, or the aggregate's own
   type when the element's type cannot be told. Each element of a list
   initializes one element or member of the aggregate, so the conversion that
   element needs is decided by that element's type and not by the aggregate's.
   Passing the aggregate's type instead left every element unconverted: a
   string literal naming a pointer member kept the array type it was written
   with, and the lowering stores the first character of a string as the value of
   a character type, so `char *p` in a braced initializer held 'h' rather than
   the address of the literal. */
static Type *initializer_element_type(Type *type, size_t index)
{
    if (type == NULL) return NULL;
    if (type->kind == TYPE_ARRAY) return type->base;
    if (type->kind == TYPE_STRUCT || type->kind == TYPE_UNION) {
        Member *member = type->members;
        for (size_t at = 0U; member != NULL && at < index; ++at) member = member->next;
        return member == NULL ? NULL : member->type;
    }
    return type;
}

static AstNode *parse_initializer(Parser *parser, Type *type)
{
    const Token *token = peek(parser);
    if (accept(parser, "{")) {
        AstNode *initializer = node_new(parser, NODE_INITIALIZER, type, token);
        if (initializer != NULL) initializer->braced = true;
        AstNode *tail = NULL;
        if (!token_text(peek(parser), "}")) {
            for (size_t index = 0U;; ++index) {
                Type *element = initializer_element_type(type, index);
                AstNode *value = parse_initializer(parser,
                                                   element == NULL ? type : element);
                if (tail == NULL) initializer->a = value;
                else tail->next = value;
                tail = value;
                if (!accept(parser, ",")) break;
                /* A trailing comma before the closing brace ends the list. A
                   comma separates two initializers, so the one that follows
                   the last of them has nothing after it. */
                if (token_text(peek(parser), "}")) break;
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

/* A compound literal is an object the initializer builds, and the expression
   names that object. The object is unnamed, so the lowering gives it storage of
   its own and the node carries the initializer for it; nothing else in the
   unit can refer to it, which is what makes it different from a named object
   with the same type. */
static AstNode *parse_compound_literal(Parser *parser, Type *type,
                                       const Token *token)
{
    /* An array type written with no length is the one type the list is allowed
       to complete, so it is parsed before the type is judged. Any other
       incomplete type is refused before the list is read, so a diagnostic is
       reported once rather than once per element. */
    bool length_from_list = type != NULL && type->kind == TYPE_ARRAY &&
                            type->incomplete && type->base != NULL &&
                            type->base->size != 0U;
    if (type == NULL || (type->incomplete && !length_from_list)) {
        semantic_error(parser, 2081U, token, "compound literal has no complete type");
        while (peek(parser) != NULL && !token_text(peek(parser), "}")) (void)take(parser);
        (void)accept(parser, "}");
        return NULL;
    }
    if (parser->scope == parser->global_scope) {
        /* At file scope the object would outlive the run, and nothing in this
           revision places a computed object in the data section. */
        semantic_error(parser, 2081U, token,
                       "a compound literal at file scope is unsupported");
        while (peek(parser) != NULL && !token_text(peek(parser), "}")) (void)take(parser);
        (void)accept(parser, "}");
        return NULL;
    }
    AstNode *initializer = parse_initializer(parser, type);
    /* The list supplies the length of an array type written with none, exactly
       as it does for a declared object, so `(int[]){1, 2, 3}` names an object of
       three elements rather than one of no type at all. A list that turns out
       to supply no length at all leaves the type incomplete, and the node is
       not built. */
    if (length_from_list) complete_initializer_array(type, initializer);
    if (type->incomplete) {
        semantic_error(parser, 2081U, token, "compound literal has no complete type");
        return NULL;
    }
    AstNode *node = node_new(parser, NODE_COMPOUND_LITERAL, type, token);
    if (node != NULL) node->a = initializer;
    return node;
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
            if (symbol == NULL || scope_lookup_here(parser->scope, name) != NULL || !scope_add_symbol(parser->arena, parser->scope, symbol)) {
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
            /* A name may be redeclared in an inner block, where it hides the
               outer one, so only a name already in this scope is a duplicate.
               The lookup used to reach the parent scopes, which made every
               ordinary shadowing a redeclaration. */
            if (scope_lookup_here(parser->scope, name) != NULL) {
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
                if (spec.storage == STORAGE_STATIC) {
                    /* The back end reads a static object's initializer after the
                       function it appears in has been lowered and its syntax
                       tree released, so the initializer is copied into the
                       durable arena. */
                    symbol->initializer = copy_tree(parser, symbol->initializer);
                }
            }
            AstNode *declaration = node_new(parser, NODE_DECLARATION, type, peek(parser));
            if (declaration != NULL) { declaration->symbol = symbol; declaration->a = symbol->initializer; if (first == NULL) first = declaration; else tail->next = declaration; tail = declaration; }
        }
        if (!accept(parser, ",")) break;
    }
    (void)expect(parser, ";");
    return first;
}

/* A static assertion is checked while the unit is parsed and produces no code,
   so it needs no node: the constant expression is evaluated here and a false
   one is a diagnostic. The optional message is consumed so that the parse
   resumes after it instead of reporting a second failure on the string. */
static bool parse_static_assert(Parser *parser)
{
    const Token *token = take(parser);
    (void)expect(parser, "(");
    AstNode *node = parse_conditional(parser);
    uint64_t value = 0UL;
    if (node == NULL || !const_eval(node, &value)) {
        semantic_error(parser, 2040U, peek(parser),
                       "static assertion requires an integer constant expression");
    } else if (value == 0UL) {
        const Token *message = token_text(peek(parser), ",") ? peek_at(parser, 1U) : NULL;
        if (message != NULL && message->kind == TOKEN_STRING) {
            semantic_error(parser, 2041U, message, message->text);
        } else {
            semantic_error(parser, 2041U, token, "static assertion failed");
        }
    }
    while (peek(parser) != NULL && !token_text(peek(parser), ")")) (void)take(parser);
    (void)expect(parser, ")");
    (void)expect(parser, ";");
    return token != NULL;
}

static AstNode *parse_statement(Parser *parser)
{
    const Token *token = peek(parser);
    if (token == NULL) return NULL;
    if (token_is_keyword(token, "_Static_assert") || token_is_keyword(token, "static_assert")) {
        (void)parse_static_assert(parser);
        return node_new(parser, NODE_NULL_STATEMENT, NULL, token);
    }
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

/* define_symbol records a file-scope name. `tentative` is a definition with no
   initializer, which a unit may repeat; an initialized definition conflicts
   with any other and sets the recorded definition. */
static bool define_symbol(Parser *parser, const char *name, Type *type,
                          StorageClass storage, bool function, bool tentative,
                          bool initialized, Symbol **result)
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
    /* A tentative definition reserves a name for the unit without being the
       one the back end emits, so a unit may repeat it and may still give the
       same name an initialized definition later. An initialized definition is
       the one that conflicts with any other. */
    if (initialized) {
        if (symbol->defined && symbol->initialized) {
            semantic_error(parser, 2105U, peek(parser), "duplicate definition");
        }
        symbol->defined = true;
        symbol->initialized = true;
    } else if (tentative) {
        symbol->defined = true;
    }
    if (result != NULL) *result = symbol;
    return true;
}

static void parse_function_definition(Parser *parser, const DeclSpec *spec,
                                      Type *type, const char *name,
                                      Symbol *parameters, size_t parameter_count)
{
    Symbol *function = NULL;
    if (!define_symbol(parser, name, type, spec->storage, true, true, true,
                         &function)) return;
    (void)parameter_count;
    Scope *function_scope = scope_create(parser->arena, parser->global_scope);
    for (Symbol *parameter = parameters; parameter != NULL; parameter = parameter->next) {
        if (!scope_add_symbol(parser->arena, function_scope, parameter)) return;
    }
    Scope *old_scope = parser->scope;
    Symbol *old_function = parser->current_function;
    parser->scope = function_scope;
    parser->current_function = function;
    /* `__func__` names the function that is being parsed. It is declared as a
       static const char array at the top of the body, so the back end's own
       static-object path emits it and the body reads it as an ordinary
       object. Each function gets its own object, so the emitted name carries
       a per-function counter. */
    char self_name[32];
    (void)snprintf(self_name, sizeof(self_name), ".Lstatic.%u",
                   parser->static_local_count++);
    size_t length = strlen(name) + 1U;
    AstNode *text = node_new(parser, NODE_STRING,
                             type_array(parser->arena,
                                        type_basic(parser->arena, TYPE_CHAR),
                                        length, true), peek(parser));
    AstNode *self_decl = NULL;
    if (text != NULL) {
        text->text = cc64_xstrdup(name);
        text->text_length = length;
        Symbol *self = symbol_new(parser, "__func__",
                                  type_array(parser->arena,
                                             type_copy(parser->arena,
                                                       type_basic(parser->arena, TYPE_CHAR),
                                                       TYPE_QUAL_CONST),
                                             length, true),
                                  SYMBOL_VARIABLE);
        if (self != NULL) {
            self->storage = STORAGE_STATIC;
            self->linkage = LINKAGE_INTERNAL;
            self->defined = true;
            /* The initializer is copied into the durable arena: the back end
               reads it after this function's syntax tree is released. */
            self->initializer = copy_tree(parser, text);
            self_decl = node_new(parser, NODE_DECLARATION, self->type, peek(parser));
            if (self_decl != NULL) self_decl->symbol = self;
        }
    }
    /* The scope is bound first, under the name the body spells, and the
       symbol is renamed afterwards to the private name its data is emitted
       under. */
    if (self_decl != NULL && self_decl->symbol != NULL) {
        (void)scope_add_symbol(parser->arena, function_scope, self_decl->symbol);
        self_decl->symbol->name = cc64_xstrdup(self_name);
    }
    AstNode *body = parse_compound(parser);
    /* The declaration is spliced in front of the statements the body
       produced, so the back end sees it as an ordinary static object. */
    if (self_decl != NULL) {
        AstNode *statement = body == NULL ? NULL : body->a;
        while (statement != NULL && statement->next != NULL) statement = statement->next;
        if (statement != NULL) statement->next = self_decl;
        else if (body != NULL) body->a = self_decl;
    }
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
            /* A unit may repeat a typedef, which is how a header that names a
               type and a source that includes it both declare it. The repeat
               is accepted only when it names the same type, because a second
               typedef of one name for a different type is a different
               declaration that would silently replace the first. */
            Symbol *previous = scope_lookup(parser->global_scope, name);
            if (previous != NULL && previous->class != SYMBOL_TYPEDEF) {
                semantic_error(parser, 2107U, peek(parser), "duplicate typedef");
                return;
            }
            if (previous != NULL && !type_compatible(previous->type, type)) {
                semantic_error(parser, 2107U, peek(parser), "duplicate typedef");
                return;
            }
            if (previous == NULL) {
                Symbol *symbol = symbol_new(parser, name, type, SYMBOL_TYPEDEF);
                if (symbol == NULL ||
                    !scope_add_symbol(parser->arena, parser->global_scope, symbol)) {
                    semantic_error(parser, 2107U, peek(parser), "duplicate typedef");
                }
            }
        } else if (type_is_function(type) && token_text(peek(parser), "{")) {
            parse_function_definition(parser, spec, type, name, parameters, parameter_count);
            return;
        } else {
            Symbol *symbol = NULL;
            bool is_function = type_is_function(type);
            /* A declaration that is not extern contributes a definition, but
               a definition with no initializer is a tentative one and a unit
               may hold several of them, so only an initialized definition is
               recorded as the one that conflicts with another. The duplicate
               check used to see only the tentative form, so two initialized
               definitions of one name were both accepted. */
            bool has_initializer = token_text(peek(parser), "=");
            bool tentative = !is_function && !has_initializer &&
                             spec->storage != STORAGE_EXTERN;
            if (!define_symbol(parser, name, type, spec->storage, is_function,
                               tentative, has_initializer, &symbol)) return;
            if (has_initializer && accept(parser, "=")) {
                symbol->initializer = parse_initializer(parser, type);
                complete_initializer_array(type, symbol->initializer);
                /* The back end emits an object's data from its initializer
                   after the whole unit has been parsed and lowered, by which
                   time the node arena is gone, so the initializer is copied
                   into the durable arena. */
                symbol->initializer = copy_tree(parser, symbol->initializer);
                AstNode *declaration = node_new(parser, NODE_DECLARATION, type, peek(parser));
                if (declaration != NULL) { declaration->symbol = symbol; declaration->a = symbol->initializer; (void)append_declaration(parser->unit, declaration); }
            } else if (tentative) {
                AstNode *declaration = node_new(parser, NODE_DECLARATION, type, peek(parser));
                if (declaration != NULL) { declaration->symbol = symbol; (void)append_declaration(parser->unit, declaration); }
            }
        }
        if (!accept(parser, ",")) break;
    }
    (void)expect(parser, ";");
}

Parser *parser_create(Arena *arena, Arena *node_arena, TokenList *tokens,
                      DiagnosticSink *diagnostics, TranslationUnit *unit)
{
    if (arena == NULL || node_arena == NULL || tokens == NULL) return NULL;
    memset(unit, 0, sizeof(*unit));
    /* Newline and end tokens are dropped by compacting the list in place: a
       second array of every token would cost more than the tokens themselves
       for a large unit, and the list belongs to the caller. */
    Token *items = tokens->items;
    size_t count = tokens->count;
    size_t kept = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if (items[i].kind == TOKEN_NEWLINE || items[i].kind == TOKEN_EOF) continue;
        if (kept != i) items[kept] = items[i];
        ++kept;
    }
    /* The list was sized from the source text and the compaction dropped both
       ends of every line, so the array is now longer than the stream it holds.
       Handing the tail back matters on the target, where the array is a large
       part of what a unit has to fit in, and a smaller request is served from
       the same block. */
    if (kept != count) {
        Token *shrunk = realloc(items, kept * sizeof(*items));
        if (shrunk != NULL) items = shrunk;
    }
    Parser *parser = arena_alloc(arena, sizeof(*parser));
    if (parser == NULL) return NULL;
    memset(parser, 0, sizeof(*parser));
    parser->arena = arena;
    parser->node_arena = node_arena;
    parser->diagnostics = diagnostics;
    parser->tokens = items;
    parser->count = kept;
    /* The parser owns the array from here; the caller's list is emptied so its
       own release is a no-op. */
    tokens->items = NULL;
    tokens->count = 0U;
    tokens->capacity = 0U;
    parser->unit = unit;
    parser->global_scope = scope_create(arena, NULL);
    parser->scope = parser->global_scope;
    return parser->global_scope == NULL ? NULL : parser;
}

void parser_destroy(Parser *parser)
{
    /* The parser itself and its scopes live in the durable arena; the token
       array it took ownership of does not. */
    if (parser == NULL) return;
    free(parser->tokens);
    parser->tokens = NULL;
    parser->count = 0U;
}

/* One declaration per call, so the caller can lower it and release its syntax
   tree before the next one is parsed. A declaration can produce several nodes,
   as in `int a, b;`, so the call reports the half-open range it appended.
   Error recovery stays inside the call: a declaration that cannot be parsed is
   skipped up to its terminator and the next call continues after it. */
bool parser_next(Parser *parser, size_t *first, size_t *last)
{
    if (first != NULL) *first = 0U;
    if (last != NULL) *last = 0U;
    if (parser == NULL || parser->failed) return false;
    while (parser->position < parser->count && !parser->failed) {
        if (accept(parser, ";")) continue;
        /* A file-scope static assertion produces no declaration, so it is
           consumed here and the loop continues with the next one. */
        if (token_is_keyword(peek(parser), "_Static_assert") ||
            token_is_keyword(peek(parser), "static_assert")) {
            (void)parse_static_assert(parser);
            continue;
        }
        DeclSpec spec;
        /* The flag records whether the declaration being parsed now is
           wrong, so it is cleared before each one and does not carry an
           earlier declaration's diagnostic into the next. */
        parser->errored = false;
        if (!parse_decl_specs(parser, &spec)) {
            while (peek(parser) != NULL && !token_text(peek(parser), ";"))
                (void)take(parser);
            (void)accept(parser, ";");
            continue;
        }
        size_t before = parser->unit->count;
        parse_global_declaration(parser, &spec);
        if (parser->failed) return false;
        if (parser->errored) {
            /* A declaration that was rejected is skipped up to its terminator
               so the next one is parsed from a known position. The nodes it
               appended are dropped, because a declaration that failed to parse
               cannot be lowered. The scan always consumes at least one token,
               so a declaration whose terminator is missing cannot hold the
               parse at the same position for ever. */
            for (size_t i = before; i < parser->unit->count; ++i) {
                parser->unit->declarations[i] = NULL;
            }
            parser->unit->count = before;
            size_t start = parser->position;
            while (peek(parser) != NULL && parser->position > start) {
                bool terminator = token_text(peek(parser), ";") ||
                                  token_text(peek(parser), "}");
                (void)take(parser);
                if (terminator) break;
            }
            if (peek(parser) != NULL) (void)take(parser);
            continue;
        }
        if (parser->unit->count == before) continue;
        if (first != NULL) *first = before;
        if (last != NULL) *last = parser->unit->count;
        return true;
    }
    return false;
}

bool parser_finish(Parser *parser)
{
    if (parser == NULL) return false;
    if (parser->unit->globals == NULL && parser->global_scope != NULL &&
        parser->global_scope->bindings != NULL) {
        parser->unit->globals = parser->global_scope->bindings->symbol;
    }
    parser->unit->global_scope = parser->global_scope;
    /* The unit is accepted when the stream was parsed to its end and no
       diagnostic was reported at all, so a declaration that was skipped after
       an error is still a rejected unit. */
    return !parser->failed &&
           (parser->diagnostics == NULL || parser->diagnostics->count == 0U);
}

bool parse_tokens(Arena *arena, TokenList *tokens, DiagnosticSink *diagnostics,
                  TranslationUnit *unit)
{
    /* One arena for both, which is what a caller that wants the whole syntax
       tree expects: nothing is released before the tree is finished. */
    Parser *parser = parser_create(arena, arena, tokens, diagnostics, unit);
    if (parser == NULL) return false;
    size_t first = 0U;
    size_t last = 0U;
    while (parser_next(parser, &first, &last)) continue;
    bool good = parser_finish(parser);
    parser_destroy(parser);
    return good;
}
