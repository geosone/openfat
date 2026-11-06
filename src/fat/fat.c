/* FAT12/16/32 reading and writing with VFAT long names; see
   include/openfat/fat.h. */
#include <string.h>
#include "openfat/fat.h"

#ifdef __WATCOMC__
#define fmemcpy _fmemcpy
#define fmemset _fmemset
#else
#define fmemcpy memcpy
#define fmemset memset
#endif

#define NOBUF 0xFFFFFFFFUL
#define CB(v) ((v)->sshift + (v)->cshift)      /* bytes per cluster, a shift */

static ofat_u16 rd16(const ofat_u8 OFAT_FAR *p) { return (ofat_u16)(p[0] | p[1] << 8); }
static ofat_u32 rd32(const ofat_u8 OFAT_FAR *p) { return rd16(p) | (ofat_u32)rd16(p + 2) << 16; }
static void wr16(ofat_u8 *p, ofat_u16 v) { p[0] = (ofat_u8)v; p[1] = (ofat_u8)(v >> 8); }
static void wr32(ofat_u8 *p, ofat_u32 v) { wr16(p, (ofat_u16)v); wr16(p + 2, (ofat_u16)(v >> 16)); }

/* scratch entries: the library is not reentrant (like the exFAT code) and
   keeps the DOS stack small */
static struct ofat_fat_ent tmp;
static ofat_u8 se[32];

/* ---- sectors ---- */

static ofat_u8 *sector(struct ofat_fat *v, ofat_u32 lba)
{
    if (v->buf_lba != lba) {
        v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + lba, v->buf)) return NULL;
        v->buf_lba = lba;
    }
    return v->buf;
}

/* writes buf (holding lba, changed by the caller) */
static int put(struct ofat_fat *v, ofat_u32 lba)
{
    if (!v->write) return OFAT_FAT_EWRITE;
    if (v->write(v->dev, v->start + lba, v->buf)) { v->buf_lba = NOBUF; return OFAT_FAT_EWRITE; }
    return OFAT_FAT_OK;
}

/* ---- the FAT ---- */

static ofat_u32 eoc(const struct ofat_fat *v)
{
    return v->bits == 12 ? 0xFFFUL : v->bits == 16 ? 0xFFFFUL : 0x0FFFFFFFUL;
}

static int valid(const struct ofat_fat *v, ofat_u32 c) { return c >= 2 && c <= v->clusters + 1; }
static int at_end(const struct ofat_fat *v, ofat_u32 c) { return c >= (eoc(v) & ~7UL); }

/* the byte at off of FAT copy k */
static int fat_byte(struct ofat_fat *v, unsigned k, ofat_u32 off, ofat_u8 *b)
{
    const ofat_u8 *s = sector(v, v->fat + k * v->fat_size + (off >> v->sshift));
    if (!s) return OFAT_FAT_EIO;
    *b = s[(unsigned)off & ((1u << v->sshift) - 1)];
    return OFAT_FAT_OK;
}

static ofat_u32 entry_off(const struct ofat_fat *v, ofat_u32 c)
{
    return v->bits == 12 ? c + c / 2 : c * (v->bits / 8);
}

static int fat_get(struct ofat_fat *v, ofat_u32 c, ofat_u32 *val)
{
    ofat_u32 off = entry_off(v, c);
    ofat_u8 b[4];
    unsigned i, n = v->bits == 32 ? 4 : 2;
    int r;
    for (i = 0; i < n; ++i)
        if ((r = fat_byte(v, v->active, off + i, &b[i])) != 0) return r;
    if (v->bits == 12) *val = c & 1 ? rd16(b) >> 4 : rd16(b) & 0xFFF;
    else if (v->bits == 16) *val = rd16(b);
    else *val = rd32(b) & 0x0FFFFFFFUL;
    return OFAT_FAT_OK;
}

static int fat_set(struct ofat_fat *v, ofat_u32 c, ofat_u32 val)
{
    ofat_u32 off = entry_off(v, c), lba;
    ofat_u8 b[4], *s;
    unsigned i, k, n = v->bits == 32 ? 4 : 2, mask = (1u << v->sshift) - 1;
    int r;
    for (i = 0; i < n; ++i)
        if ((r = fat_byte(v, v->active, off + i, &b[i])) != 0) return r;
    if (v->bits == 12) {
        ofat_u16 w = rd16(b);
        w = c & 1 ? (ofat_u16)((w & 0x000F) | (ofat_u16)(val << 4)) : (ofat_u16)((w & 0xF000) | (val & 0xFFF));
        wr16(b, w);
    } else if (v->bits == 16) wr16(b, (ofat_u16)val);
    else wr32(b, (rd32(b) & 0xF0000000UL) | (val & 0x0FFFFFFFUL));
    for (k = 0; k < v->nfats; ++k) {
        if (!v->mirror && k != v->active) continue;
        for (i = 0; i < n; ) {
            lba = v->fat + k * v->fat_size + ((off + i) >> v->sshift);
            if (!(s = sector(v, lba))) return OFAT_FAT_EIO;
            do { s[(unsigned)(off + i) & mask] = b[i]; ++i; }
            while (i < n && ((off + i) >> v->sshift) == ((off + i - 1) >> v->sshift));
            if ((r = put(v, lba)) != 0) return r;
        }
    }
    return OFAT_FAT_OK;
}

