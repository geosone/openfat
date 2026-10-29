/* exFAT writing on the host: a series of changes to an image of
   tests/exfat_image.sh, done the same way with the C library in a copy of
   its files (MIRROR); tests/exfat_write.sh then compares both through the
   Linux exFAT driver and checks the image with fsck.exfat.
   exfat_write_test IMAGE MIRROR [FULL]
   FULL: also fill the volume until it is full, then remove the file. */
#define _XOPEN_SOURCE 500                /* truncate */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "openaspi/exfat.h"

static FILE *image;
static const char *mirror;
static struct oa_exfat vol;
static oa_u8 sector_buf[OA_EXFAT_MAXSECT];
static struct oa_exfat_ent ent;
static unsigned long writes;

static int img_read(void *dev, oa_u32 lba, void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 512, 1, image) == 1 ? 0 : -1;
}

static int img_write(void *dev, oa_u32 lba, const void *buf)
{
    (void)dev; ++writes;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    return fwrite(buf, 512, 1, image) == 1 ? 0 : -1;
}

#define CHECK(x) do { int r_ = (x); if (r_) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

/* UTF-8 to UTF-16 (the BMP only) */
static unsigned to16(const char *s, unsigned n, oa_u16 *out)
{
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    unsigned k = 0;
    while (p < e) {
        if (*p < 0x80) out[k++] = *p++;
        else if (*p < 0xE0) { out[k++] = (oa_u16)((p[0] & 31) << 6 | (p[1] & 63)); p += 2; }
        else { out[k++] = (oa_u16)((p[0] & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63)); p += 3; }
    }
    return k;
}

static int find(struct oa_exfat_file *dir, const oa_u16 *name, unsigned len)
{
    oa_u32 pos = 0;
    int r;
    while (!(r = oa_exfat_dir_next(&vol, dir, &pos, &ent)))
        if (ent.name_len == len && !memcmp(ent.name, name, len * 2)) return 0;
    assert(r == OA_EXFAT_END);
    return -1;
}

static void as_dir(struct oa_exfat_file *dir)
{
    *dir = ent.file;
    if (!dir->nofat) dir->root = 1;
}

/* The directory of a path ('/' separated) with the place of its entry,
   and the last part as UTF-16. */
static oa_u16 last[256];
static unsigned last_len;

static void parent(const char *path, struct oa_exfat_file *dir, struct oa_exfat_where *dw)
{
    oa_u16 name[256];
    const char *p = path, *e;
    unsigned n;
    oa_exfat_rootdir(&vol, dir);
    memset(dw, 0, sizeof(*dw));
    dw->none = 1;
    while ((e = strchr(p, '/')) != NULL) {
        n = to16(p, (unsigned)(e - p), name);
        if (find(dir, name, n) || !(ent.attr & OA_EXFAT_DIR)) { fprintf(stderr, "no directory in %s\n", path); exit(1); }
        oa_exfat_where_of(dir, ent.pos, dw);
        as_dir(dir);
        p = e + 1;
    }
    last_len = to16(p, (unsigned)strlen(p), last);
}

/* the entry of path in ent, its directory and that directory's place */
static struct oa_exfat_file pdir;
static struct oa_exfat_where pdw;

static void locate(const char *path)
{
    parent(path, &pdir, &pdw);
    if (find(&pdir, last, last_len)) { fprintf(stderr, "not found: %s\n", path); exit(1); }
}

static char *host(const char *path)
{
    static char buf[2][1024];
    static int k;
    k ^= 1;
    assert(strlen(mirror) + strlen(path) + 2 < sizeof(buf[k]));
    sprintf(buf[k], "%s/%s", mirror, path);
    return buf[k];
}

static oa_u8 pattern(unsigned seed, oa_u32 off)
{
    return (oa_u8)(seed * 31 + off * 7 + (off >> 8) + (off >> 16));
}

static void op_create(const char *path, oa_u16 attr)
{
    oa_u32 pos;
    FILE *f;
    parent(path, &pdir, &pdw);
    assert(find(&pdir, last, last_len));
    CHECK(oa_exfat_create(&vol, &pdir, &pdw, last, last_len, attr, 0x5000, 0x5A21, &pos));
    if (attr & OA_EXFAT_DIR) assert(!mkdir(host(path), 0777));
    else { f = fopen(host(path), "wb"); assert(f); fclose(f); }
}

