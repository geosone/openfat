/* The exFAT formatting on the host, as EXFMT does it under DOS:
   exfmt_test IMAGE [CLUSTER [NFATS [LABEL [BAD...]]]]
   formats the whole image file; CLUSTER in bytes (0: by size), NFATS 2:
   TexFAT, LABEL in ASCII ("" none), BAD: bad cluster numbers, ascending. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openaspi/exfat_format.h"

static FILE *image;

static int img_write(void *dev, oa_u32 lba, unsigned count, const void *buf)
{
    (void)dev;
    if (fseek(image, (long)lba * 512, SEEK_SET)) return -1;
    return fwrite(buf, 512, count, image) == count ? 0 : -1;
}

int main(int argc, char **argv)
{
    static oa_u8 buf[16 * 512];
    oa_u32 bad[OA_EXFMT_MAXBAD];
    struct oa_exfmt f;
    unsigned nbad = 0, i;
    int r;
    if (argc < 2) { fputs("exfmt_test IMAGE [CLUSTER [NFATS [LABEL [BAD...]]]]\n", stderr); return 2; }
    image = fopen(argv[1], "r+b");
    if (!image) { perror(argv[1]); return 2; }
    memset(&f, 0, sizeof(f));
    fseek(image, 0, SEEK_END);
    f.sectors = (oa_u32)(ftell(image) / 512);
    f.cluster = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    f.nfats = argc > 3 ? (oa_u8)atoi(argv[3]) : 1;
    f.serial = 0x12345678UL;
    if (argc > 4) for (; argv[4][f.label_len] && f.label_len < 11; ++f.label_len) f.label[f.label_len] = (oa_u8)argv[4][f.label_len];
    for (i = 5; i < (unsigned)argc && nbad < OA_EXFMT_MAXBAD; ++i) bad[nbad++] = strtoul(argv[i], NULL, 0);
    if (oa_exfmt_layout(&f)) { puts("no exFAT layout for this size"); return 1; }
    printf("%lu sectors: FAT at %lu (%lu sectors, %u), heap at %lu, %lu clusters of %lu bytes\n",
           (unsigned long)f.sectors, (unsigned long)f.fat_off, (unsigned long)f.fat_len, f.nfats,
           (unsigned long)f.heap, (unsigned long)f.clusters, 512UL << f.cshift);
    r = oa_exfmt_write(&f, img_write, 0, bad, nbad, buf, 16);
    if (r) { printf(r == -2 ? "no room for the system clusters\n" : "write error\n"); return 1; }
    printf("bitmap %lu, %lu; up-case table %lu; root %lu\n", (unsigned long)f.bitmap_first[0],
           (unsigned long)f.bitmap_first[1], (unsigned long)f.upcase_first, (unsigned long)f.root);
    return fclose(image) ? 1 : 0;
}
