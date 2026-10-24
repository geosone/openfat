#ifndef OA_EXFAT_FORMAT_H
#define OA_EXFAT_FORMAT_H
/* exFAT formatting for EXFMT, after the Microsoft exFAT file system
   specification: boot region with its backup and checksum, FAT (two of
   them for TexFAT), allocation bitmap, up-case table and root directory
   with the volume label.  512-byte sectors only.  The caller reads the
   medium for bad clusters; they are marked in the FAT and the bitmap, and
   the system clusters go round them. */
#include "openaspi/exfat.h"

#define OA_EXFMT_UPCASE_LEN 5836
#define OA_EXFMT_MINSECT    2048UL      /* 1 MB, the smallest volume */
#define OA_EXFMT_MAXBAD     256

extern const oa_u8 oa_exfmt_upcase[OA_EXFMT_UPCASE_LEN];

struct oa_exfmt {
    /* set by the caller */
    oa_u32 sectors;                     /* of the volume */
    oa_u32 hidden;                      /* its first sector on the medium */
    oa_u32 cluster;                     /* bytes per cluster, 0: by size as Windows does */
    oa_u8 nfats;                        /* 1, or 2 for TexFAT */
    oa_u32 serial;
    oa_u16 label[11];                   /* UTF-16 */
    oa_u8 label_len;
    /* by oa_exfmt_layout */
    oa_u8 cshift;                       /* sectors per cluster, as a shift */
    oa_u32 fat_off, fat_len, heap;      /* sectors from the volume start */
    oa_u32 clusters;
    oa_u32 bitmap_len;                  /* bytes */
    oa_u32 sys_clusters;                /* bitmaps, up-case table, root directory */
    oa_u32 bitmap_first[2], upcase_first, root;  /* by oa_exfmt_write */
};

/* Writes count sectors at lba of the volume (0 = its boot sector). */
typedef int (*oa_exfmt_write_fn)(void *dev, oa_u32 lba, unsigned count, const void OA_FAR *buf);

/* 0, or -1: volume too small or too large, wrong cluster size or nfats. */
int oa_exfmt_layout(struct oa_exfmt *);
/* Writes the file system.  bad: the bad clusters, ascending; buf: nbuf
   sectors.  The main boot region is written last.  0, -1 a write error,
   -2 the system clusters do not fit between the bad clusters. */
int oa_exfmt_write(struct oa_exfmt *, oa_exfmt_write_fn, void *dev,
                   const oa_u32 *bad, unsigned nbad, oa_u8 OA_FAR *buf, unsigned nbuf);
#endif