static void op_write(const char *path, oa_u32 off, oa_u32 len, unsigned seed)
{
    static oa_u8 buf[65535];
    struct oa_exfat_where w;
    struct oa_exfat_file f;
    oa_u32 i, n;
    oa_u16 done;
    FILE *h;
    locate(path);
    f = ent.file;
    oa_exfat_where_of(&pdir, ent.pos, &w);
    h = fopen(host(path), "r+b");
    assert(h);
    while (len) {
        n = len > 4000 + seed * 13 ? 4000 + seed * 13 : len;   /* odd chunks */
        for (i = 0; i < n; ++i) buf[i] = pattern(seed, off + i);
        CHECK(oa_exfat_write(&vol, &f, &w, off, buf, (oa_u16)n, &done));
        assert(done == n);
        assert(!fseek(h, (long)off, SEEK_SET) && fwrite(buf, 1, n, h) == n);
        off += n; len -= n;
    }
    fclose(h);
}

static void op_resize(const char *path, oa_u32 size)
{
    struct oa_exfat_where w;
    struct oa_exfat_file f;
    locate(path);
    f = ent.file;
    oa_exfat_where_of(&pdir, ent.pos, &w);
    CHECK(oa_exfat_resize(&vol, &f, &w, size));
    assert(!truncate(host(path), (off_t)size));
}

static void op_remove(const char *path)
{
    struct oa_exfat_file d;
    locate(path);
    if (ent.attr & OA_EXFAT_DIR) {
        as_dir(&d);
        assert(oa_exfat_empty(&vol, &d) == 1);
        assert(!rmdir(host(path)));
    } else assert(!unlink(host(path)));
    CHECK(oa_exfat_remove(&vol, &pdir, ent.pos));
}

static void op_rename(const char *from, const char *to)
{
    struct oa_exfat_file odir, ndir;
    struct oa_exfat_where ndw;
    oa_u32 opos, npos;
    locate(from);
    odir = pdir; opos = ent.pos;
    parent(to, &ndir, &ndw);
    assert(find(&ndir, last, last_len));
    if (odir.first == ndir.first) {             /* the same directory: one struct */
        CHECK(oa_exfat_rename(&vol, &ndir, opos, &ndir, &ndw, last, last_len, &npos));
    } else CHECK(oa_exfat_rename(&vol, &odir, opos, &ndir, &ndw, last, last_len, &npos));
    assert(!rename(host(from), host(to)));
}

/* the content of the image's file against the mirror */
static void compare(const char *path)
{
    static oa_u8 a[4096], b[4096];
    oa_u32 off = 0;
    oa_u16 done;
    size_t got;
    FILE *h;
    locate(path);
    h = fopen(host(path), "rb");
    assert(h);
    do {
        CHECK(oa_exfat_read(&vol, &ent.file, off, a, sizeof(a), &done));
        got = fread(b, 1, sizeof(b), h);
        if (got != done || memcmp(a, b, done)) { fprintf(stderr, "differs: %s at %lu\n", path, (unsigned long)off); exit(1); }
        off += done;
    } while (done);
    fclose(h);
}

