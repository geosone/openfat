/* exFAT reading and writing: boot sector, cluster chains, directory entry
   sets, file data and 8.3 names, after the Microsoft exFAT file system
   specification. */
#include <string.h>
#include "openfat/exfat.h"

#ifdef __WATCOMC__
#define fmemcpy _fmemcpy
#define fmemset _fmemset
#else
#define fmemcpy memcpy
#define fmemset memset
#endif

#define NOBUF ((ofat_lba)~(ofat_lba)0)    /* no sector in the buffer */
#define NOCOUNT 0xFFFFFFFFUL                /* free clusters not counted */
#define EOC 0xFFFFFFFFUL

/* walk cache of the allocation bitmap */
static struct ofat_exfat *bm_owner;
static struct ofat_exfat_file bm_file;

static ofat_u16 rd16(const ofat_u8 OFAT_FAR *p) { return (ofat_u16)(p[0] | p[1] << 8); }
static ofat_u32 rd32(const ofat_u8 OFAT_FAR *p) { return rd16(p) | (ofat_u32)rd16(p+2) << 16; }
static void wr16(ofat_u8 *p, ofat_u16 v) { p[0] = (ofat_u8)v; p[1] = (ofat_u8)(v >> 8); }
static void wr32(ofat_u8 *p, ofat_u32 v) { wr16(p, (ofat_u16)v); wr16(p+2, (ofat_u16)(v >> 16)); }

/* 64-bit lengths of the stream entry: whole, or cut at 4 GB when narrow */
static ofat_fsize rdsize(const ofat_u8 *p)
{
#ifdef OFAT_NARROW
    return rd32(p+4) ? 0xFFFFFFFFUL : rd32(p);
#else
    return rd32(p) | (ofat_fsize)rd32(p+4) << 32;
#endif
}

static void wrsize(ofat_u8 *p, ofat_fsize v)
{
    wr32(p, (ofat_u32)v);
#ifdef OFAT_NARROW
    wr32(p+4, 0);
#else
    wr32(p+4, (ofat_u32)(v >> 32));
#endif
}

static const ofat_u8 *sector(struct ofat_exfat *v, ofat_lba lba)
{
    if (v->buf_lba != lba) {
        v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + lba, v->buf)) return 0;
        v->buf_lba = lba;
    }
    return v->buf;
}

int ofat_exfat_probe(const ofat_u8 OFAT_FAR *b)
{
    unsigned i;
    if (b[3] != 'E' || b[4] != 'X' || b[5] != 'F' || b[6] != 'A' || b[7] != 'T' ||
        b[8] != ' ' || b[9] != ' ' || b[10] != ' ' || b[510] != 0x55 || b[511] != 0xAA) return 0;
    for (i = 11; i < 64; ++i) if (b[i]) return 0;
    return 1;
}

/* The FAT entry of a cluster: the next cluster, or 0 on a read error. */
static ofat_u32 fat_next(struct ofat_exfat *v, ofat_u32 clu)
{
    const ofat_u8 *s = sector(v, v->fat + (clu >> (v->sshift - 2)));
    if (!s) return 0;
    return rd32(s + ((unsigned)(clu << 2) & ((1u << v->sshift) - 1)));
}

/* The cluster at index idx of a chain: 0 ok, OFAT_EXFAT_END past its end. */
static int walk(struct ofat_exfat *v, struct ofat_exfat_file *f, ofat_u32 idx, ofat_u32 *clu)
{
    ofat_u32 next;
    if (f->first < 2 || f->first >= v->clusters + 2) return OFAT_EXFAT_EBAD;
    if (f->nofat) {
        if (idx >= v->clusters + 2 - f->first) return OFAT_EXFAT_EBAD;
        *clu = f->first + idx;
        return OFAT_EXFAT_OK;
    }
    if (idx < f->idx || f->clu < 2) { f->idx = 0; f->clu = f->first; }
    while (f->idx < idx) {
        next = fat_next(v, f->clu);
        if (!next) return OFAT_EXFAT_EIO;
        if (next == 0xFFFFFFFFUL) return f->root ? OFAT_EXFAT_END : OFAT_EXFAT_EBAD;
        if (next < 2 || next >= v->clusters + 2) return OFAT_EXFAT_EBAD;
        f->clu = next; ++f->idx;
    }
    *clu = f->clu;
    return OFAT_EXFAT_OK;
}

/* The sector holding byte off of a file or directory. */
static int file_sector(struct ofat_exfat *v, struct ofat_exfat_file *f, ofat_fsize off, ofat_lba *lba)
{
    ofat_u32 clu;
    unsigned shift = v->sshift + v->cshift;
    int r = walk(v, f, (ofat_u32)(off >> shift), &clu);
    if (r) return r;
    *lba = v->heap + ((ofat_lba)(clu - 2) << v->cshift) + (ofat_u32)((off >> v->sshift) & ((1UL << v->cshift) - 1));
    return OFAT_EXFAT_OK;
}

/* The 32-byte directory entry at byte pos of a directory. */
static int dir_entry(struct ofat_exfat *v, struct ofat_exfat_file *d, ofat_u32 pos, const ofat_u8 **e)
{
    ofat_lba lba;
    const ofat_u8 *s;
    int r;
    if (!d->root && pos >= d->size) return OFAT_EXFAT_END;
    r = file_sector(v, d, pos, &lba);
    if (r) return r;
    if (!(s = sector(v, lba))) return OFAT_EXFAT_EIO;
    *e = s + ((unsigned)pos & ((1u << v->sshift) - 1));
    return OFAT_EXFAT_OK;
}

