/* FAT12/16/32 defragmentation (DEFRAG); the order of the writes is
   described in include/openaspi/fat_defrag.h.
   The files and directories are read into a list (16 bytes each: first
   cluster, clusters, the parent directory and the slot of the entry in
   it), the clusters in use into a bit map.  FILES: every fragmented file
   goes to the lowest free run that holds it.  FULL: the same first, then
   the directories and after them the files, in the order of the directory
   tree, are placed one after the other from cluster 2 on; a file in the way
   goes to the highest free run first.  Clusters of no movable file (bad
   clusters, the FAT32 root directory, files still fragmented) stay where
   they are. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "openaspi/fat_defrag.h"

#ifdef __WATCOMC__
#define FMEMSET  _fmemset
#define FMEMMOVE _fmemmove
#else
#define FMEMSET  memset
#define FMEMMOVE memmove
#endif
#define FAR OA_DEFRAG_FAR

#define MAXS     OA_DEFRAG_MAXSECT
#define NOBUF    0xFFFFFFFFUL
#define EOC      0xFFFFFFFFUL           /* normalized FAT values */
#define BADCLU   0xFFFFFFF7UL
#define NONE     0xFFFFU                /* no file; the parent of the root's entries */
#define MAXDEPTH 64
#define F_DIR    1
#define F_FRAG   2

struct item { oa_u32 first, count; oa_u16 parent, slot; oa_u8 flags, pad[3]; };
/* a FAT sector (lba from the start of the FAT), written to all FATs */
struct fsec { oa_u32 lba; int dirty; oa_u8 b[MAXS]; };
/* a directory: the fixed FAT12/16 root, or a cluster chain */
struct dir { oa_u32 first, idx, clu; int fixed; };

static struct oa_defrag *D;
static struct fsec fc[2];
static unsigned flast;                  /* fc[flast] was used last */
static oa_u32 dlba;
static oa_u8 dsec[MAXS];                /* a directory sector */
static oa_u8 boot[MAXS];
static unsigned bits, nfats, active, ssize, sshift, cshift;
static int mirror;
static oa_u32 fat0, fatlen, rootsec, rootsecs, heap, nclu, rootclu, head1, bmsize;
static unsigned nitems;
static char text[120];

static oa_u16 rd16(const oa_u8 *p) { return (oa_u16)(p[0] | p[1] << 8); }
static oa_u32 rd32(const oa_u8 *p) { return rd16(p) | (oa_u32)rd16(p+2) << 16; }
static void wr16(oa_u8 *p, oa_u16 v) { p[0] = (oa_u8)v; p[1] = (oa_u8)(v >> 8); }
static void wr32(oa_u8 *p, oa_u32 v) { wr16(p, (oa_u16)v); wr16(p + 2, (oa_u16)(v >> 16)); }

static void say(const char *fmt, ...)
{
    va_list a;
    if (!D->status) return;
    va_start(a, fmt);
    vsprintf(text, fmt, a);
    va_end(a);
    D->status(D->ctx, text);
}

static void show(oa_u32 clu, oa_u32 count, int state) { if (D->map && count) D->map(D->ctx, clu, count, state); }
static int stopped(void) { return D->stop && D->stop(D->ctx); }

/* ---- work memory: the bit map, then the list ---- */

static oa_u8 FAR *at(oa_u32 off) { return (oa_u8 FAR *)D->mem(D->ctx, off); }
static struct item FAR *item(unsigned i) { return (struct item FAR *)at(bmsize + (oa_u32)i * 16); }

static int used(oa_u32 c) { return *at(c >> 3) >> (unsigned)(c & 7) & 1; }

int oa_defrag_used(oa_u32 clu) { return clu >= 2 && clu < nclu + 2 && used(clu); }

static void set_used(oa_u32 c, oa_u32 n, int on)
{
    oa_u8 FAR *p;
    for (; n; --n, ++c) {
        p = at(c >> 3);
        if (on) *p |= (oa_u8)(1 << (unsigned)(c & 7));
        else *p &= (oa_u8)~(1 << (unsigned)(c & 7));
    }
}

