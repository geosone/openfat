/* exFAT volume check (EXCHK), after the Microsoft exFAT file system
   specification.  Reads only.  Errors about the structure (boot region,
   entry sets, chains) are reported in the first pass; errors about single
   clusters (lost, unmarked, cross-linked) in the pass whose window holds
   the cluster. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "openaspi/exfat_check.h"

#define MAXS     OA_EXCHK_MAXSECT
#define NOBUF    0xFFFFFFFFUL
#define EOC      0xFFFFFFFFUL
#define BADCLU   0xFFFFFFF7UL
#define MAXDEPTH 64
#define MAXPAIRS 4096
#define MAXRANGE 10                     /* listed ranges of each kind */

struct cache { oa_u32 lba; oa_u8 b[MAXS]; };
/* a file or directory: its chain and the cluster last walked to */
struct chain { oa_u32 first, size, idx, clu; int nofat, root; };

static struct cache cdir, cfat;
static oa_u8 boot[MAXS], other[MAXS];
static struct oa_exchk *C;
static oa_u32 fat, heap, nclu, root;
static unsigned sshift, cshift, ssize;
static oa_u32 lo, hi;                   /* the window: clusters lo+2 .. hi+1 */
static int first_pass;
static struct chain bitmap, upcase;
static oa_u16 *pairs;                   /* upcase table: character, upper case */
static unsigned npairs;
static int have_upcase;
static char path[300];
static unsigned plen;
static char text[400];
static unsigned nlost, nunmarked;       /* ranges reported */

static oa_u16 rd16(const oa_u8 *p) { return (oa_u16)(p[0] | p[1] << 8); }
static oa_u32 rd32(const oa_u8 *p) { return rd16(p) | (oa_u32)rd16(p+2) << 16; }

static void report(int level, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsprintf(text, fmt, a);
    va_end(a);
    if (level == OA_EXCHK_ERROR) ++C->errors;
    if (level == OA_EXCHK_WARN) ++C->warnings;
    C->msg(C->ctx, level, text);
}

static const char *where(void) { return plen ? path : "\\"; }

static const oa_u8 *sec(struct cache *c, oa_u32 lba)
{
    if (c->lba != lba) {
        c->lba = NOBUF;
        if (C->read(C->dev, C->start + lba, c->b)) {
            report(OA_EXCHK_ERROR, "Read error at sector %lu", (unsigned long)lba);
            return 0;
        }
        c->lba = lba;
    }
    return c->b;
}

static int fat_get(oa_u32 clu, oa_u32 *next)
{
    const oa_u8 *s = sec(&cfat, fat + (clu >> (sshift - 2)));
    if (!s) return -1;
    *next = rd32(s + ((unsigned)(clu << 2) & (ssize - 1)));
    return 0;
}

static int valid(oa_u32 clu) { return clu >= 2 && clu < nclu + 2; }

/* the sector holding byte off of a file, NOBUF past its chain */
static oa_u32 file_lba(struct chain *f, oa_u32 off)
{
    oa_u32 idx = off >> (sshift + cshift), next;
    if (!f->root && off >= f->size) return NOBUF;
    if (f->nofat) {
        f->idx = idx; f->clu = f->first + idx;
    } else {
        if (idx < f->idx || !f->clu) { f->idx = 0; f->clu = f->first; }
        while (f->idx < idx) {
            if (!valid(f->clu) || fat_get(f->clu, &next)) return NOBUF;
            f->clu = next; ++f->idx;
        }
    }
    if (!valid(f->clu)) return NOBUF;
    return heap + ((f->clu - 2) << cshift) + ((off >> sshift) & ((1UL << cshift) - 1));
}

/* the 32-byte entry at pos of a directory, 0 at its end */
static const oa_u8 *entry(struct chain *d, oa_u32 pos)
{
    oa_u32 lba = file_lba(d, pos);
    const oa_u8 *s;
    if (lba == NOBUF || !(s = sec(&cdir, lba))) return 0;
    return s + ((unsigned)pos & (ssize - 1));
}

