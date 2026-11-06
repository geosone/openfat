/* Lists the partitions of a disk image: part_test IMAGE [SECTOR_SIZE]
   scheme mbr|gpt|none backup B signature XXXXXXXX
   then per partition: index start count type|type-guid active "name" */
#include <stdio.h>
#include <stdlib.h>
#include "openfat/part.h"

static FILE *image;
static unsigned ssize = 512;

static int img_read(void *dev, ofat_u64 lba, void *buf)
{
    (void)dev;
    if (fseek(image, (long)(lba * ssize), SEEK_SET)) return -1;
    return fread(buf, ssize, 1, image) == 1 ? 0 : -1;
}

static void guid(const ofat_u8 *g)
{
    printf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
           g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
           g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int show(void *ctx, const struct ofat_part *p)
{
    unsigned i;
    (void)ctx;
    printf("%lu %lu %lu ", (unsigned long)p->index, (unsigned long)p->start, (unsigned long)p->count);
    if (p->scheme == OFAT_PART_GPT) guid(p->type_guid); else printf("%02X", p->type);
    printf(" %u \"", p->active);
    for (i = 0; i < 36 && p->name[i]; ++i) putchar(p->name[i] < 0x80 ? p->name[i] : '?');
    printf("\"\n");
    return 0;
}

int main(int argc, char **argv)
{
    static ofat_u8 buf[4096];
    static const char *names[] = { "none", "mbr", "gpt" };
    struct ofat_part_disk d;
    unsigned shift = 9;
    long size;
    int n;
    if (argc < 2) { fprintf(stderr, "part_test IMAGE [SECTOR_SIZE]\n"); return 2; }
    if (argc > 2) ssize = (unsigned)atoi(argv[2]);
    while ((1u << shift) < ssize) ++shift;
    if (!(image = fopen(argv[1], "rb"))) { perror(argv[1]); return 2; }
    fseek(image, 0, SEEK_END);
    size = ftell(image);
    n = ofat_part_scan(img_read, NULL, shift, (ofat_u64)size / ssize, buf, &d, NULL, NULL);
    if (n < 0) { printf("error %d\n", n); return 1; }
    printf("scheme %s backup %u signature %08lX\n", names[d.scheme], d.backup, (unsigned long)d.signature);
    n = ofat_part_scan(img_read, NULL, shift, (ofat_u64)size / ssize, buf, &d, show, NULL);
    return n < 0;
}
