#include "ir/ir.h"

#include <stdlib.h>
#include <string.h>

typedef struct LabelEntry {
    char *name;
    size_t id;
    struct LabelEntry *next;
} LabelEntry;

typedef struct LowerContext {
    Arena *arena;
    DiagnosticSink *diagnostics;
    TranslationUnit *unit;
    IrProgram *program;
    Symbol *function;
    size_t next_label;
    size_t break_label;
    size_t continue_label;
    LabelEntry *labels;
    bool failed;
} LowerContext;

static IrInst *ir_new(LowerContext *context, IrOp op, Type *type,
                      const AstNode *origin)
{
    IrInst *inst = arena_alloc(context->arena, sizeof(*inst));
    if (inst == NULL) {
        context->failed = true;
        return NULL;
    }
    memset(inst, 0, sizeof(*inst));
    inst->op = op;
    inst->type = type;
    inst->location = origin == NULL ? (SourceLocation){0} : origin->location;
    if (type != NULL) {
        inst->width = type_size(type);
        inst->is_signed = type_is_signed(type);
    }
    return inst;
}

static void ir_append(IrInst **head, IrInst **tail, IrInst *inst)
{
    if (inst == NULL) return;
    if (*head == NULL) *head = inst;
    else (*tail)->next = inst;
    *tail = inst;
}

static size_t new_label(LowerContext *context)
{
    return ++context->next_label;
}

static void lower_error(LowerContext *context, unsigned id, const AstNode *node,
                        const char *message)
{
    diagnostic_emit(context->diagnostics, id, DIAG_IR,
                    node == NULL ? NULL : node->location.source,
                    node == NULL ? 0U : node->location.line,
                    node == NULL ? 0U : node->location.column, message);
    context->failed = true;
}

static size_t align_up(size_t value, size_t alignment)
{
    if (alignment == 0U) return value;
    size_t mask = alignment - 1U;
    if (value > SIZE_MAX - mask) return SIZE_MAX;
    return (value + mask) & ~mask;
}

static void assign_local(LowerContext *context, Symbol *symbol,
                         size_t *frame)
{
    if (symbol == NULL || symbol->class != SYMBOL_VARIABLE ||
        symbol->has_frame_offset || symbol->storage == STORAGE_STATIC ||
        symbol->storage == STORAGE_EXTERN) return;
    size_t size = type_size(symbol->type);
    size_t alignment = type_alignment(symbol->type);
    if (size == 0U) size = 1U;
    *frame = align_up(*frame, alignment);
    if (*frame > SIZE_MAX - size) {
        lower_error(context, 3001U, NULL, "function frame size overflow");
        return;
    }
    *frame += size;
    symbol->offset = *frame;
    symbol->has_frame_offset = true;
}

static void convert_local_offsets(AstNode *node)
{
    for (; node != NULL; node = node->next) {
        if (node->kind == NODE_DECLARATION && node->symbol != NULL &&
            node->symbol->has_frame_offset && node->symbol->offset > 0U &&
            node->symbol->storage != STORAGE_STATIC &&
            node->symbol->storage != STORAGE_EXTERN) {
            node->symbol->offset = -node->symbol->offset;
        }
        if (node->kind == NODE_COMPOUND) convert_local_offsets(node->a);
        if (node->kind == NODE_IF || node->kind == NODE_WHILE || node->kind == NODE_DO) {
            convert_local_offsets(node->b);
            convert_local_offsets(node->c);
        } else if (node->kind == NODE_FOR) {
            convert_local_offsets(node->a);
            convert_local_offsets(node->d);
        }
    }
}

