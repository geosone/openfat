#ifndef OA_EXFAT_CHECK_H
#define OA_EXFAT_CHECK_H
/* exFAT volume check for EXCHK, after the Microsoft exFAT file system
   specification: boot region and its backup, FAT, directory entry sets
   (checksums, name hashes, sizes), cluster chains against the allocation
   bitmap.  With a write function it also repairs what can be repaired.
   The clusters in use are kept in a bit window of the caller; a volume
   with more clusters than the window is checked in several passes. */
#include "openaspi/exfat.h"

#define OA_EXCHK_INFO  0
#define OA_EXCHK_WARN  1
#define OA_EXCHK_ERROR 2
#define OA_EXCHK_FIXED 3                /* the message before has been repaired */

#define OA_EXCHK_MAXSECT 4096

struct oa_exchk {
    /* set by the caller */
    oa_exfat_read_fn read;              /* sectors of the device */
    int (*write)(void *dev, oa_u32 lba, const void OA_FAR *buf);  /* 0: report only */
    void *dev;
    oa_u32 start;                       /* first sector of the volume */
    const struct oa_exfat_cp *cp;       /* for the names in messages */
    void (*msg)(void *ctx, int level, const char *text);
    int (*mark)(void *ctx, oa_u32 bit); /* sets a bit, returns the old one */
    void (*clear)(void *ctx);           /* clears all bits of the window */
    oa_u32 window;                      /* bits of the window */
    void *ctx;
    /* results */
    oa_u32 errors, warnings, fixed;     /* fixed: repairs written */
    oa_u32 clusters, cluster_size;      /* of the volume */
    oa_u32 used, free;                  /* by the allocation bitmap (after repairs) */
    oa_u32 lost;                        /* allocated, but used by nothing */
    oa_u32 unmarked;                    /* used, but free in the bitmap */
    oa_u32 cross;                       /* used twice */
    oa_u32 bad;                         /* marked bad in the FAT */
    oa_u32 files, dirs;
    oa_u16 flags;                       /* VolumeFlags: 2 dirty, 4 media failure */
    oa_u32 serial;
};

/* 0: checked, the results tell; OA_EXFAT_EBAD: no exFAT boot sector;
   OA_EXFAT_EIO: the boot region cannot be read. */
int oa_exchk_run(struct oa_exchk *);
#endif
