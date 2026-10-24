/* FAT12/16/32 volume check (ASPICHK).  The checks and repairs follow
   fsck.fat of dosfstools 4.2 (GPL version 3 or later); the walk is the one
   of EXCHK: errors about the structure (boot sector, FATs, entries, chains)
   are reported and repaired in the first pass, errors about single clusters
   (lost, cross-linked) in the pass whose window holds the cluster.  Repairs
   are written at once, so the later passes see them.
   A FAT entry that is broken in the active FAT is taken from another FAT
   when that one is valid; with /F the other FATs are written from the
   active one at the end.  Cross-linked files: the one met later is cut
   before the shared cluster (as fsck.fat does).  Lost clusters are freed.
   Not repaired: invalid characters in 8.3 names, a missing "." or ".."
   entry whose slot holds another entry. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "openaspi/fat_check.h"

#define MAXS     OA_FATCHK_MAXSECT
#define NOBUF    0xFFFFFFFFUL
#define EOC      0xFFFFFFFFUL           /* normalized FAT values */
#define BADCLU   0xFFFFFFF7UL
#define MAXDEPTH 64
#define MAXRANGE 10                     /* listed ranges, cross links, FAT differences */
#define MAXLFN   20                     /* long name entries of one name */

struct cache { oa_u32 lba; oa_u8 b[MAXS]; };
/* a directory: the fixed FAT12/16 root, or a cluster chain */
struct dir { oa_u32 first, idx, clu; int fixed; };

static struct cache cdir, cfat, cfat2;  /* cfat2: the other FAT copies */
static oa_u8 boot[MAXS];
static struct oa_fatchk *C;
static unsigned bits, nfats, active, ssize, sshift, cshift;
static int mirror;                      /* the FATs are copies of each other */
static oa_u32 fat0, fatlen, rootsec, rootsecs, heap, nclu, rootclu, fsinfo, backup;
static oa_u32 lo, hi;                   /* the window: clusters lo+2 .. hi+1 */
static int first_pass;
static char path[300];
static unsigned plen;
static char text[400];
static unsigned nlost, ncross, ndiff;   /* reported */
static oa_u16 lfn[MAXLFN * 13 + 1];
static int head_written;                /* entries 0/1 written to the active FAT only */

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
    if (level == OA_FATCHK_ERROR) ++C->errors;
    if (level == OA_FATCHK_WARN) ++C->warnings;
    if (level == OA_FATCHK_FIXED) ++C->fixed;
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
            report(OA_FATCHK_ERROR, "Read error at sector %lu", (unsigned long)lba);
            return 0;
        }
        c->lba = lba;
    }
    return c->b;
}

static int put(struct cache *c)
{
    if (!C->write(C->dev, C->start + c->lba, c->b)) return 0;
    report(OA_FATCHK_ERROR, "Write error at sector %lu", (unsigned long)c->lba);
    c->lba = NOBUF;
    return -1;
}

/* ---- the FAT ---- */

static int valid(oa_u32 clu) { return clu >= 2 && clu < nclu + 2; }

/* the byte at off of the FAT at base */
static oa_u8 *fat_byte(struct cache *c, oa_u32 base, oa_u32 off)
{
    if (!sec(c, base + (off >> sshift))) return 0;
    return c->b + ((unsigned)off & (ssize - 1));
}

/* entry clu of the FAT copy at base, normalized: EOC, BADCLU, else the value */
static int raw_get(struct cache *c, oa_u32 base, oa_u32 clu, oa_u32 *v)
{
    oa_u8 *p;
    oa_u32 x;
    unsigned b0;
    if (bits == 12) {
        if (!(p = fat_byte(c, base, clu + clu / 2))) return -1;
        b0 = *p;
        if (!(p = fat_byte(c, base, clu + clu / 2 + 1))) return -1;
        x = b0 | (oa_u32)*p << 8;
        x = clu & 1 ? x >> 4 : x & 0xFFF;
        *v = x >= 0xFF8 ? EOC : x == 0xFF7 ? BADCLU : x;
    } else if (bits == 16) {
        if (!(p = fat_byte(c, base, clu * 2))) return -1;
        x = rd16(p);
        *v = x >= 0xFFF8UL ? EOC : x == 0xFFF7UL ? BADCLU : x;
    } else {
        if (!(p = fat_byte(c, base, clu * 4))) return -1;
        x = rd32(p) & 0x0FFFFFFFUL;
        *v = x >= 0x0FFFFFF8UL ? EOC : x == 0x0FFFFFF7UL ? BADCLU : x;
    }
    return 0;
}

