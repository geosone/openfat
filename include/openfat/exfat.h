#ifndef OFAT_EXFAT_H
#define OFAT_EXFAT_H
/* exFAT reading and writing, independent of I/O and of DOS: after the
   Microsoft exFAT file system specification.  Sectors come from callbacks;
   the DOS driver and the host tests give their own.  Writing keeps an order
   that leaves at worst lost clusters behind: the allocation bitmap first,
   then the FAT, the data, and the directory entry last. */
#include "openfat/types.h"

#ifdef __WATCOMC__
#define OFAT_FAR __far
#else
#define OFAT_FAR
#endif

#ifndef OFAT_EXFAT_MAXSECT
#define OFAT_EXFAT_MAXSECT 512
#endif

#define OFAT_EXFAT_OK      0
#define OFAT_EXFAT_END     1      /* end of directory */
#define OFAT_EXFAT_EIO    -1      /* sector read failed */
#define OFAT_EXFAT_EBAD   -2      /* not exFAT or damaged */
#define OFAT_EXFAT_ESIZE  -3      /* sector size larger than OFAT_EXFAT_MAXSECT */
#define OFAT_EXFAT_EFULL  -4      /* no free cluster */
#define OFAT_EXFAT_EWRITE -5      /* sector write failed, or no write function */

#define OFAT_EXFAT_RDONLY 0x01
#define OFAT_EXFAT_HIDDEN 0x02
#define OFAT_EXFAT_SYSTEM 0x04
#define OFAT_EXFAT_DIR    0x10
#define OFAT_EXFAT_ARCH   0x20

/* Unicode of the OEM characters 80h-FFh and their DOS upper case. */
struct ofat_exfat_cp {
    ofat_u16 cp;
    ofat_u16 uni[128];
    ofat_u8 upper[128];
};
extern const struct ofat_exfat_cp ofat_exfat_cp437, ofat_exfat_cp850;

/* Reads one sector (of 1 << sshift bytes) at lba of the device. */
typedef int (*ofat_exfat_read_fn)(void *dev, ofat_u32 lba, void OFAT_FAR *buf);
/* Writes one sector at lba of the device. */
typedef int (*ofat_exfat_write_fn)(void *dev, ofat_u32 lba, const void OFAT_FAR *buf);

struct ofat_exfat {
    ofat_exfat_read_fn read;
    void *dev;
    ofat_u32 start;               /* first sector of the volume on the device */
    ofat_u32 fat, heap, clusters, root, serial;
    ofat_u32 bitmap;              /* first cluster of the allocation bitmap */
    ofat_u16 flags;               /* VolumeFlags */
    ofat_u8 sshift, cshift;       /* bytes per sector, sectors per cluster */
    ofat_u8 label_len;
    ofat_u16 label[11];
    const struct ofat_exfat_cp *cp;
    ofat_u32 buf_lba;             /* sector in buf, or 0xFFFFFFFF */
    ofat_u8 *buf;                 /* OFAT_EXFAT_MAXSECT bytes; volumes may share
                                   one when they set buf_lba to 0xFFFFFFFF
                                   after another one used it */
    /* writing: set write after ofat_exfat_mount; 0 keeps the volume read-only */
    ofat_exfat_write_fn write;
    ofat_u32 fat2, bitmap2;       /* TexFAT: the inactive FAT and bitmap, kept
                                   equal to the active ones; else 0 */
    ofat_u32 upcase, upcase_len;  /* the up-case table, for name hashes */
    ofat_u32 hint;                /* the next free cluster is looked for here */
    ofat_u32 free;                /* free clusters, 0FFFFFFFFh: not counted */
    ofat_u8 dirty;                /* VolumeDirty: 1 set by us, 2 set at mount */
};

/* A file or directory: its cluster chain and the cluster last walked to. */
struct ofat_exfat_file {
    ofat_u32 first, size, valid;  /* size and valid data length, cut at 4 GB */
    ofat_u32 idx, clu;            /* cluster index and number last walked to */
    ofat_u8 nofat;                /* contiguous, no FAT chain */
    ofat_u8 big;                  /* 4 GB or larger */
    ofat_u8 root;                 /* root directory: size from the chain */
};

/* Where the entry set of a file or directory is: the directory (as in
   struct ofat_exfat_file) and the offset in it; none for the root directory. */
struct ofat_exfat_where {
    ofat_u32 first, size, pos;
    ofat_u8 nofat, none;
};

struct ofat_exfat_ent {
    ofat_u32 pos;                 /* byte offset of the file entry */
    ofat_u16 attr, time, date;
    struct ofat_exfat_file file;
    ofat_u8 name_len;
    ofat_u16 name[255];
};

