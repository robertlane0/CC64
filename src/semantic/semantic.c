#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Type *type_alloc(Arena *arena, TypeKind kind, size_t size, size_t alignment)
{
    Type *type = arena_alloc(arena, sizeof(*type));
    if (type == NULL) {
        return NULL;
    }
    type->kind = kind;
    type->qualifiers = 0U;
    type->size = size;
    type->alignment = alignment;
    type->base = NULL;
    type->array_count = 0U;
    type->incomplete = false;
    type->tag = NULL;
    type->members = NULL;
    type->return_type = NULL;
    type->parameters = NULL;
    type->parameter_count = 0U;
    type->variadic = false;
    type->next = NULL;
    return type;
}

Type *type_basic(Arena *arena, TypeKind kind)
{
    size_t size = 0U;
    size_t alignment = 1U;
    switch (kind) {
    case TYPE_VOID: size = 1U; break;
    case TYPE_BOOL:
    case TYPE_CHAR:
    case TYPE_SIGNED_CHAR:
    case TYPE_UNSIGNED_CHAR: size = 1U; break;
    case TYPE_SHORT:
    case TYPE_UNSIGNED_SHORT: size = 2U; alignment = 2U; break;
    case TYPE_INT:
    case TYPE_UNSIGNED_INT:
    case TYPE_ENUM: size = 4U; alignment = 4U; break;
    case TYPE_STRUCT:
    case TYPE_UNION: size = 0U; alignment = 1U; break;
    case TYPE_LONG:
    case TYPE_UNSIGNED_LONG:
    case TYPE_LONG_LONG:
    case TYPE_UNSIGNED_LONG_LONG: size = 8U; alignment = 8U; break;
    case TYPE_FLOAT: size = 4U; alignment = 4U; break;
    case TYPE_DOUBLE: size = 8U; alignment = 8U; break;
    case TYPE_VOID_EXPR: size = 0U; break;
    default: return NULL;
    }
    Type *type = type_alloc(arena, kind, size, alignment);
    if (type != NULL && (kind == TYPE_STRUCT || kind == TYPE_UNION)) {
        type->incomplete = true;
    }
    return type;
}

Type *type_pointer(Arena *arena, Type *base, TypeQualifiers qualifiers)
{
    if (base == NULL) {
        return NULL;
    }
    Type *type = type_alloc(arena, TYPE_POINTER, 8U, 8U);
    if (type == NULL) {
        return NULL;
    }
    type->base = base;
    type->qualifiers = qualifiers;
    return type;
}

Type *type_array(Arena *arena, Type *base, size_t count, bool known)
{
    if (base == NULL || !type_is_complete(base)) {
        return NULL;
    }
    if (known && count > SIZE_MAX / base->size) {
        return NULL;
    }
    Type *type = type_alloc(arena, TYPE_ARRAY,
                            known ? count * base->size : 0U,
                            base->alignment);
    if (type == NULL) {
        return NULL;
    }
    type->base = base;
    type->array_count = count;
    type->incomplete = !known;
    return type;
}

Type *type_function(Arena *arena, Type *return_type, Symbol *parameters,
                    size_t parameter_count, bool variadic)
{
    if (return_type == NULL) {
        return NULL;
    }
    Type *type = type_alloc(arena, TYPE_FUNCTION, 0U, 1U);
    if (type == NULL) {
        return NULL;
    }
    type->return_type = return_type;
    type->parameters = parameters;
    type->parameter_count = parameter_count;
    type->variadic = variadic;
    return type;
}

Type *type_copy(Arena *arena, Type *source, TypeQualifiers qualifiers)
{
    if (source == NULL) {
        return NULL;
    }
    Type *copy = arena_alloc(arena, sizeof(*copy));
    if (copy == NULL) {
        return NULL;
    }
    *copy = *source;
    copy->qualifiers = qualifiers;
    copy->next = NULL;
    return copy;
}

bool type_is_integer(const Type *type)
{
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
    case TYPE_BOOL:
    case TYPE_CHAR:
    case TYPE_SIGNED_CHAR:
    case TYPE_UNSIGNED_CHAR:
    case TYPE_SHORT:
    case TYPE_UNSIGNED_SHORT:
    case TYPE_INT:
    case TYPE_UNSIGNED_INT:
    case TYPE_LONG:
    case TYPE_UNSIGNED_LONG:
    case TYPE_LONG_LONG:
    case TYPE_UNSIGNED_LONG_LONG:
    case TYPE_ENUM: return true;
    default: return false;
    }
}

bool type_is_arithmetic(const Type *type)
{
    return type_is_integer(type) || (type != NULL &&
           (type->kind == TYPE_FLOAT || type->kind == TYPE_DOUBLE));
}

bool type_is_scalar(const Type *type)
{
    return type_is_arithmetic(type) || type_is_pointer(type);
}

bool type_is_pointer(const Type *type)
{
    return type != NULL && type->kind == TYPE_POINTER;
}

bool type_is_void(const Type *type)
{
    return type != NULL && (type->kind == TYPE_VOID || type->kind == TYPE_VOID_EXPR);
}

