#ifndef TUNIX_FS_EXT4_H
#define TUNIX_FS_EXT4_H

#include <stddef.h>
#include <stdint.h>
#include <tunix/block.h>
#include <tunix/build_config.h>
#include <tunix/mutex.h>
#include <tunix/vfs.h>
#include <tunix/kprintf.h>
#include <tunix/errno.h>

struct jbd2_journal;

#if TUNIX_DEBUG_LOGS
#define KDEBUG(...) kprintf(__VA_ARGS__)
#else
#define KDEBUG(...) \
    do { \
    } while (0)
#endif

#define EXT4_SUPER_MAGIC         0xEF53U
#define EXT4_MIN_BLOCK_SIZE      1024U
#define EXT4_MAX_BLOCK_SIZE      4096U
#define EXT4_GOOD_OLD_INODE_SIZE 128U
#define EXT4_INODE_FIELDS        160U
#define EXT4_MIN_DESC_SIZE       32U
#define EXT4_MIN_DESC_SIZE_64BIT 64U
#define EXT4_ROOT_INO            2U
#define EXT4_N_BLOCKS            15U
#define EXT4_NDIR_BLOCKS         12U
#define EXT4_LINK_MAX            65000U
#define EXT4_DEFAULT_EXTRA_ISIZE 32U
#define EXT4_RUN_BYTES           (32U * EXT4_MAX_BLOCK_SIZE)
#define EXT4_NAME_LEN            255U

#define EXT4_VALID_FS 0x0001U
#define EXT4_ERROR_FS 0x0002U

#define EXT4_FLAGS_SIGNED_HASH   0x0001U
#define EXT4_FLAGS_UNSIGNED_HASH 0x0002U

#define EXT4_COMPAT_HAS_JOURNAL   0x0004U
#define EXT4_COMPAT_EXT_ATTR      0x0008U
#define EXT4_COMPAT_RESIZE_INODE  0x0010U
#define EXT4_COMPAT_DIR_INDEX     0x0020U
#define EXT4_COMPAT_SPARSE_SUPER2 0x0200U
#define EXT4_COMPAT_FAST_COMMIT   0x0400U
#define EXT4_COMPAT_ORPHAN_FILE   0x1000U

#define EXT4_RO_COMPAT_SPARSE_SUPER   0x0001U
#define EXT4_RO_COMPAT_LARGE_FILE     0x0002U
#define EXT4_RO_COMPAT_BTREE_DIR      0x0004U
#define EXT4_RO_COMPAT_HUGE_FILE      0x0008U
#define EXT4_RO_COMPAT_GDT_CSUM       0x0010U
#define EXT4_RO_COMPAT_DIR_NLINK      0x0020U
#define EXT4_RO_COMPAT_EXTRA_ISIZE    0x0040U
#define EXT4_RO_COMPAT_QUOTA          0x0100U
#define EXT4_RO_COMPAT_BIGALLOC       0x0200U
#define EXT4_RO_COMPAT_METADATA_CSUM  0x0400U
#define EXT4_RO_COMPAT_READONLY       0x1000U
#define EXT4_RO_COMPAT_PROJECT        0x2000U
#define EXT4_RO_COMPAT_VERITY         0x8000U
#define EXT4_RO_COMPAT_ORPHAN_PRESENT 0x10000U

#define EXT4_INCOMPAT_COMPRESSION 0x0001U
#define EXT4_INCOMPAT_FILETYPE    0x0002U
#define EXT4_INCOMPAT_RECOVER     0x0004U
#define EXT4_INCOMPAT_JOURNAL_DEV 0x0008U
#define EXT4_INCOMPAT_META_BG     0x0010U
#define EXT4_INCOMPAT_EXTENTS     0x0040U
#define EXT4_INCOMPAT_64BIT       0x0080U
#define EXT4_INCOMPAT_MMP         0x0100U
#define EXT4_INCOMPAT_FLEX_BG     0x0200U
#define EXT4_INCOMPAT_EA_INODE    0x0400U
#define EXT4_INCOMPAT_DIRDATA     0x1000U
#define EXT4_INCOMPAT_CSUM_SEED   0x2000U
#define EXT4_INCOMPAT_LARGEDIR    0x4000U
#define EXT4_INCOMPAT_INLINE_DATA 0x8000U
#define EXT4_INCOMPAT_ENCRYPT     0x10000U
#define EXT4_INCOMPAT_CASEFOLD    0x20000U

