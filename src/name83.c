/* 8.3 names from long names, shared by FAT and exFAT: the FCB form of
   the basis name after the rules of the FAT specification (Microsoft's
   "basis-name generation"), in the OEM code page. */
#include <string.h>
#include "openfat/codepage.h"

/* A Unicode character in the code page, upper case; 0 when it has none. */
ofat_u8 ofat_cp_upper(const struct ofat_cp *cp, ofat_u16 u)
{
    unsigned i;
    if (u < 0x80) return (ofat_u8)(u >= 'a' && u <= 'z' ? u - 32 : u);
    for (i = 0; i < 128; ++i) if (cp->uni[i] == u) return cp->upper[i];
    return 0;
}

static int part83(const struct ofat_cp *cp, const ofat_u16 *name, unsigned len, char *out, unsigned max)
{
    unsigned i, n = 0;
    int lossy = 0;
    ofat_u8 c;
    for (i = 0; i < len; ++i) {
        c = ofat_cp_upper(cp, name[i]);
        /* an accented Latin letter the code page lacks: its base letter */
        if (!c && (c = ofat_latin_base(ofat_unicode_upper(name[i]))) != 0) lossy = 1;
        if (c == ' ' || c == '.') { lossy = 1; continue; }
        if (!c || c < 0x20 || strchr("\"*+,/:;<=>?[\\]|", c)) { c = '_'; lossy = 1; }
        if (n == max) { lossy = 1; break; }
        out[n++] = (char)c;
    }
    return lossy;
}

int ofat_cp_basis(const struct ofat_cp *cp, const ofat_u16 *name, unsigned len, char fcb[11])
{
    unsigned dot = len, lead = 0, i;
    int lossy;
    memset(fcb, ' ', 11);
    while (lead < len && name[lead] == '.') ++lead;
    for (i = len; i > lead; --i) if (name[i-1] == '.') { dot = i - 1; break; }
    lossy = lead != 0;
    lossy |= part83(cp, name + lead, dot - lead, fcb, 8);
    if (dot < len) lossy |= part83(cp, name + dot + 1, len - dot - 1, fcb + 8, 3);
    if (fcb[0] == ' ') { fcb[0] = '_'; lossy = 1; }
    return lossy;
}
