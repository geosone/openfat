/* ofimg: an exFAT volume in a disk image on the host, without mounting
   it, through the openfat library (mtools does not know exFAT).  Builds
   images such as GonWare's installed C: drive.

   ofimg IMAGE START COMMAND... (several in a row)
     format SECTORS [LABEL]  an exFAT volume of SECTORS sectors at START
     mkdir PATH              with the directories above it
     put FILE PATH           the host file as PATH (replaced if there, the
                             name keeping its case); PATH ending in '/' or
                             '\' takes the name of FILE

   START is the first sector of the volume in the image (a partition), 0
   for a superfloppy.  Paths are UTF-8, '/' or '\' separated, compared
   without case for ASCII letters as the exFAT up-case table does. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "openfat/exfat.h"
#include "openfat/exfat_format.h"

static FILE *image;
static unsigned long start;
static struct ofat_exfat vol;
static int mounted;
static ofat_u8 sector_buf[OFAT_EXFAT_MAXSECT];
static struct ofat_exfat_ent ent;
static ofat_u16 now_time, now_date;

static int img_read(void *dev, ofat_lba lba, void *buf)
{
    (void)dev;
    if (fseeko(image, (off_t)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 512, 1, image) == 1 ? 0 : -1;
}

static int img_write(void *dev, ofat_lba lba, const void *buf)
{
    (void)dev;
    if (fseeko(image, (off_t)lba * 512, SEEK_SET)) return -1;
    return fwrite(buf, 512, 1, image) == 1 ? 0 : -1;
}

/* the formatter counts from the volume's boot sector */
static int fmt_write(void *dev, ofat_u32 lba, unsigned count, const void *buf)
{
    (void)dev;
    if (fseeko(image, ((off_t)start + lba) * 512, SEEK_SET)) return -1;
    return fwrite(buf, 512, count, image) == count ? 0 : -1;
}

static void fail(const char *what, const char *arg, int r)
{
    fprintf(stderr, "ofimg: %s %s (%d)\n", what, arg, r);
    exit(1);
}

/* UTF-8 to UTF-16 (the BMP); 0 for a bad or too long name */
static unsigned to16(const char *s, unsigned n, ofat_u16 *out)
{
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    unsigned k = 0;
    while (p < e && k < 255) {
        if (*p < 0x80) out[k++] = *p++;
        else if (*p >= 0xC0 && *p < 0xE0 && p + 1 < e) { out[k++] = (ofat_u16)((p[0] & 31) << 6 | (p[1] & 63)); p += 2; }
        else if (*p >= 0xE0 && *p < 0xF0 && p + 2 < e) {
            out[k++] = (ofat_u16)((p[0] & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63)); p += 3;
        } else return 0;
    }
    return p == e ? k : 0;
}

static ofat_u16 up(ofat_u16 c) { return c >= 'a' && c <= 'z' ? (ofat_u16)(c - 32) : c; }

/* the entry of name in dir into ent: 0, or 1 when there is none */
static int find(struct ofat_exfat_file *dir, const ofat_u16 *name, unsigned len)
{
    ofat_u32 pos = 0;
    unsigned i;
    int r;
    while (!(r = ofat_exfat_dir_next(&vol, dir, &pos, &ent))) {
        if (ent.name_len != len) continue;
        for (i = 0; i < len && up(ent.name[i]) == up(name[i]); ++i)
            ;
        if (i == len) return 0;
    }
    if (r != OFAT_EXFAT_END) fail("cannot read a directory", "", r);
    return 1;
}

