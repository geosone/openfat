/* exFAT reading on the host: lists an image made by mkfs.exfat with 8.3
   names, size, attributes and DOS time like DIR, or prints a file.
   exfat_test                    built-in name tests
   exfat_test IMAGE [LBA]        list all directories (recursive)
   exfat_test IMAGE LBA PATH     print a file, PATH in 8.3 names, "\" separated */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openaspi/exfat.h"

static FILE *image;
static unsigned long reads;

static int img_read(void *dev, oa_u32 lba, void *buf)
{
    (void)dev; ++reads;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 512, 1, image) == 1 ? 0 : -1;
}

static struct oa_exfat vol;

static void name83(const char fcb[11], char *out)
{
    unsigned i, n = 0;
    for (i = 0; i < 8 && fcb[i] != ' '; ++i) out[n++] = fcb[i];
    if (fcb[8] != ' ') { out[n++] = '.'; for (i = 8; i < 11 && fcb[i] != ' '; ++i) out[n++] = fcb[i]; }
    out[n] = 0;
}

static void utf8(const oa_u16 *s, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        if (s[i] < 0x80) putchar(s[i]);
        else if (s[i] < 0x800) { putchar(0xC0 | s[i] >> 6); putchar(0x80 | (s[i] & 63)); }
        else { putchar(0xE0 | s[i] >> 12); putchar(0x80 | (s[i] >> 6 & 63)); putchar(0x80 | (s[i] & 63)); }
    }
}

static void list(struct oa_exfat_file *dir, const char *path)
{
    static struct oa_exfat_ent ent;
    struct oa_exfat_file sub;
    oa_u32 pos = 0, next;
    char fcb[11], name[13], subpath[260];
    int r;
    printf("%s\n", path);
    while (!(r = oa_exfat_dir_next(&vol, dir, &pos, &ent))) {
        next = pos;
        assert(!oa_exfat_short(&vol, dir, &ent, fcb));
        name83(fcb, name);
        printf("  %-12s %02X %10lu%s %02u.%02u.%04u %02u:%02u  ", name, (unsigned)ent.attr,
               (unsigned long)ent.file.size, ent.file.big ? "+" : " ",
               ent.date & 31, ent.date >> 5 & 15, 1980 + (ent.date >> 9), ent.time >> 11, ent.time >> 5 & 63);
        utf8(ent.name, ent.name_len);
        putchar('\n');
        if (ent.attr & OA_EXFAT_DIR) {
            sub = ent.file;
            sprintf(subpath, "%s%s\\", path, name);
            list(&sub, subpath);
            pos = next;
        }
    }
    assert(r == OA_EXFAT_END);
}

static void cat(const char *path)
{
    static struct oa_exfat_ent ent;
    struct oa_exfat_file dir;
    char fcb[11], part[13];
    static char buf[4096];
    const char *p = path, *e;
    oa_u32 off = 0;
    oa_u16 done;
    unsigned i, n;
    oa_exfat_rootdir(&vol, &dir);
    for (;;) {
        e = strchr(p, '\\');
        n = e ? (unsigned)(e - p) : (unsigned)strlen(p);
        memcpy(part, p, n); part[n] = 0;
        memset(fcb, ' ', 11);
        e = strchr(part, '.');
        for (i = 0; part[i] && part + i != e && i < 8; ++i) fcb[i] = part[i];
        if (e) for (i = 0; e[1+i] && i < 3; ++i) fcb[8+i] = e[1+i];
        if (oa_exfat_lookup(&vol, &dir, fcb, &ent)) { fprintf(stderr, "not found: %s\n", part); exit(1); }
        p += n;
        if (!*p) break;
        ++p; dir = ent.file;
    }
    do {
        assert(!oa_exfat_read(&vol, &ent.file, off, buf, sizeof(buf), &done));
        fwrite(buf, 1, done, stdout);
        off += done;
    } while (done);
}

static void basis(const char *ascii, const char *want, int lossy)
{
    oa_u16 name[64];
    char fcb[11];
    unsigned n;
    for (n = 0; ascii[n]; ++n) name[n] = (oa_u8)ascii[n];
    assert(oa_exfat_basis(&oa_exfat_cp850, name, n, fcb) == lossy);
    assert(!memcmp(fcb, want, 11));
}

int main(int argc, char **argv)
{
    static const oa_u16 umlaut[] = { 0x00E4, 'r', 'g', 'e', 'r', '.', 't', 'x', 't' };
    static const oa_u16 euro[] = { 0x20AC, '.', 'T', 'X', 'T' };
    oa_u32 start = argc > 2 ? strtoul(argv[2], NULL, 0) : 0, free_clusters;
    struct oa_exfat_file root;
    char fcb[11];
    int r;
    basis("README.TXT", "README  TXT", 0);
    basis("readme.txt", "README  TXT", 0);
    basis("Ein langer Name.txt", "EINLANGETXT", 1);
    basis("archive.tar.gz", "ARCHIVETGZ ", 1);
    basis(".bashrc", "BASHRC     ", 1);
    basis("a+b=c.txt", "A_B_C   TXT", 1);
    basis("NOEXT", "NOEXT      ", 0);
    basis("x.html", "X       HTM", 1);
    assert(!oa_exfat_basis(&oa_exfat_cp850, umlaut, 9, fcb) && !memcmp(fcb, "\x8ERGER   TXT", 11));
    assert(!oa_exfat_basis(&oa_exfat_cp437, umlaut, 9, fcb) && !memcmp(fcb, "\x8ERGER   TXT", 11));
    assert(oa_exfat_basis(&oa_exfat_cp850, euro, 5, fcb) && !memcmp(fcb, "_       TXT", 11));
    if (argc < 2) { puts("exFAT name tests passed"); return 0; }
    image = fopen(argv[1], "rb");
    assert(image);
    r = oa_exfat_mount(&vol, img_read, NULL, start, &oa_exfat_cp850);
    if (r) { fprintf(stderr, "mount: %d\n", r); return 1; }
    if (argc > 3) { cat(argv[3]); return 0; }
    assert(!oa_exfat_free(&vol, &free_clusters));
    printf("label \""); utf8(vol.label, vol.label_len);
    printf("\" serial %04lX-%04lX, %lu of %lu clusters of %u bytes free\n",
           (unsigned long)(vol.serial >> 16), (unsigned long)(vol.serial & 0xFFFF),
           (unsigned long)free_clusters, (unsigned long)vol.clusters, 512u << vol.cshift);
    oa_exfat_rootdir(&vol, &root);
    list(&root, "\\");
    fprintf(stderr, "%lu sector reads\n", reads);
    return 0;
}
