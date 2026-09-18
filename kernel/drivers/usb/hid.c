#include <stdint.h>

#include "../../include/hid.h"

#define ITEM_MAIN 0U
#define ITEM_GLOBAL 1U
#define ITEM_LOCAL 2U

#define MAIN_INPUT 0x8U
#define MAIN_OUTPUT 0x9U
#define MAIN_COLLECTION 0xAU
#define MAIN_FEATURE 0xBU
#define MAIN_END_COLLECTION 0xCU

#define GLOBAL_USAGE_PAGE 0x0U
#define GLOBAL_LOGICAL_MINIMUM 0x1U
#define GLOBAL_REPORT_SIZE 0x7U
#define GLOBAL_REPORT_ID 0x8U
#define GLOBAL_REPORT_COUNT 0x9U
#define GLOBAL_PUSH 0xAU
#define GLOBAL_POP 0xBU

#define LOCAL_USAGE 0x0U
#define LOCAL_USAGE_MINIMUM 0x1U
#define LOCAL_USAGE_MAXIMUM 0x2U

#define INPUT_CONSTANT 0x1U
#define INPUT_RELATIVE 0x4U

#define PAGE_GENERIC_DESKTOP 0x01U
#define PAGE_BUTTON 0x09U
#define PAGE_CONSUMER 0x0CU
#define USAGE_POINTER 0x01U
#define USAGE_MOUSE 0x02U
#define USAGE_X 0x30U
#define USAGE_Y 0x31U
#define USAGE_WHEEL 0x38U
#define USAGE_AC_PAN 0x238U
#define COLLECTION_APPLICATION 1U

#define MAX_USAGES 32U
#define MAX_STACK 4U
#define MAX_DEPTH 16U

struct globals {
    uint32_t usage_page;
    int32_t logical_minimum;
    uint32_t report_size;
    uint32_t report_count;
    uint32_t report_id;
};

static int32_t sign_extend(uint32_t value, uint32_t bytes) {
    if (bytes == 1U) return (int8_t)value;
    if (bytes == 2U) return (int16_t)value;
    return (int32_t)value;
}

static void set_field(struct hid_field *field, uint32_t offset, uint32_t size, int is_signed) {
    if (field->size) return;
    field->offset = (uint16_t)offset;
    field->size = (uint8_t)(size > 32U ? 32U : size);
    field->is_signed = (uint8_t)(is_signed ? 1 : 0);
}

