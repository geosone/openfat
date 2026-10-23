/* FAT12/16 quick-format layout and metadata generation, independent of I/O. */
#include <string.h>
#include "openaspi/fat_format.h"
static void le16(unsigned char *p, oa_u16 v) { p[0] = (oa_u8)v; p[1] = (oa_u8)(v >> 8); }
static void le32(unsigned char *p, oa_u32 v) { le16(p, (oa_u16)v); le16(p+2, (oa_u16)(v >> 16)); }
static int layout(struct oa_fat_layout *f, oa_u32 total, oa_u32 hidden,unsigned bits)
{
    oa_u32 data, clusters, needed, fat;
    unsigned spc, pass;
    if (total < 128 || total > 4194303UL || hidden > 0xFFFFFFFFUL-total) return -1;
    memset(f, 0, sizeof(*f)); f->sectors = total; f->hidden = hidden;
    f->bits = (oa_u8)(bits ? bits : total < 8400UL ? 12 : 16);
    f->reserved = f->bits==32 ? 32 : 1;
    f->root_entries = total <= 5760UL ? (total <= 1440UL ? 112 : 224) : 512;
    f->root_sectors = f->root_entries / 16;
    if(f->bits==32) f->root_entries=f->root_sectors=0;
    f->spt = total == 1440UL ? 9 : total == 2880UL ? 18 : total == 5760UL ? 36 : 32;
    f->heads = total <= 5760UL ? 2 : 8;
    f->media = total == 1440UL ? 0xF9 : total <= 5760UL ? 0xF0 : 0xF8;
    for (spc = 1; spc <= 64; spc *= 2) {
        fat = 1;
        for (pass = 0; pass < 32; ++pass) {
            if (f->reserved + 2*fat + f->root_sectors >= total) break;
            data = total - f->reserved - 2*fat - f->root_sectors;
            clusters = data / spc;
            needed = ((clusters+2)*f->bits + 4095UL) / 4096UL;
            /* Conservatively retain the larger FAT if the iteration toggles. */
            if (needed <= fat) {
                if ((f->bits == 12 && clusters >= 1 && clusters < 4085) ||
                    (f->bits == 16 && clusters >= 4085 && clusters < 65525) ||
                    (f->bits == 32 && clusters >= 65525 && clusters < 0x0FFFFFF5UL)) {
                    f->spc = (oa_u8)spc; f->fat_sectors = (oa_u16)fat;
                    f->clusters = clusters; return 0;
                }
                break;
            }
            fat = needed;
        }
    }
    return -1;
}
int oa_fat_layout(struct oa_fat_layout *f,oa_u32 total,oa_u32 hidden) { return layout(f,total,hidden,0); }
int oa_fat_layout32(struct oa_fat_layout *f,oa_u32 total,oa_u32 hidden) { return layout(f,total,hidden,32); }
void oa_fat_boot(const struct oa_fat_layout *f, oa_u32 serial, unsigned char *b)
{
    /* BIOS entry prints a non-system-disk message and invokes INT 18h. */
    static const unsigned char code[] = {
        0xFA,0x31,0xC0,0x8E,0xD8,0xFB,0xBE,0x57,0x7C,
        0xAC,0x08,0xC0,0x74,0x06,0xB4,0x0E,0xCD,0x10,0xEB,0xF5,
        0xCD,0x18,0xEB,0xFE,0x90
    };
    memset(b, 0, 512); b[0]=0xEB; b[1]=0x3C; b[2]=0x90;
    memcpy(b+3, "OPENASPI", 8); le16(b+11, 512); b[13]=f->spc;
    le16(b+14, f->reserved); b[16]=2; le16(b+17, f->root_entries);
    if (f->bits!=32 && f->sectors <= 65535UL) le16(b+19, (oa_u16)f->sectors);
    else le32(b+32, f->sectors);
    b[21]=f->media; le16(b+22, f->fat_sectors);
    le16(b+24, f->spt); le16(b+26, f->heads); le32(b+28, f->hidden);
    b[36]=0x80; b[38]=0x29; le32(b+39, serial);
    memcpy(b+43, "NO NAME    ", 11); memcpy(b+54, f->bits==12 ? "FAT12   " : "FAT16   ", 8);
    memcpy(b+62, code, sizeof(code)); memcpy(b+87, "Non-system disk\r\n", 18);
    if(f->bits==32) {
        memset(b+36,0,474);
        b[1]=0x58; le16(b+22,0); le32(b+36,f->fat_sectors);
        le32(b+44,2); le16(b+48,1); le16(b+50,6);
        b[64]=0x80; b[66]=0x29; le32(b+67,serial);
        memcpy(b+71,"NO NAME    ",11); memcpy(b+82,"FAT32   ",8);
        memcpy(b+90,code,sizeof(code)); b[97]=0x73;
        memcpy(b+115,"Non-system disk\r\n",18);
    }
    b[510]=0x55; b[511]=0xAA;
}
void oa_fat_first(const struct oa_fat_layout *f, unsigned char *b)
{
    memset(b, 0, 512); b[0]=f->media; b[1]=b[2]=0xFF;
    if (f->bits==16) b[3]=0xFF;
    if (f->bits==32) { le32(b,0x0FFFFFF8UL); le32(b+4,0xFFFFFFFFUL); le32(b+8,0x0FFFFFFFUL); }
}
void oa_fat_fsinfo(const struct oa_fat_layout *f,unsigned char *b)
{
    memset(b,0,512); le32(b,0x41615252UL); le32(b+484,0x61417272UL);
    le32(b+488,f->clusters-1); le32(b+492,3); le32(b+508,0xAA550000UL);
}
oa_u32 oa_fat_entry_offset(unsigned bits,oa_u32 cluster)
{
    return bits==12 ? cluster+cluster/2 : cluster*(bits/8);
}
int oa_fat_mark_bad(unsigned bits,oa_u32 cluster,unsigned char *p)
{
    oa_u32 old,value;
    old=(oa_u32)p[0]|(oa_u32)p[1]<<8;
    if(bits==12) {
        if((cluster&1 ? old>>4 : old&0xFFF)==0xFF7) return 0;
        value=cluster&1 ? (old&15)|0xFF70 : (old&0xF000)|0xFF7;
        le16(p,(oa_u16)value);
    } else if(bits==16) {
        if(old==0xFFF7) return 0;
        le16(p,0xFFF7);
    } else if(bits==32) {
        old|=(oa_u32)p[2]<<16|(oa_u32)p[3]<<24;
        if((old&0x0FFFFFFFUL)==0x0FFFFFF7UL) return 0;
        le32(p,(old&0xF0000000UL)|0x0FFFFFF7UL);
    } else return -1;
    return 1;
}