bool type_is_signed(const Type *type)
{
    if (!type_is_integer(type)) {
        return false;
    }
    switch (type->kind) {
    case TYPE_BOOL:
    case TYPE_UNSIGNED_CHAR:
    case TYPE_UNSIGNED_SHORT:
    case TYPE_UNSIGNED_INT:
    case TYPE_UNSIGNED_LONG:
    case TYPE_UNSIGNED_LONG_LONG: return false;
    default: return true;
    }
}

bool type_is_complete(const Type *type)
{
    if (type == NULL) {
        return false;
    }
    if (type->kind == TYPE_VOID) {
        return false;
    }
    if (type->kind == TYPE_STRUCT || type->kind == TYPE_UNION) {
        return !type->incomplete;
    }
    if (type->kind == TYPE_ARRAY) {
        return !type->incomplete && type_is_complete(type->base);
    }
    return true;
}

bool type_has_const(const Type *type)
{
    if (type == NULL) return false;
    if ((type->qualifiers & TYPE_QUAL_CONST) != 0U) return true;
    if (type->kind == TYPE_ARRAY || type->kind == TYPE_POINTER) {
        return type_has_const(type->base);
    }
    return false;
}

Type *type_unqualified(Type *type)
{
    if (type == NULL || type->qualifiers == 0U) {
        return type;
    }
    Type *copy = cc64_xmalloc(sizeof(*copy));
    *copy = *type;
    copy->qualifiers = 0U;
    copy->next = NULL;
    return copy;
}

bool type_compatible(const Type *left, const Type *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    if (left->kind == TYPE_ENUM || right->kind == TYPE_ENUM) {
        TypeKind left_kind = left->kind == TYPE_ENUM ? TYPE_INT : left->kind;
        TypeKind right_kind = right->kind == TYPE_ENUM ? TYPE_INT : right->kind;
        return left_kind == right_kind;
    }
    if (left->kind != right->kind) {
        return false;
    }
    switch (left->kind) {
    case TYPE_POINTER:
        return type_compatible(left->base, right->base);
    case TYPE_ARRAY:
        return type_compatible(left->base, right->base) &&
               (left->incomplete || right->incomplete ||
                left->array_count == right->array_count);
    case TYPE_FUNCTION:
        return type_compatible(left->return_type, right->return_type) &&
               left->parameter_count == right->parameter_count &&
               left->variadic == right->variadic;
    case TYPE_STRUCT:
    case TYPE_UNION:
        return left == right || (left->tag != NULL && right->tag != NULL &&
                                  strcmp(left->tag, right->tag) == 0);
    default: return true;
    }
}

static unsigned integer_rank(const Type *type)
{
    switch (type->kind) {
    case TYPE_BOOL:
    case TYPE_CHAR:
    case TYPE_SIGNED_CHAR:
    case TYPE_UNSIGNED_CHAR: return 1U;
    case TYPE_SHORT:
    case TYPE_UNSIGNED_SHORT: return 2U;
    case TYPE_INT:
    case TYPE_UNSIGNED_INT:
    case TYPE_ENUM: return 3U;
    case TYPE_LONG:
    case TYPE_UNSIGNED_LONG: return 4U;
    case TYPE_LONG_LONG:
    case TYPE_UNSIGNED_LONG_LONG: return 5U;
    default: return 0U;
    }
}

Type *type_integer_promote(Arena *arena, Type *type)
{
    if (!type_is_integer(type)) {
        return type;
    }
    if (integer_rank(type) < 3U) {
        return type_basic(arena, TYPE_INT);
    }
    if (type->kind == TYPE_ENUM) {
        return type_basic(arena, TYPE_INT);
    }
    return type;
}

Type *type_usual_arithmetic(Arena *arena, Type *left, Type *right)
{
    if (left == NULL || right == NULL || !type_is_arithmetic(left) ||
        !type_is_arithmetic(right)) {
        return NULL;
    }
    if (left->kind == TYPE_DOUBLE || right->kind == TYPE_DOUBLE) {
        return type_basic(arena, TYPE_DOUBLE);
    }
    if (left->kind == TYPE_FLOAT || right->kind == TYPE_FLOAT) {
        return type_basic(arena, TYPE_FLOAT);
    }
    left = type_integer_promote(arena, left);
    right = type_integer_promote(arena, right);
    if (type_compatible(left, right)) {
        return left;
    }
    bool left_unsigned = !type_is_signed(left);
    bool right_unsigned = !type_is_signed(right);
    unsigned left_rank = integer_rank(left);
    unsigned right_rank = integer_rank(right);
    if (left_rank == right_rank) {
        return left_unsigned ? left : right;
    }
    if (left_rank > right_rank) {
        if (!left_unsigned && right_unsigned && left->size <= right->size) {
            return right;
        }
        return left;
    }
    if (right_unsigned && !left_unsigned && right->size <= left->size) {
        return left;
    }
    return right;
}

size_t type_size(const Type *type)
{
    return type == NULL ? 0U : type->size;
}

size_t type_alignment(const Type *type)
{
    return type == NULL ? 1U : type->alignment;
}

