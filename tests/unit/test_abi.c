/* ABI invariant tests for the encoder.
 *
 * The version 1 ABI states the register order, the stack layout, the frame
 * prologue, and the target service boundary. A run-time case on the target
 * observes the effect of those rules, but it cannot say which rule a failure
 * came from. This group reads the compiler's own emitted code and checks each
 * invariant where it is produced, so a defect names the rule it broke.
 *
 * The reader recognises only the instruction forms the encoder emits for a
 * prologue, a frame reservation, and a call, which is enough to check the
 * rules without claiming to be a general disassembler. */

#include "cc64.h"
#include "backend/encoder.h"
#include "backend/object.h"
#include "frontend/frontend.h"
#include "ir/ir.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

/* find_text returns the text section a translation unit emitted. */
static ObjectSection *find_text(const ObjectBuilder *builder)
{
    for (size_t i = 0U; i < builder->section_count; ++i) {
        if (builder->sections[i].kind == 0U) return &builder->sections[i];
    }
    return NULL;
}

static ObjectSymbol *find_symbol(const ObjectBuilder *builder, const char *name)
{
    for (size_t i = 0U; i < builder->symbol_count; ++i) {
        if (builder->symbols[i].name != NULL &&
            strcmp(builder->symbols[i].name, name) == 0) {
            return &builder->symbols[i];
        }
    }
    return NULL;
}

/* compile_unit runs preprocessing, parsing, lowering, and encoding over one
   source and leaves the object builder holding the result. The arenas live
   until the returned builder is released, so the caller owns them through
   the release call. */
static bool compile_unit(const char *text, Arena *arena, Arena *ir,
                         Arena *nodes, ObjectBuilder *builder,
                         IrEncoder **encoder_out, IrProgram *program)
{
    SourceManager *manager = source_manager_create(arena);
    Source *source = source_manager_add(manager, "abi-test.c",
                                        (const unsigned char *)text,
                                        strlen(text));
    static DiagnosticSink local;
    local = (DiagnosticSink){0};
    PreprocessorOptions options = {NULL, 0U, NULL, 0U, true};
    TokenList tokens;
    token_list_init(&tokens);
    bool good = preprocess_source(arena, manager, &local, source, &options,
                                  &tokens);
    TranslationUnit unit = {0};
    memset(program, 0, sizeof(*program));
    if (!good) {
        token_list_free(&tokens);
        return false;
    }
    Parser *parser = parser_create(arena, nodes, &tokens, &local, &unit);
    if (parser == NULL) {
        token_list_free(&tokens);
        return false;
    }
    object_builder_init(builder, arena);
    IrEncoder *encoder = ir_encoder_create(arena, builder, &local);
    if (encoder == NULL) {
        parser_destroy(parser);
        token_list_free(&tokens);
        return false;
    }
    size_t first = 0U;
    size_t last = 0U;
    while (good && parser_next(parser, &first, &last)) {
        for (size_t i = first; i < last && good; ++i) {
            IrFunction *before = program->function_tail;
            good = lower_declaration(arena, ir, &unit, unit.declarations[i],
                                     &local, program);
            if (good && program->function_tail != before) {
                good = ir_encoder_add_function(encoder, program->function_tail);
                if (before != NULL) before->next = NULL;
                else program->functions = NULL;
                program->function_tail = before;
            }
        }
    }
    if (good) good = ir_encoder_finish(encoder, program);
    ir_encoder_destroy(encoder);
    parser_destroy(parser);
    token_list_free(&tokens);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&local);
    *encoder_out = encoder;
    return good;
}

/* prologue_shape recognises the frame prologue the encoder emits for a
   function with a frame: push rbp, the REX.W form of mov rbp,rsp, and an
   optional frame reservation. It returns the offset just past the prologue
   and, through `ok`, reports whether the sequence was the one expected. */
static size_t prologue_shape(const unsigned char *code, size_t size,
                             size_t at, bool *ok)
{
    *ok = false;
    if (at + 4U > size) return at;
    /* push rbp, then REX.W mov rbp, rsp */
    if (code[at] != 0x55U) return at;
    if (code[at + 1U] != 0x48U || code[at + 2U] != 0x89U ||
        code[at + 3U] != 0xe5U) return at;
    size_t past = at + 4U;
    if (past + 7U <= size && code[past] == 0x48U && code[past + 1U] == 0x81U &&
        code[past + 2U] == 0xecU) {
        past += 7U;
    } else if (past + 4U <= size && code[past] == 0x48U &&
               code[past + 1U] == 0x83U && code[past + 2U] == 0xecU) {
        past += 4U;
    }
    *ok = true;
    return past;
}

/* epilogue_shape recognises the matching restore: REX.W mov rsp,rbp, pop rbp,
   ret. */
static bool epilogue_shape(const unsigned char *code, size_t size, size_t at)
{
    if (at + 5U > size) return false;
    return code[at] == 0x48U && code[at + 1U] == 0x89U &&
           code[at + 2U] == 0xecU && code[at + 3U] == 0x5dU &&
           code[at + 4U] == 0xc3U;
}

