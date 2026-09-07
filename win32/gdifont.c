/* A stroke font, because there is no font file to load.
 *
 * A dialog is mostly text, so nothing renders without one, and the two easy
 * answers are both wrong here: shipping a bitmap font locks the text to one
 * size, and a dialog asks for whatever size its template says; shipping
 * somebody's typeface means shipping somebody's typeface.
 *
 * So the glyphs are strokes -- polylines on a 7x13 design grid, drawn with
 * lines at whatever scale the caller asked for. That is the same thing a pen
 * plotter did, and it has the properties this needs: any size, any weight
 * (draw it twice, offset by one, for bold), a few kilobytes, and ours.
 *
 * The grid: x runs 0..6, y runs 0..13 downward, with the cap line at y=0, the
 * x-height line at y=4, the baseline at y=10 and the descender at y=13. The
 * advance is 7 units, so a glyph has one unit of right side bearing. Text at
 * a requested cell height H is scaled by H/13.
 *
 * Encoding: two characters per point -- x is '0'..'6', y is '0'..'9' then
 * 'A'..'D' for 10..13 -- and '|' lifts the pen. It reads badly and it packs
 * 95 glyphs into a screenful, which is the trade a table like this should
 * make.
 */
#include "gdifont.h"

#include <string.h>

/* ASCII 32..126. Empty means "draw nothing", which is correct for a space. */
static const char *const GLYPH[95] = {
/* sp */ "",
/* !  */ "3037|393A",
/* "  */ "2022|4042",
/* #  */ "101A|505A|0464|0868",
/* $  */ "303A|5140200205556848 4A2A09",
/* %  */ "600A|10202111 10|4A5A5B4B4A",
/* &  */ "6420020448492A09076A",
/* '  */ "3032",
/* (  */ "4023274A",
/* )  */ "2043472A",
/* *  */ "1553|3236|1355",
/* +  */ "3339|1656",
/* ,  */ "3A2C",
/* -  */ "1656",
/* .  */ "393A",
/* /  */ "5A10",
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
/* :  */ "3435|393A",
/* ;  */ "3435|392C",
/* <  */ "52165A",
/* =  */ "1454|1858",
/* >  */ "12561A",
/* ?  */ "0220406263363 7|393A",
/* @  */ "684A2A0802204062664643332426 46",
/* A  */ "0A306A|1757",
/* B  */ "000A|0040624505|0545674A0A",
/* C  */ "62402002082A4A68",
/* D  */ "000A|004062684A0A",
/* E  */ "60000A6A|0555",
/* F  */ "6000000A|0555",
/* G  */ "62402002082A4A686535",
/* H  */ "000A|606A|0565",
/* I  */ "1050|303A|1A5A",
/* J  */ "50583A1A08",
/* K  */ "000A|60256A",
/* L  */ "000A6A",
/* M  */ "0A0036606A",
/* N  */ "0A006A60",
/* O  */ "204062684A2A080220",
/* P  */ "000A|0040624505",
/* Q  */ "204062684A2A080220|376B",
/* R  */ "000A|0040624505|456A",
/* S  */ "614020020367684A2A09",
/* T  */ "0060|303A",
/* U  */ "00082A4A6860",
/* V  */ "003A60",
/* W  */ "001A355A60",
/* X  */ "006A|600A",
/* Y  */ "003560|353A",
/* Z  */ "00600A6A",
/* [  */ "40202A4A",
/* \  */ "105A",
/* ]  */ "20404A2A",
/* ^  */ "043064",
/* _  */ "0C6C",
/* `  */ "2032",
/* a  */ "052444555A|5626081A3A58",
/* b  */ "000A|05244456584A2A09",
/* c  */ "553415193A59",
/* d  */ "606A|65442416182A4A69",
/* e  */ "0757553415193A59",
/* f  */ "4130212A|0444",
/* g  */ "545C4D2D1C|553415193A59",
/* h  */ "000A|052444565A",
/* i  */ "343A|3132",
/* j  */ "444C3D2D1C|4142",
/* k  */ "000A|4408|264A",
/* l  */ "2030|303A|3A4A",
/* m  */ "040A|051424343A|354454656A",
/* n  */ "040A|052444565A",
/* o  */ "244455594A2A191524",
/* p  */ "040D|05244456584A2A09",
/* q  */ "545D|553416183A59",
/* r  */ "040A|05143445",
/* s  */ "55341537583A19",
/* t  */ "30394A|1454",
/* u  */ "04091A3A59|545A",
/* v  */ "043A54",
/* w  */ "041A365A64",
/* x  */ "045A|540A",
/* y  */ "043A|541D",
/* z  */ "04540A5A",
/* {  */ "4030341536 3A4A",
/* |  */ "300C",
/* }  */ "203034553 63A2A",
/* ~  */ "06153755",
};

int gf_glyph(uint32_t ch, gf_point *out, int max) {
    /* Anything outside the table draws as a hollow box, which is what a
     * missing glyph should look like: visible, and obviously not a letter. */
    static const char BOX[] = "1040484 8 10";
    const char *s;
    if (ch >= 32 && ch < 127) s = GLYPH[ch - 32];
    else if (ch == 0xA0) s = "";                    /* no-break space */
    else s = BOX;

    int n = 0, pen_up = 1;
    for (const char *p = s; *p; ) {
        if (*p == '|') { pen_up = 1; p++; continue; }
        if (*p == ' ') { p++; continue; }           /* padding, so the table can breathe */
        if (!p[1]) break;                           /* a half point is no point */
        int x = p[0] - '0';
        int y = p[1] >= 'A' ? p[1] - 'A' + 10 : p[1] - '0';
        p += 2;
        if (x < 0 || x > 6 || y < 0 || y > 13) continue;
        if (n >= max) break;
        out[n].x = (signed char)x;
        out[n].y = (signed char)y;
        out[n].move = (signed char)pen_up;
        n++;
        pen_up = 0;
    }
    return n;
}
