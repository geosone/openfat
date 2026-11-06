/* The FAT check on the host, as ASPICHK does it under DOS:
   fatchk_test IMAGE [LBA [WINDOW [F]]]
   WINDOW: clusters per pass (0: all); F: repair (ASPICHK /F); exit 0 clean,
   1 errors, 2 not FAT or unreadable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openfat/fat_check.h"
#include "openfat/exfat.h"           /* the code page tables */

static FILE *image;
static unsigned char *bits;
static unsigned ssize = 512;            /* from the boot sector */

static int img_read(void *dev, ofat_u32 lba, void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * ssize, SEEK_SET)) return -1;
    return fread(buf, ssize, 1, image) == 1 ? 0 : -1;
}

static int img_write(void *dev, ofat_u32 lba, const void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * ssize, SEEK_SET)) return -1;
    return fwrite(buf, ssize, 1, image) == 1 ? 0 : -1;
}

static void msg(void *ctx, int level, const char *text)
{
    (void)ctx;
    printf("%s%s\n", level == OFAT_FATCHK_ERROR ? "ERROR: " : level == OFAT_FATCHK_WARN ? "WARNING: " :
           level == OFAT_FATCHK_FIXED ? "  FIXED: " : "", text);
}

static int mark(void *ctx, ofat_u32 bit)
{
    int old = bits[bit >> 3] >> (bit & 7) & 1;
    (void)ctx;
    bits[bit >> 3] |= (unsigned char)(1 << (bit & 7));
    return old;
}

static ofat_u32 window;
static void clear(void *ctx) { (void)ctx; memset(bits, 0, window / 8 + 1); }

int main(int argc, char **argv)
{
    struct ofat_fatchk c;
    int r;
    int fix = argc > 4 && argv[4][0] == 'F';
    if (argc < 2) { fputs("fatchk_test IMAGE [LBA [WINDOW [F]]]\n", stderr); return 2; }
    image = fopen(argv[1], fix ? "r+b" : "rb");
    if (!image) { perror(argv[1]); return 2; }
    memset(&c, 0, sizeof(c));
    c.read = img_read; c.cp = ofat_cp850.uni; c.msg = msg; c.mark = mark; c.clear = clear;
    c.start = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    {   /* the sector size of the volume (LBA in 512-byte units until known) */
        unsigned char b[512];
        unsigned bps;
        if (fseek(image, (long)c.start * 512, SEEK_SET) || fread(b, 512, 1, image) != 1) return 2;
        bps = (unsigned)(b[11] | b[12] << 8);
        if (bps >= 512 && bps <= OFAT_FATCHK_MAXSECT && !(bps & (bps - 1))) {
            ssize = bps;
            c.start = c.start * 512 / ssize;
        }
    }
    window = argc > 3 && strtoul(argv[3], NULL, 0) ? strtoul(argv[3], NULL, 0) : 1UL << 28;
    if (fix) c.write = img_write;
    c.window = window;
    bits = malloc(window / 8 + 1);
    r = ofat_fatchk_run(&c);
    if (r) { printf("not checked: %s\n", r == OFAT_FATCHK_EIO ? "read error" : "no FAT"); return 2; }
    printf("FAT%u: %lu files, %lu directories; %lu clusters of %lu bytes, %lu used, %lu free\n", c.bits,
           (unsigned long)c.files, (unsigned long)c.dirs, (unsigned long)c.clusters,
           (unsigned long)c.cluster_size, (unsigned long)c.used, (unsigned long)c.free);
    printf("%lu errors, %lu warnings, %lu fixed; lost %lu, cross-linked %lu, bad %lu\n",
           (unsigned long)c.errors, (unsigned long)c.warnings, (unsigned long)c.fixed, (unsigned long)c.lost,
           (unsigned long)c.cross, (unsigned long)c.bad);
    return c.errors ? 1 : 0;
}
