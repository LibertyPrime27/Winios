/* What can be read out of a compiled Direct3D shader without running it.
 *
 * A shader's *signature* is the contract between the vertex data a game
 * supplies and the pipeline: for each input and output, a semantic name, an
 * index, and a register. Reading it is how the layer below knows which bytes
 * of a vertex are a position and which are a colour.
 *
 * The instruction stream is deliberately not decoded. See win32/dxbc.c.
 */
#ifndef W32_DXBC_H
#define W32_DXBC_H

#include <stddef.h>
#include <stdint.h>

enum { DXBC_MAX_SIG = 32 };

typedef struct {
    char     name[32];      /* "POSITION", "TEXCOORD", "COLOR", "SV_Position" */
    uint32_t index;         /* the number after it: TEXCOORD2 is index 2      */
    uint32_t sysvalue;      /* non-zero for a system value such as SV_Position */
    uint32_t comptype;      /* 1 uint, 2 int, 3 float                          */
    uint32_t reg;           /* which register it arrives in                    */
    uint8_t  mask;          /* which components are declared                   */
    uint8_t  rw_mask;       /* which are actually used                         */
} dxbc_element;

typedef struct { int n; dxbc_element e[DXBC_MAX_SIG]; } dxbc_sig;

typedef struct {
    int      valid;
    uint32_t version;       /* the first word of the code chunk                */
    uint32_t code_words;    /* how much code there is, which we do not run     */
    dxbc_sig input, output;
} dxbc_info;

/* Returns non-zero if `blob` is a DXBC container we could walk. */
int dxbc_parse(const void *blob, size_t len, dxbc_info *out);

/* An element by semantic name and index, or NULL. */
const dxbc_element *dxbc_find(const dxbc_sig *sig, const char *semantic, uint32_t index);

#endif
