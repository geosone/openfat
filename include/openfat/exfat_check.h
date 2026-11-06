#ifndef OFAT_EXFAT_CHECK_H
#define OFAT_EXFAT_CHECK_H
/* exFAT volume check for EXCHK, after the Microsoft exFAT file system
   specification: boot region and its backup, FAT, directory entry sets
   (checksums, name hashes, sizes), cluster chains against the allocation
   bitmap.  With a write function it also repairs what can be repaired.
   The clusters in use are kept in a bit window of the caller; a volume
   with more clusters than the window is checked in several passes. */
#include "openfat/exfat.h"

#define OFAT_EXCHK_INFO  0
#define OFAT_EXCHK_WARN  1
#define OFAT_EXCHK_ERROR 2
#define OFAT_EXCHK_FIXED 3                /* the message before has been repaired */

#define OFAT_EXCHK_MAXSECT 4096

struct ofat_exchk {
    /* set by the caller */
    ofat_exfat_read_fn read;              /* sectors of the device */
    ofat_exfat_write_fn write;            /* 0: report only */
    void *dev;
    ofat_u32 start;                       /* first sector of the volume (the check
                                             reaches 2^32 sectors from it) */
    const struct ofat_cp *cp;       /* for the names in messages */
    void (*msg)(void *ctx, int level, const char *text);
    int (*mark)(void *ctx, ofat_u32 bit); /* sets a bit, returns the old one */
    void (*clear)(void *ctx);           /* clears all bits of the window */
    ofat_u32 window;                      /* bits of the window */
    void *ctx;
    /* results */
    ofat_u32 errors, warnings, fixed;     /* fixed: repairs written */
    ofat_u32 clusters, cluster_size;      /* of the volume */
    ofat_u32 used, free;                  /* by the allocation bitmap (after repairs) */
    ofat_u32 lost;                        /* allocated, but used by nothing */
    ofat_u32 unmarked;                    /* used, but free in the bitmap */
    ofat_u32 cross;                       /* used twice */
    ofat_u32 bad;                         /* marked bad in the FAT */
    ofat_u32 files, dirs;
    ofat_u16 flags;                       /* VolumeFlags: 2 dirty, 4 media failure */
    ofat_u32 serial;
};

/* 0: checked, the results tell; OFAT_EXFAT_EBAD: no exFAT boot sector;
   OFAT_EXFAT_EIO: the boot region cannot be read. */
int ofat_exchk_run(struct ofat_exchk *);
#endif
