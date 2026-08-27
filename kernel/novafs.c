/* ============================================================
 * NovaOS - NovaFS filesystem implementation with directories
 * inode 0 = root dir (parent = 0, name = "/")
 * ============================================================ */
#include "stdint.h"
#include "novafs.h"
#include "ata.h"

static int nx_str_eq(const char *a, const char *b){
    while(*a && *b){ if(*a!=*b) return 0; a++; b++; }
    return *a==*b;
}
static int nx_str_len(const char *s){ int n=0; while(s[n])n++; return n; }
static void nx_str_cpy(char *d,const char *s){ while((*d++=*s++)); }
static void nx_memset(void *d,int v,int n){ unsigned char *p=d; while(n--) *p++=(unsigned char)v; }
static void nx_memcpy(void *d,const void *s,int n){ unsigned char *dd=d; const unsigned char *ss=s; while(n--) *dd++=*ss++; }

/* The superblock lives in a full 512-byte sector buffer: ATA PIO always
 * transfers whole sectors, so reading into a bare 20-byte struct would
 * splash 492 bytes of sector data over neighbouring .bss variables. */
static uint8_t   sb_sec[FS_BLOCK_SIZE];
#define sb (*(super_t *)sb_sec)
static inode_t   inode_tab[FS_MAX_INODES];
static uint8_t   bmap[FS_BMAP_SECS*512];
static int       fs_ready = 0;
static int       cwdir = 0;  /* current working directory inode (0 = root) */

/* ---------- block bitmap ---------- */
static int bmap_alloc(void){
    for(int i = 0; i < FS_MAX_BLOCKS; i++){
        if(!(bmap[i>>3] & (1<<(i&7)))){
            bmap[i>>3] |= (1<<(i&7));
            return i;
        }
    }
    return -1;
}
static void bmap_free(int b){
    if(b >= 0 && b < FS_MAX_BLOCKS)
        bmap[b>>3] &= ~(1<<(b&7));
}

/* ---------- persistence ---------- */
static int load_inode_table(void){ return ata_read(FS_INODE_LBA, (uint8_t*)inode_tab, FS_INODE_SECS); }
static int store_inode_table(void){ return ata_write(FS_INODE_LBA, (uint8_t*)inode_tab, FS_INODE_SECS); }
static int load_bmap(void){ return ata_read(FS_BMAP_LBA, bmap, FS_BMAP_SECS); }
static int store_bmap(void){ return ata_write(FS_BMAP_LBA, bmap, FS_BMAP_SECS); }
static int load_super(void){ return ata_read(FS_SUPER_LBA, (uint8_t*)&sb, 1); }
static int store_super(void){ return ata_write(FS_SUPER_LBA, (uint8_t*)&sb, 1); }

/* clear all 64 bytes of an inode so no stale data leaks (name[], blocks[], size, ...) */
static void clear_inode(int idx){
    nx_memset(&inode_tab[idx], 0, sizeof(inode_t));
}

/* ---------- public API ---------- */
int fs_init(void){
    if(ata_read(FS_SUPER_LBA, (uint8_t*)&sb, 1) < 0) return -1;
    /* Auto-format on first boot / old layout disk. User never wants to
     * manually re-format after a kernel rebuild, so just do it silently.
     * If fs_format succeeds, fs_ready == 1 already. */
    if(sb.magic != FS_MAGIC){
        fs_format();
        return fs_ready ? 1 : -1;   /* 1 = auto-formatted this time */
    }
    if(load_inode_table() < 0) return -3;
    if(load_bmap() < 0) return -4;
    bmap[0] |= 1;
    fs_ready = 1;
    cwdir = 0;
    return 0;
}

int fs_is_ready(void){ return fs_ready; }

int fs_format(void){
    nx_memset(&sb, 0, sizeof(sb));
    sb.magic = FS_MAGIC;
    sb.total_blocks  = FS_MAX_BLOCKS;
    sb.total_inodes  = FS_MAX_INODES;
    sb.first_data_lba= FS_DATA_LBA;
    sb.first_inode_lba=FS_INODE_LBA;
    nx_memset(inode_tab, 0, sizeof(inode_tab));
    nx_memset(bmap, 0, sizeof(bmap));
    /* root dir: inode 0, parent = 0, name = "/" */
    inode_tab[0].type = T_DIR;
    inode_tab[0].parent = 0;
    inode_tab[0].name[0] = '/';
    inode_tab[0].name[1] = 0;
    bmap[0] |= 1;
    if(store_super() < 0) return -1;
    if(store_inode_table() < 0) return -2;
    if(store_bmap() < 0) return -3;
    fs_ready = 1;
    cwdir = 0;
    return 0;
}

/* find inode by name in given directory; return index or -1 */
int fs_find_in(int dir, const char *name){
    if(!fs_ready) return -1;
    if(dir < 0 || dir >= FS_MAX_INODES) return -1;
    for(int i = 1; i < FS_MAX_INODES; i++){
        if(inode_tab[i].type != T_FREE
            && inode_tab[i].parent == (uint16_t)dir
            && nx_str_eq(inode_tab[i].name, name))
            return i;
    }
    return -1;
}

