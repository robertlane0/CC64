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
    ImageFormat format;
} Options;

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: cc64 [options] input.c\n"
            "  -c             compile and emit CC64O\n"
            "  --link         link CC64O objects\n"
            "  --format NAME  raw COM (default) or mz64\n"
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
    if (options->output == NULL || strcmp(options->output, "-") == 0) {
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
        printf("%zu:%zu:%s:%s\n", token->line, token->column,
               token_kind_name(token->kind), token->text);
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

int cc64_main(int argc, char **argv)
{
    DiagnosticSink sink = {0};
    sink.limit = 100U;
    Options options;
    if (!parse_options(argc, argv, &options, &sink)) {
        print_diagnostics(&sink);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        return 1;
    }
    if (options.action == ACTION_VERSION) {
        printf("cc64 %s target=%s\n", CC64_VERSION, options.target);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        return 0;
    }
    if (options.action == ACTION_HELP) {
        usage(stdout);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        return 0;
    }
    if (options.action == ACTION_LINK) {
        Arena *arena = arena_create(256U * 1024U * 1024U);
        bool good = link_objects(arena, (const char *const *)options.inputs,
                                 options.input_count, options.output,
                                 options.format, &sink);
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
        return 1;
    }

    Arena *arena = arena_create(256U * 1024U * 1024U);
    SourceManager *manager = source_manager_create(arena);
    Source *source = manager == NULL ? NULL : source_manager_load(manager, options.input);
    if (source == NULL) {
        diagnostic_emit(&sink, 11U, DIAG_DRIVER, NULL, 0U, 0U,
                        "cannot read input file");
        print_diagnostics(&sink);
        diagnostic_sink_destroy(&sink);
        free_options(&options);
        arena_destroy(arena);
        return 1;
    }

    PreprocessorOptions pp_options = {
        options.include_paths, options.include_path_count,
        options.predefines, options.predefine_count, options.preserve_newlines
    };
    TokenList tokens;
    token_list_init(&tokens);
    size_t diagnostics_before = sink.count;
    bool good = preprocess_source(arena, manager, &sink, source, &pp_options,
                                  &tokens);
    if (good && sink.count != diagnostics_before) {
        good = false;
    }
    TranslationUnit unit = {0};
    IrProgram program = {0};
    if (good && (options.action == ACTION_COMPILE ||
                 options.action == ACTION_DUMP_AST)) {
        size_t parse_before = sink.count;
        good = parse_tokens(arena, &tokens, &sink, &unit);
        if (sink.count != parse_before) {
            good = false;
        }
    }
    if (good) {
        if (options.action == ACTION_DUMP_TOKENS) {
            good = print_tokens(&tokens);
        } else if (options.action == ACTION_PREPROCESS) {
            good = write_preprocessed(&options, &tokens);
        } else if (options.action == ACTION_DUMP_AST) {
            good = print_ast(&unit);
        } else {
            size_t lower_before = sink.count;
            good = lower_translation_unit(arena, &unit, &sink, &program);
            if (sink.count != lower_before) good = false;
            if (good) {
                ObjectBuilder builder;
                object_builder_init(&builder, arena);
                good = encode_ir_program(arena, &program, &builder, &sink) &&
                       object_write_cc64o(&builder, options.output, &sink);
                object_builder_destroy(&builder);
            }
        }
    }
    print_diagnostics(&sink);
    size_t errors = sink.count;
    token_list_free(&tokens);
    ir_program_free(&program);
    translation_unit_free(&unit);
    diagnostic_sink_destroy(&sink);
    free_options(&options);
    arena_destroy(arena);
    return good && errors == 0U ? 0 : 1;
}

int main(int argc, char **argv)
{
    return cc64_main(argc, argv);
}
