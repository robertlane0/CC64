#include "linker/linker.h"

#include "backend/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct LinkRelocation {
    uint64_t offset;
    uint32_t symbol_index;
    uint32_t type;
    int64_t addend;
    uint32_t width;
} LinkRelocation;

typedef struct LinkSection {
    char *name;
    unsigned char kind;
    unsigned alignment;
    unsigned char *data;
    size_t size;
    LinkRelocation *relocations;
    size_t relocation_count;
    size_t output_offset;
    size_t output_kind;
} LinkSection;

typedef struct LinkSymbol {
    char *name;
    uint64_t value;
    uint32_t section_index;
    unsigned char binding;
    unsigned char kind;
    uint64_t size;
    bool defined;
} LinkSymbol;

typedef struct LinkObject {
    unsigned char *bytes;
    size_t size;
    LinkSection *sections;
    size_t section_count;
    LinkSymbol *symbols;
    size_t symbol_count;
} LinkObject;

typedef struct GlobalSymbol {
    const char *name;
    uint64_t address;
    uint64_t size;
    bool defined;
} GlobalSymbol;

typedef struct OutputSection {
    unsigned char kind;
    unsigned alignment;
    unsigned char *data;
    size_t size;
    size_t capacity;
    size_t output_offset;
} OutputSection;

