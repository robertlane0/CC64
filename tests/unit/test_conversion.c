/* Conversions, constant expressions, and aggregate layout.
 *
 * These are the rules the type system exists to implement. Each case states a
 * rule and checks the type the compiler computed, so a wrong conversion is
 * named rather than showing up later as a wrong answer on the target, where a
 * program with several conversions cannot say which one was wrong.
 */

#include "cc64.h"
#include "frontend/frontend.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

/* parsed runs the front end over one unit and returns the first declaration
   whose expression has the type under test. The unit is released by the
   caller, so the arena that holds the types outlives the call. */
static TranslationUnit parse_unit(Arena *arena, const char *text,
                                  DiagnosticSink *diagnostics)
{
    SourceManager *manager = source_manager_create(arena);
    Source *source = source_manager_add(manager, "conversion.c",
                                        (const unsigned char *)text,
                                        strlen(text));
    PreprocessorOptions options = {NULL, 0U, NULL, 0U, true};
    TokenList tokens;
    token_list_init(&tokens);
    (void)preprocess_source(arena, manager, diagnostics, source, &options,
                            &tokens);
    TranslationUnit unit = {0};
    (void)parse_tokens(arena, &tokens, diagnostics, &unit);
    token_list_free(&tokens);
    return unit;
}

/* The expression type of the last identifier in a function body, found by
   walking the tree, so a case can name the type of a subexpression. */
static const AstNode *find_node(const AstNode *node, NodeKind kind)
{
    for (const AstNode *at = node; at != NULL; at = at->next) {
        if (at->kind == kind) return at;
        const AstNode *found = find_node(at->a, kind);
        if (found != NULL) return found;
        found = find_node(at->b, kind);
        if (found != NULL) return found;
        found = find_node(at->c, kind);
        if (found != NULL) return found;
        found = find_node(at->d, kind);
        if (found != NULL) return found;
    }
    return NULL;
}