static int raw_put(struct cache *c, oa_u32 base, oa_u32 clu, oa_u32 v)
{
    oa_u8 *p;
    oa_u32 off;
    if (bits == 12) {
        v = v == EOC ? 0xFFF : v == BADCLU ? 0xFF7 : v & 0xFFF;
        off = clu + clu / 2;
        if (!(p = fat_byte(c, base, off))) return -1;
        *p = clu & 1 ? (oa_u8)((*p & 0x0F) | (v << 4 & 0xF0)) : (oa_u8)v;
        if (put(c) || !(p = fat_byte(c, base, off + 1))) return -1;
        *p = clu & 1 ? (oa_u8)(v >> 4) : (oa_u8)((*p & 0xF0) | (v >> 8 & 0x0F));
    } else if (bits == 16) {
        if (!(p = fat_byte(c, base, clu * 2))) return -1;
        wr16(p, (oa_u16)(v == EOC ? 0xFFFF : v == BADCLU ? 0xFFF7 : v));
    } else {
        if (!(p = fat_byte(c, base, clu * 4))) return -1;
        v = v == EOC ? 0x0FFFFFFFUL : v == BADCLU ? 0x0FFFFFF7UL : v & 0x0FFFFFFFUL;
        wr32(p, (rd32(p) & 0xF0000000UL) | v);
    }
    return put(c);
}

static int fat_get(oa_u32 clu, oa_u32 *v) { return raw_get(&cfat, fat0 + active * fatlen, clu, v); }

/* the same entry in another copy (the first one that is not active) */
static int fat2_get(oa_u32 clu, oa_u32 *v) { return raw_get(&cfat2, fat0 + (active ? 0 : fatlen), clu, v); }

/* all FATs, or only the active one when they are no mirrors */
static int fat_set(oa_u32 clu, oa_u32 v)
{
    unsigned i;
    for (i = 0; i < nfats; ++i)
        if ((i == active || mirror) && raw_put(i == active ? &cfat : &cfat2, fat0 + i * fatlen, clu, v)) return -1;
    return 0;
}

/* ---- directories ---- */

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

/* the 32-byte entry at pos of a directory, 0 at its end; changes to it
   are written with put(&cdir) */
static oa_u8 *entry(struct dir *d, oa_u32 pos)
{
    oa_u32 lba = dir_lba(d, pos);
    if (lba == NOBUF || !sec(&cdir, lba)) return 0;
    return cdir.b + ((unsigned)pos & (ssize - 1));
}

/* marks the entries from..to (inclusive) deleted */
static void delete_entries(struct dir *d, oa_u32 from, oa_u32 to)
{
    oa_u8 *e;
    for (; from <= to; from += 32) {
        if (!(e = entry(d, from))) return;
        e[0] = 0xE5;
        if (put(&cdir)) return;
    }
}

static oa_u32 first_of(const oa_u8 *e) { return rd16(e+26) | (bits == 32 ? (oa_u32)rd16(e+20) << 16 : 0); }
static void set_first(oa_u8 *e, oa_u32 v) { wr16(e+26, (oa_u16)v); if (bits == 32) wr16(e+20, (oa_u16)(v >> 16)); }

/* ---- clusters ---- */

/* marks a cluster of the window: 1 if it was used before */
static int use(oa_u32 clu)
{
    if (clu < lo + 2 || clu >= hi + 2) return 0;
    return C->mark(C->ctx, clu - 2 - lo);
}

/* A loop in the chain from first (Brent): the index of the cluster whose
   entry closes it, or NOBUF. */
static oa_u32 loop_at(oa_u32 first)
{
    oa_u32 power = 1, lam = 1, mu, tort = first, hare, a, b, i;
    if (fat_get(first, &hare) || !valid(hare)) return NOBUF;
    while (tort != hare) {
        if (power == lam) { tort = hare; power <<= 1; lam = 0; }
        if (fat_get(hare, &hare) || !valid(hare) || lam > nclu) return NOBUF;
        ++lam;
    }
    /* the start of the loop: mu */
    a = b = first;
    for (i = 0; i < lam; ++i) if (fat_get(b, &b)) return NOBUF;
    for (mu = 0; a != b; ++mu)
        if (fat_get(a, &a) || fat_get(b, &b)) return NOBUF;
    return mu + lam - 1;
}

/* the cluster at index idx of the chain from first */
static oa_u32 cluster_at(oa_u32 first, oa_u32 idx)
{
    while (idx-- && !fat_get(first, &first)) ;
    return first;
}

/* Marks the clusters of a chain; count: clusters by the size, 0 = to the
   end of the chain (directories).  Returns the clusters it keeps; *broken:
   1 fewer than count or ended (first pass), 2 cut before a cross-linked
   first cluster, 3 cut before a cross-linked cluster later on (any pass;
   the caller sets the size).  Repairs end the chain. */