/* ---- clusters ---- */

static void use(oa_u32 clu)
{
    if (clu < lo + 2 || clu >= hi + 2) return;
    if (C->mark(C->ctx, clu - 2 - lo)) {
        ++C->cross;
        report(OA_EXCHK_ERROR, "%s: cluster %lu is used twice (cross-linked)", where(), (unsigned long)clu);
    }
}

/* Marks the clusters of a chain; count: clusters by the size, 0 = to the
   end of the chain (root directory). */
static void use_chain(oa_u32 first, int nofat, oa_u32 count)
{
    oa_u32 c = first, n = 0, next, a, b;
    if (!valid(first)) {
        if (first_pass) report(OA_EXCHK_ERROR, "%s: first cluster %lu is outside the volume", where(), (unsigned long)first);
        return;
    }
    if (nofat) {
        if (count > nclu + 2 - first) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: contiguous clusters run past the end of the volume", where());
            count = nclu + 2 - first;
        }
        a = first > lo + 2 ? first : lo + 2;
        b = first + count < hi + 2 ? first + count : hi + 2;
        for (; a < b; ++a) use(a);
        return;
    }
    for (;;) {
        use(c); ++n;
        if (fat_get(c, &next)) return;
        if (next == EOC) break;
        if (count && n >= count) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: cluster chain is longer than the size", where());
            return;
        }
        if (n >= nclu) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: cluster chain does not end", where());
            return;
        }
        if (next == BADCLU) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: cluster chain runs into a bad cluster", where());
            return;
        }
        if (!valid(next)) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: invalid FAT entry %08lX in the cluster chain", where(), (unsigned long)next);
            return;
        }
        c = next;
    }
    if (count && n < count && first_pass)
        report(OA_EXCHK_ERROR, "%s: cluster chain has %lu of %lu clusters", where(), (unsigned long)n, (unsigned long)count);
}

/* clusters for size_hi:size bytes, 0FFFFFFFFh when more than a volume has */
static oa_u32 clusters_of(oa_u32 size_hi, oa_u32 size)
{
    unsigned shift = sshift + cshift;
    oa_u32 n;
    if (size_hi >> shift) return 0xFFFFFFFFUL;
    n = (size_hi << (32 - shift)) | (size >> shift);
    if (size & ((1UL << shift) - 1)) ++n;
    return n;
}

/* ---- names ---- */

static oa_u16 up(oa_u16 c)
{
    unsigned a = 0, b = npairs, m;
    while (a < b) {
        m = (a + b) / 2;
        if (pairs[2*m] == c) return pairs[2*m+1];
        if (pairs[2*m] < c) a = m + 1; else b = m;
    }
    return c;
}

static char oem(oa_u16 c)
{
    unsigned i;
    if (c < 0x80) return (char)(c < 0x20 ? '?' : c);
    if (C->cp) for (i = 0; i < 128; ++i) if (C->cp->uni[i] == c) return (char)(0x80 + i);
    return '?';
}

static oa_u16 rot16(oa_u16 sum, oa_u8 b) { return (oa_u16)(((sum & 1) ? 0x8000u : 0) + (sum >> 1) + b); }
static oa_u32 rot32(oa_u32 sum, oa_u8 b) { return ((sum & 1) ? 0x80000000UL : 0) + (sum >> 1) + b; }

/* ---- the root directory's system entries ---- */

