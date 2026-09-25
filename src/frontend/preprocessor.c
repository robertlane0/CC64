#include "frontend/frontend.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CondState {
    bool parent_active;
    bool active;
    bool seen_true;
    size_t line;
} CondState;

typedef struct ExpandContext {
    Preprocessor *pp;
    const Source *source;
    unsigned depth;
    size_t invocation_line;
    bool has_invocation;
    Hideset *hideset;
} ExpandContext;

typedef struct ExprParser {
    const Token *tokens;
    size_t count;
    size_t position;
    Preprocessor *pp;
    bool failed;
} ExprParser;

typedef struct ExprValue {
    uint64_t bits;
    bool is_unsigned;
} ExprValue;

static void pp_emit(Preprocessor *pp, unsigned id, const Token *token,
                    const char *message)
{
    const Source *source = token == NULL ? NULL : token->source;
    size_t line = token == NULL ? 0U : token->line;
    size_t column = token == NULL ? 0U : token->column;
    diagnostic_emit(pp->diagnostics, id, DIAG_PREPROCESS, source, line,
                    column, message);
}

static void pp_emitf(Preprocessor *pp, unsigned id, const Token *token,
                     const char *format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    pp_emit(pp, id, token, message);
}

static bool token_is(const Token *token, const char *text)
{
    return token != NULL && strcmp(token->text, text) == 0;
}

static bool token_is_kind(const Token *token, TokenKind kind)
{
    return token != NULL && token->kind == kind;
}

