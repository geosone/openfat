/* exFAT reading: boot sector, cluster chains, directory entry sets, file
   data and 8.3 names, after the Microsoft exFAT file system specification. */
#include <string.h>
#include "openaspi/exfat.h"

#ifdef __WATCOMC__
#define fmemcpy _fmemcpy
#define fmemset _fmemset
#else
#define fmemcpy memcpy
#define fmemset memset
#endif

#define NOBUF 0xFFFFFFFFUL

static oa_u16 rd16(const oa_u8 OA_FAR *p) { return (oa_u16)(p[0] | p[1] << 8); }
static oa_u32 rd32(const oa_u8 OA_FAR *p) { return rd16(p) | (oa_u32)rd16(p+2) << 16; }

static const oa_u8 *sector(struct oa_exfat *v, oa_u32 lba)
{
    if (v->buf_lba != lba) {
        v->buf_lba = NOBUF;
        if (v->read(v->dev, v->start + lba, v->buf)) return 0;
        v->buf_lba = lba;
    }
    return v->buf;
}

int oa_exfat_probe(const oa_u8 OA_FAR *b)
{
    unsigned i;
    if (b[3] != 'E' || b[4] != 'X' || b[5] != 'F' || b[6] != 'A' || b[7] != 'T' ||
        b[8] != ' ' || b[9] != ' ' || b[10] != ' ' || b[510] != 0x55 || b[511] != 0xAA) return 0;
    for (i = 11; i < 64; ++i) if (b[i]) return 0;
    return 1;
}

/* The FAT entry of a cluster: the next cluster, or 0 on a read error. */
static oa_u32 fat_next(struct oa_exfat *v, oa_u32 clu)
{
    const oa_u8 *s = sector(v, v->fat + (clu >> (v->sshift - 2)));
    if (!s) return 0;
    return rd32(s + ((unsigned)(clu << 2) & ((1u << v->sshift) - 1)));
}

/* The cluster at index idx of a chain: 0 ok, OA_EXFAT_END past its end. */
static int walk(struct oa_exfat *v, struct oa_exfat_file *f, oa_u32 idx, oa_u32 *clu)
{
    oa_u32 next;
    if (f->first < 2 || f->first >= v->clusters + 2) return OA_EXFAT_EBAD;
    if (f->nofat) {
        if (idx >= v->clusters + 2 - f->first) return OA_EXFAT_EBAD;
        *clu = f->first + idx;
        return OA_EXFAT_OK;
    }
    if (idx < f->idx || f->clu < 2) { f->idx = 0; f->clu = f->first; }
    while (f->idx < idx) {
        next = fat_next(v, f->clu);
        if (!next) return OA_EXFAT_EIO;
        if (next == 0xFFFFFFFFUL) return f->root ? OA_EXFAT_END : OA_EXFAT_EBAD;
        if (next < 2 || next >= v->clusters + 2) return OA_EXFAT_EBAD;
        f->clu = next; ++f->idx;
    }
    *clu = f->clu;
    return OA_EXFAT_OK;
}

/* The sector holding byte off of a file or directory. */
static int file_sector(struct oa_exfat *v, struct oa_exfat_file *f, oa_u32 off, oa_u32 *lba)
{
    oa_u32 clu;
    unsigned shift = v->sshift + v->cshift;
    int r = walk(v, f, off >> shift, &clu);
    if (r) return r;
    *lba = v->heap + ((clu - 2) << v->cshift) + ((off >> v->sshift) & ((1UL << v->cshift) - 1));
    return OA_EXFAT_OK;
}

/* The 32-byte directory entry at byte pos of a directory. */
static int dir_entry(struct oa_exfat *v, struct oa_exfat_file *d, oa_u32 pos, const oa_u8 **e)
{
    oa_u32 lba;
    const oa_u8 *s;
    int r;
    if (!d->root && pos >= d->size) return OA_EXFAT_END;
    r = file_sector(v, d, pos, &lba);
    if (r) return r;
    if (!(s = sector(v, lba))) return OA_EXFAT_EIO;
    *e = s + ((unsigned)pos & ((1u << v->sshift) - 1));
    return OA_EXFAT_OK;
}

void oa_exfat_rootdir(const struct oa_exfat *v, struct oa_exfat_file *f)
{
    memset(f, 0, sizeof(*f));
    f->first = v->root; f->root = 1;
}

