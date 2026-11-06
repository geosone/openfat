# openfat

Portable FAT12, FAT16, FAT32 and exFAT code in C90. It builds with OpenWatcom
for 16-bit DOS and with Clang or GCC for 32/64-bit hosts and kernels. The
library does no I/O of its own: sectors come and go through callbacks of the
caller. That way one code base serves the DOS drivers and tools of
[OpenASPI](https://github.com/geosone/OpenASPI) and the GDOS kernel of
GonWare.

| Part | Files | Content |
| --- | --- | --- |
| FAT | `src/fat/fat.c` | FAT12/16/32 with VFAT long names: mounting, directories, reading, writing, creating (8.3 names with NT case bits or `~n` and a long name), removing, renaming and moving, attributes, times; dirty bit, FSInfo, every FAT copy |
| Partitions | `src/part/part.c` | MBR with the EBR chain of logical partitions, GPT with CRC checks and the backup header, superfloppy detection |
| Code pages | `src/codepage.c`, `src/latin.c`, `src/name83.c` | OEM code pages loaded from the `UNI_<cp>.TAB` files of [mars-unicode-tables](https://gitea.disconnected-by-peer.at/mars_nwe/mars-unicode-tables) (`ofat_cp_load`), DOS upper case, 8.3 basis names (shared by FAT and exFAT) |
| exFAT | `src/exfat/exfat.c` | mounting, directories, reading, writing, creating, removing, renaming; TexFAT volumes (two FATs and bitmaps) are read through the active copy and both copies are changed alike |
| exFAT format | `src/exfat/exfat_format.c`, `upcase.c` | layout, boot region, bitmap, up-case table, root directory; TexFAT, bad clusters |
| exFAT check | `src/exfat/exfat_check.c` | checking and repairing |
| FAT format | `src/fat/fat_format.c` | FAT12/16/32 layout, boot sector, FSInfo |
| FAT check | `src/fat/fat_check.c` | checking and repairing FAT12/16/32 after the rules of fsck.fat |
| FAT defrag | `src/fat/fat_defrag.c` | defragmenting FAT12/16/32, in a write order that survives power loss |

Sector numbers (`ofat_lba`) and file sizes (`ofat_fsize`) have 64 bits in
32/64-bit builds: exFAT volumes past 2 TiB, files of 4 GiB and more, FAT and
exFAT volumes anywhere on large disks. Under DOS (OpenWatcom, or
`OFAT_NARROW`) they have 32 bits, which keeps the resident drivers small;
exFAT files of 4 GiB and more are then read-only. The check, format and
defragmentation code still reaches 2^32 sectors of a volume.

All names start with `ofat_` (`OFAT_` for constants); `include/openfat/types.h`
gives the fixed-width types.

Planned: TexFAT transactions, and the tools OFORMAT, OFSCK and ODEFRAG for
every FAT variant.

## Building and testing

```sh
make            # host test programs in build/host
make test       # quick tests
make dos-check  # every source as a 16-bit DOS object (OpenWatcom, WATCOM=/opt/watcom)
make check      # everything, including the image tests
```

The image tests compare with what the Linux drivers read and write, and
with fsck.fat, fsck.exfat and sfdisk. They need mkfs.fat, fsck.fat, mkfs.exfat, fsck.exfat, mtools,
Python 3, sfdisk and udisksctl (they fill images through the Linux drivers without
root). The tests load the code pages from `UNITAB` (default
`../mars-unicode-tables/TAB/unitables`, a checkout next to this one).
`src/latin.c` is made by `tools/mklatin.py` from `UCD/UnicodeData.txt` of
the same repository.

CMake (as a subproject) gives `OpenFAT::fat` and `OpenFAT::check`.

## History

The code started in OpenASPI (`common/exfat`, `common/fat`) and moved here
with its history.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).
