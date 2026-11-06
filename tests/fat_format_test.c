#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openfat/fat_format.h"
static unsigned short le16(const unsigned char *p) { return (unsigned short)(p[0]|p[1]<<8); }
int main(int argc,char **argv)
{
    static const ofat_u32 sizes[]={1440,2880,5760,8192,8400,196608,246528,489532,4190208};
    struct ofat_fat_layout f;
    unsigned char b[514];
    unsigned i;
    ofat_u32 data,needed,n;
    FILE *file;
    for(i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        assert(!ofat_fat_layout(&f,sizes[i],63));
        data=f.sectors-1-2UL*f.fat_sectors-f.root_sectors;
        assert(f.clusters==data/f.spc);
        assert(f.bits==12 ? f.clusters<4085 : f.clusters>=4085 && f.clusters<65525);
        needed=((f.clusters+2)*f.bits+4095)/4096;
        assert(needed<=f.fat_sectors);
        memset(b,0xA5,sizeof(b)); ofat_fat_boot(&f,0x12345678,b+1);
        assert(b[0]==0xA5 && b[513]==0xA5);
        assert(le16(b+12)==512 && le16(b+511)==0xAA55);
        assert(le16(b+23)==f.fat_sectors);
    }
    assert(ofat_fat_layout(&f,0,0));
    assert(ofat_fat_layout(&f,4194304,0));
    assert(ofat_fat_layout(&f,2880,0xFFFFFFFFUL));
    assert(!ofat_fat_layout32(&f,196608,0));
    assert(f.bits==32 && f.clusters>=65525 && f.reserved==32 && f.root_entries==0);
    assert(ofat_fat_layout32(&f,2880,0));
    /* FAT32 past 2 GiB up to 2^32-1 sectors, cluster sizes by fatgen103 */
    {
        static const ofat_u32 big[]={4194304UL,16777216UL,33554432UL,67108864UL,268435456UL,0xFFFFFFFFUL};
        static const unsigned spc[]={8,8,16,32,64,64};
        for(i=0;i<sizeof(big)/sizeof(big[0]);++i) {
            assert(!ofat_fat_layout32(&f,big[i],0));
            assert(f.bits==32 && f.spc==spc[i]);
            assert(f.clusters>=65525 && f.clusters<0x0FFFFFF5UL);
            data=f.sectors-f.reserved-2UL*f.fat_sectors;
            assert(f.clusters==data/f.spc);
            assert((f.clusters+2)/128+((f.clusters+2)%128!=0)<=f.fat_sectors);
            ofat_fat_boot(&f,1,b);
            assert(b[13]==f.spc && (b[36]|b[37]<<8|(ofat_u32)b[38]<<16|(ofat_u32)b[39]<<24)==f.fat_sectors);
            assert((b[32]|b[33]<<8|(ofat_u32)b[34]<<16|(ofat_u32)b[35]<<24)==big[i]);
        }
        assert(ofat_fat_layout(&f,4194304UL,0));     /* FAT16 still ends there */
        assert(ofat_fat_layout32(&f,0xFFFFFFFFUL,1)); /* hidden + total overflow */
    }
    memset(b,0,sizeof(b)); b[1]=0xA0;
    assert(ofat_fat_mark_bad(12,2,b)==1 && b[0]==0xF7 && b[1]==0xAF);
    assert(ofat_fat_mark_bad(12,2,b)==0);
    b[0]=5; b[1]=0;
    assert(ofat_fat_mark_bad(12,341,b)==1 && b[0]==0x75 && b[1]==0xFF);
    assert(ofat_fat_entry_offset(12,341)==511);
    assert(ofat_fat_mark_bad(16,2,b)==1 && le16(b)==0xFFF7);
    memset(b,0,sizeof(b)); b[3]=0xA0;
    assert(ofat_fat_mark_bad(32,3,b)==1 && b[0]==0xF7 && b[1]==0xFF && b[2]==0xFF && b[3]==0xAF);
    assert(ofat_fat_mark_bad(32,3,b)==0);
    if(argc>=3) {
        n=(ofat_u32)strtoul(argv[1],NULL,10);
        assert(!(argc==4 ? ofat_fat_layout32(&f,n,0) : ofat_fat_layout(&f,n,0)));
        file=fopen(argv[2],"wb"); assert(file);
        assert(!fseek(file,(long)n*512-1,SEEK_SET)); assert(fputc(0,file)!=EOF);
        rewind(file); ofat_fat_boot(&f,0x12345678,b); assert(fwrite(b,512,1,file)==1);
        ofat_fat_first(&f,b);
        assert(!fseek(file,(long)f.reserved*512,SEEK_SET)); assert(fwrite(b,512,1,file)==1);
        assert(!fseek(file,(long)(f.reserved+f.fat_sectors)*512,SEEK_SET)); assert(fwrite(b,512,1,file)==1);
        if(f.bits==32) {
            ofat_fat_boot(&f,0x12345678,b); assert(!fseek(file,6*512,SEEK_SET)); assert(fwrite(b,512,1,file)==1);
            ofat_fat_fsinfo(&f,b); assert(!fseek(file,512,SEEK_SET)); assert(fwrite(b,512,1,file)==1);
            assert(!fseek(file,7*512,SEEK_SET)); assert(fwrite(b,512,1,file)==1);
        }
        assert(!fclose(file));
    }
    puts("FAT layout, cluster limits and guarded metadata tests passed"); return 0;
}
