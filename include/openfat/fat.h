#ifndef OFAT_FAT_H
#define OFAT_FAT_H
/* FAT12, FAT16 and FAT32 reading and writing with VFAT long names,
   independent of I/O and of DOS, after Microsoft's FAT specification
   (fatgen103).  Sectors come from callbacks.  Writing keeps an order that
   leaves at worst lost clusters behind: the FAT first, then the data, the
   directory entry last.  All FAT copies are kept equal (on FAT32 only the
   active one when mirroring is off).  FAT16 and FAT32 carry the "volume
   dirty" bit in FAT entry 1 while the volume is being changed. */
#include "openfat/codepage.h"

#ifndef OFAT_FAT_MAXSECT
#define OFAT_FAT_MAXSECT 512
#endif

#define OFAT_FAT_OK      0
#define OFAT_FAT_END     1      /* end of directory or of a chain */
#define OFAT_FAT_EIO    -1      /* sector read failed */
#define OFAT_FAT_EBAD   -2      /* not FAT or damaged */
#define OFAT_FAT_ESIZE  -3      /* sector size larger than OFAT_FAT_MAXSECT */
#define OFAT_FAT_EFULL  -4      /* no free cluster, or the fixed root directory is full */
#define OFAT_FAT_EWRITE -5      /* sector write failed, or no write function */
#define OFAT_FAT_EEXIST -6      /* the name exists */
#define OFAT_FAT_ENAME  -7      /* not a valid long name */

#define OFAT_FAT_RDONLY 0x01
#define OFAT_FAT_HIDDEN 0x02
#define OFAT_FAT_SYSTEM 0x04
#define OFAT_FAT_VOLUME 0x08
#define OFAT_FAT_DIR    0x10
#define OFAT_FAT_ARCH   0x20

/* Reads or writes one sector (of 1 << sshift bytes) at lba of the device. */
typedef int (*ofat_fat_read_fn)(void *dev, ofat_lba lba, void OFAT_FAR *buf);
typedef int (*ofat_fat_write_fn)(void *dev, ofat_lba lba, const void OFAT_FAR *buf);

struct ofat_fat {
    ofat_fat_read_fn read;
    void *dev;
    ofat_lba start;             /* first sector of the volume on the device */
    ofat_u32 fat, fat_size;     /* first sector of FAT 0, sectors per FAT */
    ofat_u32 root;              /* FAT12/16: first sector of the root directory;
                                   FAT32: its first cluster */
    ofat_u32 heap;              /* first sector of cluster 2 */
    ofat_u32 clusters;          /* clusters 2 .. clusters + 1 */
    ofat_u32 fsinfo;            /* FAT32: sector of FSInfo, else 0 */
    ofat_u32 serial;
    ofat_u16 root_entries;      /* FAT12/16 */
    ofat_u8 bits;               /* 12, 16 or 32 */
    ofat_u8 sshift, cshift;     /* bytes per sector, sectors per cluster */
    ofat_u8 nfats, active;      /* copies; the one read */
    ofat_u8 mirror;             /* writes go to every copy */
    char label[11];
    const struct ofat_cp *cp;
    ofat_u32 buf_lba;           /* sector in buf, or 0xFFFFFFFF */
    ofat_u8 *buf;               /* OFAT_FAT_MAXSECT bytes; volumes may share
                                   one when they set buf_lba to 0xFFFFFFFF
                                   after another one used it */
    /* writing: set write after ofat_fat_mount; 0 keeps the volume read-only */
    ofat_fat_write_fn write;
    ofat_u32 hint;              /* the next free cluster is looked for here */
    ofat_u32 free;              /* free clusters, 0FFFFFFFFh: not counted */
    ofat_u8 dirty;              /* 1 set by us, 2 set at mount */
};

/* A file or directory: its cluster chain and the cluster last walked to. */
struct ofat_fat_file {
    ofat_u32 first, size;
    ofat_u32 idx, clu;          /* cluster index and number last walked to;
                                   clu 0: none */
    ofat_u8 dir;                /* a directory: its size is its chain */
    ofat_u8 root;               /* the fixed root directory of FAT12/16 */
};

/* Where the entry of a file is: the first cluster of its directory (0 for
   the root directory) and the offset of its short entry; none for the
   root directory itself. */
struct ofat_fat_where {
    ofat_u32 dir, pos;
    ofat_u8 none;
};

struct ofat_fat_ent {
    ofat_u32 pos;               /* byte offset of the short entry */
    ofat_u32 lfn;               /* offset of the first entry of the set */
    ofat_u8 attr, ntcase;       /* ntcase: 08h base, 10h extension lower case */
    ofat_u16 time, date;        /* last written */
    ofat_u16 ctime, cdate, adate;
    ofat_u8 ctime_cs;           /* 10 ms units of ctime */
    char fcb[11];               /* the short name */
    struct ofat_fat_file file;
    ofat_u8 name_len;
    ofat_u16 name[255];         /* the long name, else the short one */
};

