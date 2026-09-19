#ifndef TUNIX_MODULE_H
#define TUNIX_MODULE_H

#include <stddef.h>
#include <stdint.h>

#include "uts.h"

#if defined(__x86_64__)
#define MODULE_VERMAGIC UTS_RELEASE " x86_64"
#elif defined(__aarch64__)
#define MODULE_VERMAGIC UTS_RELEASE " aarch64"
#endif

#define MODULE_NAME_MAX 56
#define MODULE_MAX_USES 8

#define MODULE_PARAM_INT 1
#define MODULE_PARAM_UINT 2
#define MODULE_PARAM_BOOL 3
#define MODULE_PARAM_STRING 4

#define MODULE_STATE_LOADING 0
#define MODULE_STATE_LIVE 1
#define MODULE_STATE_UNLOADING 2

struct module_export {
    const char *name;
    uint64_t value;
};

struct module_param {
    const char *name;
    void *value;
    uint32_t type;
    uint32_t mode;
};

struct module_descriptor {
    const char *name;
    int (*init)(void);
    void (*exit)(void);
};

struct module {
    char name[MODULE_NAME_MAX];
    uint64_t base;
    uint64_t physical;
    uint64_t bytes;
    uint64_t text;
    uint64_t text_bytes;
    uint64_t rodata;
    uint64_t data;
    uint32_t refs;
    int state;
    void (*exit)(void);
    char *arguments;
    const struct module_export *exports;
    unsigned export_count;
    const struct module_param *params;
    unsigned param_count;
    struct module *uses[MODULE_MAX_USES];
    unsigned use_count;
    struct module *next;
};

int module_load(const void *image, size_t bytes, const char *arguments);
int module_unload(const char *name, unsigned flags);
struct module *module_find(const char *name);
struct module *module_list(void);
struct module *module_active(void);
unsigned module_kernel_symbol_count(void);
int module_get(struct module *module);
void module_put(struct module *module);
const char *module_state_name(const struct module *module);
int module_param_format(const struct module *module, unsigned index,
                        char *out, size_t capacity);
int module_param_set(struct module *module, unsigned index, const char *text,
                     size_t length);
int module_address_owner(uint64_t address, const char **name, uint64_t *offset);
int module_image_info(const void *contents, size_t bytes, const char *key,
                      unsigned occurrence, char *out, size_t capacity);
int module_export_value(const struct module *module, const char *name,
                        uint64_t *value);

#define MODULE_JOIN_(a, b) a##b
#define MODULE_JOIN(a, b) MODULE_JOIN_(a, b)

#define MODULE_INFO(tag, value) \
    static const char MODULE_JOIN(__modinfo_, __COUNTER__)[] \
        __attribute__((section(".modinfo"), used, aligned(1))) = #tag "=" value

#define MODULE_LICENSE(text) MODULE_INFO(license, text)
#define MODULE_AUTHOR(text) MODULE_INFO(author, text)
#define MODULE_DESCRIPTION(text) MODULE_INFO(description, text)
#define MODULE_ALIAS(text) MODULE_INFO(alias, text)

/* What udev hands modprobe for a PCI device, with every wildcard it fills in. */
#define MODULE_PCI_ALIAS(vendor, device) \
    MODULE_ALIAS("pci:v0000" vendor "d0000" device "sv*sd*bc*sc*i*")

#define MODULE_MAIN(init_function, exit_function) \
    MODULE_INFO(name, TUNIX_MODULE_NAME); \
    MODULE_INFO(vermagic, MODULE_VERMAGIC); \
    static const struct module_descriptor __this_module \
        __attribute__((section(".tunix_module"), used, aligned(8))) = \
        { TUNIX_MODULE_NAME, init_function, exit_function }

#define MODULE_EXPORT(symbol) \
    static const char MODULE_JOIN(__ksymstr_, symbol)[] \
        __attribute__((section("__ksymtab_strings"), used, aligned(1))) = #symbol; \
    const struct module_export MODULE_JOIN(__ksymtab_, symbol) \
        __attribute__((section(".tunix_ksym"), used, aligned(8))) = \
        { MODULE_JOIN(__ksymstr_, symbol), (uint64_t)(uintptr_t)&symbol }

#define MODULE_PARAMETER(variable, kind) \
    static const struct module_param MODULE_JOIN(__param_, variable) \
        __attribute__((section(".tunix_param"), used, aligned(8))) = \
        { #variable, &variable, kind, 0644 }

#endif
