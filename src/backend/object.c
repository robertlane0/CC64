#include "backend/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t find_symbol(const ObjectBuilder *builder, Symbol *symbol);

static bool size_add_size(size_t left, size_t right, size_t *result)
{
    if (right > SIZE_MAX - left) return false;
    *result = left + right;
    return true;
}

static bool size_mul_size(size_t left, size_t right, size_t *result)
{
    if (left != 0U && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

void object_builder_init(ObjectBuilder *builder, Arena *arena)
{
    memset(builder, 0, sizeof(*builder));
    builder->arena = arena;
}

void object_builder_destroy(ObjectBuilder *builder)
{
    if (builder == NULL) return;
    for (size_t i = 0U; i < builder->section_count; ++i) {
        free(builder->sections[i].data);
    }
    free(builder->sections);
    free(builder->symbols);
    memset(builder, 0, sizeof(*builder));
}

ObjectSection *object_add_section(ObjectBuilder *builder, const char *name,
                                  unsigned char kind, unsigned alignment)
{
    if (builder->section_count == builder->section_capacity) {
        size_t next = builder->section_capacity == 0U ? 4U : builder->section_capacity * 2U;
        if (next < builder->section_capacity) return NULL;
        ObjectSection *sections = cc64_xrealloc(builder->sections, next * sizeof(*sections));
        builder->sections = sections;
        builder->section_capacity = next;
    }
    ObjectSection *section = &builder->sections[builder->section_count++];
    memset(section, 0, sizeof(*section));
    section->name = cc64_xstrdup(name);
    section->kind = kind;
    section->alignment = alignment;
    return section;
}

bool object_append_data(ObjectBuilder *builder, ObjectSection *section,
                        const unsigned char *data, size_t size)
{
    if (section == NULL || (size != 0U && data == NULL)) return false;
    if (size > SIZE_MAX - section->size) return false;
    size_t needed = section->size + size;
    if (needed > section->capacity) {
        size_t next = section->capacity == 0U ? 64U : section->capacity;
        while (next < needed) {
            if (next > SIZE_MAX / 2U) { next = needed; break; }
            next *= 2U;
        }
        section->data = cc64_xrealloc(section->data, next);
        section->capacity = next;
    }
    if (size != 0U) memcpy(section->data + section->size, data, size);
    section->size = needed;
    (void)builder;
    return true;
}

bool object_add_symbol(ObjectBuilder *builder, const char *name, uint64_t value,
                       uint32_t section_index, unsigned char binding,
                       unsigned char kind, uint64_t size, bool defined)
{
    if (builder->symbol_count == builder->symbol_capacity) {
        size_t next = builder->symbol_capacity == 0U ? 16U : builder->symbol_capacity * 2U;
        if (next < builder->symbol_capacity) return false;
        ObjectSymbol *symbols = cc64_xrealloc(builder->symbols, next * sizeof(*symbols));
        builder->symbols = symbols;
        builder->symbol_capacity = next;
    }
    ObjectSymbol *symbol = &builder->symbols[builder->symbol_count++];
    memset(symbol, 0, sizeof(*symbol));
    symbol->name = cc64_xstrdup(name);
    symbol->value = value;
    symbol->section_index = section_index;
    symbol->binding = binding;
    symbol->kind = kind;
    symbol->size = size;
    symbol->defined = defined;
    return true;
}

bool object_add_relocation(ObjectBuilder *builder, ObjectSection *section,
                           size_t offset, Symbol *symbol, uint32_t type,
                           int64_t addend, uint32_t width)
{
    if (find_symbol(builder, symbol) == UINT32_MAX) {
        unsigned char binding = (unsigned char)(symbol != NULL &&
                                                 symbol->linkage == LINKAGE_INTERNAL ? 0U : 1U);
        unsigned char kind = (unsigned char)(symbol != NULL &&
                                             symbol->class == SYMBOL_FUNCTION ? 2U : 1U);
        if (!object_add_symbol(builder, symbol == NULL ? "" : symbol->name,
                               0U, UINT32_MAX, binding, kind, 0U, false)) return false;
        builder->symbols[builder->symbol_count - 1U].source_symbol = symbol;
    }
    IrRelocation *reloc = cc64_xmalloc(sizeof(*reloc));
    reloc->offset = offset;
    reloc->symbol = symbol;
    reloc->type = type;
    reloc->addend = addend;
    reloc->width = width;
    reloc->next = section->relocations;
    section->relocations = reloc;
    (void)builder;
    return true;
}

static void put_u16(unsigned char *data, uint16_t value)
{
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8);
    data[2] = (unsigned char)(value >> 16);
    data[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *data, uint64_t value)
{
    for (unsigned i = 0U; i < 8U; ++i) data[i] = (unsigned char)(value >> (i * 8U));
}

uint32_t cc64_crc32(const unsigned char *data, size_t size)
{
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0U; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

uint32_t cc64_file_crc32(unsigned char *data, size_t size)
{
    if (data == NULL || size < 72U) return 0U;
    unsigned char saved[4];
    memcpy(saved, data + 68U, sizeof(saved));
    memset(data + 68U, 0, sizeof(saved));
    uint32_t crc = cc64_crc32(data, size);
    memcpy(data + 68U, saved, sizeof(saved));
    return crc;
}

typedef struct StringTable {
    unsigned char *bytes;
    size_t size;
    size_t capacity;
    const char **names;
    size_t count;
    size_t name_capacity;
} StringTable;

static bool string_add(StringTable *table, const char *text, uint32_t *offset)
{
    for (size_t i = 0U; i < table->count; ++i) {
        if (strcmp(table->names[i], text) == 0) {
            uint32_t current = 1U;
            for (size_t j = 0U; j < i; ++j) current += (uint32_t)strlen(table->names[j]) + 1U;
            *offset = current;
            return true;
        }
    }
    if (table->count == table->name_capacity) {
        size_t next = table->name_capacity == 0U ? 16U : table->name_capacity * 2U;
        const char **names = cc64_xrealloc((void *)table->names, next * sizeof(*names));
        table->names = names;
        table->name_capacity = next;
    }
    size_t length = strlen(text) + 1U;
    if (table->size > SIZE_MAX - length) return false;
    size_t needed = table->size + length;
    if (needed > table->capacity) {
        size_t next = table->capacity == 0U ? 64U : table->capacity;
        while (next < needed) {
            if (next > SIZE_MAX / 2U) { next = needed; break; }
            next *= 2U;
        }
        table->bytes = cc64_xrealloc(table->bytes, next);
        table->capacity = next;
    }
    uint32_t result = (uint32_t)table->size;
    memcpy(table->bytes + table->size, text, length);
    table->size = needed;
    table->names[table->count++] = text;
    *offset = result;
    return true;
}

static size_t reloc_count(const ObjectSection *section)
{
    size_t count = 0U;
    for (IrRelocation *reloc = section->relocations; reloc != NULL; reloc = reloc->next) ++count;
    return count;
}

static bool add_size(size_t left, size_t right, size_t *result)
{
    return size_add_size(left, right, result);
}

static bool mul_size(size_t left, size_t right, size_t *result)
{
    return size_mul_size(left, right, result);
}

static uint32_t find_symbol(const ObjectBuilder *builder, Symbol *symbol)
{
    for (size_t i = 0U; i < builder->symbol_count; ++i) {
        if (builder->symbols[i].source_symbol == symbol) return (uint32_t)i;
        if (builder->symbols[i].source_symbol == NULL && symbol != NULL &&
            builder->symbols[i].name != NULL && symbol->name != NULL &&
            strcmp(builder->symbols[i].name, symbol->name) == 0) return (uint32_t)i;
    }
    return UINT32_MAX;
}

static void free_string_table(StringTable *table)
{
    free(table->bytes);
    free((void *)table->names);
    memset(table, 0, sizeof(*table));
}

static int object_symbol_compare(const void *left, const void *right)
{
    const ObjectSymbol *a = left;
    const ObjectSymbol *b = right;
    if (a->binding != b->binding) return a->binding < b->binding ? -1 : 1;
    int name = strcmp(a->name, b->name);
    if (name != 0) return name;
    if (a->value != b->value) return a->value < b->value ? -1 : 1;
    if (a->section_index != b->section_index) {
        return a->section_index < b->section_index ? -1 : 1;
    }
    return 0;
}

bool object_write_cc64o(ObjectBuilder *builder, const char *path,
                        DiagnosticSink *diagnostics)
{
    if (builder == NULL || path == NULL) return false;
    qsort(builder->symbols, builder->symbol_count, sizeof(*builder->symbols),
          object_symbol_compare);
    StringTable strings = {0};
    uint32_t target_offset = 0U;
    if (!string_add(&strings, "", &target_offset) ||
        !string_add(&strings, CC64_TARGET, &target_offset)) {
        free_string_table(&strings);
        return false;
    }
    uint32_t *section_name_offsets = cc64_xmalloc(builder->section_count * sizeof(*section_name_offsets));
    for (size_t i = 0U; i < builder->section_count; ++i) {
        if (!string_add(&strings, builder->sections[i].name, &section_name_offsets[i])) {
            free(section_name_offsets); free_string_table(&strings); return false;
        }
    }
    uint32_t *symbol_name_offsets = cc64_xmalloc(builder->symbol_count * sizeof(*symbol_name_offsets));
    for (size_t i = 0U; i < builder->symbol_count; ++i) {
        if (!string_add(&strings, builder->symbols[i].name, &symbol_name_offsets[i])) {
            free(section_name_offsets); free(symbol_name_offsets); free_string_table(&strings); return false;
        }
    }
    size_t section_table_offset = CC64O_HEADER_SIZE;
    size_t symbol_table_offset = 0U;
    if (!add_size(section_table_offset, builder->section_count * CC64O_SECTION_SIZE, &symbol_table_offset)) {
        free(section_name_offsets); free(symbol_name_offsets); free_string_table(&strings); return false;
    }
    size_t tables_after_symbols = symbol_table_offset;
    if (!add_size(tables_after_symbols, builder->symbol_count * CC64O_SYMBOL_SIZE,
                  &tables_after_symbols) || symbol_table_offset > UINT32_MAX) {
        free(section_name_offsets); free(symbol_name_offsets); free_string_table(&strings); return false;
    }
    size_t relocation_offset = tables_after_symbols;
    for (size_t i = 0U; i < builder->section_count; ++i) {
        size_t bytes = 0U;
        if (!mul_size(reloc_count(&builder->sections[i]), CC64O_RELOC_SIZE, &bytes) ||
            !add_size(relocation_offset, bytes, &relocation_offset)) {
            free(section_name_offsets); free(symbol_name_offsets); free_string_table(&strings); return false;
        }
    }
    size_t string_offset_value = relocation_offset;
    size_t data_start = string_offset_value;
    if (!add_size(data_start, strings.size, &data_start)) {
        free(section_name_offsets); free(symbol_name_offsets); free_string_table(&strings); return false;
    }
    size_t total = data_start;
    for (size_t i = 0U; i < builder->section_count; ++i) {
        if (builder->sections[i].kind == 3U) continue;
        if (!add_size(total, builder->sections[i].size, &total)) {
            free(section_name_offsets); free(symbol_name_offsets); free_string_table(&strings); return false;
        }
    }
    unsigned char *file = cc64_xmalloc(total == 0U ? 1U : total);
    memset(file, 0, total == 0U ? 1U : total);
    if (strings.size != 0U) memcpy(file + string_offset_value, strings.bytes, strings.size);
    size_t data_cursor = data_start;
    uint32_t *data_offsets = cc64_xmalloc(builder->section_count * sizeof(*data_offsets));
    for (size_t i = 0U; i < builder->section_count; ++i) {
        ObjectSection *section = &builder->sections[i];
        if (section->kind == 3U) {
            data_offsets[i] = 0U;
        } else {
            data_offsets[i] = (uint32_t)data_cursor;
            if (section->size != 0U) memcpy(file + data_cursor, section->data, section->size);
            data_cursor += section->size;
        }
    }
    unsigned char *header = file;
    memcpy(header, "CC64OBJ\0", 8U);
    put_u16(header + 8U, 1U);
    put_u16(header + 10U, 0x3433U);
    put_u16(header + 12U, 1U);
    put_u16(header + 14U, 62U);
    put_u32(header + 16U, CC64O_HEADER_SIZE);
    put_u32(header + 20U, (uint32_t)(string_offset_value + target_offset));
    put_u32(header + 24U, (uint32_t)(strlen(CC64_TARGET) + 1U));
    put_u32(header + 28U, (uint32_t)section_table_offset);
    put_u32(header + 32U, (uint32_t)builder->section_count);
    put_u32(header + 36U, (uint32_t)symbol_table_offset);
    put_u32(header + 40U, (uint32_t)builder->symbol_count);
    put_u32(header + 44U, (uint32_t)string_offset_value);
    put_u32(header + 48U, (uint32_t)strings.size);
    memset(header + 52U, 0, 44U);
    put_u32(header + 64U, cc64_crc32(header, 64U));
    for (size_t i = 0U; i < builder->section_count; ++i) {
        unsigned char *record = file + section_table_offset + i * CC64O_SECTION_SIZE;
        ObjectSection *section = &builder->sections[i];
        put_u32(record, section_name_offsets[i]);
        put_u32(record + 4U, data_offsets[i]);
        put_u32(record + 8U, (uint32_t)section->size);
        size_t reloc_bytes = reloc_count(section) * CC64O_RELOC_SIZE;
        put_u32(record + 12U, reloc_bytes == 0U ? 0U : (uint32_t)(relocation_offset - (relocation_offset - reloc_bytes)));
        /* Relocation offsets are filled after the cumulative cursor is known. */
        put_u32(record + 16U, (uint32_t)reloc_count(section));
        unsigned alignment_power = 0U;
        while ((1U << alignment_power) < section->alignment && alignment_power < 15U) {
            ++alignment_power;
        }
        put_u32(record + 20U, alignment_power);
        record[24] = section->kind;
        record[25] = 0U;
        put_u16(record + 26U, (uint16_t)i);
        memset(record + 28U, 0, 8U);
        put_u32(record + 36U, section->kind == 3U ? 0U : cc64_crc32(section->data, section->size));
    }
    /* Rebuild section relocation offsets with a dedicated cursor. */
    size_t reloc_cursor = tables_after_symbols;
    for (size_t i = 0U; i < builder->section_count; ++i) {
        ObjectSection *section = &builder->sections[i];
        unsigned char *record = file + section_table_offset + i * CC64O_SECTION_SIZE;
        put_u32(record + 12U, reloc_count(section) == 0U ? 0U : (uint32_t)reloc_cursor);
        size_t count = reloc_count(section);
        IrRelocation **ordered = count == 0U ? NULL :
                                  cc64_xmalloc(count * sizeof(*ordered));
        size_t index = 0U;
        for (IrRelocation *reloc = section->relocations; reloc != NULL;
             reloc = reloc->next) ordered[index++] = reloc;
        for (size_t j = 0U; j < count; ++j) {
            for (size_t k = j + 1U; k < count; ++k) {
                if (ordered[k]->offset < ordered[j]->offset ||
                    (ordered[k]->offset == ordered[j]->offset &&
                     find_symbol(builder, ordered[k]->symbol) <
                     find_symbol(builder, ordered[j]->symbol))) {
                    IrRelocation *swap = ordered[j];
                    ordered[j] = ordered[k];
                    ordered[k] = swap;
                }
            }
        }
        for (size_t j = 0U; j < count; ++j) {
            unsigned char *entry = file + reloc_cursor;
            IrRelocation *reloc = ordered[j];
            put_u64(entry, reloc->offset);
            put_u32(entry + 8U, find_symbol(builder, reloc->symbol));
            put_u32(entry + 12U, reloc->type);
            put_u64(entry + 16U, (uint64_t)reloc->addend);
            put_u32(entry + 24U, reloc->width);
            put_u32(entry + 28U, 0U);
            reloc_cursor += CC64O_RELOC_SIZE;
        }
        free(ordered);
    }
    for (size_t i = 0U; i < builder->symbol_count; ++i) {
        unsigned char *record = file + symbol_table_offset + i * CC64O_SYMBOL_SIZE;
        ObjectSymbol *symbol = &builder->symbols[i];
        put_u32(record, symbol_name_offsets[i]);
        put_u32(record + 4U, (uint32_t)symbol->value);
        put_u32(record + 8U, symbol->section_index);
        record[12] = symbol->binding;
        record[13] = symbol->kind;
        put_u16(record + 14U, 0U);
        put_u64(record + 16U, symbol->size);
        put_u32(record + 24U, 0U);
        put_u32(record + 28U, 0U);
    }
    put_u32(header + 68U, cc64_file_crc32(file, total));
    size_t path_length = strlen(path) + 5U;
    char *temporary = cc64_xmalloc(path_length);
    (void)snprintf(temporary, path_length, "%s.tmp", path);
    FILE *stream = fopen(temporary, "wb");
    bool good = stream != NULL;
    if (good) {
        good = fwrite(file, 1U, total, stream) == total;
        if (fclose(stream) != 0) good = false;
        stream = NULL;
    }
    if (good && rename(temporary, path) != 0) good = false;
    if (!good) {
        if (stream != NULL) (void)fclose(stream);
        (void)remove(temporary);
        diagnostic_emit(diagnostics, 4001U, DIAG_BACKEND, NULL, 0U, 0U,
                        "cannot write CC64O output");
    }
    free(temporary);
    free(data_offsets);
    free(file);
    free(section_name_offsets);
    free(symbol_name_offsets);
    free_string_table(&strings);
    return good;
}