/* the cluster number idx of a chain */
static int walk(struct ofat_fat *v, struct ofat_fat_file *f, ofat_u32 idx, ofat_u32 *clu)
{
    ofat_u32 i = 0, c = f->first, next;
    int r;
    if (f->clu && f->idx <= idx) { i = f->idx; c = f->clu; }
    if (!valid(v, c)) return c ? OFAT_FAT_EBAD : OFAT_FAT_END;
    while (i < idx) {
        if ((r = fat_get(v, c, &next)) != 0) return r;
        if (at_end(v, next)) return OFAT_FAT_END;
        if (!valid(v, next) || i > v->clusters) return OFAT_FAT_EBAD;
        c = next; ++i;
    }
    f->idx = i; f->clu = c;
    *clu = c;
    return OFAT_FAT_OK;
}

static ofat_u32 cluster_lba(const struct ofat_fat *v, ofat_u32 c)
{
    return v->heap + ((c - 2) << v->cshift);
}

static ofat_u32 clusters_for(const struct ofat_fat *v, ofat_u32 bytes)
{
    return bytes ? ((bytes - 1) >> CB(v)) + 1 : 0;
}

/* the sector holding byte off of a file or directory */
static int file_sector(struct ofat_fat *v, struct ofat_fat_file *f, ofat_u32 off, ofat_u32 *lba)
{
    ofat_u32 c;
    int r;
    if (f->root) {
        if (off >= (ofat_u32)v->root_entries * 32) return OFAT_FAT_END;
        *lba = v->root + (off >> v->sshift);
        return OFAT_FAT_OK;
    }
    if ((r = walk(v, f, off >> CB(v), &c)) != 0) return r;
    *lba = cluster_lba(v, c) + ((off >> v->sshift) & ((1UL << v->cshift) - 1));
    return OFAT_FAT_OK;
}

/* ---- mounting ---- */

static int pow2(ofat_u32 x) { return x && !(x & (x - 1)); }
static unsigned log2u(ofat_u32 x) { unsigned n = 0; while (x > 1) { x >>= 1; ++n; } return n; }

/* the geometry of a boot sector: 0 when it is no FAT one */
static int geometry(const ofat_u8 OFAT_FAR *b, struct ofat_fat *v)
{
    ofat_u32 bps = rd16(b + 11), spc = b[13], res = rd16(b + 14), total, fatsz, rootsecs;
    if (b[0] != 0xEB && b[0] != 0xE9) return 0;
    if (!pow2(bps) || bps < 512 || bps > 4096 || !pow2(spc) || spc > 128 || !res || !b[16]) return 0;
    if (b[21] != 0xF0 && b[21] < 0xF8) return 0;
    total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    fatsz = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    v->root_entries = rd16(b + 17);
    rootsecs = ((ofat_u32)v->root_entries * 32 + bps - 1) / bps;
    if (!total || !fatsz) return 0;
    v->sshift = (ofat_u8)log2u(bps);
    v->cshift = (ofat_u8)log2u(spc);
    v->nfats = b[16];
    v->fat = res;
    v->fat_size = fatsz;
    v->heap = res + b[16] * fatsz + rootsecs;
    if (v->heap >= total) return 0;
    v->clusters = (total - v->heap) >> v->cshift;
    /* FAT32 by its BPB, as Linux and Windows do (small FAT32 volumes
       exist); FAT12 or FAT16 by the cluster count */
    if (!rd16(b + 22)) {
        if (v->root_entries || !rd32(b + 36)) return 0;
        v->bits = 32;
    } else v->bits = v->clusters < 4085 ? 12 : 16;
    if (v->bits == 32 && v->clusters > 0x0FFFFFF5UL) return 0;
    if (v->bits == 12 ? (v->clusters + 2) * 3 / 2 > fatsz * bps
                      : v->clusters + 2 > fatsz * (bps / (v->bits / 8))) return 0;
    return v->bits;
}

int ofat_fat_probe(const ofat_u8 OFAT_FAR *boot)
{
    struct ofat_fat v;
    return geometry(boot, &v);
}

int ofat_fat_mount(struct ofat_fat *v, ofat_fat_read_fn read, void *dev, ofat_lba start,
                   const struct ofat_cp *cp, ofat_u8 *buf)
{
    const ofat_u8 *b, *l;
    ofat_u32 e1;
    int r;
    memset(v, 0, sizeof(*v));
    v->read = read; v->dev = dev; v->start = start; v->cp = cp; v->buf = buf;
    v->buf_lba = NOBUF;
    v->free = NOBUF;
    if (read(dev, start, buf)) return OFAT_FAT_EIO;
    if (rd16(buf + 11) > OFAT_FAT_MAXSECT && pow2(rd16(buf + 11))) return OFAT_FAT_ESIZE;
    if (!geometry(buf, v)) return OFAT_FAT_EBAD;
    v->buf_lba = 0;
    b = buf;
    v->mirror = 1;
    if (v->bits == 32) {
        if (b[40] & 0x80) { v->mirror = 0; v->active = (ofat_u8)(b[40] & 15); }
        if (v->active >= v->nfats) return OFAT_FAT_EBAD;
        v->root = rd32(b + 44);
        if (!valid(v, v->root)) return OFAT_FAT_EBAD;
        v->fsinfo = rd16(b + 48);
        l = b + 64;
    } else {
        v->root = v->fat + v->nfats * v->fat_size;
        l = b + 36;
    }
    if (l[2] == 0x29) { v->serial = rd32(l + 3); memcpy(v->label, l + 7, 11); }
    else memset(v->label, ' ', 11);
    if (v->fsinfo && v->fsinfo < v->fat) {
        if (!(b = sector(v, v->fsinfo))) return OFAT_FAT_EIO;
        if (rd32(b) == 0x41615252UL && rd32(b + 484) == 0x61417272UL) {
            if (rd32(b + 488) <= v->clusters) v->free = rd32(b + 488);
            if (valid(v, rd32(b + 492))) v->hint = rd32(b + 492);
        }
    } else v->fsinfo = 0;
    if (!v->hint) v->hint = 2;
    if (v->bits != 12) {
        if ((r = fat_get(v, 1, &e1)) != 0) return r;
        if (!(e1 & (v->bits == 16 ? 0x8000UL : 0x08000000UL))) v->dirty = 2;
    }
    return OFAT_FAT_OK;
}