static void load_upcase(oa_u32 want)
{
    oa_u32 off, sum = 0, lba, idx = 0;
    const oa_u8 *s;
    oa_u16 v, prev = 0;
    int run = 0, half = 0;
    unsigned i;
    npairs = 0;
    if (!pairs) pairs = malloc(MAXPAIRS * 4);
    for (off = 0; off < upcase.size; off += ssize) {
        if ((lba = file_lba(&upcase, off)) == NOBUF || !(s = sec(&cdir, lba))) return;
        for (i = 0; i < ssize && off + i < upcase.size; ++i) {
            sum = rot32(sum, s[i]);
            if (!half) { prev = s[i]; half = 1; continue; }
            half = 0;
            v = (oa_u16)(prev | s[i] << 8);
            if (run) { idx += v; run = 0; continue; }
            if (v == 0xFFFF && idx < 0xFFFF) { run = 1; continue; }
            if (v != idx && pairs && npairs < MAXPAIRS) {
                pairs[2*npairs] = (oa_u16)idx; pairs[2*npairs+1] = v; ++npairs;
            }
            ++idx;
        }
    }
    if (sum != want) report(OA_EXCHK_ERROR, "Up-case table checksum is wrong");
    else have_upcase = pairs != 0 && npairs < MAXPAIRS;
}

static void system_entries(void)
{
    struct chain r;
    const oa_u8 *e;
    oa_u32 pos, upsum = 0;
    unsigned nbitmap = 0, nupcase = 0;
    memset(&r, 0, sizeof(r));
    r.first = root; r.root = 1;
    memset(&bitmap, 0, sizeof(bitmap));
    memset(&upcase, 0, sizeof(upcase));
    for (pos = 0; (e = entry(&r, pos)) != 0 && e[0]; pos += 32) {
        if (e[0] == 0x81 && !(e[1] & 1)) {
            ++nbitmap;
            bitmap.first = rd32(e+20); bitmap.size = rd32(e+24);
        } else if (e[0] == 0x82) {
            ++nupcase;
            upsum = rd32(e+4); upcase.first = rd32(e+20); upcase.size = rd32(e+24);
        }
    }
    if (nbitmap != 1) report(OA_EXCHK_ERROR, "%u allocation bitmaps in the root directory", nbitmap);
    if (nupcase != 1) report(OA_EXCHK_ERROR, "%u up-case tables in the root directory", nupcase);
    if (nbitmap && bitmap.size < (nclu + 7) / 8) {
        report(OA_EXCHK_ERROR, "Allocation bitmap is too small");
        bitmap.first = 0;
    }
    if (nbitmap && !valid(bitmap.first)) {
        report(OA_EXCHK_ERROR, "Allocation bitmap starts outside the volume");
        bitmap.first = 0;
    }
    if (nupcase && valid(upcase.first) && upcase.size && upcase.size <= 0x20000UL) load_upcase(upsum);
    else if (nupcase) report(OA_EXCHK_ERROR, "Up-case table is invalid");
}

/* ---- directories ---- */

static void walk_dir(struct chain *d, unsigned depth);

/* The entry set at *pos: checks it, marks its clusters, enters a
   directory.  *pos is left at its last entry. */
