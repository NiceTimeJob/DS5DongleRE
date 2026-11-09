#include "macro_store.h"
#include "lfs_port.h"
#include "debug_log.h"
#include <string.h>

static struct lfs_context s_ctx = { .partition_name = "media" };
static struct lfs_config  s_cfg = {
    .read_size      = 256,
    .prog_size      = 256,
    .lookahead_size = 256,
    .cache_size     = 512,
    .block_size     = 4096,
    .block_cycles   = 500,
};

static lfs_t *s_lfs = NULL;

static map_macro_dir_t s_dir[MAP_MACRO_MAX_COUNT];
static bool            s_present[MAP_MACRO_MAX_COUNT];
static int             s_count;
static uint32_t        s_free_bytes;

static uint8_t  s_stage[MAP_MACRO_MAX_BYTES];
static uint32_t s_stage_len;
static uint8_t  s_stage_id;
static bool     s_stage_active;

static void file_name(uint8_t id, char *buf)
{
    buf[0] = 'm';
    buf[1] = (char)('0' + (id / 10));
    buf[2] = (char)('0' + (id % 10));
    buf[3] = '\0';
}

static int parse_id(const char *name, uint8_t *id)
{
    if (name[0] != 'm' || name[1] < '0' || name[1] > '9' ||
        name[2] < '0' || name[2] > '9' || name[3] != '\0')
        return -1;
    int v = (name[1] - '0') * 10 + (name[2] - '0');
    if (v >= MAP_MACRO_MAX_COUNT)
        return -1;
    *id = (uint8_t)v;
    return 0;
}

static void update_free(void)
{
    if (!s_lfs || s_cfg.block_count == 0)
        return;
    lfs_ssize_t used = lfs_fs_size(s_lfs);
    if (used >= 0 && used <= (lfs_ssize_t)s_cfg.block_count)
        s_free_bytes = (uint32_t)(s_cfg.block_count - (uint32_t)used) * s_cfg.block_size;
}

static void read_header_name(uint8_t id, uint16_t *size_out)
{
    lfs_file_t f;
    map_macro_hdr_t hdr;
    char name[8];
    file_name(id, name);

    s_dir[id].id = id;
    s_dir[id].flags = 0;
    s_dir[id].size = 0;
    s_dir[id].name[0] = '\0';

    if (lfs_file_open(s_lfs, &f, name, LFS_O_RDONLY) != 0)
        return;

    lfs_ssize_t rd = lfs_file_read(s_lfs, &f, &hdr, sizeof(hdr));
    lfs_soff_t sz = lfs_file_size(s_lfs, &f);
    lfs_file_close(s_lfs, &f);

    if (rd == (lfs_ssize_t)sizeof(hdr) && hdr.magic == MAP_MACRO_MAGIC) {
        memcpy(s_dir[id].name, hdr.name, MAP_MACRO_NAME_LEN);
        s_dir[id].name[MAP_MACRO_NAME_LEN - 1] = '\0';
    }
    if (sz >= 0 && sz <= 0xFFFF)
        s_dir[id].size = (uint16_t)sz;
    if (size_out)
        *size_out = s_dir[id].size;
}

static void scan_dir(void)
{
    memset(s_present, 0, sizeof(s_present));
    s_count = 0;

    if (!s_lfs) {
        update_free();
        return;
    }

    lfs_dir_t dir;
    struct lfs_info info;
    if (lfs_dir_open(s_lfs, &dir, "/") == 0) {
        while (lfs_dir_read(s_lfs, &dir, &info) > 0) {
            if (info.type != LFS_TYPE_REG)
                continue;
            uint8_t id;
            if (parse_id(info.name, &id) != 0)
                continue;
            if (s_present[id])
                continue;
            s_present[id] = true;
            read_header_name(id, NULL);
            s_count++;
        }
        lfs_dir_close(s_lfs, &dir);
    }
    update_free();
}

bool macro_store_init(void)
{
    if (s_lfs)
        return true;
    s_lfs = lfs_xip_init(&s_ctx, &s_cfg);
    if (!s_lfs) {
        LOG_ERR("[MAC] littlefs mount on 'media' failed\n");
        return false;
    }
    LOG_INF("[MAC] littlefs mounted on 'media' (%u blocks x %u B)\n",
            (unsigned)s_cfg.block_count, (unsigned)s_cfg.block_size);
    scan_dir();
    LOG_INF("[MAC] %d macro(s), free %u bytes\n", s_count, (unsigned)s_free_bytes);
    return true;
}

lfs_t *macro_store_lfs(void)
{
    return s_lfs;
}