void ofat_fat_rootdir(const struct ofat_fat *v, struct ofat_fat_file *f)
{
    memset(f, 0, sizeof(*f));
    f->dir = 1;
    if (v->bits == 32) f->first = v->root; else f->root = 1;
}

void ofat_fat_dirfile(const struct ofat_fat *v, ofat_u32 first, struct ofat_fat_file *f)
{
    if (!first) { ofat_fat_rootdir(v, f); return; }
    memset(f, 0, sizeof(*f));
    f->dir = 1;
    f->first = first;
}

/* ---- directories ---- */

static ofat_u32 first_of(const struct ofat_fat *v, const ofat_u8 *e)
{
    ofat_u32 c = rd16(e + 26);
    if (v->bits == 32) c |= (ofat_u32)rd16(e + 20) << 16;
    return c;
}

/* the 32-byte entry at pos of a directory (pointer into the buffer) */
static int dir_entry(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 pos, const ofat_u8 **e)
{
    ofat_u32 lba;
    const ofat_u8 *s;
    int r = file_sector(v, d, pos, &lba);
    if (r) return r;
    if (!(s = sector(v, lba))) return OFAT_FAT_EIO;
    *e = s + ((unsigned)pos & ((1u << v->sshift) - 1));
    return OFAT_FAT_OK;
}

static int put_entry(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 pos, const ofat_u8 *e)
{
    ofat_u32 lba;
    ofat_u8 *s;
    int r = file_sector(v, d, pos, &lba);
    if (r) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
    if (!(s = sector(v, lba))) return OFAT_FAT_EIO;
    memcpy(s + ((unsigned)pos & ((1u << v->sshift) - 1)), e, 32);
    return put(v, lba);
}

static ofat_u8 lfn_sum(const char *fcb)
{
    ofat_u8 sum = 0;
    unsigned i;
    for (i = 0; i < 11; ++i) sum = (ofat_u8)(((sum & 1) << 7) + (sum >> 1) + (ofat_u8)fcb[i]);
    return sum;
}

static const ofat_u8 lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static ofat_u16 oem_uni(const struct ofat_fat *v, ofat_u8 c)
{
    return c < 0x80 ? c : v->cp->uni[c - 0x80];
}

/* the display name of a short entry, with the NT case bits */
static unsigned short_name(const struct ofat_fat *v, const char *fcb, ofat_u8 ntcase, ofat_u16 *out)
{
    unsigned i, n = 0, len;
    ofat_u8 c;
    for (len = 8; len && fcb[len - 1] == ' '; --len) ;
    for (i = 0; i < len; ++i) {
        c = (ofat_u8)fcb[i];
        if (!i && c == 0x05) c = 0xE5;
        if ((ntcase & 0x08) && c >= 'A' && c <= 'Z') c = (ofat_u8)(c + 32);
        out[n++] = oem_uni(v, c);
    }
    for (len = 3; len && fcb[8 + len - 1] == ' '; --len) ;
    if (len) out[n++] = '.';
    for (i = 0; i < len; ++i) {
        c = (ofat_u8)fcb[8 + i];
        if ((ntcase & 0x10) && c >= 'A' && c <= 'Z') c = (ofat_u8)(c + 32);
        out[n++] = oem_uni(v, c);
    }
    return n;
}

int ofat_fat_dir_next(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 *pos, struct ofat_fat_ent *ent)
{
    const ofat_u8 *e;
    unsigned seq = 0, count = 0, i, k;
    ofat_u8 sum = 0;
    ofat_u32 lfnpos = 0;
    int r;
    for (;; *pos += 32) {
        if ((r = dir_entry(v, d, *pos, &e)) != 0) return r;
        if (!e[0]) return OFAT_FAT_END;
        if (e[0] == 0xE5) { seq = 0; continue; }
        if ((e[11] & 0x3F) == 0x0F) {                /* a long name part */
            k = e[0] & 0x1F;
            if (e[0] & 0x40) {
                if (!k || k > 20) { seq = 0; continue; }
                count = seq = k; sum = e[13]; lfnpos = *pos;
                memset(ent->name, 0, sizeof(ent->name));
            } else if (!seq || k != seq - 1 || e[13] != sum) { seq = 0; continue; }
            else seq = k;
            for (i = 0; i < 13; ++i)
                if ((k - 1) * 13 + i < 255) ent->name[(k - 1) * 13 + i] = rd16(e + lfn_at[i]);
                else if (rd16(e + lfn_at[i]) && rd16(e + lfn_at[i]) != 0xFFFF) seq = 0;
            continue;
        }
        if ((e[11] & OFAT_FAT_VOLUME) || (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' ')))) {
            seq = 0;
            continue;
        }
        memcpy(ent->fcb, e, 11);
        ent->pos = *pos;
        ent->attr = e[11];
        ent->ntcase = e[12];
        ent->ctime_cs = e[13];
        ent->ctime = rd16(e + 14);
        ent->cdate = rd16(e + 16);
        ent->adate = rd16(e + 18);
        ent->time = rd16(e + 22);
        ent->date = rd16(e + 24);
        memset(&ent->file, 0, sizeof(ent->file));
        ent->file.first = first_of(v, e);
        ent->file.size = rd32(e + 28);
        ent->file.dir = (ofat_u8)(e[11] & OFAT_FAT_DIR ? 1 : 0);
        if (ent->file.dir) ent->file.size = 0;
        if (seq == 1 && sum == lfn_sum(ent->fcb)) {
            for (i = 0; i < count * 13 && i < 255 && ent->name[i]; ++i) ;
            ent->name_len = (ofat_u8)i;
            ent->lfn = lfnpos;
        } else {
            ent->name_len = (ofat_u8)short_name(v, ent->fcb, ent->ntcase, ent->name);
            ent->lfn = *pos;
        }
        *pos += 32;
        return OFAT_FAT_OK;
    }
}

