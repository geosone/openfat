/* DEFRAG on the host:
   defrag_test IMAGE [MODE [SECTORS]]
   MODE: A analyze (default), U only the fragmented files, F full.
   SECTORS: power loss after that many sectors written (exit 3).
   Prints the counts and "gaps": free runs below the last cluster in use. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openaspi/fat_defrag.h"

#define MEM (8UL << 20)

static FILE *image;
static unsigned char *mem;
static unsigned long budget = (unsigned long)-1, written;

static int img_read(void *dev, oa_u32 lba, unsigned count, void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 512, count, image) == count ? 0 : -1;
}

static int img_write(void *dev, oa_u32 lba, unsigned count, const void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    if (written + count > budget) {             /* the power fails within this write */
        if (fwrite(buf, 512, budget - written, image) != budget - written) return -1;
        fclose(image);
        printf("power loss after %lu sectors\n", budget);
        exit(3);
    }
    written += count;
    return fwrite(buf, 512, count, image) == count ? 0 : -1;
}

static void *at(void *ctx, oa_u32 off) { (void)ctx; return mem + off; }

int main(int argc, char **argv)
{
    static oa_u16 order[65535];
    static unsigned char buf[32768];
    struct oa_defrag d;
    oa_u32 c, last = 0, gaps = 0;
    int r, mode = argc > 2 ? argv[2][0] : 'A';
    if (argc < 2) { fputs("defrag_test IMAGE [A|U|F [SECTORS]]\n", stderr); return 2; }
    if (argc > 3) budget = strtoul(argv[3], NULL, 0);
    image = fopen(argv[1], mode == 'A' ? "rb" : "r+b");
    if (!image) { perror(argv[1]); return 2; }
    mem = malloc(MEM);
    memset(&d, 0, sizeof(d));
    d.read = img_read; d.write = img_write;
    d.mode = mode == 'F' ? OA_DEFRAG_FULL : mode == 'U' ? OA_DEFRAG_FILES : OA_DEFRAG_ANALYZE;
    d.mem = at; d.memsize = MEM; d.block = 1UL << 30;
    d.order = order; d.maxitems = 65535;
    d.buf = buf; d.bufsize = sizeof(buf);
    r = oa_defrag_run(&d);
    if (r) { printf("stopped: %d\n", r); return 2; }
    for (c = 2; c < d.clusters + 2; ++c) if (oa_defrag_used(c)) last = c;
    for (c = 2; c < last; ++c) if (!oa_defrag_used(c) && oa_defrag_used(c + 1)) ++gaps;
    printf("FAT%u: %lu files, %lu directories, %lu clusters, %lu used\n", d.bits, (unsigned long)d.files,
           (unsigned long)d.dirs, (unsigned long)d.clusters, (unsigned long)d.used);
    printf("fragmented %lu, after %lu; moves %lu, clusters moved %lu, sectors written %lu; gaps %lu\n",
           (unsigned long)d.fragmented, (unsigned long)d.fragmented_after, (unsigned long)d.moves,
           (unsigned long)d.moved, written, (unsigned long)gaps);
    return 0;
}