#define EXT4_INCOMPAT_SUPPORTED \
    (EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_RECOVER | EXT4_INCOMPAT_META_BG | \
     EXT4_INCOMPAT_EXTENTS | EXT4_INCOMPAT_64BIT | EXT4_INCOMPAT_MMP | EXT4_INCOMPAT_FLEX_BG | \
     EXT4_INCOMPAT_EA_INODE | EXT4_INCOMPAT_CSUM_SEED | EXT4_INCOMPAT_LARGEDIR | \
     EXT4_INCOMPAT_INLINE_DATA)
#define EXT4_INCOMPAT_READ_ONLY (EXT4_INCOMPAT_ENCRYPT | EXT4_INCOMPAT_CASEFOLD)
#define EXT4_RO_COMPAT_WRITABLE \
    (EXT4_RO_COMPAT_SPARSE_SUPER | EXT4_RO_COMPAT_LARGE_FILE | EXT4_RO_COMPAT_BTREE_DIR | \
     EXT4_RO_COMPAT_HUGE_FILE | EXT4_RO_COMPAT_GDT_CSUM | EXT4_RO_COMPAT_DIR_NLINK | \
     EXT4_RO_COMPAT_EXTRA_ISIZE | EXT4_RO_COMPAT_METADATA_CSUM | EXT4_RO_COMPAT_ORPHAN_PRESENT)

#define EXT4_BG_INODE_UNINIT 0x0001U
#define EXT4_BG_BLOCK_UNINIT 0x0002U
#define EXT4_BG_INODE_ZEROED 0x0004U

#define EXT4_SECRM_FL       0x00000001U
#define EXT4_INDEX_FL       0x00001000U
#define EXT4_ENCRYPT_FL     0x00000800U
#define EXT4_HUGE_FILE_FL   0x00040000U
#define EXT4_EXTENTS_FL     0x00080000U
#define EXT4_VERITY_FL      0x00100000U
#define EXT4_EA_INODE_FL    0x00200000U
#define EXT4_INLINE_DATA_FL 0x10000000U
#define EXT4_CASEFOLD_FL    0x40000000U

#define EXT4_S_IFMT   0xF000U
#define EXT4_S_IFSOCK 0xC000U
#define EXT4_S_IFLNK  0xA000U
#define EXT4_S_IFREG  0x8000U
#define EXT4_S_IFBLK  0x6000U
#define EXT4_S_IFDIR  0x4000U
#define EXT4_S_IFCHR  0x2000U
#define EXT4_S_IFIFO  0x1000U

#define EXT4_FT_UNKNOWN  0U
#define EXT4_FT_REG_FILE 1U
#define EXT4_FT_DIR      2U
#define EXT4_FT_CHRDEV   3U
#define EXT4_FT_BLKDEV   4U
#define EXT4_FT_FIFO     5U
#define EXT4_FT_SOCK     6U
#define EXT4_FT_SYMLINK  7U
#define EXT4_FT_DIR_CSUM 0xDEU

#define EXT4_EXT_MAGIC      0xF30AU
#define EXT4_EXT_INIT_MAX   32768U
#define EXT4_EXT_UNINIT_MAX 32767U
#define EXT4_EXT_MAX_DEPTH  5U

#define EXT4_XATTR_MAGIC             0xEA020000U
#define EXT4_XATTR_INDEX_USER        1U
#define EXT4_XATTR_INDEX_ACL_ACCESS  2U
#define EXT4_XATTR_INDEX_ACL_DEFAULT 3U
#define EXT4_XATTR_INDEX_TRUSTED     4U
#define EXT4_XATTR_INDEX_SECURITY    6U
#define EXT4_XATTR_INDEX_SYSTEM      7U

