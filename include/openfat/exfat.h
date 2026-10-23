#ifndef OA_EXFAT_H
#define OA_EXFAT_H
/* exFAT reading, independent of I/O and of DOS: after the Microsoft exFAT
   file system specification.  Sectors come from a callback; the DOS driver
   and the host tests give their own. */
#include "openaspi/types.h"

#ifdef __WATCOMC__
#define OA_FAR __far
#else
#define OA_FAR
#endif

#ifndef OA_EXFAT_MAXSECT
#define OA_EXFAT_MAXSECT 512
#endif

#define OA_EXFAT_OK      0
#define OA_EXFAT_END     1      /* end of directory */
#define OA_EXFAT_EIO    -1      /* sector read failed */
#define OA_EXFAT_EBAD   -2      /* not exFAT or damaged */
#define OA_EXFAT_ESIZE  -3      /* sector size larger than OA_EXFAT_MAXSECT */

#define OA_EXFAT_RDONLY 0x01
#define OA_EXFAT_HIDDEN 0x02
#define OA_EXFAT_SYSTEM 0x04
#define OA_EXFAT_DIR    0x10
#define OA_EXFAT_ARCH   0x20

/* Unicode of the OEM characters 80h-FFh and their DOS upper case. */
struct oa_exfat_cp {
    oa_u16 cp;
    oa_u16 uni[128];
    oa_u8 upper[128];
};
extern const struct oa_exfat_cp oa_exfat_cp437, oa_exfat_cp850;

/* Reads one sector (of 1 << sshift bytes) at lba of the device. */
typedef int (*oa_exfat_read_fn)(void *dev, oa_u32 lba, void OA_FAR *buf);

struct oa_exfat {
    oa_exfat_read_fn read;
    void *dev;
    oa_u32 start;               /* first sector of the volume on the device */
    oa_u32 fat, heap, clusters, root, serial;
    oa_u32 bitmap;              /* first cluster of the allocation bitmap */
    oa_u16 flags;               /* VolumeFlags */
    oa_u8 sshift, cshift;       /* bytes per sector, sectors per cluster */
    oa_u8 label_len;
    oa_u16 label[11];
    const struct oa_exfat_cp *cp;
    oa_u32 buf_lba;             /* sector in buf, or 0xFFFFFFFF */
    oa_u8 buf[OA_EXFAT_MAXSECT];
};

/* A file or directory: its cluster chain and the cluster last walked to. */
struct oa_exfat_file {
    oa_u32 first, size, valid;  /* size and valid data length, cut at 4 GB */
    oa_u32 idx, clu;            /* cluster index and number last walked to */
    oa_u8 nofat;                /* contiguous, no FAT chain */
    oa_u8 big;                  /* 4 GB or larger */
    oa_u8 root;                 /* root directory: size from the chain */
};

struct oa_exfat_ent {
    oa_u32 pos;                 /* byte offset of the file entry */
    oa_u16 attr, time, date;
    struct oa_exfat_file file;
    oa_u8 name_len;
    oa_u16 name[255];
};

int oa_exfat_probe(const oa_u8 OA_FAR *boot);
int oa_exfat_mount(struct oa_exfat *, oa_exfat_read_fn, void *dev, oa_u32 start, const struct oa_exfat_cp *);
void oa_exfat_rootdir(const struct oa_exfat *, struct oa_exfat_file *);
int oa_exfat_dir_next(struct oa_exfat *, struct oa_exfat_file *dir, oa_u32 *pos, struct oa_exfat_ent *);
int oa_exfat_read(struct oa_exfat *, struct oa_exfat_file *, oa_u32 off, void OA_FAR *buf, oa_u16 len, oa_u16 *done);
int oa_exfat_free(struct oa_exfat *, oa_u32 *free_clusters);

/* 8.3 names: fcb is the 11-character FCB form ("LETTER~1TXT").  Returns 1
   when the long name had to be changed (it then needs a "~n"). */
int oa_exfat_basis(const struct oa_exfat_cp *, const oa_u16 *name, unsigned len, char fcb[11]);
int oa_exfat_short(struct oa_exfat *, struct oa_exfat_file *dir, const struct oa_exfat_ent *, char fcb[11]);
int oa_exfat_lookup(struct oa_exfat *, struct oa_exfat_file *dir, const char fcb[11], struct oa_exfat_ent *);
#endif