/* the first cluster from c on (below end) whose bit is not state */
static oa_u32 skip(oa_u32 c, oa_u32 end, int state)
{
    oa_u8 all = (oa_u8)(state ? 0xFF : 0);
    oa_u8 FAR *p;
    oa_u32 left;
    while (c < end) {
        if (!(c & 7) && end - c >= 8) {
            p = at(c >> 3);
            left = D->block - ((c >> 3) & (D->block - 1));
            if (*p == all) {
                do { ++p; c += 8; } while (--left && end - c >= 8 && *p == all);
                continue;
            }
        }
        if (used(c) != state) return c;
        ++c;
    }
    return end;
}

/* the lowest run of n free clusters from c on; 0: none */
static oa_u32 first_fit(oa_u32 n, oa_u32 c)
{
    oa_u32 end = nclu + 2, b;
    for (;; c = b) {
        if ((c = skip(c, end, 1)) >= end) return 0;
        b = skip(c, end, 0);
        if (b - c >= n) return c;
    }
}

/* n free clusters at the end of the highest free run from c on that holds them; 0: none */
static oa_u32 last_fit(oa_u32 n, oa_u32 c)
{
    oa_u32 end = nclu + 2, b, t = 0;
    for (;; c = b) {
        if ((c = skip(c, end, 1)) >= end) return t;
        b = skip(c, end, 0);
        if (b - c >= n) t = b - n;
    }
}

/* the first position k of the sorted list (n entries) whose file starts at
   first or later */
static unsigned pos_of(oa_u32 first, unsigned n)
{
    unsigned lo = 0, mid;
    while (lo < n) {
        mid = lo + (n - lo) / 2;
        if (item(D->order[mid])->first < first) lo = mid + 1; else n = mid;
    }
    return lo;
}

/* the movable file that holds cluster c, NONE */
static oa_u16 owner(oa_u32 c)
{
    unsigned k = pos_of(c + 1, nitems);
    struct item FAR *it;
    if (!k) return NONE;
    it = item(D->order[k - 1]);
    if (it->flags & F_FRAG || c >= it->first + it->count) return NONE;
    return D->order[k - 1];
}

/* file i starts at first now: its place in the sorted list */
static void resort(oa_u16 i, oa_u32 first)
{
    unsigned k = pos_of(item(i)->first, nitems);
    FMEMMOVE(D->order + k, D->order + k + 1, (nitems - k - 1) * sizeof(oa_u16));
    item(i)->first = first;
    k = pos_of(first, nitems - 1);
    FMEMMOVE(D->order + k + 1, D->order + k, (nitems - 1 - k) * sizeof(oa_u16));
    D->order[k] = i;
}

static void sort(void)
{
    unsigned gap, i, j;
    oa_u16 v;
    oa_u32 f;
    for (i = 0; i < nitems; ++i) D->order[i] = (oa_u16)i;
    for (gap = nitems / 2; gap; gap /= 2)
        for (i = gap; i < nitems; ++i) {
            v = D->order[i]; f = item(v)->first;
            for (j = i; j >= gap && item(D->order[j - gap])->first > f; j -= gap) D->order[j] = D->order[j - gap];
            D->order[j] = v;
        }
}

/* ---- the FAT ---- */

static int valid(oa_u32 clu) { return clu >= 2 && clu < nclu + 2; }

static int fat_out(struct fsec *f)
{
    unsigned i;
    if (!f->dirty) return 0;
    for (i = 0; i < nfats; ++i)
        if ((mirror || i == active) && D->write(D->dev, D->start + fat0 + i * fatlen + f->lba, 1, f->b)) return -1;
    f->dirty = 0;
    return 0;
}

static int fat_flush(void) { return fat_out(&fc[0]) || fat_out(&fc[1]) ? -1 : 0; }

/* the byte at off of the FAT; fc[flast] holds it */
static oa_u8 *fat_byte(oa_u32 off)
{
    oa_u32 s = off >> sshift;
    struct fsec *f;
    if (fc[flast].lba != s) {
        flast = !flast;
        f = &fc[flast];
        if (f->lba != s) {
            if (fat_out(f)) return 0;
            f->lba = NOBUF;
            if (D->read(D->dev, D->start + fat0 + active * fatlen + s, 1, f->b)) return 0;
            f->lba = s;
        }
    }
    return fc[flast].b + ((unsigned)off & (ssize - 1));
}

