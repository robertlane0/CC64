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
    uint64_t original_value;
    uint32_t section_index;
    unsigned char binding;
    unsigned char kind;
    uint64_t size;
    bool defined;
} LinkSymbol;

typedef struct ObjectRange {
    size_t offset;
    size_t length;
} ObjectRange;

typedef struct LinkObject {
    unsigned char *bytes;
    size_t size;
    LinkSection *sections;
    size_t section_count;
    LinkSymbol *symbols;
    size_t symbol_count;
    ObjectRange *ranges;
    size_t range_count;
    size_t range_capacity;
} LinkObject;

typedef struct GlobalSymbol {
    const char *name;
    uint64_t address;
    uint64_t size;
    bool defined;
    bool common;
} GlobalSymbol;

typedef struct OutputSection {
    unsigned char kind;
    unsigned alignment;
    unsigned char *data;
    size_t size;
    size_t capacity;
    size_t output_offset;
} OutputSection;

typedef struct ImageRelocation {
    uint64_t destination;
    int64_t addend;
} ImageRelocation;

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

static bool object_range_add(LinkObject *object, size_t offset, size_t length)
{
    if (length == 0U) return true;
    if (!range_ok(offset, length, object->size)) return false;
    size_t end = offset + length;
    for (size_t i = 0U; i < object->range_count; ++i) {
        size_t other_end = object->ranges[i].offset + object->ranges[i].length;
        if (offset < other_end && object->ranges[i].offset < end) return false;
    }
    if (object->range_count == object->range_capacity) {
        size_t next = object->range_capacity == 0U ? 16U : object->range_capacity * 2U;
        object->ranges = cc64_xrealloc(object->ranges, next * sizeof(*object->ranges));
        object->range_capacity = next;
    }
    object->ranges[object->range_count].offset = offset;
    object->ranges[object->range_count].length = length;
    ++object->range_count;
    return true;
}

static bool object_uncovered_bytes_are_zero(const LinkObject *object)
{
    unsigned char *covered = calloc(object->size == 0U ? 1U : object->size, 1U);
    if (covered == NULL) return false;
    for (size_t i = 0U; i < object->range_count; ++i) {
        for (size_t j = 0U; j < object->ranges[i].length; ++j) {
            covered[object->ranges[i].offset + j] = 1U;
        }
    }
    bool good = true;
    for (size_t i = 0U; i < object->size; ++i) {
        if (covered[i] == 0U && object->bytes[i] != 0U) {
            good = false;
            break;
        }
    }
    free(covered);
    return good;
}

