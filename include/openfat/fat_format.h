/* Pure FAT layout/metadata helpers. All sizes use 512-byte sectors; hidden
 * is the physical partition offset recorded in the BPB. Layout returns 0/-1;
 * use outputs only after success. Metadata generators require a successful
 * layout and a writable 512-byte output buffer. They perform no I/O or DOS
 * capability checks. The caller owns partition bounds and FAT32 qualification.
 */

#ifndef OA_FAT_FORMAT_H
#define OA_FAT_FORMAT_H
#include "openaspi/types.h"
/* Host-side calculated layout, not an on-disk BPB. sectors is volume length;
 * hidden is its physical base; clusters counts data clusters (not the two FAT
 * reserved entries). fat_sectors is per copy, root_sectors is the fixed FAT12/16
 * root area, and FAT32 instead allocates cluster 2 to its root directory.
 */
struct oa_fat_layout {
    oa_u32 sectors, hidden, clusters;
    oa_u16 fat_sectors, root_entries, root_sectors, heads, spt, reserved;
    oa_u8 bits, spc, media;
};
/* Calculate FAT12/16 geometry automatically for the supported bounded size;
 * layout32 explicitly requests FAT32. Failure may leave partial fields, so do
 * not call generators until a layout function returned zero.
 */
int oa_fat_layout(struct oa_fat_layout *, oa_u32 sectors, oa_u32 hidden);
int oa_fat_layout32(struct oa_fat_layout *, oa_u32 sectors, oa_u32 hidden);
/* Generate one 512-byte boot sector. Serial is filesystem metadata, not a
 * hardware ID. Boot code reports a non-system disk; this does not install DOS.
 */
void oa_fat_boot(const struct oa_fat_layout *, oa_u32 serial, unsigned char *);
/* Generate the first FAT sector including reserved entries and FAT32 root
 * allocation. The caller zeros remaining FAT sectors and writes both copies.
 */
void oa_fat_first(const struct oa_fat_layout *, unsigned char *);
/* Generate FAT32 free-space hints for a newly initialized layout, before
 * accounting for any bad clusters. The formatter adjusts hints after scanning.
 */
void oa_fat_fsinfo(const struct oa_fat_layout *, unsigned char *);
/* Return a byte offset from FAT start. Only pass bits=12/16/32 and a
 * validated cluster number; this arithmetic helper does not check either.
 */
oa_u32 oa_fat_entry_offset(unsigned bits,oa_u32 cluster);
/* entry holds 2 bytes for FAT12/16 or 4 for FAT32, assembled across sectors
 * when needed. Return 1 changed, 0 already marked, -1 unsupported width. The
 * caller validates the cluster and writes all affected FAT copies.
 */
int oa_fat_mark_bad(unsigned bits,oa_u32 cluster,unsigned char *entry);
#endif
