#include "cc64.h"
#include "frontend/frontend.h"
#include "semantic/semantic.h"
#include "ir/ir.h"
#include "backend/encoder.h"
#include "backend/object.h"
#include "linker/linker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum Action {
    ACTION_COMPILE,
    ACTION_LINK,
    ACTION_PREPROCESS,
    ACTION_DUMP_TOKENS,
    ACTION_DUMP_AST,
    ACTION_VERSION,
    ACTION_HELP
} Action;

typedef struct Options {
    Action action;
    const char *input;
    const char **inputs;
    size_t input_count;
    const char *output;
    bool output_set;
    const char *target;
    const char **include_paths;
    size_t include_path_count;
    const char **predefines;
    size_t predefine_count;
    bool line_markers;
    bool preserve_newlines;
    bool release_inputs;
    ImageFormat format;
} Options;

/* The target's process contract gives a command line 127 bytes, so a build
 * with more than about two dozen objects cannot be written as one line: the
 * self-hosted linker reached twenty-one inputs and the tail was cut short. An
 * argument of the form @NAME is therefore replaced by the whitespace-separated
 * words inside NAME, read through the same file interface the rest of the
 * compiler uses, so a response file means the same thing on the host and on the
 * target. A word may itself name a response file; nesting is bounded so a file
 * that names itself is reported rather than followed, and a file is bounded so
 * a large one costs a stated amount rather than whatever the heap has left. */
#define RESPONSE_DEPTH 8
#define RESPONSE_BYTES 65536U

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: cc64 [options] input.c\n"
            "  @FILE          read further arguments from FILE\n"
            "  -c             compile and emit CC64O\n"
            "  --link         link CC64O objects\n"
            "  --format NAME  raw COM (default) or mz64\n"
            "  --free         unlink each object after reading it\n"
            "  -E             preprocess only\n"
            "  -D NAME[=TEXT] define a macro\n"
            "  -I DIR         add an include directory\n"
            "  -o FILE        output path\n"
            "  -P             omit line markers\n"
            "  --dump-tokens  print the final token stream\n"
            "  --dump-ast     parse and print the typed AST\n"
            "  --target NAME  target contract (default: " CC64_TARGET ")\n"
            "  --version      print version\n"
            "  --help         print help\n");
}

static bool response_push(const char **items, size_t *count, size_t capacity,
                          const char *word)
{
    if (*count >= capacity) {
        return false;
    }
    items[(*count)++] = word;
    return true;
}

static bool is_space(int character)
{
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n' || character == '\f' || character == '\v';
}

/* Read one response file and append its words. A word is either a plain
 * argument or a quoted run, so a path may contain a space. */
static bool append_response_file(const char *name, const char **items, size_t *count,
                                 size_t capacity, unsigned depth, int *status)
{
    char *text;
    size_t used = 0U;
    FILE *stream;
    if (depth >= RESPONSE_DEPTH) {
        fprintf(stderr, "cc64: CC1001: @%s nests deeper than %u response files\n",
                name, (unsigned)RESPONSE_DEPTH);
        *status = 1;
        return false;
    }
    stream = fopen(name, "rb");
    if (stream == NULL) {
        fprintf(stderr, "cc64: CC1002: cannot read response file '%s'\n", name);
        *status = 1;
        return false;
    }
    /* The file is read up to the ceiling; a file that fills it is refused
     * rather than truncated, because a truncated argument list would link the
     * wrong image without saying so. */
    text = cc64_xmalloc(RESPONSE_BYTES);
    for (;;) {
        size_t got = fread(text + used, 1U, RESPONSE_BYTES - used, stream);
        used += got;
        if (used < RESPONSE_BYTES) {
            break;
        }
        fprintf(stderr, "cc64: CC1003: response file '%s' is larger than "
                "%lu bytes\n", name, (unsigned long)RESPONSE_BYTES);
        free(text);
        fclose(stream);
        *status = 1;
        return false;
    }
    fclose(stream);
    text[used] = '\0';
    for (size_t i = 0U; i < used; ++i) {
        size_t start;
        size_t length;
        char *word;
        if (is_space((unsigned char)text[i])) {
            continue;
        }
        start = i;
        if (text[i] == '"' || text[i] == '\'') {
            char quote = text[i++];
            start = i;
            while (i < used && text[i] != quote) {
                ++i;
            }
            length = i - start;
            if (i < used) {
                ++i;
            }
        } else {
            while (i < used && !is_space((unsigned char)text[i])) {
                ++i;
            }
            length = i - start;
        }
        word = cc64_xmalloc(length + 1U);
        memcpy(word, text + start, length);
        word[length] = '\0';
        if (word[0] == '@' && word[1] != '\0') {
            bool good = append_response_file(word + 1, items, count, capacity,
                                             depth + 1U, status);
            free(word);
            if (!good) {
                free(text);
                return false;
            }
        } else if (!response_push(items, count, capacity, word)) {
            fprintf(stderr, "cc64: CC1004: response file '%s' holds more than "
                    "%lu arguments\n", name, (unsigned long)capacity);
            free(word);
            free(text);
            *status = 1;
            return false;
        }
    }
    free(text);
    return true;
}