static int fat_get(oa_u32 clu, oa_u32 *v)
{
    oa_u8 *p;
    oa_u32 x;
    unsigned b0;
    if (bits == 12) {
        if (!(p = fat_byte(clu + clu / 2))) return -1;
        b0 = *p;
        if (!(p = fat_byte(clu + clu / 2 + 1))) return -1;
        x = b0 | (oa_u32)*p << 8;
        x = clu & 1 ? x >> 4 : x & 0xFFF;
        *v = x >= 0xFF8 ? EOC : x == 0xFF7 ? BADCLU : x;
    } else if (bits == 16) {
        if (!(p = fat_byte(clu * 2))) return -1;
        x = rd16(p);
        *v = x >= 0xFFF8UL ? EOC : x == 0xFFF7UL ? BADCLU : x;
    } else {
        if (!(p = fat_byte(clu * 4))) return -1;
        x = rd32(p) & 0x0FFFFFFFUL;
        *v = x >= 0x0FFFFFF8UL ? EOC : x == 0x0FFFFFF7UL ? BADCLU : x;
    }
    return 0;
}

/* v: a cluster, 0 or EOC */
static int fat_put(oa_u32 clu, oa_u32 v)
{
    oa_u8 *p;
    oa_u32 off;
    if (bits == 12) {
        v = v == EOC ? 0xFFF : v & 0xFFF;
        off = clu + clu / 2;
        if (!(p = fat_byte(off))) return -1;
        *p = clu & 1 ? (oa_u8)((*p & 0x0F) | (v << 4 & 0xF0)) : (oa_u8)v;
        fc[flast].dirty = 1;
        if (!(p = fat_byte(off + 1))) return -1;
        *p = clu & 1 ? (oa_u8)(v >> 4) : (oa_u8)((*p & 0xF0) | (v >> 8 & 0x0F));
    } else if (bits == 16) {
        if (!(p = fat_byte(clu * 2))) return -1;
        wr16(p, (oa_u16)(v == EOC ? 0xFFFF : v));
    } else {
        if (!(p = fat_byte(clu * 4))) return -1;
        wr32(p, (rd32(p) & 0xF0000000UL) | (v == EOC ? 0x0FFFFFFFUL : v));
    }
    fc[flast].dirty = 1;
    return 0;
}

/* FAT entry 1: the clean bit off while DEFRAG runs, as found after */
static int set_head(int dirty)
{
    oa_u8 *p;
    if (bits == 12) return 0;
    if (!(p = fat_byte(bits / 8))) return -1;
    if (bits == 16) wr16(p, (oa_u16)(dirty ? head1 & ~0x8000UL : head1));
    else wr32(p, (rd32(p) & 0xF0000000UL) | (dirty ? head1 & ~0x08000000UL : head1));
    fc[flast].dirty = 1;
    return fat_flush();
}

/* clusters of the chain at first and whether it is fragmented */
static int chain(oa_u32 first, oa_u32 *count, int *frag)
{
    oa_u32 c = first, v, n = 0;
    *frag = 0;
    for (;;) {
        if (!valid(c) || n >= nclu) return OA_DEFRAG_EDIR;
        ++n;
        if (fat_get(c, &v)) return OA_DEFRAG_EIO;
        if (v == EOC) break;
        if (v != c + 1) *frag = 1;
        c = v;
    }
    *count = n;
    return 0;
}

/* ---- directories ---- */

static void dir_of(struct dir *d, oa_u16 parent)
{
    memset(d, 0, sizeof(*d));
    if (parent != NONE) d->first = item(parent)->first;
    else if (bits == 32) d->first = rootclu;
    else d->fixed = 1;
}

/* the sector of byte pos of a directory, NOBUF past its end */
static oa_u32 dir_lba(struct dir *d, oa_u32 pos)
{
    oa_u32 idx = pos >> (sshift + cshift), next;
    if (d->fixed) return pos >> sshift < rootsecs ? rootsec + (pos >> sshift) : NOBUF;
    if (idx < d->idx || !d->clu) { d->idx = 0; d->clu = d->first; }
    while (d->idx < idx) {
        if (!valid(d->clu) || fat_get(d->clu, &next) || d->idx >= nclu) return NOBUF;
        d->clu = next; ++d->idx;
    }
    if (!valid(d->clu)) return NOBUF;
    return heap + ((d->clu - 2) << cshift) + ((pos >> sshift) & ((1UL << cshift) - 1));
}

static oa_u8 *dread(oa_u32 lba)
{
    if (dlba != lba) {
        dlba = NOBUF;
        if (D->read(D->dev, D->start + lba, 1, dsec)) return 0;
        dlba = lba;
    }
    return dsec;
}