static void free_link_object(LinkObject *object);

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
    if (object->size < CC64O_HEADER_SIZE || object->size > 512U * 1024U * 1024U ||
        memcmp(object->bytes, "CC64OBJ\0", 8U) != 0) {
        link_error(diagnostics, 5004U, path, "invalid CC64O magic"); return false;
    }
    if (read_u16(object->bytes + 8U) != 1U || read_u16(object->bytes + 10U) != 0x3433U ||
        read_u16(object->bytes + 12U) != 1U || read_u16(object->bytes + 14U) != 62U ||
        read_u32(object->bytes + 16U) != CC64O_HEADER_SIZE ||
        read_u32(object->bytes + 60U) != 0U) {
        link_error(diagnostics, 5005U, path, "unsupported object contract"); return false;
    }
    for (size_t i = 72U; i < CC64O_HEADER_SIZE; ++i) {
        if (object->bytes[i] != 0U) {
            link_error(diagnostics, 5008U, path, "object reserved bytes are nonzero");
            return false;
        }
    }
    uint32_t source_map_offset = read_u32(object->bytes + 52U);
    uint32_t source_map_size = read_u32(object->bytes + 56U);
    if ((source_map_offset == 0U) != (source_map_size == 0U) ||
        (source_map_size != 0U &&
         !range_ok(source_map_offset, source_map_size, object->size))) {
        link_error(diagnostics, 5008U, path, "object source map range is invalid");
        return false;
    }
    uint32_t target_offset = read_u32(object->bytes + 20U);
    uint32_t target_size = read_u32(object->bytes + 24U);
    if (!range_ok(target_offset, target_size, object->size) ||
        target_size != (uint32_t)strlen(CC64_TARGET) + 1U ||
        object->bytes[target_offset + target_size - 1U] != 0U ||
        memcmp(object->bytes + target_offset, CC64_TARGET, strlen(CC64_TARGET)) != 0) {
        link_error(diagnostics, 5006U, path, "object target triple is invalid"); return false;
    }
    if (link_crc32(object->bytes, 64U) != read_u32(object->bytes + 64U) ||
        cc64_file_crc32(object->bytes, object->size) != read_u32(object->bytes + 68U)) {
        link_error(diagnostics, 5007U, path, "object CRC mismatch"); return false;
    }
    uint32_t section_offset = read_u32(object->bytes + 28U);
    uint32_t section_count = read_u32(object->bytes + 32U);
    uint32_t symbol_offset = read_u32(object->bytes + 36U);
    uint32_t symbol_count = read_u32(object->bytes + 40U);
    uint32_t string_offset = read_u32(object->bytes + 44U);
    uint32_t string_size = read_u32(object->bytes + 48U);
    if (section_count > 65535U || symbol_count > 1000000U ||
        string_size == 0U || string_size > object->size ||
        section_offset < CC64O_HEADER_SIZE || symbol_offset < CC64O_HEADER_SIZE ||
        string_offset < CC64O_HEADER_SIZE ||
        !range_ok(section_offset, (size_t)section_count * CC64O_SECTION_SIZE, object->size) ||
        !range_ok(symbol_offset, (size_t)symbol_count * CC64O_SYMBOL_SIZE, object->size) ||
        !range_ok(string_offset, string_size, object->size)) {
        link_error(diagnostics, 5008U, path, "object tables are truncated"); return false;
    }
    if (!object_range_add(object, 0U, CC64O_HEADER_SIZE) ||
        !object_range_add(object, section_offset,
                          (size_t)section_count * CC64O_SECTION_SIZE) ||
        !object_range_add(object, symbol_offset,
                          (size_t)symbol_count * CC64O_SYMBOL_SIZE) ||
        !object_range_add(object, string_offset, string_size) ||
        (source_map_size != 0U &&
         !object_range_add(object, source_map_offset, source_map_size))) {
        link_error(diagnostics, 5008U, path, "object tables overlap"); return false;
    }
    bool target_in_string = target_offset >= string_offset &&
                            target_offset + target_size <= string_offset + string_size;
    if (!target_in_string &&
        !object_range_add(object, target_offset, target_size)) {
        link_error(diagnostics, 5008U, path, "object target range overlaps tables");
        return false;
    }
    object->section_count = section_count;
    object->sections = cc64_xmalloc((section_count == 0U ? 1U : section_count) * sizeof(*object->sections));
    memset(object->sections, 0, (section_count == 0U ? 1U : section_count) * sizeof(*object->sections));
    object->symbol_count = symbol_count;
    object->symbols = cc64_xmalloc((symbol_count == 0U ? 1U : symbol_count) * sizeof(*object->symbols));
    memset(object->symbols, 0, (symbol_count == 0U ? 1U : symbol_count) * sizeof(*object->symbols));
    const unsigned char *strings = object->bytes + string_offset;
    if (object->bytes[string_offset + string_size - 1U] != 0U) {
        link_error(diagnostics, 5008U, path, "object string table is unterminated");
        return false;
    }
    for (uint32_t i = 0U; i < section_count; ++i) {
        const unsigned char *record = object->bytes + section_offset + i * CC64O_SECTION_SIZE;
        LinkSection *section = &object->sections[i];
        uint32_t name_offset = read_u32(record);
        uint32_t payload_offset = read_u32(record + 4U);
        uint32_t payload_size = read_u32(record + 8U);
        uint32_t reloc_offset = read_u32(record + 12U);
        uint32_t reloc_count = read_u32(record + 16U);
        uint32_t alignment_power = read_u32(record + 20U);
        uint16_t target_index = (uint16_t)read_u16(record + 26U);
        section->kind = record[24];
        section->alignment = alignment_power > 15U ? 0U : 1U << alignment_power;
        if (name_offset >= string_size || section->kind > 3U || section->alignment == 0U ||
            target_index != (uint16_t)i || record[25] != 0U ||
            read_u32(record + 28U) != 0U || read_u32(record + 32U) != 0U ||
            (section->kind != 3U && (!range_ok(payload_offset, payload_size, object->size) ||
                                      payload_size > 256U * 1024U * 1024U)) ||
            (section->kind == 3U && payload_offset != 0U) ||
            (section->kind == 3U && read_u32(record + 36U) != 0U) ||
            (section->kind != 3U && read_u32(record + 36U) !=
                                      cc64_crc32(object->bytes + payload_offset, payload_size)) ||
            (reloc_count != 0U && (!range_ok(reloc_offset,
                                             (size_t)reloc_count * CC64O_RELOC_SIZE,
                                             object->size) ||
                                  reloc_offset < CC64O_HEADER_SIZE))) {
            link_error(diagnostics, 5009U, path, "invalid object section"); return false;
        }
        if ((section->kind != 3U &&
             !object_range_add(object, payload_offset, payload_size)) ||
            (reloc_count != 0U &&
             !object_range_add(object, reloc_offset,
                               (size_t)reloc_count * CC64O_RELOC_SIZE))) {
            link_error(diagnostics, 5009U, path, "object section ranges overlap");
            return false;
        }
        uint32_t name_end = name_offset;
        while (name_end < string_size && strings[name_end] != 0U) ++name_end;
        if (name_end == string_size) { link_error(diagnostics, 5010U, path, "invalid section name"); return false; }
        size_t name_length = name_end - name_offset;
        if ((section->kind == 0U || section->kind == 1U) &&
            section->alignment < 16U) {
            link_error(diagnostics, 5009U, path,
                       "executable section alignment is too small");
            return false;
        }
        for (uint32_t prior = 0U; prior < i; ++prior) {
            const char *prior_name = object->sections[prior].name;
            size_t prior_length = strlen(prior_name);
            if (prior_length == name_length &&
                memcmp(strings + name_offset, prior_name, name_length) == 0) {
                link_error(diagnostics, 5009U, path, "duplicate object section");
                return false;
            }
        }
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
                read_u32(entry + 28U) != 0U ||
                (section->relocations[j].type == CC64O_REL_ABS32 &&
                 section->relocations[j].width != 4U) ||
                (section->relocations[j].type == CC64O_REL_ABS64 &&
                 section->relocations[j].width != 8U) ||
                (section->relocations[j].type == CC64O_REL_PC32 &&
                 section->relocations[j].width != 4U) ||
                (section->relocations[j].type == CC64O_REL_SECTION32 &&
                 section->relocations[j].width != 4U) ||
                (section->relocations[j].type == CC64O_REL_DATA64 &&
                 section->relocations[j].width != 8U) ||
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
        symbol->original_value = symbol->value;
        symbol->section_index = read_u32(record + 8U);
        symbol->binding = record[12];
        symbol->kind = record[13];
        symbol->size = read_u64(record + 16U);
        symbol->defined = symbol->section_index != UINT32_MAX;
        if (symbol->binding > 1U || symbol->kind > 4U ||
            read_u16(record + 14U) != 0U || read_u32(record + 24U) != 0U ||
            read_u32(record + 28U) != 0U ||
            (symbol->section_index != UINT32_MAX &&
             symbol->section_index >= section_count)) {
            link_error(diagnostics, 5014U, path, "invalid object symbol"); return false;
        }
        if (symbol->section_index != UINT32_MAX &&
            symbol->value > object->sections[symbol->section_index].size) {
            link_error(diagnostics, 5014U, path, "symbol value is outside its section");
            return false;
        }
        if (symbol->section_index != UINT32_MAX && symbol->kind >= 1U &&
            symbol->kind <= 3U && symbol->size >
                object->sections[symbol->section_index].size - symbol->value) {
            link_error(diagnostics, 5014U, path, "symbol range is outside its section");
            return false;
        }
    }
    if (!object_uncovered_bytes_are_zero(object)) {
        link_error(diagnostics, 5008U, path, "object contains undescribed data");
        free_link_object(object);
        return false;
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
    free(object->ranges);
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

static bool output_append_zeros(OutputSection *section, size_t size)
{
    if (size == 0U) return true;
    unsigned char *bytes = calloc(size, 1U);
    if (bytes == NULL) return false;
    bool good = output_append(section, bytes, size);
    free(bytes);
    return good;
}

static bool output_pad_to(OutputSection *section, size_t old_size)
{
    if (section->size <= old_size) return true;
    size_t padding = section->size - old_size;
    unsigned char *bytes = calloc(padding, 1U);
    if (bytes == NULL) return false;
    bool good = output_append(section, bytes, padding);
    free(bytes);
    return good;
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

static bool runtime_symbol_name(const char *name)
{
    static const char *const names[] = {
        "cc64_putc", "cc64_write", "cc64_read", "cc64_alloc",
        "cc64_free", "cc64_open", "cc64_close", "cc64_exit"
    };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0) return true;
    }
    return false;
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
            bool is_common = symbol->kind == 4U && symbol->section_index == UINT32_MAX;
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
                if (is_common) {
                    size_t alignment = 8U;
                    size_t mask = alignment - 1U;
                    if (outputs[3].size > SIZE_MAX - mask) return false;
                    size_t offset = (outputs[3].size + mask) & ~mask;
                    if (symbol->size > SIZE_MAX - offset) return false;
                    outputs[3].size = offset + symbol->size;
                    symbol->value = outputs[3].output_offset + offset;
                    symbol->defined = true;
                    if (global->defined && !global->common) {
                        link_error(diagnostics, 5020U, NULL,
                                   "common symbol conflicts with definition");
                        return false;
                    }
                    if (!global->defined || symbol->size > global->size) {
                        global->address = symbol->value;
                        global->size = symbol->size;
                    }
                    global->defined = true;
                    global->common = true;
                } else if (global->defined && symbol->defined && !global->common) {
                    bool both_tentative = symbol->kind == 1U &&
                                          object->sections[symbol->section_index].kind == 3U &&
                                          global->size == symbol->size;
                    if (!both_tentative && !runtime_symbol_name(symbol->name)) {
                        link_error(diagnostics, 5020U, NULL,
                                   "duplicate global definition");
                        return false;
                    }
                } else if (symbol->defined) {
                    global->address = symbol->value;
                    global->size = symbol->size;
                    global->defined = true;
                    global->common = false;
                }
            }
        }
    }
    return true;
}

