/* FAT12/16/32 reading and writing on the host.  First the whole tree of
   an image of tests/fat_image.sh (written by the Linux vfat driver) is
   read and compared with its files (MIRROR); then a series of changes is
   done to the image with the C library and the same way in MIRROR, and the
   tree is compared again.  tests/fat_write.sh then compares both through
   the Linux vfat driver and checks the image with fsck.fat.
   fat_write_test IMAGE MIRROR [FULL|ROOTFULL]
   FULL: also fill the volume until it is full, then remove the file.
   ROOTFULL: fill the fixed root directory of FAT12/16 until it is full. */
#define _XOPEN_SOURCE 500                /* truncate */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "testcp.h"
#include "openfat/fat.h"

static FILE *image;
static const char *mirror;
static struct ofat_fat vol;
static ofat_u8 sector_buf[OFAT_FAT_MAXSECT];
static struct ofat_fat_ent ent;
static unsigned long writes;
static unsigned ssize = 512;

static int img_read(void *dev, ofat_lba lba, void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * ssize, SEEK_SET)) return -1;
    return fread(buf, ssize, 1, image) == 1 ? 0 : -1;
}

static int img_write(void *dev, ofat_lba lba, const void *buf)
{
    (void)dev; ++writes;
    if (fseek(image, (long)lba * ssize, SEEK_SET)) return -1;
    return fwrite(buf, ssize, 1, image) == 1 ? 0 : -1;
}

#define CHECK(x) do { int r_ = (x); if (r_) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

/* UTF-8 to UTF-16 (the BMP only) and back */
static unsigned to16(const char *s, unsigned n, ofat_u16 *out)
{
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    unsigned k = 0;
    while (p < e) {
        if (*p < 0x80) out[k++] = *p++;
        else if (*p < 0xE0) { out[k++] = (ofat_u16)((p[0] & 31) << 6 | (p[1] & 63)); p += 2; }
        else { out[k++] = (ofat_u16)((p[0] & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63)); p += 3; }
    }
    return k;
}

static void to8(const ofat_u16 *s, unsigned n, char *out)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        if (s[i] < 0x80) *out++ = (char)s[i];
        else if (s[i] < 0x800) { *out++ = (char)(0xC0 | s[i] >> 6); *out++ = (char)(0x80 | (s[i] & 63)); }
        else { *out++ = (char)(0xE0 | s[i] >> 12); *out++ = (char)(0x80 | (s[i] >> 6 & 63)); *out++ = (char)(0x80 | (s[i] & 63)); }
    }
    *out = 0;
}

/* The directory of a path ('/' separated), and the last part as UTF-16. */
static ofat_u16 last[256];
static unsigned last_len;

static void parent(const char *path, struct ofat_fat_file *dir)
{
    ofat_u16 name[256];
    const char *p = path, *e;
    unsigned n;
    ofat_fat_rootdir(&vol, dir);
    while ((e = strchr(p, '/')) != NULL) {
        n = to16(p, (unsigned)(e - p), name);
        if (ofat_fat_find(&vol, dir, name, n, &ent) || !(ent.attr & OFAT_FAT_DIR)) {
            fprintf(stderr, "no directory in %s\n", path); exit(1);
        }
        ofat_fat_dirfile(&vol, ent.file.first, dir);
        p = e + 1;
    }
    last_len = to16(p, (unsigned)strlen(p), last);
}

/* the entry of path in ent and its directory */
static struct ofat_fat_file pdir;