void ofat_exfat_rootdir(const struct ofat_exfat *v, struct ofat_exfat_file *f)
{
    memset(f, 0, sizeof(*f));
    f->first = v->root; f->root = 1;
}

int ofat_exfat_mount(struct ofat_exfat *v, ofat_exfat_read_fn read, void *dev, ofat_lba start, const struct ofat_cp *cp, ofat_u8 *buf)
{
    const ofat_u8 *b;
    struct ofat_exfat_file root;
    ofat_u32 pos;
    ofat_lba len;
    unsigned i, active;
    int r;
    memset(v, 0, sizeof(*v));
    v->read = read; v->dev = dev; v->start = start; v->cp = cp; v->buf = buf; v->buf_lba = NOBUF;
    v->free = NOCOUNT;
    bm_owner = 0;
    if (read(dev, start, v->buf)) return OFAT_EXFAT_EIO;
    b = v->buf;
    if (!ofat_exfat_probe(b)) return OFAT_EXFAT_EBAD;
    v->sshift = b[108]; v->cshift = b[109];
    if (v->sshift < 9 || v->sshift > 12 || v->cshift > 25 - v->sshift) return OFAT_EXFAT_EBAD;
    if ((1u << v->sshift) > OFAT_EXFAT_MAXSECT) return OFAT_EXFAT_ESIZE;
    len = (ofat_lba)rdsize(b+72);           /* VolumeLength */
    v->fat = rd32(b+80); v->heap = rd32(b+88); v->clusters = rd32(b+92);
    v->root = rd32(b+96); v->serial = rd32(b+100); v->flags = rd16(b+106);
    if (b[110] != 1 && b[110] != 2) return OFAT_EXFAT_EBAD;
    /* TexFAT: two FATs and bitmaps, ActiveFat names the ones in use */
    active = b[110] == 2 ? v->flags & 1 : 0;
    if (b[110] == 2) v->fat2 = active ? v->fat : v->fat + rd32(b+84);
    if (active) v->fat += rd32(b+84);
    if (v->flags & 2) v->dirty = 2;     /* not ours to clear */
    if (v->fat < 24 || v->heap < v->fat || !v->clusters || v->clusters > 0xFFFFFFF5UL ||
        v->heap + ((ofat_lba)v->clusters << v->cshift) > len ||
        rd32(b+84) < ((v->clusters + 2) >> (v->sshift - 2)) ||
        v->root < 2 || v->root >= v->clusters + 2) return OFAT_EXFAT_EBAD;
    /* the root directory: allocation bitmap and volume label */
    ofat_exfat_rootdir(v, &root);
    for (pos = 0; ; pos += 32) {
        r = dir_entry(v, &root, pos, &b);
        if (r == OFAT_EXFAT_END) break;
        if (r) return r;
        if (!b[0]) break;
        if (b[0] == 0x81) {
            if ((b[1] & 1) == active) v->bitmap = rd32(b+20);
            else v->bitmap2 = rd32(b+20);
        }
        if (b[0] == 0x82) { v->upcase = rd32(b+20); v->upcase_len = rd32(b+24); }
        if (b[0] == 0x83) {
            v->label_len = b[1] > 11 ? 11 : b[1];
            for (i = 0; i < v->label_len; ++i) v->label[i] = rd16(b + 2 + 2*i);
        }
    }
    if (v->bitmap < 2 || v->bitmap >= v->clusters + 2) return OFAT_EXFAT_EBAD;
    if (!v->fat2 || v->bitmap2 < 2 || v->bitmap2 >= v->clusters + 2) v->fat2 = v->bitmap2 = 0;
    v->hint = 2;
    return OFAT_EXFAT_OK;
}

static void stream_file(const ofat_u8 *e, struct ofat_exfat_file *f)
{
    memset(f, 0, sizeof(*f));
    f->nofat = (e[1] & 2) != 0;
    f->first = rd32(e+20);
    f->size = rdsize(e+24); f->valid = rdsize(e+8);
#ifdef OFAT_NARROW
    if (rd32(e+28)) f->big = 1;
#endif
    if (f->valid > f->size) f->valid = f->size;
}

static ofat_u16 checksum(ofat_u16 sum, const ofat_u8 *e, int first)
{
    unsigned i;
    for (i = 0; i < 32; ++i) {
        if (first && (i == 2 || i == 3)) continue;
        sum = (ofat_u16)(((sum & 1) ? 0x8000u : 0) + (sum >> 1) + e[i]);
    }
    return sum;
}