static bool hideset_contains(const Hideset *set, const char *name)
{
    if (set == NULL) {
        return false;
    }
    for (size_t i = 0U; i < set->count; ++i) {
        if (strcmp(set->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static Hideset *hideset_add(const Hideset *set, const char *name)
{
    if (hideset_contains(set, name)) {
        if (set == NULL) {
            Hideset *created = cc64_xmalloc(sizeof(*created));
            created->count = 1U;
            created->names = cc64_xmalloc(sizeof(*created->names));
            created->names[0] = name;
            return created;
        }
        Hideset *copy = cc64_xmalloc(sizeof(*copy));
        copy->count = set->count;
        copy->names = cc64_xmalloc(copy->count * sizeof(*copy->names));
        memcpy(copy->names, set->names, copy->count * sizeof(*copy->names));
        return copy;
    }
    size_t count = set == NULL ? 1U : set->count + 1U;
    Hideset *created = cc64_xmalloc(sizeof(*created));
    created->count = count;
    created->names = cc64_xmalloc(count * sizeof(*created->names));
    if (set != NULL && set->count != 0U) {
        memcpy(created->names, set->names, set->count * sizeof(*created->names));
    }
    created->names[count - 1U] = name;
    return created;
}

static bool token_blocked_by_hide(const Token *token, const char *name)
{
    return name != NULL && token_is_kind(token, TOKEN_IDENTIFIER) &&
           strcmp(token->text, name) == 0;
}

static Hideset *hideset_union(const Hideset *left, const Hideset *right)
{
    Hideset *result = NULL;
    if (left != NULL) {
        result = cc64_xmalloc(sizeof(*result));
        result->count = left->count;
        result->names = cc64_xmalloc(result->count * sizeof(*result->names));
        memcpy(result->names, left->names, result->count * sizeof(*result->names));
    }
    if (right != NULL) {
        for (size_t i = 0U; i < right->count; ++i) {
            result = hideset_add(result, right->names[i]);
        }
    }
    return result;
}

static Macro *find_macro(const Preprocessor *pp, const Token *token)
{
    if (!token_is_kind(token, TOKEN_IDENTIFIER)) {
        return NULL;
    }
    for (Macro *macro = pp->macros; macro != NULL; macro = macro->next) {
        if (strcmp(macro->name, token->text) == 0) {
            return macro;
        }
    }
    return NULL;
}

static bool output_push(Preprocessor *pp, TokenList *output, const Token *token)
{
    if (pp->token_count >= CC64_PP_MAX_TOKENS ||
        !token_list_push(output, token)) {
        pp_emit(pp, 1101U, token, "preprocessor token limit exceeded");
        return false;
    }
    ++pp->token_count;
    return true;
}

static size_t skip_newlines(const Token *tokens, size_t count, size_t position)
{
    while (position < count && tokens[position].kind == TOKEN_NEWLINE) {
        ++position;
    }
    return position;
}

static void copy_token(Token *result, const Token *token)
{
    result->kind = token->kind;
    result->source = token->source;
    result->start = token->start;
    result->end = token->end;
    result->line = token->line;
    result->column = token->column;
    result->at_bol = token->at_bol;
    result->has_space = token->has_space;
    result->text = token->text;
    result->hideset = token->hideset;
}

static bool make_text_token(const Token *like, TokenKind kind, const char *text,
                            Token *result)
{
    copy_token(result, like);
    result->kind = kind;
    result->text = cc64_xstrdup(text);
    result->hideset = NULL;
    return true;
}

static bool expand_range(const Token *input, size_t begin, size_t end,
                         ExpandContext *context, TokenList *output,
                         const char *hide_name);

static bool is_parameter(const Macro *macro, const Token *token, size_t *index)
{
    if (!token_is_kind(token, TOKEN_IDENTIFIER)) {
        return false;
    }
    for (size_t i = 0U; i < macro->parameter_count; ++i) {
        if (strcmp(macro->parameters[i].name, token->text) == 0) {
            *index = i;
            return true;
        }
    }
    return false;
}

static bool parse_arguments(const Token *input, size_t count, size_t *position,
                            const Macro *macro, TokenList **arguments,
                            size_t *argument_count, Preprocessor *pp)
{
    size_t open = skip_newlines(input, count, *position);
    if (open >= count || !token_is(&input[open], "(")) {
        return false;
    }
    ++open;
    size_t capacity = macro->parameter_count == 0U ? 1U : macro->parameter_count;
    TokenList *lists = cc64_xmalloc(capacity * sizeof(*lists));
    for (size_t i = 0U; i < capacity; ++i) {
        token_list_init(&lists[i]);
    }
    size_t used = 0U;
    unsigned parenthesis = 0U;
    for (;;) {
        open = skip_newlines(input, count, open);
        if (open >= count) {
            for (size_t i = 0U; i < capacity; ++i) {
                token_list_free(&lists[i]);
            }
            free(lists);
            pp_emit(pp, 1102U, NULL, "unterminated macro argument list");
            return false;
        }
        if (token_is(&input[open], ")") && parenthesis == 0U) {
            ++open;
            break;
        }
        if (used == capacity) {
            if (!(macro->variadic && macro->parameter_count != 0U)) {
                for (size_t i = 0U; i < capacity; ++i) {
                    token_list_free(&lists[i]);
                }
                free(lists);
                pp_emit(pp, 1103U, &input[*position], "too many macro arguments");
                return false;
            }
            ++capacity;
            TokenList *grown = cc64_xrealloc(lists, capacity * sizeof(*lists));
            lists = grown;
            token_list_init(&lists[used]);
        }
        for (;;) {
            if (open < count && token_is(&input[open], ")") && parenthesis == 0U) {
                break;
            }
            if (open >= count) {
                for (size_t i = 0U; i < capacity; ++i) {
                    token_list_free(&lists[i]);
                }
                free(lists);
                pp_emit(pp, 1102U, NULL, "unterminated macro argument list");
                return false;
            }
            if (input[open].kind == TOKEN_NEWLINE) {
                ++open;
                continue;
            }
            if (token_is(&input[open], "(")) {
                ++parenthesis;
            } else if (token_is(&input[open], ")")) {
                if (parenthesis != 0U) {
                    --parenthesis;
                }
            } else if (token_is(&input[open], ",") && parenthesis == 0U &&
                       !(macro->variadic && used + 1U >= macro->parameter_count)) {
                ++open;
                break;
            }
            if (!token_list_push(&lists[used], &input[open])) {
                for (size_t i = 0U; i < capacity; ++i) {
                    token_list_free(&lists[i]);
                }
                free(lists);
                return false;
            }
            ++open;
        }
        ++used;
    }
    size_t required = macro->parameter_count;
    if (macro->variadic) {
        if (used + 1U < required) {
            for (size_t i = 0U; i < capacity; ++i) {
                token_list_free(&lists[i]);
            }
            free(lists);
            pp_emit(pp, 1104U, NULL, "too few macro arguments");
            return false;
        }
    } else if (used != required) {
        for (size_t i = 0U; i < capacity; ++i) {
            token_list_free(&lists[i]);
        }
        free(lists);
        const char *message = "too few macro arguments";
        if (macro->parameter_count == 0U) {
            message = "macro takes no arguments";
        }
        pp_emit(pp, 1104U, NULL, message);
        return false;
    }
    *arguments = lists;
    *argument_count = used;
    *position = open;
    return true;
}

static char *stringize_arguments(const TokenList *list)
{
    size_t length = 1U;
    for (size_t i = 0U; i < list->count; ++i) {
        length += strlen(list->items[i].text) + 1U;
    }
    char *text = cc64_xmalloc(length + 1U);
    size_t used = 0U;
    text[used++] = '"';
    for (size_t i = 0U; i < list->count; ++i) {
        const char *value = list->items[i].text;
        if (i != 0U && list->items[i].has_space) {
            text[used++] = ' ';
        }
        for (size_t j = 0U; value[j] != '\0'; ++j) {
            if (value[j] == '"' || value[j] == '\\') {
                text[used++] = '\\';
            }
            text[used++] = value[j];
        }
    }
    text[used++] = '"';
    text[used] = '\0';
    return text;
}

static bool append_paste(TokenList *list, const Token *right)
{
    Token *left = token_list_last(list);
    if (left == NULL) {
        return token_list_push(list, right);
    }
    size_t left_length = strlen(left->text);
    size_t right_length = strlen(right->text);
    char *joined = cc64_xmalloc(left_length + right_length + 1U);
    memcpy(joined, left->text, left_length);
    memcpy(joined + left_length, right->text, right_length + 1U);
    left->text = joined;
    left->end = right->end;
    if (left->kind != right->kind) {
        left->kind = right->kind;
    }
    return true;
}

static bool expand_function_body(const Macro *macro, const Token *body,
                                  size_t body_count, TokenList *arguments,
                                  size_t argument_count, ExpandContext *context,
                                  TokenList *output)
{
    TokenList substituted;
    token_list_init(&substituted);
    for (size_t i = 0U; i < body_count; ++i) {
        const Token *token = &body[i];
        size_t parameter = 0U;
        if (token_is(token, "#") && i + 1U < body_count &&
            is_parameter(macro, &body[i + 1U], &parameter)) {
            Token string;
            char *string_text = stringize_arguments(&arguments[parameter]);
            (void)make_text_token(token, TOKEN_STRING, string_text, &string);
            free(string_text);
            if (!token_list_push(&substituted, &string)) {
                token_list_free(&substituted);
                return false;
            }
            ++i;
            continue;
        }
        if (is_parameter(macro, token, &parameter)) {
            if (parameter < argument_count) {
                for (size_t j = 0U; j < arguments[parameter].count; ++j) {
                    if (!token_list_push(&substituted, &arguments[parameter].items[j])) {
                        token_list_free(&substituted);
                        return false;
                    }
                }
            }
        } else if (!token_is(token, "##")) {
            if (!token_list_push(&substituted, token)) {
                token_list_free(&substituted);
                return false;
            }
        }
        if (i + 1U < body_count && token_is(&body[i + 1U], "##")) {
            ++i;
            if (i + 1U >= body_count) {
                continue;
            }
            const Token *right = &body[++i];
            size_t right_parameter = 0U;
            if (is_parameter(macro, right, &right_parameter)) {
                if (right_parameter < argument_count &&
                    arguments[right_parameter].count != 0U) {
                    append_paste(&substituted, &arguments[right_parameter].items[0]);
                    for (size_t j = 1U; j < arguments[right_parameter].count; ++j) {
                        if (!token_list_push(&substituted,
                                              &arguments[right_parameter].items[j])) {
                            token_list_free(&substituted);
                            return false;
                        }
                    }
                }
            } else if (!append_paste(&substituted, right)) {
                token_list_free(&substituted);
                return false;
            }
        }
    }
    bool good = expand_range(substituted.items, 0U, substituted.count, context,
                             output, macro->name);
    token_list_free(&substituted);
    (void)argument_count;
    return good;
}

static bool predefined_token(ExpandContext *context, const Token *token,
                             Token *result)
{
    if (token_is(token, "__FILE__")) {
        const char *path = context->pp->display_path == NULL
                               ? (context->source == NULL ? "<unknown>" : context->source->path)
                               : context->pp->display_path;
        size_t length = strlen(path);
        char *text = cc64_xmalloc(length + 3U);
        text[0] = '"';
        memcpy(text + 1U, path, length);
        text[length + 1U] = '"';
        text[length + 2U] = '\0';
        bool good = make_text_token(token, TOKEN_STRING, text, result);
        free(text);
        return good;
    }
    if (token_is(token, "__LINE__")) {
        char text[32];
        size_t source_line = context->has_invocation ? context->invocation_line
                                                      : token->line;
        int64_t logical_line = (int64_t)source_line + context->pp->line_delta;
        if (logical_line < 0) {
            logical_line = 0;
        }
        (void)snprintf(text, sizeof(text), "%lld", (long long)logical_line);
        return make_text_token(token, TOKEN_NUMBER, text, result);
    }
    copy_token(result, token);
    return true;
}

static void expansion_context(const ExpandContext *context, const Token *token,
                              ExpandContext *nested)
{
    nested->pp = context->pp;
    nested->source = context->source;
    nested->depth = context->depth + 1U;
    nested->invocation_line = context->invocation_line;
    nested->has_invocation = context->has_invocation;
    nested->hideset = context->hideset;
    if (!nested->has_invocation) {
        nested->invocation_line = token->line;
        nested->has_invocation = true;
    }
    if (token_is_kind(token, TOKEN_IDENTIFIER)) {
        nested->hideset = hideset_add(nested->hideset, token->text);
    }
}

static bool expand_range(const Token *input, size_t begin, size_t end,
                         ExpandContext *context, TokenList *output,
                         const char *hide_name)
{
    Preprocessor *pp = context->pp;
    if (context->depth >= CC64_PP_MAX_MACRO_DEPTH) {
        pp_emit(pp, 1105U, begin < end ? &input[begin] : NULL,
                "macro expansion depth exceeded");
        return false;
    }
    size_t position = begin;
    while (position < end) {
        const Token *token = &input[position];
        if (token->kind == TOKEN_NEWLINE) {
            if (pp->options.preserve_newlines &&
                !token_list_push(output, token)) {
                return false;
            }
            ++position;
            continue;
        }
        if (token_is(token, "__FILE__") || token_is(token, "__LINE__")) {
            if (hide_name != NULL && hideset_contains(token->hideset, hide_name)) {
                if (!output_push(pp, output, token)) {
                    return false;
                }
            } else {
                Token predefined;
                if (!predefined_token(context, token, &predefined) ||
                    !output_push(pp, output, &predefined)) {
                    return false;
                }
            }
            ++position;
            continue;
        }
        Macro *macro = find_macro(pp, token);
        if (macro != NULL && !hideset_contains(token->hideset, macro->name) &&
            !hideset_contains(context->hideset, macro->name) &&
            (hide_name == NULL || !hideset_contains(token->hideset, hide_name)) &&
            !token_blocked_by_hide(token, hide_name)) {
            if (!macro->function_like) {
                ExpandContext nested;
                expansion_context(context, token, &nested);
                size_t body_end = macro->body_count;
                if (!expand_range(macro->body, 0U, body_end, &nested, output,
                                  macro->name)) {
                    return false;
                }
                ++position;
                continue;
            }
            size_t next = skip_newlines(input, end, position + 1U);
            if (next < end && token_is(&input[next], "(")) {
                TokenList *arguments = NULL;
                size_t argument_count = 0U;
                size_t after = position + 1U;
                if (!parse_arguments(input, end, &after, macro, &arguments,
                                     &argument_count, pp)) {
                    return false;
                }
                ExpandContext nested;
                expansion_context(context, token, &nested);
                bool good = expand_function_body(macro, macro->body,
                                                 macro->body_count, arguments,
                                                 argument_count, &nested, output);
                for (size_t i = 0U; i < argument_count; ++i) {
                    token_list_free(&arguments[i]);
                }
                free(arguments);
                if (!good) {
                    return false;
                }
                position = after;
                continue;
            }
        }
        Token copy;
        copy_token(&copy, token);
        if (hide_name != NULL) {
            copy.hideset = hideset_union(token->hideset, NULL);
            copy.hideset = hideset_add(copy.hideset, hide_name);
        }
        if (!output_push(pp, output, &copy)) {
            return false;
        }
        ++position;
    }
    return true;
}

static bool expand_macro_at(const Token *input, size_t count, size_t *position,
                            ExpandContext *context, TokenList *output)
{
    const Token *token = &input[*position];
    if (token->kind == TOKEN_NEWLINE) {
        if (context->pp->options.preserve_newlines) {
            return token_list_push(output, token);
        }
        ++*position;
        return true;
    }
    if (token_is(token, "__FILE__") || token_is(token, "__LINE__")) {
        ++*position;
        Token predefined;
        return predefined_token(context, token, &predefined) &&
               output_push(context->pp, output, &predefined);
    }
    Macro *macro = find_macro(context->pp, token);
    if (macro == NULL || hideset_contains(token->hideset, macro->name)) {
        ++*position;
        return output_push(context->pp, output, token);
    }
    if (!macro->function_like) {
        ++*position;
        ExpandContext nested;
        expansion_context(context, token, &nested);
        return expand_range(macro->body, 0U, macro->body_count, &nested,
                            output, macro->name);
    }
    size_t next = skip_newlines(input, count, *position + 1U);
    if (next >= count || !token_is(&input[next], "(")) {
        ++*position;
        return output_push(context->pp, output, token);
    }
    TokenList *arguments = NULL;
    size_t argument_count = 0U;
    size_t after = *position + 1U;
    if (!parse_arguments(input, count, &after, macro, &arguments,
                         &argument_count, context->pp)) {
        return false;
    }
    ExpandContext nested;
    expansion_context(context, token, &nested);
    bool good = expand_function_body(macro, macro->body, macro->body_count,
                                     arguments, argument_count, &nested, output);
    for (size_t i = 0U; i < argument_count; ++i) {
        token_list_free(&arguments[i]);
    }
    free(arguments);
    if (good) {
        *position = after;
    }
    return good;
}

static bool parse_macro_definition(Preprocessor *pp, const Token *body,
                                   size_t count, const Token *directive)
{
    if (count == 0U || !token_is_kind(&body[0], TOKEN_IDENTIFIER)) {
        pp_emit(pp, 1110U, directive, "macro name must be an identifier");
        return false;
    }
    Macro *macro = arena_alloc(pp->arena, sizeof(*macro));
    if (macro == NULL) {
        return false;
    }
    macro->name = cc64_xstrdup(body[0].text);
    macro->function_like = false;
    macro->variadic = false;
    macro->parameters = NULL;
    macro->parameter_count = 0U;
    macro->body = NULL;
    macro->body_count = 0U;
    macro->next = pp->macros;
    size_t position = 1U;
    if (position < count && token_is(&body[position], "(") &&
        !body[position].has_space) {
        macro->function_like = true;
        ++position;
        while (position < count && !token_is(&body[position], ")")) {
            if (token_is(&body[position], "...")) {
                macro->variadic = true;
                MacroParameter *parameter = arena_alloc(pp->arena, sizeof(*parameter));
                if (parameter == NULL) {
                    return false;
                }
                parameter->name = cc64_xstrdup("__VA_ARGS__");
                MacroParameter *grown = arena_alloc_array(
                    pp->arena, macro->parameter_count + 1U, sizeof(*grown));
                if (grown == NULL) {
                    return false;
                }
                for (size_t i = 0U; i < macro->parameter_count; ++i) {
                    grown[i] = macro->parameters[i];
                }
                grown[macro->parameter_count] = *parameter;
                macro->parameters = grown;
                ++macro->parameter_count;
                ++position;
                if (position < count && token_is(&body[position], ",")) {
                    ++position;
                }
                continue;
            }
            if (!token_is_kind(&body[position], TOKEN_IDENTIFIER)) {
                pp_emit(pp, 1111U, &body[position],
                        "macro parameter must be an identifier");
                return false;
            }
            MacroParameter parameter = {cc64_xstrdup(body[position].text)};
            MacroParameter *grown = arena_alloc_array(
                pp->arena, macro->parameter_count + 1U, sizeof(*grown));
            if (grown == NULL) {
                return false;
            }
            for (size_t i = 0U; i < macro->parameter_count; ++i) {
                grown[i] = macro->parameters[i];
            }
            grown[macro->parameter_count] = parameter;
            macro->parameters = grown;
            ++macro->parameter_count;
            ++position;
            if (position < count && token_is(&body[position], ",")) {
                ++position;
                continue;
            }
            if (position >= count || !token_is(&body[position], ")")) {
                pp_emit(pp, 1112U, directive, "expected ')' in macro parameters");
                return false;
            }
        }
        if (position >= count || !token_is(&body[position], ")")) {
            pp_emit(pp, 1112U, directive, "expected ')' in macro parameters");
            return false;
        }
        ++position;
    }
    macro->body_count = count - position;
    if (macro->body_count != 0U) {
        macro->body = arena_alloc_array(pp->arena, macro->body_count,
                                        sizeof(*macro->body));
        if (macro->body == NULL) {
            return false;
        }
        memcpy(macro->body, body + position,
               macro->body_count * sizeof(*macro->body));
    }
    for (Macro *old = pp->macros; old != NULL; old = old->next) {
        if (strcmp(old->name, macro->name) == 0) {
            Macro *next = old->next;
            *old = *macro;
            old->next = next;
            return true;
        }
    }
    pp->macros = macro;
    return true;
}

static bool source_path_join(const char *directory, const char *name,
                             char **result)
{
    if (name[0] == '/') {
        *result = cc64_xstrdup(name);
        return true;
    }
    size_t directory_length = strlen(directory);
    bool slash = directory_length != 0U && directory[directory_length - 1U] == '/';
    size_t length = directory_length + (slash ? 0U : 1U) + strlen(name) + 1U;
    char *joined = cc64_xmalloc(length);
    const char *separator = "/";
    if (slash) {
        separator = "";
    }
    (void)snprintf(joined, length, "%s%s%s", directory, separator, name);
    *result = joined;
    return true;
}

static char *source_directory(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return cc64_xstrdup(".");
    }
    size_t length = (size_t)(slash - path);
    if (length == 0U) {
        return cc64_xstrdup("/");
    }
    char *directory = cc64_xmalloc(length + 1U);
    memcpy(directory, path, length);
    directory[length] = '\0';
    return directory;
}

static char *include_text(const Token *body, size_t count, bool *angled,
                          Preprocessor *pp, const Token *directive)
{
    *angled = false;
    char *name = NULL;
    if (count == 0U) {
        pp_emit(pp, 1120U, directive, "#include expects a file name");
        return NULL;
    }
    if (token_is_kind(&body[0], TOKEN_STRING) && body[0].text[0] == '"') {
        size_t length = strlen(body[0].text);
        if (length < 2U) {
            return NULL;
        }
        name = cc64_xmalloc(length - 1U);
        memcpy(name, body[0].text + 1U, length - 2U);
        name[length - 2U] = '\0';
        return name;
    }
    if (token_is(&body[0], "<")) {
        *angled = true;
        size_t capacity = 64U;
        size_t length = 0U;
        name = cc64_xmalloc(capacity);
        for (size_t i = 1U; i < count; ++i) {
            if (token_is(&body[i], ">")) {
                name[length] = '\0';
                return name;
            }
            size_t part = strlen(body[i].text);
            if (length + part + 2U > capacity) {
                while (length + part + 2U > capacity) {
                    capacity *= 2U;
                }
                name = cc64_xrealloc(name, capacity);
            }
            memcpy(name + length, body[i].text, part);
            length += part;
        }
    }
    pp_emit(pp, 1120U, directive, "malformed #include file name");
    free(name);
    return NULL;
}

static bool is_active(const CondState *stack, size_t count)
{
    return count == 0U || stack[count - 1U].active;
}

static bool evaluate_expression(Preprocessor *pp, const Token *body,
                                size_t count, const Token *directive);

static bool process_source(Preprocessor *pp, const Source *source,
                           TokenList *output);

static bool process_include(Preprocessor *pp, const Source *source,
                            const Token *body, size_t count,
                            const Token *directive, TokenList *output)
{
    bool angled = false;
    char *name = include_text(body, count, &angled, pp, directive);
    if (name == NULL) {
        return false;
    }
    if (pp->include_stack_count >= CC64_PP_MAX_INCLUDE_DEPTH) {
        pp_emit(pp, 1121U, directive, "include depth limit exceeded");
        free(name);
        return false;
    }
    char *directory = angled ? cc64_xstrdup(".") : source_directory(source->path);
    char *candidate = NULL;
    Source *included = NULL;
    (void)source_path_join(directory, name, &candidate);
    included = source_manager_load(pp->sources, candidate);
    free(candidate);
    if (included == NULL && !angled) {
        free(directory);
        directory = cc64_xstrdup(".");
        (void)source_path_join(directory, name, &candidate);
        included = source_manager_load(pp->sources, candidate);
        free(candidate);
    }
    for (size_t i = 0U; included == NULL && i < pp->options.include_path_count; ++i) {
        (void)source_path_join(pp->options.include_paths[i], name, &candidate);
        included = source_manager_load(pp->sources, candidate);
        free(candidate);
    }
    free(directory);
    free(name);
    if (included == NULL) {
        pp_emitf(pp, 1122U, directive, "cannot open include file");
        return false;
    }
    for (size_t i = 0U; i < pp->include_stack_count; ++i) {
        if (strcmp(pp->include_stack[i], included->path) == 0) {
            pp_emit(pp, 1123U, directive, "include cycle detected");
            return false;
        }
    }
    const char **stack = cc64_xrealloc(
        (void *)pp->include_stack,
        (pp->include_stack_count + 1U) * sizeof(*stack));
    pp->include_stack = stack;
    pp->include_stack[pp->include_stack_count++] = included->path;
    bool good = process_source(pp, included, output);
    if (good && pp->options.preserve_newlines &&
        (output->count == 0U || token_list_last(output)->kind != TOKEN_NEWLINE)) {
        Token newline = {0};
        if (output->count != 0U) {
            copy_token(&newline, token_list_last(output));
        }
        newline.kind = TOKEN_NEWLINE;
        newline.source = included;
        newline.line = 1U;
        newline.column = 1U;
        newline.text = cc64_xstrdup("\n");
        newline.hideset = NULL;
        good = output_push(pp, output, &newline);
    }
    --pp->include_stack_count;
    return good;
}

static bool process_directive(Preprocessor *pp, const Source *source,
                              const Token *line, size_t count,
                              size_t directive_line, CondState **conditions,
                              size_t *condition_count, size_t *condition_capacity,
                              TokenList *output)
{
    const Token *directive = count == 0U ? NULL : &line[0];
    if (directive == NULL || directive->kind == TOKEN_NEWLINE) {
        return true;
    }
    const char *name = directive->text;
    bool active = is_active(*conditions, *condition_count);
    if (strcmp(name, "if") == 0) {
        bool value = false;
        if (active) {
            value = evaluate_expression(pp, line + 1U, count - 1U, directive);
        }
        if (*condition_count == *condition_capacity) {
            size_t next = *condition_capacity == 0U ? 8U : *condition_capacity * 2U;
            CondState *grown = cc64_xrealloc(*conditions, next * sizeof(*grown));
            *conditions = grown;
            *condition_capacity = next;
        }
        CondState *state = &(*conditions)[(*condition_count)++];
        state->parent_active = active;
        state->active = active && value;
        state->seen_true = state->active;
        state->line = directive_line;
        return true;
    }
    if (strcmp(name, "ifdef") == 0 || strcmp(name, "ifndef") == 0) {
        bool defined = count > 1U && find_macro(pp, &line[1]) != NULL;
        bool value = strcmp(name, "ifdef") == 0 ? defined : !defined;
        if (*condition_count == *condition_capacity) {
            size_t next = *condition_capacity == 0U ? 8U : *condition_capacity * 2U;
            CondState *grown = cc64_xrealloc(*conditions, next * sizeof(*grown));
            *conditions = grown;
            *condition_capacity = next;
        }
        CondState *state = &(*conditions)[(*condition_count)++];
        state->parent_active = active;
        state->active = active && value;
        state->seen_true = state->active;
        state->line = directive_line;
        return true;
    }
    if (strcmp(name, "elif") == 0) {
        if (*condition_count == 0U) {
            pp_emit(pp, 1124U, directive, "#elif without #if");
            return false;
        }
        CondState *state = &(*conditions)[*condition_count - 1U];
        if (state->seen_true || !state->parent_active) {
            state->active = false;
        } else {
            state->active = evaluate_expression(pp, line + 1U, count - 1U,
                                                 directive);
            state->seen_true = state->active;
        }
        return true;
    }
    if (strcmp(name, "else") == 0) {
        if (*condition_count == 0U) {
            pp_emit(pp, 1125U, directive, "#else without #if");
            return false;
        }
        CondState *state = &(*conditions)[*condition_count - 1U];
        if (state->seen_true) {
            state->active = false;
        } else {
            state->active = state->parent_active;
            state->seen_true = true;
        }
        return true;
    }
    if (strcmp(name, "endif") == 0) {
        if (*condition_count == 0U) {
            pp_emit(pp, 1126U, directive, "#endif without #if");
            return false;
        }
        --*condition_count;
        return true;
    }
    if (!active) {
        return true;
    }
    if (strcmp(name, "define") == 0) {
        return parse_macro_definition(pp, line + 1U, count - 1U, directive);
    }
    if (strcmp(name, "undef") == 0) {
        if (count < 2U || !token_is_kind(&line[1], TOKEN_IDENTIFIER)) {
            pp_emit(pp, 1127U, directive, "#undef expects a macro name");
            return false;
        }
        Macro **link = &pp->macros;
        while (*link != NULL && strcmp((*link)->name, line[1].text) != 0) {
            link = &(*link)->next;
        }
        if (*link != NULL) {
            *link = (*link)->next;
        }
        return true;
    }
    if (strcmp(name, "include") == 0) {
        TokenList expanded;
        token_list_init(&expanded);
        ExpandContext context = {pp, source, 0U, 0U, false, NULL};
        if (!expand_range(line + 1U, 0U, count - 1U, &context, &expanded,
                          NULL)) {
            token_list_free(&expanded);
            return false;
        }
        bool good = process_include(pp, source, expanded.items, expanded.count,
                                    directive, output);
        token_list_free(&expanded);
        return good;
    }
    if (strcmp(name, "error") == 0) {
        size_t length = 1U;
        for (size_t i = 1U; i < count; ++i) {
            length += strlen(line[i].text) + (i == 1U ? 0U : 1U);
        }
        char *message = cc64_xmalloc(length);
        size_t used = 0U;
        for (size_t i = 1U; i < count; ++i) {
            size_t part = strlen(line[i].text);
            if (i != 1U) {
                message[used++] = ' ';
            }
            memcpy(message + used, line[i].text, part);
            used += part;
        }
        message[used] = '\0';
        pp_emit(pp, 1128U, directive, message);
        free(message);
        return false;
    }
    if (strcmp(name, "line") == 0) {
        if (count < 2U || !token_is_kind(&line[1], TOKEN_NUMBER)) {
            pp_emit(pp, 1129U, directive, "#line expects a line number");
            return false;
        }
        char *end = NULL;
        unsigned long number = strtoul(line[1].text, &end, 10);
        if (end == line[1].text || *end != '\0' || number == 0UL) {
            pp_emit(pp, 1129U, directive, "invalid #line number");
            return false;
        }
        if (number > (unsigned long)INT64_MAX) {
            pp_emit(pp, 1129U, directive, "#line number is too large");
            return false;
        }
        pp->line_delta = (int64_t)number - (int64_t)directive_line - 1LL;
        if (count >= 3U && token_is_kind(&line[2], TOKEN_STRING) &&
            line[2].text[0] == '"') {
            size_t length = strlen(line[2].text);
            char *path = cc64_xmalloc(length - 1U);
            memcpy(path, line[2].text + 1U, length - 2U);
            path[length - 2U] = '\0';
            pp->display_path = path;
        }
        return true;
    }
    if (strcmp(name, "pragma") == 0 || strcmp(name, "ident") == 0 ||
        strcmp(name, "sccs") == 0) {
        return true;
    }
    pp_emitf(pp, 1130U, directive, "unknown preprocessing directive '%s'", name);
    return false;
}

static bool process_source(Preprocessor *pp, const Source *source,
                           TokenList *output)
{
    TokenList raw;
    if (!lex_source(pp->arena, source, pp->diagnostics, &raw)) {
        return false;
    }
    CondState *conditions = NULL;
    size_t condition_count = 0U;
    size_t condition_capacity = 0U;
    size_t position = 0U;
    bool good = true;
    const char *saved_path = pp->display_path;
    int64_t saved_line_delta = pp->line_delta;
    pp->display_path = source->path;
    pp->line_delta = 0U;
    while (position < raw.count && good) {
        const Token *token = &raw.items[position];
        if (token->kind == TOKEN_NEWLINE) {
            if (is_active(conditions, condition_count) &&
                pp->options.preserve_newlines) {
                good = output_push(pp, output, token);
            }
            ++position;
            continue;
        }
        if (token->at_bol && token_is(token, "#")) {
            size_t begin = position + 1U;
            size_t end = begin;
            while (end < raw.count && raw.items[end].kind != TOKEN_NEWLINE) {
                ++end;
            }
            if (end > begin && !is_active(conditions, condition_count) &&
                !token_is(&raw.items[begin], "if") &&
                !token_is(&raw.items[begin], "ifdef") &&
                !token_is(&raw.items[begin], "ifndef") &&
                !token_is(&raw.items[begin], "elif") &&
                !token_is(&raw.items[begin], "else") &&
                !token_is(&raw.items[begin], "endif")) {
                position = end;
                continue;
            }
            good = process_directive(pp, source, raw.items + begin, end - begin,
                                     token->line, &conditions, &condition_count,
                                     &condition_capacity, output);
            position = end < raw.count ? end + 1U : end;
            continue;
        }
        if (!is_active(conditions, condition_count)) {
            ++position;
            continue;
        }
        ExpandContext context = {pp, source, 0U, 0U, false, NULL};
        good = expand_macro_at(raw.items, raw.count, &position, &context, output);
    }
    if (condition_count != 0U) {
        diagnostic_emit(pp->diagnostics, 1131U, DIAG_PREPROCESS, source, 1U,
                        1U, "unterminated conditional directive");
        good = false;
    }
    pp->display_path = saved_path;
    pp->line_delta = saved_line_delta;
    free(conditions);
    token_list_free(&raw);
    return good;
}

static uint64_t parse_integer_token(const Token *token, bool *is_unsigned,
                                    bool *valid)
{
    const char *text = token->text;
    char *end = NULL;
    errno = 0;
    unsigned base = 10U;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16U;
    } else if (text[0] == '0' && text[1] != '\0') {
        base = 8U;
    }
    uint64_t value = strtoull(text, &end, (int)base);
    *is_unsigned = false;
    while (*end != '\0') {
        if (*end == 'u' || *end == 'U') {
            *is_unsigned = true;
        } else if (*end != 'l' && *end != 'L') {
            *valid = false;
            return 0U;
        }
        ++end;
    }
    *valid = errno != ERANGE && end != text;
    return value;
}

static bool expr_make(uint64_t bits, bool is_unsigned, ExprValue *result)
{
    result->bits = bits;
    result->is_unsigned = is_unsigned;
    return true;
}

static void expr_copy(ExprValue *result, const ExprValue *value)
{
    result->bits = value->bits;
    result->is_unsigned = value->is_unsigned;
}

static bool expr_binary(const char *op, const ExprValue *left,
                        const ExprValue *right, bool *valid, ExprValue *result)
{
    bool result_unsigned = left->is_unsigned || right->is_unsigned;
    uint64_t left_bits = left->bits;
    uint64_t right_bits = right->bits;
    if (!result_unsigned) {
        int64_t signed_left = (int64_t)left_bits;
        int64_t signed_right = (int64_t)right_bits;
        if (strcmp(op, "+") == 0) return expr_make((uint64_t)(signed_left + signed_right), false, result);
        if (strcmp(op, "-") == 0) return expr_make((uint64_t)(signed_left - signed_right), false, result);
        if (strcmp(op, "*") == 0) return expr_make((uint64_t)(signed_left * signed_right), false, result);
        if (strcmp(op, "/") == 0) {
            if (signed_right == 0) { *valid = false; return expr_make(0U, false, result); }
            return expr_make((uint64_t)(signed_left / signed_right), false, result);
        }
        if (strcmp(op, "%") == 0) {
            if (signed_right == 0) { *valid = false; return expr_make(0U, false, result); }
            return expr_make((uint64_t)(signed_left % signed_right), false, result);
        }
        if (strcmp(op, "<") == 0) return expr_make((uint64_t)(signed_left < signed_right), false, result);
        if (strcmp(op, ">") == 0) return expr_make((uint64_t)(signed_left > signed_right), false, result);
        if (strcmp(op, "<=") == 0) return expr_make((uint64_t)(signed_left <= signed_right), false, result);
        if (strcmp(op, ">=") == 0) return expr_make((uint64_t)(signed_left >= signed_right), false, result);
        if (strcmp(op, "<<") == 0) return expr_make((uint64_t)(signed_left << (signed_right & 63)), false, result);
        if (strcmp(op, ">>") == 0) return expr_make((uint64_t)(signed_left >> (signed_right & 63)), false, result);
    } else {
        if (strcmp(op, "+") == 0) return expr_make(left_bits + right_bits, true, result);
        if (strcmp(op, "-") == 0) return expr_make(left_bits - right_bits, true, result);
        if (strcmp(op, "*") == 0) return expr_make(left_bits * right_bits, true, result);
        if (strcmp(op, "/") == 0) {
            if (right_bits == 0U) { *valid = false; return expr_make(0U, true, result); }
            return expr_make(left_bits / right_bits, true, result);
        }
        if (strcmp(op, "%") == 0) {
            if (right_bits == 0U) { *valid = false; return expr_make(0U, true, result); }
            return expr_make(left_bits % right_bits, true, result);
        }
        if (strcmp(op, "<") == 0) return expr_make((uint64_t)(left_bits < right_bits), false, result);
        if (strcmp(op, ">") == 0) return expr_make((uint64_t)(left_bits > right_bits), false, result);
        if (strcmp(op, "<=") == 0) return expr_make((uint64_t)(left_bits <= right_bits), false, result);
        if (strcmp(op, ">=") == 0) return expr_make((uint64_t)(left_bits >= right_bits), false, result);
    }
    if (strcmp(op, "==") == 0) return expr_make((uint64_t)(left_bits == right_bits), false, result);
    if (strcmp(op, "!=") == 0) return expr_make((uint64_t)(left_bits != right_bits), false, result);
    if (strcmp(op, "&") == 0) return expr_make(left_bits & right_bits, result_unsigned, result);
    if (strcmp(op, "|") == 0) return expr_make(left_bits | right_bits, result_unsigned, result);
    if (strcmp(op, "^") == 0) return expr_make(left_bits ^ right_bits, result_unsigned, result);
    if (strcmp(op, "<<") == 0) return expr_make(left_bits << (right_bits & 63U), result_unsigned, result);
    if (strcmp(op, ">>") == 0) return expr_make(left_bits >> (right_bits & 63U), result_unsigned, result);
    if (strcmp(op, "&&") == 0) return expr_make((uint64_t)(left_bits != 0U && right_bits != 0U), false, result);
    if (strcmp(op, "||") == 0) return expr_make((uint64_t)(left_bits != 0U || right_bits != 0U), false, result);
    *valid = false;
    return expr_make(0U, false, result);
}

static bool expr_parse_binary(ExprParser *parser, unsigned precedence,
                              ExprValue *result);

static bool decode_character(const char **cursor, uint64_t *value)
{
    const char *p = *cursor;
    if (*p != '\\') {
        *value = (unsigned char)*p++;
        *cursor = p;
        return true;
    }
    ++p;
    if (*p == '\0') {
        return false;
    }
    unsigned char escaped = (unsigned char)*p++;
    switch (escaped) {
    case 'a': *value = 7U; break;
    case 'b': *value = 8U; break;
    case 'f': *value = 12U; break;
    case 'n': *value = 10U; break;
    case 'r': *value = 13U; break;
    case 't': *value = 9U; break;
    case 'v': *value = 11U; break;
    case '\\': *value = '\\'; break;
    case '\'': *value = '\''; break;
    case '"': *value = '"'; break;
    case '?': *value = '?'; break;
    case 'x': {
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
        unsigned int result = 0U;
        unsigned int digits = 0U;
        while (isxdigit((unsigned char)*p)) {
            unsigned char digit = (unsigned char)*p++;
            unsigned int numeric = digit <= (unsigned char)'9'
                                        ? (unsigned int)(digit - (unsigned char)'0')
                                        : (unsigned int)(digit <= (unsigned char)'F'
                                                             ? digit - (unsigned char)'A' + 10U
                                                             : digit - (unsigned char)'a' + 10U);
            result = result * 16U + numeric;
            ++digits;
        }
        if (digits == 0U || digits > 2U) {
            return false;
        }
        *value = result;
        break;
    }
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
        unsigned int result = (unsigned int)(escaped - '0');
        unsigned int digits = 1U;
        while (digits < 3U && *p >= '0' && *p <= '7') {
            result = result * 8U + (unsigned int)(*p++ - '0');
            ++digits;
        }
        *value = result;
        break;
    }
    default:
        *value = escaped;
        break;
    }
    *cursor = p;
    return true;
}

static bool expr_parse_unary(ExprParser *parser, ExprValue *result)
{
    if (parser->position >= parser->count) {
        parser->failed = true;
        return expr_make(0U, false, result);
    }
    const Token *token = &parser->tokens[parser->position++];
    if (token_is(token, "!")) {
        ExprValue value;
        if (!expr_parse_unary(parser, &value)) return false;
        return expr_make((uint64_t)(value.bits == 0U), false, result);
    }
    if (token_is(token, "~")) {
        ExprValue value;
        if (!expr_parse_unary(parser, &value)) return false;
        value.bits = ~value.bits;
        expr_copy(result, &value);
        return true;
    }
    if (token_is(token, "+")) {
        return expr_parse_unary(parser, result);
    }
    if (token_is(token, "-")) {
        ExprValue value;
        if (!expr_parse_unary(parser, &value)) return false;
        value.bits = (uint64_t)0 - value.bits;
        expr_copy(result, &value);
        return true;
    }
    if (token_is(token, "(")) {
        ExprValue value;
        if (!expr_parse_binary(parser, 0U, &value)) {
            expr_copy(result, &value);
            return false;
        }
        if (parser->position >= parser->count ||
            !token_is(&parser->tokens[parser->position], ")")) {
            parser->failed = true;
        } else {
            ++parser->position;
        }
        expr_copy(result, &value);
        return !parser->failed;
    }
    if (token->kind == TOKEN_NUMBER) {
        bool is_unsigned = false;
        bool valid = false;
        uint64_t value = parse_integer_token(token, &is_unsigned, &valid);
        if (!valid) {
            parser->failed = true;
        }
        return expr_make(value, is_unsigned, result);
    }
    if (token->kind == TOKEN_CHARACTER) {
        const char *cursor = token->text;
        while (*cursor != '\0' && *cursor != '\'') {
            ++cursor;
        }
        if (*cursor == '\'') {
            ++cursor;
        }
        if (*cursor == '\0' || *cursor == '\'') {
            parser->failed = true;
            return expr_make(0U, false, result);
        }
        uint64_t value = 0U;
        if (!decode_character(&cursor, &value)) {
            parser->failed = true;
            return expr_make(0U, false, result);
        }
        return expr_make(value, false, result);
    }
    if (token->kind == TOKEN_IDENTIFIER || token->kind == TOKEN_KEYWORD) {
        return expr_make(0U, false, result);
    }
    parser->failed = true;
    return expr_make(0U, false, result);
}

static unsigned binary_precedence(const Token *token)
{
    if (token_is(token, "||")) return 1U;
    if (token_is(token, "&&")) return 2U;
    if (token_is(token, "|")) return 3U;
    if (token_is(token, "^")) return 4U;
    if (token_is(token, "&")) return 5U;
    if (token_is(token, "==") || token_is(token, "!=")) return 6U;
    if (token_is(token, "<") || token_is(token, ">") ||
        token_is(token, "<=") || token_is(token, ">=")) return 7U;
    if (token_is(token, "<<") || token_is(token, ">>")) return 8U;
    if (token_is(token, "+") || token_is(token, "-")) return 9U;
    if (token_is(token, "*") || token_is(token, "/") || token_is(token, "%")) return 10U;
    return 0U;
}

static bool expr_parse_binary(ExprParser *parser, unsigned minimum,
                              ExprValue *result)
{
    ExprValue left;
    if (!expr_parse_unary(parser, &left)) {
        expr_copy(result, &left);
        return false;
    }
    while (!parser->failed && parser->position < parser->count) {
        unsigned precedence = binary_precedence(&parser->tokens[parser->position]);
        if (precedence == 0U || precedence < minimum) {
            break;
        }
        const Token *op = &parser->tokens[parser->position++];
        ExprValue right;
        if (!expr_parse_binary(parser, precedence + 1U, &right)) {
            expr_copy(result, &left);
            return false;
        }
        bool valid = true;
        ExprValue computed;
        (void)expr_binary(op->text, &left, &right, &valid, &computed);
        if (!valid) {
            parser->failed = true;
            expr_copy(result, &computed);
            return false;
        }
        expr_copy(&left, &computed);
    }
    expr_copy(result, &left);
    return true;
}

static bool expr_parse_conditional(ExprParser *parser, ExprValue *result)
{
    ExprValue condition;
    if (!expr_parse_binary(parser, 0U, &condition)) {
        expr_copy(result, &condition);
        return false;
    }
    if (parser->position < parser->count &&
        token_is(&parser->tokens[parser->position], "?")) {
        ++parser->position;
        ExprValue yes;
        if (!expr_parse_conditional(parser, &yes)) {
            expr_copy(result, &yes);
            return false;
        }
        if (parser->position >= parser->count ||
            !token_is(&parser->tokens[parser->position], ":")) {
            parser->failed = true;
            expr_copy(result, &yes);
            return false;
        }
        ++parser->position;
        ExprValue no;
        if (!expr_parse_conditional(parser, &no)) {
            expr_copy(result, &no);
            return false;
        }
        if (condition.bits != 0U) {
            expr_copy(result, &yes);
        } else {
            expr_copy(result, &no);
        }
        return true;
    }
    expr_copy(result, &condition);
    return true;
}


static bool replace_defined(Preprocessor *pp, const Token *body, size_t count,
                            const Token *directive, TokenList *output)
{
    for (size_t i = 0U; i < count; ++i) {
        if (!token_is(&body[i], "defined")) {
            if (!token_list_push(output, &body[i])) {
                return false;
            }
            continue;
        }
        size_t next = i + 1U;
        bool parentheses = next < count && token_is(&body[next], "(");
        if (parentheses) {
            ++next;
        }
        if (next >= count || !token_is_kind(&body[next], TOKEN_IDENTIFIER)) {
            pp_emit(pp, 1132U, directive, "operator 'defined' requires an identifier");
            return false;
        }
        bool value = find_macro(pp, &body[next]) != NULL ||
                     token_is(&body[next], "__FILE__") ||
                     token_is(&body[next], "__LINE__");
        ++next;
        if (parentheses) {
            if (next >= count || !token_is(&body[next], ")")) {
                pp_emit(pp, 1132U, directive, "expected ')' after 'defined'");
                return false;
            }
            ++next;
        }
        Token value_token;
        const char *value_text = "0";
        if (value) {
            value_text = "1";
        }
        (void)make_text_token(&body[i], TOKEN_NUMBER, value_text, &value_token);
        if (!token_list_push(output, &value_token)) {
            return false;
        }
        i = next - 1U;
    }
    return true;
}

static bool evaluate_expression(Preprocessor *pp, const Token *body,
                                size_t count, const Token *directive)
{
    TokenList replaced;
    token_list_init(&replaced);
    if (!replace_defined(pp, body, count, directive, &replaced)) {
        token_list_free(&replaced);
        return false;
    }
    TokenList expanded;
    token_list_init(&expanded);
    ExpandContext context = {pp, directive == NULL ? NULL : directive->source, 0U, 0U, false, NULL};
    if (!expand_range(replaced.items, 0U, replaced.count, &context, &expanded,
                      NULL)) {
        token_list_free(&replaced);
        token_list_free(&expanded);
        return false;
    }
    if (expanded.count == 0U) {
        pp_emit(pp, 1133U, directive, "empty conditional expression");
        token_list_free(&replaced);
        token_list_free(&expanded);
        return false;
    }
    ExprParser parser = {expanded.items, expanded.count, 0U, pp, false};
    ExprValue value;
    bool parsed = expr_parse_conditional(&parser, &value);
    if (!parsed || parser.failed || parser.position != parser.count) {
        pp_emit(pp, 1134U, directive, "invalid conditional expression");
        token_list_free(&replaced);
        token_list_free(&expanded);
        return false;
    }
    token_list_free(&replaced);
    token_list_free(&expanded);
    return value.bits != 0U;
}

static bool is_valid_macro_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0U; name[i] != '\0'; ++i) {
        unsigned char byte = (unsigned char)name[i];
        if (!(isalpha(byte) || byte == '_' || (i != 0U && isdigit(byte)))) {
            return false;
        }
    }
    return true;
}

