#ifndef OA_FAT_DEFRAG_H
#define OA_FAT_DEFRAG_H
/* FAT12/16/32 defragmentation for DEFRAG.  Files and directories are moved
   whole, in an order that is safe against power loss at any sector write:
   the data is copied to free clusters, the new chain is written to the
   FATs, then the directory entry is switched to it (one sector), the ".."
   entries of moved directories' subdirectories follow, and the old chain is
   freed last.  An interruption leaves at most lost clusters, FATs that
   differ in them and stale ".." entries, which ASPICHK /F repairs; no data
   is lost.  The volume is marked dirty (FAT entry 1, FAT16/32) while it
   runs.  The volume must be free of errors (ASPICHK) before. */
#include "openaspi/types.h"

#ifdef __WATCOMC__
#define OA_DEFRAG_FAR __far
#else
#define OA_DEFRAG_FAR
#endif

#define OA_DEFRAG_FULL    0             /* files contiguous, directories first, free space at the end */
#define OA_DEFRAG_FILES   1             /* only the fragmented files made contiguous */
#define OA_DEFRAG_ANALYZE 2             /* nothing written */

#define OA_DEFRAG_FREE  0               /* states of the map callback */
#define OA_DEFRAG_USED  1
#define OA_DEFRAG_READ  2
#define OA_DEFRAG_WRITE 3

#define OA_DEFRAG_OK    0
#define OA_DEFRAG_EBAD  (-1)            /* no FAT boot sector */
#define OA_DEFRAG_EIO   (-2)            /* read or write error: stopped, the volume is consistent */
#define OA_DEFRAG_EMEM  (-3)            /* too little work memory for the volume */
#define OA_DEFRAG_ESTOP (-4)            /* stopped by the caller (between two moves) */
#define OA_DEFRAG_EDIR  (-5)            /* a directory entry is not as expected (check the volume) */

#define OA_DEFRAG_MAXSECT 4096

struct oa_defrag {
    /* set by the caller */
    int (*read)(void *dev, oa_u32 lba, unsigned count, void OA_DEFRAG_FAR *buf);
    int (*write)(void *dev, oa_u32 lba, unsigned count, const void OA_DEFRAG_FAR *buf);
    void *dev;
    oa_u32 start;                       /* first sector of the volume */
    int mode;
    /* work memory: one bit per cluster and 16 bytes per file; mem returns
       byte off, valid up to the end of its block (block: a power of two,
       at least 16) */
    void OA_DEFRAG_FAR *(*mem)(void *ctx, oa_u32 off);
    oa_u32 memsize, block;
    oa_u16 OA_DEFRAG_FAR *order;        /* maxitems entries, for the files sorted by cluster */
    unsigned maxitems;
    void OA_DEFRAG_FAR *buf;            /* copy buffer */
    unsigned bufsize;                   /* bytes, at least one sector */
    void (*map)(void *ctx, oa_u32 clu, oa_u32 count, int state);   /* 0: no map */
    void (*status)(void *ctx, const char *text);
    int (*stop)(void *ctx);             /* nonzero: stop after the current move */
    void *ctx;
    /* results */
    unsigned bits;
    oa_u32 clusters, cluster_size, used;
    oa_u32 files, dirs;                 /* with clusters */
    oa_u32 fragmented, fragmented_after;
    oa_u32 moves, moved;                /* files moved, clusters copied */
    oa_u32 done;                        /* FULL: clusters 2 .. done-1 are in their final place */
};

int oa_defrag_run(struct oa_defrag *);
/* after oa_defrag_run (also during, from the callbacks): cluster in use */
int oa_defrag_used(oa_u32 clu);
#endif
