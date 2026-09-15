#include <stddef.h>
#include <stdint.h>

#include "aarch64.h"

#define FDT_MAGIC 0xD00DFEEDU
#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE 2U
#define FDT_PROP 3U
#define FDT_NOP 4U
#define FDT_END 9U
#define FDT_MAX_DEPTH 16U

static const uint8_t *blob;
static const uint8_t *structure;
static uint32_t structure_size;
static const char *strings;
static uint32_t strings_size;
static uint32_t total_size;

uint32_t fdt_read32(const void *cell) {
    const uint8_t *b = cell;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint64_t read_cells(const uint8_t *cells, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t index = 0; index < count; index++)
        value = (value << 32) | fdt_read32(cells + index * 4U);
    return value;
}

static int text_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static size_t text_length(const char *text, size_t limit) {
    size_t length = 0;
    while (length < limit && text[length]) length++;
    return length;
}

int fdt_init(const void *address) {
    const uint8_t *header = address;
    if (!header || fdt_read32(header) != FDT_MAGIC) return -1;
    total_size = fdt_read32(header + 4);
    uint32_t off_struct = fdt_read32(header + 8);
    uint32_t off_strings = fdt_read32(header + 12);
    strings_size = fdt_read32(header + 32);
    structure_size = fdt_read32(header + 36);
    if (off_struct + structure_size > total_size || off_strings + strings_size > total_size)
        return -1;
    blob = header;
    structure = header + off_struct;
    strings = (const char *)(header + off_strings);
    return 0;
}

uint32_t fdt_total_size(void) { return blob ? total_size : 0; }

int fdt_memreserve(unsigned index, uint64_t *base, uint64_t *size) {
    if (!blob) return -1;
    const uint8_t *entry = blob + fdt_read32(blob + 16) + (uint64_t)index * 16U;
    uint64_t start = read_cells(entry, 2);
    uint64_t length = read_cells(entry + 8, 2);
    if (!start && !length) return -1;
    *base = start;
    *size = length;
    return 0;
}

struct walk_state {
    uint32_t offset;
    const char *name;
    unsigned depth;
    uint32_t address_cells;
    uint32_t size_cells;
    uint32_t parent_offset;
};

typedef int (*walk_visit)(const struct walk_state *state, void *context);

static uint32_t align4(uint32_t value) {
    return (value + 3U) & ~3U;
}

static int walk(walk_visit visit, void *context) {
    if (!structure) return -1;
    uint32_t cells_address[FDT_MAX_DEPTH + 1] = {2};
    uint32_t cells_size[FDT_MAX_DEPTH + 1] = {1};
    uint32_t offsets[FDT_MAX_DEPTH + 1] = {0};
    unsigned depth = 0;
    uint32_t position = 0;

    while (position + 4U <= structure_size) {
        uint32_t token = fdt_read32(structure + position);
        uint32_t token_offset = position;
        position += 4U;
        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)(structure + position);
            size_t length = text_length(name, structure_size - position);
            position = align4(position + (uint32_t)length + 1U);
            if (depth >= FDT_MAX_DEPTH) return -1;
            depth++;
            offsets[depth] = token_offset;
            cells_address[depth] = 2;
            cells_size[depth] = 1;
            uint32_t scan = position;
            while (scan + 12U <= structure_size && fdt_read32(structure + scan) == FDT_PROP) {
                uint32_t value_length = fdt_read32(structure + scan + 4U);
                const char *property = strings + fdt_read32(structure + scan + 8U);
                if (value_length == 4U && text_equal(property, "#address-cells"))
                    cells_address[depth] = fdt_read32(structure + scan + 12U);
                if (value_length == 4U && text_equal(property, "#size-cells"))
                    cells_size[depth] = fdt_read32(structure + scan + 12U);
                scan = align4(scan + 12U + value_length);
            }
            struct walk_state state = {
                .offset = token_offset,
                .name = name,
                .depth = depth,
                .address_cells = cells_address[depth - 1],
                .size_cells = cells_size[depth - 1],
                .parent_offset = depth > 1 ? offsets[depth - 1] : 0xFFFFFFFFU,
            };
            if (depth == 1) {
                state.address_cells = 2;
                state.size_cells = 1;
            }
            int result = visit(&state, context);
            if (result) return result > 0 ? 0 : result;
        } else if (token == FDT_END_NODE) {
            if (!depth) return -1;
            depth--;
        } else if (token == FDT_PROP) {
            uint32_t value_length = fdt_read32(structure + position);
            position = align4(position + 8U + value_length);
        } else if (token == FDT_NOP) {
            continue;
        } else {
            break;
        }
    }
    return -1;
}