static void file_set(struct chain *d, oa_u32 *pos, unsigned depth)
{
    const oa_u8 *e = entry(d, *pos);
    unsigned n = e[1], i, k, attr = rd16(e+4), namelen = 0, got = 0, bad = 0, base = plen;
    oa_u16 want = rd16(e+2), sum, hash = 0, h = 0, c;
    oa_u8 sflags = 0;
    oa_u32 first = 0, size = 0, size_hi = 0, vdl = 0, vdl_hi = 0, count;
    struct chain sub;
    if (n < 2 || n > 18) {
        if (first_pass) report(OA_EXCHK_ERROR, "%s: file entry with %u secondary entries", where(), n);
        return;
    }
    sum = 0;
    for (i = 0; i < 32; ++i) if (i != 2 && i != 3) sum = rot16(sum, e[i]);
    if (plen < sizeof(path) - 2) { path[plen++] = '\\'; path[plen] = 0; }
    for (i = 1; i <= n; ++i) {
        if (!(e = entry(d, *pos + 32UL*i))) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: entry set is cut off", where());
            goto out;
        }
        for (k = 0; k < 32; ++k) sum = rot16(sum, e[k]);
        if (i == 1) {
            if (e[0] != 0xC0) {
                if (first_pass) report(OA_EXCHK_ERROR, "%s: file entry without stream entry", where());
                goto out;
            }
            sflags = e[1]; namelen = e[3]; hash = rd16(e+4);
            vdl = rd32(e+8); vdl_hi = rd32(e+12);
            first = rd32(e+20); size = rd32(e+24); size_hi = rd32(e+28);
        } else if (e[0] == 0xC1 && i <= 1 + (namelen + 14) / 15) {
            for (k = 0; k < 15 && got < namelen; ++k, ++got) {
                c = rd16(e + 2 + 2*k);
                if (c < 0x20 || (c < 0x80 && strchr("\"*/:<>?\\|", (char)c))) bad = 1;
                h = rot16(h, (oa_u8)up(c)); h = rot16(h, (oa_u8)(up(c) >> 8));
                if (plen < sizeof(path) - 1) { path[plen++] = oem(c); path[plen] = 0; }
            }
        } else if (e[0] < 0xE0) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: entry set broken by entry type %02Xh", where(), e[0]);
            goto out;
        }
    }
    *pos += 32UL * n;
    if (sum != want) {
        if (first_pass) report(OA_EXCHK_ERROR, "%s: entry set checksum is wrong", where());
        goto out;
    }
    if (!namelen || got != namelen) {
        if (first_pass) report(OA_EXCHK_ERROR, "%s: name length %u, %u characters", where(), namelen, got);
        goto out;
    }
    if (first_pass) {
        if (bad) report(OA_EXCHK_ERROR, "%s: invalid character in the name", where());
        if (have_upcase && h != hash) report(OA_EXCHK_ERROR, "%s: name hash is wrong", where());
        if (vdl_hi > size_hi || (vdl_hi == size_hi && vdl > size))
            report(OA_EXCHK_ERROR, "%s: valid data length is larger than the size", where());
        if (attr & OA_EXFAT_DIR) ++C->dirs; else ++C->files;
    }
    if (!(sflags & 1)) {                        /* no allocation possible */
        if ((first || size || size_hi) && first_pass)
            report(OA_EXCHK_ERROR, "%s: clusters without allocation", where());
        goto out;
    }
    if (!size && !size_hi) {
        if (first && first_pass) report(OA_EXCHK_WARN, "%s: empty, but with first cluster %lu", where(), (unsigned long)first);
        goto out;
    }
    count = clusters_of(size_hi, size);
    if (count == 0xFFFFFFFFUL || count > nclu) {
        if (first_pass) report(OA_EXCHK_ERROR, "%s: size is larger than the volume", where());
        goto out;
    }
    use_chain(first, sflags & 2, count);
    if (!(attr & OA_EXFAT_DIR)) goto out;
    if (size_hi || size & ((1UL << (sshift + cshift)) - 1) || vdl != size || vdl_hi) {
        if (first_pass) report(OA_EXCHK_ERROR, "%s: directory size is not whole clusters", where());
        goto out;
    }
    if (depth >= MAXDEPTH) {
        if (first_pass) report(OA_EXCHK_ERROR, "%s: directories nested too deep", where());
        goto out;
    }
    if (!valid(first) || ((sflags & 2) && count > nclu + 2 - first)) goto out;
    memset(&sub, 0, sizeof(sub));
    sub.first = first; sub.size = size; sub.nofat = sflags & 2;
    walk_dir(&sub, depth + 1);
out:
    plen = base; path[plen] = 0;
}

static void walk_dir(struct chain *d, unsigned depth)
{
    const oa_u8 *e;
    oa_u32 pos;
    oa_u8 type;
    for (pos = 0; (e = entry(d, pos)) != 0; pos += 32) {
        type = e[0];
        if (!type) break;
        if (!(type & 0x80)) continue;           /* unused */
        if (type == 0x85) {
            file_set(d, &pos, depth);
        } else if (type == 0x81 || type == 0x82 || type == 0x83) {
            if (!d->root && first_pass) report(OA_EXCHK_ERROR, "%s: system entry %02Xh outside the root directory", where(), type);
            if (type == 0x83 && e[1] > 11 && first_pass) report(OA_EXCHK_ERROR, "Volume label is longer than 11 characters");
        } else if (type >= 0xC0) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: secondary entry without its file entry", where());
        } else if (type < 0xA0) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: unknown critical entry type %02Xh", where(), type);
        } else {
            pos += 32UL * e[1];                 /* benign: skip its secondary entries */
        }
    }
}

