#ifndef OFAT_CODEPAGE_H
#define OFAT_CODEPAGE_H
/* OEM code pages for 8.3 names: the Unicode of the characters 80h-FFh and
   their DOS upper case, filled from a UNI_<cp>.TAB file of
   mars-unicode-tables (src/codepage.c), and the 8.3 basis of a long name
   (src/name83.c). */
#include "openfat/types.h"

struct ofat_cp {
    ofat_u16 cp;
    ofat_u16 uni[128];
    ofat_u8 upper[128];
};

/* Fills cp from a single-byte UNI_<number>.TAB (the NSS table layout: a
   256-byte header, table type 1, the 256 Unicode values of the bytes).
   The DOS upper case of a character is the code page's character for its
   Unicode capital, else the base letter of an accented Latin capital
   ("a" with grave -> "A" in code page 437), else the character itself.
   0, or -1 when tab is no single-byte table. */
int ofat_cp_load(struct ofat_cp *cp, ofat_u16 number, const ofat_u8 *tab, ofat_u32 size);
/* The simple Unicode capital of u (exFAT's recommended up-case table). */
ofat_u16 ofat_unicode_upper(ofat_u16 u);
/* "A"-"Z" for an accented Latin letter (U+00C0-U+024F), else 0 (src/latin.c,
   made by tools/mklatin.py from UnicodeData.txt). */
ofat_u8 ofat_latin_base(ofat_u16 u);

/* A Unicode character in the code page, upper case; 0 when it has none. */
ofat_u8 ofat_cp_upper(const struct ofat_cp *, ofat_u16 u);
/* The 8.3 basis of a UTF-16 name in the 11-character FCB form
   ("LETTER  TXT").  Returns 1 when the name had to be changed (it then
   needs a long name and a "~n" tail). */
int ofat_cp_basis(const struct ofat_cp *, const ofat_u16 *name, unsigned len, char fcb[11]);
#endif
