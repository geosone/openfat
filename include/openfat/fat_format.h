#ifndef OA_FAT_FORMAT_H
#define OA_FAT_FORMAT_H
#include "openaspi/types.h"
struct oa_fat_layout {
    oa_u32 sectors, hidden, clusters;
    oa_u16 fat_sectors, root_entries, root_sectors, heads, spt, reserved;
    oa_u8 bits, spc, media;
};
int oa_fat_layout(struct oa_fat_layout *, oa_u32 sectors, oa_u32 hidden);
int oa_fat_layout32(struct oa_fat_layout *, oa_u32 sectors, oa_u32 hidden);
void oa_fat_boot(const struct oa_fat_layout *, oa_u32 serial, unsigned char *);
void oa_fat_first(const struct oa_fat_layout *, unsigned char *);
void oa_fat_fsinfo(const struct oa_fat_layout *, unsigned char *);
oa_u32 oa_fat_entry_offset(unsigned bits,oa_u32 cluster);
int oa_fat_mark_bad(unsigned bits,oa_u32 cluster,unsigned char *entry);
#endif