static void assign_frame(LowerContext *context, Symbol *function, AstNode *body)
{
    size_t frame = 0U;
    size_t index = 0U;
    for (Symbol *parameter = function->type->parameters; parameter != NULL;
         parameter = parameter->next, ++index) {
        if (index < 6U) {
            frame = align_up(frame, 8U);
            if (frame > SIZE_MAX - 8U) {
                lower_error(context, 3002U, NULL, "parameter frame overflow");
                break;
            }
            frame += 8U;
            parameter->offset = frame;
            parameter->has_frame_offset = true;
        } else {
            parameter->offset = 16U + (index - 6U) * 8U;
            parameter->has_frame_offset = true;
        }
    }
    /* Frame offsets are positive while collecting and become negative below. */
    for (AstNode *node = body; node != NULL; node = node->next) {
        if (node->kind == NODE_DECLARATION && node->symbol != NULL) {
            assign_local(context, node->symbol, &frame);
        }
        if (node->a != NULL && node->kind == NODE_COMPOUND) {
            /* Nested declarations are visited by the recursive helper below. */
        }
    }
    /* A small explicit walk keeps declaration scopes and nested blocks visible. */
    AstNode *stack[128];
    size_t depth = 0U;
    if (body != NULL) stack[depth++] = body->a;
    while (depth != 0U && depth < 128U) {
        AstNode *node = stack[--depth];
        while (node != NULL) {
            if (node->kind == NODE_DECLARATION && node->symbol != NULL) {
                assign_local(context, node->symbol, &frame);
            }
            if (node->kind == NODE_COMPOUND && node->a != NULL && depth < 128U) {
                stack[depth++] = node->a;
            }
            if (node->b != NULL && depth < 128U &&
                (node->kind == NODE_IF || node->kind == NODE_WHILE ||
                 node->kind == NODE_DO || node->kind == NODE_FOR)) {
                stack[depth++] = node->b;
            }
            if (node->c != NULL && depth < 128U &&
                (node->kind == NODE_IF || node->kind == NODE_FOR)) {
                stack[depth++] = node->c;
            }
            if (node->d != NULL && depth < 128U && node->kind == NODE_FOR) {
                stack[depth++] = node->d;
            }
            node = node->next;
        }
    }
    for (Symbol *parameter = function->type->parameters; parameter != NULL;
         parameter = parameter->next) {
        if (parameter->has_frame_offset && parameter->offset <= frame) {
            parameter->offset = -parameter->offset;
        }
    }
    /* Locals were assigned positive offsets; convert them after collection. */
    convert_local_offsets(body);
    /* The walk above is intentionally bounded; reject deeper frame nesting. */
    if (frame > SIZE_MAX - 15U) {
        context->failed = true;
    } else {
        frame = align_up(frame, 16U);
    }
    IrFunction *function_ir = arena_alloc(context->arena, sizeof(*function_ir));
    if (function_ir == NULL) {
        context->failed = true;
        return;
    }
    function_ir->symbol = function;
    function_ir->frame_size = frame;
    function_ir->next = NULL;
    if (context->program->functions == NULL) {
        context->program->functions = function_ir;
    } else {
        IrFunction *last = context->program->functions;
        while (last->next != NULL) last = last->next;
        last->next = function_ir;
    }
}

static IrInst *lower_expr(LowerContext *context, AstNode *node);

static IrInst *lower_address(LowerContext *context, AstNode *node)
{
    if (node == NULL) return NULL;
    if (node->kind == NODE_IDENTIFIER && node->symbol != NULL) {
        IrInst *inst = ir_new(context, IR_ADDR, type_pointer(context->arena, node->type, 0U), node);
        if (inst != NULL) inst->symbol = node->symbol;
        return inst;
    }
    if (node->kind == NODE_DEREFERENCE) return lower_expr(context, node->a);
    if (node->kind == NODE_INDEX) {
        IrInst *base = lower_expr(context, node->a);
        IrInst *index = lower_expr(context, node->b);
        Type *element = node->type == NULL ? NULL : node->type;
        size_t scale = type_size(element);
        IrInst *scale_inst = ir_new(context, IR_CONST, type_basic(context->arena, TYPE_LONG), node);
        if (scale_inst != NULL) scale_inst->immediate = scale;
        IrInst *scaled = ir_new(context, IR_MUL, type_basic(context->arena, TYPE_LONG), node);
        if (scaled != NULL) { scaled->a = index; scaled->b = scale_inst; }
        IrInst *add = ir_new(context, IR_ADD, type_basic(context->arena, TYPE_LONG), node);
        if (add != NULL) { add->a = base; add->b = scaled; }
        return add;
    }
    if (node->kind == NODE_MEMBER) {
        IrInst *base = lower_address(context, node->a);
        IrInst *member = ir_new(context, IR_MEMBER, node->type, node);
        if (member != NULL) { member->a = base; member->offset = node->field == NULL ? 0 : (int64_t)node->field->offset; }
        return member;
    }
    lower_error(context, 3004U, node, "address expression is unsupported");
    return NULL;
}

