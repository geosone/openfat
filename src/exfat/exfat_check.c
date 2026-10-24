/* exFAT volume check (EXCHK), after the Microsoft exFAT file system
   specification.  Errors about the structure (boot region, entry sets,
   chains) are reported and repaired in the first pass; errors about single
   clusters (lost, unmarked, cross-linked) in the pass whose window holds
   the cluster.  Repairs are written at once, so the later passes see them.
   Not repaired: cross-linked clusters, invalid characters in names, a
   damaged allocation bitmap or up-case table entry. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "openaspi/exfat_check.h"

#define MAXS     OA_EXCHK_MAXSECT
#define NOBUF    0xFFFFFFFFUL
#define EOC      0xFFFFFFFFUL
#define BADCLU   0xFFFFFFF7UL
#define MAXDEPTH 64
#define MAXPAIRS 4096
#define MAXRANGE 10                     /* listed ranges and cross links */

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
static oa_u16 pairs[2 * MAXPAIRS];      /* upcase table: character, upper case */
static unsigned npairs;
static int have_upcase;
static char path[300];
static unsigned plen;
static char text[400];
static unsigned nlost, nunmarked, ncross;   /* reported */

static oa_u16 rd16(const oa_u8 *p) { return (oa_u16)(p[0] | p[1] << 8); }
static oa_u32 rd32(const oa_u8 *p) { return rd16(p) | (oa_u32)rd16(p+2) << 16; }
static void wr16(oa_u8 *p, oa_u16 v) { p[0] = (oa_u8)v; p[1] = (oa_u8)(v >> 8); }
static void wr32(oa_u8 *p, oa_u32 v) { wr16(p, (oa_u16)v); wr16(p + 2, (oa_u16)(v >> 16)); }

static void report(int level, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsprintf(text, fmt, a);
    va_end(a);
    if (level == OA_EXCHK_ERROR) ++C->errors;
    if (level == OA_EXCHK_WARN) ++C->warnings;
    if (level == OA_EXCHK_FIXED) ++C->fixed;
    C->msg(C->ctx, level, text);
}

static const char *where(void) { return plen ? path : "\\"; }
/* repairs of the structure: with a write function, in the first pass */
static int fixing(void) { return first_pass && C->write; }

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

static int put(struct cache *c)
{
    if (!C->write(C->dev, C->start + c->lba, c->b)) return 0;
    report(OA_EXCHK_ERROR, "Write error at sector %lu", (unsigned long)c->lba);
    c->lba = NOBUF;
    return -1;
}

static int fat_get(oa_u32 clu, oa_u32 *next)
{
    const oa_u8 *s = sec(&cfat, fat + (clu >> (sshift - 2)));
    if (!s) return -1;
    *next = rd32(s + ((unsigned)(clu << 2) & (ssize - 1)));
    return 0;
}