int main(int argc, char **argv)
{
    oa_u8 boot[512];
    oa_u32 free0, free1;
    struct oa_exfat_file f;
    struct oa_exfat_where w;
    oa_u16 done;
    char name[64];
    unsigned i;
    int r;
    if (argc < 3) { fprintf(stderr, "exfat_write_test IMAGE MIRROR [FULL]\n"); return 2; }
    image = fopen(argv[1], "r+b");
    mirror = argv[2];
    assert(image);
    CHECK(oa_exfat_mount(&vol, img_read, NULL, 0, &oa_exfat_cp850, sector_buf));
    vol.write = img_write;
    CHECK(oa_exfat_free(&vol, &free0));

    /* a new file, written in pieces, partly over itself */
    op_create("NEU.TXT", OA_EXFAT_ARCH);
    op_write("NEU.TXT", 0, 100, 1);
    img_read(NULL, 0, boot);
    assert(boot[106] & 2);                      /* VolumeDirty while changing */
    op_write("NEU.TXT", 100, 3000, 1);
    op_write("NEU.TXT", 50, 20, 9);
    /* a large contiguous file, two files written in turns (FAT chains) */
    op_create("gross.bin", OA_EXFAT_ARCH);
    op_write("gross.bin", 0, 300000, 2);
    op_create("a.bin", OA_EXFAT_ARCH);
    op_create("b.bin", OA_EXFAT_ARCH);
    for (i = 0; i < 30; ++i) {
        op_write("a.bin", i * 5000UL, 5000, 3);
        op_write("b.bin", i * 5000UL, 5000, 4);
    }
    locate("a.bin");
    assert(!ent.file.nofat);                    /* no longer contiguous */
    /* a directory that grows beyond its first cluster */
    op_create("Neuer Ordner mit langem Namen", OA_EXFAT_DIR);
    for (i = 0; i < 150; ++i) {
        sprintf(name, "Neuer Ordner mit langem Namen/Datei %03u.txt", i);
        op_create(name, OA_EXFAT_ARCH);
        op_write(name, 0, 10 + i, 5 + i);
    }
    /* shorter, longer (zeros up to the new data), after the valid length */
    op_resize("gross.bin", 100000);
    op_resize("NEU.TXT", 10000);
    op_write("NEU.TXT", 12000, 500, 6);
    op_write("sparse.dat", 1000, 10, 7);
    op_resize("a.bin", 0);
    op_write("a.bin", 0, 7000, 8);
    /* names: renamed, moved to another directory, a directory renamed */
    op_rename("klein.txt", "umbenannt.txt");
    op_rename("SUB/A.B", "A.B");
    op_rename("Unterordner mit langem Namen", "Ordner2");
    op_rename("README.TXT", "readme.txt");      /* only the case */
    op_rename("Ordner2/Tiefer/Datei.dat", "Neuer Ordner mit langem Namen/Datei aus Tiefer.dat");
    /* removed: files, empty directories */
    op_remove("zufall.bin");
    op_remove("frag.bin");
    op_remove("SUB");
    op_create("leer", OA_EXFAT_DIR);
    op_remove("leer");
    for (i = 0; i < 150; i += 3) {
        sprintf(name, "Neuer Ordner mit langem Namen/Datei %03u.txt", i);
        op_remove(name);
    }
    /* name hashes through the up-case table */
    op_create("\xC3\x84rger2.txt", OA_EXFAT_ARCH);
    op_write("\xC3\x84rger2.txt", 0, 10, 10);
    op_create("\xC2\xB5m und \xC3\xBF.txt", OA_EXFAT_ARCH);
    op_write("\xC2\xB5m und \xC3\xBF.txt", 0, 10, 11);
    /* attributes and time */
    locate("NEU.TXT");
    CHECK(oa_exfat_attr(&vol, &pdir, ent.pos, OA_EXFAT_RDONLY | OA_EXFAT_ARCH | OA_EXFAT_DIR));
    CHECK(oa_exfat_time(&vol, &pdir, ent.pos, OA_EXFAT_MODIFIED, 0x3905, 0x50A6, 0));
    locate("NEU.TXT");
    assert(ent.attr == (OA_EXFAT_RDONLY | OA_EXFAT_ARCH) && ent.time == 0x3905 && ent.date == 0x50A6);
    locate("Ordner2");
    assert(ent.attr & OA_EXFAT_DIR);

    if (argc > 3) {                             /* until the volume is full */
        static oa_u8 buf[65000];
        oa_u32 off = 0;
        op_create("voll.bin", 0);
        locate("voll.bin");
        f = ent.file;
        oa_exfat_where_of(&pdir, ent.pos, &w);
        memset(buf, 0xA5, sizeof(buf));
        while ((r = oa_exfat_write(&vol, &f, &w, off, buf, sizeof(buf), &done)) == 0) off += done;
        assert(r == OA_EXFAT_EFULL);
        off += done;
        assert(vol.free == 0);
        locate("voll.bin");
        assert(ent.file.size == off);
        op_remove("voll.bin");
    }

    CHECK(oa_exfat_clean(&vol));
    img_read(NULL, 0, boot);
    assert(!(boot[106] & 2));
    free0 = vol.free;                           /* the count kept on the way */
    CHECK(oa_exfat_free(&vol, &free1));
    assert(free0 == free1);
    compare("NEU.TXT");
    compare("gross.bin");
    compare("a.bin");
    compare("b.bin");
    compare("sparse.dat");
    compare("Neuer Ordner mit langem Namen/Datei 149.txt");
    compare("Neuer Ordner mit langem Namen/Datei aus Tiefer.dat");
    compare("\xC3\x84rger2.txt");
    printf("exFAT write test passed: %lu sector writes, %lu clusters free\n", writes, (unsigned long)free1);
    fclose(image);
    return 0;
}