/* upper case for comparing names: ASCII and Latin-1 */
static ofat_u16 fold(ofat_u16 c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return (ofat_u16)(c - 32);
    if (c == 0xFF) return 0x178;
    return c;
}

static int same_name(const ofat_u16 *a, unsigned na, const ofat_u16 *b, unsigned nb)
{
    unsigned i;
    if (na != nb) return 0;
    for (i = 0; i < na; ++i) if (fold(a[i]) != fold(b[i])) return 0;
    return 1;
}

int ofat_fat_find(struct ofat_fat *v, struct ofat_fat_file *d, const ofat_u16 *name, unsigned len,
                  struct ofat_fat_ent *ent)
{
    static ofat_u16 sn[12];
    ofat_u32 pos = 0;
    unsigned n;
    int r;
    while (!(r = ofat_fat_dir_next(v, d, &pos, ent))) {
        if (same_name(ent->name, ent->name_len, name, len)) return OFAT_FAT_OK;
        n = short_name(v, ent->fcb, 0, sn);
        if (same_name(sn, n, name, len)) return OFAT_FAT_OK;
    }
    return r;
}

int ofat_fat_lookup(struct ofat_fat *v, struct ofat_fat_file *d, const char fcb[11], struct ofat_fat_ent *ent)
{
    ofat_u32 pos = 0;
    int r;
    while (!(r = ofat_fat_dir_next(v, d, &pos, ent)))
        if (!memcmp(ent->fcb, fcb, 11)) return OFAT_FAT_OK;
    return r;
}

/* ---- reading ---- */

int ofat_fat_read(struct ofat_fat *v, struct ofat_fat_file *f, ofat_u32 off, void OFAT_FAR *buf,
                  ofat_u16 len, ofat_u16 *done)
{
    ofat_u8 OFAT_FAR *out = (ofat_u8 OFAT_FAR *)buf;
    unsigned ssize = 1u << v->sshift, in, n;
    ofat_u32 lba;
    const ofat_u8 *s;
    int r;
    *done = 0;
    if (f->dir) return OFAT_FAT_EBAD;
    if (off >= f->size) return OFAT_FAT_OK;
    if (len > f->size - off) len = (ofat_u16)(f->size - off);
    while (len) {
        in = (unsigned)off & (ssize - 1);
        n = ssize - in;
        if (n > len) n = len;
        r = file_sector(v, f, off, &lba);
        if (r) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
        if (!in && n == ssize) {
            if (v->buf_lba == lba) v->buf_lba = NOBUF;
            if (v->read(v->dev, v->start + lba, out)) return OFAT_FAT_EIO;
        } else {
            if (!(s = sector(v, lba))) return OFAT_FAT_EIO;
            fmemcpy(out, s + in, n);
        }
        out += n; off += n; len = (ofat_u16)(len - n); *done = (ofat_u16)(*done + n);
    }
    return OFAT_FAT_OK;
}

int ofat_fat_free(struct ofat_fat *v, ofat_u32 *free_clusters)
{
    ofat_u32 c, val;
    int r;
    *free_clusters = 0;
    for (c = 2; c < v->clusters + 2; ++c) {
        if ((r = fat_get(v, c, &val)) != 0) return r;
        if (!val) ++*free_clusters;
    }
    v->free = *free_clusters;
    return OFAT_FAT_OK;
}

/* ---- writing ---- */

/* the dirty bit, before the first change */
static int begin(struct ofat_fat *v)
{
    ofat_u32 e1, bit = v->bits == 16 ? 0x8000UL : 0x08000000UL;
    int r;
    if (!v->write) return OFAT_FAT_EWRITE;
    if (v->dirty || v->bits == 12) { if (!v->dirty) v->dirty = 1; return OFAT_FAT_OK; }
    if ((r = fat_get(v, 1, &e1)) != 0) return r;
    if ((r = fat_set(v, 1, e1 & ~bit)) != 0) return r;
    v->dirty = 1;
    return OFAT_FAT_OK;
}