const void *fdt_property(const struct fdt_node *node, const char *name, uint32_t *length) {
    if (!structure || !node) return NULL;
    uint32_t position = node->offset + 4U;
    const char *node_name = (const char *)(structure + position);
    position = align4(position + (uint32_t)text_length(node_name, structure_size - position) + 1U);
    while (position + 4U <= structure_size) {
        uint32_t token = fdt_read32(structure + position);
        if (token == FDT_NOP) {
            position += 4U;
            continue;
        }
        if (token != FDT_PROP) break;
        uint32_t value_length = fdt_read32(structure + position + 4U);
        uint32_t name_offset = fdt_read32(structure + position + 8U);
        if (name_offset < strings_size && text_equal(strings + name_offset, name)) {
            if (length) *length = value_length;
            return structure + position + 12U;
        }
        position = align4(position + 12U + value_length);
    }
    return NULL;
}

static void fill_node(const struct walk_state *state, struct fdt_node *out) {
    out->offset = state->offset;
    out->address_cells = state->address_cells;
    out->size_cells = state->size_cells;
}

int fdt_is_compatible(const struct fdt_node *node, const char *compatible) {
    uint32_t length = 0;
    const char *list = fdt_property(node, "compatible", &length);
    if (!list) return 0;
    uint32_t position = 0;
    while (position < length) {
        if (text_equal(list + position, compatible)) return 1;
        position += (uint32_t)text_length(list + position, length - position) + 1U;
    }
    return 0;
}

struct path_search {
    const char *path;
    const char *parts[FDT_MAX_DEPTH];
    unsigned matched;
    struct fdt_node *out;
};

static int component_matches(const char *name, const char *path) {
    while (*path && *path != '/' && *name && *name == *path) {
        name++;
        path++;
    }
    if (*path && *path != '/') return 0;
    return *name == '\0' || (*name == '@' && *path != '@');
}

static int visit_path(const struct walk_state *state, void *context) {
    struct path_search *search = context;
    if (state->depth == 1) {
        search->matched = 1;
        search->parts[0] = search->path;
        while (*search->parts[0] == '/') search->parts[0]++;
        if (!*search->parts[0]) {
            fill_node(state, search->out);
            return 1;
        }
        return 0;
    }
    if (state->depth != search->matched + 1U) return 0;
    const char *want = search->parts[search->matched - 1U];
    if (!component_matches(state->name, want)) return 0;
    const char *next = want;
    while (*next && *next != '/') next++;
    while (*next == '/') next++;
    if (!*next) {
        fill_node(state, search->out);
        return 1;
    }
    search->parts[search->matched] = next;
    search->matched++;
    return 0;
}

int fdt_find_path(const char *path, struct fdt_node *out) {
    struct path_search search = { .path = path, .matched = 0, .out = out };
    out->offset = 0xFFFFFFFFU;
    walk(visit_path, &search);
    return out->offset == 0xFFFFFFFFU ? -1 : 0;
}

struct match_search {
    const char *compatible;
    const char *device_type;
    unsigned index;
    struct fdt_node *out;
    int found;
};

static int visit_match(const struct walk_state *state, void *context) {
    struct match_search *search = context;
    struct fdt_node node;
    fill_node(state, &node);
    int hit;
    if (search->compatible) {
        hit = fdt_is_compatible(&node, search->compatible);
    } else {
        const char *type = fdt_property(&node, "device_type", NULL);
        hit = type && text_equal(type, search->device_type);
    }
    if (!hit) return 0;
    if (search->index) {
        search->index--;
        return 0;
    }
    *search->out = node;
    search->found = 1;
    return 1;
}

int fdt_find_compatible(const char *compatible, unsigned index, struct fdt_node *out) {
    struct match_search search = { compatible, NULL, index, out, 0 };
    walk(visit_match, &search);
    return search.found ? 0 : -1;
}

int fdt_find_device_type(const char *type, unsigned index, struct fdt_node *out) {
    struct match_search search = { NULL, type, index, out, 0 };
    walk(visit_match, &search);
    return search.found ? 0 : -1;
}

struct child_search {
    uint32_t parent;
    unsigned index;
    struct fdt_node *out;
    int found;
};

static int visit_child(const struct walk_state *state, void *context) {
    struct child_search *search = context;
    if (state->parent_offset != search->parent) return 0;
    if (search->index) {
        search->index--;
        return 0;
    }
    fill_node(state, search->out);
    search->found = 1;
    return 1;
}

int fdt_child(const struct fdt_node *parent, unsigned index, struct fdt_node *out) {
    struct child_search search = { parent->offset, index, out, 0 };
    walk(visit_child, &search);
    return search.found ? 0 : -1;
}

int fdt_reg(const struct fdt_node *node, unsigned index, uint64_t *base, uint64_t *size) {
    uint32_t length = 0;
    const uint8_t *reg = fdt_property(node, "reg", &length);
    uint32_t entry = (node->address_cells + node->size_cells) * 4U;
    if (!reg || !entry || node->address_cells > 2U || node->size_cells > 2U) return -1;
    if ((index + 1U) * entry > length) return -1;
    const uint8_t *cells = reg + index * entry;
    *base = read_cells(cells, node->address_cells);
    *size = read_cells(cells + node->address_cells * 4U, node->size_cells);
    return 0;
}
