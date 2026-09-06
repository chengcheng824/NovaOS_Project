/* ============================================================
 * NovaOS - NovaFS tiny filesystem on-disk layout
 * ============================================================
 * Lives on the DATA disk (primary IDE slave, data.img) - separate from
 * the boot disk - so rebuilding the boot image never wipes user data.
 * Layout (LBA, 512B sectors):
 *   0   - 128   reserved
 *   129         Superblock
 *   130 - 161   Inode table (32 sectors, 256 inodes x 64B)
 *   162 - 169   Data block bitmap (8 sectors, 32768 blocks)
 *   170 +       Data blocks (512B each)
 * ============================================================ */
#ifndef NEXFS_H
#define NEXFS_H

#include "stdint.h"

#define FS_MAGIC        0x4E584653u   /* "NXFS" — kept from the old name so existing disks still mount */
#define FS_SUPER_LBA    129
#define FS_INODE_LBA    130
#define FS_INODE_SECS   32
#define FS_BMAP_LBA     162
#define FS_BMAP_SECS    8
#define FS_DATA_LBA     170
#define FS_BLOCK_SIZE   512
#define FS_MAX_INODES   256            /* 32 sectors / 64B */
#define FS_MAX_BLOCKS   (8 * 512 * 8)  /* 8 sectors bitmap = 32768 bits */
#define FS_NAME_LEN     24

/* inode types */
#define T_FREE   0
#define T_FILE   1
#define T_DIR    2

/* Direct block pointers per inode.
 * 64B inode: type(1)+pad(1)+parent(2)+name(24)+size(4)+blocks(6*4=24)+pad(6)=64 */
#define NDIRECT 6

typedef struct {
    uint8_t  type;                 /* T_FREE / T_FILE / T_DIR        */
    uint8_t  flags;                /* unused/reserved                */
    uint16_t parent;               /* parent directory inode index   */
    char     name[FS_NAME_LEN];    /* file name, NUL-terminated      */
    uint32_t size;                 /* size in bytes                  */
    uint32_t block[NDIRECT];       /* direct block indices (0=none)  */
    uint8_t  pad[6];
} inode_t;                         /* = 64 bytes                     */

typedef struct {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t first_data_lba;
    uint32_t first_inode_lba;
} super_t;

/* ---- NovaFS public API ---- */
int fs_init(void);
int fs_is_ready(void);
int fs_format(void);
int fs_find(const char *name);              /* find in current dir */
int fs_find_in(int dir, const char *name);  /* find in given dir */
int fs_create(const char *name);
int fs_write(const char *name, const uint8_t *data, uint32_t len);
int fs_read(const char *name, uint8_t *buf, uint32_t max);
                                            /* -2 = read permission denied */
int fs_size(const char *name);
int fs_remove(const char *name);
int fs_list(void (*cb)(const char *name, int type, uint32_t size, uint8_t mode));
int fs_list_dir(int dir, void (*cb)(const char*,int,uint32_t,uint8_t));
int fs_space(uint32_t *used_blocks, uint32_t *used_inodes, uint32_t *used_bytes);
void fs_setuid(uint8_t uid);   /* owner uid for newly created inodes (0=root) */
uint8_t fs_getuid(void);       /* current caller uid (0 = root)               */
void fs_setcwd(int idx);       /* per-process cwd switch (validated)          */

/* permission mode byte (inode pad[4]): high nibble = owner rwx,
 * low nibble = others rwx; 0 = legacy default rwxr-x. */
int fs_chmod(const char *name, uint8_t mode);  /* owner/root only, -2 denied  */
int fs_may_read(const char *name);             /* 1 = allowed                 */
int fs_may_exec(const char *name);             /* 1 = allowed                 */

/* directory ops */
int fs_mkdir(const char *name);
int fs_rmdir(const char *name);
int fs_rmtree(const char *name);             /* recursive force delete dir */
int fs_cd(const char *name);
int fs_cwd(void);                           /* return current dir inode idx */
int fs_getcwd(char *buf, int max);          /* build path string */

#endif /* NEXFS_H */
