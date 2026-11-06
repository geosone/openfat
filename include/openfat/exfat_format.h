#ifndef OFAT_EXFAT_FORMAT_H
#define OFAT_EXFAT_FORMAT_H
/* exFAT formatting for EXFMT, after the Microsoft exFAT file system
   specification: boot region with its backup and checksum, FAT (two of
   them for TexFAT), allocation bitmap, up-case table and root directory
   with the volume label.  512-byte sectors only.  The caller reads the
   medium for bad clusters; they are marked in the FAT and the bitmap, and
   the system clusters go round them. */
#include "openfat/exfat.h"

#define OFAT_EXFMT_UPCASE_LEN 5836
#define OFAT_EXFMT_MINSECT    2048UL      /* 1 MB, the smallest volume */
#define OFAT_EXFMT_MAXBAD     256

extern const ofat_u8 ofat_exfmt_upcase[OFAT_EXFMT_UPCASE_LEN];

struct ofat_exfmt {
    /* set by the caller */
    ofat_u32 sectors;                     /* of the volume */
    ofat_u32 hidden;                      /* its first sector on the medium */
    ofat_u32 cluster;                     /* bytes per cluster, 0: by size as Windows does */
    ofat_u8 nfats;                        /* 1, or 2 for TexFAT */
    ofat_u32 serial;
    ofat_u16 label[11];                   /* UTF-16 */
    ofat_u8 label_len;
    /* by ofat_exfmt_layout */
    ofat_u8 cshift;                       /* sectors per cluster, as a shift */
    ofat_u32 fat_off, fat_len, heap;      /* sectors from the volume start */
    ofat_u32 clusters;
    ofat_u32 bitmap_len;                  /* bytes */
    ofat_u32 sys_clusters;                /* bitmaps, up-case table, root directory */
    ofat_u32 bitmap_first[2], upcase_first, root;  /* by ofat_exfmt_write */
};

/* Writes count sectors at lba of the volume (0 = its boot sector). */
typedef int (*ofat_exfmt_write_fn)(void *dev, ofat_u32 lba, unsigned count, const void OFAT_FAR *buf);

/* 0, or -1: volume too small or too large, wrong cluster size or nfats. */
int ofat_exfmt_layout(struct ofat_exfmt *);
/* Writes the file system.  bad: the bad clusters, ascending; buf: nbuf
   sectors.  The main boot region is written last.  0, -1 a write error,
   -2 the system clusters do not fit between the bad clusters. */
int ofat_exfmt_write(struct ofat_exfmt *, ofat_exfmt_write_fn, void *dev,
                   const ofat_u32 *bad, unsigned nbad, ofat_u8 OFAT_FAR *buf, unsigned nbuf);
#endif