int ofat_exfat_dir_next(struct ofat_exfat *v, struct ofat_exfat_file *d, ofat_u32 *pos, struct ofat_exfat_ent *ent)
{
    const ofat_u8 *e;
    ofat_u16 sum, want;
    unsigned n, i, k, names;
    int r;
    for (;; *pos += 32) {
        r = dir_entry(v, d, *pos, &e);
        if (r) return r;
        if (!e[0]) return OFAT_EXFAT_END;
        if (e[0] != 0x85) continue;
        n = e[1];
        if (n < 2 || n > 18) continue;
        ent->pos = *pos;
        ent->attr = rd16(e+4);
        ent->time = rd16(e+12); ent->date = rd16(e+14);
        want = rd16(e+2);
        sum = checksum(0, e, 1);
        ent->name_len = 0; names = 0;
        for (i = 1; i <= n; ++i) {
            r = dir_entry(v, d, *pos + 32UL*i, &e);
            if (r == OFAT_EXFAT_END) return OFAT_EXFAT_EBAD;
            if (r) return r;
            sum = checksum(sum, e, 0);
            if (i == 1) {
                if (e[0] != 0xC0) break;
                stream_file(e, &ent->file);
                ent->name_len = e[3];
            } else if (e[0] == 0xC1) {
                for (k = 0; k < 15 && names < ent->name_len; ++k) ent->name[names++] = rd16(e + 2 + 2*k);
            } else if (!(e[0] & 0x80) || e[0] < 0xE0) break;
        }
        if (i <= n || sum != want || !ent->name_len || names != ent->name_len) continue;
        *pos += 32UL * (n + 1);
        return OFAT_EXFAT_OK;
    }
}

int ofat_exfat_read(struct ofat_exfat *v, struct ofat_exfat_file *f, ofat_fsize off, void OFAT_FAR *buf, ofat_u16 len, ofat_u16 *done)
{
    ofat_u8 OFAT_FAR *out = (ofat_u8 OFAT_FAR *)buf;
    unsigned ssize = 1u << v->sshift, in, n;
    ofat_lba lba;
    const ofat_u8 *s;
    int r;
    *done = 0;
    if (off >= f->size) return OFAT_EXFAT_OK;
    if (len > f->size - off) len = (ofat_u16)(f->size - off);
    while (len) {
        in = (unsigned)off & (ssize - 1);
        n = ssize - in;
        if (n > len) n = len;
        if (off >= f->valid) {
            fmemset(out, 0, n);
        } else {
            if (f->valid - off < n) n = (unsigned)(f->valid - off);
            r = file_sector(v, f, off, &lba);
            if (r) return r == OFAT_EXFAT_END ? OFAT_EXFAT_EBAD : r;
            if (!in && n == ssize) {
                if (v->buf_lba == lba) v->buf_lba = NOBUF;
                if (v->read(v->dev, v->start + lba, out)) return OFAT_EXFAT_EIO;
            } else {
                if (!(s = sector(v, lba))) return OFAT_EXFAT_EIO;
                fmemcpy(out, s + in, n);
            }
        }
        out += n; off += n; len -= n; *done += n;
    }
    return OFAT_EXFAT_OK;
}

int ofat_exfat_free(struct ofat_exfat *v, ofat_u32 *free_clusters)
{
    struct ofat_exfat_file bm;
    ofat_u32 bit;
    ofat_lba lba;
    const ofat_u8 *s;
    unsigned bits = 8u << v->sshift, i;
    ofat_u8 c;
    int r;
    memset(&bm, 0, sizeof(bm));
    bm.first = v->bitmap;
    *free_clusters = 0;
    for (bit = 0; bit < v->clusters; bit += bits) {
        r = file_sector(v, &bm, bit >> 3, &lba);
        if (r) return r == OFAT_EXFAT_END ? OFAT_EXFAT_EBAD : r;
        if (!(s = sector(v, lba))) return OFAT_EXFAT_EIO;
        for (i = 0; i < bits && bit + i < v->clusters; ++i) {
            c = s[i >> 3];
            if (!(c & (1 << (i & 7)))) ++*free_clusters;
        }
    }
    v->free = *free_clusters;
    return OFAT_EXFAT_OK;
}

static struct ofat_exfat_ent other;

int ofat_exfat_short(struct ofat_exfat *v, struct ofat_exfat_file *d, const struct ofat_exfat_ent *ent, char fcb[11])
{
    char basis[11], tail[8];
    ofat_u32 pos = 0, n = 1;
    unsigned len, keep, t;
    int r;
    if (!ofat_cp_basis(v->cp, ent->name, ent->name_len, fcb)) return OFAT_EXFAT_OK;
    /* the n-th entry with this basis in the directory gets "~n" */
    while (pos < ent->pos) {
        r = ofat_exfat_dir_next(v, d, &pos, &other);
        if (r == OFAT_EXFAT_END) break;
        if (r) return r;
        if (other.pos < ent->pos && ofat_cp_basis(v->cp, other.name, other.name_len, basis) &&
            !memcmp(basis, fcb, 11)) ++n;
    }
    t = sizeof(tail);
    do { tail[--t] = (char)('0' + n % 10); n /= 10; } while (n);
    tail[--t] = '~';
    for (len = 8; len && fcb[len-1] == ' '; --len) ;
    keep = 8 - (sizeof(tail) - t);
    if (len > keep) len = keep;
    memcpy(fcb + len, tail + t, sizeof(tail) - t);
    return OFAT_EXFAT_OK;
}

int ofat_exfat_lookup(struct ofat_exfat *v, struct ofat_exfat_file *d, const char fcb[11], struct ofat_exfat_ent *ent)
{
    char name[11];
    ofat_u32 pos = 0, next;
    unsigned tilde;
    int r;
    for (tilde = 0; tilde < 8 && fcb[tilde] != '~'; ++tilde) ;
    for (;;) {
        r = ofat_exfat_dir_next(v, d, &pos, ent);
        if (r) return r;
        if (!ofat_cp_basis(v->cp, ent->name, ent->name_len, name)) {
            if (!memcmp(name, fcb, 11)) return OFAT_EXFAT_OK;
            continue;
        }
        /* a changed name: only one with the same start and extension can be it */
        if (tilde == 8 || memcmp(name, fcb, tilde) || memcmp(name + 8, fcb + 8, 3)) continue;
        next = pos;
        r = ofat_exfat_short(v, d, ent, name);
        if (r) return r;
        if (!memcmp(name, fcb, 11)) return OFAT_EXFAT_OK;
        pos = next;
    }
}