int ofat_fat_clean(struct ofat_fat *v)
{
    ofat_u32 e1, bit = v->bits == 16 ? 0x8000UL : 0x08000000UL;
    ofat_u8 *s;
    int r;
    if (v->dirty != 1) return OFAT_FAT_OK;
    if (v->fsinfo) {
        if (!(s = sector(v, v->fsinfo))) return OFAT_FAT_EIO;
        if (rd32(s) == 0x41615252UL && rd32(s + 484) == 0x61417272UL) {
            wr32(s + 488, v->free);
            wr32(s + 492, v->hint);
            if ((r = put(v, v->fsinfo)) != 0) return r;
        }
    }
    if (v->bits != 12) {
        if ((r = fat_get(v, 1, &e1)) != 0) return r;
        if ((r = fat_set(v, 1, e1 | bit)) != 0) return r;
    }
    v->dirty = 0;
    return OFAT_FAT_OK;
}

/* a free cluster, marked as the end of a chain */
static int alloc(struct ofat_fat *v, ofat_u32 *clu)
{
    ofat_u32 c = v->hint, n, val;
    int r;
    if (!valid(v, c)) c = 2;
    for (n = 0; n < v->clusters; ++n, ++c) {
        if (c > v->clusters + 1) c = 2;
        if ((r = fat_get(v, c, &val)) != 0) return r;
        if (!val) {
            if ((r = fat_set(v, c, eoc(v))) != 0) return r;
            if (v->free != NOBUF && v->free) --v->free;
            v->hint = c + 1 > v->clusters + 1 ? 2 : c + 1;
            *clu = c;
            return OFAT_FAT_OK;
        }
    }
    v->free = 0;
    return OFAT_FAT_EFULL;
}

static int free_chain(struct ofat_fat *v, ofat_u32 c)
{
    ofat_u32 next, n = 0;
    int r;
    while (valid(v, c) && n++ <= v->clusters) {
        if ((r = fat_get(v, c, &next)) != 0) return r;
        if ((r = fat_set(v, c, 0)) != 0) return r;
        if (v->free != NOBUF) ++v->free;
        c = next;
    }
    return OFAT_FAT_OK;
}

/* zeros over a whole cluster */
static int zero_cluster(struct ofat_fat *v, ofat_u32 c)
{
    ofat_u32 lba = cluster_lba(v, c), i;
    int r;
    for (i = 0; i < (1UL << v->cshift); ++i) {
        memset(v->buf, 0, 1u << v->sshift);
        v->buf_lba = lba + i;
        if ((r = put(v, lba + i)) != 0) return r;
    }
    return OFAT_FAT_OK;
}

/* the chain of f grown to `want` clusters; *have its length before */
static int grow(struct ofat_fat *v, struct ofat_fat_file *f, ofat_u32 want, int zero)
{
    ofat_u32 have = 0, last = 0, c, next;
    int r;
    if (f->first) {
        /* the last cluster and the length of the chain */
        r = walk(v, f, f->clu ? f->idx : 0, &last);
        if (r) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
        have = f->idx;
        for (;;) {
            if ((r = fat_get(v, last, &next)) != 0) return r;
            if (at_end(v, next)) break;
            if (!valid(v, next) || have > v->clusters) return OFAT_FAT_EBAD;
            last = next; ++have;
        }
        ++have;
        f->idx = have - 1; f->clu = last;
    }
    while (have < want) {
        if ((r = alloc(v, &c)) != 0) return r;
        if (zero && (r = zero_cluster(v, c)) != 0) return r;
        if (last) { if ((r = fat_set(v, last, c)) != 0) return r; }
        else f->first = c;
        last = c;
        f->idx = have++; f->clu = c;
    }
    return OFAT_FAT_OK;
}

/* the short entry of a file: first cluster and size */
static int put_size(struct ofat_fat *v, const struct ofat_fat_where *w, const struct ofat_fat_file *f)
{
    struct ofat_fat_file d;
    const ofat_u8 *e;
    int r;
    if (w->none) return OFAT_FAT_OK;
    ofat_fat_dirfile(v, w->dir, &d);
    if ((r = dir_entry(v, &d, w->pos, &e)) != 0) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
    memcpy(se, e, 32);
    wr16(se + 26, (ofat_u16)f->first);
    if (v->bits == 32) wr16(se + 20, (ofat_u16)(f->first >> 16));
    if (!f->dir) wr32(se + 28, f->size);
    return put_entry(v, &d, w->pos, se);
}

/* data at off, from in or zeros; the chain must reach off + len */
static int put_data(struct ofat_fat *v, struct ofat_fat_file *f, ofat_u32 off,
                    const ofat_u8 OFAT_FAR *in, ofat_u32 len)
{
    unsigned ssize = 1u << v->sshift, at, n;
    ofat_u32 lba;
    ofat_u8 *s;
    int r;
    while (len) {
        at = (unsigned)off & (ssize - 1);
        n = ssize - at;
        if (n > len) n = (unsigned)len;
        if ((r = file_sector(v, f, off, &lba)) != 0) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
        if (!at && n == ssize && in) {
            if (v->buf_lba == lba) v->buf_lba = NOBUF;
            if (v->write(v->dev, v->start + lba, in)) return OFAT_FAT_EWRITE;
        } else {
            if (n == ssize) { memset(v->buf, 0, ssize); v->buf_lba = lba; s = v->buf; }
            else if (!(s = sector(v, lba))) return OFAT_FAT_EIO;
            if (in) fmemcpy(s + at, in, n); else memset(s + at, 0, n);
            if ((r = put(v, lba)) != 0) return r;
        }
        if (in) in += n;
        off += n; len -= n;
    }
    return OFAT_FAT_OK;
}