int oa_exfat_mount(struct oa_exfat *v, oa_exfat_read_fn read, void *dev, oa_u32 start, const struct oa_exfat_cp *cp, oa_u8 *buf)
{
    const oa_u8 *b;
    struct oa_exfat_file root;
    oa_u32 pos, len;
    unsigned i, active;
    int r;
    memset(v, 0, sizeof(*v));
    v->read = read; v->dev = dev; v->start = start; v->cp = cp; v->buf = buf; v->buf_lba = NOBUF;
    if (read(dev, start, v->buf)) return OA_EXFAT_EIO;
    b = v->buf;
    if (!oa_exfat_probe(b)) return OA_EXFAT_EBAD;
    v->sshift = b[108]; v->cshift = b[109];
    if (v->sshift < 9 || v->sshift > 12 || v->cshift > 25 - v->sshift) return OA_EXFAT_EBAD;
    if ((1u << v->sshift) > OA_EXFAT_MAXSECT) return OA_EXFAT_ESIZE;
    len = rd32(b+72);
    if (rd32(b+76)) len = 0xFFFFFFFFUL;
    v->fat = rd32(b+80); v->heap = rd32(b+88); v->clusters = rd32(b+92);
    v->root = rd32(b+96); v->serial = rd32(b+100); v->flags = rd16(b+106);
    if (b[110] != 1 && b[110] != 2) return OA_EXFAT_EBAD;
    /* TexFAT: two FATs and bitmaps, ActiveFat names the ones in use */
    active = b[110] == 2 ? v->flags & 1 : 0;
    if (active) v->fat += rd32(b+84);
    if (v->fat < 24 || v->heap < v->fat || !v->clusters || v->clusters > 0xFFFFFFF5UL ||
        (oa_u32)(v->heap + (v->clusters << v->cshift)) > len ||
        rd32(b+84) < ((v->clusters + 2) >> (v->sshift - 2)) ||
        v->root < 2 || v->root >= v->clusters + 2) return OA_EXFAT_EBAD;
    /* the root directory: allocation bitmap and volume label */
    oa_exfat_rootdir(v, &root);
    for (pos = 0; ; pos += 32) {
        r = dir_entry(v, &root, pos, &b);
        if (r == OA_EXFAT_END) break;
        if (r) return r;
        if (!b[0]) break;
        if (b[0] == 0x81 && (b[1] & 1) == active) v->bitmap = rd32(b+20);
        if (b[0] == 0x83) {
            v->label_len = b[1] > 11 ? 11 : b[1];
            for (i = 0; i < v->label_len; ++i) v->label[i] = rd16(b + 2 + 2*i);
        }
    }
    if (v->bitmap < 2 || v->bitmap >= v->clusters + 2) return OA_EXFAT_EBAD;
    return OA_EXFAT_OK;
}

static void stream_file(const oa_u8 *e, struct oa_exfat_file *f)
{
    memset(f, 0, sizeof(*f));
    f->nofat = (e[1] & 2) != 0;
    f->first = rd32(e+20);
    f->size = rd32(e+24); f->valid = rd32(e+8);
    if (rd32(e+28)) { f->big = 1; f->size = 0xFFFFFFFFUL; }
    if (rd32(e+12)) f->valid = 0xFFFFFFFFUL;
    if (f->valid > f->size) f->valid = f->size;
}

static oa_u16 checksum(oa_u16 sum, const oa_u8 *e, int first)
{
    unsigned i;
    for (i = 0; i < 32; ++i) {
        if (first && (i == 2 || i == 3)) continue;
        sum = (oa_u16)(((sum & 1) ? 0x8000u : 0) + (sum >> 1) + e[i]);
    }
    return sum;
}

int oa_exfat_dir_next(struct oa_exfat *v, struct oa_exfat_file *d, oa_u32 *pos, struct oa_exfat_ent *ent)
{
    const oa_u8 *e;
    oa_u16 sum, want;
    unsigned n, i, k, names;
    int r;
    for (;; *pos += 32) {
        r = dir_entry(v, d, *pos, &e);
        if (r) return r;
        if (!e[0]) return OA_EXFAT_END;
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
            if (r == OA_EXFAT_END) return OA_EXFAT_EBAD;
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
        return OA_EXFAT_OK;
    }
}

int oa_exfat_read(struct oa_exfat *v, struct oa_exfat_file *f, oa_u32 off, void OA_FAR *buf, oa_u16 len, oa_u16 *done)
{
    oa_u8 OA_FAR *out = (oa_u8 OA_FAR *)buf;
    unsigned ssize = 1u << v->sshift, in, n;
    oa_u32 lba;
    const oa_u8 *s;
    int r;
    *done = 0;
    if (off >= f->size) return OA_EXFAT_OK;
    if (len > f->size - off) len = (oa_u16)(f->size - off);
    while (len) {
        in = (unsigned)off & (ssize - 1);
        n = ssize - in;
        if (n > len) n = len;
        if (off >= f->valid) {
            fmemset(out, 0, n);
        } else {
            if (f->valid - off < n) n = (unsigned)(f->valid - off);
            r = file_sector(v, f, off, &lba);
            if (r) return r == OA_EXFAT_END ? OA_EXFAT_EBAD : r;
            if (!in && n == ssize) {
                if (v->buf_lba == lba) v->buf_lba = NOBUF;
                if (v->read(v->dev, v->start + lba, out)) return OA_EXFAT_EIO;
            } else {
                if (!(s = sector(v, lba))) return OA_EXFAT_EIO;
                fmemcpy(out, s + in, n);
            }
        }
        out += n; off += n; len -= n; *done += n;
    }
    return OA_EXFAT_OK;
}