/* ---- writing ---- */

#define CB(v) ((v)->sshift + (v)->cshift)       /* bytes per cluster, a shift */

static int flush(struct ofat_exfat *v)
{
    return v->write(v->dev, v->start + v->buf_lba, v->buf) ? OFAT_EXFAT_EWRITE : OFAT_EXFAT_OK;
}

/* the cached sector also at lba: the TexFAT copy */
static int mirror(struct ofat_exfat *v, ofat_lba lba)
{
    return v->write(v->dev, v->start + lba, v->buf) ? OFAT_EXFAT_EWRITE : OFAT_EXFAT_OK;
}

static int bad(int r) { return r == OFAT_EXFAT_END ? OFAT_EXFAT_EBAD : r; }

/* VolumeDirty before the first change */
static int begin(struct ofat_exfat *v)
{
    ofat_u8 *s;
    if (!v->write) return OFAT_EXFAT_EWRITE;
    if (v->dirty) return OFAT_EXFAT_OK;
    if (!(s = (ofat_u8 *)sector(v, 0))) return OFAT_EXFAT_EIO;
    s[106] |= 2;
    v->dirty = 1;
    return flush(v);
}

int ofat_exfat_clean(struct ofat_exfat *v)
{
    ofat_u32 used;
    ofat_u8 *s;
    if (v->dirty != 1) return OFAT_EXFAT_OK;
    if (!(s = (ofat_u8 *)sector(v, 0))) return OFAT_EXFAT_EIO;
    s[106] &= ~2;
    if (v->free != NOCOUNT) {
        used = v->clusters - v->free;
        s[112] = (ofat_u8)(v->clusters > 0x1000000UL ? (used >> 8) * 100 / (v->clusters >> 8)
                                                   : used * 100 / v->clusters);
    }
    v->dirty = 0;
    return flush(v);
}

/* the sector of a bit of an allocation bitmap */
static int bitmap_lba(struct ofat_exfat *v, ofat_u32 first, ofat_u32 bit, ofat_lba *lba)
{
    if (bm_owner != v || bm_file.first != first) {
        memset(&bm_file, 0, sizeof(bm_file));
        bm_file.first = first; bm_owner = v;
    }
    return bad(file_sector(v, &bm_file, bit >> 3, lba));
}

static int set_bit(struct ofat_exfat *v, ofat_u32 clu, int on)
{
    ofat_u32 bit = clu - 2;
    ofat_lba lba, lba2 = 0;
    ofat_u8 *s, m = (ofat_u8)(1 << (bit & 7));
    unsigned at = (unsigned)(bit >> 3) & ((1u << v->sshift) - 1);
    int r;
    if (v->bitmap2 && (r = bitmap_lba(v, v->bitmap2, bit, &lba2)) != 0) return r;
    if ((r = bitmap_lba(v, v->bitmap, bit, &lba)) != 0) return r;
    if (!(s = (ofat_u8 *)sector(v, lba))) return OFAT_EXFAT_EIO;
    if (on) s[at] |= m; else s[at] &= (ofat_u8)~m;
    if ((r = flush(v)) != 0 || (lba2 && (r = mirror(v, lba2)) != 0)) return r;
    if (v->free != NOCOUNT) { if (on) --v->free; else ++v->free; }
    return OFAT_EXFAT_OK;
}

/* A free cluster, from want on (else from the hint), marked in the bitmap. */
static int alloc(struct ofat_exfat *v, ofat_u32 want, ofat_u32 *clu)
{
    ofat_u32 n, bit, bits = 8UL << v->sshift;
    ofat_lba lba;
    const ofat_u8 *s;
    unsigned at;
    int r;
    if (want < 2 || want >= v->clusters + 2) want = v->hint;
    if (want < 2 || want >= v->clusters + 2) want = 2;
    bit = want - 2;
    for (n = 0; n < v->clusters; ) {
        if (bit >= v->clusters) bit = 0;
        if ((r = bitmap_lba(v, v->bitmap, bit, &lba)) != 0) return r;
        if (!(s = sector(v, lba))) return OFAT_EXFAT_EIO;
        for (;;) {
            at = (unsigned)(bit >> 3) & ((1u << v->sshift) - 1);
            if (!(bit & 7) && s[at] == 0xFF && bit + 8 <= v->clusters) { bit += 8; n += 8; }
            else {
                if (!(s[at] >> (bit & 7) & 1)) {
                    *clu = bit + 2; v->hint = bit + 3;
                    return set_bit(v, *clu, 1);
                }
                ++bit; ++n;
            }
            if (n >= v->clusters || bit >= v->clusters || !(bit & (bits - 1))) break;
        }
    }
    return OFAT_EXFAT_EFULL;
}

