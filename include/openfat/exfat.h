#ifndef OA_EXFAT_H
#define OA_EXFAT_H
/* exFAT reading and writing, independent of I/O and of DOS: after the
   Microsoft exFAT file system specification.  Sectors come from callbacks;
   the DOS driver and the host tests give their own.  Writing keeps an order
   that leaves at worst lost clusters behind: the allocation bitmap first,
   then the FAT, the data, and the directory entry last. */
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
#define OA_EXFAT_EFULL  -4      /* no free cluster */
#define OA_EXFAT_EWRITE -5      /* sector write failed, or no write function */

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
/* Writes one sector at lba of the device. */
typedef int (*oa_exfat_write_fn)(void *dev, oa_u32 lba, const void OA_FAR *buf);

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
    oa_u8 *buf;                 /* OA_EXFAT_MAXSECT bytes; volumes may share
                                   one when they set buf_lba to 0xFFFFFFFF
                                   after another one used it */
    /* writing: set write after oa_exfat_mount; 0 keeps the volume read-only */
    oa_exfat_write_fn write;
    oa_u32 fat2, bitmap2;       /* TexFAT: the inactive FAT and bitmap, kept
                                   equal to the active ones; else 0 */
    oa_u32 upcase, upcase_len;  /* the up-case table, for name hashes */
    oa_u32 hint;                /* the next free cluster is looked for here */
    oa_u32 free;                /* free clusters, 0FFFFFFFFh: not counted */
    oa_u8 dirty;                /* VolumeDirty: 1 set by us, 2 set at mount */
};

/* A file or directory: its cluster chain and the cluster last walked to. */
struct oa_exfat_file {
    oa_u32 first, size, valid;  /* size and valid data length, cut at 4 GB */
    oa_u32 idx, clu;            /* cluster index and number last walked to */
    oa_u8 nofat;                /* contiguous, no FAT chain */
    oa_u8 big;                  /* 4 GB or larger */
    oa_u8 root;                 /* root directory: size from the chain */
};

/* Where the entry set of a file or directory is: the directory (as in
   struct oa_exfat_file) and the offset in it; none for the root directory. */
struct oa_exfat_where {
    oa_u32 first, size, pos;
    oa_u8 nofat, none;
};

struct oa_exfat_ent {
    oa_u32 pos;                 /* byte offset of the file entry */
    oa_u16 attr, time, date;
    struct oa_exfat_file file;
    oa_u8 name_len;
    oa_u16 name[255];
};

int oa_exfat_probe(const oa_u8 OA_FAR *boot);
int oa_exfat_mount(struct oa_exfat *, oa_exfat_read_fn, void *dev, oa_u32 start, const struct oa_exfat_cp *, oa_u8 *buf);
void oa_exfat_rootdir(const struct oa_exfat *, struct oa_exfat_file *);
int oa_exfat_dir_next(struct oa_exfat *, struct oa_exfat_file *dir, oa_u32 *pos, struct oa_exfat_ent *);
int oa_exfat_read(struct oa_exfat *, struct oa_exfat_file *, oa_u32 off, void OA_FAR *buf, oa_u16 len, oa_u16 *done);
int oa_exfat_free(struct oa_exfat *, oa_u32 *free_clusters);

/* Writing.  dir is a directory as given by oa_exfat_rootdir or by an entry
   (a FAT chained one with root set); dw the place of its own entry, which
   changes when the directory grows.  Names are UTF-16, 1-255 characters,
   checked by the caller.  Each call leaves the volume consistent;
   VolumeDirty stays set until oa_exfat_clean. */
void oa_exfat_where_of(const struct oa_exfat_file *dir, oa_u32 pos, struct oa_exfat_where *);
void oa_exfat_where_dir(const struct oa_exfat_where *, struct oa_exfat_file *dir);
/* Writes len bytes at off, from valid on with zeros before off; the file
   grows.  *done may be less than len: OA_EXFAT_EFULL. */
int oa_exfat_write(struct oa_exfat *, struct oa_exfat_file *, const struct oa_exfat_where *,
                   oa_u32 off, const void OA_FAR *buf, oa_u16 len, oa_u16 *done);
/* The size: shorter frees clusters, longer allocates them (read as zeros). */
int oa_exfat_resize(struct oa_exfat *, struct oa_exfat_file *, const struct oa_exfat_where *, oa_u32 size);
/* A new empty file, or with OA_EXFAT_DIR a directory with one cluster;
   *pos its entry set. */
int oa_exfat_create(struct oa_exfat *, struct oa_exfat_file *dir, struct oa_exfat_where *dw,
                    const oa_u16 *name, unsigned len, oa_u16 attr, oa_u16 time, oa_u16 date, oa_u32 *pos);
/* Removes the entry set at pos and frees its clusters (a directory must
   be empty: oa_exfat_empty). */
int oa_exfat_remove(struct oa_exfat *, struct oa_exfat_file *dir, oa_u32 pos);
/* 1: no entries in the directory, 0: some, or an error. */
int oa_exfat_empty(struct oa_exfat *, struct oa_exfat_file *dir);
/* The entry set at pos gets a new name, in the directory to (may be dir). */
int oa_exfat_rename(struct oa_exfat *, struct oa_exfat_file *dir, oa_u32 pos,
                    struct oa_exfat_file *to, struct oa_exfat_where *tw,
                    const oa_u16 *name, unsigned len, oa_u32 *newpos);
/* Attributes (the directory bit is kept). */
int oa_exfat_attr(struct oa_exfat *, struct oa_exfat_file *dir, oa_u32 pos, oa_u16 attr);
/* A time stamp: OA_EXFAT_CREATED, _MODIFIED or _ACCESSED; cs10 10 ms units. */
#define OA_EXFAT_CREATED  8
#define OA_EXFAT_MODIFIED 12
#define OA_EXFAT_ACCESSED 16
int oa_exfat_time(struct oa_exfat *, struct oa_exfat_file *dir, oa_u32 pos, unsigned which,
                  oa_u16 time, oa_u16 date, oa_u8 cs10);
/* VolumeDirty cleared (unless it was set before the mount), PercentInUse. */
int oa_exfat_clean(struct oa_exfat *);

/* 8.3 names: fcb is the 11-character FCB form ("LETTER~1TXT").  Returns 1
   when the long name had to be changed (it then needs a "~n"). */
int oa_exfat_basis(const struct oa_exfat_cp *, const oa_u16 *name, unsigned len, char fcb[11]);
int oa_exfat_short(struct oa_exfat *, struct oa_exfat_file *dir, const struct oa_exfat_ent *, char fcb[11]);
int oa_exfat_lookup(struct oa_exfat *, struct oa_exfat_file *dir, const char fcb[11], struct oa_exfat_ent *);
#endif