static bool checked_add_signed(uint64_t value, int64_t addend,
                               int64_t *result)
{
    if (addend >= 0) {
        uint64_t extra = (uint64_t)addend;
        if (value > UINT64_MAX - extra) return false;
        if (value + extra > (uint64_t)INT64_MAX) return false;
        *result = (int64_t)(value + extra);
        return true;
    }
    uint64_t amount = (uint64_t)(-(addend + 1)) + 1U;
    if (value < amount) {
        if (value > (uint64_t)INT64_MAX + 1U) {
            *result = (int64_t)(value - amount);
            return *result >= INT64_MIN;
        }
        return false;
    }
    uint64_t difference = value - amount;
    if (difference > (uint64_t)INT64_MAX) {
        *result = (int64_t)(difference - ((uint64_t)1U << 63U));
        return true;
    }
    *result = (int64_t)difference;
    return true;
}

static bool append_image_relocation(ImageRelocation **items, size_t *count,
                                    size_t *capacity, uint64_t destination,
                                    int64_t addend)
{
    if (*count == *capacity) {
        size_t next = *capacity == 0U ? 8U : *capacity * 2U;
        if (next < *capacity || next > SIZE_MAX / sizeof(**items)) return false;
        ImageRelocation *grown = cc64_xrealloc(*items, next * sizeof(**items));
        *items = grown;
        *capacity = next;
    }
    size_t position = *count;
    while (position > 0U && (*items)[position - 1U].destination > destination) {
        (*items)[position] = (*items)[position - 1U];
        --position;
    }
    (*items)[position].destination = destination;
    (*items)[position].addend = addend;
    ++*count;
    return true;
}

