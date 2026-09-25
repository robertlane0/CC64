#ifndef CC64_SEMANTIC_H
#define CC64_SEMANTIC_H

#include "cc64.h"
#include "frontend/frontend.h"

typedef enum TypeKind {
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_CHAR,
    TYPE_SIGNED_CHAR,
    TYPE_UNSIGNED_CHAR,
    TYPE_SHORT,
    TYPE_UNSIGNED_SHORT,
    TYPE_INT,
    TYPE_UNSIGNED_INT,
    TYPE_LONG,
    TYPE_UNSIGNED_LONG,
    TYPE_LONG_LONG,
    TYPE_UNSIGNED_LONG_LONG,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_POINTER,
    TYPE_ARRAY,
    TYPE_FUNCTION,
    TYPE_STRUCT,
    TYPE_UNION,
    TYPE_ENUM,
    TYPE_VOID_EXPR
} TypeKind;

typedef unsigned TypeQualifiers;
#define TYPE_QUAL_CONST     0x0001U
#define TYPE_QUAL_VOLATILE  0x0002U
#define TYPE_QUAL_RESTRICT  0x0004U

typedef struct Type Type;
typedef struct Symbol Symbol;
typedef struct Scope Scope;
typedef struct AstNode AstNode;

typedef struct Member {
    char *name;
    Type *type;
    size_t offset;
    struct Member *next;
} Member;

struct Type {
    TypeKind kind;
    TypeQualifiers qualifiers;
    size_t size;
    size_t alignment;
    Type *base;
    size_t array_count;
    bool incomplete;
    char *tag;
    Member *members;
    Type *return_type;
    Symbol *parameters;
    size_t parameter_count;
    bool variadic;
    Type *next;
};

typedef enum SymbolClass {
    SYMBOL_VARIABLE,
    SYMBOL_FUNCTION,
    SYMBOL_TYPEDEF,
    SYMBOL_ENUM_CONSTANT
} SymbolClass;

typedef enum StorageClass {
    STORAGE_NONE,
    STORAGE_EXTERN,
    STORAGE_STATIC,
    STORAGE_TYPEDEF,
    STORAGE_AUTO,
    STORAGE_REGISTER
} StorageClass;

typedef enum Linkage {
    LINKAGE_NONE,
    LINKAGE_INTERNAL,
    LINKAGE_EXTERNAL
} Linkage;

struct Symbol {
    char *name;
    Type *type;
    SymbolClass class;
    StorageClass storage;
    Linkage linkage;
    bool defined;
    bool is_parameter;
    size_t offset;
    bool has_frame_offset;
    int64_t enum_value;
    AstNode *initializer;
    Symbol *next;
    Symbol *next_scope;
};

typedef struct Binding {
    char *name;
    bool tag;
    Symbol *symbol;
    Type *type;
    struct Binding *next;
} Binding;

struct Scope {
    Scope *parent;
    Binding *bindings;
};

typedef struct SourceLocation {
    const Source *source;
    size_t line;
    size_t column;
} SourceLocation;

typedef enum NodeKind {
    NODE_INTEGER,
    NODE_FLOAT,
    NODE_STRING,
    NODE_IDENTIFIER,
    NODE_CALL,
    NODE_UNARY,
    NODE_BINARY,
    NODE_ASSIGNMENT,
    NODE_CONDITIONAL,
    NODE_COMMA,
    NODE_CAST,
    NODE_SIZEOF,
    NODE_ADDRESS,
    NODE_DEREFERENCE,
    NODE_INDEX,
    NODE_MEMBER,
    NODE_INITIALIZER,
    NODE_COMPOUND,
    NODE_EXPRESSION_STATEMENT,
    NODE_IF,
    NODE_WHILE,
    NODE_DO,
    NODE_FOR,
    NODE_SWITCH,
    NODE_CASE,
    NODE_DEFAULT,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_RETURN,
    NODE_GOTO,
    NODE_LABEL,
    NODE_NULL_STATEMENT,
    NODE_DECLARATION,
    NODE_FUNCTION_DEFINITION
} NodeKind;

typedef enum UnaryOperator {
    UNARY_PLUS,
    UNARY_MINUS,
    UNARY_LOGICAL_NOT,
    UNARY_BITWISE_NOT,
    UNARY_PRE_INCREMENT,
    UNARY_PRE_DECREMENT,
    UNARY_POST_INCREMENT,
    UNARY_POST_DECREMENT
} UnaryOperator;

typedef enum BinaryOperator {
    BINARY_ADD,
    BINARY_SUBTRACT,
    BINARY_MULTIPLY,
    BINARY_DIVIDE,
    BINARY_REMAINDER,
    BINARY_LEFT_SHIFT,
    BINARY_RIGHT_SHIFT,
    BINARY_LESS,
    BINARY_LESS_EQUAL,
    BINARY_GREATER,
    BINARY_GREATER_EQUAL,
    BINARY_EQUAL,
    BINARY_NOT_EQUAL,
    BINARY_BITWISE_AND,
    BINARY_BITWISE_XOR,
    BINARY_BITWISE_OR,
    BINARY_LOGICAL_AND,
    BINARY_LOGICAL_OR
} BinaryOperator;

struct AstNode {
    NodeKind kind;
    Type *type;
    SourceLocation location;
    AstNode *a;
    AstNode *b;
    AstNode *c;
    AstNode *d;
    AstNode *next;
    Symbol *symbol;
    Symbol *field;
    UnaryOperator unary;
    BinaryOperator binary;
    bool compound_assignment;
    int64_t integer;
    uint64_t unsigned_integer;
    double floating;
    char *text;
    size_t text_length;
    Type *type_operand;
    Symbol *literal_symbol;
    unsigned label_id;
};

typedef struct TranslationUnit {
    AstNode **declarations;
    size_t count;
    size_t capacity;
    Symbol *globals;
    bool has_main;
} TranslationUnit;

typedef struct Parser Parser;

Type *type_basic(Arena *arena, TypeKind kind);
Type *type_pointer(Arena *arena, Type *base, TypeQualifiers qualifiers);
Type *type_array(Arena *arena, Type *base, size_t count, bool known);
Type *type_function(Arena *arena, Type *return_type, Symbol *parameters,
                    size_t parameter_count, bool variadic);
Type *type_copy(Arena *arena, Type *source, TypeQualifiers qualifiers);
bool type_is_integer(const Type *type);
bool type_is_arithmetic(const Type *type);
bool type_is_scalar(const Type *type);
bool type_is_pointer(const Type *type);
bool type_is_void(const Type *type);
bool type_is_signed(const Type *type);
bool type_is_complete(const Type *type);
bool type_compatible(const Type *left, const Type *right);
bool type_has_const(const Type *type);
Type *type_unqualified(Type *type);
Type *type_integer_promote(Arena *arena, Type *type);
Type *type_usual_arithmetic(Arena *arena, Type *left, Type *right);
size_t type_size(const Type *type);
size_t type_alignment(const Type *type);
const char *type_kind_name(TypeKind kind);

Scope *scope_create(Arena *arena, Scope *parent);
Symbol *scope_lookup(Scope *scope, const char *name);
Binding *scope_add_symbol(Arena *arena, Scope *scope, Symbol *symbol);
Binding *scope_add_tag(Arena *arena, Scope *scope, const char *name, Type *type);
Type *scope_lookup_tag(Scope *scope, const char *name);

bool parse_tokens(Arena *arena, TokenList *tokens, DiagnosticSink *diagnostics,
                  TranslationUnit *unit);
void translation_unit_free(TranslationUnit *unit);
const char *ast_node_kind_name(NodeKind kind);

#endif
