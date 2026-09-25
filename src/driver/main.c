#include "cc64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum Action {
    ACTION_COMPILE,
    ACTION_PREPROCESS,
    ACTION_VERSION,
    ACTION_HELP
} Action;

typedef struct Options {
    Action action;
    const char *input;
    const char *output;
    const char *target;
} Options;

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: cc64 [options] input.c\n"
            "  -c             compile and emit CC64O\n"
            "  -E             preprocess only\n"
            "  -o FILE        output path\n"
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

static bool parse_options(int argc, char **argv, Options *options,
                          DiagnosticSink *sink)
{
    options->action = ACTION_COMPILE;
    options->input = NULL;
    options->output = "a.o";
    options->target = CC64_TARGET;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-c") == 0) {
            options->action = ACTION_COMPILE;
        } else if (strcmp(arg, "-E") == 0) {
            options->action = ACTION_PREPROCESS;
        } else if (strcmp(arg, "-o") == 0) {
            if (!take_value(argc, argv, &i, &options->output)) {
                diagnostic_emit(sink, 1U, DIAG_DRIVER, NULL, 0U, 0U,
                                "option '-o' requires a path");
                return false;
            }
        } else if (strcmp(arg, "--target") == 0) {
            if (!take_value(argc, argv, &i, &options->target)) {
                diagnostic_emit(sink, 2U, DIAG_DRIVER, NULL, 0U, 0U,
                                "option '--target' requires a name");
                return false;
            }
        } else if (strcmp(arg, "--version") == 0) {
            options->action = ACTION_VERSION;
        } else if (strcmp(arg, "--help") == 0) {
            options->action = ACTION_HELP;
        } else if (arg[0] == '-') {
            char message[160];
            snprintf(message, sizeof(message), "unknown option '%s'", arg);
            diagnostic_emit(sink, 3U, DIAG_DRIVER, NULL, 0U, 0U, message);
            return false;
        } else if (options->input == NULL) {
            options->input = arg;
        } else {
            diagnostic_emit(sink, 4U, DIAG_DRIVER, NULL, 0U, 0U,
                            "only one input file is supported");
            return false;
        }
    }
    if ((options->action == ACTION_COMPILE || options->action == ACTION_PREPROCESS) &&
        options->input == NULL) {
        diagnostic_emit(sink, 5U, DIAG_DRIVER, NULL, 0U, 0U, "missing input file");
        return false;
    }
    return true;
}

int cc64_main(int argc, char **argv)
{
    DiagnosticSink sink = {0};
    sink.limit = 100U;
    Options options;
    if (!parse_options(argc, argv, &options, &sink)) {
        for (size_t i = 0U; i < sink.count; ++i) {
            diagnostic_print(&sink.items[i], stderr);
        }
        free(sink.items);
        return 1;
    }
    if (options.action == ACTION_VERSION) {
        printf("cc64 %s target=%s\n", CC64_VERSION, options.target);
        free(sink.items);
        return 0;
    }
    if (options.action == ACTION_HELP) {
        usage(stdout);
        free(sink.items);
        return 0;
    }

    Arena *arena = arena_create(256U * 1024U * 1024U);
    SourceManager *manager = source_manager_create(arena);
    if (manager == NULL) {
        diagnostic_emit(&sink, 10U, DIAG_DRIVER, NULL, 0U, 0U,
                        "source manager allocation failed");
        for (size_t i = 0U; i < sink.count; ++i) {
            diagnostic_print(&sink.items[i], stderr);
        }
        free(sink.items);
        arena_destroy(arena);
        return 2;
    }
    Source *source = source_manager_load(manager, options.input);
    if (source == NULL) {
        diagnostic_emit(&sink, 11U, DIAG_DRIVER, NULL, 0U, 0U,
                        "cannot read input file");
    } else if (options.target != NULL && strcmp(options.target, CC64_TARGET) != 0) {
        diagnostic_emit(&sink, 12U, DIAG_DRIVER, source, 1U, 1U,
                        "unsupported target contract");
    } else {
        printf("cc64: loaded %s (%zu bytes)\n", source->path, source->length);
    }
    for (size_t i = 0U; i < sink.count; ++i) {
        diagnostic_print(&sink.items[i], stderr);
    }
    size_t errors = sink.count;
    free(sink.items);
    arena_destroy(arena);
    return errors == 0U ? 0 : 1;
}

int main(int argc, char **argv)
{
    return cc64_main(argc, argv);
}