/* writes, growing the file; in NULL: zeros */
static int extend(struct ofat_fat *v, struct ofat_fat_file *f, const struct ofat_fat_where *w,
                  ofat_u32 off, const ofat_u8 OFAT_FAR *in, ofat_u32 len, ofat_u32 *done)
{
    ofat_u32 end = off + len, avail;
    int r, full = 0;
    *done = 0;
    if (end < off) return OFAT_FAT_EFULL;                   /* 4 GB */
    if ((r = begin(v)) != 0) return r;
    r = grow(v, f, clusters_for(v, end), 0);
    if (r == OFAT_FAT_EFULL) {
        /* as much as there is room for */
        full = 1;
        avail = f->first ? (f->idx + 1) << CB(v) : 0;
        if (avail < end) end = avail;
    } else if (r) return r;
    if (off > f->size && end > f->size) {
        if ((r = put_data(v, f, f->size, NULL, (off < end ? off : end) - f->size)) != 0) return r;
    }
    if (end > off) {
        if ((r = put_data(v, f, off, in, end - off)) != 0) return r;
        *done = end - off;
    }
    if (end > f->size) f->size = end;
    if ((r = put_size(v, w, f)) != 0) return r;
    return full ? OFAT_FAT_EFULL : OFAT_FAT_OK;
}

void ofat_fat_where_of(const struct ofat_fat_file *dir, ofat_u32 pos, struct ofat_fat_where *w)
{
    w->dir = dir->root ? 0 : dir->first;
    w->pos = pos;
    w->none = 0;
}

int ofat_fat_write(struct ofat_fat *v, struct ofat_fat_file *f, const struct ofat_fat_where *w,
                   ofat_u32 off, const void OFAT_FAR *buf, ofat_u16 len, ofat_u16 *done)
{
    ofat_u32 n;
    int r;
    *done = 0;
    if (f->dir) return OFAT_FAT_EBAD;
    if (!len) return OFAT_FAT_OK;
    r = extend(v, f, w, off, (const ofat_u8 OFAT_FAR *)buf, len, &n);
    *done = (ofat_u16)n;
    return r;
}

int ofat_fat_resize(struct ofat_fat *v, struct ofat_fat_file *f, const struct ofat_fat_where *w, ofat_u32 size)
{
    ofat_u32 keep, last, rest, n;
    int r;
    if (f->dir) return OFAT_FAT_EBAD;
    if (size > f->size) return extend(v, f, w, size, NULL, 0, &n);
    if (size == f->size) return OFAT_FAT_OK;
    if ((r = begin(v)) != 0) return r;
    keep = clusters_for(v, size);
    rest = f->first;
    if (keep) {
        if ((r = walk(v, f, keep - 1, &last)) != 0) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
        if ((r = fat_get(v, last, &rest)) != 0) return r;
    }
    f->size = size;
    if (!keep) { f->first = 0; f->clu = 0; }
    /* the entry first, then the clusters: at worst lost ones */
    if ((r = put_size(v, w, f)) != 0) return r;
    if (keep && !at_end(v, rest)) {
        if ((r = fat_set(v, last, eoc(v))) != 0) return r;
        return free_chain(v, rest);
    }
    return keep ? OFAT_FAT_OK : free_chain(v, rest);
}

/* ---- creating, removing, renaming ---- */

static int bad_char(ofat_u16 c)
{
    return c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
        c == '>' || c == '?' || c == '\\' || c == '|';
}

/* the short name of a new entry: the name itself (ntcase for lower case
   ones) or a basis with "~n" (1 is returned: it needs a long name) */
static int make_short(struct ofat_fat *v, struct ofat_fat_file *d, const ofat_u16 *name, unsigned len,
                      char fcb[11], ofat_u8 *ntcase)
{
    unsigned i, dot = len, k, keep, tl;
    int lower[2] = { 0, 0 }, upper[2] = { 0, 0 }, part = 0;
    char tail[8];
    ofat_u32 n, m;
    *ntcase = 0;
    if (!ofat_cp_basis(v->cp, name, len, fcb)) {
        for (i = len; i; --i) if (name[i - 1] == '.') { dot = i - 1; break; }
        for (i = 0; i < len; ++i) {
            if (i == dot) { part = 1; continue; }
            if (name[i] >= 0x80) { lower[0] = upper[0] = 1; break; }
            if (name[i] >= 'a' && name[i] <= 'z') lower[part] = 1;
            if (name[i] >= 'A' && name[i] <= 'Z') upper[part] = 1;
        }
        if (!(lower[0] && upper[0]) && !(lower[1] && upper[1])) {
            if (lower[0]) *ntcase |= 0x08;
            if (lower[1]) *ntcase |= 0x10;
            return 0;
        }
    }
    /* "~n": the first n that no entry of the directory has */
    for (n = 1; n < 1000000UL; ++n) {
        char want[11];
        ofat_u32 pos = 0;
        int r;
        memcpy(want, fcb, 11);
        tl = sizeof(tail);
        m = n;
        do { tail[--tl] = (char)('0' + m % 10); m /= 10; } while (m);
        tail[--tl] = '~';
        for (k = 8; k && want[k - 1] == ' '; --k) ;
        keep = 8 - (sizeof(tail) - tl);
        if (k > keep) k = keep;
        memcpy(want + k, tail + tl, sizeof(tail) - tl);
        for (i = k + (unsigned)(sizeof(tail) - tl); i < 8; ++i) want[i] = ' ';
        while (!(r = ofat_fat_dir_next(v, d, &pos, &tmp)))
            if (!memcmp(tmp.fcb, want, 11)) break;
        if (r == OFAT_FAT_END) { memcpy(fcb, want, 11); return 1; }
        if (r) return r;
    }
    return OFAT_FAT_EFULL;
}