int fs_find(const char *name){ return fs_find_in(cwdir, name); }

/* allocate a free inode, clear it, return index or -1 */
static int alloc_inode(void){
    for(int i = 1; i < FS_MAX_INODES; i++){
        if(inode_tab[i].type == T_FREE){
            clear_inode(i);
            return i;
        }
    }
    return -1;
}

/* safe name setter: zero the whole name[] first so trailing bytes
 * never contain stale garbage that would leak through kputs() or
 * confuse future string operations when this inode is re-read. */
static void set_name(int idx, const char *name){
    nx_memset(inode_tab[idx].name, 0, FS_NAME_LEN);
    for(int i = 0; i < FS_NAME_LEN - 1 && name[i]; i++)
        inode_tab[idx].name[i] = name[i];
    /* last byte stays 0 (NUL terminator guaranteed) */
}

int fs_create(const char *name){
    if(!fs_ready) return -1;
    if(fs_find(name) >= 0) return -1;
    if(nx_str_len(name) >= FS_NAME_LEN) return -1;
    int i = alloc_inode();   /* clears the whole inode */
    if(i < 0) return -1;
    inode_tab[i].type = T_FILE;
    inode_tab[i].parent = (uint16_t)cwdir;
    inode_tab[i].size = 0;
    set_name(i, name);
    /* block[] already zeroed by clear_inode */
    store_inode_table();
    return i;
}

/* ---------- directory ops ---------- */
int fs_mkdir(const char *name){
    if(!fs_ready) return -1;
    if(fs_find(name) >= 0) return -1;
    if(nx_str_len(name) >= FS_NAME_LEN) return -1;
    int i = alloc_inode();   /* clears the whole inode */
    if(i < 0) return -1;
    inode_tab[i].type = T_DIR;
    inode_tab[i].parent = (uint16_t)cwdir;
    inode_tab[i].size = 0;
    set_name(i, name);
    /* block[] already zeroed by clear_inode */
    store_inode_table();
    return 0;
}

int fs_rmdir(const char *name){
    if(!fs_ready) return -1;
    int idx = fs_find(name);
    if(idx < 0) return -1;
    inode_t *d = &inode_tab[idx];
    if(d->type != T_DIR) return -1;
    /* check empty: scan inodes whose parent == idx */
    for(int i = 1; i < FS_MAX_INODES; i++){
        if(inode_tab[i].type != T_FREE && inode_tab[i].parent == (uint16_t)idx)
            return -2;  /* not empty */
    }
    clear_inode(idx);      /* wipe every single byte: type=0 => T_FREE */
    store_inode_table();
    return 0;
}

/* Free a single file inode (release blocks + mark T_FREE).
 * Caller must have verified type != T_DIR. Uses in-memory index directly. */
static void kill_file_inode(int idx){
    inode_t *in = &inode_tab[idx];
    for(int b=0;b<NDIRECT;b++){
        if(in->block[b]){ bmap_free((int)in->block[b]); in->block[b]=0; }
    }
    clear_inode(idx);
}

/* Recursively delete all descendants of dir inode; then delete dir itself.
 * Works on in-memory indices. Caller persists the table + bmap afterwards. */
static void rmtree_recursive(int dir){
    for(int i = 1; i < FS_MAX_INODES; i++){
        if(inode_tab[i].type != T_FREE && inode_tab[i].parent == (uint16_t)dir){
            if(inode_tab[i].type == T_DIR){
                rmtree_recursive(i);
            }else{
                kill_file_inode(i);
            }
        }
    }
    clear_inode(dir);
}

/* Prevent the user from deleting the current working directory (or any
 * ancestor) — otherwise cwdir would point to a free inode. */
static int is_ancestor_of_cwd(int dir){
    int cur = cwdir;
    while(cur != 0){
        if(cur == dir) return 1;
        cur = (int)inode_tab[cur].parent;
    }
    return (dir == 0);  /* root is always ancestor of everything */
}

int fs_rmtree(const char *name){
    if(!fs_ready) return -1;
    int idx = fs_find(name);
    if(idx < 0) return -1;
    if(inode_tab[idx].type != T_DIR) return -2;
    if(is_ancestor_of_cwd(idx)) return -3;  /* safety: can't delete cwd chain */
    rmtree_recursive(idx);
    store_inode_table();
    store_bmap();
    return 0;
}

int fs_cd(const char *name){
    if(!fs_ready) return -1;
    /* special: ".." goes to parent */
    if(nx_str_eq(name, "..")){
        cwdir = (int)inode_tab[cwdir].parent;
        return 0;
    }
    /* special: "/" or "\" goes to root */
    if(nx_str_eq(name, "/") || nx_str_eq(name, "\\")){
        cwdir = 0;
        return 0;
    }
    int idx = fs_find(name);
    if(idx < 0) return -1;
    if(inode_tab[idx].type != T_DIR) return -1;
    cwdir = idx;
    return 0;
}