static void define_predefined(Preprocessor *pp)
{
    static const char *const definitions[] = {
        "__CC64__=1",
        "__CC64_TARGET__=1",
        "__STDC__=1",
        "__STDC_VERSION__=201710L",
        "__STDC_HOSTED__=0"
    };
    for (size_t i = 0U; i < sizeof(definitions) / sizeof(definitions[0]); ++i) {
        preprocessor_define_text(pp, definitions[i]);
    }
}

Preprocessor *preprocessor_create(Arena *arena, SourceManager *sources,
                                  DiagnosticSink *diagnostics,
                                  const PreprocessorOptions *options)
{
    Preprocessor *pp = arena_alloc(arena, sizeof(*pp));
    if (pp == NULL) {
        return NULL;
    }
    pp->arena = arena;
    pp->sources = sources;
    pp->diagnostics = diagnostics;
    pp->options = *options;
    pp->macros = NULL;
    pp->active_includes = 0U;
    pp->token_count = 0U;
    pp->include_stack = NULL;
    pp->include_stack_count = 0U;
    pp->line_delta = 0U;
    pp->display_path = NULL;
    define_predefined(pp);
    for (size_t i = 0U; i < options->predefine_count; ++i) {
        preprocessor_define_text(pp, options->predefines[i]);
    }
    return pp;
}

