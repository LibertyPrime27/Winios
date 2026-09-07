/* A stroke font, because there is no font file to load.
 *
 * A dialog is mostly text, so nothing renders without one, and the two easy
 * answers are both wrong here: shipping a bitmap font locks the text to one
 * size, and a dialog asks for whatever size its template says; shipping
 * somebody's typeface means shipping somebody's typeface.
 *
 * So the glyphs are strokes -- polylines on a design grid, drawn with lines
 * at whatever scale the caller asked for. That is the same thing a pen
 * plotter did, and it has the properties this needs: any size, any weight
 * (draw it twice, offset by one, for bold), a few kilobytes, and ours.
 *
 * It is *proportional*. That is not a refinement -- it is the difference
 * between a dialog that reads and one that does not. Every Windows dialog
 * positions its controls in units derived from the average character width
 * of a proportional font and sizes each control for the text it holds, so
 * drawing that text monospaced overruns every label and button on the page.
 * An `i` is three units wide and a `W` is eleven, and the narrow and wide
 * letters are drawn inside their own advance rather than squeezed into a
 * common box.
 *
 * The grid: y runs 0..13 downward, with the cap line at y=0, the x-height
 * line at y=4, the baseline at y=10 and the descender at y=13. x runs 0..11,
 * and how far it runs is per glyph.
 *
 * Encoding: two characters per point -- x is '0'..'9' then 'a'..'c' for
 * 10..12, y is '0'..'9' then 'A'..'D' for 10..13 -- and '|' lifts the pen.
 * It reads badly and it packs 95 glyphs into a screenful, which is the trade
 * a table like this should make.
 */
#include "gdifont.h"

#include <string.h>

/* ASCII 32..126. Empty means "draw nothing", which is correct for a space. */
static const char *const GLYPH[95] = {
/* sp */ "",
/* !  */ "1017|191A",
/* "  */ "1012|3032",
/* #  */ "101A|505A|0464|0868",
/* $  */ "303A|5140200205556848 4A2A09",
/* %  */ "900A|1020211110|7A8A8B7B7A",
/* &  */ "8420020448492A09077A",
/* '  */ "1012",
/* (  */ "3023273A",
/* )  */ "1033371A",
/* *  */ "1553|3236|1355",
/* +  */ "3339|1656",
/* ,  */ "1A0C",
/* -  */ "1454",
/* .  */ "191A",
/* /  */ "4A00",
/* 0  */ "204062684A2A080220|1828",
/* 1  */ "1230|303A|1A5A",
/* 2  */ "02204062630A6A",
/* 3  */ "0060345466684A2A08",
/* 4  */ "4A400767",
/* 5  */ "601005345567593A09",
/* 6  */ "51301206092A4A58462608",
/* 7  */ "00602A",
/* 8  */ "20405245251220|2545574A2A1725",
/* 9  */ "543514123052585 83A19",
/* :  */ "1415|191A",
/* ;  */ "1415|190C",
/* <  */ "52165A",
/* =  */ "1454|1858",
/* >  */ "12561A",
/* ?  */ "0220405152253 6|393A",
/* @  */ "684A2A0802204062664643332426 46",
/* A  */ "0A386A|1757",
/* B  */ "000A|0040624505|0545674A0A",
/* C  */ "72502102082A5A78",
/* D  */ "000A|005072784A0A",
/* E  */ "60000A6A|0555",
/* F  */ "5000000A|0545",
/* G  */ "72502102082A5A787545",
/* H  */ "000A|707A|0575",
/* I  */ "0020|101A|0A2A",
/* J  */ "40483A1A08",
/* K  */ "000A|60256A",
/* L  */ "000A5A",
/* M  */ "0A0046808A",
/* N  */ "0A007A70",
/* O  */ "205072784A2A080220",
/* P  */ "000A|0050625505",
/* Q  */ "205072784A2A080220|476B",
/* R  */ "000A|0050625505|556A",
/* S  */ "614020020367684A2A09",
/* T  */ "0060|303A",
/* U  */ "00082A4A6860",
/* V  */ "003A70",
/* W  */ "002A548Aa0",
/* X  */ "006A|600A",
/* Y  */ "003560|353A",
/* Z  */ "00600A6A",
/* [  */ "30202A3A",
/* \  */ "004A",
/* ]  */ "10303A1A",
/* ^  */ "043064",
/* _  */ "0C6C",
/* `  */ "1022",
/* a  */ "052444555A|5626081A3A58",
/* b  */ "000A|05244456584A2A09",
/* c  */ "553415193A59",
/* d  */ "606A|65442416182A4A69",
/* e  */ "0757553415193A59",
/* f  */ "3120111A|0333",
/* g  */ "545C4D2D1C|553415193A59",
/* h  */ "000A|052444565A",
/* i  */ "141A|1112",
/* j  */ "242C1D0C|2122",
/* k  */ "000A|4408|264A",
/* l  */ "0010|101A|1A2A",
/* m  */ "040A|05142434 3A|35445464 74 7A|75849404 a5 aA",
/* n  */ "040A|052444565A",
/* o  */ "244455594A2A191524",
/* p  */ "040D|05244456584A2A09",
/* q  */ "545D|553416183A59",
/* r  */ "040A|05143445",
/* s  */ "55341537583A19",
/* t  */ "20293A|0333",
/* u  */ "04091A3A59|545A",
/* v  */ "043A54",
/* w  */ "041A365A74",
/* x  */ "045A|540A",
/* y  */ "043A|541D",
/* z  */ "04540A5A",
/* {  */ "3020243 5 26 2A3A",
/* |  */ "100C",
/* }  */ "1030343 5 26 3A2A",
/* ~  */ "06153755",
};