static oa_u32 use_chain(oa_u32 first, oa_u32 count, int *broken)
{
    oa_u32 c = first, prev = 0, n = 0, next, next2, pick, at, limit = NOBUF;
    const char *why = 0;
    int fp = first_pass, fix = fixing();
    *broken = 0;
    if (!count && fp && (at = loop_at(first)) != NOBUF) {
        /* a directory has no size to stop a loop: find it first */
        report(OA_FATCHK_ERROR, "%s: cluster chain runs in a loop", where());
        if (fix && !fat_set(cluster_at(first, at), EOC))
            report(OA_FATCHK_FIXED, "chain ended after %lu clusters", (unsigned long)(at + 1));
        limit = at + 1;
        *broken = 1;
    }
    for (;;) {
        if (use(c)) {
            /* the second user of a cluster: a loop of its own, or another file's */
            if ((at = loop_at(first)) != NOBUF) {
                if (fp) report(OA_FATCHK_ERROR, "%s: cluster chain runs in a loop", where());
                if (fix && !fat_set(cluster_at(first, at), EOC))
                    report(OA_FATCHK_FIXED, "chain ended after %lu clusters", (unsigned long)(at + 1));
                *broken = 1;
                return at + 1 < n ? at + 1 : n;
            }
            ++C->cross;
            if (++ncross <= MAXRANGE)
                report(OA_FATCHK_ERROR, "%s: cluster %lu is used twice (cross-linked)", where(), (unsigned long)c);
            if (C->write && (prev ? !fat_set(prev, EOC) : 1)) {
                *broken = prev ? 3 : 2;
                report(OA_FATCHK_FIXED, "cut before the shared cluster, %lu clusters kept", (unsigned long)n);
            }
            return n;
        }
        ++n;
        if (n == limit) return n;
        if (fat_get(c, &next)) return n;
        if (nfats >= 2 && mirror && !fat2_get(c, &next2) && next2 != next) {
            /* the valid entry wins, or the one that goes on to the size */
            pick = next;
            if (next != EOC && (!valid(next) || next == BADCLU) && (next2 == EOC || valid(next2))) pick = next2;
            else if (next == EOC && count && n < count && valid(next2)) pick = next2;
            if (fp && ++ndiff <= MAXRANGE) report(OA_FATCHK_ERROR, "%s: the FATs differ at cluster %lu", where(), (unsigned long)c);
            if (fix && !fat_set(c, pick) && ndiff <= MAXRANGE)
                report(OA_FATCHK_FIXED, "entry of the %s FAT written to all", pick == next ? "active" : "other");
            next = pick;
        }
        if (next == EOC) break;
        if ((count && n >= count) || n >= nclu) {
            /* past the size or the volume: a loop outside the window? */
            if (fp && (at = loop_at(first)) != NOBUF) {
                report(OA_FATCHK_ERROR, "%s: cluster chain runs in a loop", where());
                if (fix && !fat_set(cluster_at(first, at), EOC))
                    report(OA_FATCHK_FIXED, "chain ended after %lu clusters", (unsigned long)(at + 1));
                *broken = 1;
                return at + 1 < n ? at + 1 : n;
            }
        }
        if (count && n >= count) {
            if (fp) report(OA_FATCHK_ERROR, "%s: cluster chain is longer than the size", where());
            if (fix && !fat_set(c, EOC)) report(OA_FATCHK_FIXED, "chain ended after %lu clusters", (unsigned long)n);
            return n;
        }
        if (n >= nclu) why = "cluster chain does not end";
        else if (!next) why = "cluster chain runs into a free cluster";
        else if (next == BADCLU) why = "cluster chain runs into a bad cluster";
        else if (!valid(next)) why = "invalid FAT entry in the cluster chain";
        if (why) {
            if (fp) report(OA_FATCHK_ERROR, "%s: %s", where(), why);
            if (fix && !fat_set(c, EOC)) report(OA_FATCHK_FIXED, "chain ended after %lu clusters", (unsigned long)n);
            *broken = 1;
            return n;
        }
        prev = c;
        c = next;
    }
    if (count && n < count) {
        if (fp) report(OA_FATCHK_ERROR, "%s: cluster chain has %lu of %lu clusters", where(), (unsigned long)n, (unsigned long)count);
        *broken = 1;
    }
    return n;
}

/* ---- names ---- */

static char oem(oa_u16 c)
{
    unsigned i;
    if (c < 0x80) return (char)(c < 0x20 ? '?' : c);
    if (C->cp) for (i = 0; i < 128; ++i) if (C->cp[i] == c) return (char)(0x80 + i);
    return '?';
}