static int fat_set(oa_u32 clu, oa_u32 v)
{
    if (!sec(&cfat, fat + (clu >> (sshift - 2)))) return -1;
    wr32(cfat.b + ((unsigned)(clu << 2) & (ssize - 1)), v);
    return put(&cfat);
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

/* the 32-byte entry at pos of a directory, 0 at its end; changes to it
   are written with put(&cdir) */
static oa_u8 *entry(struct chain *d, oa_u32 pos)
{
    oa_u32 lba = file_lba(d, pos);
    if (lba == NOBUF || !sec(&cdir, lba)) return 0;
    return cdir.b + ((unsigned)pos & (ssize - 1));
}

static void delete_entries(struct chain *d, oa_u32 pos, unsigned n)
{
    oa_u8 *e;
    for (; n; --n, pos += 32) {
        if (!(e = entry(d, pos))) return;
        e[0] &= 0x7F;
        if (put(&cdir)) return;
    }
}

static oa_u16 rot16(oa_u16 sum, oa_u8 b) { return (oa_u16)(((sum & 1) ? 0x8000u : 0) + (sum >> 1) + b); }
static oa_u32 rot32(oa_u32 sum, oa_u8 b) { return ((sum & 1) ? 0x80000000UL : 0) + (sum >> 1) + b; }

static void set_checksum(struct chain *d, oa_u32 pos, unsigned n)
{
    oa_u16 sum = 0;
    unsigned i, k;
    oa_u8 *e;
    for (i = 0; i <= n; ++i) {
        if (!(e = entry(d, pos + 32UL*i))) return;
        for (k = 0; k < 32; ++k) if (i || (k != 2 && k != 3)) sum = rot16(sum, e[k]);
    }
    if (!(e = entry(d, pos))) return;
    wr16(e + 2, sum);
    put(&cdir);
}

/* ---- clusters ---- */

static void use(oa_u32 clu)
{
    if (clu < lo + 2 || clu >= hi + 2) return;
    if (C->mark(C->ctx, clu - 2 - lo)) {
        ++C->cross;
        if (++ncross <= MAXRANGE)
            report(OA_EXCHK_ERROR, "%s: cluster %lu is used twice (cross-linked)", where(), (unsigned long)clu);
    }
}

/* Marks the clusters of a chain; count: clusters by the size, 0 = to the
   end of the chain (root directory).  Returns the clusters found; *broken:
   fewer than count.  A chain that runs on is ended (fix). */
static oa_u32 use_chain(oa_u32 first, int nofat, oa_u32 count, int fix, int *broken)
{
    oa_u32 c = first, n = 0, next, a, b;
    const char *why = 0;
    *broken = 0;
    if (!valid(first)) {
        if (first_pass) report(OA_EXCHK_ERROR, "%s: first cluster %lu is outside the volume", where(), (unsigned long)first);
        *broken = 1;
        return 0;
    }
    if (nofat) {
        if (count > nclu + 2 - first) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: contiguous clusters run past the end of the volume", where());
            count = nclu + 2 - first;
            *broken = 1;
        }
        a = first > lo + 2 ? first : lo + 2;
        b = first + count < hi + 2 ? first + count : hi + 2;
        for (; a < b; ++a) use(a);
        return count;
    }
    for (;;) {
        use(c); ++n;
        if (fat_get(c, &next)) return n;
        if (next == EOC) break;
        if (count && n >= count) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: cluster chain is longer than the size", where());
            if (fix && fixing() && !fat_set(c, EOC)) report(OA_EXCHK_FIXED, "chain ended after %lu clusters", (unsigned long)n);
            return n;
        }
        if (n >= nclu) why = "cluster chain does not end";
        else if (next == BADCLU) why = "cluster chain runs into a bad cluster";
        else if (!valid(next)) why = "invalid FAT entry in the cluster chain";
        if (why) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: %s", where(), why);
            if (fix && fixing() && !fat_set(c, EOC)) report(OA_EXCHK_FIXED, "chain ended after %lu clusters", (unsigned long)n);
            *broken = 1;
            return n;
        }
        c = next;
    }
    if (count && n < count) {
        if (first_pass) report(OA_EXCHK_ERROR, "%s: cluster chain has %lu of %lu clusters", where(), (unsigned long)n, (unsigned long)count);
        *broken = 1;
    }
    return n;
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

/* ---- the root directory's system entries ---- */

