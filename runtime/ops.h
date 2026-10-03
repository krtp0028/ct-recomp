/* Instruction semantics shared by generated code. */
#ifndef CT_OPS_H
#define CT_OPS_H

#include <stdint.h>

#include "bus.h"
#include "cpu.h"
#include "cycles.h"

/* ---- flags ---- */

static inline void set_nz8(CPU *c, uint8_t v)   { c->n = v >> 7;  c->z = v == 0; }
static inline void set_nz16(CPU *c, uint16_t v) { c->n = v >> 15; c->z = v == 0; }

static inline uint8_t get_p(const CPU *c)
{
    return (uint8_t)(c->n << 7 | c->v << 6 | c->m << 5 | c->x << 4 |
                     c->d << 3 | c->i << 2 | c->z << 1 | c->c);
}

static inline void index_width(CPU *c)
{
    if (c->x) {
        c->X &= 0xFF;
        c->Y &= 0xFF;
    }
}

static inline void set_p(CPU *c, uint8_t p)
{
    c->n = p >> 7 & 1; c->v = p >> 6 & 1; c->m = p >> 5 & 1; c->x = p >> 4 & 1;
    c->d = p >> 3 & 1; c->i = p >> 2 & 1; c->z = p >> 1 & 1; c->c = p & 1;
    if (c->e) {
        c->m = 1;
        c->x = 1;
    }
    index_width(c);
}

/* XCE: swap carry and emulation; entering emulation forces 8-bit A/X and page-1 S. */
static inline void op_xce(CPU *c)
{
    uint8_t t = c->c;
    c->c = c->e;
    c->e = t;
    if (c->e) {
        c->m = 1;
        c->x = 1;
        index_width(c);
        c->S = (uint16_t)(0x0100 | (c->S & 0xFF));
    }
}

static inline void op_rep(CPU *c, uint8_t v) { set_p(c, get_p(c) & (uint8_t)~v); }
static inline void op_sep(CPU *c, uint8_t v) { set_p(c, get_p(c) | v); }

/* ---- loop budget ----
   Charged on every backward branch/jump in generated code. 0 = unlimited. */
static inline void ct_loop(uint32_t at)
{
    if (ct_budget && --ct_budget == 0)
        ct_fatal("$%06X: step budget exhausted", at);
#ifdef CT_TEST_BUILD
    /* Hard backstop, independent of ct_budget: catches a run where the
       caller didn't set it. Test builds only -- see CMakeLists.txt. */
    if (ct_test_cap && --ct_test_cap == 0)
        ct_fatal("$%06X: backward-branch cap exceeded", at);
#endif
}

/* Per-instruction trace hook, called before the instruction at `at` runs. */
static inline void ct_trace(const CPU *c, uint32_t at)
{
    if (ct_trace_hook)
        ct_trace_hook(c, at);
}

/* Start of each generated instruction, in the interpreter's order: the
   scheduler's tick (charge the previous instruction, events, due
   interrupts, begin this one), the trace hook, then the fetch, which
   leaves the instruction's last byte on the data bus (open-bus reads
   return it). */
static inline void ct_insn(CPU *c, uint32_t at, uint8_t op, uint8_t last)
{
    if (ct_tick_hook)
        ct_tick_hook(c, at, op);
    ct_trace(c, at);
    bus_mdr = last;
}

/* The same for a run of straight-line register-only instructions the
   emitter grouped (emit.py): the scheduler charges the run in one step
   when it is provably unobservable (no pending interrupt/DMA/event), and
   otherwise replays the ordinary sequence exactly, trace hook included. */
static inline void ct_insn_run(CPU *c, unsigned n, const uint32_t *at, const uint8_t *op,
                               const uint8_t *last)
{
    if (ct_tick_run && ct_tick_run(c, n, at, op, last))
        return;
    for (unsigned k = 0; k < n; k++)
        ct_insn(c, at[k], op[k], last[k]);
}

/* ---- mode checks ---- */