static oa_u8 lfn_sum(const oa_u8 *e)
{
    unsigned i;
    oa_u8 s = 0;
    for (i = 0; i < 11; ++i) s = (oa_u8)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

/* an 8.3 name with a character DOS does not allow (fsck.fat's bad_name) */
static int bad_name(const oa_u8 *e)
{
    unsigned i;
    if (e[0] == ' ') return 1;
    for (i = 0; i < 11; ++i)
        if ((e[i] < ' ' && !(i == 0 && e[0] == 0x05)) || e[i] == 0x7F || strchr("*?<>|\"+,;:=[]\\/.", e[i])) return 1;
    return 0;
}

static void add_short_name(const oa_u8 *e)
{
    unsigned i, n = 8;
    while (n && e[n - 1] == ' ') --n;
    for (i = 0; i < n && plen < sizeof(path) - 1; ++i) path[plen++] = (char)(!i && e[0] == 0x05 ? 0xE5 : e[i]);
    for (n = 11; n > 8 && e[n - 1] == ' '; --n) ;
    if (n > 8 && plen < sizeof(path) - 1) path[plen++] = '.';
    for (i = 8; i < n && plen < sizeof(path) - 1; ++i) path[plen++] = (char)e[i];
    path[plen] = 0;
}

/* ---- directories ---- */

static void walk_dir(struct dir *d, oa_u32 parent, unsigned depth);

/* "." (dotdot 0) or ".." at pos of a subdirectory pointing to start */
static void dot_entry(struct dir *d, oa_u32 pos, int dotdot, oa_u32 start)
{
    oa_u8 *e = entry(d, pos);
    const char *name = dotdot ? "..         " : ".          ";
    int fix = fixing();
    if (!e || !first_pass) return;
    if (!memcmp(e, name, 11) && (e[11] & 0x10)) {
        if (first_of(e) == start) return;
        report(OA_FATCHK_ERROR, "%s: \"%s\" entry points to cluster %lu", where(), dotdot ? ".." : ".", (unsigned long)first_of(e));
        if (fix) { set_first(e, start); if (!put(&cdir)) report(OA_FATCHK_FIXED, "pointed to cluster %lu", (unsigned long)start); }
    } else if (!e[0] || e[0] == 0xE5) {
        report(OA_FATCHK_ERROR, "%s: \"%s\" entry is missing", where(), dotdot ? ".." : ".");
        if (fix) {
            memset(e, 0, 32);
            memcpy(e, name, 11);
            e[11] = 0x10;
            set_first(e, start);
            if (!put(&cdir)) report(OA_FATCHK_FIXED, "\"%s\" entry created", dotdot ? ".." : ".");
        }
    } else {
        report(OA_FATCHK_ERROR, "%s: another entry in the slot of \"%s\"%s", where(), dotdot ? ".." : ".", C->write ? " (not repaired)" : "");
    }
}

/* the short entry at pos with its long name entries from lstart (NOBUF:
   none): checks it, marks its clusters, enters a directory */
static void file_entry(struct dir *d, oa_u32 pos, oa_u32 lstart, unsigned nlfn, oa_u32 parent, unsigned depth)
{
    oa_u8 *e = entry(d, pos), attr = e[11];
    oa_u32 first = first_of(e), size = rd32(e+28), count, good, csz = 1UL << (sshift + cshift);
    unsigned base = plen, i;
    int broken, changed = 0, fp = first_pass, fix = fixing(), dir = attr & 0x10;
    struct dir sub;
    oa_u8 st[32];
    memcpy(st, e, 32);
    if (plen < sizeof(path) - 2) { path[plen++] = '\\'; path[plen] = 0; }
    if (nlfn) {
        for (i = 0; i < nlfn * 13 && lfn[i] && lfn[i] != 0xFFFF && plen < sizeof(path) - 1; ++i) path[plen++] = oem(lfn[i]);
        path[plen] = 0;
    } else add_short_name(st);
    if (fp) {
        if (dir) ++C->dirs; else ++C->files;
        if (bad_name(st)) report(OA_FATCHK_WARN, "%s: invalid character in the 8.3 name%s", where(), C->write ? " (not repaired)" : "");
    }
    if (dir) {
        if (size) {
            if (fp) report(OA_FATCHK_ERROR, "%s: directory with size %lu", where(), (unsigned long)size);
            if (fix) { size = 0; changed = 1; report(OA_FATCHK_FIXED, "size set to 0"); }
        }
        if (!first || first == parent || first == d->first || first == rootclu) {
            if (fp) report(OA_FATCHK_ERROR, "%s: directory starts at cluster %lu (root or a parent)", where(), (unsigned long)first);
            if (fix) {
                delete_entries(d, lstart == NOBUF ? pos : lstart, pos);
                report(OA_FATCHK_FIXED, "entry deleted");
            }
            goto out;
        }
    }
    if (first && !valid(first)) {
        if (fp) report(OA_FATCHK_ERROR, "%s: first cluster %lu is outside the volume", where(), (unsigned long)first);
        if (!fix) goto out;
        if (dir) {
            delete_entries(d, lstart == NOBUF ? pos : lstart, pos);
            report(OA_FATCHK_FIXED, "entry deleted");
            goto out;
        }
        first = size = 0; changed = 1;
        report(OA_FATCHK_FIXED, "size set to 0");
    }
    if (!dir && !size) {
        if (first) {
            if (fp) report(OA_FATCHK_ERROR, "%s: empty, but with first cluster %lu", where(), (unsigned long)first);
            if (fix) { first = 0; changed = 1; report(OA_FATCHK_FIXED, "first cluster cleared (its clusters are freed)"); }
        }
        goto out;
    }
    if (!first) {
        if (!dir && fp) report(OA_FATCHK_ERROR, "%s: size %lu without clusters", where(), (unsigned long)size);
        if (!dir && fix) { size = 0; changed = 1; report(OA_FATCHK_FIXED, "size set to 0"); }
        goto out;
    }
    count = dir ? 0 : size / csz + (size % csz != 0);
    if (count > nclu) {
        if (fp) report(OA_FATCHK_ERROR, "%s: size is larger than the volume", where());
        count = nclu;
    }
    good = use_chain(first, count, &broken);
    if (broken && C->write && (fix || broken >= 2)) {
        if (broken == 2 || !good) first = 0;
        if (!dir && size > good * csz) {
            size = good * csz;
            report(OA_FATCHK_FIXED, "size set to %lu clusters", (unsigned long)good);
        }
        changed = 1;
    }
    if (!dir || !first) goto out;
    if (depth >= MAXDEPTH) {
        if (fp) report(OA_FATCHK_ERROR, "%s: directories nested too deep", where());
        goto out;
    }
    memset(&sub, 0, sizeof(sub));
    sub.first = first;
    dot_entry(&sub, 0, 0, first);
    dot_entry(&sub, 32, 1, d->fixed || d->first == rootclu ? 0 : d->first);
    walk_dir(&sub, d->first, depth + 1);
out:
    if (changed && (e = entry(d, pos)) != 0) {
        set_first(e, first);
        wr32(e+28, size);
        put(&cdir);
    }
    plen = base; path[plen] = 0;
}

static void walk_dir(struct dir *d, oa_u32 parent, unsigned depth)
{
    oa_u8 *e, sum = 0;
    oa_u32 pos, lstart = NOBUF;
    unsigned expect = 0, nlfn = 0, ord, i;
    static const unsigned char at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    int fp = first_pass, fix = fixing();
    for (pos = d->fixed || d->first == rootclu ? 0 : 64; (e = entry(d, pos)) != 0; pos += 32) {
        if (!e[0]) break;
        if (e[0] == 0xE5) {
            if (lstart != NOBUF) {
                if (fp) report(OA_FATCHK_ERROR, "%s: long name entries without their 8.3 entry", where());
                if (fix) { delete_entries(d, lstart, pos - 32); report(OA_FATCHK_FIXED, "long name entries deleted"); }
            }
            lstart = NOBUF;
            continue;
        }
        if ((e[11] & 0x3F) == 0x0F) {           /* a long name entry */
            ord = e[0] & 0x3F;
            if (e[0] & 0x40) {
                if (lstart != NOBUF) {
                    if (fp) report(OA_FATCHK_ERROR, "%s: long name entries without their 8.3 entry", where());
                    if (fix) { delete_entries(d, lstart, pos - 32); report(OA_FATCHK_FIXED, "long name entries deleted"); }
                }
                lstart = NOBUF;
                if (ord && ord <= MAXLFN && !(e[0] & 0x80)) {
                    lstart = pos; nlfn = ord; expect = ord; sum = e[13];
                    memset(lfn, 0, sizeof(lfn));
                }
            } else if (lstart == NOBUF || ord != expect - 1 || e[13] != sum) {
                if (fp) report(OA_FATCHK_ERROR, "%s: long name entries out of order", where());
                if (fix) {
                    delete_entries(d, lstart == NOBUF ? pos : lstart, pos);
                    report(OA_FATCHK_FIXED, "long name entries deleted");
                }
                lstart = NOBUF;
                continue;
            } else expect = ord;
            if (lstart == NOBUF) {
                if (fp) report(OA_FATCHK_ERROR, "%s: invalid long name entry", where());
                if (fix) { delete_entries(d, pos, pos); report(OA_FATCHK_FIXED, "entry deleted"); }
                continue;
            }
            for (i = 0; i < 13; ++i) lfn[(ord - 1) * 13 + i] = rd16(e + at[i]);
            continue;
        }
        if (lstart != NOBUF && (expect != 1 || lfn_sum(e) != sum)) {
            if (fp) report(OA_FATCHK_ERROR, "%s: long name entries do not belong to the 8.3 entry after them", where());
            if (fix) { delete_entries(d, lstart, pos - 32); report(OA_FATCHK_FIXED, "long name entries deleted"); }
            if (!(e = entry(d, pos))) break;
            lstart = NOBUF;
        }
        if (e[11] & 0x08 && !(e[11] & 0x10)) {  /* volume label */
            if (!d->fixed && d->first != rootclu && fp)
                report(OA_FATCHK_WARN, "%s: volume label entry outside the root directory", where());
        } else if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) {
            if (fp) report(OA_FATCHK_ERROR, "%s: \"%s\" entry in the wrong place", where(), e[1] == '.' ? ".." : ".");
            if (fix) { delete_entries(d, pos, pos); report(OA_FATCHK_FIXED, "entry deleted"); }
        } else {
            file_entry(d, pos, lstart, lstart == NOBUF ? 0 : nlfn, parent, depth);
        }
        lstart = NOBUF;
    }
    if (lstart != NOBUF) {
        if (fp) report(OA_FATCHK_ERROR, "%s: long name entries without their 8.3 entry", where());
        if (fix) { delete_entries(d, lstart, pos - 32); report(OA_FATCHK_FIXED, "long name entries deleted"); }
    }
}