/* room for n entries in a row: *pos the first; the directory grows */
static int place(struct ofat_fat *v, struct ofat_fat_file *d, unsigned n, ofat_u32 *pos)
{
    const ofat_u8 *e;
    ofat_u32 p = 0, run = 0;
    int r;
    for (;; p += 32) {
        r = dir_entry(v, d, p, &e);
        if (r == OFAT_FAT_END) break;
        if (r) return r;
        if (!e[0] || e[0] == 0xE5) {
            if (!run) *pos = p;
            if (++run == n) return OFAT_FAT_OK;
        } else run = 0;
    }
    if (!run) *pos = p;
    if (d->root || *pos + 32UL * n > 0x200000UL) return OFAT_FAT_EFULL;   /* 65536 entries */
    /* new zeroed clusters at the end (a directory's chain is never empty) */
    return grow(v, d, clusters_for(v, *pos + 32UL * n), 1);
}

/* writes the entry set: long name parts, then the short entry se */
static int put_set(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 pos,
                   const ofat_u16 *name, unsigned len, unsigned parts)
{
    ofat_u8 e[32];
    ofat_u8 sum = lfn_sum((const char *)se);
    unsigned k, i, j;
    ofat_u16 c;
    int r;
    for (k = parts; k; --k, pos += 32) {
        memset(e, 0, 32);
        e[0] = (ofat_u8)(k | (k == parts ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (i = 0; i < 13; ++i) {
            j = (k - 1) * 13 + i;
            c = j < len ? name[j] : j == len ? 0 : 0xFFFF;
            wr16(e + lfn_at[i], c);
        }
        if ((r = put_entry(v, d, pos, e)) != 0) return r;
    }
    return put_entry(v, d, pos, se);
}

static int check_name(const ofat_u16 *name, unsigned len)
{
    unsigned i;
    if (!len || len > 255 || name[len - 1] == '.' || name[len - 1] == ' ') return OFAT_FAT_ENAME;
    for (i = 0; i < len; ++i) if (bad_char(name[i])) return OFAT_FAT_ENAME;
    return OFAT_FAT_OK;
}

/* a new entry set for name in d with the short entry se (name and case
   bits are filled in); *pos the short entry */
static int add(struct ofat_fat *v, struct ofat_fat_file *d, const ofat_u16 *name, unsigned len, ofat_u32 *pos)
{
    char fcb[11];
    ofat_u8 ntcase;
    unsigned parts;
    int r;
    if ((r = make_short(v, d, name, len, fcb, &ntcase)) < 0) return r;
    parts = r ? (len + 12) / 13 : 0;
    memcpy(se, fcb, 11);
    if ((ofat_u8)se[0] == 0xE5) se[0] = 0x05;
    se[12] = ntcase;
    if ((r = place(v, d, parts + 1, pos)) != 0) return r;
    if ((r = put_set(v, d, *pos, name, len, parts)) != 0) return r;
    *pos += 32UL * parts;
    return OFAT_FAT_OK;
}

static int exists(struct ofat_fat *v, struct ofat_fat_file *d, const ofat_u16 *name, unsigned len)
{
    int r = ofat_fat_find(v, d, name, len, &tmp);
    return r == OFAT_FAT_END ? OFAT_FAT_OK : r ? r : OFAT_FAT_EEXIST;
}

int ofat_fat_create(struct ofat_fat *v, struct ofat_fat_file *d, const ofat_u16 *name, unsigned len,
                    ofat_u8 attr, ofat_u16 time, ofat_u16 date, ofat_u32 *pos)
{
    ofat_u8 dot[32];
    ofat_u32 c = 0, parent = d->root ? 0 : d->first;
    struct ofat_fat_file nd;
    int r;
    if ((r = check_name(name, len)) != 0) return r;
    if ((r = exists(v, d, name, len)) != 0) return r;
    if ((r = begin(v)) != 0) return r;
    if (v->bits == 32 && parent == v->root) parent = 0;
    if (attr & OFAT_FAT_DIR) {
        /* the new directory with "." and ".." */
        if ((r = alloc(v, &c)) != 0) return r;
        if ((r = zero_cluster(v, c)) != 0) return r;
        ofat_fat_dirfile(v, c, &nd);
        memset(dot, 0, 32);
        memset(dot, ' ', 11);
        dot[0] = '.';
        dot[11] = OFAT_FAT_DIR;
        wr16(dot + 14, time); wr16(dot + 16, date); wr16(dot + 18, date);
        wr16(dot + 22, time); wr16(dot + 24, date);
        wr16(dot + 26, (ofat_u16)c);
        if (v->bits == 32) wr16(dot + 20, (ofat_u16)(c >> 16));
        if ((r = put_entry(v, &nd, 0, dot)) != 0) return r;
        dot[1] = '.';
        wr16(dot + 26, (ofat_u16)parent);
        wr16(dot + 20, v->bits == 32 ? (ofat_u16)(parent >> 16) : 0);
        if ((r = put_entry(v, &nd, 32, dot)) != 0) return r;
    }
    memset(se, 0, 32);
    se[11] = (ofat_u8)(attr & ~OFAT_FAT_VOLUME);
    wr16(se + 14, time); wr16(se + 16, date); wr16(se + 18, date);
    wr16(se + 22, time); wr16(se + 24, date);
    wr16(se + 26, (ofat_u16)c);
    if (v->bits == 32) wr16(se + 20, (ofat_u16)(c >> 16));
    r = add(v, d, name, len, pos);
    if (r && c) free_chain(v, c);
    return r;
}

/* the entry set whose short entry is at pos, in tmp */
static int entry_at(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 pos)
{
    ofat_u32 p = pos >= 20 * 32 + 32 ? pos - 20 * 32 : 0;
    int r;
    /* from up to 20 entries before: a long name has at most 20 parts */
    for (;;) {
        r = ofat_fat_dir_next(v, d, &p, &tmp);
        if (r) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
        if (tmp.pos == pos) return OFAT_FAT_OK;
        if (tmp.pos > pos) return OFAT_FAT_EBAD;
    }
}

/* marks the entries of tmp's set deleted, the short one first */
static int unmark(struct ofat_fat *v, struct ofat_fat_file *d)
{
    const ofat_u8 *e;
    ofat_u32 p = tmp.pos, end = tmp.pos;
    int r;
    for (;;) {
        if ((r = dir_entry(v, d, p, &e)) != 0) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
        memcpy(se, e, 32);
        se[0] = 0xE5;
        if ((r = put_entry(v, d, p, se)) != 0) return r;
        p = p == end ? tmp.lfn : p + 32;
        if (p == end) return OFAT_FAT_OK;
    }
}

int ofat_fat_remove(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 pos)
{
    ofat_u32 c;
    int r;
    if ((r = entry_at(v, d, pos)) != 0) return r;
    if ((r = begin(v)) != 0) return r;
    c = tmp.file.first;
    if ((r = unmark(v, d)) != 0) return r;
    return free_chain(v, c);
}

int ofat_fat_empty(struct ofat_fat *v, struct ofat_fat_file *d)
{
    ofat_u32 pos = 0;
    int r = ofat_fat_dir_next(v, d, &pos, &tmp);
    return r == OFAT_FAT_END ? 1 : r ? r : 0;
}

int ofat_fat_rename(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 pos,
                    struct ofat_fat_file *to, const ofat_u16 *name, unsigned len, ofat_u32 *newpos)
{
    static ofat_u8 old[32];
    struct ofat_fat_file moved;
    ofat_u32 parent, lfn;
    const ofat_u8 *e;
    int r, same = (d->root && to->root) || (!d->root && !to->root && d->first == to->first);
    if ((r = check_name(name, len)) != 0) return r;
    r = ofat_fat_find(v, to, name, len, &tmp);
    if (!r && !(same && tmp.pos == pos)) return OFAT_FAT_EEXIST;
    if (r && r != OFAT_FAT_END) return r;
    if ((r = entry_at(v, d, pos)) != 0) return r;
    lfn = tmp.lfn;
    if ((r = begin(v)) != 0) return r;
    if ((r = dir_entry(v, d, pos, &e)) != 0) return r;
    memcpy(old, e, 32);
    /* the new set first, then the old one removed: at worst both stay */
    memcpy(se, old, 32);
    if ((r = add(v, to, name, len, newpos)) != 0) return r;
    tmp.pos = pos; tmp.lfn = lfn;
    if ((r = unmark(v, d)) != 0) return r;
    if (!same) {
        if (old[11] & OFAT_FAT_DIR) {
            /* ".." of a moved directory */
            parent = to->root ? 0 : to->first;
            if (v->bits == 32 && parent == v->root) parent = 0;
            ofat_fat_dirfile(v, first_of(v, old), &moved);
            if ((r = dir_entry(v, &moved, 32, &e)) != 0) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
            memcpy(se, e, 32);
            if (se[0] == '.' && se[1] == '.') {
                wr16(se + 26, (ofat_u16)parent);
                if (v->bits == 32) wr16(se + 20, (ofat_u16)(parent >> 16));
                if ((r = put_entry(v, &moved, 32, se)) != 0) return r;
            }
        }
    }
    return OFAT_FAT_OK;
}

static int change(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 pos)
{
    const ofat_u8 *e;
    int r = begin(v);
    if (r) return r;
    if ((r = dir_entry(v, d, pos, &e)) != 0) return r == OFAT_FAT_END ? OFAT_FAT_EBAD : r;
    memcpy(se, e, 32);
    return OFAT_FAT_OK;
}

int ofat_fat_attr(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 pos, ofat_u8 attr)
{
    int r = change(v, d, pos);
    if (r) return r;
    se[11] = (ofat_u8)((se[11] & (OFAT_FAT_DIR | OFAT_FAT_VOLUME)) |
                       (attr & ~(OFAT_FAT_DIR | OFAT_FAT_VOLUME)));
    return put_entry(v, d, pos, se);
}

int ofat_fat_time(struct ofat_fat *v, struct ofat_fat_file *d, ofat_u32 pos, unsigned which,
                  ofat_u16 time, ofat_u16 date, ofat_u8 cs10)
{
    int r = change(v, d, pos);
    if (r) return r;
    if (which == OFAT_FAT_ACCESSED) wr16(se + 18, date);
    else {
        wr16(se + which, time);
        wr16(se + which + 2, date);
        if (which == OFAT_FAT_CREATED) se[13] = cs10;
    }
    return put_entry(v, d, pos, se);
}