/* Replace every @NAME argument with the words it names. The result is a fresh
 * argument vector: the caller's is left alone, and argv[0] stays in place. */
static char **expand_response_files(int argc, char **argv, int *expanded,
                                    int *status)
{
    const size_t capacity = 1024U;
    const char **items = cc64_xmalloc(capacity * sizeof(*items));
    char **result;
    size_t count = 0U;
    *status = 0;
    items[count++] = argv[0];
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '@' && argv[i][1] != '\0') {
            if (!append_response_file(argv[i] + 1, items, &count, capacity,
                                      1U, status)) {
                free(items);
                return NULL;
            }
        } else if (!response_push(items, &count, capacity, argv[i])) {
            fprintf(stderr, "cc64: CC1004: more than %lu arguments\n",
                    (unsigned long)capacity);
            free(items);
            *status = 1;
            return NULL;
        }
    }
    result = cc64_xmalloc((count + 1U) * sizeof(*result));
    for (size_t i = 0U; i < count; ++i) {
        result[i] = (char *)items[i];
    }
    result[count] = NULL;
    free(items);
    *expanded = (int)count;
    return result;
}

static bool take_value(int argc, char **argv, int *index, const char **value)
{
    if (*index + 1 >= argc) {
        return false;
    }
    ++*index;
    *value = argv[*index];
    return true;
}

static bool add_option_value(const char **values, size_t *count, size_t capacity,
                             const char *value)
{
    if (*count == capacity) {
        return false;
    }
    values[(*count)++] = value;
    return true;
}

static void free_options(Options *options)
{
    free(options->include_paths);
    free(options->predefines);
    free(options->inputs);
}