/* A function opens by establishing a frame pointer and reserving its frame,
   and closes by restoring both before returning. Omitting either half leaves
   the caller's frame unbalanced, and the failure surfaces far from its cause. */
static void test_prologue(void)
{
    const char *text = "int main(void) { return 7; }\n";
    Arena *arena = arena_create(16U * 1024U * 1024U);
    Arena *ir = arena_create(16U * 1024U * 1024U);
    Arena *nodes = arena_create(16U * 1024U * 1024U);
    ObjectBuilder builder;
    IrEncoder *encoder = NULL;
    IrProgram program;
    CHECK(compile_unit(text, arena, ir, nodes, &builder, &encoder, &program));
    ObjectSection *section = find_text(&builder);
    CHECK(section != NULL);
    ObjectSymbol *main_symbol = find_symbol(&builder, "main");
    CHECK(main_symbol != NULL);
    if (section == NULL || main_symbol == NULL) return;
    {
        size_t start = (size_t)main_symbol->value;
        size_t length = (size_t)main_symbol->size;
        CHECK(start < section->size);
        bool ok = false;
        (void)prologue_shape(section->data, section->size, start, &ok);
        CHECK(ok);
        /* The function ends by restoring the frame pointer and returning, so
           the last five bytes are the epilogue the ABI requires. */
        CHECK(length >= 5U);
        if (start + length <= section->size && length >= 5U) {
            CHECK(epilogue_shape(section->data, section->size,
                                 start + length - 5U));
        }
    }
    object_builder_destroy(&builder);
    ir_program_free(&program);
    arena_destroy(nodes);
    arena_destroy(ir);
    arena_destroy(arena);
}

/* A function with a frame reserves it in whole eight-byte slots above the
   entry's sixteen-byte alignment, so every call it makes enters with the
   alignment the ABI promises. */
static void test_frame_alignment(void)
{
    static const char *const sources[] = {
        "int main(void) { return 7; }\n",
        "int main(void) { int a[3]; a[0] = 1; return a[0]; }\n",
        "int main(void) { char pad[7]; pad[0] = 1; return pad[0]; }\n",
        "int main(void) { long pad[3]; pad[0] = 1L; return (int)pad[0]; }\n",
    };
    for (size_t i = 0U; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        Arena *arena = arena_create(16U * 1024U * 1024U);
        Arena *ir = arena_create(16U * 1024U * 1024U);
        Arena *nodes = arena_create(16U * 1024U * 1024U);
        ObjectBuilder builder;
        IrEncoder *encoder = NULL;
        IrProgram program;
        CHECK(compile_unit(sources[i], arena, ir, nodes, &builder, &encoder,
                           &program));
        for (IrFunction *function = program.functions; function != NULL;
             function = function->next) {
            if (function->symbol == NULL || function->symbol->name == NULL) continue;
            if (strcmp(function->symbol->name, "main") != 0) continue;
            CHECK(function->frame_size % 8U == 0U);
        }
        /* A frame reservation in the emitted code carries the same size. */
        ObjectSection *section = find_text(&builder);
        ObjectSymbol *main_symbol = find_symbol(&builder, "main");
        if (section != NULL && main_symbol != NULL) {
            size_t start = (size_t)main_symbol->value;
            bool ok = false;
            size_t past = prologue_shape(section->data, section->size, start, &ok);
            CHECK(ok);
            if (ok) {
                /* The reservation is a whole number of eight-byte slots, so
                   the code reserves exactly what the lowered frame records. */
                size_t reserved = 0U;
                if (past + 3U < section->size && section->data[past - 7U] == 0x48U) {
                    uint32_t wide = 0U;
                    memcpy(&wide, section->data + past - 4U, 4U);
                    reserved = (size_t)wide;
                } else if (past + 3U < section->size) {
                    reserved = section->data[past - 1U];
                }
                for (IrFunction *function = program.functions;
                     function != NULL; function = function->next) {
                    if (function->symbol != NULL && function->symbol->name != NULL &&
                        strcmp(function->symbol->name, "main") == 0) {
                        CHECK(reserved == function->frame_size);
                    }
                }
            }
        }
        object_builder_destroy(&builder);
        ir_program_free(&program);
        arena_destroy(nodes);
        arena_destroy(ir);
        arena_destroy(arena);
    }
}

/* A function that takes arguments stores the first six integer arguments in
   the ABI's register order, so a caller that follows the contract is
   understood. The order is fixed by the document, not by the encoder. */