/* ---- the FAT against the clusters in use ---- */

static void range(oa_u32 a, oa_u32 b)
{
    if (++nlost > MAXRANGE) return;
    if (a == b) report(OA_FATCHK_ERROR, "Cluster %lu is allocated, but not used (lost)", (unsigned long)a);
    else report(OA_FATCHK_ERROR, "Clusters %lu-%lu are allocated, but not used (lost)", (unsigned long)a, (unsigned long)b);
}

static void compare(void)
{
    oa_u32 i, v, start = 0;
    int inuse, open = 0, lost;
    for (i = lo; i <= hi; ++i) {
        lost = 0;
        if (i < hi) {
            if (fat_get(i + 2, &v)) return;
            inuse = C->mark(C->ctx, i - lo);
            if (!v) {
                if (inuse) ++C->used;           /* (a chain into a free cluster has been reported) */
                else ++C->free;
            } else if (v == BADCLU) ++C->bad;
            else if (!inuse) {
                ++C->lost; lost = 1;
                if (C->write && !fat_set(i + 2, 0)) ++C->free;
            } else ++C->used;
        }
        if (lost != open) {
            if (open) range(start + 2, i + 1);
            open = lost; start = i;
        }
    }
}

/* ---- boot sector, FATs, FSInfo ---- */