static IrInst *lower_call(LowerContext *context, AstNode *node)
{
    IrInst *inst = ir_new(context, IR_CALL, node->type, node);
    if (inst == NULL) return NULL;
    inst->a = lower_expr(context, node->a);
    IrInst *tail = NULL;
    for (AstNode *arg = node->b; arg != NULL; arg = arg->next) {
        IrInst *value = lower_expr(context, arg);
        if (tail == NULL) inst->args = value;
        else tail->next = value;
        tail = value;
    }
    return inst;
}

static IrInst *lower_expr(LowerContext *context, AstNode *node)
{
    if (node == NULL) return NULL;
    switch (node->kind) {
    case NODE_INTEGER: {
        IrInst *inst = ir_new(context, IR_CONST, node->type, node);
        if (inst != NULL) inst->immediate = node->unsigned_integer;
        return inst;
    }
    case NODE_FLOAT: {
        IrInst *inst = ir_new(context, IR_FLOAT_CONST, node->type, node);
        if (inst != NULL) inst->floating = node->floating;
        return inst;
    }
    case NODE_STRING: {
        IrInst *inst = ir_new(context, IR_ADDR, type_pointer(context->arena, type_basic(context->arena, TYPE_CHAR), 0U), node);
        if (inst != NULL) { inst->symbol = NULL; inst->offset = 0; }
        lower_error(context, 3005U, node, "string literals require global data lowering");
        return inst;
    }
    case NODE_IDENTIFIER:
        if (node->symbol != NULL && node->symbol->class == SYMBOL_ENUM_CONSTANT) {
            IrInst *inst = ir_new(context, IR_CONST, node->type, node);
            if (inst != NULL) inst->immediate = (uint64_t)node->symbol->enum_value;
            return inst;
        }
        if (node->type->kind == TYPE_FUNCTION || node->type->kind == TYPE_ARRAY) return lower_address(context, node);
        {
            IrInst *inst = ir_new(context, IR_LOAD, node->type, node);
            if (inst != NULL) inst->symbol = node->symbol;
            return inst;
        }
    case NODE_CALL: return lower_call(context, node);
    case NODE_CAST: {
        IrInst *inst = ir_new(context, IR_CAST, node->type, node);
        if (inst != NULL) inst->a = lower_expr(context, node->a);
        return inst;
    }
    case NODE_SIZEOF: {
        IrInst *inst = ir_new(context, IR_CONST, type_basic(context->arena, TYPE_UNSIGNED_LONG), node);
        if (inst != NULL) inst->immediate = node->unsigned_integer;
        return inst;
    }
    case NODE_ADDRESS: {
        IrInst *value = lower_address(context, node->a);
        IrInst *inst = ir_new(context, IR_ADDR, node->type, node);
        if (inst != NULL) inst->a = value;
        return inst;
    }
    case NODE_DEREFERENCE: {
        IrInst *address = lower_expr(context, node->a);
        IrInst *inst = ir_new(context, IR_LOAD, node->type, node);
        if (inst != NULL) inst->a = address;
        return inst;
    }
    case NODE_INDEX: {
        IrInst *address = lower_address(context, node);
        IrInst *inst = ir_new(context, IR_LOAD, node->type, node);
        if (inst != NULL) inst->a = address;
        return inst;
    }
    case NODE_MEMBER: {
        IrInst *address = lower_address(context, node);
        IrInst *inst = ir_new(context, IR_LOAD, node->type, node);
        if (inst != NULL) inst->a = address;
        return inst;
    }
    case NODE_ASSIGNMENT: {
        IrInst *address = lower_address(context, node->a);
        IrInst *value = lower_expr(context, node->b);
        IrInst *inst = ir_new(context, IR_STORE, node->type, node);
        if (inst != NULL) { inst->a = address; inst->b = value; }
        return inst;
    }
    case NODE_BINARY: {
        IrOp op;
        switch (node->binary) {
        case BINARY_ADD: op = IR_ADD; break; case BINARY_SUBTRACT: op = IR_SUB; break;
        case BINARY_MULTIPLY: op = IR_MUL; break; case BINARY_DIVIDE: op = IR_DIV; break;
        case BINARY_REMAINDER: op = IR_MOD; break; case BINARY_LEFT_SHIFT: op = IR_SHL; break;
        case BINARY_RIGHT_SHIFT: op = IR_SHR; break; case BINARY_BITWISE_AND: op = IR_BIT_AND; break;
        case BINARY_BITWISE_OR: op = IR_BIT_OR; break; case BINARY_BITWISE_XOR: op = IR_BIT_XOR; break;
        case BINARY_LESS: case BINARY_LESS_EQUAL: case BINARY_GREATER:
        case BINARY_GREATER_EQUAL: case BINARY_EQUAL: case BINARY_NOT_EQUAL: {
            IrInst *inst = ir_new(context, IR_COMPARE, node->type, node);
            if (inst != NULL) { inst->a = lower_expr(context, node->a); inst->b = lower_expr(context, node->b); inst->compare = node->binary == BINARY_LESS ? COMPARE_LESS : node->binary == BINARY_LESS_EQUAL ? COMPARE_LESS_EQUAL : node->binary == BINARY_GREATER ? COMPARE_GREATER : node->binary == BINARY_GREATER_EQUAL ? COMPARE_GREATER_EQUAL : node->binary == BINARY_EQUAL ? COMPARE_EQUAL : COMPARE_NOT_EQUAL; }
            return inst;
        }
        case BINARY_LOGICAL_AND: op = IR_LOGICAL_AND; break;
        case BINARY_LOGICAL_OR: op = IR_LOGICAL_OR; break;
        default: lower_error(context, 3006U, node, "unsupported binary operator"); return NULL;
        }
        IrInst *inst = ir_new(context, op, node->type, node);
        if (inst != NULL) { inst->a = lower_expr(context, node->a); inst->b = lower_expr(context, node->b); }
        return inst;
    }
    case NODE_UNARY: {
        IrOp op;
        switch (node->unary) {
        case UNARY_MINUS: op = IR_NEG; break; case UNARY_BITWISE_NOT: op = IR_BIT_NOT; break;
        case UNARY_LOGICAL_NOT: op = IR_LOGICAL_NOT; break;
        case UNARY_PRE_INCREMENT: case UNARY_PRE_DECREMENT:
        case UNARY_POST_INCREMENT: case UNARY_POST_DECREMENT:
            lower_error(context, 3007U, node, "increment lowering is not yet implemented");
            return NULL;
        default: op = IR_NEG; break;
        }
        IrInst *inst = ir_new(context, op, node->type, node);
        if (inst != NULL) inst->a = lower_expr(context, node->a);
        return inst;
    }
    case NODE_CONDITIONAL: {
        IrInst *inst = ir_new(context, IR_CONDITIONAL, node->type, node);
        if (inst != NULL) {
            inst->true_label = new_label(context);
            inst->false_label = new_label(context);
            inst->end_label = new_label(context);
            inst->a = lower_expr(context, node->a);
            inst->b = lower_expr(context, node->b);
            inst->c = lower_expr(context, node->c);
        }
        return inst;
    }
    case NODE_COMMA: {
        IrInst *inst = ir_new(context, IR_COMMA, node->type, node);
        if (inst != NULL) { inst->a = lower_expr(context, node->a); inst->b = lower_expr(context, node->b); }
        return inst;
    }
    case NODE_INITIALIZER:
        return lower_expr(context, node->a);
    default:
        lower_error(context, 3008U, node, "expression node cannot be lowered");
        return NULL;
    }
}