static int set_fat(struct ofat_exfat *v, ofat_u32 clu, ofat_u32 next)
{
    ofat_u32 off = clu >> (v->sshift - 2);
    ofat_u8 *s = (ofat_u8 *)sector(v, v->fat + off);
    int r;
    if (!s) return OFAT_EXFAT_EIO;
    wr32(s + ((unsigned)(clu << 2) & ((1u << v->sshift) - 1)), next);
    if ((r = flush(v)) != 0) return r;
    return v->fat2 ? mirror(v, v->fat2 + off) : OFAT_EXFAT_OK;
}

/* the clusters a file or directory has */
static int nclusters(struct ofat_exfat *v, struct ofat_exfat_file *f, ofat_u32 *n)
{
    ofat_u32 clu;
    int r;
    if (f->first < 2) { *n = 0; return OFAT_EXFAT_OK; }
    if (!f->root) {
        *n = (ofat_u32)(f->size >> CB(v)) + ((f->size & ((1UL << CB(v)) - 1)) != 0);
        return OFAT_EXFAT_OK;
    }
    for (*n = f->idx; ; ++*n) {
        r = walk(v, f, *n, &clu);
        if (r == OFAT_EXFAT_END) return OFAT_EXFAT_OK;
        if (r) return r;
    }
}

/* bytes in n clusters (narrow: at most 0FFFFFFFFh) */
static ofat_fsize bytes_of(struct ofat_exfat *v, ofat_u32 n)
{
#ifdef OFAT_NARROW
    return n >> (32 - CB(v)) ? 0xFFFFFFFFUL : n << CB(v);
#else
    return (ofat_fsize)n << CB(v);
#endif
}

/* *n clusters to want: contiguous (NoFatChain) while the next cluster is
   free, else a FAT chain.  zero: the new clusters are zeroed before they
   are linked.  On OFAT_EXFAT_EFULL *n tells how far it went. */
static int grow(struct ofat_exfat *v, struct ofat_exfat_file *f, ofat_u32 *n, ofat_u32 want, int zero)
{
    ofat_u32 last = 0, clu, c, k;
    int r;
    if (*n && (r = walk(v, f, *n - 1, &last)) != 0) return bad(r);
    for (; *n < want; ++*n) {
        if ((r = alloc(v, *n ? last + 1 : 0, &clu)) != 0) return r;
        if (zero) {
            v->buf_lba = NOBUF;
            memset(v->buf, 0, 1u << v->sshift);
            for (k = 0; k < 1UL << v->cshift; ++k)
                if (v->write(v->dev, v->start + v->heap + ((ofat_lba)(clu - 2) << v->cshift) + k, v->buf))
                    return OFAT_EXFAT_EWRITE;
        }
        if (!*n) { f->first = clu; f->nofat = (ofat_u8)!f->root; f->idx = 0; f->clu = clu; }
        else if (f->nofat && clu != last + 1) {     /* no longer contiguous */
            for (c = f->first; c < last; ++c) if ((r = set_fat(v, c, c + 1)) != 0) return r;
            if ((r = set_fat(v, last, EOC)) != 0) return r;
            f->nofat = 0;
        }
        if (!f->nofat) {
            if ((r = set_fat(v, clu, EOC)) != 0) return r;
            if (*n && (r = set_fat(v, last, clu)) != 0) return r;
        }
        last = clu;
    }
    return OFAT_EXFAT_OK;
}

/* frees the clusters k to n - 1 of a file or directory */
static int shrink(struct ofat_exfat *v, struct ofat_exfat_file *f, ofat_u32 n, ofat_u32 k)
{
    ofat_u32 clu, next, i;
    int r;
    if (k >= n) return OFAT_EXFAT_OK;
    if (f->nofat) {
        for (clu = f->first + k; clu < f->first + n; ++clu) if ((r = set_bit(v, clu, 0)) != 0) return r;
    } else {
        clu = f->first;
        if (k) {
            if ((r = walk(v, f, k - 1, &clu)) != 0) return bad(r);
            if (!(next = fat_next(v, clu))) return OFAT_EXFAT_EIO;
            if ((r = set_fat(v, clu, EOC)) != 0) return r;
            clu = next;
        }
        for (i = k; i < n; ++i) {
            if (clu < 2 || clu >= v->clusters + 2) return OFAT_EXFAT_EBAD;
            if (!(next = fat_next(v, clu))) return OFAT_EXFAT_EIO;
            if ((r = set_bit(v, clu, 0)) != 0) return r;
            clu = next;
        }
    }
    if (!k) { f->first = 0; f->nofat = 0; }
    f->idx = 0; f->clu = 0;
    return OFAT_EXFAT_OK;
}

/* ---- entry sets ---- */

static ofat_u8 fe[32], se[32];            /* file and stream entry of a set */

/* Writes e as the entry at pos; the sector goes to the medium when the
   entry is the last one in it, or when last (the next entry is then not
   in the cache's sector). */
static int put_entry(struct ofat_exfat *v, struct ofat_exfat_file *d, ofat_u32 pos, const ofat_u8 *e, int last)
{
    const ofat_u8 *p;
    int r = dir_entry(v, d, pos, &p);
    if (r) return bad(r);
    memcpy((ofat_u8 *)p, e, 32);
    return last || !((pos + 32) & ((1u << v->sshift) - 1)) ? flush(v) : OFAT_EXFAT_OK;
}