void preprocessor_define_text(Preprocessor *pp, const char *definition)
{
    const char *equals = strchr(definition, '=');
    size_t name_length = equals == NULL ? strlen(definition) : (size_t)(equals - definition);
    char *name = cc64_xmalloc(name_length + 1U);
    memcpy(name, definition, name_length);
    name[name_length] = '\0';
    char *expanded = NULL;
    if (equals != NULL) {
        size_t length = strlen(equals + 1U);
        expanded = cc64_xmalloc(length + 1U);
        memcpy(expanded, equals + 1U, length + 1U);
    }
    if (!is_valid_macro_name(name)) {
        diagnostic_emit(pp->diagnostics, 1135U, DIAG_PREPROCESS, NULL, 0U, 0U,
                        "invalid command-line macro name");
        free(name);
        free(expanded);
        return;
    }
    char *text = cc64_xmalloc(name_length + (expanded == NULL ? 1U : strlen(expanded) + 2U));
    const char *equals_text = "";
    const char *expanded_text = "";
    if (expanded != NULL) {
        equals_text = "=";
        expanded_text = expanded;
    }
    (void)snprintf(text, name_length + (expanded == NULL ? 1U : strlen(expanded) + 2U),
                   "%s%s%s", name, expanded == NULL ? "" : equals_text, expanded_text);
    Source *source = source_manager_add(pp->sources, "<command-line>",
                                        (const unsigned char *)text, strlen(text));
    free(text);
    free(name);
    free(expanded);
    TokenList tokens;
    if (!lex_source(pp->arena, source, pp->diagnostics, &tokens)) {
        return;
    }
    size_t definition_count = tokens.count;
    while (definition_count != 0U &&
           tokens.items[definition_count - 1U].kind == TOKEN_EOF) {
        --definition_count;
    }
    (void)parse_macro_definition(pp, tokens.items, definition_count,
                                 definition_count == 0U ? NULL : &tokens.items[0]);
    token_list_free(&tokens);
}

