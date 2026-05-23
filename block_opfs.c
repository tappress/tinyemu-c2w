/*
 * OPFS-backed BlockDevice for browser execution.
 *
 * Reads and writes are forwarded to two wasm imports that the host JS
 * (in the browser worker) provides. The host opens a backing file via
 * the Origin Private File System sync-access API (workers only) and
 * issues plain seek + read/write against it.
 *
 * The disk size is taken from the filename query string (opfs://<id>?size=<MiB>)
 * so no import is called during machine init — wizer pre-init can therefore
 * snapshot without needing host-provided block functions.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "cutils.h"
#include "virtio.h"

#ifdef WASI

#define OPFS_SECTOR_SIZE 512

/* These imports are resolved by the browser worker at instantiation time.
   They must NOT be called during wizer pre-init (no host implementation
   is available there); see size-from-config note above. */
__attribute__((import_module("c2w_blk"), import_name("read")))
extern int32_t c2w_blk_read(int32_t disk_id, int64_t sector,
                            int32_t n_sectors, void *buf);

__attribute__((import_module("c2w_blk"), import_name("write")))
extern int32_t c2w_blk_write(int32_t disk_id, int64_t sector,
                             int32_t n_sectors, const void *buf);

typedef struct {
    int32_t disk_id;
    int64_t nb_sectors;
} BlockDeviceOpfs;

static int64_t bo_get_sector_count(BlockDevice *bs)
{
    BlockDeviceOpfs *bo = bs->opaque;
    return bo->nb_sectors;
}

static int bo_read_async(BlockDevice *bs,
                         uint64_t sector_num, uint8_t *buf, int n,
                         BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceOpfs *bo = bs->opaque;
    int32_t ret = c2w_blk_read(bo->disk_id, (int64_t)sector_num, n, buf);
    return ret == 0 ? 0 : -1;
}

static int bo_write_async(BlockDevice *bs,
                          uint64_t sector_num, const uint8_t *buf, int n,
                          BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceOpfs *bo = bs->opaque;
    int32_t ret = c2w_blk_write(bo->disk_id, (int64_t)sector_num, n, buf);
    return ret == 0 ? 0 : -1;
}

/* Parse "opfs://<id>?size=<MiB>" — minimal, fail closed on anything weird. */
static int parse_opfs_url(const char *url, int32_t *disk_id, int64_t *size_mb)
{
    const char *p = url;
    if (strncmp(p, "opfs://", 7) != 0)
        return -1;
    p += 7;

    char *end;
    long id = strtol(p, &end, 10);
    if (end == p || id < 0 || id > 255)
        return -1;
    *disk_id = (int32_t)id;
    p = end;

    if (*p != '?') return -1;
    p++;
    if (strncmp(p, "size=", 5) != 0) return -1;
    p += 5;

    long sz = strtol(p, &end, 10);
    if (end == p || sz <= 0 || sz > 65536) /* cap at 64 GiB */
        return -1;
    *size_mb = sz;
    return 0;
}

BlockDevice *block_opfs_init(const char *url)
{
    int32_t disk_id;
    int64_t size_mb;
    if (parse_opfs_url(url, &disk_id, &size_mb) < 0) {
        fprintf(stderr, "block_opfs: bad URL '%s' (want opfs://<id>?size=<MiB>)\n", url);
        return NULL;
    }

    BlockDevice *bs = calloc(1, sizeof(*bs));
    BlockDeviceOpfs *bo = calloc(1, sizeof(*bo));
    bo->disk_id = disk_id;
    bo->nb_sectors = (size_mb * 1024 * 1024) / OPFS_SECTOR_SIZE;
    bs->opaque = bo;
    bs->get_sector_count = bo_get_sector_count;
    bs->read_async = bo_read_async;
    bs->write_async = bo_write_async;

    fprintf(stderr, "block_opfs: disk %d, %lld sectors (%lld MiB)\n",
            (int)disk_id, (long long)bo->nb_sectors, (long long)size_mb);
    return bs;
}

#else /* !WASI */

BlockDevice *block_opfs_init(const char *url)
{
    (void)url;
    fprintf(stderr, "block_opfs: not supported in native build\n");
    return NULL;
}

#endif /* WASI */