/* fe and se of the set at pos; *n its secondary count */
static int get_set(struct ofat_exfat *v, struct ofat_exfat_file *d, ofat_u32 pos, unsigned *n)
{
    const ofat_u8 *e;
    int r = dir_entry(v, d, pos, &e);
    if (r) return bad(r);
    memcpy(fe, e, 32);
    if ((r = dir_entry(v, d, pos + 32, &e)) != 0) return bad(r);
    memcpy(se, e, 32);
    if (fe[0] != 0x85 || se[0] != 0xC0 || fe[1] < 2 || fe[1] > 18) return OFAT_EXFAT_EBAD;
    *n = fe[1];
    return OFAT_EXFAT_OK;
}

/* fe and se back, with the checksum over the set */
static int put_set(struct ofat_exfat *v, struct ofat_exfat_file *d, ofat_u32 pos)
{
    const ofat_u8 *e;
    unsigned i;
    ofat_u16 sum = checksum(checksum(0, fe, 1), se, 0);
    int r;
    for (i = 2; i <= fe[1]; ++i) {
        if ((r = dir_entry(v, d, pos + 32UL*i, &e)) != 0) return bad(r);
        sum = checksum(sum, e, 0);
    }
    wr16(fe+2, sum);
    if ((r = put_entry(v, d, pos, fe, 0)) != 0) return r;
    return put_entry(v, d, pos + 32, se, 1);
}

void ofat_exfat_where_of(const struct ofat_exfat_file *dir, ofat_u32 pos, struct ofat_exfat_where *w)
{
    w->first = dir->first; w->size = dir->size; w->nofat = dir->nofat;
    w->pos = pos; w->none = 0;
}

void ofat_exfat_where_dir(const struct ofat_exfat_where *w, struct ofat_exfat_file *dir)
{
    memset(dir, 0, sizeof(*dir));
    dir->first = w->first; dir->size = w->size; dir->nofat = w->nofat;
    dir->root = (ofat_u8)!w->nofat;
}

/* the stream entry of a file or directory after a change of its clusters */
static int put_stream(struct ofat_exfat *v, const struct ofat_exfat_where *w, const struct ofat_exfat_file *f)
{
    struct ofat_exfat_file dir;
    unsigned n;
    int r;
    if (w->none) return OFAT_EXFAT_OK;
    ofat_exfat_where_dir(w, &dir);
    if ((r = get_set(v, &dir, w->pos, &n)) != 0) return r;
    se[1] = (ofat_u8)((se[1] & ~2) | 1 | (f->nofat ? 2 : 0));
    wrsize(se+8, f->valid);
    wr32(se+20, f->first);
    wrsize(se+24, f->size);
    return put_set(v, &dir, w->pos);
}

/* a character of a name in upper case, by the up-case table of the volume */
static ofat_u16 upcase(struct ofat_exfat *v, ofat_u16 c)
{
    struct ofat_exfat_file t;
    ofat_u32 off, idx = 0;
    ofat_u16 done, w;
    ofat_u8 b[2];
    if (c < 0x80) return (ofat_u16)(c >= 'a' && c <= 'z' ? c - 32 : c);
    memset(&t, 0, sizeof(t));
    t.first = v->upcase; t.size = t.valid = v->upcase_len;
    if (t.first < 2) return c;
    for (off = 0; off + 1 < t.size && idx <= c; off += 2) {
        if (ofat_exfat_read(v, &t, off, b, 2, &done) || done != 2) break;
        w = rd16(b);
        if (w == 0xFFFF) {                      /* a run of unchanged characters */
            off += 2;
            if (ofat_exfat_read(v, &t, off, b, 2, &done) || done != 2) break;
            idx += rd16(b);
            continue;
        }
        if (idx++ == c) return w;
    }
    return c;
}

static ofat_u16 name_hash(struct ofat_exfat *v, const ofat_u16 *name, unsigned len)
{
    ofat_u16 h = 0, c;
    unsigned i;
    for (i = 0; i < len; ++i) {
        c = upcase(v, name[i]);
        h = (ofat_u16)(((h & 1) ? 0x8000u : 0) + (h >> 1) + (c & 0xFF));
        h = (ofat_u16)(((h & 1) ? 0x8000u : 0) + (h >> 1) + (c >> 8));
    }
    return h;
}

/* a directory one cluster longer, zeroed */
static int grow_dir(struct ofat_exfat *v, struct ofat_exfat_file *dir, struct ofat_exfat_where *dw)
{
    ofat_u32 n;
    int r;
    if ((r = nclusters(v, dir, &n)) != 0) return r;
    if ((r = grow(v, dir, &n, n + 1, 1)) != 0) return r;
    dir->size += 1UL << CB(v);
    dir->valid = dir->size;
    return put_stream(v, dw, dir);
}

/* entry i of a new set: f, s, then the name */
static void make_entry(unsigned i, ofat_u8 *e, const ofat_u8 *f, const ofat_u8 *s, const ofat_u16 *name, unsigned len)
{
    unsigned k, at = (i - 2) * 15;
    if (i < 2) { memcpy(e, i ? s : f, 32); return; }
    memset(e, 0, 32);
    e[0] = 0xC1;
    for (k = 0; k < 15 && at + k < len; ++k) wr16(e + 2 + 2*k, name[at + k]);
}

/* fe and se with the name as a new set in dir, where enough entries are
   free; the directory grows when there are not. */
