/* exFAT formatting, after the Microsoft exFAT file system specification:
   the layout, then the FAT(s), allocation bitmap(s), up-case table, root
   directory and the boot region. */
#include <string.h>
#include "openaspi/exfat_format.h"

#ifdef __WATCOMC__
#define fmemcpy _fmemcpy
#define fmemset _fmemset
#else
#define fmemcpy memcpy
#define fmemset memset
#endif

#define BADCLU 0xFFFFFFF7UL
#define EOC    0xFFFFFFFFUL
#define NFILES 4                        /* bitmap, second bitmap, up-case table, root */

static void wr16(oa_u8 OA_FAR *p, oa_u16 v) { p[0] = (oa_u8)v; p[1] = (oa_u8)(v >> 8); }
static void wr32(oa_u8 OA_FAR *p, oa_u32 v) { wr16(p, (oa_u16)v); wr16(p + 2, (oa_u16)(v >> 16)); }
static oa_u32 rot32(oa_u32 sum, oa_u8 b) { return ((sum & 1) ? 0x80000000UL : 0) + (sum >> 1) + b; }

int oa_exfmt_layout(struct oa_exfmt *f)
{
    oa_u32 csz = f->cluster, spc, mb = f->sectors >> 11, len = 0, need, n;
    if (f->sectors < OA_EXFMT_MINSECT || (f->nfats != 1 && f->nfats != 2) || f->label_len > 11) return -1;
    if (!csz) csz = mb <= 256 ? 4096UL : mb <= 32768UL ? 32768UL : 131072UL;
    for (f->cshift = 0; f->cshift < 16 && (512UL << f->cshift) < csz; ++f->cshift) ;
    if ((512UL << f->cshift) != csz) return -1;     /* a power of two, 512 bytes to 32 MB */
    spc = 1UL << f->cshift;
    f->fat_off = (24 + spc - 1) & ~(spc - 1);       /* FAT and heap on cluster boundaries */
    for (;;) {
        f->heap = (f->fat_off + f->nfats * len + spc - 1) & ~(spc - 1);
        if (f->heap >= f->sectors) return -1;
        n = (f->sectors - f->heap) >> f->cshift;
        need = (n + 2 + 127) / 128;
        if (need <= len) break;
        len = need;
    }
    f->fat_len = len;
    f->clusters = n;
    f->bitmap_len = (n + 7) / 8;
    f->sys_clusters = f->nfats * ((f->bitmap_len + (csz - 1)) >> (9 + f->cshift))
                      + ((OA_EXFMT_UPCASE_LEN + csz - 1) >> (9 + f->cshift)) + 1;
    return n > 0xFFFFFFF5UL || n <= f->sys_clusters ? -1 : 0;
}

/* the format in progress */
static struct oa_exfmt *F;
static const oa_u32 *bad;
static unsigned nbad, bi;               /* bi: first bad cluster >= the last asked */
static oa_u32 first[NFILES], last[NFILES], size[NFILES], sys_end;

static int is_bad(oa_u32 c)
{
    if (bi && bad[bi - 1] >= c) bi = 0;
    while (bi < nbad && bad[bi] < c) ++bi;
    return bi < nbad && bad[bi] == c;
}

/* the next good cluster after c */
static oa_u32 next_good(oa_u32 c)
{
    do ++c; while (is_bad(c));
    return c;
}

static oa_u32 fat_entry(oa_u32 c)
{
    unsigned k;
    if (c < 2) return c ? EOC : 0xFFFFFFF8UL;
    if (is_bad(c)) return BADCLU;
    if (c >= sys_end) return 0;
    for (k = 0; k < NFILES; ++k) if (size[k] && c == last[k]) return EOC;
    return next_good(c);
}

/* the clusters from c on that are free: neither system nor bad */
static oa_u32 free_run(oa_u32 c)
{
    if (c < sys_end || is_bad(c)) return 0;
    return bi < nbad ? bad[bi] - c : 0xFFFFFFFFUL;
}