static bool apply_relocations(LinkObject *objects, size_t object_count,
                              OutputSection outputs[4], unsigned char *payload,
                              size_t payload_size, GlobalSymbol *globals,
                              size_t global_count, ImageFormat format,
                              ImageRelocation **image_relocations,
                              size_t *image_relocation_count,
                              size_t *image_relocation_capacity,
                              DiagnosticSink *diagnostics)
{
    for (size_t o = 0U; o < object_count; ++o) {
        LinkObject *object = &objects[o];
        for (size_t s = 0U; s < object->section_count; ++s) {
            LinkSection *section = &object->sections[s];
            OutputSection *output = &outputs[section->output_kind];
            for (size_t r = 0U; r < section->relocation_count; ++r) {
                LinkRelocation *reloc = &section->relocations[r];
                if ((reloc->type == CC64O_REL_ABS32 && reloc->width != 4U) ||
                    (reloc->type == CC64O_REL_ABS64 && reloc->width != 8U) ||
                    (reloc->type == CC64O_REL_PC32 && reloc->width != 4U) ||
                    (reloc->type == CC64O_REL_SECTION32 && reloc->width != 4U) ||
                    (reloc->type == CC64O_REL_DATA64 && reloc->width != 8U)) {
                    link_error(diagnostics, 5031U, NULL,
                               "relocation type and width do not match");
                    return false;
                }
                LinkSymbol *symbol = &object->symbols[reloc->symbol_index];
                uint64_t value = symbol->value;
                if (symbol->binding == 1U) {
                    GlobalSymbol *global = find_global(globals, global_count, symbol->name);
                    if (global == NULL || !global->defined) {
                        char text[256];
                        (void)snprintf(text, sizeof(text),
                                       "unresolved symbol '%s'", symbol->name);
                        link_error(diagnostics, 5021U, NULL, text);
                        return false;
                    }
                    value = global->address;
                }
                size_t section_position = section->output_offset +
                                         (size_t)reloc->offset;
                if (output->output_offset > SIZE_MAX - section_position) {
                    link_error(diagnostics, 5023U, NULL,
                               "relocation is outside output section");
                    return false;
                }
                size_t position = output->output_offset + section_position;
                if (position > payload_size ||
                    reloc->width > payload_size - position) {
                    link_error(diagnostics, 5023U, NULL,
                               "relocation is outside output section");
                    return false;
                }
                if (reloc->type == CC64O_REL_DATA64) {
                    if (format == IMAGE_RAW_COM) {
                        link_error(diagnostics, 5022U, NULL,
                                   "load-biased relocation is not valid in raw COM");
                        return false;
                    }
                    if ((position & 7U) != 0U) {
                        link_error(diagnostics, 5032U, NULL,
                                   "data relocation destination is not aligned");
                        return false;
                    }
                    int64_t target = 0;
                    if (!checked_add_signed(value, reloc->addend, &target) ||
                        !append_image_relocation(image_relocations,
                                                 image_relocation_count,
                                                 image_relocation_capacity,
                                                 (uint64_t)position, target)) {
                        link_error(diagnostics, 5033U, NULL,
                                   "data relocation is out of range");
                        return false;
                    }
                    write_u64(payload + position, 0U);
                    continue;
                }
                if (reloc->type == CC64O_REL_ABS32 ||
                    reloc->type == CC64O_REL_ABS64) {
                    link_error(diagnostics, 5022U, NULL,
                               "absolute relocation is not load-safe");
                    return false;
                }
                if (reloc->type == CC64O_REL_PC32) {
                    int64_t target = 0;
                    if (!checked_add_signed(value, reloc->addend, &target)) {
                        link_error(diagnostics, 5024U, NULL,
                                   "PC-relative relocation overflow");
                        return false;
                    }
                    int64_t pc = (int64_t)(position + reloc->width);
                    int64_t result = target - pc;
                    if (result < INT32_MIN || result > INT32_MAX) {
                        link_error(diagnostics, 5024U, NULL,
                                   "PC-relative relocation overflow");
                        return false;
                    }
                    write_u32(payload + position, (uint32_t)(int32_t)result);
                } else if (reloc->type == CC64O_REL_SECTION32) {
                    int64_t result = 0;
                    if (!checked_add_signed(symbol->original_value, reloc->addend,
                                            &result) || result < 0 || result > UINT32_MAX) {
                        link_error(diagnostics, 5025U, NULL,
                                   "section relocation overflow");
                        return false;
                    }
                    write_u32(payload + position, (uint32_t)result);
                }
            }
        }
    }
    return true;
}

