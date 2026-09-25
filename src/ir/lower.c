#include "ir/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct LabelEntry {
    char *name;
    size_t id;
    struct LabelEntry *next;
} LabelEntry;

typedef struct CaseLabel {
    AstNode *node;
    size_t id;
    uint64_t value;
    bool has_value;
    struct CaseLabel *next;
} CaseLabel;

typedef struct StringLiteral {
    AstNode *node;
    Symbol *symbol;
    struct StringLiteral *next;
} StringLiteral;

typedef struct LowerContext {
    Arena *arena;
    DiagnosticSink *diagnostics;
    const TranslationUnit *unit;
    IrProgram *program;
    Symbol *function;
    size_t next_label;
    size_t break_label;
    size_t continue_label;
    size_t switch_end_label;
    LabelEntry *labels;
    CaseLabel *cases;
    StringLiteral *literals;
    size_t literal_count;
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
    inst->location.source = origin == NULL ? NULL : origin->location.source;
    inst->location.line = origin == NULL ? 0U : origin->location.line;
    inst->location.column = origin == NULL ? 0U : origin->location.column;
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

static void ir_append_list(IrInst **head, IrInst **tail,
                           IrInst *list_head, IrInst *list_tail)
{
    if (list_head == NULL) return;
    if (*head == NULL) *head = list_head;
    else (*tail)->next = list_head;
    *tail = list_tail == NULL ? list_head : list_tail;
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

static bool program_add_global(LowerContext *context, Symbol *symbol)
{
    if (symbol == NULL) return false;
    for (size_t i = 0U; i < context->program->global_count; ++i) {
        if (context->program->global_symbols[i] == symbol) return true;
    }
    if (context->program->global_count == context->program->global_capacity) {
        size_t next = context->program->global_capacity == 0U
                          ? 8U : context->program->global_capacity * 2U;
        if (next < context->program->global_capacity) return false;
        Symbol **grown = arena_alloc_array(context->arena, next, sizeof(*grown));
        if (grown == NULL) return false;
        if (context->program->global_symbols != NULL) {
            memcpy(grown, context->program->global_symbols,
                   context->program->global_count * sizeof(*grown));
        }
        context->program->global_symbols = grown;
        context->program->global_capacity = next;
    }
    context->program->global_symbols[context->program->global_count++] = symbol;
    return true;
}

static Symbol *literal_symbol(LowerContext *context, AstNode *node)
{
    if (node == NULL || node->kind != NODE_STRING) return NULL;
    if (node->literal_symbol != NULL) return node->literal_symbol;
    Symbol *symbol = arena_alloc(context->arena, sizeof(*symbol));
    if (symbol == NULL) {
        context->failed = true;
        return NULL;
    }
    memset(symbol, 0, sizeof(*symbol));
    char name[32];
    (void)snprintf(name, sizeof(name), ".Lstr.%zu", context->literal_count++);
    symbol->name = cc64_xstrdup(name);
    symbol->type = node->type;
    symbol->class = SYMBOL_VARIABLE;
    symbol->storage = STORAGE_STATIC;
    symbol->linkage = LINKAGE_INTERNAL;
    symbol->defined = true;
    symbol->initializer = node;
    node->literal_symbol = symbol;
    if (!program_add_global(context, symbol)) {
        context->failed = true;
        return NULL;
    }
    return symbol;
}

static void collect_literals(LowerContext *context, AstNode *node)
{
    for (; node != NULL; node = node->next) {
        if (node->kind == NODE_STRING) (void)literal_symbol(context, node);
        collect_literals(context, node->a);
        collect_literals(context, node->b);
        collect_literals(context, node->c);
        collect_literals(context, node->d);
    }
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
        convert_local_offsets(node->a);
        convert_local_offsets(node->b);
        convert_local_offsets(node->c);
        convert_local_offsets(node->d);
    }
}

static void assign_frame_nodes(LowerContext *context, AstNode *node,
                               size_t *frame, unsigned depth)
{
    if (depth > 1024U) {
        lower_error(context, 3003U, NULL, "function nesting limit exceeded");
        return;
    }
    for (; node != NULL; node = node->next) {
        if (node->kind == NODE_DECLARATION && node->symbol != NULL) {
            assign_local(context, node->symbol, frame);
        }
        assign_frame_nodes(context, node->a, frame, depth + 1U);
        assign_frame_nodes(context, node->b, frame, depth + 1U);
        assign_frame_nodes(context, node->c, frame, depth + 1U);
        assign_frame_nodes(context, node->d, frame, depth + 1U);
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
    assign_frame_nodes(context, body == NULL ? NULL : body->a, &frame, 0U);
    size_t parameter_index = 0U;
    for (Symbol *parameter = function->type->parameters; parameter != NULL;
         parameter = parameter->next, ++parameter_index) {
        if (parameter_index < 6U && parameter->offset > 0U) {
            parameter->offset = -parameter->offset;
        }
    }
    /* Locals were assigned positive offsets; convert them after collection. */
    convert_local_offsets(body);
    /* A variadic function needs a register save area for the unnamed
       arguments. It is reserved after every local so that its offset is
       known before the frame is finalized. */
    size_t va_area = 0U;
    if (function->type->variadic) {
        frame = align_up(frame, 8U);
        frame += 48U;
        va_area = frame;
    }
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
    function_ir->va_area_offset = va_area == 0U ? 0 : -(int64_t)va_area;
    function_ir->next = NULL;
    if (context->program->functions == NULL) {
        context->program->functions = function_ir;
    } else {
        context->program->function_tail->next = function_ir;
    }
    context->program->function_tail = function_ir;
}

static void collect_static_symbols(LowerContext *context, AstNode *node)
{
    for (; node != NULL; node = node->next) {
        if (node->kind == NODE_DECLARATION && node->symbol != NULL &&
            node->symbol->class == SYMBOL_VARIABLE &&
            node->symbol->storage == STORAGE_STATIC) {
            (void)program_add_global(context, node->symbol);
        }
        collect_static_symbols(context, node->a);
        collect_static_symbols(context, node->b);
        collect_static_symbols(context, node->c);
        collect_static_symbols(context, node->d);
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
        IrInst *base = node->a != NULL && node->a->type != NULL &&
                       (node->a->type->kind == TYPE_ARRAY ||
                        node->a->type->kind == TYPE_STRUCT ||
                        node->a->type->kind == TYPE_UNION)
                           ? lower_address(context, node->a)
                           : lower_expr(context, node->a);
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
        IrInst *base = node->a != NULL && node->a->type != NULL &&
                       node->a->type->kind == TYPE_POINTER
                       ? lower_expr(context, node->a)
                       : lower_address(context, node->a);
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
        Symbol *symbol = literal_symbol(context, node);
        IrInst *inst = ir_new(context, IR_ADDR,
                              type_pointer(context->arena, type_basic(context->arena, TYPE_CHAR), 0U), node);
        if (inst != NULL) inst->symbol = symbol;
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
        if (node->type != NULL && (node->type->kind == TYPE_ARRAY ||
                                   node->type->kind == TYPE_STRUCT ||
                                   node->type->kind == TYPE_UNION)) {
            return lower_address(context, node);
        }
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
        if (node->compound_assignment && node->type != NULL &&
            type_is_scalar(node->type)) {
            IrInst *address = lower_address(context, node->a);
            AstNode *rhs = node->b;
            if (rhs != NULL && rhs->kind == NODE_BINARY &&
                rhs->binary == node->binary) rhs = rhs->b;
            IrInst *value = lower_expr(context, rhs);
            if (value != NULL && type_is_pointer(node->type) &&
                (node->binary == BINARY_ADD || node->binary == BINARY_SUBTRACT)) {
                IrInst *scale = ir_new(context, IR_CONST,
                                       type_basic(context->arena, TYPE_LONG), node);
                IrInst *scaled = ir_new(context, IR_MUL,
                                        type_basic(context->arena, TYPE_LONG), node);
                if (scale != NULL) scale->immediate = type_size(node->type->base);
                if (scaled != NULL) { scaled->a = value; scaled->b = scale; }
                value = scaled;
            }
            IrInst *inst = ir_new(context, IR_COMPOUND, node->type, node);
            if (inst != NULL) {
                inst->a = address;
                inst->b = value;
                inst->binary = node->binary;
            }
            return inst;
        }
        if (node->type != NULL && (node->type->kind == TYPE_STRUCT ||
                                   node->type->kind == TYPE_UNION) &&
            node->b != NULL) {
            IrInst *address = lower_address(context, node->a);
            IrInst *source = lower_address(context, node->b);
            IrInst *inst = ir_new(context, IR_COPY, node->type, node);
            if (inst != NULL) {
                inst->a = address;
                inst->b = source;
                inst->immediate = type_size(node->type);
            }
            return inst;
        }
        IrInst *address = lower_address(context, node->a);
        IrInst *value = lower_expr(context, node->b);
        IrInst *inst = ir_new(context, IR_STORE, node->type, node);
        if (inst != NULL) { inst->a = address; inst->b = value; }
        return inst;
    }
    case NODE_BINARY: {
        if (node->type != NULL && type_is_pointer(node->type) &&
            (node->binary == BINARY_ADD || node->binary == BINARY_SUBTRACT)) {
            Type *pointer_type = NULL;
            AstNode *integer_side = NULL;
            if (node->a != NULL && type_is_pointer(node->a->type)) {
                pointer_type = node->a->type;
                integer_side = node->b;
            } else if (node->binary == BINARY_ADD && node->b != NULL &&
                       type_is_pointer(node->b->type)) {
                pointer_type = node->b->type;
                integer_side = node->a;
            }
            if (pointer_type != NULL && integer_side != NULL) {
                IrInst *base = node->a != NULL && type_is_pointer(node->a->type)
                                   ? lower_expr(context, node->a)
                                   : lower_expr(context, node->b);
                IrInst *index = lower_expr(context, integer_side);
                IrInst *scale = ir_new(context, IR_CONST,
                                       type_basic(context->arena, TYPE_LONG), node);
                IrInst *scaled = ir_new(context, IR_MUL,
                                        type_basic(context->arena, TYPE_LONG), node);
                IrInst *result = ir_new(context,
                                        node->binary == BINARY_ADD ? IR_ADD : IR_SUB,
                                        node->type, node);
                if (scale != NULL) scale->immediate = type_size(pointer_type->base);
                if (scaled != NULL) { scaled->a = index; scaled->b = scale; }
                if (result != NULL) {
                    result->a = base;
                    result->b = scaled;
                }
                return result;
            }
        }
        if (node->binary == BINARY_SUBTRACT && node->a != NULL && node->b != NULL &&
            type_is_pointer(node->a->type) && type_is_pointer(node->b->type)) {
            IrInst *left = lower_expr(context, node->a);
            IrInst *right = lower_expr(context, node->b);
            IrInst *difference = ir_new(context, IR_SUB,
                                        type_basic(context->arena, TYPE_LONG), node);
            IrInst *scale = ir_new(context, IR_CONST,
                                   type_basic(context->arena, TYPE_LONG), node);
            IrInst *result = ir_new(context, IR_DIV,
                                    type_basic(context->arena, TYPE_LONG), node);
            if (difference != NULL) { difference->a = left; difference->b = right; }
            if (scale != NULL) scale->immediate = type_size(node->a->type->base);
            if (result != NULL) { result->a = difference; result->b = scale; }
            return result;
        }
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
        if (inst != NULL) {
            inst->a = lower_expr(context, node->a);
            inst->b = lower_expr(context, node->b);
            if (op == IR_LOGICAL_AND || op == IR_LOGICAL_OR) {
                inst->true_label = new_label(context);
                inst->false_label = new_label(context);
                inst->end_label = new_label(context);
            }
        }
        return inst;
    }
    case NODE_UNARY: {
        IrOp op;
        switch (node->unary) {
        case UNARY_MINUS: op = IR_NEG; break; case UNARY_BITWISE_NOT: op = IR_BIT_NOT; break;
        case UNARY_LOGICAL_NOT: op = IR_LOGICAL_NOT; break;
        case UNARY_PRE_INCREMENT: case UNARY_PRE_DECREMENT:
        case UNARY_POST_INCREMENT: case UNARY_POST_DECREMENT: {
            IrInst *address = lower_address(context, node->a);
            IrInst *inst = ir_new(context, IR_INCREMENT, node->type, node);
            if (inst != NULL) {
                int64_t step = 1;
                if (node->type != NULL && type_is_pointer(node->type)) {
                    step = (int64_t)type_size(node->type->base);
                }
                if (node->unary == UNARY_PRE_DECREMENT ||
                    node->unary == UNARY_POST_DECREMENT) step = -step;
                inst->a = address;
                inst->immediate = (uint64_t)step;
                inst->post = node->unary == UNARY_POST_INCREMENT ||
                             node->unary == UNARY_POST_DECREMENT;
            }
            return inst;
        }
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

static void lower_statement(LowerContext *context, AstNode *node,
                            IrInst **head, IrInst **tail);
static void lower_statement_list(LowerContext *context, AstNode *node,
                                 IrInst **head, IrInst **tail);

static CaseLabel *find_case(LowerContext *context, AstNode *node)
{
    for (CaseLabel *entry = context->cases; entry != NULL; entry = entry->next) {
        if (entry->node == node) return entry;
    }
    return NULL;
}

static bool constant_case_value(const AstNode *node, uint64_t *value)
{
    if (node == NULL) return false;
    if (node->kind == NODE_INITIALIZER || node->kind == NODE_CAST) {
        if (!constant_case_value(node->a, value)) return false;
        if (node->kind == NODE_CAST && node->type != NULL) {
            size_t width = type_size(node->type);
            if (width == 1U) *value = type_is_signed(node->type)
                                         ? (uint64_t)(int64_t)(int8_t)*value
                                         : (uint64_t)(uint8_t)*value;
            else if (width == 2U) *value = type_is_signed(node->type)
                                          ? (uint64_t)(int64_t)(int16_t)*value
                                          : (uint64_t)(uint16_t)*value;
            else if (width == 4U) *value = type_is_signed(node->type)
                                          ? (uint64_t)(int64_t)(int32_t)*value
                                          : (uint64_t)(uint32_t)*value;
        }
        return true;
    }
    if (node->kind == NODE_INTEGER) {
        *value = node->unsigned_integer;
        return true;
    }
    if (node->kind == NODE_SIZEOF) {
        *value = node->unsigned_integer;
        return true;
    }
    if (node->kind == NODE_IDENTIFIER && node->symbol != NULL &&
        node->symbol->class == SYMBOL_ENUM_CONSTANT) {
        *value = (uint64_t)node->symbol->enum_value;
        return true;
    }
    if (node->kind == NODE_UNARY) {
        uint64_t operand = 0U;
        if (!constant_case_value(node->a, &operand)) return false;
        switch (node->unary) {
        case UNARY_PLUS: *value = operand; return true;
        case UNARY_MINUS: *value = 0U - operand; return true;
        case UNARY_BITWISE_NOT: *value = ~operand; return true;
        case UNARY_LOGICAL_NOT: *value = operand == 0U ? 1U : 0U; return true;
        default: return false;
        }
    }
    if (node->kind == NODE_BINARY) {
        uint64_t left = 0U, right = 0U;
        if (!constant_case_value(node->a, &left) ||
            !constant_case_value(node->b, &right)) return false;
        switch (node->binary) {
        case BINARY_ADD: *value = left + right; return true;
        case BINARY_SUBTRACT: *value = left - right; return true;
        case BINARY_MULTIPLY: *value = left * right; return true;
        case BINARY_DIVIDE: if (right == 0U) return false; *value = left / right; return true;
        case BINARY_REMAINDER: if (right == 0U) return false; *value = left % right; return true;
        case BINARY_LEFT_SHIFT: *value = left << (right & 63U); return true;
        case BINARY_RIGHT_SHIFT: *value = left >> (right & 63U); return true;
        case BINARY_BITWISE_AND: *value = left & right; return true;
        case BINARY_BITWISE_OR: *value = left | right; return true;
        case BINARY_BITWISE_XOR: *value = left ^ right; return true;
        case BINARY_LOGICAL_AND: *value = left != 0U && right != 0U; return true;
        case BINARY_LOGICAL_OR: *value = left != 0U || right != 0U; return true;
        case BINARY_LESS: *value = left < right; return true;
        case BINARY_LESS_EQUAL: *value = left <= right; return true;
        case BINARY_GREATER: *value = left > right; return true;
        case BINARY_GREATER_EQUAL: *value = left >= right; return true;
        case BINARY_EQUAL: *value = left == right; return true;
        case BINARY_NOT_EQUAL: *value = left != right; return true;
        }
    }
    return false;
}

static uint64_t normalize_case_value(Type *type, uint64_t value)
{
    size_t width = type_size(type);
    if (width == 1U) {
        return type_is_signed(type) ? (uint64_t)(int64_t)(int8_t)value
                                    : (uint64_t)(uint8_t)value;
    }
    if (width == 2U) {
        return type_is_signed(type) ? (uint64_t)(int64_t)(int16_t)value
                                    : (uint64_t)(uint16_t)value;
    }
    if (width == 4U) {
        return type_is_signed(type) ? (uint64_t)(int64_t)(int32_t)value
                                    : (uint64_t)(uint32_t)value;
    }
    return value;
}

static void collect_cases(LowerContext *context, AstNode *node,
                          CaseLabel **head, CaseLabel **tail,
                          size_t *count, size_t *default_label,
                          bool *has_default)
{
    for (; node != NULL; node = node->next) {
        if (node->kind == NODE_SWITCH) continue;
        if (node->kind == NODE_CASE || node->kind == NODE_DEFAULT) {
            uint64_t value = 0U;
            if (node->kind == NODE_CASE && !constant_case_value(node->a, &value)) {
                lower_error(context, 3009U, node, "case label is not an integer constant");
            }
            if (node->kind == NODE_CASE) {
                for (CaseLabel *entry = *head; entry != NULL; entry = entry->next) {
                    if (entry->has_value && entry->value == value) {
                        lower_error(context, 3011U, node, "duplicate case label");
                    }
                }
            }
            CaseLabel *entry = arena_alloc(context->arena, sizeof(*entry));
            if (entry == NULL) {
                context->failed = true;
                return;
            }
            entry->node = node;
            entry->id = new_label(context);
            entry->value = value;
            entry->has_value = node->kind == NODE_CASE;
            entry->next = NULL;
            if (*tail == NULL) *head = entry;
            else (*tail)->next = entry;
            *tail = entry;
            ++*count;
            if (node->kind == NODE_DEFAULT) {
                if (*has_default) lower_error(context, 3010U, node, "duplicate default label");
                *has_default = true;
                *default_label = entry->id;
            }
            continue;
        }
        collect_cases(context, node->a, head, tail, count,
                      default_label, has_default);
        collect_cases(context, node->b, head, tail, count,
                      default_label, has_default);
        collect_cases(context, node->c, head, tail, count,
                      default_label, has_default);
        collect_cases(context, node->d, head, tail, count,
                      default_label, has_default);
    }
}

static void lower_local_zero(LowerContext *context, Symbol *symbol,
                             IrInst **head, IrInst **tail)
{
    if (symbol == NULL || symbol->type == NULL ||
        (type_size(symbol->type) == 0U)) return;
    IrInst *address = ir_new(context, IR_ADDR,
                             type_pointer(context->arena, symbol->type, 0U),
                             NULL);
    IrInst *zero = ir_new(context, IR_ZERO, symbol->type, NULL);
    if (address != NULL) address->symbol = symbol;
    if (zero != NULL) { zero->a = address; zero->immediate = type_size(symbol->type); }
    ir_append(head, tail, zero);
}

static void lower_string_at(LowerContext *context, Symbol *symbol,
                            Type *type, size_t offset, AstNode *value,
                            IrInst **head, IrInst **tail)
{
    if (type == NULL || value == NULL || value->kind != NODE_STRING) return;
    for (size_t i = 0U; i < value->text_length && i < type->array_count; ++i) {
        IrInst *address = ir_new(context, IR_ADDR,
                                  type_pointer(context->arena, type->base, 0U), value);
        IrInst *constant = ir_new(context, IR_CONST,
                                   type_basic(context->arena, TYPE_LONG), value);
        IrInst *add = ir_new(context, IR_ADD,
                             type_basic(context->arena, TYPE_LONG), value);
        IrInst *byte = ir_new(context, IR_CONST, type->base, value);
        IrInst *store = ir_new(context, IR_STORE, type->base, value);
        if (address != NULL) address->symbol = symbol;
        if (constant != NULL) constant->immediate = offset + i * type_size(type->base);
        if (byte != NULL) byte->immediate = (unsigned char)value->text[i];
        if (add != NULL) { add->a = address; add->b = constant; }
        if (store != NULL) { store->a = add; store->b = byte; }
        ir_append(head, tail, store);
    }
}

static void lower_init_at(LowerContext *context, Symbol *symbol,
                          Type *type, size_t offset, AstNode *value,
                          IrInst **head, IrInst **tail)
{
    if (symbol == NULL || type == NULL || value == NULL) return;
    if (value->kind == NODE_INITIALIZER && value->a == NULL) return;
    while (value != NULL && value->kind == NODE_INITIALIZER && value->a != NULL &&
           type_is_scalar(type)) value = value->a;
    if (value == NULL) return;
    if (type->kind == TYPE_ARRAY) {
        if (value->kind == NODE_STRING) {
            lower_string_at(context, symbol, type, offset, value, head, tail);
            return;
        }
        if (value->kind == NODE_INITIALIZER) {
            size_t index = 0U;
            for (AstNode *item = value->a; item != NULL && index < type->array_count;
                 item = item->next, ++index) {
                lower_init_at(context, symbol, type->base,
                              offset + index * type_size(type->base), item,
                              head, tail);
            }
        }
        return;
    }
    if (type->kind == TYPE_STRUCT || type->kind == TYPE_UNION) {
        if (value->kind != NODE_INITIALIZER) return;
        Member *member = type->members;
        for (AstNode *item = value->a; item != NULL && member != NULL;
             item = item->next, member = member->next) {
            lower_init_at(context, symbol, member->type,
                          offset + member->offset, item, head, tail);
            if (type->kind == TYPE_UNION) break;
        }
        return;
    }
    IrInst *address = ir_new(context, IR_ADDR,
                              type_pointer(context->arena, type, 0U), value);
    IrInst *add = NULL;
    IrInst *address_value = address;
    if (offset != 0U) {
        IrInst *constant = ir_new(context, IR_CONST,
                                   type_basic(context->arena, TYPE_LONG), value);
        add = ir_new(context, IR_ADD, type_basic(context->arena, TYPE_LONG), value);
        if (constant != NULL) constant->immediate = offset;
        if (add != NULL) { add->a = address; add->b = constant; }
        address_value = add;
    }
    IrInst *store = ir_new(context, IR_STORE, type, value);
    IrInst *scalar = value->kind == NODE_STRING
                         ? ir_new(context, IR_CONST, type, value) : lower_expr(context, value);
    if (scalar != NULL && value->kind == NODE_STRING) scalar->immediate = (unsigned char)value->text[0];
    if (address != NULL) address->symbol = symbol;
    if (store != NULL) { store->a = address_value; store->b = scalar; }
    ir_append(head, tail, store);
}

static void lower_local_initializer(LowerContext *context, Symbol *symbol,
                                   AstNode *initializer, IrInst **head,
                                   IrInst **tail)
{
    if (symbol == NULL || initializer == NULL) return;
    Type *type = symbol->type;
    if (type != NULL && (type->kind == TYPE_ARRAY || type->kind == TYPE_STRUCT ||
                         type->kind == TYPE_UNION)) {
        lower_local_zero(context, symbol, head, tail);
    }
    if (type != NULL && type->kind == TYPE_ARRAY && initializer->kind == NODE_INITIALIZER &&
        initializer->a != NULL && initializer->a->kind == NODE_STRING) {
        lower_string_at(context, symbol, type, 0U, initializer->a, head, tail);
        return;
    }
    lower_init_at(context, symbol, type, 0U, initializer, head, tail);
}

static void lower_one_statement(LowerContext *context, AstNode *node,
                                IrInst **head, IrInst **tail)
{
    if (node == NULL) return;
    switch (node->kind) {
    case NODE_COMPOUND: lower_statement_list(context, node->a, head, tail); break;
    case NODE_DECLARATION: {
        if (node->symbol != NULL && node->a != NULL &&
            node->symbol->storage != STORAGE_STATIC &&
            node->symbol->storage != STORAGE_EXTERN) {
            lower_local_initializer(context, node->symbol, node->a, head, tail);
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
        ir_append_list(head, tail, then_head, then_tail);
        if (node->c != NULL) {
            ir_append(head, tail, jump_end);
            ir_append(head, tail, label_else);
            ir_append_list(head, tail, else_head, else_tail);
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
        ir_append(head, tail, label_start); ir_append(head, tail, branch); ir_append_list(head, tail, body_head, body_tail); ir_append(head, tail, jump_start); ir_append(head, tail, label_end);
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
        IrInst *branch = ir_new(context, IR_BRANCH, type_basic(context->arena, TYPE_INT), node); if (branch != NULL) { branch->a = lower_expr(context, node->a); branch->false_label = end; }
        IrInst *jump_start = ir_new(context, IR_JUMP, NULL, node); if (jump_start != NULL) jump_start->id = start;
        IrInst *label_end = ir_new(context, IR_LABEL, NULL, node); if (label_end != NULL) label_end->id = end;
        ir_append(head, tail, label_start); ir_append_list(head, tail, body_head, body_tail); ir_append(head, tail, label_continue); ir_append(head, tail, branch); ir_append(head, tail, jump_start); ir_append(head, tail, label_end);
        context->break_label = old_break; context->continue_label = old_continue;
        break;
    }
    case NODE_FOR: {
        size_t start = new_label(context); size_t continue_label = new_label(context); size_t end = new_label(context);
        if (node->a != NULL && node->a->kind == NODE_DECLARATION) {
            lower_statement(context, node->a, head, tail);
        } else {
            IrInst *init = lower_expr(context, node->a);
            ir_append(head, tail, init);
        }
        IrInst *label_start = ir_new(context, IR_LABEL, NULL, node); if (label_start != NULL) label_start->id = start;
        IrInst *branch = NULL;
        if (node->b != NULL) {
            branch = ir_new(context, IR_BRANCH, type_basic(context->arena, TYPE_INT), node);
            if (branch != NULL) { branch->a = lower_expr(context, node->b); branch->false_label = end; }
        }
        size_t old_break = context->break_label; size_t old_continue = context->continue_label; context->break_label = end; context->continue_label = continue_label;
        IrInst *body_head = NULL; IrInst *body_tail = NULL; lower_statement(context, node->d, &body_head, &body_tail);
        IrInst *label_continue = ir_new(context, IR_LABEL, NULL, node); if (label_continue != NULL) label_continue->id = continue_label;
        IrInst *step = lower_expr(context, node->c);
        IrInst *jump_start = ir_new(context, IR_JUMP, NULL, node); if (jump_start != NULL) jump_start->id = start;
        IrInst *label_end = ir_new(context, IR_LABEL, NULL, node); if (label_end != NULL) label_end->id = end;
        ir_append(head, tail, label_start); ir_append(head, tail, branch); ir_append_list(head, tail, body_head, body_tail); ir_append(head, tail, label_continue); ir_append(head, tail, step); ir_append(head, tail, jump_start); ir_append(head, tail, label_end);
        context->break_label = old_break; context->continue_label = old_continue;
        break;
    }
    case NODE_SWITCH: {
        if (node->a == NULL || !type_is_integer(node->a->type)) {
            lower_error(context, 3013U, node, "switch condition must have integer type");
        }
        size_t end_label = new_label(context);
        CaseLabel *case_head = NULL;
        CaseLabel *case_tail = NULL;
        size_t case_count = 0U;
        size_t default_label = end_label;
        bool has_default = false;
        collect_cases(context, node->b, &case_head, &case_tail, &case_count,
                      &default_label, &has_default);
        IrInst *dispatch = ir_new(context, IR_SWITCH,
                                   type_basic(context->arena, TYPE_LONG), node);
        if (dispatch != NULL) {
            dispatch->a = lower_expr(context, node->a);
            dispatch->default_label = default_label;
            dispatch->end_label = end_label;
            dispatch->case_count = case_count;
            IrCase *case_tail_ir = NULL;
            for (CaseLabel *entry = case_head; entry != NULL; entry = entry->next) {
                if (entry->node == NULL || entry->node->kind != NODE_CASE) continue;
                IrCase *item = arena_alloc(context->arena, sizeof(*item));
                if (item == NULL) { context->failed = true; break; }
                item->value = normalize_case_value(node->a == NULL ? NULL : node->a->type,
                                                     entry->value);
                item->label = entry->id;
                item->next = NULL;
                if (case_tail_ir == NULL) dispatch->cases = item;
                else case_tail_ir->next = item;
                case_tail_ir = item;
            }
        }
        size_t old_break = context->break_label;
        size_t old_continue = context->continue_label;
        CaseLabel *old_cases = context->cases;
        context->break_label = end_label;
        context->continue_label = old_continue;
        context->cases = case_head;
        IrInst *body_head = NULL;
        IrInst *body_tail = NULL;
        lower_statement(context, node->b, &body_head, &body_tail);
        IrInst *label_end = ir_new(context, IR_LABEL, NULL, node);
        if (label_end != NULL) label_end->id = end_label;
        ir_append(head, tail, dispatch);
        ir_append_list(head, tail, body_head, body_tail);
        ir_append(head, tail, label_end);
        context->break_label = old_break;
        context->continue_label = old_continue;
        context->cases = old_cases;
        break;
    }
    case NODE_CASE:
    case NODE_DEFAULT: {
        CaseLabel *entry = find_case(context, node);
        if (entry == NULL) {
            lower_error(context, 3012U, node, "case label is not attached to a switch");
            break;
        }
        IrInst *label = ir_new(context, IR_LABEL, NULL, node);
        if (label != NULL) label->id = entry->id;
        ir_append(head, tail, label);
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

/* A declaration statement carries one node per declarator, chained through
   next. Walk the whole chain so that no declarator is silently dropped. */
static void lower_statement(LowerContext *context, AstNode *node,
                            IrInst **head, IrInst **tail)
{
    for (; node != NULL; node = node->next) {
        lower_one_statement(context, node, head, tail);
    }
}

static void lower_statement_list(LowerContext *context, AstNode *node,
                                 IrInst **head, IrInst **tail)
{
    lower_statement(context, node, head, tail);
}

bool lower_translation_unit(Arena *arena, const TranslationUnit *unit,
                            DiagnosticSink *diagnostics, IrProgram *program)
{
    memset(program, 0, sizeof(*program));
    program->globals = unit->globals;
    LowerContext globals = {
        arena, diagnostics, unit, program, NULL, 0U, 0U, 0U, 0U,
        NULL, NULL, NULL, 0U, false
    };
    for (size_t i = 0U; i < unit->count; ++i) {
        AstNode *declaration = unit->declarations[i];
        if (declaration == NULL) continue;
        if (declaration->kind == NODE_DECLARATION && declaration->symbol != NULL &&
            declaration->symbol->class == SYMBOL_VARIABLE) {
            if (!program_add_global(&globals, declaration->symbol)) return false;
        }
        collect_literals(&globals, declaration);
        collect_static_symbols(&globals, declaration);
    }
    if (globals.failed) return false;
    for (size_t i = 0U; i < unit->count; ++i) {
        AstNode *declaration = unit->declarations[i];
        if (declaration == NULL || declaration->kind != NODE_FUNCTION_DEFINITION) continue;
        LowerContext context = {
            arena, diagnostics, unit, program, declaration->symbol, 0U, 0U, 0U,
            0U, NULL, NULL, NULL, 0U, false
        };
        collect_static_symbols(&context, declaration->a);
        collect_literals(&context, declaration->a);
        assign_frame(&context, declaration->symbol, declaration->a);
        if (context.failed) return false;
        IrFunction *function = program->functions;
        while (function != NULL && function->symbol != declaration->symbol) function = function->next;
        IrInst *head = NULL;
        IrInst *tail = NULL;
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