#define EXT4_ORPHAN_BLOCK_MAGIC 0x0B10CA04U
#define EXT4_MMP_MAGIC          0x004D4D50U
#define EXT4_MMP_SEQ_CLEAN      0xFF4D4D50U
#define EXT4_MMP_SEQ_FSCK       0xE24D4D50U
#define EXT4_MMP_SEQ_MAX        0xE24D4D4FU

#define DX_HASH_LEGACY            0U
#define DX_HASH_HALF_MD4          1U
#define DX_HASH_TEA               2U
#define DX_HASH_LEGACY_UNSIGNED   3U
#define DX_HASH_HALF_MD4_UNSIGNED 4U
#define DX_HASH_TEA_UNSIGNED      5U

struct ext4_super_block {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
    char s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t s_prealloc_blocks;
    uint8_t s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t s_def_hash_version;
    uint8_t s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_update_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t s_log_groups_per_flex;
    uint8_t s_checksum_type;
    uint8_t s_encryption_level;
    uint8_t s_reserved_pad;
    uint64_t s_kbytes_written;
    uint32_t s_snapshot_inum;
    uint32_t s_snapshot_id;
    uint64_t s_snapshot_r_blocks_count;
    uint32_t s_snapshot_list;
    uint32_t s_error_count;
    uint32_t s_first_error_time;
    uint32_t s_first_error_ino;
    uint64_t s_first_error_block;
    uint8_t s_first_error_func[32];
    uint32_t s_first_error_line;
    uint32_t s_last_error_time;
    uint32_t s_last_error_ino;
    uint32_t s_last_error_line;
    uint64_t s_last_error_block;
    uint8_t s_last_error_func[32];
    uint8_t s_mount_opts[64];
    uint32_t s_usr_quota_inum;
    uint32_t s_grp_quota_inum;
    uint32_t s_overhead_clusters;
    uint32_t s_backup_bgs[2];
    uint8_t s_encrypt_algos[4];
    uint8_t s_encrypt_pw_salt[16];
    uint32_t s_lpf_ino;
    uint32_t s_prj_quota_inum;
    uint32_t s_checksum_seed;
    uint8_t s_wtime_hi;
    uint8_t s_mtime_hi;
    uint8_t s_mkfs_time_hi;
    uint8_t s_lastcheck_hi;
    uint8_t s_first_error_time_hi;
    uint8_t s_last_error_time_hi;
    uint8_t s_first_error_errcode;
    uint8_t s_last_error_errcode;
    uint16_t s_encoding;
    uint16_t s_encoding_flags;
    uint32_t s_orphan_file_inum;
    uint32_t s_reserved[94];
    uint32_t s_checksum;
} __attribute__((packed));

struct ext4_group_desc {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed));

struct ext4_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_version;
    uint32_t i_block[EXT4_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_faddr;
    uint16_t i_blocks_high;
    uint16_t i_file_acl_high;
    uint16_t i_uid_high;
    uint16_t i_gid_high;
    uint16_t i_checksum_lo;
    uint16_t i_reserved;
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} __attribute__((packed));

struct ext4_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
} __attribute__((packed));

struct ext4_extent_header {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed));

struct ext4_extent {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed));

struct ext4_extent_idx {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed));

struct ext4_xattr_entry {
    uint8_t e_name_len;
    uint8_t e_name_index;
    uint16_t e_value_offs;
    uint32_t e_value_inum;
    uint32_t e_value_size;
    uint32_t e_hash;
    char e_name[];
} __attribute__((packed));

_Static_assert(sizeof(struct ext4_super_block) == 1024, "ext4 superblock layout");
_Static_assert(sizeof(struct ext4_group_desc) == 64, "ext4 group descriptor layout");
_Static_assert(sizeof(struct ext4_inode) == EXT4_INODE_FIELDS, "ext4 inode layout");
_Static_assert(sizeof(struct ext4_extent) == 12 && sizeof(struct ext4_extent_idx) == 12 &&
                   sizeof(struct ext4_extent_header) == 12,
               "ext4 extent layout");

struct ext4_buffer {
    struct ext4_buffer *hash_next;
    struct ext4_buffer *lru_prev;
    struct ext4_buffer *lru_next;
    uint64_t block;
    uint8_t dirty;
    uint8_t writing;
    uint8_t data[] __attribute__((aligned(16)));
};