static bool parse_options(int argc, char **argv, Options *options,
                          DiagnosticSink *sink)
{
    options->action = ACTION_COMPILE;
    options->input = NULL;
    options->inputs = cc64_xmalloc(64U * sizeof(*options->inputs));
    options->input_count = 0U;
    options->output = "a.o";
    options->output_set = false;
    options->target = CC64_TARGET;
    options->format = IMAGE_RAW_COM;
    options->include_paths = cc64_xmalloc(64U * sizeof(*options->include_paths));
    options->include_path_count = 0U;
    options->predefines = cc64_xmalloc(64U * sizeof(*options->predefines));
    options->predefine_count = 0U;
    options->line_markers = false;
    options->preserve_newlines = true;
    options->release_inputs = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = NULL;
        if (strcmp(arg, "-c") == 0) {
            options->action = ACTION_COMPILE;
        } else if (strcmp(arg, "--link") == 0) {
            options->action = ACTION_LINK;
        } else if (strcmp(arg, "--format") == 0) {
            if (!take_value(argc, argv, &i, &value)) {
                diagnostic_emit(sink, 8U, DIAG_DRIVER, NULL, 0U, 0U,
                                "option '--format' requires a name");
                return false;
            }
            if (strcmp(value, "com") == 0 || strcmp(value, "raw") == 0) options->format = IMAGE_RAW_COM;
            else if (strcmp(value, "mz64") == 0) options->format = IMAGE_MZ64;
            else {
                diagnostic_emit(sink, 9U, DIAG_DRIVER, NULL, 0U, 0U,
                                "unsupported image format");
                return false;
            }
        } else if (strcmp(arg, "-E") == 0) {
            options->action = ACTION_PREPROCESS;
        } else if (strcmp(arg, "--dump-tokens") == 0) {
            options->action = ACTION_DUMP_TOKENS;
        } else if (strcmp(arg, "--dump-ast") == 0) {
            options->action = ACTION_DUMP_AST;
        } else if (strcmp(arg, "-P") == 0) {
            options->line_markers = false;
        } else if (strcmp(arg, "--free") == 0) {
            options->release_inputs = true;
        } else if (strcmp(arg, "--line-markers") == 0) {
            options->line_markers = true;
        } else if (strcmp(arg, "-o") == 0) {
            if (!take_value(argc, argv, &i, &options->output)) {
                diagnostic_emit(sink, 1U, DIAG_DRIVER, NULL, 0U, 0U,
                                "option '-o' requires a path");
                return false;
            }
            options->output_set = true;
        } else if (strcmp(arg, "-I") == 0 || strcmp(arg, "--include-dir") == 0) {
            if (!take_value(argc, argv, &i, &value) ||
                !add_option_value(options->include_paths,
                                  &options->include_path_count, 64U, value)) {
                diagnostic_emit(sink, 2U, DIAG_DRIVER, NULL, 0U, 0U,
                                "option '-I' requires a directory");
                return false;
            }
        } else if (strncmp(arg, "-I", 2U) == 0 && arg[2] != '\0') {
            /* The attached -Idirectory form is also accepted. */
            if (!add_option_value(options->include_paths,
                                  &options->include_path_count, 64U, arg + 2)) {
                diagnostic_emit(sink, 2U, DIAG_DRIVER, NULL, 0U, 0U,
                                "option '-I' requires a directory");
                return false;
            }
        } else if (strncmp(arg, "-D", 2U) == 0 && arg[2] != '\0') {
            /* The attached -DNAME=VALUE form is also accepted. */
            if (!add_option_value(options->predefines,
                                  &options->predefine_count, 64U, arg + 2)) {
                diagnostic_emit(sink, 3U, DIAG_DRIVER, NULL, 0U, 0U,
                                "option '-D' requires a definition");
                return false;
            }
        } else if (strcmp(arg, "-D") == 0) {
            if (!take_value(argc, argv, &i, &value) ||
                !add_option_value(options->predefines,
                                  &options->predefine_count, 64U, value)) {
                diagnostic_emit(sink, 3U, DIAG_DRIVER, NULL, 0U, 0U,
                                "option '-D' requires a macro definition");
                return false;
            }
        } else if (strcmp(arg, "--target") == 0) {
            if (!take_value(argc, argv, &i, &options->target)) {
                diagnostic_emit(sink, 4U, DIAG_DRIVER, NULL, 0U, 0U,
                                "option '--target' requires a name");
                return false;
            }
        } else if (strcmp(arg, "--version") == 0) {
            options->action = ACTION_VERSION;
        } else if (strcmp(arg, "--help") == 0) {
            options->action = ACTION_HELP;
        } else if (arg[0] == '-') {
            char message[160];
            (void)snprintf(message, sizeof(message), "unknown option '%s'", arg);
            diagnostic_emit(sink, 5U, DIAG_DRIVER, NULL, 0U, 0U, message);
            return false;
        } else if (options->input_count < 64U) {
            options->inputs[options->input_count++] = arg;
            options->input = options->inputs[0];
        } else {
            diagnostic_emit(sink, 6U, DIAG_DRIVER, NULL, 0U, 0U,
                            "too many input files");
            return false;
        }
    }
    if ((options->action == ACTION_COMPILE || options->action == ACTION_LINK ||
         options->action == ACTION_PREPROCESS || options->action == ACTION_DUMP_TOKENS ||
         options->action == ACTION_DUMP_AST) && options->input == NULL) {
        diagnostic_emit(sink, 7U, DIAG_DRIVER, NULL, 0U, 0U, "missing input file");
        return false;
    }
    if (options->action == ACTION_LINK && !options->output_set) {
        options->output = "a.com";
    }
    return true;
}