/* How wide each of those is, in design units, including the space that
 * belongs to it on the right. Read down the column of a rendered line and
 * these are what make it look like text rather than a telegram. */
static const unsigned char WIDTH[95] = {
/*   ! " # $ % & ' ( ) * + , - . / */
   5,3,5,8,8,10,9,3,4,4,6,8,3,6,3,5,
/* 0 1 2 3 4 5 6 7 8 9 : ; < = > ? */
   8,8,8,8,8,8,8,8,8,8,3,3,8,8,8,7,
/* @ A B C D E F G H I J K L M N O */
   9,9,8,9,9,8,7,9,9,4,6,8,7,10,9,9,
/* P Q R S T U V W X Y Z [ \ ] ^ _ */
   8,9,8,8,8,9,8,12,8,8,8,4,5,4,7,8,
/* ` a b c d e f g h i j k l m n o */
   4,7,8,7,8,7,4,8,8,3,4,7,4,12,8,8,
/* p q r s t u v w x y z { | } ~   */
   8,8,5,7,4,8,7,9,7,7,7,4,3,4,8,
};

int gf_advance(uint32_t ch) {
    if (ch >= 32 && ch < 127) return WIDTH[ch - 32];
    if (ch == 0xA0) return WIDTH[0];               /* no-break space */
    return 6;                                      /* the missing-glyph box */
}

int gf_glyph(uint32_t ch, gf_point *out, int max) {
    /* Anything outside the table draws as a hollow box, which is what a
     * missing glyph should look like: visible, and obviously not a letter. */
    static const char BOX[] = "1040484818|1040";
    const char *s;
    if (ch >= 32 && ch < 127) s = GLYPH[ch - 32];
    else if (ch == 0xA0) s = "";
    else s = BOX;

    int n = 0, pen_up = 1;
    for (const char *p = s; *p; ) {
        if (*p == '|') { pen_up = 1; p++; continue; }
        if (*p == ' ') { p++; continue; }           /* padding, so the table can breathe */
        if (!p[1]) break;                           /* a half point is no point */
        int x = p[0] >= 'a' ? p[0] - 'a' + 10 : p[0] - '0';
        int y = p[1] >= 'A' ? p[1] - 'A' + 10 : p[1] - '0';
        p += 2;
        if (x < 0 || x > 12 || y < 0 || y > 13) continue;
        if (n >= max) break;
        out[n].x = (signed char)x;
        out[n].y = (signed char)y;
        out[n].move = (signed char)pen_up;
        n++;
        pen_up = 0;
    }
    return n;
}