static void load_upcase(oa_u32 want)
{
    oa_u32 off, sum = 0, lba, idx = 0;
    const oa_u8 *s;
    oa_u16 v, prev = 0;
    int run = 0, half = 0;
    unsigned i;
    npairs = 0;
    for (off = 0; off < upcase.size; off += ssize) {
        if ((lba = file_lba(&upcase, off)) == NOBUF || !(s = sec(&cdir, lba))) return;
        for (i = 0; i < ssize && off + i < upcase.size; ++i) {
            sum = rot32(sum, s[i]);
            if (!half) { prev = s[i]; half = 1; continue; }
            half = 0;
            v = (oa_u16)(prev | s[i] << 8);
            if (run) { idx += v; run = 0; continue; }
            if (v == 0xFFFF && idx < 0xFFFF) { run = 1; continue; }
            if (v != idx && npairs < MAXPAIRS) {
                pairs[2*npairs] = (oa_u16)idx; pairs[2*npairs+1] = v; ++npairs;
            }
            ++idx;
        }
    }
    if (sum != want) report(OA_EXCHK_ERROR, "Up-case table checksum is wrong%s", C->write ? " (not repaired)" : "");
    else if (npairs >= MAXPAIRS) report(OA_EXCHK_WARN, "Up-case table too large: name hashes not checked");
    else have_upcase = 1;
}

static void system_entries(void)
{
    struct chain r;
    const oa_u8 *e;
    oa_u32 pos, upsum = 0;
    unsigned nbitmap = 0, nupcase = 0;
    const char *no = C->write ? " (not repaired)" : "";
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
    if (nbitmap != 1) report(OA_EXCHK_ERROR, "%u allocation bitmaps in the root directory%s", nbitmap, no);
    if (nupcase != 1) report(OA_EXCHK_ERROR, "%u up-case tables in the root directory%s", nupcase, no);
    if (nbitmap && bitmap.size < (nclu + 7) / 8) {
        report(OA_EXCHK_ERROR, "Allocation bitmap is too small%s", no);
        bitmap.first = 0;
    }
    if (nbitmap && !valid(bitmap.first)) {
        report(OA_EXCHK_ERROR, "Allocation bitmap starts outside the volume%s", no);
        bitmap.first = 0;
    }
    if (nupcase && valid(upcase.first) && upcase.size && upcase.size <= 0x20000UL) load_upcase(upsum);
    else if (nupcase) report(OA_EXCHK_ERROR, "Up-case table is invalid%s", no);
}

/* ---- directories ---- */

static void walk_dir(struct chain *d, unsigned depth);

/* The entry set at *pos: checks and repairs it, marks its clusters, enters
   a directory.  *pos is left at its last entry. */
