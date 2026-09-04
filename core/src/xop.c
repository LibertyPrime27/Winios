#include "xop.h"
#include <string.h>

static int gpr_slot(ZydisRegister r, int *hi8) {
    int id = ZydisRegisterGetId(r);
    *hi8 = 0;
    if (ZydisRegisterGetClass(r) == ZYDIS_REGCLASS_GPR8) {
        if (id >= 4 && id <= 7) { *hi8 = 1; return id - 4; }     /* AH CH DH BH */
        if (id >= 8) return id - 4;                              /* SPL BPL SIL DIL come after the high bytes */
    }
    return id;
}

static void convert_reg(ZydisRegister r, int mode, xop *o) {
    ZydisRegisterClass cls = ZydisRegisterGetClass(r);
    o->reg = (uint16_t)r;
    o->rbits = (uint8_t)ZydisRegisterGetWidth(mode == 64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LONG_COMPAT_32, r);
    o->rhi8 = 0;
    switch (cls) {
    case ZYDIS_REGCLASS_GPR8: case ZYDIS_REGCLASS_GPR16: case ZYDIS_REGCLASS_GPR32: case ZYDIS_REGCLASS_GPR64: {
        int hi; o->rcls = XR_GPR; o->ridx = (uint8_t)gpr_slot(r, &hi); o->rhi8 = (uint8_t)hi; break;
    }
    case ZYDIS_REGCLASS_XMM:     o->rcls = XR_XMM; o->ridx = (uint8_t)ZydisRegisterGetId(r); o->rbits = 128; break;
    case ZYDIS_REGCLASS_X87:     o->rcls = XR_X87; o->ridx = (uint8_t)(r - ZYDIS_REGISTER_ST0); break;
    case ZYDIS_REGCLASS_SEGMENT: o->rcls = XR_SEG; o->ridx = (uint8_t)(r - ZYDIS_REGISTER_ES); break;
    case ZYDIS_REGCLASS_IP:      o->rcls = XR_IP; o->ridx = 0; break;
    case ZYDIS_REGCLASS_FLAGS:   o->rcls = XR_FLAGS; o->ridx = 0; break;
    default:                     o->rcls = XR_OTHER; o->ridx = 0; break;
    }
}

void xop_convert(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *zops, uint64_t rip, int mode, xop *out) {
    uint64_t next = rip + in->length;
    for (int i = 0; i < in->operand_count; i++) {
        const ZydisDecodedOperand *z = &zops[i];
        xop *o = &out[i];
        memset(o, 0, sizeof *o);
        o->size = z->size;
        o->mbase = o->mindex = -1;
        switch (z->type) {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            o->type = XOP_REG;
            convert_reg(z->reg.value, mode, o);
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY: {
            o->type = XOP_MEM;
            int hi;
            if (z->mem.base == ZYDIS_REGISTER_RIP || z->mem.base == ZYDIS_REGISTER_EIP) o->disp = (int64_t)next;
            else if (z->mem.base != ZYDIS_REGISTER_NONE) o->mbase = (int8_t)gpr_slot(z->mem.base, &hi);
            if (z->mem.index != ZYDIS_REGISTER_NONE) o->mindex = (int8_t)gpr_slot(z->mem.index, &hi);
            o->mscale = z->mem.scale;
            o->disp += z->mem.disp.value;
            o->mseg = z->mem.segment == ZYDIS_REGISTER_FS ? 1 : z->mem.segment == ZYDIS_REGISTER_GS ? 2 : 0;
            break;
        }
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            o->type = XOP_IMM;
            if (z->imm.is_relative) { ZyanU64 t = 0; ZydisCalcAbsoluteAddress(in, z, rip, &t); o->imm = t; }
            else o->imm = z->imm.is_signed ? (uint64_t)z->imm.value.s : z->imm.value.u;
            break;
        default:
            o->type = XOP_NONE;
        }
    }
}