static void locate(const char *path)
{
    parent(path, &pdir);
    if (ofat_fat_find(&vol, &pdir, last, last_len, &ent)) { fprintf(stderr, "not found: %s\n", path); exit(1); }
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

static ofat_u8 pattern(unsigned seed, ofat_u32 off)
{
    return (ofat_u8)(seed * 31 + off * 7 + (off >> 8) + (off >> 16));
}

static void op_create(const char *path, ofat_u8 attr)
{
    ofat_u32 pos;
    FILE *f;
    parent(path, &pdir);
    CHECK(ofat_fat_create(&vol, &pdir, last, last_len, attr, 0x5000, 0x5A21, &pos));
    if (attr & OFAT_FAT_DIR) assert(!mkdir(host(path), 0777));
    else { f = fopen(host(path), "wb"); assert(f); fclose(f); }
}

static void op_write(const char *path, ofat_u32 off, ofat_u32 len, unsigned seed)
{
    static ofat_u8 buf[65535];
    struct ofat_fat_where w;
    struct ofat_fat_file f;
    ofat_u32 i, n;
    ofat_u16 done;
    FILE *h;
    locate(path);
    f = ent.file;
    ofat_fat_where_of(&pdir, ent.pos, &w);
    h = fopen(host(path), "r+b");
    assert(h);
    while (len) {
        n = len > 4000 + seed * 13 ? 4000 + seed * 13 : len;   /* odd chunks */
        for (i = 0; i < n; ++i) buf[i] = pattern(seed, off + i);
        CHECK(ofat_fat_write(&vol, &f, &w, off, buf, (ofat_u16)n, &done));
        assert(done == n);
        assert(!fseek(h, (long)off, SEEK_SET) && fwrite(buf, 1, n, h) == n);
        off += n; len -= n;
    }
    fclose(h);
}

static void op_resize(const char *path, ofat_u32 size)
{
    struct ofat_fat_where w;
    struct ofat_fat_file f;
    locate(path);
    f = ent.file;
    ofat_fat_where_of(&pdir, ent.pos, &w);
    CHECK(ofat_fat_resize(&vol, &f, &w, size));
    assert(!truncate(host(path), (off_t)size));
}

static void op_remove(const char *path)
{
    struct ofat_fat_file d;
    locate(path);
    if (ent.attr & OFAT_FAT_DIR) {
        ofat_fat_dirfile(&vol, ent.file.first, &d);
        assert(ofat_fat_empty(&vol, &d) == 1);
        assert(!rmdir(host(path)));
    } else assert(!unlink(host(path)));
    CHECK(ofat_fat_remove(&vol, &pdir, ent.pos));
}

static void op_rename(const char *from, const char *to)
{
    struct ofat_fat_file odir, ndir;
    ofat_u32 opos, npos;
    locate(from);
    odir = pdir; opos = ent.pos;
    parent(to, &ndir);
    CHECK(ofat_fat_rename(&vol, &odir, opos, &ndir, last, last_len, &npos));
    assert(!rename(host(from), host(to)));
}

/* the content of the image's file against the mirror */
static void compare(const char *path)
{
    static ofat_u8 a[4096], b[4096];
    ofat_u32 off = 0;
    ofat_u16 done;
    size_t got;
    FILE *h;
    locate(path);
    h = fopen(host(path), "rb");
    assert(h);
    do {
        CHECK(ofat_fat_read(&vol, &ent.file, off, a, sizeof(a), &done));
        got = fread(b, 1, sizeof(b), h);
        if (got != done || memcmp(a, b, done)) { fprintf(stderr, "differs: %s at %lu\n", path, (unsigned long)off); exit(1); }
        off += done;
    } while (done);
    fclose(h);
}

/* the whole tree below path (a directory, "" the root) against the mirror:
   the same names, every file the same; the number of entries */
static unsigned long tree(const char *path)
{
    struct ofat_fat_file d;
    struct ofat_fat_ent e;
    ofat_u32 pos = 0;
    unsigned long n = 0, hn = 0;
    char name[800], sub[1024];
    struct dirent *de;
    struct stat st;
    DIR *h;
    int r;
    if (*path) { locate(path); ofat_fat_dirfile(&vol, ent.file.first, &d); }
    else ofat_fat_rootdir(&vol, &d);
    while (!(r = ofat_fat_dir_next(&vol, &d, &pos, &e))) {
        to8(e.name, e.name_len, name);
        sprintf(sub, "%s%s%s", path, *path ? "/" : "", name);
        if (stat(host(sub), &st)) { fprintf(stderr, "only in the image: %s\n", sub); exit(1); }
        if (e.attr & OFAT_FAT_DIR) {
            assert(S_ISDIR(st.st_mode));
            n += tree(sub);
        } else {
            assert(S_ISREG(st.st_mode) && (off_t)e.file.size == st.st_size);
            compare(sub);
        }
        ++n; ++hn;
        if (*path) { locate(path); ofat_fat_dirfile(&vol, ent.file.first, &d); }
    }
    assert(r == OFAT_FAT_END);
    /* nothing more in the mirror */
    h = opendir(host(path));
    assert(h);
    while ((de = readdir(h)) != NULL) if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) --hn;
    closedir(h);
    if (hn) { fprintf(stderr, "%s: the mirror has other entries\n", path); exit(1); }
    return n;
}