static int is_pow2(oa_u32 v) { return v && !(v & (v - 1)); }

/* a FAT boot sector (BPB): 0 no, 1 yes */
static int bpb_ok(const oa_u8 *b)
{
    oa_u32 bps = rd16(b+11);
    return bps >= 512 && bps <= MAXS && is_pow2(bps) && is_pow2(b[13]) && rd16(b+14) && b[16] &&
           (rd16(b+19) || rd32(b+32)) && (rd16(b+22) || rd32(b+36));
}

static int check_boot(void)
{
    oa_u32 total, data, need;
    unsigned i;
    int fix = C->write != 0, from_backup = 0;
    if (C->read(C->dev, C->start, boot)) return OA_FATCHK_EIO;
    if (!bpb_ok(boot)) {
        /* FAT32: the backup boot sector (sector 6) */
        if (C->read(C->dev, C->start + 6, boot) || !bpb_ok(boot) || rd16(boot+22)) return OA_FATCHK_EBAD;
        report(OA_FATCHK_ERROR, "Boot sector is not FAT; using the backup in sector 6");
        from_backup = 1;
    }
    ssize = rd16(boot+11);
    for (sshift = 9; (1u << sshift) < ssize; ++sshift) ;
    for (cshift = 0; (1u << cshift) < boot[13]; ++cshift) ;
    nfats = boot[16];
    fat0 = rd16(boot+14);
    fatlen = rd16(boot+22) ? rd16(boot+22) : rd32(boot+36);
    rootsecs = ((oa_u32)rd16(boot+17) * 32 + ssize - 1) >> sshift;
    rootsec = fat0 + nfats * fatlen;
    heap = rootsec + rootsecs;
    total = rd16(boot+19) ? rd16(boot+19) : rd32(boot+32);
    if (heap >= total) { report(OA_FATCHK_ERROR, "Invalid FAT layout"); return OA_FATCHK_EBAD; }
    data = total - heap;
    nclu = data >> cshift;
    if (!rd16(boot+22)) bits = 32;              /* as fsck.fat: the FAT32 BPB decides */
    else bits = nclu < 4085 ? 12 : nclu < 65525UL ? 16 : 0;
    if (!bits) { report(OA_FATCHK_ERROR, "Too many clusters for FAT16"); return OA_FATCHK_EBAD; }
    if (bits == 32 && nclu > 0x0FFFFFF5UL) nclu = 0x0FFFFFF5UL;
    need = ((nclu + 2) * (bits / 4) + 1) / 2;   /* bytes of the FAT */
    if (fatlen < (need + ssize - 1) >> sshift) {
        report(OA_FATCHK_ERROR, "FAT is too small for %lu clusters", (unsigned long)nclu);
        return OA_FATCHK_EBAD;
    }
    C->bits = bits; C->clusters = nclu; C->cluster_size = 1UL << (sshift + cshift);
    mirror = 1; active = 0; rootclu = 0; fsinfo = backup = 0;
    if (bits == 32) {
        rootclu = rd32(boot+44);
        if (rd16(boot+40) & 0x80) { mirror = 0; active = rd16(boot+40) & 0x0F; }
        if (active >= nfats) { report(OA_FATCHK_ERROR, "Active FAT %u of %u FATs", active, nfats); active = 0; }
        fsinfo = rd16(boot+48); backup = rd16(boot+50);
        if (!valid(rootclu)) { report(OA_FATCHK_ERROR, "Root directory starts outside the volume"); return OA_FATCHK_EBAD; }
        C->serial = boot[66] == 0x29 ? rd32(boot+67) : 0;
    } else C->serial = boot[38] == 0x29 ? rd32(boot+39) : 0;
    if (bits != 32 && !rootsecs) { report(OA_FATCHK_ERROR, "No root directory"); return OA_FATCHK_EBAD; }
    if (boot[510] != 0x55 || boot[511] != 0xAA) report(OA_FATCHK_WARN, "Boot sector without signature 55AAh");
    if (from_backup && fix) {
        if (!C->write(C->dev, C->start, boot)) report(OA_FATCHK_FIXED, "boot sector written from the backup");
    } else if (bits == 32 && backup && backup < fat0) {
        /* the backup boot sector against the main one */
        static oa_u8 b2[MAXS];
        if (C->read(C->dev, C->start + backup, b2)) report(OA_FATCHK_ERROR, "Backup boot sector cannot be read");
        else {
            for (i = 0; i < 90 && (i == 65 || b2[i] == boot[i]); ++i) ;
            if (i < 90 || b2[510] != boot[510] || b2[511] != boot[511]) {
                report(OA_FATCHK_ERROR, "Backup boot sector differs from the main one");
                if (fix && !C->write(C->dev, C->start + backup, boot)) report(OA_FATCHK_FIXED, "backup boot sector written from the main one");
            }
        }
    }
    return OA_FATCHK_OK;
}