static oa_u32 first_of(const oa_u8 *e) { return rd16(e+26) | (bits == 32 ? (oa_u32)rd16(e+20) << 16 : 0); }

static void set_first(oa_u8 FAR *e, oa_u32 v)
{
    e[26] = (oa_u8)v; e[27] = (oa_u8)(v >> 8);
    if (bits == 32) { e[20] = (oa_u8)(v >> 16); e[21] = (oa_u8)(v >> 24); }
}

static int is_name(const oa_u8 FAR *e, const char *name)
{
    unsigned i;
    for (i = 0; i < 11 && e[i] == (oa_u8)name[i]; ++i) ;
    return i == 11;
}

/* the files and subdirectories of a directory, then those of the subdirectories */
static int walk(oa_u16 parent, unsigned depth)
{
    struct dir d;
    struct item FAR *it;
    oa_u32 pos, lba, first, count;
    oa_u8 *e;
    unsigned from = nitems, to, i;
    int rc, frag, isdir;
    if (depth > MAXDEPTH) return OA_DEFRAG_EDIR;
    dir_of(&d, parent);
    for (pos = 0; (lba = dir_lba(&d, pos)) != NOBUF; pos += 32) {
        if (!(e = dread(lba))) return OA_DEFRAG_EIO;
        e += (unsigned)pos & (ssize - 1);
        if (!e[0]) break;
        if (e[0] == 0xE5 || e[11] == 0x0F || e[11] & 0x08 || e[0] == '.' || !(first = first_of(e))) continue;
        isdir = e[11] & 0x10;
        if (pos >> 5 > 0xFFFFUL) return OA_DEFRAG_EDIR;
        if (nitems >= D->maxitems || nitems >= NONE || bmsize + (nitems + 1UL) * 16 > D->memsize) return OA_DEFRAG_EMEM;
        if ((rc = chain(first, &count, &frag)) != 0) return rc;
        it = item(nitems++);
        it->first = first; it->count = count; it->parent = parent; it->slot = (oa_u16)(pos >> 5);
        it->flags = (oa_u8)((isdir ? F_DIR : 0) | (frag ? F_FRAG : 0));
        if (isdir) ++D->dirs; else ++D->files;
        if (frag) ++D->fragmented;
    }
    for (to = nitems, i = from; i < to; ++i)
        if (item(i)->flags & F_DIR && (rc = walk((oa_u16)i, depth + 1)) != 0) return rc;
    return 0;
}

/* ---- moving a file ---- */