static int dirty(void)
{
    ofat_u8 b[4096];
    ofat_u32 e1;
    if (vol.bits == 12) return 0;
    img_read(NULL, vol.fat + (vol.bits == 16 ? 2 : 4) / ssize, b);
    e1 = vol.bits == 16 ? (ofat_u32)(b[2] | b[3] << 8)
                        : (ofat_u32)(b[4] | b[5] << 8 | (ofat_u32)b[6] << 16 | (ofat_u32)b[7] << 24);
    return !(e1 & (vol.bits == 16 ? 0x8000UL : 0x08000000UL));
}

int main(int argc, char **argv)
{
    ofat_u32 free0, free1;
    struct ofat_fat_file f, root;
    struct ofat_fat_where w;
    ofat_u16 done, nm[32];
    char name[96];
    unsigned i;
    unsigned long n;
    int r;
    if (argc < 3) { fprintf(stderr, "fat_write_test IMAGE MIRROR [FULL|ROOTFULL]\n"); return 2; }
    image = fopen(argv[1], "r+b");
    mirror = argv[2];
    assert(image);
    {   /* the sector size of the volume */
        ofat_u8 b[512];
        assert(fread(b, 512, 1, image) == 1);
        ssize = (unsigned)(b[11] | b[12] << 8);
    }
    CHECK(ofat_fat_mount(&vol, img_read, NULL, 0, test_cp(850), sector_buf));
    vol.write = img_write;
    assert(!dirty());
    CHECK(ofat_fat_free(&vol, &free0));         /* counted from here on */

    /* what Linux wrote */
    n = tree("");
    printf("FAT%u: %lu entries read\n", vol.bits, n);

    /* a new file, written in pieces, partly over itself */
    op_create("NEU.TXT", OFAT_FAT_ARCH);
    op_write("NEU.TXT", 0, 100, 1);
    assert(vol.bits == 12 || dirty());          /* the dirty bit while changing */
    op_write("NEU.TXT", 100, 3000, 1);
    op_write("NEU.TXT", 50, 20, 9);
    /* short names in lower case: no long name, the NT case bits */
    op_create("klein2.txt", OFAT_FAT_ARCH);
    op_write("klein2.txt", 0, 5, 12);
    locate("klein2.txt");
    assert(ent.lfn == ent.pos && ent.ntcase == 0x18);
    op_create("GROSS.txt", OFAT_FAT_ARCH);
    locate("GROSS.txt");
    assert(ent.lfn == ent.pos && ent.ntcase == 0x10);
    op_create("MiXed.TXT", OFAT_FAT_ARCH);      /* mixed case: a long name, the upper case short name */
    locate("MiXed.TXT");
    assert(ent.lfn != ent.pos && !memcmp(ent.fcb, "MIXED   TXT", 11));
    /* the same name in other case exists already */
    parent("neu.txt", &pdir);
    assert(ofat_fat_create(&vol, &pdir, last, last_len, 0, 0, 0, &free0) == OFAT_FAT_EEXIST);
    to16("bad?name", 8, nm);
    assert(ofat_fat_create(&vol, &pdir, nm, 8, 0, 0, 0, &free0) == OFAT_FAT_ENAME);
    /* a large file, two files written in turns (fragmented chains) */
    op_create("gross.bin", OFAT_FAT_ARCH);
    op_write("gross.bin", 0, 300000, 2);
    op_create("a.bin", OFAT_FAT_ARCH);
    op_create("b.bin", OFAT_FAT_ARCH);
    for (i = 0; i < 30; ++i) {
        op_write("a.bin", i * 5000UL, 5000, 3);
        op_write("b.bin", i * 5000UL, 5000, 4);
    }
    /* a directory that grows beyond its first cluster, long names that
       share their basis (~1 ... ~4, then mangled as in NSS) */
    op_create("Neuer Ordner mit langem Namen", OFAT_FAT_DIR);
    for (i = 0; i < 150; ++i) {
        sprintf(name, "Neuer Ordner mit langem Namen/Datei mit langem Namen %03u.txt", i);
        op_create(name, OFAT_FAT_ARCH);
        op_write(name, 0, 10 + i, 5 + i);
    }
    /* a name of 255 characters */
    memset(name, 0, sizeof(name));
    {
        static char lng[300];
        memset(lng, 'x', 255);
        memcpy(lng, "Neuer Ordner mit langem Namen/", 30);
        memset(lng + 30, 'L', 255);
        lng[30 + 255] = 0;
        op_create(lng, OFAT_FAT_ARCH);
        op_write(lng, 0, 33, 40);
    }
    /* shorter, longer (zeros up to the new data), a gap before written data */
    op_resize("gross.bin", 100000);
    op_resize("NEU.TXT", 10000);
    op_write("NEU.TXT", 12000, 500, 6);
    op_create("sparse.dat", OFAT_FAT_ARCH);
    op_write("sparse.dat", 70000, 10, 7);
    op_resize("a.bin", 0);
    op_write("a.bin", 0, 7000, 8);
    op_resize("b.bin", 4096);
    /* names: renamed, moved to another directory, a directory renamed and moved */
    op_rename("klein.txt", "umbenannt mit langem Namen.txt");
    op_rename("SUB/A.B", "A.B");
    op_rename("Unterordner mit langem Namen", "Ordner2");
    op_rename("README.TXT", "readme.txt");      /* only the case */
    op_rename("Ordner2/Tiefer/Datei.dat", "Neuer Ordner mit langem Namen/Datei aus Tiefer.dat");
    op_rename("Ordner2/Tiefer", "Neuer Ordner mit langem Namen/Tiefer verschoben");
    /* removed: files, empty directories */
    op_remove("zufall.bin");
    op_remove("frag.bin");
    op_remove("SUB");
    op_create("leer", OFAT_FAT_DIR);
    op_remove("leer");
    for (i = 0; i < 150; i += 3) {
        sprintf(name, "Neuer Ordner mit langem Namen/Datei mit langem Namen %03u.txt", i);
        op_remove(name);
    }
    /* code page characters: a long name, and in the short one */
    op_create("\xC3\x84rger2.txt", OFAT_FAT_ARCH);
    op_write("\xC3\x84rger2.txt", 0, 10, 10);
    op_create("\xC2\xB5m und \xC3\xBF.txt", OFAT_FAT_ARCH);
    op_write("\xC2\xB5m und \xC3\xBF.txt", 0, 10, 11);
    /* attributes and time */
    locate("NEU.TXT");
    CHECK(ofat_fat_attr(&vol, &pdir, ent.pos, OFAT_FAT_RDONLY | OFAT_FAT_ARCH | OFAT_FAT_DIR));
    CHECK(ofat_fat_time(&vol, &pdir, ent.pos, OFAT_FAT_MODIFIED, 0x3905, 0x50A6, 0));
    locate("NEU.TXT");
    assert(ent.attr == (OFAT_FAT_RDONLY | OFAT_FAT_ARCH) && ent.time == 0x3905 && ent.date == 0x50A6);
    locate("Ordner2");
    assert(ent.attr & OFAT_FAT_DIR);

    if (argc > 3 && !strcmp(argv[3], "FULL")) {   /* until the volume is full */
        static ofat_u8 buf[65000];
        ofat_u32 off = 0;
        op_create("voll.bin", 0);
        locate("voll.bin");
        f = ent.file;
        ofat_fat_where_of(&pdir, ent.pos, &w);
        memset(buf, 0xA5, sizeof(buf));
        while ((r = ofat_fat_write(&vol, &f, &w, off, buf, sizeof(buf), &done)) == 0) off += done;
        assert(r == OFAT_FAT_EFULL);
        off += done;
        assert(vol.free == 0);
        locate("voll.bin");
        assert(ent.file.size == off);
        op_remove("voll.bin");
    }
    if (argc > 3 && !strcmp(argv[3], "ROOTFULL")) {  /* the fixed root directory */
        ofat_u32 pos;
        assert(vol.bits != 32);
        ofat_fat_rootdir(&vol, &root);
        for (i = 0; ; ++i) {
            sprintf(name, "R%u.TXT", i);
            n = to16(name, (unsigned)strlen(name), nm);
            r = ofat_fat_create(&vol, &root, nm, (unsigned)n, OFAT_FAT_ARCH, 0, 0, &pos);
            if (r == OFAT_FAT_EFULL) break;
            CHECK(r);
            fclose(fopen(host(name), "wb"));
        }
        assert(i > 10 && i < vol.root_entries);
        printf("root directory full after %u files\n", i);
    }

    CHECK(ofat_fat_clean(&vol));
    assert(!dirty());
    free0 = vol.free;                           /* the count kept on the way */
    CHECK(ofat_fat_free(&vol, &free1));
    assert(free0 == 0xFFFFFFFFUL || free0 == free1);
    n = tree("");
    printf("FAT%u write test passed: %lu entries, %lu sector writes, %lu clusters free\n",
           vol.bits, n, writes, (unsigned long)free1);
    fclose(image);
    return 0;
}