int oa_exfat_free(struct oa_exfat *v, oa_u32 *free_clusters)
{
    struct oa_exfat_file bm;
    oa_u32 bit, lba;
    const oa_u8 *s;
    unsigned bits = 8u << v->sshift, i;
    oa_u8 c;
    int r;
    memset(&bm, 0, sizeof(bm));
    bm.first = v->bitmap;
    *free_clusters = 0;
    for (bit = 0; bit < v->clusters; bit += bits) {
        r = file_sector(v, &bm, bit >> 3, &lba);
        if (r) return r == OA_EXFAT_END ? OA_EXFAT_EBAD : r;
        if (!(s = sector(v, lba))) return OA_EXFAT_EIO;
        for (i = 0; i < bits && bit + i < v->clusters; ++i) {
            c = s[i >> 3];
            if (!(c & (1 << (i & 7)))) ++*free_clusters;
        }
    }
    return OA_EXFAT_OK;
}

/* A Unicode character in the code page, upper case; 0 when it has none. */
static oa_u8 oem_upper(const struct oa_exfat_cp *cp, oa_u16 u)
{
    unsigned i;
    if (u < 0x80) return (oa_u8)(u >= 'a' && u <= 'z' ? u - 32 : u);
    for (i = 0; i < 128; ++i) if (cp->uni[i] == u) return cp->upper[i];
    return 0;
}

static int part83(const struct oa_exfat_cp *cp, const oa_u16 *name, unsigned len, char *out, unsigned max)
{
    unsigned i, n = 0;
    int lossy = 0;
    oa_u8 c;
    for (i = 0; i < len; ++i) {
        c = oem_upper(cp, name[i]);
        if (c == ' ' || c == '.') { lossy = 1; continue; }
        if (!c || c < 0x20 || strchr("\"*+,/:;<=>?[\\]|", c)) { c = '_'; lossy = 1; }
        if (n == max) { lossy = 1; break; }
        out[n++] = (char)c;
    }
    return lossy;
}

int oa_exfat_basis(const struct oa_exfat_cp *cp, const oa_u16 *name, unsigned len, char fcb[11])
{
    unsigned dot = len, lead = 0, i;
    int lossy;
    memset(fcb, ' ', 11);
    while (lead < len && name[lead] == '.') ++lead;
    for (i = len; i > lead; --i) if (name[i-1] == '.') { dot = i - 1; break; }
    lossy = lead != 0;
    lossy |= part83(cp, name + lead, dot - lead, fcb, 8);
    if (dot < len) lossy |= part83(cp, name + dot + 1, len - dot - 1, fcb + 8, 3);
    if (fcb[0] == ' ') { fcb[0] = '_'; lossy = 1; }
    return lossy;
}

static struct oa_exfat_ent other;

int oa_exfat_short(struct oa_exfat *v, struct oa_exfat_file *d, const struct oa_exfat_ent *ent, char fcb[11])
{
    char basis[11], tail[8];
    oa_u32 pos = 0, n = 1;
    unsigned len, keep, t;
    int r;
    if (!oa_exfat_basis(v->cp, ent->name, ent->name_len, fcb)) return OA_EXFAT_OK;
    /* the n-th entry with this basis in the directory gets "~n" */
    while (pos < ent->pos) {
        r = oa_exfat_dir_next(v, d, &pos, &other);
        if (r == OA_EXFAT_END) break;
        if (r) return r;
        if (other.pos < ent->pos && oa_exfat_basis(v->cp, other.name, other.name_len, basis) &&
            !memcmp(basis, fcb, 11)) ++n;
    }
    t = sizeof(tail);
    do { tail[--t] = (char)('0' + n % 10); n /= 10; } while (n);
    tail[--t] = '~';
    for (len = 8; len && fcb[len-1] == ' '; --len) ;
    keep = 8 - (sizeof(tail) - t);
    if (len > keep) len = keep;
    memcpy(fcb + len, tail + t, sizeof(tail) - t);
    return OA_EXFAT_OK;
}

int oa_exfat_lookup(struct oa_exfat *v, struct oa_exfat_file *d, const char fcb[11], struct oa_exfat_ent *ent)
{
    char name[11];
    oa_u32 pos = 0, next;
    unsigned tilde;
    int r;
    for (tilde = 0; tilde < 8 && fcb[tilde] != '~'; ++tilde) ;
    for (;;) {
        r = oa_exfat_dir_next(v, d, &pos, ent);
        if (r) return r;
        if (!oa_exfat_basis(v->cp, ent->name, ent->name_len, name)) {
            if (!memcmp(name, fcb, 11)) return OA_EXFAT_OK;
            continue;
        }
        /* a changed name: only one with the same start and extension can be it */
        if (tilde == 8 || memcmp(name, fcb, tilde) || memcmp(name + 8, fcb + 8, 3)) continue;
        next = pos;
        r = oa_exfat_short(v, d, ent, name);
        if (r) return r;
        if (!memcmp(name, fcb, 11)) return OA_EXFAT_OK;
        pos = next;
    }
}