static inline void cpu_enter(const CPU *c, uint32_t at, int m, int x)
{
    if (c->e || c->m != m || c->x != x)
        ct_fatal("$%06X: entry state m%d x%d e%d, expected m%d x%d e0",
                 at, c->m, c->x, c->e, m, x);
}

static inline void cpu_check_mx(const CPU *c, uint32_t at, int m, int x)
{
    if (c->m != m || c->x != x)
        ct_fatal("$%06X: runtime state m%d x%d, static state m%d x%d",
                 at, c->m, c->x, m, x);
}

/* After a JSR'd callee returns: it must have returned to the JSR site + 3. */
static inline void cpu_check_return(const CPU *c, uint32_t site, uint16_t expect)
{
    if (c->PC != expect)
        ct_fatal("$%06X: callee returned to $%04X, expected $%04X", site, c->PC, expect);
}

/* After a JSL'd callee returns: PB:PC must be the JSL site + 4. */
static inline void cpu_check_return_long(CPU *c, uint32_t site, uint32_t expect)
{
    if (c->PC != (uint16_t)expect || c->PB != (uint8_t)(expect >> 16))
        ct_fatal("$%06X: callee returned to $%02X%04X, expected $%06X", site, c->PB, c->PC,
                 expect);
}

/* ---- effective addresses ---- */

static inline uint32_t ea_dp(const CPU *c, uint8_t d)  { return (uint16_t)(c->DP + d); }
static inline uint32_t ea_abs(const CPU *c, uint16_t a) { return (uint32_t)c->DB << 16 | a; }

/* Indexed absolute/long addresses carry into the next bank. */
/* Indexed reads cost a cycle more when the index crosses a page; the
   addressing helpers record it for the cycle model (cycles.h), exactly
   where the interpreter does. */
static inline uint32_t ea_abs_x(const CPU *c, uint16_t a)
{
    uint32_t b = (uint32_t)c->DB << 16 | a;
    ct_cyc_cross = ((b ^ (b + c->X)) & 0xFF00) != 0;
    return (b + c->X) & 0xFFFFFF;
}

static inline uint32_t ea_abs_y(const CPU *c, uint16_t a)
{
    uint32_t b = (uint32_t)c->DB << 16 | a;
    ct_cyc_cross = ((b ^ (b + c->Y)) & 0xFF00) != 0;
    return (b + c->Y) & 0xFFFFFF;
}

static inline uint32_t ea_long_x(const CPU *c, uint32_t a) { return (a + c->X) & 0xFFFFFF; }

/* Bank-0 wrapping forms (direct page, stack relative). */
static inline uint32_t ea_dp_x(const CPU *c, uint8_t d) { return (uint16_t)(c->DP + d + c->X); }
static inline uint32_t ea_dp_y(const CPU *c, uint8_t d) { return (uint16_t)(c->DP + d + c->Y); }
static inline uint32_t ea_sr(const CPU *c, uint8_t d)   { return (uint16_t)(c->S + d); }

static inline uint16_t read16_b0(uint32_t a)
{
    return (uint16_t)(read8(a) | read8((uint16_t)(a + 1)) << 8);
}