/* n entries of the FAT from entry e on */
static void fill_fat(oa_u8 OA_FAR *b, oa_u32 e, unsigned n)
{
    oa_u32 run;
    unsigned i = 0, k;
    while (i < n) {
        run = e + i >= F->clusters + 2 ? 0xFFFFFFFFUL : free_run(e + i);
        if (run) {
            k = run < (oa_u32)(n - i) ? (unsigned)run : n - i;
            fmemset(b + 4 * i, 0, 4 * k);
            i += k;
        } else {
            wr32(b + 4 * i, fat_entry(e + i));
            ++i;
        }
    }
}

/* n bytes of the allocation bitmap from byte o on */
static void fill_bitmap(oa_u8 OA_FAR *b, oa_u32 o, unsigned n)
{
    unsigned i, j;
    oa_u32 c;
    oa_u8 v;
    for (i = 0; i < n; ++i, ++o) {
        c = 2 + o * 8;
        v = 0;
        if (o >= F->bitmap_len || free_run(c) >= 8) { b[i] = 0; continue; }
        for (j = 0; j < 8; ++j, ++c)
            if (c < F->clusters + 2 && (c < sys_end || is_bad(c))) v |= (oa_u8)(1 << j);
        b[i] = v;
    }
}

static void root_dir(oa_u8 OA_FAR *b)
{
    oa_u32 sum = 0;
    unsigned i;
    oa_u8 OA_FAR *e = b;
    fmemset(b, 0, 512);
    if (F->label_len) {
        e[0] = 0x83;
        e[1] = F->label_len;
        for (i = 0; i < F->label_len; ++i) wr16(e + 2 + 2 * i, F->label[i]);
        e += 32;
    }
    for (i = 0; i < F->nfats; ++i, e += 32) {
        e[0] = 0x81;
        e[1] = (oa_u8)i;                /* BitmapFlags: first or second bitmap */
        wr32(e + 20, first[i]);
        wr32(e + 24, F->bitmap_len);
    }
    for (i = 0; i < OA_EXFMT_UPCASE_LEN; ++i) sum = rot32(sum, oa_exfmt_upcase[i]);
    e[0] = 0x82;
    wr32(e + 4, sum);
    wr32(e + 20, first[2]);
    wr32(e + 24, OA_EXFMT_UPCASE_LEN);
}

/* sector s of system file k */
static void file_sector(unsigned k, oa_u32 s, oa_u8 OA_FAR *b)
{
    oa_u32 o = s * 512;
    fmemset(b, 0, 512);
    if (k < 2) fill_bitmap(b, o, 512);
    else if (k == 2) {
        if (o < OA_EXFMT_UPCASE_LEN)
            fmemcpy(b, oa_exfmt_upcase + (unsigned)o, OA_EXFMT_UPCASE_LEN - o < 512 ? (unsigned)(OA_EXFMT_UPCASE_LEN - o) : 512);
    } else if (!s) root_dir(b);
}

static void boot_sector(oa_u8 OA_FAR *b)
{
    oa_u32 used = F->sys_clusters + nbad;
    fmemset(b, 0, 512);
    b[0] = 0xEB; b[1] = 0x76; b[2] = 0x90;
    fmemcpy(b + 3, "EXFAT   ", 8);
    wr32(b + 64, F->hidden);
    wr32(b + 72, F->sectors);
    wr32(b + 80, F->fat_off);
    wr32(b + 84, F->fat_len);
    wr32(b + 88, F->heap);
    wr32(b + 92, F->clusters);
    wr32(b + 96, first[3]);
    wr32(b + 100, F->serial);
    wr16(b + 104, 0x0100);              /* revision 1.00 */
    b[108] = 9;
    b[109] = F->cshift;
    b[110] = F->nfats;
    b[111] = 0x80;
    b[112] = (oa_u8)(used * 100 / F->clusters);
    b[120] = 0xF4; b[121] = 0xEB; b[122] = 0xFD;    /* no boot code: hlt, jmp back */
    b[510] = 0x55; b[511] = 0xAA;
}