static void file_set(struct chain *d, oa_u32 *pos, unsigned depth)
{
    oa_u32 at = *pos;
    oa_u8 *e = entry(d, at), st[32];
    unsigned n = e[1], i, k, attr = rd16(e+4), namelen = 0, got = 0, bad = 0, base = plen;
    unsigned shift = sshift + cshift;
    oa_u16 want = rd16(e+2), sum = 0, h = 0, c;
    oa_u32 first, size, size_hi, vdl, vdl_hi, count, good;
    int broken, changed = 0, fp = first_pass, fix = fixing();
    struct chain sub;
    if (n < 2 || n > 18) {
        if (fp) report(OA_EXCHK_ERROR, "%s: file entry with %u secondary entries", where(), n);
        if (fix) { delete_entries(d, at, 1); report(OA_EXCHK_FIXED, "file entry deleted"); }
        return;
    }
    for (i = 0; i < 32; ++i) if (i != 2 && i != 3) sum = rot16(sum, e[i]);
    if (plen < sizeof(path) - 2) { path[plen++] = '\\'; path[plen] = 0; }
    for (i = 1; i <= n; ++i) {
        if (!(e = entry(d, at + 32UL*i))) {
            if (fp) report(OA_EXCHK_ERROR, "%s: entry set is cut off", where());
            if (fix) { delete_entries(d, at, i); report(OA_EXCHK_FIXED, "entry set deleted"); }
            goto out;
        }
        for (k = 0; k < 32; ++k) sum = rot16(sum, e[k]);
        if (i == 1) {
            if (e[0] != 0xC0) {
                if (fp) report(OA_EXCHK_ERROR, "%s: file entry without stream entry", where());
                if (fix) { delete_entries(d, at, 1); report(OA_EXCHK_FIXED, "file entry deleted"); }
                goto out;
            }
            memcpy(st, e, 32);
            namelen = e[3];
        } else if (e[0] == 0xC1 && i <= 1 + (namelen + 14) / 15) {
            for (k = 0; k < 15 && got < namelen; ++k, ++got) {
                c = rd16(e + 2 + 2*k);
                if (c < 0x20 || (c < 0x80 && strchr("\"*/:<>?\\|", (char)c))) bad = 1;
                h = rot16(h, (oa_u8)up(c)); h = rot16(h, (oa_u8)(up(c) >> 8));
                if (plen < sizeof(path) - 1) { path[plen++] = oem(c); path[plen] = 0; }
            }
        } else if (e[0] < 0xE0) {
            /* another set begins: the next walk starts with it */
            if (fp) report(OA_EXCHK_ERROR, "%s: entry set broken by entry type %02Xh", where(), e[0]);
            if (fix) { delete_entries(d, at, i); report(OA_EXCHK_FIXED, "entry set deleted"); }
            *pos = at + 32UL * (i - 1);
            goto out;
        }
    }
    *pos = at + 32UL * n;
    if (!namelen || got != namelen) {
        if (fp) report(OA_EXCHK_ERROR, "%s: name length %u, %u characters", where(), namelen, got);
        if (fix) { delete_entries(d, at, n + 1); report(OA_EXCHK_FIXED, "entry set deleted"); }
        goto out;
    }
    if (sum != want) {
        if (fp) report(OA_EXCHK_ERROR, "%s: entry set checksum is wrong", where());
        if (!fix) goto out;
        report(OA_EXCHK_FIXED, "checksum corrected");
        changed = 1;
    }
    vdl = rd32(st+8); vdl_hi = rd32(st+12);
    first = rd32(st+20); size = rd32(st+24); size_hi = rd32(st+28);
    if (fp) {
        if (bad) report(OA_EXCHK_ERROR, "%s: invalid character in the name%s", where(), fix ? " (not repaired)" : "");
        if (have_upcase && h != rd16(st+4)) {
            report(OA_EXCHK_ERROR, "%s: name hash is wrong", where());
            if (fix) { wr16(st+4, h); changed = 1; report(OA_EXCHK_FIXED, "name hash corrected"); }
        }
        if (vdl_hi > size_hi || (vdl_hi == size_hi && vdl > size)) {
            report(OA_EXCHK_ERROR, "%s: valid data length is larger than the size", where());
            if (fix) { vdl = size; vdl_hi = size_hi; changed = 1; report(OA_EXCHK_FIXED, "valid data length set to the size"); }
        }
        if (attr & OA_EXFAT_DIR) ++C->dirs; else ++C->files;
    }
    if (!(st[1] & 1)) {                         /* no allocation possible */
        if (!first && !size && !size_hi) goto out;
        if (fp) report(OA_EXCHK_ERROR, "%s: clusters without allocation", where());
        if (!fix) goto out;
        st[1] |= 1; changed = 1;
        report(OA_EXCHK_FIXED, "allocation flag set");
    }
    if (!size && !size_hi) {
        if (first && fp) report(OA_EXCHK_WARN, "%s: empty, but with first cluster %lu", where(), (unsigned long)first);
        if (first && fix) { first = 0; changed = 1; report(OA_EXCHK_FIXED, "first cluster cleared"); }
        goto out;
    }
    count = clusters_of(size_hi, size);
    if (count == 0xFFFFFFFFUL || count > nclu) {
        if (fp) report(OA_EXCHK_ERROR, "%s: size is larger than the volume", where());
        if (fix) { first = size = size_hi = vdl = vdl_hi = 0; changed = 1; report(OA_EXCHK_FIXED, "size set to 0"); }
        goto out;
    }
    good = use_chain(first, st[1] & 2, count, 1, &broken);
    if (broken && fix) {                        /* the size to the clusters there are */
        size = good << shift; size_hi = shift ? good >> (32 - shift) : 0;
        if (!good) first = 0;
        if (vdl_hi > size_hi || (vdl_hi == size_hi && vdl > size)) { vdl = size; vdl_hi = size_hi; }
        changed = 1;
        report(OA_EXCHK_FIXED, "size set to %lu clusters", (unsigned long)good);
        count = good;
    }
    if (!(attr & OA_EXFAT_DIR) || !count) goto out;
    if (size_hi || size & ((1UL << shift) - 1) || vdl != size || vdl_hi) {
        if (fp) report(OA_EXCHK_ERROR, "%s: directory size is not whole clusters", where());
        if (!fix) goto out;
        size = count << shift; size_hi = 0; vdl = size; vdl_hi = 0; changed = 1;
        report(OA_EXCHK_FIXED, "directory size set to %lu clusters", (unsigned long)count);
    }
    if (depth >= MAXDEPTH) {
        if (fp) report(OA_EXCHK_ERROR, "%s: directories nested too deep", where());
        goto out;
    }
    if (!valid(first) || ((st[1] & 2) && count > nclu + 2 - first)) goto out;
    memset(&sub, 0, sizeof(sub));
    sub.first = first; sub.size = size; sub.nofat = st[1] & 2;
    walk_dir(&sub, depth + 1);
out:
    if (changed) {                              /* the stream entry and the checksum */
        wr32(st+8, vdl); wr32(st+12, vdl_hi);
        wr32(st+20, first); wr32(st+24, size); wr32(st+28, size_hi);
        if ((e = entry(d, at + 32)) != 0) {
            memcpy(e, st, 32);
            if (!put(&cdir)) set_checksum(d, at, n);
        }
    }
    plen = base; path[plen] = 0;
}