static LabelEntry *find_label(LowerContext *context, const char *name, bool create)
{
    for (LabelEntry *entry = context->labels; entry != NULL; entry = entry->next) {
        if (strcmp(entry->name, name) == 0) return entry;
    }
    if (!create) return NULL;
    LabelEntry *entry = arena_alloc(context->arena, sizeof(*entry));
    if (entry == NULL) return NULL;
    entry->name = cc64_xstrdup(name);
    entry->id = new_label(context);
    entry->next = context->labels;
    context->labels = entry;
    return entry;
}

static void lower_statement_list(LowerContext *context, AstNode *node,
                                 IrInst **head, IrInst **tail);

static void lower_statement(LowerContext *context, AstNode *node,
                            IrInst **head, IrInst **tail)
{
    if (node == NULL) return;
    switch (node->kind) {
    case NODE_COMPOUND: lower_statement_list(context, node->a, head, tail); break;
    case NODE_DECLARATION: {
        if (node->symbol != NULL && node->a != NULL && type_is_scalar(node->symbol->type)) {
            IrInst *address = ir_new(context, IR_ADDR, type_pointer(context->arena, node->symbol->type, 0U), node);
            if (address != NULL) address->symbol = node->symbol;
            IrInst *value = lower_expr(context, node->a->a);
            IrInst *store = ir_new(context, IR_STORE, node->symbol->type, node);
            if (store != NULL) { store->a = address; store->b = value; }
            ir_append(head, tail, store);
        }
        break;
    }
    case NODE_EXPRESSION_STATEMENT: {
        IrInst *value = lower_expr(context, node->a);
        ir_append(head, tail, value);
        break;
    }
    case NODE_IF: {
        size_t then_label = new_label(context);
        size_t else_label = new_label(context);
        size_t end_label = new_label(context);
        IrInst *branch = ir_new(context, IR_BRANCH, type_basic(context->arena, TYPE_INT), node);
        if (branch != NULL) { branch->a = lower_expr(context, node->a); branch->true_label = then_label; branch->false_label = node->c == NULL ? end_label : else_label; }
        IrInst *label_then = ir_new(context, IR_LABEL, NULL, node); if (label_then != NULL) label_then->id = then_label;
        IrInst *then_head = NULL; IrInst *then_tail = NULL; lower_statement(context, node->b, &then_head, &then_tail);
        IrInst *jump_end = ir_new(context, IR_JUMP, NULL, node); if (jump_end != NULL) jump_end->id = end_label;
        IrInst *label_else = ir_new(context, IR_LABEL, NULL, node); if (label_else != NULL) label_else->id = else_label;
        IrInst *else_head = NULL; IrInst *else_tail = NULL;
        if (node->c != NULL) lower_statement(context, node->c, &else_head, &else_tail);
        IrInst *label_end = ir_new(context, IR_LABEL, NULL, node); if (label_end != NULL) label_end->id = end_label;
        ir_append(head, tail, branch);
        ir_append(head, tail, label_then);
        ir_append(head, tail, then_head);
        if (node->c != NULL) {
            ir_append(head, tail, jump_end);
            ir_append(head, tail, label_else);
            ir_append(head, tail, else_head);
        }
        ir_append(head, tail, label_end);
        break;
    }
    case NODE_WHILE: {
        size_t start = new_label(context); size_t end = new_label(context);
        size_t old_break = context->break_label; size_t old_continue = context->continue_label;
        context->break_label = end; context->continue_label = start;
        IrInst *label_start = ir_new(context, IR_LABEL, NULL, node); if (label_start != NULL) label_start->id = start;
        IrInst *branch = ir_new(context, IR_BRANCH, type_basic(context->arena, TYPE_INT), node); if (branch != NULL) { branch->a = lower_expr(context, node->a); branch->false_label = end; }
        IrInst *body_head = NULL; IrInst *body_tail = NULL; lower_statement(context, node->b, &body_head, &body_tail);
        IrInst *jump_start = ir_new(context, IR_JUMP, NULL, node); if (jump_start != NULL) jump_start->id = start;
        IrInst *label_end = ir_new(context, IR_LABEL, NULL, node); if (label_end != NULL) label_end->id = end;
        ir_append(head, tail, label_start); ir_append(head, tail, branch); ir_append(head, tail, body_head); ir_append(head, tail, jump_start); ir_append(head, tail, label_end);
        context->break_label = old_break; context->continue_label = old_continue;
        break;
    }
    case NODE_DO: {
        size_t start = new_label(context); size_t continue_label = new_label(context); size_t end = new_label(context);
        size_t old_break = context->break_label; size_t old_continue = context->continue_label;
        context->break_label = end; context->continue_label = continue_label;
        IrInst *label_start = ir_new(context, IR_LABEL, NULL, node); if (label_start != NULL) label_start->id = start;
        IrInst *body_head = NULL; IrInst *body_tail = NULL; lower_statement(context, node->b, &body_head, &body_tail);
        IrInst *label_continue = ir_new(context, IR_LABEL, NULL, node); if (label_continue != NULL) label_continue->id = continue_label;
        IrInst *branch = ir_new(context, IR_BRANCH, type_basic(context->arena, TYPE_INT), node); if (branch != NULL) { branch->a = lower_expr(context, node->a); branch->true_label = start; branch->false_label = end; }
        IrInst *label_end = ir_new(context, IR_LABEL, NULL, node); if (label_end != NULL) label_end->id = end;
        ir_append(head, tail, label_start); ir_append(head, tail, body_head); ir_append(head, tail, label_continue); ir_append(head, tail, branch); ir_append(head, tail, label_end);
        context->break_label = old_break; context->continue_label = old_continue;
        break;
    }
    case NODE_FOR: {
        size_t start = new_label(context); size_t continue_label = new_label(context); size_t end = new_label(context);
        IrInst *init = lower_expr(context, node->a); ir_append(head, tail, init);
        IrInst *label_start = ir_new(context, IR_LABEL, NULL, node); if (label_start != NULL) label_start->id = start;
        IrInst *branch = ir_new(context, IR_BRANCH, type_basic(context->arena, TYPE_INT), node); if (branch != NULL) { branch->a = lower_expr(context, node->b); branch->false_label = end; }
        size_t old_break = context->break_label; size_t old_continue = context->continue_label; context->break_label = end; context->continue_label = continue_label;
        IrInst *body_head = NULL; IrInst *body_tail = NULL; lower_statement(context, node->d, &body_head, &body_tail);
        IrInst *label_continue = ir_new(context, IR_LABEL, NULL, node); if (label_continue != NULL) label_continue->id = continue_label;
        IrInst *step = lower_expr(context, node->c);
        IrInst *jump_start = ir_new(context, IR_JUMP, NULL, node); if (jump_start != NULL) jump_start->id = start;
        IrInst *label_end = ir_new(context, IR_LABEL, NULL, node); if (label_end != NULL) label_end->id = end;
        ir_append(head, tail, label_start); ir_append(head, tail, branch); ir_append(head, tail, body_head); ir_append(head, tail, label_continue); ir_append(head, tail, step); ir_append(head, tail, jump_start); ir_append(head, tail, label_end);
        context->break_label = old_break; context->continue_label = old_continue;
        break;
    }
    case NODE_BREAK: { IrInst *inst = ir_new(context, IR_JUMP, NULL, node); if (inst != NULL) inst->id = context->break_label; ir_append(head, tail, inst); break; }
    case NODE_CONTINUE: { IrInst *inst = ir_new(context, IR_JUMP, NULL, node); if (inst != NULL) inst->id = context->continue_label; ir_append(head, tail, inst); break; }
    case NODE_RETURN: { IrInst *value = lower_expr(context, node->a); IrInst *inst = ir_new(context, IR_RETURN, context->function == NULL ? NULL : context->function->type->return_type, node); if (inst != NULL) inst->a = value; ir_append(head, tail, inst); break; }
    case NODE_LABEL: { LabelEntry *entry = find_label(context, node->text, true); IrInst *inst = ir_new(context, IR_LABEL, NULL, node); if (inst != NULL) inst->id = entry->id; ir_append(head, tail, inst); break; }
    case NODE_GOTO: { LabelEntry *entry = find_label(context, node->text, true); IrInst *inst = ir_new(context, IR_JUMP, NULL, node); if (inst != NULL) inst->id = entry->id; ir_append(head, tail, inst); break; }
    default: {
        IrInst *value = lower_expr(context, node);
        ir_append(head, tail, value);
        break;
    }
    }
}