int ofat_exfat_probe(const ofat_u8 OFAT_FAR *boot);
int ofat_exfat_mount(struct ofat_exfat *, ofat_exfat_read_fn, void *dev, ofat_u32 start, const struct ofat_exfat_cp *, ofat_u8 *buf);
void ofat_exfat_rootdir(const struct ofat_exfat *, struct ofat_exfat_file *);
int ofat_exfat_dir_next(struct ofat_exfat *, struct ofat_exfat_file *dir, ofat_u32 *pos, struct ofat_exfat_ent *);
int ofat_exfat_read(struct ofat_exfat *, struct ofat_exfat_file *, ofat_u32 off, void OFAT_FAR *buf, ofat_u16 len, ofat_u16 *done);
int ofat_exfat_free(struct ofat_exfat *, ofat_u32 *free_clusters);

/* Writing.  dir is a directory as given by ofat_exfat_rootdir or by an entry
   (a FAT chained one with root set); dw the place of its own entry, which
   changes when the directory grows.  Names are UTF-16, 1-255 characters,
   checked by the caller.  Each call leaves the volume consistent;
   VolumeDirty stays set until ofat_exfat_clean. */
void ofat_exfat_where_of(const struct ofat_exfat_file *dir, ofat_u32 pos, struct ofat_exfat_where *);
void ofat_exfat_where_dir(const struct ofat_exfat_where *, struct ofat_exfat_file *dir);
/* Writes len bytes at off, from valid on with zeros before off; the file
   grows.  *done may be less than len: OFAT_EXFAT_EFULL. */
int ofat_exfat_write(struct ofat_exfat *, struct ofat_exfat_file *, const struct ofat_exfat_where *,
                   ofat_u32 off, const void OFAT_FAR *buf, ofat_u16 len, ofat_u16 *done);
/* The size: shorter frees clusters, longer allocates them (read as zeros). */
int ofat_exfat_resize(struct ofat_exfat *, struct ofat_exfat_file *, const struct ofat_exfat_where *, ofat_u32 size);
/* A new empty file, or with OFAT_EXFAT_DIR a directory with one cluster;
   *pos its entry set. */
int ofat_exfat_create(struct ofat_exfat *, struct ofat_exfat_file *dir, struct ofat_exfat_where *dw,
                    const ofat_u16 *name, unsigned len, ofat_u16 attr, ofat_u16 time, ofat_u16 date, ofat_u32 *pos);
/* Removes the entry set at pos and frees its clusters (a directory must
   be empty: ofat_exfat_empty). */
int ofat_exfat_remove(struct ofat_exfat *, struct ofat_exfat_file *dir, ofat_u32 pos);
/* 1: no entries in the directory, 0: some, or an error. */
int ofat_exfat_empty(struct ofat_exfat *, struct ofat_exfat_file *dir);
/* The entry set at pos gets a new name, in the directory to (may be dir). */
int ofat_exfat_rename(struct ofat_exfat *, struct ofat_exfat_file *dir, ofat_u32 pos,
                    struct ofat_exfat_file *to, struct ofat_exfat_where *tw,
                    const ofat_u16 *name, unsigned len, ofat_u32 *newpos);
/* Attributes (the directory bit is kept). */
int ofat_exfat_attr(struct ofat_exfat *, struct ofat_exfat_file *dir, ofat_u32 pos, ofat_u16 attr);
/* A time stamp: OFAT_EXFAT_CREATED, _MODIFIED or _ACCESSED; cs10 10 ms units. */
#define OFAT_EXFAT_CREATED  8
#define OFAT_EXFAT_MODIFIED 12
#define OFAT_EXFAT_ACCESSED 16
int ofat_exfat_time(struct ofat_exfat *, struct ofat_exfat_file *dir, ofat_u32 pos, unsigned which,
                  ofat_u16 time, ofat_u16 date, ofat_u8 cs10);
/* VolumeDirty cleared (unless it was set before the mount), PercentInUse. */
int ofat_exfat_clean(struct ofat_exfat *);

/* 8.3 names: fcb is the 11-character FCB form ("LETTER~1TXT").  Returns 1
   when the long name had to be changed (it then needs a "~n"). */
int ofat_exfat_basis(const struct ofat_exfat_cp *, const ofat_u16 *name, unsigned len, char fcb[11]);
int ofat_exfat_short(struct ofat_exfat *, struct ofat_exfat_file *dir, const struct ofat_exfat_ent *, char fcb[11]);
int ofat_exfat_lookup(struct ofat_exfat *, struct ofat_exfat_file *dir, const char fcb[11], struct ofat_exfat_ent *);
#endif