static bool image_extents(const OutputSection outputs[4], size_t *payload_size,
                          size_t *memory_size)
{
    size_t payload = 0U;
    size_t memory = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        if (outputs[i].output_offset > SIZE_MAX - outputs[i].size) return false;
        size_t end = outputs[i].output_offset + outputs[i].size;
        if (i != 3U && end > payload) payload = end;
        if (end > memory) memory = end;
    }
    *payload_size = payload;
    *memory_size = memory;
    return true;
}

/* The per-kind output buffers are concatenated into one payload image before
   relocations are applied, so every relocation writes into the final layout
   rather than into a per-section scratch buffer. */
static bool flatten_outputs(const OutputSection outputs[4], size_t payload_size,
                            unsigned char **payload)
{
    *payload = NULL;
    if (payload_size == 0U) return true;
    unsigned char *image = cc64_xmalloc(payload_size);
    memset(image, 0, payload_size);
    for (size_t i = 0U; i < 4U; ++i) {
        if (i == 3U) continue;
        if (outputs[i].size == 0U || outputs[i].data == NULL) continue;
        if (outputs[i].output_offset > payload_size - outputs[i].size) {
            free(image);
            return false;
        }
        memcpy(image + outputs[i].output_offset, outputs[i].data, outputs[i].size);
    }
    *payload = image;
    return true;
}