static void print_diagnostics(const DiagnosticSink *sink)
{
    for (size_t i = 0U; i < sink->count; ++i) {
        diagnostic_print(&sink->items[i], stderr);
    }
}

static bool write_preprocessed(const Options *options, const TokenList *tokens)
{
    /* With no -o the preprocessed text goes to the standard output. The default
       output name belongs to the compile and link actions and must not be
       inherited here: taking it made `-E` write the text to a file called a.o
       and print nothing at all. */
    if (!options->output_set || options->output == NULL ||
        strcmp(options->output, "-") == 0) {
        return write_token_list(stdout, tokens, options->line_markers);
    }
    /* Preprocessing has already succeeded, so the output file is created once
       and streamed directly; there is no temporary-file rename step. */
    FILE *stream = fopen(options->output, "wb");
    if (stream == NULL) {
        return false;
    }
    bool good = write_token_list(stream, tokens, options->line_markers);
    if (fclose(stream) != 0) {
        good = false;
    }
    return good;
}

static bool print_tokens(const TokenList *tokens)
{
    for (size_t i = 0U; i < tokens->count; ++i) {
        const Token *token = &tokens->items[i];
        if (token->kind == TOKEN_EOF) {
            break;
        }
        printf("%lu:%lu:%s:%s\n", (unsigned long)token->line,
               (unsigned long)token->column, token_kind_name(token->kind),
               token->text);
    }
    return !ferror(stdout);
}

static void print_ast_node(const AstNode *node, unsigned depth)
{
    if (node == NULL) return;
    for (unsigned i = 0U; i < depth; ++i) fputs("  ", stdout);
    printf("%s:%s", ast_node_kind_name(node->kind),
           node->type == NULL ? "none" : type_kind_name(node->type->kind));
    if (node->symbol != NULL) printf(" symbol=%s", node->symbol->name);
    putchar('\n');
    print_ast_node(node->a, depth + 1U);
    print_ast_node(node->b, depth + 1U);
    print_ast_node(node->c, depth + 1U);
    print_ast_node(node->d, depth + 1U);
    print_ast_node(node->next, depth);
}

static bool print_ast(const TranslationUnit *unit)
{
    for (size_t i = 0U; i < unit->count; ++i) {
        print_ast_node(unit->declarations[i], 0U);
    }
    return !ferror(stdout);
}

/* derive_output_name builds the per-input output path for a multi-input run: the
   input's own stem with the action's extension. A single -o cannot name more
   than one output, so that combination is rejected instead of guessed. */
static char *derive_output_name(const char *input, const char *extension)
{
    size_t length = strlen(input);
    size_t base = length;
    while (base > 0U) {
        char c = input[base - 1U];
        if (c == '/' || c == '\\' || c == ':') break;
        --base;
    }
    const char *dot = NULL;
    for (size_t i = length; i > base; --i) {
        if (input[i - 1U] == '.') { dot = input + i - 1U; break; }
    }
    size_t stem = dot == NULL ? length : (size_t)(dot - input);
    size_t extra = strlen(extension);
    char *name = cc64_xmalloc(stem + extra + 1U);
    if (name == NULL) return NULL;
    memcpy(name, input, stem);
    memcpy(name + stem, extension, extra + 1U);
    return name;
}