static void walk_dir(struct chain *d, unsigned depth)
{
    oa_u8 *e;
    oa_u32 pos;
    oa_u8 type;
    int fix = fixing();
    for (pos = 0; (e = entry(d, pos)) != 0; pos += 32) {
        type = e[0];
        if (!type) break;
        if (!(type & 0x80)) continue;           /* unused */
        if (type == 0x85) {
            file_set(d, &pos, depth);
        } else if (type == 0x81 || type == 0x82 || type == 0x83) {
            if (!d->root) {
                if (first_pass) report(OA_EXCHK_ERROR, "%s: system entry %02Xh outside the root directory", where(), type);
                if (fix) { delete_entries(d, pos, 1); report(OA_EXCHK_FIXED, "entry deleted"); }
            } else if (type == 0x83 && e[1] > 11) {
                if (first_pass) report(OA_EXCHK_ERROR, "Volume label is longer than 11 characters");
                if (fix) { e[1] = 11; if (!put(&cdir)) report(OA_EXCHK_FIXED, "label cut to 11 characters"); }
            }
        } else if (type >= 0xC0) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: secondary entry without its file entry", where());
            if (fix) { delete_entries(d, pos, 1); report(OA_EXCHK_FIXED, "entry deleted"); }
        } else if (type < 0xA0) {
            if (first_pass) report(OA_EXCHK_ERROR, "%s: unknown critical entry type %02Xh", where(), type);
            if (fix) { delete_entries(d, pos, 1); report(OA_EXCHK_FIXED, "entry deleted"); }
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
    oa_u8 *s = 0, *p, m;
    int bit, inuse, kind = 0, k, dirty = 0;     /* kind of the open range: 1 lost, 2 unmarked */
    if (!bitmap.first) return;
    for (i = lo; i <= hi; ++i) {
        k = 0;
        if (i < hi) {
            if ((i >> 3) / ssize != cur) {      /* the bitmap's next sector */
                if (dirty && put(&cdir)) return;
                dirty = 0;
                if ((lba = file_lba(&bitmap, i >> 3)) == NOBUF || !sec(&cdir, lba)) return;
                s = cdir.b;
                cur = (i >> 3) / ssize;
            }
            p = s + ((unsigned)(i >> 3) & (ssize - 1));
            m = (oa_u8)(1 << (unsigned)(i & 7));
            bit = (*p & m) != 0;
            inuse = C->mark(C->ctx, i - lo);
            if (bit && !inuse) {
                if (!fat_get(i + 2, &next) && next == BADCLU) ++C->bad;
                else {
                    ++C->lost; k = 1;
                    if (C->write) { *p &= (oa_u8)~m; dirty = 1; bit = 0; }
                }
            } else if (!bit && inuse) {
                ++C->unmarked; k = 2;
                if (C->write) { *p |= m; dirty = 1; bit = 1; }
            }
            if (bit) ++C->used; else ++C->free;
        }
        if (k != kind) {
            if (kind) range(kind == 1, start + 2, i + 1);
            kind = k; start = i;
        }
    }
    if (dirty) put(&cdir);
}

/* ---- the boot region ---- */

static oa_u32 region_sum;

/* the checksum of sectors base..base+10 against sector base+11: 1 right,
   0 wrong, -1 read error; *ext: an extended boot sector without signature */
static int region_ok(oa_u32 base, int *ext)
{
    unsigned s, i;
    oa_u32 sum = 0;
    for (s = 0; s < 12; ++s) {
        if (C->read(C->dev, C->start + base + s, other)) return -1;
        if (s < 11) {
            for (i = 0; i < ssize; ++i)
                if (s || (i != 106 && i != 107 && i != 112)) sum = rot32(sum, other[i]);
            if (s >= 1 && s <= 8 && rd32(other + ssize - 4) != 0xAA550000UL) *ext = 1;
        } else {
            region_sum = sum;
            for (i = 0; i < ssize; i += 4) if (rd32(other + i) != sum) return 0;
        }
    }
    return 1;
}

static int copy_region(oa_u32 from, oa_u32 to)
{
    unsigned s;
    for (s = 0; s < 12; ++s)
        if (C->read(C->dev, C->start + from + s, other) || C->write(C->dev, C->start + to + s, other)) {
            report(OA_EXCHK_ERROR, "Read or write error in the boot region");
            return -1;
        }
    return 0;
}

static int check_boot(void)
{
    oa_u32 len;
    unsigned s, i;
    int main_ok, backup_ok, ext = 0, dummy = 0, differs = 0, fix = C->write != 0;
    if (C->read(C->dev, C->start, boot)) return OA_EXFAT_EIO;
    main_ok = oa_exfat_probe(boot);
    if (C->read(C->dev, C->start + 12, other)) backup_ok = 0;
    else backup_ok = oa_exfat_probe(other);
    if (!main_ok) {
        if (!backup_ok) return OA_EXFAT_EBAD;
        memcpy(boot, other, 512);
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
    if (backup_ok) backup_ok = region_ok(12, &dummy) == 1;
    if (!main_ok) {
        report(OA_EXCHK_ERROR, "Main boot sector is not exFAT; using the backup");
        if (fix && !copy_region(12, 0)) report(OA_EXCHK_FIXED, "main boot region written from the backup");
    } else if ((main_ok = region_ok(0, &ext)) < 0) {
        return OA_EXFAT_EIO;
    } else if (!main_ok) {
        report(OA_EXCHK_ERROR, "Boot region checksum is wrong");
        if (fix && backup_ok) {
            if (!copy_region(12, 0)) report(OA_EXCHK_FIXED, "main boot region written from the backup");
        } else if (fix) {
            for (i = 0; i < ssize; i += 4) wr32(other + i, region_sum);
            if (!C->write(C->dev, C->start + 11, other) && !copy_region(0, 12))
                report(OA_EXCHK_FIXED, "checksum written, backup boot region written from the main one");
        }
    } else {
        for (s = 0; s < 12 && !differs; ++s) {
            if (C->read(C->dev, C->start + s, boot) || C->read(C->dev, C->start + 12 + s, other)) { differs = 1; break; }
            for (i = 0; i < ssize; ++i)
                if (boot[i] != other[i] && (s || (i != 106 && i != 107 && i != 112))) { differs = 1; break; }
        }
        if (differs) {
            report(OA_EXCHK_ERROR, "Backup boot region differs from the main one");
            if (fix && !copy_region(0, 12)) report(OA_EXCHK_FIXED, "backup boot region written from the main one");
        }
    }
    if (ext) report(OA_EXCHK_WARN, "Extended boot sectors without signature");
    if (C->flags & 2) report(OA_EXCHK_WARN, "Volume is marked dirty (not cleanly unmounted)");
    if (C->flags & 4) report(OA_EXCHK_WARN, "Volume is marked as having media failures");
    return OA_EXFAT_OK;
}

int oa_exchk_run(struct oa_exchk *c)
{
    struct chain r;
    oa_u32 v0, v1;
    int rc, broken;
    C = c;
    c->errors = c->warnings = c->fixed = c->used = c->free = c->lost = c->unmarked = c->cross = c->bad = 0;
    c->files = c->dirs = 0;
    cdir.lba = cfat.lba = NOBUF;
    plen = 0; path[0] = 0; nlost = nunmarked = ncross = 0; have_upcase = 0;
    first_pass = 1;
    if ((rc = check_boot()) != OA_EXFAT_OK) return rc;
    if (!fat_get(0, &v0) && !fat_get(1, &v1) && (v0 != 0xFFFFFFF8UL || v1 != EOC)) {
        report(OA_EXCHK_WARN, "FAT entries 0 and 1 are %08lX %08lX", (unsigned long)v0, (unsigned long)v1);
        if (c->write && !fat_set(0, 0xFFFFFFF8UL) && !fat_set(1, EOC)) report(OA_EXCHK_FIXED, "FAT entries 0 and 1 corrected");
    }
    system_entries();
    if (!c->window) c->window = nclu;
    for (lo = 0; lo < nclu; lo = hi) {
        hi = nclu - lo > c->window ? lo + c->window : nclu;
        c->clear(c->ctx);
        if (bitmap.first) use_chain(bitmap.first, 0, clusters_of(0, bitmap.size), 0, &broken);
        if (upcase.first) use_chain(upcase.first, 0, clusters_of(0, upcase.size), 0, &broken);
        memset(&r, 0, sizeof(r));
        r.first = root; r.root = 1;
        use_chain(root, 0, 0, 1, &broken);
        walk_dir(&r, 0);
        compare();
        first_pass = 0;
    }
    if (nlost > MAXRANGE || nunmarked > MAXRANGE || ncross > MAXRANGE)
        report(OA_EXCHK_INFO, "(only the first %u ranges and cross links are listed)", MAXRANGE);
    if (c->write && (c->lost || c->unmarked))
        report(OA_EXCHK_FIXED, "allocation bitmap corrected: %lu lost clusters freed, %lu clusters marked as used",
               (unsigned long)c->lost, (unsigned long)c->unmarked);
    if (c->write && c->cross)
        report(OA_EXCHK_INFO, "Cross-linked clusters are not repaired: copy the files named and delete the originals");
    /* a repaired volume is clean: VolumeDirty off, PercentInUse new; both
       are outside the boot checksum and are kept in the main region only */
    if (c->write && (c->fixed || (c->flags & 2)) && !c->read(c->dev, c->start, boot)) {
        boot[106] &= (oa_u8)~2;
        boot[112] = (oa_u8)(nclu ? c->used * 100 / nclu : 0);
        if (!c->write(c->dev, c->start, boot) && (c->flags & 2)) report(OA_EXCHK_FIXED, "volume no longer marked dirty");
    }
    return OA_EXFAT_OK;
}
