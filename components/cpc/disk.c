/*
 * The disk calls that change what's in the drives, and the disks' free
 * space for the web page.
 *
 * AMSDOS counts it in CP/M 2.2's directory (esp32emu's diskspace). The
 * format from track 0's lowest sector ID: data (&C1-&C9, no reserved
 * tracks), system (&41-&49, two: CP/M Plus disks too) or IBM (&01-&08,
 * one); 512-byte sectors, 1 KB blocks, 64 directory entries and 40 tracks
 * as AMSDOS's fixed parameter blocks have it (an image's extra tracks
 * aren't the filesystem's).
 *
 * Worked out at insert, eject and flush (where the disks may be read; the
 * writes in between are in memory and get flushed), kept for the web
 * page's task.
 */
#include "cpc_internal.h"
#include "diskspace.h"

typedef struct {
    volatile bool known;
    volatile uint64_t free, size;
} space_t;

static space_t s_space[CPC_DRIVES];

typedef struct {
    int drive;
    uint8_t first_id;
    uint32_t sectors;
} cpm_disk_t;

/* CP/M's track t, sector s: cylinder t, side 0, ID first_id + s */
static bool cpm_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    const cpm_disk_t *c = ctx;
    uint8_t *p = buf;
    while (len) {
        uint32_t n = offset / 512, in = offset % 512, part = 512 - in < len ? 512 - in : len;
        if (!upd765_disk_read(c->drive, n / c->sectors, 0, c->first_id + n % c->sectors, in, p, part)) {
            return false;
        }
        offset += part;
        p += part;
        len -= part;
    }
    return true;
}

static bool count(int drive, uint64_t *free_bytes, uint64_t *size)
{
    uint8_t ids[32];
    int n = upd765_disk_track_ids(drive, 0, 0, ids, sizeof(ids));
    if (n == 0) {
        return false;
    }
    uint8_t first = ids[0];
    for (int i = 1; i < n; i++) {
        first = ids[i] < first ? ids[i] : first;
    }
    diskspace_cpm_geometry_t g = { 512, 9, 40, 0, 1024, 64 };
    if (first == 0x41) {
        g.reserved_tracks = 2;
    } else if (first == 0x01) {
        g.sectors = 8;
        g.reserved_tracks = 1;
    } else if (first != 0xC1) {
        return false;
    }
    cpm_disk_t c = { drive, first, g.sectors };
    diskspace_disk_t d = { cpm_read, &c, 0 };
    diskspace_t s;
    if (!diskspace_cpm(&d, &g, &s)) {
        return false;
    }
    *free_bytes = s.free;
    *size = s.size;
    return true;
}

static void update(void)
{
    for (int d = 0; d < CPC_DRIVES; d++) {
        uint64_t f = 0, size = 0;
        bool known = count(d, &f, &size);
        s_space[d].known = false;
        s_space[d].free = f;
        s_space[d].size = size;
        s_space[d].known = known;
    }
}

bool cpc_disk_insert(int drive, const char *path)
{
    bool ok = upd765_disk_insert(drive, path);
    update();
    return ok;
}

void cpc_disk_eject(int drive)
{
    upd765_disk_eject(drive);
    update();
}

void cpc_disk_flush(void)
{
    upd765_disk_flush();
    update();
}

bool cpc_disk_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    if (drive < 0 || drive >= CPC_DRIVES || !s_space[drive].known) {
        return false;
    }
    *free_bytes = s_space[drive].free;
    *size = s_space[drive].size;
    return s_space[drive].known && *size > 0;
}