#define EXT4_BUFFER_BUCKETS 4096U
#define EXT4_BUFFER_BYTES   (8U * 1024U * 1024U)

struct ext4_volume {
    struct ext4_volume *next;
    const struct block_device *device;
    struct vfs_node *root;
    int loading;
    int read_only;

    struct ext4_super_block sb;
    struct ext4_super_block sb_written;
    uint64_t sb_home_block;
    uint32_t sb_home_offset;

    uint8_t *gdt;
    uint8_t *gdt_written;
    uint8_t *group_dirty;
    uint32_t group_count;
    uint32_t desc_size;
    uint32_t gd_per_block;
    uint32_t gd_blocks;
    uint64_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint64_t blocks_count;

    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t inode_size;
    uint32_t inodes_per_block;
    uint32_t pointers;
    uint16_t extra_isize;

    uint32_t csum_seed;
    uint32_t hash_seed[4];
    uint32_t hash_unsigned;

    struct ext4_buffer **buffers;
    struct ext4_buffer *lru_head;
    struct ext4_buffer *lru_tail;
    uint32_t buffer_count;
    uint32_t buffer_limit;
    uint32_t dirty_buffers;

    uint64_t *pending_free;
    uint32_t pending_free_count;
    uint32_t pending_free_capacity;
    uint64_t last_commit_ns;

    uint32_t block_cursor_group;
    uint32_t block_cursor_bit;
    uint32_t inode_cursor_group;

    struct jbd2_journal *journal;
    uint32_t journal_ino;
    struct ext4_inode journal_inode;

    uint32_t orphan_ino;
    struct ext4_inode orphan_inode;
    uint32_t orphan_blocks;

    uint64_t mmp_block;
    uint32_t mmp_sequence;
    uint64_t mmp_last_ns;
};

extern struct ext4_volume *fs;
extern struct ext4_volume *ext4_volumes;
extern struct mutex ext4_lock;
extern struct mutex ext4_io_lock;
extern const char *ext4_failed_stage;
extern uint8_t ext4_scratch[EXT4_MAX_BLOCK_SIZE];

static inline int has_compat(uint32_t mask) { return (fs->sb.s_feature_compat & mask) != 0; }
static inline int has_incompat(uint32_t mask) { return (fs->sb.s_feature_incompat & mask) != 0; }
static inline int has_ro_compat(uint32_t mask) { return (fs->sb.s_feature_ro_compat & mask) != 0; }
static inline int has_csum(void) { return has_ro_compat(EXT4_RO_COMPAT_METADATA_CSUM); }
static inline int has_group_csum(void) {
    return has_ro_compat(EXT4_RO_COMPAT_METADATA_CSUM | EXT4_RO_COMPAT_GDT_CSUM);
}

static inline uint16_t get16(const void *at) {
    uint16_t value;
    __builtin_memcpy(&value, at, sizeof(value));
    return value;
}

static inline uint32_t get32(const void *at) {
    uint32_t value;
    __builtin_memcpy(&value, at, sizeof(value));
    return value;
}

static inline void put16(void *at, uint16_t value) { __builtin_memcpy(at, &value, sizeof(value)); }
static inline void put32(void *at, uint32_t value) { __builtin_memcpy(at, &value, sizeof(value)); }

struct ext4_volume *ext4_enter(struct ext4_volume *volume);
struct ext4_volume *ext4_op_enter(struct ext4_volume *volume);
void ext4_op_leave(struct ext4_volume *saved);
struct ext4_volume *ext4_volume_of(const struct vfs_node *node);
struct ext4_volume *ext4_active_volume_of(const struct vfs_node *node);
void ext4_report_failure(const char *what, const char *name);
uint32_t ext4_now(void);
void ext4_now_ns(uint32_t *seconds, uint32_t *nanoseconds);

int ext4_volume_read(struct ext4_volume *volume, uint64_t block, uint32_t count, void *out);
int ext4_volume_write(struct ext4_volume *volume, uint64_t block, uint32_t count, const void *data);
int ext4_volume_flush(struct ext4_volume *volume);