int hid_parse_mouse(const uint8_t *descriptor, uint32_t length, struct hid_mouse_layout *out) {
    struct globals global = {0, 0, 0, 0, 0};
    struct globals stack[MAX_STACK];
    unsigned stack_depth = 0;
    uint32_t usages[MAX_USAGES];
    unsigned usage_count = 0;
    uint32_t usage_minimum = 0, usage_maximum = 0;
    int have_range = 0;
    uint16_t bits[256];
    int mouse_depth = -1;
    int depth = 0;
    struct hid_mouse_layout found;
    int chosen = -1;

    for (unsigned index = 0; index < 256U; index++) bits[index] = 0;
    for (unsigned index = 0; index < sizeof(found); index++) ((uint8_t *)&found)[index] = 0;

    for (uint32_t at = 0; at < length;) {
        uint8_t prefix = descriptor[at];
        if (prefix == 0xFEU) {
            if (at + 2U >= length) break;
            at += 3U + descriptor[at + 1U];
            continue;
        }
        uint32_t size = prefix & 3U;
        if (size == 3U) size = 4U;
        uint32_t type = (prefix >> 2) & 3U;
        uint32_t tag = prefix >> 4;
        if (at + 1U + size > length) break;
        uint32_t data = 0;
        for (uint32_t byte = 0; byte < size; byte++)
            data |= (uint32_t)descriptor[at + 1U + byte] << (8U * byte);
        at += 1U + size;

        if (type == ITEM_GLOBAL) {
            switch (tag) {
                case GLOBAL_USAGE_PAGE: global.usage_page = data; break;
                case GLOBAL_LOGICAL_MINIMUM: global.logical_minimum = sign_extend(data, size); break;
                case GLOBAL_REPORT_SIZE: global.report_size = data; break;
                case GLOBAL_REPORT_COUNT: global.report_count = data; break;
                case GLOBAL_REPORT_ID: global.report_id = data & 0xFFU; break;
                case GLOBAL_PUSH:
                    if (stack_depth < MAX_STACK) stack[stack_depth++] = global;
                    break;
                case GLOBAL_POP:
                    if (stack_depth) global = stack[--stack_depth];
                    break;
                default: break;
            }
            continue;
        }
        if (type == ITEM_LOCAL) {
            uint32_t usage = size == 4U ? data : (global.usage_page << 16) | data;
            if (tag == LOCAL_USAGE && usage_count < MAX_USAGES) usages[usage_count++] = usage;
            if (tag == LOCAL_USAGE_MINIMUM) {
                usage_minimum = usage;
                have_range = 1;
            }
            if (tag == LOCAL_USAGE_MAXIMUM) {
                usage_maximum = usage;
                have_range = 1;
            }
            continue;
        }
        if (type != ITEM_MAIN) continue;

        if (tag == MAIN_COLLECTION) {
            uint32_t usage = usage_count ? usages[0] : 0;
            if (mouse_depth < 0 && data == COLLECTION_APPLICATION &&
                (usage == ((PAGE_GENERIC_DESKTOP << 16) | USAGE_MOUSE) ||
                 usage == ((PAGE_GENERIC_DESKTOP << 16) | USAGE_POINTER)))
                mouse_depth = depth;
            if (depth < (int)MAX_DEPTH) depth++;
        } else if (tag == MAIN_END_COLLECTION) {
            if (depth > 0) depth--;
            if (mouse_depth == depth) mouse_depth = -1;
        } else if (tag == MAIN_INPUT) {
            uint32_t id = global.report_id;
            uint32_t offset = bits[id];
            uint32_t total = global.report_size * global.report_count;
            if (mouse_depth >= 0 && !(data & INPUT_CONSTANT) && global.report_size &&
                (chosen < 0 || (uint32_t)chosen == id)) {
                for (uint32_t item = 0; item < global.report_count && item < 64U; item++) {
                    uint32_t usage;
                    if (have_range) {
                        usage = usage_minimum + item;
                        if (usage_maximum && usage > usage_maximum) break;
                    } else if (usage_count) {
                        usage = usages[item < usage_count ? item : usage_count - 1U];
                    } else {
                        break;
                    }
                    uint32_t page = usage >> 16;
                    uint32_t code = usage & 0xFFFFU;
                    uint32_t field = offset + item * global.report_size;
                    int is_signed = global.logical_minimum < 0;
                    if (page == PAGE_BUTTON && code >= 1U && code <= 32U) {
                        if (!found.buttons) {
                            found.buttons_offset = (uint16_t)(field - (code - 1U));
                            chosen = (int)id;
                        }
                        if (code > found.buttons) found.buttons = (uint8_t)code;
                    } else if (page == PAGE_GENERIC_DESKTOP && (data & INPUT_RELATIVE)) {
                        if (code == USAGE_X) set_field(&found.x, field, global.report_size, is_signed);
                        if (code == USAGE_Y) set_field(&found.y, field, global.report_size, is_signed);
                        if (code == USAGE_WHEEL)
                            set_field(&found.wheel, field, global.report_size, is_signed);
                        if (code == USAGE_X || code == USAGE_Y) chosen = (int)id;
                    } else if (page == PAGE_CONSUMER && code == USAGE_AC_PAN && (data & INPUT_RELATIVE)) {
                        set_field(&found.pan, field, global.report_size, is_signed);
                    }
                }
            }
            bits[id] = (uint16_t)(offset + total);
        }
        usage_count = 0;
        have_range = 0;
        usage_minimum = 0;
        usage_maximum = 0;
    }

    if (!found.x.size || !found.y.size || chosen < 0) return -1;
    found.report_id = (uint8_t)chosen;
    found.report_bits = bits[chosen];
    *out = found;
    return 0;
}

static int32_t extract(const uint8_t *report, uint32_t length, const struct hid_field *field) {
    if (!field->size) return 0;
    uint32_t end = (uint32_t)field->offset + field->size;
    if ((end + 7U) / 8U > length) return 0;
    uint64_t value = 0;
    for (uint32_t bit = 0; bit < field->size; bit++) {
        uint32_t position = field->offset + bit;
        if (report[position / 8U] & (1U << (position % 8U))) value |= 1ULL << bit;
    }
    if (field->is_signed && field->size < 64U && (value & (1ULL << (field->size - 1U))))
        value |= ~0ULL << field->size;
    return (int32_t)(int64_t)value;
}

int hid_decode_mouse(const struct hid_mouse_layout *layout, const uint8_t *report,
                     uint32_t length, int *dx, int *dy, int *wheel, uint32_t *buttons) {
    if (layout->report_id) {
        if (!length || report[0] != layout->report_id) return -1;
        report++;
        length--;
    }
    if (length * 8U < layout->y.offset + layout->y.size) return -1;
    *dx = extract(report, length, &layout->x);
    *dy = extract(report, length, &layout->y);
    *wheel = extract(report, length, &layout->wheel);
    uint32_t pressed = 0;
    for (uint32_t button = 0; button < layout->buttons && button < 32U; button++) {
        uint32_t position = (uint32_t)layout->buttons_offset + button;
        if ((position + 8U) / 8U > length) break;
        if (report[position / 8U] & (1U << (position % 8U))) pressed |= 1U << button;
    }
    *buttons = pressed;
    return 0;
}