/* the FAT copies against the active one: sectors that differ */
static oa_u32 fats_differ(int write)
{
    oa_u32 s, n = 0;
    unsigned i;
    const oa_u8 *a;
    if (!mirror) return 0;
    for (i = 0; i < nfats; ++i) {
        if (i == active) continue;
        for (s = 0; s < fatlen; ++s) {
            if (!(a = sec(&cfat, fat0 + active * fatlen + s)) || !sec(&cfat2, fat0 + i * fatlen + s)) return n;
            if (!memcmp(a, cfat2.b, ssize)) continue;
            ++n;
            if (write) { memcpy(cfat2.b, a, ssize); if (put(&cfat2)) return n; }
        }
    }
    return n;
}

/* FAT entries 0 and 1: media byte, the clean and error bits; the dirty
   flag of the boot sector */
static void check_head(void)
{
    oa_u32 v0, v1, mask = bits == 12 ? 0xFFF : bits == 16 ? 0xFFFF : 0x0FFFFFFFUL;
    oa_u32 clean = bits == 16 ? 0x8000 : bits == 32 ? 0x08000000UL : 0;
    oa_u32 noerr = bits == 16 ? 0x4000 : bits == 32 ? 0x04000000UL : 0;
    oa_u8 *p;
    unsigned dflag = bits == 32 ? 65 : 37;
    oa_u32 w0, w1, base = fat0 + active * fatlen;
    if (!(p = fat_byte(&cfat, base, 0))) return;
    w0 = bits == 12 ? (rd16(p) & 0xFFF) : bits == 16 ? rd16(p) : rd32(p) & mask;
    if (!(p = fat_byte(&cfat, base, bits == 12 ? 1 : bits / 8))) return;
    w1 = bits == 12 ? (rd16(p) >> 4) : bits == 16 ? rd16(p) : rd32(p) & mask;
    v0 = (mask & ~0xFFUL) | boot[21];
    if (w0 != v0) {
        report(OA_FATCHK_WARN, "FAT entry 0 is %lX, the media byte is %02Xh", (unsigned long)w0, boot[21]);
        if (C->write && !raw_put(&cfat, base, 0, v0)) { head_written = 1; report(OA_FATCHK_FIXED, "FAT entry 0 corrected"); }
    }
    v1 = w1;
    if (clean && !(w1 & clean)) { report(OA_FATCHK_WARN, "Volume is marked dirty (not cleanly shut down)"); v1 |= clean; }
    if (noerr && !(w1 & noerr)) { report(OA_FATCHK_WARN, "Volume is marked as having had disk errors"); v1 |= noerr; }
    if ((v1 | clean | noerr) != mask) v1 = mask;
    if (v1 != w1 && C->write) {
        head_written = 1;
        if (!(p = fat_byte(&cfat, base, bits == 12 ? 1 : bits / 8))) return;
        if (bits == 12) { p[0] = (oa_u8)((p[0] & 0x0F) | (v1 << 4 & 0xF0)); }
        else if (bits == 16) wr16(p, (oa_u16)v1);
        else wr32(p, (rd32(p) & 0xF0000000UL) | v1);
        if (bits == 12) { if (!put(&cfat) && (p = fat_byte(&cfat, base, 2)) != 0) { p[0] = (oa_u8)(v1 >> 4); if (!put(&cfat)) report(OA_FATCHK_FIXED, "FAT entry 1 corrected"); } }
        else if (!put(&cfat)) report(OA_FATCHK_FIXED, "volume marked clean");
    }
    if (boot[dflag] & 1) {
        report(OA_FATCHK_WARN, "Boot sector flags the volume dirty");
        if (C->write) {
            boot[dflag] &= (oa_u8)~1;
            if (!C->write(C->dev, C->start, boot)) report(OA_FATCHK_FIXED, "dirty flag of the boot sector cleared");
        }
    }
}