bool preprocessor_run(Preprocessor *pp, const Source *source, TokenList *output)
{
    pp->include_stack = cc64_xmalloc(sizeof(*pp->include_stack));
    pp->include_stack[0] = source->path;
    pp->include_stack_count = 1U;
    bool good = process_source(pp, source, output);
    if (good) {
        Token eof = {0};
        eof.kind = TOKEN_EOF;
        eof.source = source;
        eof.line = 1U;
        eof.column = 1U;
        eof.text = cc64_xstrdup("");
        good = output_push(pp, output, &eof);
    }
    token_list_classify_keywords(output);
    pp->include_stack_count = 0U;
    free((void *)pp->include_stack);
    pp->include_stack = NULL;
    return good;
}

bool preprocess_source(Arena *arena, SourceManager *sources,
                       DiagnosticSink *diagnostics, const Source *source,
                       const PreprocessorOptions *options, TokenList *output)
{
    Preprocessor *pp = preprocessor_create(arena, sources, diagnostics, options);
    if (pp == NULL) {
        return false;
    }
    return preprocessor_run(pp, source, output);
}

bool write_token_list(FILE *stream, const TokenList *list, bool line_markers)
{
    const Token *previous = NULL;
    for (size_t i = 0U; i < list->count; ++i) {
        const Token *token = &list->items[i];
        if (token->kind == TOKEN_EOF) {
            break;
        }
        if (token->kind == TOKEN_NEWLINE) {
            (void)fputc('\n', stream);
            previous = token;
            continue;
        }
        if (line_markers && (previous == NULL || token->source != previous->source ||
                             token->line != previous->line)) {
            if (previous != NULL) {
                (void)fputc('\n', stream);
            }
            (void)fprintf(stream, "# %zu \"%s\"\n", token->line,
                          token->source == NULL ? "<unknown>" : token->source->path);
        } else if (previous != NULL) {
            (void)fputc(' ', stream);
        }
        (void)fputs(token->text, stream);
        previous = token;
    }
    if (previous != NULL && previous->kind != TOKEN_NEWLINE) {
        (void)fputc('\n', stream);
    }
    return !ferror(stream);
}
