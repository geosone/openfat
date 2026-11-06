# openfat

Portable FAT12, FAT16, FAT32 and exFAT code in C90. It builds with OpenWatcom
for 16-bit DOS and with Clang or GCC for 32/64-bit hosts and kernels. The
library does no I/O of its own: sectors come and go through callbacks of the
caller. That way one code base serves the DOS drivers and tools of
[OpenASPI](https://github.com/geosone/OpenASPI) and the GDOS kernel of
GonWare.

| Part | Files | Content |
| --- | --- | --- |
| exFAT | `src/exfat/exfat.c`, `codepage.c` | mounting, directories, reading, writing, creating, removing, renaming; TexFAT volumes (two FATs and bitmaps) are read through the active copy and both copies are changed alike |
| exFAT format | `src/exfat/exfat_format.c`, `upcase.c` | layout, boot region, bitmap, up-case table, root directory; TexFAT, bad clusters |
| exFAT check | `src/exfat/exfat_check.c` | checking and repairing |
| FAT format | `src/fat/fat_format.c` | FAT12/16/32 layout, boot sector, FSInfo |
| FAT check | `src/fat/fat_check.c` | checking and repairing FAT12/16/32 after the rules of fsck.fat |
| FAT defrag | `src/fat/fat_defrag.c` | defragmenting FAT12/16/32, in a write order that survives power loss |

All names start with `ofat_` (`OFAT_` for constants); `include/openfat/types.h`
gives the fixed-width types.

Planned: partition tables (GPT, MBR with extended partitions), file access to
FAT12/16/32 with long names, TexFAT transactions, and the tools OFORMAT, OFSCK
and ODEFRAG for every FAT variant.

## Building and testing

```sh
make            # host test programs in build/host
make test       # quick tests
make dos-check  # every source as a 16-bit DOS object (OpenWatcom, WATCOM=/opt/watcom)
make check      # everything, including the image tests
```

The image tests need mkfs.fat, fsck.fat, mkfs.exfat, fsck.exfat, mtools,
Python 3 and udisksctl (they fill images through the Linux drivers without
root).

CMake (as a subproject) gives `OpenFAT::fat` and `OpenFAT::check`.

## History

The code started in OpenASPI (`common/exfat`, `common/fat`) and moved here
with its history.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).