/* ---- the allocation bitmap against the clusters in use ---- */

static void range(int lost, oa_u32 a, oa_u32 b)
{
    unsigned *n = lost ? &nlost : &nunmarked;
    if (++*n > MAXRANGE) return;
    if (a == b) report(OA_EXCHK_ERROR, lost ? "Cluster %lu is allocated, but not used (lost)" :
        "Cluster %lu is used, but free in the allocation bitmap", (unsigned long)a);
    else report(OA_EXCHK_ERROR, lost ? "Clusters %lu-%lu are allocated, but not used (lost)" :
        "Clusters %lu-%lu are used, but free in the allocation bitmap", (unsigned long)a, (unsigned long)b);
}

static void compare(void)
{
    oa_u32 i, lba, next, start = 0, cur = NOBUF;
    const oa_u8 *s = 0;
    int bit, inuse, kind = 0, k;              /* kind of the open range: 1 lost, 2 unmarked */
    if (!bitmap.first) return;
    for (i = lo; i <= hi; ++i) {
        k = 0;
        if (i < hi) {
            if ((i >> 3) / ssize != cur) {      /* the bitmap's next sector */
                if ((lba = file_lba(&bitmap, i >> 3)) == NOBUF || !(s = sec(&cdir, lba))) return;
                cur = (i >> 3) / ssize;
            }
            bit = (s[(unsigned)(i >> 3) & (ssize - 1)] >> (i & 7)) & 1;
            inuse = C->mark(C->ctx, i - lo);
            if (bit) ++C->used; else ++C->free;
            if (bit && !inuse) {
                if (!fat_get(i + 2, &next) && next == BADCLU) ++C->bad;
                else { ++C->lost; k = 1; }
            } else if (!bit && inuse) {
                ++C->unmarked; k = 2;
            }
        }
        if (k != kind) {
            if (kind) range(kind == 1, start + 2, i + 1);
            kind = k; start = i;
        }
    }
}

/* ---- the boot region ---- */