/* 12, 16 or 32 when the boot sector is one of a FAT volume, else 0. */
int ofat_fat_probe(const ofat_u8 OFAT_FAR *boot);
int ofat_fat_mount(struct ofat_fat *, ofat_fat_read_fn, void *dev, ofat_lba start,
                   const struct ofat_cp *, ofat_u8 *buf);
void ofat_fat_rootdir(const struct ofat_fat *, struct ofat_fat_file *);
/* The directory whose first cluster is first (0: the root directory). */
void ofat_fat_dirfile(const struct ofat_fat *, ofat_u32 first, struct ofat_fat_file *);
/* The next entry from *pos on ("." and "..", volume labels and deleted
   entries are skipped); OFAT_FAT_END at the end. */
int ofat_fat_dir_next(struct ofat_fat *, struct ofat_fat_file *dir, ofat_u32 *pos, struct ofat_fat_ent *);
/* The entry of a long or short name, case-insensitive. */
int ofat_fat_find(struct ofat_fat *, struct ofat_fat_file *dir, const ofat_u16 *name, unsigned len,
                  struct ofat_fat_ent *);
/* The entry with the short name fcb (11 characters, FCB form). */
int ofat_fat_lookup(struct ofat_fat *, struct ofat_fat_file *dir, const char fcb[11], struct ofat_fat_ent *);
int ofat_fat_read(struct ofat_fat *, struct ofat_fat_file *, ofat_u32 off, void OFAT_FAR *buf,
                  ofat_u16 len, ofat_u16 *done);
int ofat_fat_free(struct ofat_fat *, ofat_u32 *free_clusters);

/* Writing.  Each call leaves the volume consistent; the dirty bit stays set
   until ofat_fat_clean. */
void ofat_fat_where_of(const struct ofat_fat_file *dir, ofat_u32 pos, struct ofat_fat_where *);
/* Writes len bytes at off; the file grows, with zeros between its end and
   off.  *done may be less than len: OFAT_FAT_EFULL. */
int ofat_fat_write(struct ofat_fat *, struct ofat_fat_file *, const struct ofat_fat_where *,
                   ofat_u32 off, const void OFAT_FAR *buf, ofat_u16 len, ofat_u16 *done);
/* The size: shorter frees clusters, longer adds zeros. */
int ofat_fat_resize(struct ofat_fat *, struct ofat_fat_file *, const struct ofat_fat_where *, ofat_u32 size);
/* A new empty file, or with OFAT_FAT_DIR a directory with "." and "..";
   names are UTF-16, 1-255 characters.  The short name is the name itself
   when it is a valid 8.3 name (lower case through the NT case bits), else
   a basis name with "~n" and the long name in VFAT entries.  *pos: the
   short entry. */
int ofat_fat_create(struct ofat_fat *, struct ofat_fat_file *dir, const ofat_u16 *name, unsigned len,
                    ofat_u8 attr, ofat_u16 time, ofat_u16 date, ofat_u32 *pos);
/* Removes the entry at pos and frees its clusters (a directory must be
   empty: ofat_fat_empty). */
int ofat_fat_remove(struct ofat_fat *, struct ofat_fat_file *dir, ofat_u32 pos);
/* 1: no entries in the directory, 0: some, or an error. */
int ofat_fat_empty(struct ofat_fat *, struct ofat_fat_file *dir);
/* The entry at pos gets a new name, in the directory to (may be dir); a
   moved directory's ".." follows. */
int ofat_fat_rename(struct ofat_fat *, struct ofat_fat_file *dir, ofat_u32 pos,
                    struct ofat_fat_file *to, const ofat_u16 *name, unsigned len, ofat_u32 *newpos);
/* Attributes (the directory and volume bits are kept). */
int ofat_fat_attr(struct ofat_fat *, struct ofat_fat_file *dir, ofat_u32 pos, ofat_u8 attr);
/* A time stamp: OFAT_FAT_CREATED, _MODIFIED or _ACCESSED (date only). */
#define OFAT_FAT_CREATED  14
#define OFAT_FAT_MODIFIED 22
#define OFAT_FAT_ACCESSED 18
int ofat_fat_time(struct ofat_fat *, struct ofat_fat_file *dir, ofat_u32 pos, unsigned which,
                  ofat_u16 time, ofat_u16 date, ofat_u8 cs10);
/* The dirty bit cleared (unless it was set before the mount), FSInfo. */
int ofat_fat_clean(struct ofat_fat *);
#endif