static int place(struct ofat_exfat *v, struct ofat_exfat_file *dir, struct ofat_exfat_where *dw,
                 const ofat_u16 *name, unsigned len, ofat_u32 *pos)
{
    ofat_u8 f[32], s[32], e[32];
    unsigned n = 2 + (len + 14) / 15, i, run = 0;
    ofat_u32 p, start = 0;
    const ofat_u8 *x;
    ofat_u16 sum = 0;
    int r;
    if (!len || len > 255) return OFAT_EXFAT_EBAD;
    fe[0] = 0x85; fe[1] = (ofat_u8)(n - 1);
    se[0] = 0xC0; se[2] = 0; se[3] = (ofat_u8)len;
    memcpy(f, fe, 32); memcpy(s, se, 32);       /* grow_dir uses fe and se */
    wr16(s+4, name_hash(v, name, len));
    for (i = 0; i < n; ++i) { make_entry(i, e, f, s, name, len); sum = checksum(sum, e, !i); }
    wr16(f+2, sum);
    for (p = 0; run < n; p += 32) {
        r = dir_entry(v, dir, p, &x);
        if (r == OFAT_EXFAT_END) {
            if ((r = grow_dir(v, dir, dw)) != 0) return r;
            r = dir_entry(v, dir, p, &x);
        }
        if (r) return bad(r);
        if (x[0] & 0x80) { run = 0; continue; }
        if (!run++) start = p;
    }
    for (i = 0; i < n; ++i) {
        make_entry(i, e, f, s, name, len);
        if ((r = put_entry(v, dir, start + 32UL*i, e, i == n - 1)) != 0) return r;
    }
    *pos = start;
    return OFAT_EXFAT_OK;
}

/* the entries of a set as unused */
static int unmark(struct ofat_exfat *v, struct ofat_exfat_file *d, ofat_u32 pos, unsigned n)
{
    const ofat_u8 *e;
    unsigned i;
    int r;
    for (i = 0; i <= n; ++i) {
        if ((r = dir_entry(v, d, pos + 32UL*i, &e)) != 0) return bad(r);
        ((ofat_u8 *)e)[0] &= 0x7F;
        if (i == n || !((pos + 32UL*(i + 1)) & ((1u << v->sshift) - 1)))
            if ((r = flush(v)) != 0) return r;
    }
    return OFAT_EXFAT_OK;
}

int ofat_exfat_write(struct ofat_exfat *v, struct ofat_exfat_file *f, const struct ofat_exfat_where *w,
                   ofat_fsize off, const void OFAT_FAR *buf, ofat_u16 len, ofat_u16 *done)
{
    const ofat_u8 OFAT_FAR *in = (const ofat_u8 OFAT_FAR *)buf;
    unsigned ssize = 1u << v->sshift, at, a, b;
    ofat_fsize end, pos, sbeg;
    ofat_u32 have, need;
    ofat_lba lba;
    ofat_u8 *s;
    int r, full = 0;
    *done = 0;
    if (f->big) return OFAT_EXFAT_EBAD;
    if (!len) return OFAT_EXFAT_OK;
    if ((r = begin(v)) != 0) return r;
    end = off + len;
    if (end < off) end = (ofat_fsize)~(ofat_fsize)0;   /* narrow: 4 GB */
    if ((r = nclusters(v, f, &have)) != 0) return r;
    if ((end - 1) >> CB(v) >= v->clusters) return OFAT_EXFAT_EFULL;
    need = (ofat_u32)(end >> CB(v)) + ((end & ((1UL << CB(v)) - 1)) != 0);
    if (need > have) {
        r = grow(v, f, &have, need, 0);
        if (r == OFAT_EXFAT_EFULL) {
            full = 1;
            if (end > bytes_of(v, have)) end = bytes_of(v, have);
            if (off > end) off = end;
        } else if (r) return r;
    }
    for (pos = f->valid < off ? f->valid : off; pos < end; ) {
        at = (unsigned)pos & (ssize - 1);
        sbeg = pos - at;
        if ((r = file_sector(v, f, pos, &lba)) != 0) return bad(r);
        if (!at && pos >= off && end - pos >= ssize) {          /* a whole sector */
            if (v->buf_lba == lba) v->buf_lba = NOBUF;
            if (v->write(v->dev, v->start + lba, in + (unsigned)(pos - off))) return OFAT_EXFAT_EWRITE;
            pos += ssize;
            continue;
        }
        if (sbeg < f->valid) {
            if (!(s = (ofat_u8 *)sector(v, lba))) return OFAT_EXFAT_EIO;
            if (f->valid - sbeg < ssize) memset(s + (unsigned)(f->valid - sbeg), 0, ssize - (unsigned)(f->valid - sbeg));
        } else {                                /* after the valid data: zeros */
            s = v->buf;
            memset(s, 0, ssize);
            v->buf_lba = lba;
        }
        a = off > sbeg ? (unsigned)(off - sbeg) : 0;
        b = end - sbeg >= ssize ? ssize : (unsigned)(end - sbeg);
        if (a < b) fmemcpy(s + a, in + (unsigned)(sbeg + a - off), b - a);
        if ((r = flush(v)) != 0) return r;
        pos = sbeg + b;
    }
    if (end > f->size) f->size = end;
    if (end > f->valid) f->valid = end;
    *done = end > off ? (ofat_u16)(end - off) : 0;
    if ((r = put_stream(v, w, f)) != 0) return r;
    return full ? OFAT_EXFAT_EFULL : OFAT_EXFAT_OK;
}

