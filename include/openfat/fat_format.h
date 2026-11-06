/* Pure FAT layout/metadata helpers. All sizes use 512-byte sectors; hidden
 * is the physical partition offset recorded in the BPB. Layout returns 0/-1;
 * use outputs only after success. Metadata generators require a successful
 * layout and a writable 512-byte output buffer. They perform no I/O or DOS
 * capability checks. The caller owns partition bounds and FAT32 qualification.
 */

#ifndef OFAT_FAT_FORMAT_H
#define OFAT_FAT_FORMAT_H
#include "openfat/types.h"
/* Host-side calculated layout, not an on-disk BPB. sectors is volume length;
 * hidden is its physical base; clusters counts data clusters (not the two FAT
 * reserved entries). fat_sectors is per copy, root_sectors is the fixed FAT12/16
 * root area, and FAT32 instead allocates cluster 2 to its root directory.
 */
struct ofat_fat_layout {
    ofat_u32 sectors, hidden, clusters, fat_sectors;
    ofat_u16 root_entries, root_sectors, heads, spt, reserved;
    ofat_u8 bits, spc, media;
};
/* Calculate FAT12/16 geometry automatically (up to 4194303 sectors, 2 GiB);
 * layout32 explicitly requests FAT32, from 65525 clusters up to 2^32-1
 * sectors (2 TiB), with the cluster sizes of Microsoft's FAT specification
 * (fatgen103) as the smallest. Failure may leave partial fields, so do not
 * call generators until a layout function returned zero.
 */
int ofat_fat_layout(struct ofat_fat_layout *, ofat_u32 sectors, ofat_u32 hidden);
int ofat_fat_layout32(struct ofat_fat_layout *, ofat_u32 sectors, ofat_u32 hidden);
/* Generate one 512-byte boot sector. Serial is filesystem metadata, not a
 * hardware ID. Boot code reports a non-system disk; this does not install DOS.
 */
void ofat_fat_boot(const struct ofat_fat_layout *, ofat_u32 serial, unsigned char *);
/* Generate the first FAT sector including reserved entries and FAT32 root
 * allocation. The caller zeros remaining FAT sectors and writes both copies.
 */
void ofat_fat_first(const struct ofat_fat_layout *, unsigned char *);
/* Generate FAT32 free-space hints for a newly initialized layout, before
 * accounting for any bad clusters. The formatter adjusts hints after scanning.
 */
void ofat_fat_fsinfo(const struct ofat_fat_layout *, unsigned char *);
/* Return a byte offset from FAT start. Only pass bits=12/16/32 and a
 * validated cluster number; this arithmetic helper does not check either.
 */
ofat_u32 ofat_fat_entry_offset(unsigned bits,ofat_u32 cluster);
/* entry holds 2 bytes for FAT12/16 or 4 for FAT32, assembled across sectors
 * when needed. Return 1 changed, 0 already marked, -1 unsupported width. The
 * caller validates the cluster and writes all affected FAT copies.
 */
int ofat_fat_mark_bad(unsigned bits,ofat_u32 cluster,unsigned char *entry);
#endif