static void as_dir(struct ofat_exfat_file *dir)
{
    *dir = ent.file;
    if (!dir->nofat) dir->root = 1;
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* Walks to the directory of path's last part, making the directories on
   the way when make is set; the last part into name. */
static unsigned walk(const char *path, int make, struct ofat_exfat_file *dir, struct ofat_exfat_where *dw,
                     ofat_u16 *name)
{
    const char *p = path, *e;
    unsigned n;
    ofat_u32 pos;
    int r;
    ofat_exfat_rootdir(&vol, dir);
    memset(dw, 0, sizeof(*dw));
    dw->none = 1;
    for (;;) {
        while (is_sep(*p)) ++p;
        for (e = p; *e && !is_sep(*e); ++e)
            ;
        if (!*e) break; /* the last part */
        if (!(n = to16(p, (unsigned)(e - p), name))) fail("bad name in", path, 0);
        if (find(dir, name, n)) {
            if (!make) fail("no directory", path, 0);
            if ((r = ofat_exfat_create(&vol, dir, dw, name, n, OFAT_EXFAT_DIR, now_time, now_date, &pos)))
                fail("cannot make a directory for", path, r);
            if (find(dir, name, n)) fail("lost the new directory of", path, 0);
        } else if (!(ent.attr & OFAT_EXFAT_DIR)) fail("not a directory in", path, 0);
        ofat_exfat_where_of(dir, ent.pos, dw);
        as_dir(dir);
        p = e;
    }
    return *p ? to16(p, (unsigned)strlen(p), name) : 0;
}

static void need_mount(void)
{
    int r;
    if (mounted) return;
    if ((r = ofat_exfat_mount(&vol, img_read, NULL, (ofat_lba)start, NULL, sector_buf)))
        fail("no exFAT volume at sector", "START", r);
    vol.write = img_write;
    mounted = 1;
}

static void do_format(unsigned long sectors, const char *label)
{
    static ofat_u8 buf[16 * 512];
    struct ofat_exfmt f;
    int r;
    memset(&f, 0, sizeof(f));
    f.sectors = (ofat_u32)sectors;
    f.hidden = (ofat_u32)start;
    f.nfats = 1;
    f.serial = (ofat_u32)time(NULL);
    if (label) f.label_len = (ofat_u8)to16(label, (unsigned)(strlen(label) > 11 ? 11 : strlen(label)), f.label);
    if (ofat_exfmt_layout(&f)) fail("no exFAT layout for", "SECTORS", 0);
    if ((r = ofat_exfmt_write(&f, fmt_write, NULL, NULL, 0, buf, 16))) fail("cannot format", "", r);
    mounted = 0;
}

static void do_mkdir(const char *path)
{
    struct ofat_exfat_file dir;
    struct ofat_exfat_where dw;
    ofat_u16 name[256];
    ofat_u32 pos;
    unsigned n;
    int r;
    need_mount();
    n = walk(path, 1, &dir, &dw, name);
    if (!n) return; /* ended in a separator: made on the way */
    if (!find(&dir, name, n)) {
        if (!(ent.attr & OFAT_EXFAT_DIR)) fail("a file is in the way of", path, 0);
        return;
    }
    if ((r = ofat_exfat_create(&vol, &dir, &dw, name, n, OFAT_EXFAT_DIR, now_time, now_date, &pos)))
        fail("cannot make", path, r);
}

static void do_put(const char *file, const char *path)
{
    static ofat_u8 buf[32768];
    struct ofat_exfat_file dir, f;
    struct ofat_exfat_where dw, w;
    ofat_u16 name[256];
    ofat_u32 pos;
    ofat_fsize off = 0;
    ofat_u16 done;
    unsigned n;
    size_t got;
    FILE *h;
    int r;
    need_mount();
    n = walk(path, 1, &dir, &dw, name);
    if (!n) { /* into the directory: the host file's name */
        const char *b = file + strlen(file);
        while (b > file && !is_sep(b[-1])) --b;
        if (!(n = to16(b, (unsigned)strlen(b), name))) fail("bad name", b, 0);
    }
    if (!find(&dir, name, n)) {
        if (ent.attr & OFAT_EXFAT_DIR) fail("a directory is in the way of", path, 0);
        /* replaced: the name keeps its case, as on DOS */
        n = ent.name_len;
        memcpy(name, ent.name, n * sizeof(name[0]));
        if ((r = ofat_exfat_remove(&vol, &dir, ent.pos))) fail("cannot replace", path, r);
    }
    if ((r = ofat_exfat_create(&vol, &dir, &dw, name, n, OFAT_EXFAT_ARCH, now_time, now_date, &pos)))
        fail("cannot create", path, r);
    if (find(&dir, name, n)) fail("lost the new file", path, 0);
    f = ent.file;
    ofat_exfat_where_of(&dir, ent.pos, &w);
    if (!(h = fopen(file, "rb"))) { perror(file); exit(1); }
    while ((got = fread(buf, 1, sizeof(buf), h)) > 0) {
        if ((r = ofat_exfat_write(&vol, &f, &w, off, buf, (ofat_u16)got, &done)) || done != got)
            fail("cannot write", path, r);
        off += got;
    }
    fclose(h);
}

int main(int argc, char **argv)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    int i, r;

    if (argc < 4) {
        fputs("ofimg IMAGE START [format SECTORS [LABEL]] [mkdir PATH] [put FILE PATH] ...\n", stderr);
        return 2;
    }
    now_time = (ofat_u16)(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2);
    now_date = (ofat_u16)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday);
    if (!(image = fopen(argv[1], "r+b"))) { perror(argv[1]); return 1; }
    start = strtoul(argv[2], NULL, 0);
    for (i = 3; i < argc; ) {
        if (!strcmp(argv[i], "format") && i + 1 < argc) {
            int label = i + 2 < argc && strcmp(argv[i + 2], "mkdir") && strcmp(argv[i + 2], "put");
            do_format(strtoul(argv[i + 1], NULL, 0), label ? argv[i + 2] : NULL);
            i += label ? 3 : 2;
        } else if (!strcmp(argv[i], "mkdir") && i + 1 < argc) {
            do_mkdir(argv[i + 1]);
            i += 2;
        } else if (!strcmp(argv[i], "put") && i + 2 < argc) {
            do_put(argv[i + 1], argv[i + 2]);
            i += 3;
        } else {
            fprintf(stderr, "ofimg: what is %s?\n", argv[i]);
            return 2;
        }
    }
    if (mounted && (r = ofat_exfat_clean(&vol))) fail("cannot mark the volume clean", "", r);
    return fclose(image) ? 1 : 0;
}
