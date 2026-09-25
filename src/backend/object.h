#ifndef CC64_OBJECT_H
#define CC64_OBJECT_H

#include "ir/ir.h"

#define CC64O_HEADER_SIZE 96U
#define CC64O_SECTION_SIZE 40U
#define CC64O_SYMBOL_SIZE 32U
#define CC64O_RELOC_SIZE 32U

#define CC64O_REL_ABS32 1U
#define CC64O_REL_ABS64 2U
#define CC64O_REL_PC32 3U
#define CC64O_REL_SECTION32 4U
#define CC64O_REL_DATA64 5U

typedef struct ObjectSection {
    char *name;
    unsigned char kind;
    unsigned alignment;
    unsigned char *data;
    size_t size;
    size_t capacity;
    IrRelocation *relocations;
} ObjectSection;

typedef struct ObjectSymbol {
    char *name;
    uint64_t value;
    uint32_t section_index;
    unsigned char binding;
    unsigned char kind;
    uint64_t size;
    bool defined;
    Symbol *source_symbol;
} ObjectSymbol;

typedef struct ObjectBuilder {
    Arena *arena;
    ObjectSection *sections;
    size_t section_count;
    size_t section_capacity;
    ObjectSymbol *symbols;
    size_t symbol_count;
    size_t symbol_capacity;
} ObjectBuilder;

void object_builder_init(ObjectBuilder *builder, Arena *arena);
void object_builder_destroy(ObjectBuilder *builder);
ObjectSection *object_add_section(ObjectBuilder *builder, const char *name,
                                  unsigned char kind, unsigned alignment);
bool object_append_data(ObjectBuilder *builder, ObjectSection *section,
                        const unsigned char *data, size_t size);
bool object_add_symbol(ObjectBuilder *builder, const char *name, uint64_t value,
                       uint32_t section_index, unsigned char binding,
                       unsigned char kind, uint64_t size, bool defined);
bool object_add_relocation(ObjectBuilder *builder, ObjectSection *section,
                           size_t offset, Symbol *symbol, uint32_t type,
                           int64_t addend, uint32_t width);
bool object_write_cc64o(ObjectBuilder *builder, const char *path,
                        DiagnosticSink *diagnostics);

uint32_t cc64_crc32(const unsigned char *data, size_t size);
uint32_t cc64_file_crc32(unsigned char *data, size_t size);

#endif