int macro_store_count(void)
{
    return s_count;
}

const map_macro_dir_t *macro_store_dir(void)
{
    return s_dir;
}

bool macro_store_find(uint8_t id, uint32_t *size)
{
    if (id >= MAP_MACRO_MAX_COUNT || !s_present[id])
        return false;
    if (s_dir[id].size == 0)
        return false;
    if (size)
        *size = s_dir[id].size;
    return true;
}

int macro_store_read(uint8_t id, uint32_t off, uint8_t *buf,
                     uint16_t len, uint16_t *got)
{
    if (!s_lfs || id >= MAP_MACRO_MAX_COUNT || !s_present[id])
        return -1;
    if (got)
        *got = 0;
    if (len == 0)
        return 0;

    /* serve staged (uncommitted) content when the same id is staged */
    if (s_stage_active && s_stage_id == id) {
        if (off >= s_stage_len)
            return 0;
        uint32_t avail = s_stage_len - off;
        if (avail > len)
            avail = len;
        memcpy(buf, s_stage + off, avail);
        if (got)
            *got = (uint16_t)avail;
        return 0;
    }

    lfs_file_t f;
    char name[8];
    file_name(id, name);
    if (lfs_file_open(s_lfs, &f, name, LFS_O_RDONLY) != 0)
        return -1;
    lfs_file_seek(s_lfs, &f, (lfs_soff_t)off, LFS_SEEK_SET);
    lfs_ssize_t rd = lfs_file_read(s_lfs, &f, buf, len);
    lfs_file_close(s_lfs, &f);
    if (rd < 0)
        return -1;
    if (got)
        *got = (uint16_t)rd;
    return 0;
}

int macro_store_erase(uint8_t id)
{
    if (!s_lfs || id >= MAP_MACRO_MAX_COUNT)
        return -1;

    if (s_stage_active && s_stage_id == id) {
        s_stage_active = false;
        s_stage_len = 0;
    }

    char name[8];
    file_name(id, name);
    int ret = lfs_remove(s_lfs, name);
    if (ret != 0 && ret != LFS_ERR_NOENT)
        return -1;

    if (s_present[id]) {
        s_present[id] = false;
        s_dir[id].size = 0;
        s_dir[id].name[0] = '\0';
        if (s_count > 0)
            s_count--;
        update_free();
    }
    LOG_INF("[MAC] erased macro %u\n", id);
    return 0;
}

int macro_store_stage_write(uint8_t id, uint32_t off,
                            const uint8_t *data, uint16_t len)
{
    if (id >= MAP_MACRO_MAX_COUNT)
        return -1;
    if (off + len > MAP_MACRO_MAX_BYTES)
        return -1;

    if (!s_stage_active || s_stage_id != id) {
        if (!s_stage_active) {
            memset(s_stage, 0, sizeof(s_stage));
            s_stage_len = 0;
        }
        s_stage_active = true;
        s_stage_id = id;
    }

    if (off > s_stage_len)
        memset(s_stage + s_stage_len, 0, off - s_stage_len); /* gap fill */

    memcpy(s_stage + off, data, len);
    if (off + len > s_stage_len)
        s_stage_len = off + len;
    return 0;
}

bool macro_store_stage_busy(void)
{
    return s_stage_active;
}

uint8_t macro_store_stage_id(void)
{
    return s_stage_id;
}

int macro_store_stage_commit(void)
{
    if (!s_lfs || !s_stage_active || s_stage_len == 0)
        return -1;

    uint8_t id = s_stage_id;
    char name[8];
    file_name(id, name);

    lfs_file_t f;
    int ret = lfs_file_open(s_lfs, &f, name,
                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (ret != 0)
        return -1;

    lfs_ssize_t wr = lfs_file_write(s_lfs, &f, s_stage, s_stage_len);
    if (wr != (lfs_ssize_t)s_stage_len) {
        lfs_file_close(s_lfs, &f);
        return -1;
    }
    if (lfs_file_close(s_lfs, &f) != 0)
        return -1;

    s_stage_active = false;
    s_stage_len = 0;

    s_present[id] = true;
    read_header_name(id, NULL);
    if (!s_present[id])
        return -1;
    /* recount */
    s_count = 0;
    for (int i = 0; i < MAP_MACRO_MAX_COUNT; i++)
        if (s_present[i])
            s_count++;
    update_free();
    LOG_INF("[MAC] committed macro %u (%u bytes)\n", id,
            (unsigned)s_dir[id].size);
    return 0;
}

uint32_t macro_store_free_bytes(void)
{
    return s_free_bytes;
}