static int move(oa_u16 i, oa_u32 t)
{
    struct item FAR *it = item(i), FAR *s;
    struct dir d;
    oa_u32 first = it->first, n = it->count, c, v, run, done, sectors, src, dst, k, lba;
    unsigned per = D->bufsize >> sshift, cnt;
    oa_u8 FAR *b = (oa_u8 FAR *)D->buf;
    oa_u8 *e;
    int isdir = it->flags & F_DIR;
    dlba = NOBUF;                       /* the old clusters may be written over */
    /* 1. the data, run by run of the old chain */
    for (c = first, done = 0; done < n; done += run, c = v) {
        if (!valid(c)) return OA_DEFRAG_EDIR;
        for (run = 1;; ++run) {
            if (fat_get(c + run - 1, &v)) return OA_DEFRAG_EIO;
            if (done + run >= n || v != c + run) break;
        }
        sectors = run << cshift;
        src = heap + ((c - 2) << cshift);
        dst = heap + ((t + done - 2) << cshift);
        show(c, run, OA_DEFRAG_READ);
        for (k = 0; k < sectors; k += cnt) {
            cnt = sectors - k > per ? per : (unsigned)(sectors - k);
            if (D->read(D->dev, D->start + src + k, cnt, b)) return OA_DEFRAG_EIO;
            if (isdir && !done && !k) {         /* the "." entry */
                if (!is_name(b, ".          ")) return OA_DEFRAG_EDIR;
                set_first(b, t);
            }
            if (D->write(D->dev, D->start + dst + k, cnt, b)) return OA_DEFRAG_EIO;
        }
        show(t + done, run, OA_DEFRAG_WRITE);
    }
    /* 2. the new chain, in all FATs */
    for (k = 0; k < n; ++k)
        if (fat_put(t + k, k + 1 < n ? t + k + 1 : EOC)) return OA_DEFRAG_EIO;
    if (fat_flush()) return OA_DEFRAG_EIO;
    set_used(t, n, 1);
    /* 3. the directory entry */
    dir_of(&d, it->parent);
    if ((lba = dir_lba(&d, (oa_u32)it->slot << 5)) == NOBUF) return OA_DEFRAG_EDIR;
    if (!(e = dread(lba))) return OA_DEFRAG_EIO;
    e += ((unsigned)it->slot << 5) & (ssize - 1);
    if (first_of(e) != first) return OA_DEFRAG_EDIR;
    set_first(e, t);
    if (D->write(D->dev, D->start + lba, 1, dsec)) return OA_DEFRAG_EIO;
    /* 4. the ".." entries of the subdirectories */
    if (isdir)
        for (k = 0; k < nitems; ++k) {
            s = item((unsigned)k);
            if (s->parent != i || !(s->flags & F_DIR)) continue;
            lba = heap + ((s->first - 2) << cshift);
            if (!(e = dread(lba))) return OA_DEFRAG_EIO;
            if (!is_name(e + 32, "..         ") || first_of(e + 32) != first) return OA_DEFRAG_EDIR;
            set_first(e + 32, t);
            if (D->write(D->dev, D->start + lba, 1, dsec)) return OA_DEFRAG_EIO;
        }
    /* 5. the old chain freed */
    for (c = first, run = 0, k = 0; k < n; ++k, c = v) {
        if (fat_get(c, &v) || fat_put(c, 0)) return OA_DEFRAG_EIO;
        set_used(c, 1, 0);
        if (k + 1 == n || v != c + 1) { show(c + 1 - (run + 1), run + 1, OA_DEFRAG_FREE); run = 0; }
        else ++run;
    }
    if (fat_flush()) return OA_DEFRAG_EIO;
    resort(i, t);
    it->flags &= (oa_u8)~F_FRAG;
    ++D->moves; D->moved += n;
    show(t, n, OA_DEFRAG_USED);
    return 0;
}

/* every fragmented file to the lowest free run that holds it, or with
   to_end to the highest (FULL places them after) */
static int unfragment(int to_end)
{
    struct item FAR *it;
    unsigned i;
    oa_u32 t, left = D->fragmented;
    int rc;
    for (i = 0; i < nitems; ++i) {
        it = item(i);
        if (!(it->flags & F_FRAG)) continue;
        say("Joining fragmented files: %lu left", (unsigned long)left--);
        if (!(t = to_end ? last_fit(it->count, 2) : first_fit(it->count, 2))) continue;
        if (stopped()) return OA_DEFRAG_ESTOP;
        if ((rc = move((oa_u16)i, t)) != 0) return rc;
    }
    return 0;
}

static int full(void)
{
    struct item FAR *it;
    oa_u32 p = 2, c, n, t;
    unsigned pass, k, placed = 0;
    oa_u16 o;
    int rc;
    for (pass = 0; pass < 2; ++pass)    /* directories, then files */
        for (k = 0; k < nitems; ++k) {
            it = item(k);
            if ((it->flags & F_DIR ? 0u : 1u) != pass) continue;
            n = it->count;
            say("Placing %s %u of %u", pass ? "files" : "directories", ++placed, nitems);
            for (;;) {
                D->done = p;
                if (it->first == p && !(it->flags & F_FRAG)) { p += n; break; }
                if (p + n > nclu + 2) return 0;
                if ((c = skip(p, p + n, 0)) == p + n) {
                    if (stopped()) return OA_DEFRAG_ESTOP;
                    if ((rc = move((oa_u16)k, p)) != 0) return rc;
                    p += n;
                    break;
                }
                if ((o = owner(c)) == NONE) { p = c + 1; continue; }   /* stays where it is */
                /* the file in the way (or this one) to the end */
                if (!(t = last_fit(item(o)->count, p + n))) {
                    say("Not enough free space to go on");
                    return 0;
                }
                if (stopped()) return OA_DEFRAG_ESTOP;
                if ((rc = move(o, t)) != 0) return rc;
            }
        }
    D->done = p;
    return 0;
}

/* ---- boot sector ---- */

static int is_pow2(oa_u32 v) { return v && !(v & (v - 1)); }

