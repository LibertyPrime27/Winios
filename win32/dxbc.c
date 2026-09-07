/* Reading a compiled shader, as far as is useful.
 *
 * A Direct3D 11 program does not hand over shader source. It hands over
 * DXBC: a container holding the output of Microsoft's HLSL compiler, whose
 * instruction stream is a tokenised bytecode for a GPU that does not exist
 * here. Running that stream is a shader interpreter, and a shader
 * interpreter is its own project.
 *
 * But the container is not opaque, and the part that is not opaque is the
 * part that matters most for getting a picture on screen. Alongside the code
 * a shader carries its *signatures*: for each input and output, the semantic
 * name ("POSITION", "TEXCOORD", "COLOR"), its index, and which register it
 * occupies. That is the contract between the vertex data a game supplies and
 * the pipeline that consumes it -- and it is exactly what is needed to know
 * which bytes of a vertex are a position and which are a colour, without
 * guessing from the layout.
 *
 * So this file reads signatures and nothing else, and is honest about that.
 * What the rest of the D3D11 layer does with them is in d3d11.c: it runs a
 * fixed-function interpretation of the pipeline, which is right for the
 * kind of drawing a 2D engine does and wrong for a shader that does
 * something of its own. Knowing the signature is what makes the first case
 * work; knowing that we did not run the code is what makes the second case
 * reportable rather than silently wrong.
 */
#define _GNU_SOURCE
#include "dxbc.h"

#include <stdio.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The signature chunks. ISGN and OSGN are the originals; ISG1 and OSG1 are
 * the shader-model-5.1 forms, which add fields after the ones we read and so
 * parse identically as far as we go. PCSG is the patch-constant signature of
 * a tessellation shader, which nothing here uses. */
enum {
    FOURCC_ISGN = 0x4E475349u,   /* "ISGN" */
    FOURCC_OSGN = 0x4E47534Fu,   /* "OSGN" */
    FOURCC_ISG1 = 0x31475349u,
    FOURCC_OSG1 = 0x3147534Fu,
    FOURCC_SHDR = 0x52444853u,   /* the code itself, which we do not read */
    FOURCC_SHEX = 0x58454853u,
    FOURCC_DXBC = 0x43425844u,
};

/* One signature chunk into `out`. Each element is 24 bytes: a name offset
 * relative to the start of the chunk's data, then the semantic index, the
 * system-value type, the component type, the register, and the two masks. */
static int parse_signature(const uint8_t *chunk, uint32_t size, dxbc_sig *out) {
    out->n = 0;
    if (size < 8) return 0;
    uint32_t count = rd32(chunk);
    if (count > DXBC_MAX_SIG) count = DXBC_MAX_SIG;
    if (8 + (uint64_t)count * 24 > size) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = chunk + 8 + i * 24;
        uint32_t name_off = rd32(e);
        dxbc_element *el = &out->e[out->n];
        memset(el, 0, sizeof *el);
        /* The name is a NUL-terminated string inside the same chunk. A name
         * pointing outside it is a malformed shader, not a shader with a
         * long name -- so it is dropped rather than read. */
        if (name_off < size) {
            uint32_t max = size - name_off;
            const char *nm = (const char *)chunk + name_off;
            uint32_t j = 0;
            while (j < max && j + 1 < sizeof el->name && nm[j]) { el->name[j] = nm[j]; j++; }
            el->name[j] = 0;
        }
        el->index     = rd32(e + 4);
        el->sysvalue  = rd32(e + 8);
        el->comptype  = rd32(e + 12);
        el->reg       = rd32(e + 16);
        el->mask      = e[20];
        el->rw_mask   = e[21];
        out->n++;
    }
    return 1;
}

int dxbc_parse(const void *blob, size_t len, dxbc_info *out) {
    memset(out, 0, sizeof *out);
    const uint8_t *p = blob;
    if (!p || len < 32) return 0;
    if (rd32(p) != FOURCC_DXBC) return 0;
    uint32_t total = rd32(p + 24);
    /* The container states its own length. Trusting the caller's instead
     * would let a truncated blob walk off the end of the mapping. */
    if (total > len) total = (uint32_t)len;
    uint32_t nchunks = rd32(p + 28);
    if (nchunks > 32) return 0;                    /* no real shader has more */
    if (32 + (uint64_t)nchunks * 4 > total) return 0;

    out->valid = 1;
    for (uint32_t i = 0; i < nchunks; i++) {
        uint32_t off = rd32(p + 32 + i * 4);
        if ((uint64_t)off + 8 > total) continue;
        uint32_t fourcc = rd32(p + off);
        uint32_t size = rd32(p + off + 4);
        if ((uint64_t)off + 8 + size > total) continue;
        const uint8_t *chunk = p + off + 8;
        switch (fourcc) {
        case FOURCC_ISGN: case FOURCC_ISG1: parse_signature(chunk, size, &out->input); break;
        case FOURCC_OSGN: case FOURCC_OSG1: parse_signature(chunk, size, &out->output); break;
        case FOURCC_SHDR: case FOURCC_SHEX:
            /* The code. Recorded, not run -- see the note at the top of the
             * file, and `d3d11_shader_note` in d3d11.c, which is what tells
             * the person that a shader they wrote was not executed. */
            out->code_words = size / 4;
            if (size >= 4) out->version = rd32(chunk);
            break;
        default: break;
        }
    }
    return 1;
}

/* Find an element by semantic. Case-insensitive, because HLSL semantics are:
 * a vertex declaration saying "position" and a shader saying "POSITION" are
 * the same semantic, and matching them exactly would silently drop the
 * position of any game whose tooling writes them in lower case. */
const dxbc_element *dxbc_find(const dxbc_sig *sig, const char *semantic, uint32_t index) {
    for (int i = 0; i < sig->n; i++)
        if (!strcasecmp(sig->e[i].name, semantic) && sig->e[i].index == index)
            return &sig->e[i];
    return 0;
}