/* the boot region (12 sectors) at lba; nbuf >= 1 */
static int boot_region(oa_exfmt_write_fn wr, void *dev, oa_u32 lba, oa_u8 OA_FAR *b)
{
    oa_u32 sum = 0;
    unsigned s, i;
    for (s = 0; s < 12; ++s) {
        if (!s) boot_sector(b);
        else if (s < 11) {
            fmemset(b, 0, 512);
            if (s < 9) { b[510] = 0x55; b[511] = 0xAA; }    /* extended boot sectors */
        } else for (i = 0; i < 128; ++i) wr32(b + 4 * i, sum);
        if (s < 11)
            for (i = 0; i < 512; ++i)
                if (s || (i != 106 && i != 107 && i != 112)) sum = rot32(sum, b[i]);
        if (wr(dev, lba + s, 1, b)) return -1;
    }
    return 0;
}

static int zeros(oa_exfmt_write_fn wr, void *dev, oa_u32 lba, oa_u32 n, oa_u8 OA_FAR *buf, unsigned nbuf)
{
    unsigned k;
    fmemset(buf, 0, nbuf * 512);
    for (; n; n -= k, lba += k) {
        k = n < nbuf ? (unsigned)n : nbuf;
        if (wr(dev, lba, k, buf)) return -1;
    }
    return 0;
}

int oa_exfmt_write(struct oa_exfmt *f, oa_exfmt_write_fn wr, void *dev,
                   const oa_u32 *badlist, unsigned badcount, oa_u8 OA_FAR *buf, unsigned nbuf)
{
    oa_u32 c, s, n, lba, spc = 1UL << f->cshift, csz = spc * 512;
    unsigned k, i, j;
    F = f; bad = badlist; nbad = badcount; bi = 0;
    /* the system files, one after the other round the bad clusters */
    size[0] = (f->bitmap_len + csz - 1) / csz;
    size[1] = f->nfats == 2 ? size[0] : 0;
    size[2] = (OA_EXFMT_UPCASE_LEN + csz - 1) / csz;
    size[3] = 1;
    c = 2;
    for (k = 0; k < NFILES; ++k) {
        first[k] = last[k] = 0;
        for (n = 0; n < size[k]; ++n, ++c) {
            while (is_bad(c)) ++c;
            if (c >= f->clusters + 2) return -2;
            if (!n) first[k] = c;
            last[k] = c;
        }
    }
    sys_end = c;
    f->bitmap_first[0] = first[0];
    f->bitmap_first[1] = first[1];
    f->upcase_first = first[2];
    f->root = first[3];
    /* no old file system while this one is incomplete */
    if (zeros(wr, dev, 0, 1, buf, 1)) return -1;
    if (boot_region(wr, dev, 12, buf)) return -1;
    if (zeros(wr, dev, 24, f->fat_off - 24, buf, nbuf)) return -1;
    for (j = 0; j < f->nfats; ++j)
        for (s = 0; s < f->fat_len; s += k) {
            k = f->fat_len - s < nbuf ? (unsigned)(f->fat_len - s) : nbuf;
            for (i = 0; i < k; ++i) fill_fat(buf + i * 512, (s + i) * 128, 128);
            if (wr(dev, f->fat_off + j * f->fat_len + s, k, buf)) return -1;
        }
    lba = f->fat_off + f->nfats * f->fat_len;
    if (zeros(wr, dev, lba, f->heap - lba, buf, nbuf)) return -1;
    for (k = 0; k < NFILES; ++k) {
        if (!size[k]) continue;
        s = 0;
        for (c = first[k]; ; c = next_good(c)) {
            for (n = 0; n < spc; n += i) {
                i = spc - n < nbuf ? (unsigned)(spc - n) : nbuf;
                for (j = 0; j < i; ++j) file_sector(k, s + j, buf + j * 512);
                if (wr(dev, f->heap + ((c - 2) << f->cshift) + n, i, buf)) return -1;
                s += i;
            }
            if (c == last[k]) break;
        }
    }
    return boot_region(wr, dev, 0, buf);
}