static int read_boot(void)
{
    oa_u32 total, bps;
    if (D->read(D->dev, D->start, 1, boot)) return OA_DEFRAG_EIO;
    bps = rd16(boot+11);
    if (bps < 512 || bps > MAXS || !is_pow2(bps) || !is_pow2(boot[13]) || !rd16(boot+14) || !boot[16] ||
        !(rd16(boot+19) || rd32(boot+32)) || !(rd16(boot+22) || rd32(boot+36))) return OA_DEFRAG_EBAD;
    ssize = (unsigned)bps;
    for (sshift = 9; (1u << sshift) < ssize; ++sshift) ;
    for (cshift = 0; (1u << cshift) < boot[13]; ++cshift) ;
    if (D->bufsize < ssize) return OA_DEFRAG_EMEM;
    nfats = boot[16];
    fat0 = rd16(boot+14);
    fatlen = rd16(boot+22) ? rd16(boot+22) : rd32(boot+36);
    rootsecs = ((oa_u32)rd16(boot+17) * 32 + ssize - 1) >> sshift;
    rootsec = fat0 + nfats * fatlen;
    heap = rootsec + rootsecs;
    total = rd16(boot+19) ? rd16(boot+19) : rd32(boot+32);
    if (heap >= total) return OA_DEFRAG_EBAD;
    nclu = (total - heap) >> cshift;
    if (!rd16(boot+22)) bits = 32;
    else bits = nclu < 4085 ? 12 : nclu < 65525UL ? 16 : 0;
    if (!bits) return OA_DEFRAG_EBAD;
    if (bits == 32 && nclu > 0x0FFFFFF5UL) nclu = 0x0FFFFFF5UL;
    mirror = 1; active = 0; rootclu = 0;
    if (bits == 32) {
        rootclu = rd32(boot+44);
        if (rd16(boot+40) & 0x80) { mirror = 0; active = rd16(boot+40) & 0x0F; }
        if (active >= nfats || !valid(rootclu)) return OA_DEFRAG_EBAD;
    } else if (!rootsecs) return OA_DEFRAG_EBAD;
    D->bits = bits; D->clusters = nclu; D->cluster_size = (oa_u32)ssize << cshift;
    return 0;
}

int oa_defrag_run(struct oa_defrag *d)
{
    struct item FAR *it;
    oa_u32 c, v, off, len;
    oa_u8 *p;
    unsigned i;
    int rc, rc2;
    D = d;
    d->used = d->files = d->dirs = d->fragmented = d->fragmented_after = d->moves = d->moved = d->done = 0;
    fc[0].lba = fc[1].lba = NOBUF; fc[0].dirty = fc[1].dirty = 0;
    dlba = NOBUF; nitems = 0;
    if ((rc = read_boot()) != 0) return rc;
    bmsize = ((nclu + 2 + 7) / 8 + 15) & ~15UL;
    if (bmsize > d->memsize) return OA_DEFRAG_EMEM;
    for (off = 0; off < bmsize; off += len) {
        len = d->block - (off & (d->block - 1));
        if (len > bmsize - off) len = bmsize - off;
        FMEMSET(at(off), 0, (unsigned)len);
    }
    say("Reading the FAT");
    for (c = 2; c < nclu + 2; ++c) {
        if (fat_get(c, &v)) return OA_DEFRAG_EIO;
        if (v) { set_used(c, 1, 1); ++d->used; }
    }
    if (bits != 12) {
        if (!(p = fat_byte(bits / 8))) return OA_DEFRAG_EIO;
        head1 = bits == 16 ? rd16(p) : rd32(p) & 0x0FFFFFFFUL;
    }
    show(2, nclu, OA_DEFRAG_USED);
    say("Reading the directories");
    if ((rc = walk(NONE, 0)) != 0) return rc;
    sort();
    if (d->mode == OA_DEFRAG_ANALYZE) { d->fragmented_after = d->fragmented; return 0; }
    if (set_head(1)) return OA_DEFRAG_EIO;
    rc = unfragment(d->mode == OA_DEFRAG_FULL);
    if (!rc && d->mode == OA_DEFRAG_FULL) rc = full();
    rc2 = fat_flush();
    if (rc == OA_DEFRAG_EIO || rc == OA_DEFRAG_EDIR || rc2) return rc ? rc : OA_DEFRAG_EIO;
    if (set_head(0)) return OA_DEFRAG_EIO;
    for (i = 0; i < nitems; ++i) {
        it = item(i);
        if (it->flags & F_FRAG) ++d->fragmented_after;
    }
    return rc;
}