/* compile_one runs the whole pipeline for a single input. The driver keeps one
   options block, so several inputs share every setting and differ only in the
   input path and the output path derived from it. Diagnostics accumulate in the
   shared sink, and the return value reports only what this input produced. */
/* Parse, lower, encode, and release one declaration at a time.
 *
 * A function's syntax tree and its lowered form are both dead once the encoder
 * has emitted the function, so they go back to their arenas before the next
 * declaration is parsed. A declaration of an object is kept: the encoder reads
 * its initializer to emit the data after every function has been encoded. */
static bool compile_declarations(Arena *arena, Arena *ir, Arena *nodes,
                                 Parser *parser, DiagnosticSink *sink,
                                 TranslationUnit *unit, IrProgram *program,
                                 IrEncoder *encoder)
{
    if (parser == NULL) return false;
    bool good = true;
    size_t first = 0U;
    size_t last = 0U;
    ArenaMark mark;
    ArenaMark irmark;
    arena_mark(nodes, &mark);
    while (good && parser_next(parser, &first, &last)) {
        bool function = false;
        for (size_t i = first; i < last && good; ++i) {
            if (unit->declarations[i]->kind == NODE_FUNCTION_DEFINITION) {
                function = true;
            }
            IrFunction *before = program->function_tail;
            arena_mark(ir, &irmark);
            good = lower_declaration(arena, ir, unit, unit->declarations[i], sink,
                                     program);
            if (good && program->function_tail != before) {
                good = ir_encoder_add_function(encoder, program->function_tail);
                if (before != NULL) {
                    before->next = NULL;
                } else {
                    program->functions = NULL;
                }
                program->function_tail = before;
            }
            arena_release(ir, &irmark);
        }
        if (good && function) {
            arena_release(nodes, &mark);
            for (size_t i = first; i < last; ++i) unit->declarations[i] = NULL;
        }
        arena_mark(nodes, &mark);
    }
    if (good) good = parser_finish(parser);
    parser_destroy(parser);
    return good;
}

static int compile_one(Options *options, DiagnosticSink *sink)
{
    size_t errors_before = sink->count;
    Arena *arena = arena_create(256U * 1024U * 1024U);
    SourceManager *manager = source_manager_create(arena);
    Source *source = manager == NULL ? NULL
                                  : source_manager_load(manager, options->input);
    if (source == NULL) {
        diagnostic_emit(sink, 11U, DIAG_DRIVER, NULL, 0U, 0U,
                        "cannot read input file");
        print_diagnostics(sink);
        arena_destroy(arena);
        return 1;
    }

    /* Newline tokens are not only for the dumping actions: a preprocessing
       directive ends at the end of its line, so the token stream has to carry
       line boundaries through the whole preprocessor. The parser drops them
       again when it compacts the stream in place. */
    PreprocessorOptions pp_options = {
        options->include_paths, options->include_path_count,
        options->predefines, options->predefine_count, options->preserve_newlines
    };
    TokenList tokens;
    token_list_init(&tokens);
    size_t diagnostics_before = sink->count;
    bool good = preprocess_source(arena, manager, sink, source, &pp_options, &tokens);
    if (good && sink->count != diagnostics_before) {
        good = false;
    }
    TranslationUnit unit = {0};
    IrProgram program = {0};
    if (good && options->action == ACTION_DUMP_AST) {
        size_t parse_before = sink->count;
        good = parse_tokens(arena, &tokens, sink, &unit);
        if (sink->count != parse_before) {
            good = false;
        }
    }
    if (good) {
        if (options->action == ACTION_DUMP_TOKENS) {
            good = print_tokens(&tokens);
        } else if (options->action == ACTION_PREPROCESS) {
            good = write_preprocessed(options, &tokens);
        } else if (options->action == ACTION_DUMP_AST) {
            good = print_ast(&unit);
        } else {
            /* A declaration is parsed, lowered, encoded, and then released, so
               neither the front end nor the middle end ever holds every
               function's syntax tree or its lowered form at once. That is what
               keeps a large translation unit inside the target's memory: names,
               types, and literal text live in the durable arena, and the only
               long-lived copy of a function is the machine code emitted from
               it. */
            Arena *nodes = arena_create(64U * 1024U * 1024U);
            Arena *ir = arena_create(64U * 1024U * 1024U);
            ObjectBuilder builder;
            object_builder_init(&builder, arena);
            IrEncoder *encoder = nodes == NULL || ir == NULL
                                     ? NULL
                                     : ir_encoder_create(arena, &builder, sink);
            /* The parser keeps its own compact copy of the token stream, so the
               preprocessed list is released here instead of being held for the
               whole compile. */
            good = encoder != NULL &&
                   compile_declarations(arena, ir, nodes,
                                        nodes == NULL ? NULL
                                                      : parser_create(arena, nodes,
                                                                      &tokens, sink,
                                                                      &unit),
                                        sink, &unit, &program, encoder) &&
                   ir_encoder_finish(encoder, &program) &&
                   object_write_cc64o(&builder, options->output, sink);
            ir_encoder_destroy(encoder);
            object_builder_destroy(&builder);
            arena_destroy(ir);
            arena_destroy(nodes);
        }
    }
    print_diagnostics(sink);
    size_t errors = sink->count - errors_before;
    token_list_free(&tokens);
    ir_program_free(&program);
    translation_unit_free(&unit);
    arena_destroy(arena);
    return good && errors == 0U ? 0 : 1;
}