uint8_t *buf_get(uint64_t block);
uint8_t *buf_zero(uint64_t block);
void buf_mark(uint8_t *data);
void buf_forget(uint64_t block);
int buf_write(uint64_t block, const void *data);
int read_block(uint64_t block, void *out);
void buffers_trim(void);
void buffers_drop_all(void);
int ext4_flush_meta(void);
int ext4_volume_commit(void);
int ext4_commit_volumes(int force, int flush);
int ext4_sync_volume(struct ext4_volume *volume);

uint32_t ext4_csum(uint32_t seed, const void *data, size_t length);
uint32_t ext4_inode_seed(uint32_t ino, uint32_t generation);
void ext4_superblock_csum_set(struct ext4_super_block *sb);

struct ext4_group_desc *gd(uint32_t group);
uint64_t gd_block_bitmap(uint32_t group);
uint64_t gd_inode_bitmap(uint32_t group);
uint64_t gd_inode_table(uint32_t group);
uint32_t gd_free_blocks(uint32_t group);
uint32_t gd_free_inodes(uint32_t group);
uint32_t gd_used_dirs(uint32_t group);
uint32_t gd_itable_unused(uint32_t group);
void gd_set_free_blocks(uint32_t group, uint32_t value);
void gd_set_free_inodes(uint32_t group, uint32_t value);
void gd_set_used_dirs(uint32_t group, uint32_t value);
void gd_set_itable_unused(uint32_t group, uint32_t value);
uint64_t gd_location(uint32_t index);
uint64_t sb_blocks_count(void);
uint64_t sb_free_blocks(void);
uint64_t sb_reserved_blocks(void);
void sb_set_free_blocks(uint64_t value);
uint64_t group_first_block(uint32_t group);
uint32_t group_block_count(uint32_t group);
int group_has_super(uint32_t group);
uint32_t group_base_meta_blocks(uint32_t group);
void group_seal(uint32_t group);
void groups_seal_all(void);

uint64_t alloc_block(uint64_t goal, int below_4g);
void free_block(uint64_t block);
void release_block_now(uint64_t block);
void apply_pending_frees(void);
uint32_t alloc_inode(uint32_t parent_ino, int directory);
void free_inode(uint32_t ino, int is_directory);
void inode_group_dirs_inc(uint32_t ino);

int inode_read(uint32_t ino, struct ext4_inode *out);
int inode_write(uint32_t ino, const struct ext4_inode *in);
int inode_write_new(uint32_t ino, struct ext4_inode *in);
uint8_t *inode_slot(uint32_t ino, uint8_t **table_out);
uint32_t inode_seed(uint32_t ino, const struct ext4_inode *inode);
uint16_t inode_kind(const struct ext4_inode *inode);
uint64_t inode_size_of(const struct ext4_inode *inode);
void inode_set_size(struct ext4_inode *inode, uint64_t size);
uint64_t inode_sectors(const struct ext4_inode *inode);
void inode_set_sectors(struct ext4_inode *inode, uint64_t sectors);
void inode_count_block(struct ext4_inode *inode);
void inode_uncount_block(struct ext4_inode *inode);
uint32_t inode_uid(const struct ext4_inode *inode);
uint32_t inode_gid(const struct ext4_inode *inode);
void inode_set_owner(struct ext4_inode *inode, uint32_t uid, uint32_t gid);
uint64_t inode_file_acl(const struct ext4_inode *inode);
void inode_set_file_acl(struct ext4_inode *inode, uint64_t block);
int inode_has_extra(const struct ext4_inode *inode, uint32_t field_end);
void inode_get_times(const struct ext4_inode *inode, struct vfs_node *node);
void inode_set_times(struct ext4_inode *inode, uint32_t atime, uint32_t atime_ns, uint32_t mtime,
                     uint32_t mtime_ns, uint32_t ctime, uint32_t ctime_ns);
void inode_set_crtime(struct ext4_inode *inode, uint32_t seconds, uint32_t nanoseconds);
void inode_links_adjust(uint32_t ino, int delta);
int inode_is_fast_symlink(const struct ext4_inode *inode);
uint32_t inode_ea_inode_refs_get(const struct ext4_inode *inode);

int64_t inode_map(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, uint32_t *run,
                  int *unwritten);
