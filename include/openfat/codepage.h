#ifndef OFAT_CODEPAGE_H
#define OFAT_CODEPAGE_H
/* OEM code pages for 8.3 names: the Unicode of the characters 80h-FFh and
   their DOS upper case (src/codepage.c, made by tools/mkcptab.py), and the
   8.3 basis of a long name (src/name83.c). */
#include "openfat/types.h"

struct ofat_cp {
    ofat_u16 cp;
    ofat_u16 uni[128];
    ofat_u8 upper[128];
};
extern const struct ofat_cp ofat_cp437, ofat_cp850;

/* A Unicode character in the code page, upper case; 0 when it has none. */
ofat_u8 ofat_cp_upper(const struct ofat_cp *, ofat_u16 u);
/* The 8.3 basis of a UTF-16 name in the 11-character FCB form
   ("LETTER  TXT").  Returns 1 when the name had to be changed (it then
   needs a long name and a "~n" tail). */
int ofat_cp_basis(const struct ofat_cp *, const ofat_u16 *name, unsigned len, char fcb[11]);
#endif