int cc64_main(int argc, char **argv)
{
    DiagnosticSink sink = {0};
    sink.limit = 100U;
    Options options;
    char **expanded = NULL;
    int expanded_count = argc;
    int expansion_status = 0;
    expanded = expand_response_files(argc, argv, &expanded_count,
                                     &expansion_status);
    if (expanded == NULL) {
        return expansion_status == 0 ? 1 : expansion_status;
    }
    if (!parse_options(expanded_count, expanded, &options, &sink)) {
        print_diagnostics(&sink);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        free(expanded);
        return 1;
    }
    if (options.action == ACTION_VERSION) {
        printf("cc64 %s target=%s\n", CC64_VERSION, options.target);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        free(expanded);
        return 0;
    }
    if (options.action == ACTION_HELP) {
        usage(stdout);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        free(expanded);
        return 0;
    }
    if (options.action == ACTION_LINK) {
        Arena *arena = arena_create(256U * 1024U * 1024U);
        bool good = link_objects(arena, (const char *const *)options.inputs,
                                 options.input_count, options.output,
                                 options.format, options.release_inputs, &sink);
        print_diagnostics(&sink);
        size_t errors = sink.count;
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        arena_destroy(arena);
        return good && errors == 0U ? 0 : 1;
    }
    if (strcmp(options.target, CC64_TARGET) != 0) {
        diagnostic_emit(&sink, 12U, DIAG_DRIVER, NULL, 0U, 0U,
                        "unsupported target contract");
        print_diagnostics(&sink);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        free(expanded);
        return 1;
    }
    if (options.input_count > 1U && options.output_set) {
        diagnostic_emit(&sink, 13U, DIAG_DRIVER, NULL, 0U, 0U,
                        "option '-o' cannot be used with several inputs");
        print_diagnostics(&sink);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        free(expanded);
        return 1;
    }
    /* Every action other than linking produces one file per input. */
    if (options.input_count > 1U) {
        const char *extension =
            options.action == ACTION_COMPILE ? ".cc64o" : ".i";
        int status = 0;
        for (size_t i = 0U; i < options.input_count; ++i) {
            char *derived = derive_output_name(options.inputs[i], extension);
            if (derived == NULL) {
                status = 1;
                break;
            }
            options.input = options.inputs[i];
            options.output = derived;
            if (compile_one(&options, &sink) != 0) status = 1;
            free(derived);
        }
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        free(expanded);
        return status;
    }
    {
        int status = compile_one(&options, &sink);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        free(expanded);
        return status;
    }
}

int main(int argc, char **argv)
{
    return cc64_main(argc, argv);
}