int64_t inode_bmap(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, int alloc,
                   int *inode_dirty);
int inode_truncate_blocks(uint32_t ino, struct ext4_inode *inode, uint64_t keep);
void inode_release_blocks(uint32_t ino, struct ext4_inode *inode);
uint64_t inode_goal(uint32_t ino, struct ext4_inode *inode, uint64_t lblk);

int64_t ind_bmap(struct ext4_inode *inode, uint64_t lblk, int alloc, int *inode_dirty);
void ind_truncate(struct ext4_inode *inode, uint64_t keep);

void ext_init_root(struct ext4_inode *inode);
int64_t ext_map(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, uint32_t *run,
                int *unwritten);
int64_t ext_alloc(uint32_t ino, struct ext4_inode *inode, uint64_t lblk, int *inode_dirty);
int ext_truncate(uint32_t ino, struct ext4_inode *inode, uint64_t keep);
void ext_block_seal(uint32_t seed, uint8_t *block);

int dirent_sane(const struct ext4_dir_entry *entry, uint32_t at, uint32_t size);
uint32_t dirent_rec_len(uint32_t name_len);
int dir_tail_present(const uint8_t *block);
void dir_init_tail(uint8_t *block);
void dir_block_seal(uint32_t seed, uint8_t *block);
void dir_seal_any(uint32_t dir_ino, const struct ext4_inode *dir, uint64_t lblk, uint8_t *block);
int dir_add_entry(uint32_t dir_ino, const char *name, uint32_t child_ino, uint8_t file_type);
int dir_remove_entry(uint32_t dir_ino, const char *name);
int dir_set_dotdot(uint32_t dir_ino, uint32_t parent_ino);
int dir_create_block(uint32_t dir_ino, struct ext4_inode *dir, uint32_t parent_ino);
uint32_t ext4_dirhash(const char *name, size_t length, uint32_t version, const uint32_t seed[4],
                      uint32_t *minor);

struct ext4_xattr {
    uint8_t index;
    uint8_t name_len;
    char name[EXT4_NAME_LEN + 1];
    uint32_t value_size;
    uint32_t value_inum;
    uint32_t ea_hash;
    uint8_t *value;
};

struct ext4_xattr_set {
    struct ext4_xattr *items;
    uint32_t count;
    uint32_t capacity;
};

int xattr_load(uint32_t ino, const struct ext4_inode *inode, struct ext4_xattr_set *out);
void xattr_set_free(struct ext4_xattr_set *set);
int xattr_set_put(struct ext4_xattr_set *set, uint8_t index, const char *name, const void *value,
                  uint32_t size);
int xattr_set_remove(struct ext4_xattr_set *set, uint8_t index, const char *name);
struct ext4_xattr *xattr_set_find(struct ext4_xattr_set *set, uint8_t index, const char *name);
int xattr_store(uint32_t ino, struct ext4_inode *inode, const struct ext4_xattr_set *set);
void xattr_release(uint32_t ino, struct ext4_inode *inode);
int xattr_to_vfs(struct vfs_node *node, const struct ext4_xattr_set *set);
int xattr_from_vfs(const struct vfs_node *node, struct ext4_xattr_set *set);
uint32_t xattr_ibody_space(const struct ext4_inode *inode);

int inline_read(uint32_t ino, const struct ext4_inode *inode, uint8_t *out, uint64_t capacity);
uint64_t inline_size(uint32_t ino, const struct ext4_inode *inode);
int inline_expand(uint32_t ino, struct ext4_inode *inode);
int inline_dir_entries(uint32_t ino, const struct ext4_inode *inode, uint8_t **data,
                       uint32_t *length, uint32_t *parent);

int orphan_add(uint32_t ino);
int orphan_remove(uint32_t ino);
int orphans_replay(void);
int orphans_open(void);
void orphans_settle(void);

int mmp_start(void);
void mmp_tick(void);
void mmp_stop(void);

void ext4_set_persist_ops(void);
int ext4_load_tree(uint32_t root_ino, struct vfs_node *root);
void ext4_links_reset(void);
void ext4_restore_owner(struct vfs_node *node, const struct ext4_inode *inode);
void ext4_commit_kick(void);

#endif