static int check_boot(void)
{
    oa_u32 sum = 0, len;
    unsigned s, i, backup = 0, ext = 0, differs = 0;
    if (C->read(C->dev, C->start, boot)) return OA_EXFAT_EIO;
    if (!oa_exfat_probe(boot)) {
        if (C->read(C->dev, C->start + 12, boot) || !oa_exfat_probe(boot)) return OA_EXFAT_EBAD;
        report(OA_EXCHK_ERROR, "Main boot sector is not exFAT; using the backup");
        backup = 12;
    }
    sshift = boot[108]; cshift = boot[109];
    if (sshift < 9 || sshift > 12 || cshift > 25 - sshift) {
        report(OA_EXCHK_ERROR, "Invalid sector or cluster size");
        return OA_EXFAT_EBAD;
    }
    ssize = 1u << sshift;
    len = rd32(boot+72);
    if (rd32(boot+76)) len = 0xFFFFFFFFUL;
    fat = rd32(boot+80); heap = rd32(boot+88); nclu = rd32(boot+92); root = rd32(boot+96);
    C->serial = rd32(boot+100); C->flags = rd16(boot+106);
    C->clusters = nclu; C->cluster_size = 1UL << (sshift + cshift);
    if (boot[105] != 1) report(OA_EXCHK_WARN, "File system revision %u.%02u", boot[105], boot[104]);
    if (boot[110] != 1 && boot[110] != 2) { report(OA_EXCHK_ERROR, "Invalid number of FATs"); return OA_EXFAT_EBAD; }
    if (fat < 24 || rd32(boot+84) < ((nclu + 2) >> (sshift - 2)) || heap < fat + rd32(boot+84) * boot[110] ||
        !nclu || nclu > 0xFFFFFFF5UL || heap + (nclu << cshift) > len || heap + (nclu << cshift) < heap) {
        report(OA_EXCHK_ERROR, "Invalid FAT or cluster heap layout");
        return OA_EXFAT_EBAD;
    }
    if (!valid(root)) { report(OA_EXCHK_ERROR, "Root directory starts outside the volume"); return OA_EXFAT_EBAD; }
    if (boot[110] == 2 && (C->flags & 1)) fat += rd32(boot+84);
    /* the checksum of sectors 0-10 in sector 11, the backup in 12-23 */
    for (s = 0; s < 12; ++s) {
        if (C->read(C->dev, C->start + backup + s, boot)) return OA_EXFAT_EIO;
        if (s < 11) {
            for (i = 0; i < ssize; ++i)
                if (s || (i != 106 && i != 107 && i != 112)) sum = rot32(sum, boot[i]);
            if (s >= 1 && s <= 8 && rd32(boot + ssize - 4) != 0xAA550000UL) ext = 1;
        } else {
            for (i = 0; i < ssize; i += 4) if (rd32(boot + i) != sum) break;
            if (i < ssize) report(OA_EXCHK_ERROR, "Boot region checksum is wrong");
        }
        if (!backup) {
            if (C->read(C->dev, C->start + 12 + s, other)) { differs = 1; continue; }
            for (i = 0; i < ssize; ++i)
                if (boot[i] != other[i] && (s || (i != 106 && i != 107 && i != 112))) { differs = 1; break; }
        }
    }
    if (ext) report(OA_EXCHK_WARN, "Extended boot sectors without signature");
    if (differs) report(OA_EXCHK_ERROR, "Backup boot region differs from the main one");
    if (C->flags & 2) report(OA_EXCHK_WARN, "Volume is marked dirty (not cleanly unmounted)");
    if (C->flags & 4) report(OA_EXCHK_WARN, "Volume is marked as having media failures");
    return OA_EXFAT_OK;
}

int oa_exchk_run(struct oa_exchk *c)
{
    struct chain r;
    oa_u32 v0, v1;
    int rc;
    C = c;
    c->errors = c->warnings = c->used = c->free = c->lost = c->unmarked = c->cross = c->bad = 0;
    c->files = c->dirs = 0;
    cdir.lba = cfat.lba = NOBUF;
    plen = 0; path[0] = 0; nlost = nunmarked = 0; have_upcase = 0;
    first_pass = 1;
    if ((rc = check_boot()) != OA_EXFAT_OK) return rc;
    if (!fat_get(0, &v0) && !fat_get(1, &v1) && (v0 != 0xFFFFFFF8UL || v1 != EOC))
        report(OA_EXCHK_WARN, "FAT entries 0 and 1 are %08lX %08lX", (unsigned long)v0, (unsigned long)v1);
    system_entries();
    if (!c->window) c->window = nclu;
    for (lo = 0; lo < nclu; lo = hi) {
        hi = nclu - lo > c->window ? lo + c->window : nclu;
        c->clear(c->ctx);
        if (bitmap.first) use_chain(bitmap.first, 0, clusters_of(0, bitmap.size));
        if (upcase.first) use_chain(upcase.first, 0, clusters_of(0, upcase.size));
        memset(&r, 0, sizeof(r));
        r.first = root; r.root = 1;
        use_chain(root, 0, 0);
        walk_dir(&r, 0);
        compare();
        first_pass = 0;
    }
    if (nlost > MAXRANGE || nunmarked > MAXRANGE)
        report(OA_EXCHK_INFO, "(only the first %u ranges of each kind are listed)", MAXRANGE);
    return OA_EXFAT_OK;
}