static inline void write16_b0(uint32_t a, uint16_t v)
{
    write8(a, (uint8_t)v);
    write8((uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static inline void write16_b0_rmw(uint32_t a, uint16_t v)
{
    write8((uint16_t)(a + 1), (uint8_t)(v >> 8));
    write8(a, (uint8_t)v);
}

/* Indirect forms: pointer read from bank 0. */
static inline uint32_t ea_dp_ind(const CPU *c, uint8_t d)
{
    return (uint32_t)c->DB << 16 | read16_b0(ea_dp(c, d));
}

static inline uint32_t ea_dp_x_ind(const CPU *c, uint8_t d)
{
    return (uint32_t)c->DB << 16 | read16_b0(ea_dp_x(c, d));
}

static inline uint32_t ea_dp_ind_y(const CPU *c, uint8_t d)
{
    uint32_t b = ea_dp_ind(c, d);
    ct_cyc_cross = ((b ^ (b + c->Y)) & 0xFF00) != 0;
    return (b + c->Y) & 0xFFFFFF;
}

static inline uint32_t ea_dp_ind_long(const CPU *c, uint8_t d)
{
    uint32_t p = ea_dp(c, d);
    return read16_b0(p) | (uint32_t)read8((uint16_t)(p + 2)) << 16;
}

static inline uint32_t ea_dp_ind_long_y(const CPU *c, uint8_t d)
{
    return (ea_dp_ind_long(c, d) + c->Y) & 0xFFFFFF;
}

static inline uint32_t ea_sr_ind_y(const CPU *c, uint8_t d)
{
    return (((uint32_t)c->DB << 16 | read16_b0(ea_sr(c, d))) + c->Y) & 0xFFFFFF;
}

/* Direct-page 16-bit access wraps within bank 0. */
static inline uint16_t read16_dp(const CPU *c, uint8_t d)
{
    return (uint16_t)(read8(ea_dp(c, d)) | read8((uint16_t)(c->DP + d + 1)) << 8);
}

static inline void write16_dp(const CPU *c, uint8_t d, uint16_t v)
{
    write8(ea_dp(c, d), (uint8_t)v);
    write8((uint16_t)(c->DP + d + 1), (uint8_t)(v >> 8));
}

/* ---- stack (bank 0) ---- */

static inline void push8(CPU *c, uint8_t v)
{
    write8(c->S, v);
    c->S = c->e ? (uint16_t)(0x0100 | ((c->S - 1) & 0xFF)) : (uint16_t)(c->S - 1);
}

static inline uint8_t pull8(CPU *c)
{
    c->S = c->e ? (uint16_t)(0x0100 | ((c->S + 1) & 0xFF)) : (uint16_t)(c->S + 1);
    return read8(c->S);
}

static inline void push16(CPU *c, uint16_t v)
{
    push8(c, (uint8_t)(v >> 8));
    push8(c, (uint8_t)v);
}

static inline uint16_t pull16(CPU *c)
{
    uint8_t lo = pull8(c);
    return (uint16_t)(lo | pull8(c) << 8);
}

/* RTS: pull return address - 1. RTL: also pull the bank. */
static inline void op_rts(CPU *c) { c->PC = (uint16_t)(pull16(c) + 1); }

/* RTI (native mode): P, then PC, then PB. Outside the frame scheduler
   (diff_all) the strict interpreter doesn't run interrupt returns, and
   generated code fails the same way there. */
static inline void op_rti(CPU *c, uint32_t at)
{
    if (!ct_exec_hook)
        ct_fatal("interp $%06X: opcode $40 not supported", at);
    set_p(c, pull8(c));
    c->PC = pull16(c);
    c->PB = pull8(c);
}

static inline void op_rtl(CPU *c)
{
    c->PC = (uint16_t)(pull16(c) + 1);
    c->PB = pull8(c);
}

/* ---- accumulator ---- */

static inline uint8_t a8(const CPU *c) { return (uint8_t)c->A; }
static inline void set_a8(CPU *c, uint8_t v) { c->A = (uint16_t)((c->A & 0xFF00) | v); }

static inline void lda8(CPU *c, uint8_t v)   { set_a8(c, v); set_nz8(c, v); }
static inline void lda16(CPU *c, uint16_t v) { c->A = v; set_nz16(c, v); }
static inline void ldx16(CPU *c, uint16_t v) { c->X = v; set_nz16(c, v); }
static inline void ldy16(CPU *c, uint16_t v) { c->Y = v; set_nz16(c, v); }
static inline void ldx8(CPU *c, uint8_t v)   { c->X = v; set_nz8(c, v); }
static inline void ldy8(CPU *c, uint8_t v)   { c->Y = v; set_nz8(c, v); }

static inline void ora8(CPU *c, uint8_t v)   { set_a8(c, a8(c) | v); set_nz8(c, a8(c)); }
static inline void and8(CPU *c, uint8_t v)   { set_a8(c, a8(c) & v); set_nz8(c, a8(c)); }
static inline void eor8(CPU *c, uint8_t v)   { set_a8(c, a8(c) ^ v); set_nz8(c, a8(c)); }
static inline void ora16(CPU *c, uint16_t v) { c->A |= v; set_nz16(c, c->A); }
static inline void and16(CPU *c, uint16_t v) { c->A &= v; set_nz16(c, c->A); }
static inline void eor16(CPU *c, uint16_t v) { c->A ^= v; set_nz16(c, c->A); }

/* BIT: immediate form sets Z only. */
static inline void bit8(CPU *c, uint8_t v, int imm)
{
    c->z = (a8(c) & v) == 0;
    if (!imm) {
        c->n = v >> 7;
        c->v = (v >> 6) & 1;
    }
}

static inline void bit16(CPU *c, uint16_t v, int imm)
{
    c->z = (c->A & v) == 0;
    if (!imm) {
        c->n = v >> 15;
        c->v = (v >> 14) & 1;
    }
}

/* ---- read-modify-write values ---- */

static inline uint16_t dec16(CPU *c, uint16_t v) { v = (uint16_t)(v - 1); set_nz16(c, v); return v; }
static inline uint16_t inc16(CPU *c, uint16_t v) { v = (uint16_t)(v + 1); set_nz16(c, v); return v; }
static inline uint8_t asl8(CPU *c, uint8_t v)  { c->c = v >> 7; v = (uint8_t)(v << 1); set_nz8(c, v); return v; }
static inline uint8_t lsr8(CPU *c, uint8_t v)  { c->c = v & 1; v >>= 1; set_nz8(c, v); return v; }
static inline uint8_t rol8(CPU *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v << 1 | c->c);
    c->c = v >> 7;
    set_nz8(c, r);
    return r;
}
static inline uint8_t ror8(CPU *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v >> 1 | c->c << 7);
    c->c = v & 1;
    set_nz8(c, r);
    return r;
}
static inline uint16_t asl16(CPU *c, uint16_t v) { c->c = v >> 15; v = (uint16_t)(v << 1); set_nz16(c, v); return v; }
static inline uint16_t lsr16(CPU *c, uint16_t v) { c->c = v & 1; v >>= 1; set_nz16(c, v); return v; }
static inline uint16_t rol16(CPU *c, uint16_t v)
{
    uint16_t r = (uint16_t)(v << 1 | c->c);
    c->c = v >> 15;
    set_nz16(c, r);
    return r;
}
static inline uint16_t ror16(CPU *c, uint16_t v)
{
    uint16_t r = (uint16_t)(v >> 1 | c->c << 15);
    c->c = v & 1;
    set_nz16(c, r);
    return r;
}

/* TSB/TRB: Z from A & mem, then set/clear A's bits in mem. */
static inline uint8_t tsb8(CPU *c, uint8_t v)   { c->z = (a8(c) & v) == 0; return v | a8(c); }
static inline uint8_t trb8(CPU *c, uint8_t v)   { c->z = (a8(c) & v) == 0; return (uint8_t)(v & ~a8(c)); }
static inline uint16_t tsb16(CPU *c, uint16_t v) { c->z = (c->A & v) == 0; return v | c->A; }
static inline uint16_t trb16(CPU *c, uint16_t v) { c->z = (c->A & v) == 0; return (uint16_t)(v & ~c->A); }

/* ---- index registers (16-bit forms; x=0) ---- */

static inline void tax16(CPU *c) { c->X = c->A; set_nz16(c, c->X); }
static inline void tay16(CPU *c) { c->Y = c->A; set_nz16(c, c->Y); }
static inline void iny16(CPU *c) { c->Y = (uint16_t)(c->Y + 1); set_nz16(c, c->Y); }
static inline void inx16(CPU *c) { c->X = (uint16_t)(c->X + 1); set_nz16(c, c->X); }
static inline void dex16(CPU *c) { c->X = (uint16_t)(c->X - 1); set_nz16(c, c->X); }

/* MVN with 16-bit index registers: copy C+1 bytes src:X -> dst:Y ascending,
   one byte per execution of the instruction: every byte after the first
   is its own instruction start (ct_insn, 7 cycles each), so interrupts and
   frame edges fall between bytes. Ends with A=$FFFF, DB=dst. */
static inline void mvn16(CPU *c, uint32_t at, uint8_t dst, uint8_t src)
{
    c->DB = dst;
    for (;;) {
        write8((uint32_t)dst << 16 | c->Y, read8((uint32_t)src << 16 | c->X));
        c->X = (uint16_t)(c->X + 1);
        c->Y = (uint16_t)(c->Y + 1);
        c->A = (uint16_t)(c->A - 1);
        if (c->A == 0xFFFF)
            break;
        ct_insn(c, at, 0x54, src);
    }
}

static inline void op_tdc(CPU *c) { c->A = c->DP; set_nz16(c, c->A); }

static inline void asl_a8(CPU *c)
{
    uint8_t v = a8(c);
    c->c = v >> 7;
    v = (uint8_t)(v << 1);
    set_a8(c, v);
    set_nz8(c, v);
}

static inline void asl_a16(CPU *c)
{
    c->c = c->A >> 15;
    c->A = (uint16_t)(c->A << 1);
    set_nz16(c, c->A);
}

static inline void lsr_a8(CPU *c)
{
    uint8_t v = a8(c);
    c->c = v & 1;
    v >>= 1;
    set_a8(c, v);
    set_nz8(c, v);
}

static inline void lsr_a16(CPU *c)
{
    c->c = c->A & 1;
    c->A >>= 1;
    set_nz16(c, c->A);
}

static inline uint8_t inc8(CPU *c, uint8_t v) { v = (uint8_t)(v + 1); set_nz8(c, v); return v; }
static inline uint8_t dec8(CPU *c, uint8_t v) { v = (uint8_t)(v - 1); set_nz8(c, v); return v; }

/* ---- arithmetic (binary only; decimal mode is fatal in v0) ---- */

static inline void adc8(CPU *c, uint8_t b, uint32_t at)
{
    if (c->d)
        ct_fatal("$%06X: ADC with D=1 not supported", at);
    uint8_t a = a8(c);
    unsigned r = (unsigned)a + b + c->c;
    c->c = r > 0xFF;
    c->v = ((~(a ^ b) & (a ^ r)) >> 7) & 1;
    set_a8(c, (uint8_t)r);
    set_nz8(c, (uint8_t)r);
}

static inline void adc16(CPU *c, uint16_t b, uint32_t at)
{
    if (c->d)
        ct_fatal("$%06X: ADC with D=1 not supported", at);
    uint16_t a = c->A;
    unsigned r = (unsigned)a + b + c->c;
    c->c = r > 0xFFFF;
    c->v = ((~(a ^ b) & (a ^ r)) >> 15) & 1;
    c->A = (uint16_t)r;
    set_nz16(c, c->A);
}

static inline void sbc8(CPU *c, uint8_t b, uint32_t at)
{
    if (c->d)
        ct_fatal("$%06X: SBC with D=1 not supported", at);
    adc8(c, (uint8_t)~b, at);
}

static inline void sbc16(CPU *c, uint16_t b, uint32_t at)
{
    if (c->d)
        ct_fatal("$%06X: SBC with D=1 not supported", at);
    adc16(c, (uint16_t)~b, at);
}

static inline void cmp8(CPU *c, uint8_t r, uint8_t b)
{
    c->c = r >= b;
    set_nz8(c, (uint8_t)(r - b));
}

static inline void cmp16(CPU *c, uint16_t r, uint16_t b)
{
    c->c = r >= b;
    set_nz16(c, (uint16_t)(r - b));
}

#endif