const char *type_kind_name(TypeKind kind)
{
    switch (kind) {
    case TYPE_VOID: return "void";
    case TYPE_BOOL: return "_Bool";
    case TYPE_CHAR: return "char";
    case TYPE_SIGNED_CHAR: return "signed char";
    case TYPE_UNSIGNED_CHAR: return "unsigned char";
    case TYPE_SHORT: return "short";
    case TYPE_UNSIGNED_SHORT: return "unsigned short";
    case TYPE_INT: return "int";
    case TYPE_UNSIGNED_INT: return "unsigned int";
    case TYPE_LONG: return "long";
    case TYPE_UNSIGNED_LONG: return "unsigned long";
    case TYPE_LONG_LONG: return "long long";
    case TYPE_UNSIGNED_LONG_LONG: return "unsigned long long";
    case TYPE_FLOAT: return "float";
    case TYPE_DOUBLE: return "double";
    case TYPE_POINTER: return "pointer";
    case TYPE_ARRAY: return "array";
    case TYPE_FUNCTION: return "function";
    case TYPE_STRUCT: return "struct";
    case TYPE_UNION: return "union";
    case TYPE_ENUM: return "enum";
    case TYPE_VOID_EXPR: return "void expression";
    }
    return "unknown";
}

Scope *scope_create(Arena *arena, Scope *parent)
{
    Scope *scope = arena_alloc(arena, sizeof(*scope));
    if (scope == NULL) {
        return NULL;
    }
    scope->parent = parent;
    scope->bindings = NULL;
    return scope;
}

Symbol *scope_lookup(Scope *scope, const char *name)
{
    for (; scope != NULL; scope = scope->parent) {
        for (Binding *binding = scope->bindings; binding != NULL;
             binding = binding->next) {
            if (!binding->tag && binding->symbol != NULL &&
                strcmp(binding->name, name) == 0) {
                return binding->symbol;
            }
        }
    }
    return NULL;
}

Binding *scope_add_symbol(Arena *arena, Scope *scope, Symbol *symbol)
{
    Binding *binding = arena_alloc(arena, sizeof(*binding));
    if (binding == NULL) {
        return NULL;
    }
    binding->name = symbol->name;
    binding->tag = false;
    binding->symbol = symbol;
    binding->type = symbol->type;
    binding->next = scope->bindings;
    scope->bindings = binding;
    return binding;
}

Binding *scope_add_tag(Arena *arena, Scope *scope, const char *name, Type *type)
{
    Binding *binding = arena_alloc(arena, sizeof(*binding));
    if (binding == NULL) {
        return NULL;
    }
    binding->name = cc64_xstrdup(name);
    binding->tag = true;
    binding->symbol = NULL;
    binding->type = type;
    binding->next = scope->bindings;
    scope->bindings = binding;
    return binding;
}

Type *scope_lookup_tag(Scope *scope, const char *name)
{
    for (; scope != NULL; scope = scope->parent) {
        for (Binding *binding = scope->bindings; binding != NULL;
             binding = binding->next) {
            if (binding->tag && strcmp(binding->name, name) == 0) {
                return binding->type;
            }
        }
    }
    return NULL;
}

const char *ast_node_kind_name(NodeKind kind)
{
    switch (kind) {
    case NODE_INTEGER: return "integer";
    case NODE_FLOAT: return "float";
    case NODE_STRING: return "string";
    case NODE_IDENTIFIER: return "identifier";
    case NODE_CALL: return "call";
    case NODE_UNARY: return "unary";
    case NODE_BINARY: return "binary";
    case NODE_ASSIGNMENT: return "assignment";
    case NODE_CONDITIONAL: return "conditional";
    case NODE_COMMA: return "comma";
    case NODE_CAST: return "cast";
    case NODE_SIZEOF: return "sizeof";
    case NODE_ADDRESS: return "address";
    case NODE_DEREFERENCE: return "dereference";
    case NODE_INDEX: return "index";
    case NODE_MEMBER: return "member";
    case NODE_INITIALIZER: return "initializer";
    case NODE_COMPOUND: return "compound";
    case NODE_EXPRESSION_STATEMENT: return "expression-statement";
    case NODE_IF: return "if";
    case NODE_WHILE: return "while";
    case NODE_DO: return "do";
    case NODE_FOR: return "for";
    case NODE_SWITCH: return "switch";
    case NODE_CASE: return "case";
    case NODE_DEFAULT: return "default";
    case NODE_BREAK: return "break";
    case NODE_CONTINUE: return "continue";
    case NODE_RETURN: return "return";
    case NODE_GOTO: return "goto";
    case NODE_LABEL: return "label";
    case NODE_NULL_STATEMENT: return "null";
    case NODE_DECLARATION: return "declaration";
    case NODE_FUNCTION_DEFINITION: return "function-definition";
    }
    return "unknown";
}

void translation_unit_free(TranslationUnit *unit)
{
    if (unit == NULL) {
        return;
    }
    free(unit->declarations);
    unit->declarations = NULL;
    unit->count = 0U;
    unit->capacity = 0U;
}