int fs_cwd(void){ return cwdir; }

int fs_getcwd(char *buf, int max){
    if(!fs_ready || max < 2) return -1;
    int stack[FS_MAX_INODES];
    int sp = 0;
    int cur = cwdir;
    while(cur != 0){
        if(sp >= FS_MAX_INODES) return -1;
        stack[sp++] = cur;
        cur = (int)inode_tab[cur].parent;
    }
    int pos = 0;
    buf[pos++] = '/';
    while(sp-- > 0){
        const char *n = inode_tab[stack[sp]].name;
        while(*n && pos < max-1) buf[pos++] = *n++;
        if(sp > 0 && pos < max-1) buf[pos++] = '/';
    }
    buf[pos] = 0;
    return pos;
}

/* ---------- file ops ---------- */
int fs_write(const char *name, const uint8_t *data, uint32_t len){
    if(!fs_ready) return -1;
    uint32_t need = (len + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
    if(need > NDIRECT) return -1;   /* check BEFORE creating, so an oversized
                                       write doesn't leave an empty file behind */
    int idx = fs_find(name);
    if(idx < 0) idx = fs_create(name);
    if(idx < 0) return -1;
    inode_t *in = &inode_tab[idx];

    for(int b=0;b<NDIRECT;b++){
        if(in->block[b]){ bmap_free(in->block[b]); in->block[b]=0; }
    }
    static uint8_t buf[FS_BLOCK_SIZE];
    for(uint32_t b=0; b<need; b++){
        int blk = bmap_alloc();
        if(blk < 0){
            /* disk full mid-write: keep only the fully written prefix and
               drop the dangling block pointers, so size matches the data */
            in->size = b * FS_BLOCK_SIZE;
            for(uint32_t k=b;k<NDIRECT;k++) in->block[k]=0;
            store_inode_table(); store_bmap();
            return -1;
        }
        in->block[b] = (uint32_t)blk;
        nx_memset(buf, 0, FS_BLOCK_SIZE);
        uint32_t n = len - b*FS_BLOCK_SIZE;
        if(n > FS_BLOCK_SIZE) n = FS_BLOCK_SIZE;
        nx_memcpy(buf, data + b*FS_BLOCK_SIZE, (int)n);
        ata_write(FS_DATA_LBA + blk, buf, 1);
    }
    in->size = len;
    store_inode_table();
    store_bmap();
    return (int)len;
}

int fs_read(const char *name, uint8_t *buf, uint32_t max){
    if(!fs_ready) return -1;
    int idx = fs_find(name);
    if(idx < 0) return -1;
    inode_t *in = &inode_tab[idx];
    uint32_t n = in->size;
    if(n > max) n = max;
    uint32_t need = (in->size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
    if(need > NDIRECT) need = NDIRECT;
    static uint8_t blkbuf[FS_BLOCK_SIZE];
    uint32_t off = 0;
    for(uint32_t b=0; b<need && off < n; b++){
        if(in->block[b] == 0) break;
        ata_read(FS_DATA_LBA + in->block[b], blkbuf, 1);
        for(uint32_t i=0;i<FS_BLOCK_SIZE && off<n;i++){
            buf[off++] = blkbuf[i];
        }
    }
    return (int)n;
}

int fs_size(const char *name){
    if(!fs_ready) return -1;
    int idx = fs_find(name);
    if(idx < 0) return -1;
    return (int)inode_tab[idx].size;
}

int fs_remove(const char *name){
    if(!fs_ready) return -1;
    int idx = fs_find(name);
    if(idx < 0) return -1;
    inode_t *in = &inode_tab[idx];
    if(in->type == T_DIR) return -1;  /* use rmdir for dirs */
    for(int b=0;b<NDIRECT;b++){
        if(in->block[b]){ bmap_free((int)in->block[b]); in->block[b]=0; }
    }
    clear_inode(idx);      /* wipe ALL bytes — type becomes 0 => T_FREE,
                              parent becomes 0 so rmdir on the parent works,
                              name[] becomes all zeros so no stray display */
    store_inode_table();
    store_bmap();
    return 0;
}

int fs_list(void (*cb)(const char*,int,uint32_t)){
    return fs_list_dir(cwdir, cb);
}

int fs_list_dir(int dir, void (*cb)(const char*,int,uint32_t)){
    if(!fs_ready) return -1;
    if(dir < 0 || dir >= FS_MAX_INODES) return -1;
    int n = 0;
    for(int i = 1; i < FS_MAX_INODES; i++){
        if(inode_tab[i].type != T_FREE && inode_tab[i].parent == (uint16_t)dir){
            cb(inode_tab[i].name, (int)inode_tab[i].type, inode_tab[i].size);
            n++;
        }
    }
    return n;
}
