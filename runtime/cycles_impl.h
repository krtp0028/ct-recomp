/* Private single-source cycle accounting, inlined both into the
 * interpreter-facing wrappers in cycles.c and into the production tick in
 * sched.c. cycles.c's CT_PROFILE_TIME wrappers time these; no body is
 * duplicated anywhere. */
#ifndef CT_CYCLES_IMPL_H
#define CT_CYCLES_IMPL_H

#include <stdlib.h>

#include "bus.h"
#include "cpu.h"
#include "cycles.h"

/* The instruction begun and not yet charged. */
struct cyc_cur {
    int pending, compiled;
    uint32_t at;
    uint8_t op, m16, x16, dl, native, interrupt;
    unsigned speed, size;
};
extern struct cyc_cur cur;
extern uint8_t cyc_base[256], cyc_pen[256], cyc_n1[2][256];
extern int built;
extern int check_mode;
extern const uint8_t cyc_op_size[256];   /* bytes with M=X=1 (recomp/decode.py) */
void cyc_build_tables(void);
void cyc_check(void);

/* Master clocks per CPU cycle for code fetched from PB:PC: 6 for FastROM
   (banks $80-$FF ROM with MEMSEL bit 0 set), otherwise 8. */
static inline unsigned cyc_impl_master_per_cycle(uint8_t pb, uint16_t pc)
{
    int rom = pb >= 0xC0 || ((pb & 0x7F) < 0x40 && pc >= 0x8000);
    return (pb & 0x80) && rom && bus_fastrom() ? 6 : 8;
}

static inline unsigned cyc_impl_cur_size(void)
{
    unsigned size = cyc_op_size[cur.op];
    if (cur.m16 && (cur.op & 0x1F) == 0x09)
        size++;
    if (cur.x16 && (cur.op == 0xA0 || cur.op == 0xA2 || cur.op == 0xC0 || cur.op == 0xE0))
        size++;
    return size;
}

/* Clocks of the instruction begun last, by the per-opcode count: the
   opcode fetch (and, for compiled code, the operand fetches it never
   makes) at the fetch speed, every counted bus access at its address's
   speed, the rest as 6-clock internal cycles. */
static inline unsigned cyc_impl_model_clocks(void)
{
    uint8_t pen = cyc_pen[cur.op];
    unsigned n;
    if (cur.compiled && cur.x16 && !cur.dl && cur.native) {
        /* the common state: everything but P_BR is already folded in */
        n = cyc_n1[cur.m16][cur.op];
        if ((pen & P_BR) && ct_cyc_taken)
            n += 1;
    } else {
        n = cyc_base[cur.op];
        if ((pen & P_M) && cur.m16)
            n += 1;
        if ((pen & P_M2) && cur.m16)
            n += 2;
        if ((pen & P_X) && cur.x16)
            n += 1;
        if ((pen & P_DL) && cur.dl)
            n += 1;
        if ((pen & P_IDX) && (ct_cyc_cross || cur.x16))
            n += 1;
        if ((pen & P_BR) && ct_cyc_taken)
            n += 1;
        if ((pen & P_RTI) && cur.native)
            n += 1;
    }
    if (!n)
        ct_fatal("interp $%06X: no cycle count for opcode $%02X", cur.at, cur.op);
    unsigned fetches = cur.compiled ? cur.size - 1 : 0;
    unsigned used = 1 + fetches + ct_bus_n;
    unsigned clocks = cur.speed * (1 + fetches) + ct_bus_clocks;
    if (n > used)
        clocks += (n - used) * 6;
    return clocks;
}

static inline void cyc_impl_begin(const CPU *c, uint32_t at, uint8_t op)
{
    if (!built)
        cyc_build_tables();
    cur.pending = 1;
    cur.at = at;
    cur.op = op;
    cur.m16 = !c->m;
    cur.x16 = !c->x;
    cur.dl = (c->DP & 0xFF) != 0;
    cur.native = !c->e;
    cur.speed = cyc_impl_master_per_cycle((uint8_t)(at >> 16), (uint16_t)at);
    cur.compiled = 0;
    cur.interrupt = 0;
    ct_cyc_cross = ct_cyc_taken = 0;
    ct_bus_clocks = ct_bus_n = 0;   /* the interpreter's opcode fetch is before this */
}

static inline void cyc_impl_begin_compiled(const CPU *c, uint32_t at, uint8_t op)
{
    cyc_impl_begin(c, at, op);
    cur.compiled = 1;
    cur.size = cyc_impl_cur_size();   /* immediates grow with M and X */
}

static inline unsigned cyc_impl_finish(void)
{
    if (!cur.pending)
        return 0;
    if (check_mode < 0)
        check_mode = getenv("CT_CYC_CHECK") != NULL;
    if (check_mode)
        cyc_check();
    cur.pending = 0;
    cur.interrupt = 0;
    return cyc_impl_model_clocks();
}

#endif