static void check_fsinfo(void)
{
    oa_u32 nfree, next;
    if (bits != 32 || !fsinfo || fsinfo >= fat0 || !sec(&cdir, fsinfo)) return;
    if (rd32(cdir.b) != 0x41615252UL || rd32(cdir.b + 484) != 0x61417272UL || rd32(cdir.b + 508) != 0xAA550000UL) {
        report(OA_FATCHK_WARN, "FSInfo sector has no valid signatures%s", C->write ? " (not repaired)" : "");
        return;
    }
    nfree = rd32(cdir.b + 488); next = rd32(cdir.b + 492);
    if (nfree != 0xFFFFFFFFUL && nfree != C->free) {
        report(OA_FATCHK_WARN, "FSInfo free count is %lu, the FAT has %lu free clusters", (unsigned long)nfree, (unsigned long)C->free);
        if (C->write) {
            wr32(cdir.b + 488, C->free);
            if (!put(&cdir)) report(OA_FATCHK_FIXED, "FSInfo free count corrected");
        }
    }
    if (next != 0xFFFFFFFFUL && !valid(next)) {
        report(OA_FATCHK_WARN, "FSInfo next free cluster %lu is outside the volume", (unsigned long)next);
        if (C->write && sec(&cdir, fsinfo)) {
            wr32(cdir.b + 492, 0xFFFFFFFFUL);
            if (!put(&cdir)) report(OA_FATCHK_FIXED, "FSInfo next free cluster cleared");
        }
    }
}

int oa_fatchk_run(struct oa_fatchk *c)
{
    struct dir r;
    oa_u32 differ;
    int rc, broken;
    C = c;
    c->errors = c->warnings = c->fixed = c->used = c->free = c->bad = c->lost = c->cross = 0;
    c->files = c->dirs = 0;
    cdir.lba = cfat.lba = cfat2.lba = NOBUF;
    plen = 0; path[0] = 0; nlost = ncross = ndiff = 0; head_written = 0;
    first_pass = 1;
    if ((rc = check_boot()) != OA_FATCHK_OK) return rc;
    if (nfats >= 2 && mirror && (differ = fats_differ(0)) != 0)
        report(OA_FATCHK_ERROR, "The FATs differ in %lu sectors", (unsigned long)differ);
    else differ = 0;
    check_head();
    if (!c->window) c->window = nclu;
    for (lo = 0; lo < nclu; lo = hi) {
        hi = nclu - lo > c->window ? lo + c->window : nclu;
        c->clear(c->ctx);
        memset(&r, 0, sizeof(r));
        if (bits == 32) {
            r.first = rootclu;
            use_chain(rootclu, 0, &broken);
        } else r.fixed = 1;
        walk_dir(&r, 0, 0);
        compare();
        first_pass = 0;
    }
    if (nlost > MAXRANGE || ncross > MAXRANGE || ndiff > MAXRANGE)
        report(OA_FATCHK_INFO, "(only the first %u ranges, cross links and FAT differences are listed)", MAXRANGE);
    if (c->write && c->lost) report(OA_FATCHK_FIXED, "%lu lost clusters freed", (unsigned long)c->lost);
    if (differ && c->write) {
        fats_differ(1);
        report(OA_FATCHK_FIXED, "the other FATs written from the active one");
    } else if (head_written && nfats >= 2 && mirror) fats_differ(1);
    check_fsinfo();
    return OA_FATCHK_OK;
}