static void lower_statement_list(LowerContext *context, AstNode *node,
                                 IrInst **head, IrInst **tail)
{
    for (; node != NULL; node = node->next) lower_statement(context, node, head, tail);
}

bool lower_translation_unit(Arena *arena, const TranslationUnit *unit,
                            DiagnosticSink *diagnostics, IrProgram *program)
{
    memset(program, 0, sizeof(*program));
    program->globals = unit->globals;
    for (size_t i = 0U; i < unit->count; ++i) {
        AstNode *declaration = unit->declarations[i];
        Symbol *symbol = declaration == NULL ? NULL : declaration->symbol;
        if (symbol == NULL || symbol->class != SYMBOL_VARIABLE) continue;
        bool seen = false;
        for (size_t j = 0U; j < program->global_count; ++j) {
            if (program->global_symbols[j] == symbol) { seen = true; break; }
        }
        if (!seen) {
            if (program->global_count == program->global_capacity) {
                size_t next = program->global_capacity == 0U ? 8U : program->global_capacity * 2U;
                Symbol **grown = arena_alloc_array(arena, next, sizeof(*grown));
                if (grown == NULL) return false;
                if (program->global_symbols != NULL) memcpy(grown, program->global_symbols, program->global_count * sizeof(*grown));
                program->global_symbols = grown;
                program->global_capacity = next;
            }
            program->global_symbols[program->global_count++] = symbol;
        }
    }
    for (size_t i = 0U; i < unit->count; ++i) {
        AstNode *declaration = unit->declarations[i];
        if (declaration == NULL || declaration->kind != NODE_FUNCTION_DEFINITION) continue;
        LowerContext context = {arena, diagnostics, (TranslationUnit *)unit, program, declaration->symbol, 0U, 0U, 0U, NULL, false};
        assign_frame(&context, declaration->symbol, declaration->a);
        if (context.failed) return false;
        IrFunction *function = program->functions;
        while (function != NULL && function->symbol != declaration->symbol) function = function->next;
        IrInst *head = NULL; IrInst *tail = NULL;
        lower_statement_list(&context, declaration->a->a, &head, &tail);
        if (function != NULL) function->body = head;
        if (context.failed) return false;
    }
    return true;
}

void ir_program_free(IrProgram *program)
{
    if (program == NULL) return;
    program->functions = NULL;
    program->function_tail = NULL;
    program->globals = NULL;
    program->global_symbols = NULL;
    program->global_count = 0U;
    program->global_capacity = 0U;
}