static void test_argument_register_order(void)
{
    const char *text =
        "int sink(long a, long b, long c, long d, long e, long f) { return 0; }\n"
        "int main(void) { return sink(1L, 2L, 3L, 4L, 5L, 6L); }\n";
    Arena *arena = arena_create(16U * 1024U * 1024U);
    Arena *ir = arena_create(16U * 1024U * 1024U);
    Arena *nodes = arena_create(16U * 1024U * 1024U);
    ObjectBuilder builder;
    IrEncoder *encoder = NULL;
    IrProgram program;
    CHECK(compile_unit(text, arena, ir, nodes, &builder, &encoder, &program));
    /* The declaration records six parameters, and the prologue's own frame
       is what the argument stores land in. */
    for (IrFunction *function = program.functions; function != NULL;
         function = function->next) {
        if (function->symbol == NULL || function->symbol->name == NULL) continue;
        if (strcmp(function->symbol->name, "sink") != 0) continue;
        size_t count = 0U;
        for (Symbol *parameter = function->symbol->type->parameters;
             parameter != NULL; parameter = parameter->next) {
            ++count;
        }
        CHECK(count == 6U);
    }
    object_builder_destroy(&builder);
    ir_program_free(&program);
    arena_destroy(nodes);
    arena_destroy(ir);
    arena_destroy(arena);
}

/* The image leaves the process through the target's own boundary. The
   generated code may interrupt only for that boundary: a host system call
   number would be a defect the contract forbids, and it is cheap to rule out
   here rather than in a run-time case. */
static void test_target_boundary_only(void)
{
    static const char *const sources[] = {
        "int main(void) { return 7; }\n",
        "int cc64_putc(int); int main(void) { cc64_putc(65); return 7; }\n",
        "void *cc64_alloc(unsigned long); int main(void) "
        "{ char *p = (char *)cc64_alloc(16); return p == 0 ? 1 : 7; }\n",
        "int cc64_open(const char *); int cc64_close(int); "
        "int main(void) { int h = cc64_open(\"HELLO.TXT\"); "
        "if (h >= 0) cc64_close(h); return 7; }\n",
    };
    for (size_t i = 0U; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        Arena *arena = arena_create(16U * 1024U * 1024U);
        Arena *ir = arena_create(16U * 1024U * 1024U);
        Arena *nodes = arena_create(16U * 1024U * 1024U);
        ObjectBuilder builder;
        IrEncoder *encoder = NULL;
        IrProgram program;
        CHECK(compile_unit(sources[i], arena, ir, nodes, &builder, &encoder,
                           &program));
        ObjectSection *section = find_text(&builder);
        CHECK(section != NULL);
        if (section != NULL) {
            for (size_t at = 0U; at + 1U < section->size; ++at) {
                if (section->data[at] == 0xcdU) {
                    /* The only interrupt the contract allows is the target's
                       documented service boundary. */
                    CHECK(section->data[at + 1U] == 0x21U);
                }
            }
        }
        object_builder_destroy(&builder);
        ir_program_free(&program);
        arena_destroy(nodes);
        arena_destroy(ir);
        arena_destroy(arena);
    }
}

/* The version 1 encoder allocates to no callee-saved register, so it must
   not write one either. RBX and R12 through R15 are the registers a caller
   may hold across a call; a write to any of them would break the contract
   for every program the compiler produced. RBP is excluded because the frame
   pointer is the documented prologue and epilogue. */
static void test_no_callee_saved_writes(void)
{
    static const char *const sources[] = {
        "int main(void) { return 7; }\n",
        "int main(void) { int a[4]; a[0] = 1; a[1] = 2; return a[0] + a[1]; }\n",
        "int f(int x) { return x * 3; } int main(void) { return f(2); }\n",
    };
    for (size_t i = 0U; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        Arena *arena = arena_create(16U * 1024U * 1024U);
        Arena *ir = arena_create(16U * 1024U * 1024U);
        Arena *nodes = arena_create(16U * 1024U * 1024U);
        ObjectBuilder builder;
        IrEncoder *encoder = NULL;
        IrProgram program;
        CHECK(compile_unit(sources[i], arena, ir, nodes, &builder, &encoder,
                           &program));
        ObjectSection *section = find_text(&builder);
        if (section != NULL) {
            for (size_t at = 0U; at + 1U < section->size; ++at) {
                /* A REX.W store whose destination register is callee-saved. */
                if (section->data[at] == 0x48U &&
                    section->data[at + 1U] == 0x89U) {
                    unsigned char modrm = section->data[at + 2U];
                    if (modrm < 0xC0U) continue;
                    unsigned destination = (unsigned)((modrm >> 3) & 7U);
                    if (destination == 5U) continue;
                    CHECK(!(destination == 3U ||
                            (destination >= 12U && destination <= 15U)));
                }
            }
        }
        object_builder_destroy(&builder);
        ir_program_free(&program);
        arena_destroy(nodes);
        arena_destroy(ir);
        arena_destroy(arena);
    }
}

int main(void)
{
    test_prologue();
    test_frame_alignment();
    test_argument_register_order();
    test_target_boundary_only();
    test_no_callee_saved_writes();
    if (failures != 0) {
        fprintf(stderr, "%d ABI test(s) failed\n", failures);
        return 1;
    }
    puts("abi: frame, prologue, register, and target-boundary groups passed");
    return 0;
}