static bool write_linked_image(const OutputSection outputs[4],
                               const unsigned char *payload, size_t payload_size,
                               size_t memory_size,
                               ImageFormat format,
                               const ImageRelocation *image_relocations,
                               size_t image_relocation_count,
                               const char *output,
                               DiagnosticSink *diagnostics)
{
    if (!image_extents(outputs, &payload_size, &memory_size)) return false;
    const size_t image_limit = 16U * 1024U * 1024U;
    if (memory_size > image_limit || payload_size > UINT32_MAX ||
        outputs[0].output_offset >= payload_size) {
        link_error(diagnostics, 5034U, output, "linked image exceeds target limits");
        return false;
    }
    size_t relocation_bytes = 0U;
    if (image_relocation_count > SIZE_MAX / 16U) return false;
    relocation_bytes = image_relocation_count * 16U;
    size_t file_size = format == IMAGE_RAW_COM ? memory_size : 48U + payload_size + relocation_bytes;
    if (file_size < payload_size || file_size > UINT32_MAX) {
        link_error(diagnostics, 5034U, output, "linked image size overflow");
        return false;
    }
    unsigned char *image = calloc(file_size == 0U ? 1U : file_size, 1U);
    if (image == NULL) return false;
    if (format == IMAGE_RAW_COM) {
        if (payload != NULL && payload_size != 0U) {
            memcpy(image, payload, payload_size);
        }
    } else {
        memcpy(image, "MZ64", 4U);
        write_u32(image + 4U, 48U);
        write_u64(image + 8U, (uint64_t)payload_size);
        write_u32(image + 16U, (uint32_t)outputs[0].output_offset);
        write_u32(image + 24U, (uint32_t)image_relocation_count);
        if (48U + payload_size > UINT32_MAX) {
            free(image);
            return false;
        }
        write_u32(image + 28U, (uint32_t)(48U + payload_size));
        write_u64(image + 32U, (uint64_t)memory_size);
        if (payload != NULL && payload_size != 0U) {
            memcpy(image + 48U, payload, payload_size);
        }
        for (size_t i = 0U; i < image_relocation_count; ++i) {
            unsigned char *entry = image + 48U + payload_size + i * 16U;
            write_u64(entry, image_relocations[i].destination);
            write_u64(entry + 8U, (uint64_t)image_relocations[i].addend);
        }
    }
    bool wrote = cc64_write_file(output, image, file_size);
    if (!wrote) {
        const char *message = "cannot write MZ64 image";
        if (format == IMAGE_RAW_COM) {
            message = "cannot write raw image";
        }
        link_error(diagnostics, 5029U, output, message);
    }
    free(image);
    return wrote;
}