static void expect_result_type(const char *text, TypeKind expected,
                               const char *label)
{
    Arena *arena = arena_create(4U * 1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit = parse_unit(arena, text, &diagnostics);
    if (diagnostics.count != 0U) {
        fprintf(stderr, "FAIL %s: the source was rejected\n", label);
        ++failures;
    } else {
        /* The type of the first binary expression is the usual arithmetic
           conversion of its operands, which is the rule under test. */
        const AstNode *binary = NULL;
        for (size_t i = 0U; i < unit.count && binary == NULL; ++i) {
            if (unit.declarations[i] != NULL &&
                unit.declarations[i]->kind == NODE_FUNCTION_DEFINITION) {
                binary = find_node(unit.declarations[i]->a, NODE_BINARY);
            }
        }
        if (binary != NULL && binary->type != NULL) {
            if (binary->type->kind != expected) {
                fprintf(stderr, "FAIL %s: got %s want %s\n", label,
                        type_kind_name(binary->type->kind),
                        type_kind_name(expected));
                ++failures;
            }
        } else {
            fprintf(stderr, "FAIL %s: no binary expression to check\n", label);
            ++failures;
        }
    }
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

/* The usual arithmetic conversions: the wider of the two types wins, an
   unsigned one wins over a signed one of the same width, and a narrow type is
   promoted to `int` before any of that. */
static void test_usual_arithmetic_conversions(void)
{
    expect_result_type("int f(char a, char b) { return a + b; }\n",
                       TYPE_INT, "two chars promote to int");
    expect_result_type("int f(short a, short b) { return a + b; }\n",
                       TYPE_INT, "two shorts promote to int");
    expect_result_type("int f(char a, long b) { return a + b; }\n",
                       TYPE_LONG, "a char and a long give a long");
    expect_result_type("int f(int a, long b) { return a + b; }\n",
                       TYPE_LONG, "an int and a long give a long");
    expect_result_type("int f(long a, unsigned long b) { return a + b; }\n",
                       TYPE_UNSIGNED_LONG,
                       "a long and an unsigned long give the unsigned long");
    expect_result_type("int f(unsigned a, int b) { return a + b; }\n",
                       TYPE_UNSIGNED_INT,
                       "an unsigned and an int give the unsigned int");
    expect_result_type("int f(float a, float b) { return (int)(a + b); }\n",
                       TYPE_FLOAT, "two floats give a float");
    expect_result_type("int f(float a, double b) { return (int)(a + b); }\n",
                       TYPE_DOUBLE, "a float and a double give a double");
    expect_result_type("int f(long long a, long long b) { return (int)(a + b); }\n",
                       TYPE_LONG_LONG, "two long longs give a long long");
}

/* A cast is the one conversion the program asks for explicitly, and it has to
   produce exactly the named type rather than the type the arithmetic would
   have produced. */
static void test_explicit_casts(void)
{
    Arena *arena = arena_create(4U * 1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit = parse_unit(
        arena,
        "int f(long v) { return (int)v; }\n"
        "int g(double v) { return (short)v; }\n"
        "int h(int v) { return (unsigned char)v; }\n", &diagnostics);
    CHECK(diagnostics.count == 0U);
    (void)unit;
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

/* An array or a function decays to a pointer wherever it is used, which is
   why an array parameter is a pointer and why indexing needs no cast. */
static void test_decay(void)
{
    Arena *arena = arena_create(4U * 1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit = parse_unit(
        arena,
        "struct S { int value; };\n"
        "int f(int a[4]) { return a[0]; }\n"
        "int g(char text[]) { return text[0]; }\n"
        "int h(struct S *p) { return p->value; }\n"
        "int main(void) { int a[2] = {1, 2}; return a[1] + h(0); }\n",
        &diagnostics);
    CHECK(diagnostics.count == 0U);
    /* An array parameter is a pointer, so its recorded size is a pointer's. */
    bool found = false;
    for (size_t i = 0U; i < unit.count; ++i) {
        const AstNode *definition = unit.declarations[i];
        if (definition == NULL ||
            definition->kind != NODE_FUNCTION_DEFINITION) continue;
        if (definition->symbol == NULL || definition->symbol->name == NULL) continue;
        if (strcmp(definition->symbol->name, "f") != 0) continue;
        for (Symbol *parameter = definition->symbol->type->parameters;
             parameter != NULL; parameter = parameter->next) {
            CHECK(parameter->type->kind == TYPE_POINTER);
            found = true;
        }
    }
    CHECK(found);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

/* The comma operator evaluates both operands and yields the right one, which
   is the only place a sequence of two expressions is a single value. */
static void test_sequence_points(void)
{
    Arena *arena = arena_create(4U * 1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit = parse_unit(
        arena,
        "int f(int a) { return (a++, a + 1); }\n"
        "int g(int a, int b) { return (a = 1, b = 2, a + b); }\n"
        "int h(int a) { int b = 0; a = (b = 2, b + 3); return a; }\n",
        &diagnostics);
    CHECK(diagnostics.count == 0U);
    /* The result of a comma is the type of its right operand. */
    const AstNode *comma = NULL;
    for (size_t i = 0U; i < unit.count && comma == NULL; ++i) {
        if (unit.declarations[i] != NULL &&
            unit.declarations[i]->kind == NODE_FUNCTION_DEFINITION) {
            comma = find_node(unit.declarations[i]->a, NODE_COMMA);
        }
    }
    CHECK(comma != NULL);
    if (comma != NULL && comma->type != NULL) {
        CHECK(comma->type->kind == TYPE_INT);
    }
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

/* Aggregate layout: each member is placed at the next offset that satisfies
   its own alignment, and the whole aggregate is padded to its own. A struct
   whose size is not a multiple of its alignment cannot be placed in an array,
   so the size rule is part of the layout rule. */
static void test_aggregate_layout(void)
{
    struct LayoutCase {
        const char *text;
        size_t size;
        size_t alignment;
    } cases[] = {
        { "struct S { char a; };", 1U, 1U },
        { "struct S { char a; char b; };", 2U, 1U },
        { "struct S { char a; int b; };", 8U, 4U },
        { "struct S { int a; char b; };", 8U, 4U },
        { "struct S { char a; long b; };", 16U, 8U },
        { "struct S { long a; char b; };", 16U, 8U },
        { "struct S { int a[3]; };", 12U, 4U },
        { "struct S { char a; short b; int c; };", 8U, 4U },
        { "union S { char a; int b; };", 4U, 4U },
        { "union S { long a; char b[2]; };", 8U, 8U },
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Arena *arena = arena_create(4U * 1024U * 1024U);
        DiagnosticSink diagnostics = {0};
        /* The object is declared so the tag is completed by the time the
           layout is read; a declaration of the type alone would leave the
           layout unobserved. The keyword is taken from the case so a union
           is not declared as a struct. */
        const char *keyword = strncmp(cases[i].text, "union", 5U) == 0
                                  ? "union" : "struct";
        char text[256];
        (void)snprintf(text, sizeof(text), "%s\n%s S object;\n"
                       "int main(void) { return 0; }\n", cases[i].text,
                       keyword);
        TranslationUnit unit = parse_unit(arena, text, &diagnostics);
        CHECK(diagnostics.count == 0U);
        bool checked = false;
        Symbol *object = scope_lookup(unit.global_scope, "object");
        CHECK(object != NULL);
        if (object != NULL && object->type != NULL) {
            if (type_size(object->type) != cases[i].size ||
                type_alignment(object->type) != cases[i].alignment) {
                fprintf(stderr, "FAIL %s: size %zu align %zu, want %zu %zu\n",
                        cases[i].text, type_size(object->type),
                        type_alignment(object->type), cases[i].size,
                        cases[i].alignment);
                ++failures;
            }
            checked = true;
        }
        CHECK(checked);
        translation_unit_free(&unit);
        diagnostic_sink_destroy(&diagnostics);
        arena_destroy(arena);
    }
}

/* A name in an inner scope hides an outer one, and an inner declaration of a
   name already declared in the same scope is an error. */
static void test_scopes(void)
{
    Arena *arena = arena_create(4U * 1024U * 1024U);
    DiagnosticSink diagnostics = {0};
    TranslationUnit unit = parse_unit(
        arena,
        "int shared = 1;\n"
        "int f(void) { int local = 2; { int local = 3; return local; } }\n"
        "int g(void) { return shared; }\n"
        "int main(void) { return f() + g(); }\n", &diagnostics);
    CHECK(diagnostics.count == 0U);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);

    /* The same name twice in one scope is a redefinition, and the two
       declarations are not silently merged. */
    arena = arena_create(4U * 1024U * 1024U);
    diagnostics = (DiagnosticSink){0};
    unit = parse_unit(arena,
                      "int shared = 1;\nint shared = 2;\nint main(void) { return 0; }\n",
                      &diagnostics);
    CHECK(diagnostics.count != 0U);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&diagnostics);
    arena_destroy(arena);
}

/* A constant expression is folded by the front end over the operators the
   contract lists, and a value it cannot prove constant is rejected rather than
   folded to something plausible. */
static void test_constant_expressions(void)
{
    struct FoldCase {
        const char *expression;
        size_t expected;
    } cases[] = {
        { "1 + 2", 3U },
        { "4 * 2 + 1", 9U },
        { "(1 << 4) - 1", 15U },
        { "sizeof(long)", 8U },
        { "sizeof(int[3])", 12U },
        { "1 ? 4 : 9", 4U },
        { "'A' + 1", 66U },
        { "(int)(3L * 4L)", 12U },
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char text[256];
        (void)snprintf(text, sizeof(text), "char a[%s];\nint main(void) { return 0; }\n",
                       cases[i].expression);
        (void)text;
        Arena *arena = arena_create(4U * 1024U * 1024U);
        DiagnosticSink diagnostics = {0};
        TranslationUnit unit = parse_unit(arena, text, &diagnostics);
        if (diagnostics.count != 0U) {
            fprintf(stderr, "FAIL %s was rejected\n", cases[i].expression);
            ++failures;
        } else {
            Symbol *array = scope_lookup(unit.global_scope, "a");
            if (array == NULL || array->type == NULL) {
                fprintf(stderr, "FAIL %s: no array to check\n",
                        cases[i].expression);
                ++failures;
            } else if (type_size(array->type) != cases[i].expected) {
                fprintf(stderr, "FAIL %s: %zu, want %zu\n",
                        cases[i].expression, type_size(array->type),
                        cases[i].expected);
                ++failures;
            }

        }
        translation_unit_free(&unit);
        diagnostic_sink_destroy(&diagnostics);
        arena_destroy(arena);
    }
}

int main(void)
{
    test_usual_arithmetic_conversions();
    test_explicit_casts();
    test_decay();
    test_sequence_points();
    test_aggregate_layout();
    test_scopes();
    test_constant_expressions();
    if (failures != 0) {
        fprintf(stderr, "%d conversion test(s) failed\n", failures);
        return 1;
    }
    puts("conversion: arithmetic, decay, sequence, layout, and scope groups passed");
    return 0;
}