static uint16_t read_u16(const unsigned char *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

static uint32_t read_u32(const unsigned char *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t read_u64(const unsigned char *data)
{
    uint64_t value = 0U;
    for (unsigned i = 0U; i < 8U; ++i) value |= (uint64_t)data[i] << (i * 8U);
    return value;
}

static int64_t read_i64(const unsigned char *data)
{
    return (int64_t)read_u64(data);
}

static void write_u32(unsigned char *data, uint32_t value)
{
    for (unsigned i = 0U; i < 4U; ++i) data[i] = (unsigned char)(value >> (i * 8U));
}

static void write_u64(unsigned char *data, uint64_t value)
{
    for (unsigned i = 0U; i < 8U; ++i) data[i] = (unsigned char)(value >> (i * 8U));
}

static bool range_ok(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static uint32_t link_crc32(const unsigned char *data, size_t size)
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

static void link_error(DiagnosticSink *diagnostics, unsigned id, const char *path,
                       const char *message)
{
    char text[512];
    (void)snprintf(text, sizeof(text), "%s: %s", path == NULL ? "linker" : path, message);
    diagnostic_emit(diagnostics, id, DIAG_LINK, NULL, 0U, 0U, text);
}

static bool read_object_file(const char *path, LinkObject *object,
                             DiagnosticSink *diagnostics)
{
    memset(object, 0, sizeof(*object));
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        link_error(diagnostics, 5001U, path, "cannot open object");
        return false;
    }
    if (fseek(stream, 0L, SEEK_END) != 0) {
        fclose(stream); link_error(diagnostics, 5002U, path, "cannot seek object"); return false;
    }
    long end = ftell(stream);
    if (end < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        fclose(stream); link_error(diagnostics, 5002U, path, "cannot size object"); return false;
    }
    object->size = (size_t)end;
    object->bytes = cc64_xmalloc(object->size == 0U ? 1U : object->size);
    if (object->size != 0U && fread(object->bytes, 1U, object->size, stream) != object->size) {
        fclose(stream); free(object->bytes); object->bytes = NULL;
        link_error(diagnostics, 5003U, path, "cannot read object"); return false;
    }
    fclose(stream);
    if (object->size < CC64O_HEADER_SIZE || memcmp(object->bytes, "CC64OBJ\0", 8U) != 0) {
        link_error(diagnostics, 5004U, path, "invalid CC64O magic"); return false;
    }
    if (read_u16(object->bytes + 8U) != 1U || read_u16(object->bytes + 10U) != 0x3433U ||
        read_u16(object->bytes + 12U) != 1U || read_u16(object->bytes + 14U) != 62U ||
        read_u32(object->bytes + 16U) != CC64O_HEADER_SIZE) {
        link_error(diagnostics, 5005U, path, "unsupported object contract"); return false;
    }
    uint32_t target_offset = read_u32(object->bytes + 20U);
    uint32_t target_size = read_u32(object->bytes + 24U);
    if (!range_ok(target_offset, target_size, object->size) || target_size == 0U ||
        target_size < strlen(CC64_TARGET) + 1U ||
        memcmp(object->bytes + target_offset, CC64_TARGET, strlen(CC64_TARGET)) != 0) {
        link_error(diagnostics, 5006U, path, "object target triple is invalid"); return false;
    }
    if (link_crc32(object->bytes, 64U) != read_u32(object->bytes + 64U) ||
        link_crc32(object->bytes, 68U) != read_u32(object->bytes + 68U)) {
        link_error(diagnostics, 5007U, path, "object CRC mismatch"); return false;
    }
    uint32_t section_offset = read_u32(object->bytes + 28U);
    uint32_t section_count = read_u32(object->bytes + 32U);
    uint32_t symbol_offset = read_u32(object->bytes + 36U);
    uint32_t symbol_count = read_u32(object->bytes + 40U);
    uint32_t string_offset = read_u32(object->bytes + 44U);
    uint32_t string_size = read_u32(object->bytes + 48U);
    if (!range_ok(section_offset, (size_t)section_count * CC64O_SECTION_SIZE, object->size) ||
        !range_ok(symbol_offset, (size_t)symbol_count * CC64O_SYMBOL_SIZE, object->size) ||
        !range_ok(string_offset, string_size, object->size) || string_size == 0U) {
        link_error(diagnostics, 5008U, path, "object tables are truncated"); return false;
    }
    object->section_count = section_count;
    object->sections = cc64_xmalloc((section_count == 0U ? 1U : section_count) * sizeof(*object->sections));
    object->symbol_count = symbol_count;
    object->symbols = cc64_xmalloc((symbol_count == 0U ? 1U : symbol_count) * sizeof(*object->symbols));
    const unsigned char *strings = object->bytes + string_offset;
    for (uint32_t i = 0U; i < section_count; ++i) {
        const unsigned char *record = object->bytes + section_offset + i * CC64O_SECTION_SIZE;
        LinkSection *section = &object->sections[i];
        uint32_t name_offset = read_u32(record);
        uint32_t payload_offset = read_u32(record + 4U);
        uint32_t payload_size = read_u32(record + 8U);
        uint32_t reloc_offset = read_u32(record + 12U);
        uint32_t reloc_count = read_u32(record + 16U);
        uint32_t alignment_power = read_u32(record + 20U);
        section->kind = record[24];
        section->alignment = alignment_power > 15U ? 0U : 1U << alignment_power;
        if (name_offset >= string_size || section->kind > 3U || section->alignment == 0U ||
            (section->kind != 3U && !range_ok(payload_offset, payload_size, object->size)) ||
            (reloc_count != 0U && !range_ok(reloc_offset, (size_t)reloc_count * CC64O_RELOC_SIZE, object->size))) {
            link_error(diagnostics, 5009U, path, "invalid object section"); return false;
        }
        uint32_t name_end = name_offset;
        while (name_end < string_size && strings[name_end] != 0U) ++name_end;
        if (name_end == string_size) { link_error(diagnostics, 5010U, path, "invalid section name"); return false; }
        size_t name_length = name_end - name_offset;
        section->name = cc64_xmalloc(name_length + 1U);
        memcpy(section->name, strings + name_offset, name_length);
        section->name[name_length] = '\0';
        section->size = payload_size;
        section->data = section->kind == 3U ? NULL : object->bytes + payload_offset;
        section->relocation_count = reloc_count;
        section->relocations = reloc_count == 0U ? NULL : cc64_xmalloc(reloc_count * sizeof(*section->relocations));
        for (uint32_t j = 0U; j < reloc_count; ++j) {
            const unsigned char *entry = object->bytes + reloc_offset + j * CC64O_RELOC_SIZE;
            section->relocations[j].offset = read_u64(entry);
            section->relocations[j].symbol_index = read_u32(entry + 8U);
            section->relocations[j].type = read_u32(entry + 12U);
            section->relocations[j].addend = read_i64(entry + 16U);
            section->relocations[j].width = read_u32(entry + 24U);
            if (section->relocations[j].symbol_index >= symbol_count ||
                (section->relocations[j].width != 4U && section->relocations[j].width != 8U) ||
                section->relocations[j].type < 1U || section->relocations[j].type > 5U ||
                section->relocations[j].offset > section->size ||
                section->relocations[j].width > section->size - section->relocations[j].offset) {
                link_error(diagnostics, 5011U, path, "invalid object relocation"); return false;
            }
        }
    }
    for (uint32_t i = 0U; i < symbol_count; ++i) {
        const unsigned char *record = object->bytes + symbol_offset + i * CC64O_SYMBOL_SIZE;
        uint32_t name_offset = read_u32(record);
        if (name_offset >= string_size) { link_error(diagnostics, 5012U, path, "invalid symbol name"); return false; }
        uint32_t name_end = name_offset;
        while (name_end < string_size && strings[name_end] != 0U) ++name_end;
        if (name_end == string_size) { link_error(diagnostics, 5013U, path, "unterminated symbol name"); return false; }
        size_t name_length = name_end - name_offset;
        LinkSymbol *symbol = &object->symbols[i];
        symbol->name = cc64_xmalloc(name_length + 1U);
        memcpy(symbol->name, strings + name_offset, name_length);
        symbol->name[name_length] = '\0';
        symbol->value = read_u32(record + 4U);
        symbol->section_index = read_u32(record + 8U);
        symbol->binding = record[12];
        symbol->kind = record[13];
        symbol->size = read_u64(record + 16U);
        symbol->defined = symbol->section_index != UINT32_MAX;
        if (symbol->section_index != UINT32_MAX && symbol->section_index >= section_count) {
            link_error(diagnostics, 5014U, path, "symbol section is out of range"); return false;
        }
    }
    return true;
}

static void free_link_object(LinkObject *object)
{
    if (object == NULL) return;
    for (size_t i = 0U; i < object->section_count; ++i) {
        free(object->sections[i].name);
        free(object->sections[i].relocations);
    }
    for (size_t i = 0U; i < object->symbol_count; ++i) free(object->symbols[i].name);
    free(object->sections);
    free(object->symbols);
    free(object->bytes);
    memset(object, 0, sizeof(*object));
}

static bool output_append(OutputSection *section, const unsigned char *data, size_t size)
{
    if (size > SIZE_MAX - section->size) return false;
    size_t needed = section->size + size;
    if (needed > section->capacity) {
        size_t next = section->capacity == 0U ? 256U : section->capacity;
        while (next < needed) {
            if (next > SIZE_MAX / 2U) { next = needed; break; }
            next *= 2U;
        }
        section->data = cc64_xrealloc(section->data, next);
        section->capacity = next;
    }
    if (size != 0U) memcpy(section->data + section->size, data, size);
    section->size = needed;
    return true;
}

static GlobalSymbol *find_global(GlobalSymbol *globals, size_t count, const char *name)
{
    for (size_t i = 0U; i < count; ++i) if (strcmp(globals[i].name, name) == 0) return &globals[i];
    return NULL;
}

static bool add_global(GlobalSymbol **globals, size_t *count, size_t *capacity,
                       const char *name)
{
    if (find_global(*globals, *count, name) != NULL) return true;
    if (*count == *capacity) {
        size_t next = *capacity == 0U ? 16U : *capacity * 2U;
        GlobalSymbol *grown = cc64_xrealloc(*globals, next * sizeof(*grown));
        *globals = grown;
        *capacity = next;
    }
    GlobalSymbol *symbol = &(*globals)[(*count)++];
    memset(symbol, 0, sizeof(*symbol));
    symbol->name = cc64_xstrdup(name);
    return true;
}

static bool align_output(OutputSection *section, size_t alignment, size_t *offset)
{
    if (alignment == 0U) alignment = 1U;
    size_t mask = alignment - 1U;
    if (*offset > SIZE_MAX - mask) return false;
    *offset = (*offset + mask) & ~mask;
    section->output_offset = *offset;
    return true;
}

static bool resolve_symbols(LinkObject *objects, size_t object_count,
                            OutputSection outputs[4], GlobalSymbol **globals_out,
                            size_t *global_count, size_t *global_capacity,
                            DiagnosticSink *diagnostics)
{
    for (size_t o = 0U; o < object_count; ++o) {
        LinkObject *object = &objects[o];
        for (size_t s = 0U; s < object->symbol_count; ++s) {
            LinkSymbol *symbol = &object->symbols[s];
            if (symbol->section_index != UINT32_MAX) {
                LinkSection *section = &object->sections[symbol->section_index];
                if (section->output_offset > UINT64_MAX - symbol->value) return false;
                symbol->value += outputs[section->output_kind].output_offset + section->output_offset;
                symbol->defined = true;
            }
            if (symbol->binding == 1U) {
                GlobalSymbol *global = NULL;
                if (!add_global(globals_out, global_count, global_capacity, symbol->name)) return false;
                global = find_global(*globals_out, *global_count, symbol->name);
                if (global->defined && symbol->defined) {
                    link_error(diagnostics, 5020U, NULL, "duplicate global definition");
                    return false;
                }
                if (symbol->defined) { global->address = symbol->value; global->size = symbol->size; global->defined = true; }
            }
        }
    }
    return true;
}

static bool apply_relocations(LinkObject *objects, size_t object_count,
                              OutputSection outputs[4], GlobalSymbol *globals,
                              size_t global_count, bool raw_format,
                              DiagnosticSink *diagnostics)
{
    for (size_t o = 0U; o < object_count; ++o) {
        LinkObject *object = &objects[o];
        for (size_t s = 0U; s < object->section_count; ++s) {
            LinkSection *section = &object->sections[s];
            OutputSection *output = &outputs[section->output_kind];
            for (size_t r = 0U; r < section->relocation_count; ++r) {
                LinkRelocation *reloc = &section->relocations[r];
                LinkSymbol *symbol = &object->symbols[reloc->symbol_index];
                uint64_t value = symbol->value;
                if (symbol->binding == 1U) {
                    GlobalSymbol *global = find_global(globals, global_count, symbol->name);
                    if (global == NULL || !global->defined) { link_error(diagnostics, 5021U, NULL, "unresolved symbol"); return false; }
                    value = global->address;
                }
                if (raw_format && (reloc->type == CC64O_REL_ABS64 || reloc->type == CC64O_REL_DATA64 || reloc->type == CC64O_REL_ABS32)) {
                    link_error(diagnostics, 5022U, NULL, "load-biased relocation is not valid in raw COM"); return false;
                }
                size_t position = output->output_offset + section->output_offset + (size_t)reloc->offset;
                if (position > output->size || reloc->width > output->size - position) { link_error(diagnostics, 5023U, NULL, "relocation is outside output section"); return false; }
                if (reloc->type == CC64O_REL_PC32) {
                    int64_t pc = (int64_t)(position + reloc->width);
                    int64_t result = (int64_t)value + reloc->addend - pc;
                    if (result < INT32_MIN || result > INT32_MAX) { link_error(diagnostics, 5024U, NULL, "PC-relative relocation overflow"); return false; }
                    write_u32(output->data + position, (uint32_t)(int32_t)result);
                } else if (reloc->type == CC64O_REL_SECTION32) {
                    uint64_t result = symbol->value + (uint64_t)reloc->addend;
                    if (result > UINT32_MAX) { link_error(diagnostics, 5025U, NULL, "section relocation overflow"); return false; }
                    write_u32(output->data + position, (uint32_t)result);
                } else {
                    uint64_t result = value + (uint64_t)reloc->addend;
                    if (reloc->width == 4U && result > UINT32_MAX) { link_error(diagnostics, 5026U, NULL, "32-bit relocation overflow"); return false; }
                    if (reloc->width == 4U) write_u32(output->data + position, (uint32_t)result);
                    else write_u64(output->data + position, result);
                }
            }
        }
    }
    return true;
}

bool link_objects(Arena *arena, const char *const *objects, size_t object_count,
                  const char *output, ImageFormat format,
                  DiagnosticSink *diagnostics)
{
    (void)arena;
    if (format != IMAGE_RAW_COM) { link_error(diagnostics, 5040U, output, "image format is not implemented"); return false; }
    if (object_count == 0U || output == NULL) { link_error(diagnostics, 5041U, output, "linker requires objects and output"); return false; }
    LinkObject *loaded = cc64_xmalloc(object_count * sizeof(*loaded));
    OutputSection outputs[4] = {0};
    outputs[0].kind = 0; outputs[0].alignment = 16U;
    outputs[1].kind = 1; outputs[1].alignment = 16U;
    outputs[2].kind = 2; outputs[2].alignment = 8U;
    outputs[3].kind = 3; outputs[3].alignment = 8U;
    GlobalSymbol *globals = NULL;
    size_t global_count = 0U;
    size_t global_capacity = 0U;
    bool good = true;
    for (size_t i = 0U; i < object_count; ++i) {
        if (!read_object_file(objects[i], &loaded[i], diagnostics)) { good = false; break; }
    }
    if (good) {
        /* Reserve the target entry trampoline in the text section. */
        static const unsigned char entry_stub[] = {
            0x31U, 0xffU, 0x31U, 0xf6U, 0xe8U, 0U, 0U, 0U, 0U,
            0x89U, 0xc1U, 0xb8U, 0U, 0x4cU, 0U, 0U, 0x88U,
            0xc8U, 0xcdU, 0x21U, 0xc3U
        };
        if (!output_append(&outputs[0], entry_stub, sizeof(entry_stub))) good = false;
        for (size_t o = 0U; o < object_count && good; ++o) {
            for (size_t s = 0; s < loaded[o].section_count; ++s) {
                LinkSection *section = &loaded[o].sections[s];
                unsigned output_kind = section->kind;
                if (!align_output(&outputs[output_kind], section->alignment, &outputs[output_kind].size)) { good = false; break; }
                section->output_offset = outputs[output_kind].size;
                section->output_kind = output_kind;
                if (section->kind != 3U && !output_append(&outputs[output_kind], section->data, section->size)) { good = false; break; }
                if (section->kind == 3U) outputs[output_kind].size += section->size;
            }
        }
    }
    if (good) {
        size_t base = 0U;
        for (size_t i = 0U; i < 4U; ++i) {
            if (!align_output(&outputs[i], outputs[i].alignment, &base)) { good = false; break; }
            outputs[i].output_offset = base;
            base += outputs[i].size;
        }
        /* The section offsets above are filled again after each output size is known. */
        if (good) {
            base = 0U;
            for (size_t i = 0U; i < 4U; ++i) {
                outputs[i].output_offset = base;
                base += outputs[i].size;
            }
        }
    }
    if (good) {
        /* Recompute each input section's final offset after output layout. */
        for (size_t o = 0U; o < object_count; ++o) {
            for (size_t s = 0; s < loaded[o].section_count; ++s) {
                LinkSection *section = &loaded[o].sections[s];
                section->output_offset = outputs[section->output_kind].output_offset + section->output_offset - outputs[section->output_kind].output_offset;
            }
        }
        if (!resolve_symbols(loaded, object_count, outputs, &globals, &global_count,
                             &global_capacity, diagnostics)) good = false;
    }
    if (good && !apply_relocations(loaded, object_count, outputs, globals,
                                   global_count, true, diagnostics)) good = false;
    if (good) {
        GlobalSymbol *main_symbol = find_global(globals, global_count, "main");
        if (main_symbol == NULL || !main_symbol->defined) { link_error(diagnostics, 5027U, output, "undefined main symbol"); good = false; }
        else {
            size_t field = outputs[0].output_offset + 5U;
            int64_t displacement = (int64_t)main_symbol->address -
                                   (int64_t)(field + 4U);
            if (displacement < INT32_MIN || displacement > INT32_MAX) { link_error(diagnostics, 5028U, output, "entry trampoline overflow"); good = false; }
            else write_u32(outputs[0].data + field, (uint32_t)(int32_t)displacement);
        }
    }
    if (good) {
        size_t total = 0U;
        for (size_t i = 0U; i < 4U; ++i) {
            if (outputs[i].output_offset > SIZE_MAX - outputs[i].size) { good = false; break; }
            size_t end = outputs[i].output_offset + outputs[i].size;
            if (end > total) total = end;
        }
        if (good) {
            /* Reconstruct contiguous output because BSS is zero-filled. */
            unsigned char *image = calloc(total == 0U ? 1U : total, 1U);
            if (image == NULL) { good = false; }
            else {
            for (size_t i = 0U; i < 4U; ++i) {
                if (outputs[i].size != 0U && outputs[i].data != NULL) {
                    memcpy(image + outputs[i].output_offset, outputs[i].data, outputs[i].size);
                }
            }
            /* Write through a temporary flat buffer using a small helper path. */
            size_t path_len = strlen(output) + 5U;
            char *temporary = cc64_xmalloc(path_len);
            (void)snprintf(temporary, path_len, "%s.tmp", output);
            FILE *stream = fopen(temporary, "wb");
            bool wrote = stream != NULL && (total == 0U || fwrite(image, 1U, total, stream) == total);
            if (stream != NULL && fclose(stream) != 0) wrote = false;
            if (wrote && rename(temporary, output) != 0) wrote = false;
            if (!wrote) { (void)remove(temporary); link_error(diagnostics, 5029U, output, "cannot write raw image"); good = false; }
            free(temporary); free(image);
            }
        }
    }
    for (size_t i = 0; i < object_count; ++i) free_link_object(&loaded[i]);
    for (size_t i = 0; i < global_count; ++i) free((void *)globals[i].name);
    free(globals);
    for (size_t i = 0; i < 4U; ++i) free(outputs[i].data);
    free(loaded);
    return good;
}