bool link_objects(Arena *arena, const char *const *objects, size_t object_count,
                  const char *output, ImageFormat format,
                  DiagnosticSink *diagnostics)
{
    (void)arena;
    if (object_count == 0U || output == NULL) {
        link_error(diagnostics, 5041U, output,
                   "linker requires objects and output");
        return false;
    }
    LinkObject *loaded = cc64_xmalloc(object_count * sizeof(*loaded));
    OutputSection outputs[4] = {0};
    outputs[0].kind = 0U; outputs[0].alignment = 16U;
    outputs[1].kind = 1U; outputs[1].alignment = 16U;
    outputs[2].kind = 2U; outputs[2].alignment = 8U;
    outputs[3].kind = 3U; outputs[3].alignment = 8U;
    GlobalSymbol *globals = NULL;
    size_t global_count = 0U;
    size_t global_capacity = 0U;
    ImageRelocation *image_relocations = NULL;
    size_t image_relocation_count = 0U;
    size_t image_relocation_capacity = 0U;
    bool good = true;
    size_t loaded_count = 0U;
    for (size_t i = 0U; i < object_count; ++i) {
        if (!read_object_file(objects[i], &loaded[i], diagnostics)) {
            free_link_object(&loaded[i]);
            good = false;
            break;
        }
        ++loaded_count;
    }
    if (good) {
        static const unsigned char entry_stub[] = {
            0x48U, 0x83U, 0xecU, 0x28U,
            0x48U, 0x89U, 0x3cU, 0x24U,
            0x48U, 0xc7U, 0x44U, 0x24U, 0x10U, 0U, 0U, 0U, 0U,
            0x49U, 0x89U, 0xfaU,
            0x41U, 0x8bU, 0x82U, 0xa0U, 0U, 0U, 0U,
            0x85U, 0xc0U, 0x0fU, 0x95U, 0xc0U,
            0x0fU, 0xb6U, 0xc8U, 0x83U, 0xc1U, 0x01U,
            0x49U, 0x8dU, 0x82U, 0xa1U, 0U, 0U, 0U,
            0x48U, 0x89U, 0x44U, 0x24U, 0x08U,
            0x48U, 0x89U, 0xcfU, 0x48U, 0x89U, 0xe6U, 0x31U, 0xd2U,
            0xe8U, 0U, 0U, 0U, 0U,
            0x89U, 0xc1U, 0x48U, 0x83U, 0xc4U, 0x28U,
            0xb8U, 0U, 0x4cU, 0U, 0U, 0x88U,
            0xc8U, 0xcdU, 0x21U, 0xc3U
        };
        if (!output_append(&outputs[0], entry_stub, sizeof(entry_stub))) good = false;
        for (size_t o = 0U; o < object_count && good; ++o) {
            for (size_t s = 0U; s < loaded[o].section_count; ++s) {
                LinkSection *section = &loaded[o].sections[s];
                unsigned output_kind = section->kind;
                size_t old_size = outputs[output_kind].size;
                if (!align_output(&outputs[output_kind], section->alignment,
                                   &outputs[output_kind].size) ||
                    !output_pad_to(&outputs[output_kind], old_size)) {
                    good = false;
                    break;
                }
                section->output_offset = outputs[output_kind].size;
                section->output_kind = output_kind;
                if (section->kind != 3U &&
                    !output_append(&outputs[output_kind], section->data, section->size)) {
                    good = false;
                    break;
                }
                if (section->kind == 3U &&
                    !output_append_zeros(&outputs[output_kind], section->size)) {
                    good = false;
                    break;
                }
            }
        }
    }
    if (good) {
        size_t base = 0U;
        for (size_t i = 0U; i < 4U; ++i) {
            if (!align_output(&outputs[i], outputs[i].alignment, &base)) {
                good = false;
                break;
            }
            outputs[i].output_offset = base;
            if (base > SIZE_MAX - outputs[i].size) { good = false; break; }
            base += outputs[i].size;
        }
    }
    if (good && !resolve_symbols(loaded, object_count, outputs, &globals,
                                 &global_count, &global_capacity, diagnostics)) {
        good = false;
    }
    unsigned char *payload = NULL;
    size_t payload_size = 0U;
    size_t memory_size = 0U;
    if (good && (!image_extents(outputs, &payload_size, &memory_size) ||
                 !flatten_outputs(outputs, payload_size, &payload))) {
        link_error(diagnostics, 5034U, output, "linked image exceeds target limits");
        good = false;
    }
    if (good && !apply_relocations(loaded, object_count, outputs, payload,
                                   payload_size, globals, global_count, format,
                                   &image_relocations, &image_relocation_count,
                                   &image_relocation_capacity, diagnostics)) {
        good = false;
    }
    if (good) {
        GlobalSymbol *main_symbol = find_global(globals, global_count, "main");
        if (main_symbol == NULL || !main_symbol->defined) {
            link_error(diagnostics, 5027U, output, "undefined main symbol");
            good = false;
        } else {
            size_t field = outputs[0].output_offset + 59U;
            int64_t displacement = (int64_t)main_symbol->address -
                                   (int64_t)(field + 4U);
            if (field > payload_size || 4U > payload_size - field) {
                link_error(diagnostics, 5028U, output, "entry trampoline overflow");
                good = false;
            } else if (displacement < INT32_MIN || displacement > INT32_MAX) {
                link_error(diagnostics, 5028U, output,
                           "entry trampoline overflow");
                good = false;
            } else {
                write_u32(payload + field, (uint32_t)(int32_t)displacement);
            }
        }
    }
    if (good && !write_linked_image(outputs, payload, payload_size, memory_size,
                                    format, image_relocations,
                                    image_relocation_count, output, diagnostics)) {
        good = false;
    }
    free(payload);
    for (size_t i = 0U; i < loaded_count; ++i) free_link_object(&loaded[i]);
    for (size_t i = 0U; i < global_count; ++i) free((void *)globals[i].name);
    free(globals);
    free(image_relocations);
    for (size_t i = 0U; i < 4U; ++i) free(outputs[i].data);
    free(loaded);
    return good;
}
