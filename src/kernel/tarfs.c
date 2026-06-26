#include <stddef.h>
#include <stdint.h>
#include "include/build_config.h"
#include "include/kstring.h"
#include "include/tarfs.h"
#include "include/vfs.h"
#include "include/vmm.h"

extern void kprintf(const char *fmt, ...);

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) do { } while (0)
#endif

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed));

typedef char tar_header_size_must_be_512[(sizeof(struct tar_header) == 512) ? 1 : -1];

static uint64_t parse_octal(const char *text, size_t length) {
    uint64_t value = 0;
    size_t i = 0;
    while (i < length && (text[i] == ' ' || text[i] == '\0')) i++;
    for (; i < length && text[i] >= '0' && text[i] <= '7'; i++) value = value * 8 + (uint64_t)(text[i] - '0');
    return value;
}

static int block_is_zero(const uint8_t *block) {
    for (size_t i = 0; i < 512; i++) if (block[i]) return 0;
    return 1;
}

static void normalize_path(char out[256], const struct tar_header *header) {
    size_t at = 0;
    out[at++] = '/';
    if (header->prefix[0]) {
        for (size_t i = 0; i < sizeof(header->prefix) && header->prefix[i] && at + 1 < 256; i++) out[at++] = header->prefix[i];
        if (at + 1 < 256) out[at++] = '/';
    }
    const char *name = header->name;
    if (name[0] == '.' && name[1] == '/') name += 2;
    while (*name == '/') name++;
    for (size_t i = 0; i < 100 && name[i] && at + 1 < 256; i++) out[at++] = name[i];
    while (at > 1 && out[at - 1] == '/') at--;
    out[at] = '\0';
}

int tarfs_unpack(uint64_t initramfs_physical_address, uint64_t initramfs_size) {
    const uint8_t *archive = (const uint8_t *)vmm_phys_to_virt(initramfs_physical_address);
    uint64_t offset = 0;
    int files = 0;

    for (;;) {
        if (offset + 512 > initramfs_size) return -1;
        const struct tar_header *header = (const struct tar_header *)(archive + offset);
        if (block_is_zero((const uint8_t *)header)) break;
        if (header->magic[0] && strncmp(header->magic, "ustar", 5) != 0) {
            kprintf("TarFS: invalid ustar header at %x\n", (unsigned)offset);
            return -1;
        }

        uint64_t size = parse_octal(header->size, sizeof(header->size));
        char path[256];
        normalize_path(path, header);
        if (path[1]) {
            uint32_t mode = (uint32_t)parse_octal(header->mode, sizeof(header->mode));
            if (header->typeflag == '5') {
                struct vfs_node *directory = vfs_mkdir_p(path);
                if (!directory) return -1;
                if (mode) directory->mode = mode & 07777U;
            } else if (header->typeflag == '2') {
                char target[101];
                size_t target_length = 0;
                while (target_length < sizeof(header->linkname) && header->linkname[target_length]) target_length++;
                memcpy(target, header->linkname, target_length);
                target[target_length] = '\0';
                struct vfs_node *link = vfs_create_symlink(path, target, VFS_READONLY);
                if (!link) return -1;
                if (mode) link->mode = mode & 07777U;
                files++;
                KDEBUG("TarFS: %s -> %s\n", path, target);
            } else if (header->typeflag == '0' || header->typeflag == '\0') {
                struct vfs_node *file = vfs_create_file(path, archive + offset + 512, size, VFS_READONLY, 0);
                if (!file) return -1;
                if (mode) file->mode = mode & 07777U;
                files++;
                KDEBUG("TarFS: %s (%u bytes)\n", path, (unsigned)size);
            }
        }
        uint64_t padded = (size + 511) & ~511ULL;
        if (padded > initramfs_size - offset - 512) return -1;
        offset += 512 + padded;
    }
    KDEBUG("TarFS: unpacked %d files\n", files);
    return files;
}