int ofat_exfat_resize(struct ofat_exfat *v, struct ofat_exfat_file *f, const struct ofat_exfat_where *w, ofat_fsize size)
{
    ofat_u32 have, need, n;
    int r;
    if (f->big) return OFAT_EXFAT_EBAD;
    if ((r = begin(v)) != 0) return r;
    if ((r = nclusters(v, f, &have)) != 0) return r;
    if (size && (size - 1) >> CB(v) >= v->clusters) return OFAT_EXFAT_EFULL;
    need = (ofat_u32)(size >> CB(v)) + ((size & ((1UL << CB(v)) - 1)) != 0);
    if (need > have) {
        n = have;
        if ((r = grow(v, f, &n, need, 0)) != 0) { shrink(v, f, n, have); return r; }
    } else if ((r = shrink(v, f, have, need)) != 0) return r;
    f->size = size;
    if (f->valid > size) f->valid = size;
    return put_stream(v, w, f);
}

int ofat_exfat_create(struct ofat_exfat *v, struct ofat_exfat_file *dir, struct ofat_exfat_where *dw,
                    const ofat_u16 *name, unsigned len, ofat_u16 attr, ofat_u16 time, ofat_u16 date, ofat_u32 *pos)
{
    struct ofat_exfat_file f;
    ofat_u32 n = 0, ts = (ofat_u32)date << 16 | time;
    int r = begin(v);
    if (r) return r;
    memset(&f, 0, sizeof(f));
    if (attr & OFAT_EXFAT_DIR) {
        if ((r = grow(v, &f, &n, 1, 1)) != 0) return r;
        f.size = f.valid = 1UL << CB(v);
    }
    memset(fe, 0, 32); memset(se, 0, 32);
    wr16(fe+4, attr);
    wr32(fe+8, ts); wr32(fe+12, ts); wr32(fe+16, ts);
    se[1] = (ofat_u8)(1 | (f.nofat ? 2 : 0));
    wrsize(se+8, f.valid); wr32(se+20, f.first); wrsize(se+24, f.size);
    r = place(v, dir, dw, name, len, pos);
    if (r && f.first) shrink(v, &f, 1, 0);
    return r;
}

/* the clusters of the stream entry se (DataLength of 64 bits) */
static ofat_u32 stream_clusters(struct ofat_exfat *v)
{
    ofat_u32 lo = rd32(se+24), hi = rd32(se+28);
    if (rd32(se+20) < 2) return 0;
    return (hi << (32 - CB(v))) + (lo >> CB(v)) + ((lo & ((1UL << CB(v)) - 1)) != 0);
}

int ofat_exfat_remove(struct ofat_exfat *v, struct ofat_exfat_file *dir, ofat_u32 pos)
{
    struct ofat_exfat_file f;
    unsigned n;
    ofat_u32 k;
    int r;
    if ((r = get_set(v, dir, pos, &n)) != 0) return r;
    if ((r = begin(v)) != 0) return r;
    memset(&f, 0, sizeof(f));
    f.nofat = (se[1] & 2) != 0;
    f.first = rd32(se+20);
    k = stream_clusters(v);
    if ((r = unmark(v, dir, pos, n)) != 0) return r;
    return shrink(v, &f, k, 0);
}

int ofat_exfat_empty(struct ofat_exfat *v, struct ofat_exfat_file *dir)
{
    const ofat_u8 *e;
    ofat_u32 p;
    int r;
    for (p = 0; ; p += 32) {
        r = dir_entry(v, dir, p, &e);
        if (r == OFAT_EXFAT_END) return 1;
        if (r) return r;
        if (!e[0]) return 1;
        if (e[0] & 0x80) return 0;
    }
}

int ofat_exfat_rename(struct ofat_exfat *v, struct ofat_exfat_file *dir, ofat_u32 pos,
                    struct ofat_exfat_file *to, struct ofat_exfat_where *tw,
                    const ofat_u16 *name, unsigned len, ofat_u32 *newpos)
{
    unsigned n;
    int r;
    if ((r = get_set(v, dir, pos, &n)) != 0) return r;
    if ((r = begin(v)) != 0) return r;
    if ((r = place(v, to, tw, name, len, newpos)) != 0) return r;
    return unmark(v, dir, pos, n);
}

int ofat_exfat_attr(struct ofat_exfat *v, struct ofat_exfat_file *dir, ofat_u32 pos, ofat_u16 attr)
{
    unsigned n;
    int r;
    if ((r = get_set(v, dir, pos, &n)) != 0) return r;
    if ((r = begin(v)) != 0) return r;
    wr16(fe+4, (ofat_u16)((attr & ~OFAT_EXFAT_DIR) | (rd16(fe+4) & OFAT_EXFAT_DIR)));
    return put_set(v, dir, pos);
}

int ofat_exfat_time(struct ofat_exfat *v, struct ofat_exfat_file *dir, ofat_u32 pos, unsigned which,
                  ofat_u16 time, ofat_u16 date, ofat_u8 cs10)
{
    unsigned n, k = (which - 8) / 4;
    int r;
    if ((r = get_set(v, dir, pos, &n)) != 0) return r;
    if ((r = begin(v)) != 0) return r;
    wr32(fe + which, (ofat_u32)date << 16 | time);
    if (k < 2) fe[20 + k] = cs10;
    fe[22 + k] = 0;                             /* no UTC offset */
    return put_set(v, dir, pos);
}
