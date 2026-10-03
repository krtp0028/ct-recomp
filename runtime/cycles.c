#include "cycles.h"

#include <stdio.h>
#include <stdlib.h>

#include "bus.h"
#include "cycles_impl.h"

int ct_cyc_cross;
int ct_cyc_taken;

uint8_t cyc_base[256], cyc_pen[256];
int built;

static void cyc(uint8_t op, uint8_t base, uint8_t pen)
{
    cyc_base[op] = base;
    cyc_pen[op] = pen;
}

static void build_cycle_table(void)
{
    /* ALU group: ORA AND EOR ADC STA LDA CMP SBC by addressing column. */
    static const struct { uint8_t lo, base, pen; } col[] = {
        {0x01, 6, P_DL}, {0x03, 4, 0}, {0x05, 3, P_DL}, {0x07, 6, P_DL}, {0x09, 2, 0},
        {0x0D, 4, 0}, {0x0F, 5, 0}, {0x11, 5, P_DL | P_IDX}, {0x12, 5, P_DL}, {0x13, 7, 0},
        {0x15, 4, P_DL}, {0x17, 6, P_DL}, {0x19, 4, P_IDX}, {0x1D, 4, P_IDX}, {0x1F, 5, 0},
    };
    for (unsigned hi = 0; hi < 8; hi++)
        for (unsigned k = 0; k < sizeof col / sizeof col[0]; k++)
            cyc((uint8_t)(hi << 5 | col[k].lo), col[k].base, (uint8_t)(col[k].pen | P_M));
    cyc(0x91, 6, P_DL | P_M);      /* STA (dp),Y: writes take the extra cycle always */
    cyc(0x99, 5, P_M);             /* STA abs,Y */
    cyc(0x9D, 5, P_M);             /* STA abs,X */

    /* read-modify-write: ASL ROL LSR ROR / INC DEC */
    static const uint8_t rmw_ops[] = {0x00, 0x20, 0x40, 0x60, 0xE0, 0xC0};
    for (unsigned k = 0; k < sizeof rmw_ops; k++) {
        cyc((uint8_t)(rmw_ops[k] | 0x06), 5, P_DL | P_M2);
        cyc((uint8_t)(rmw_ops[k] | 0x0E), 6, P_M2);
        cyc((uint8_t)(rmw_ops[k] | 0x16), 6, P_DL | P_M2);
        cyc((uint8_t)(rmw_ops[k] | 0x1E), 7, P_M2);
    }
    static const uint8_t two[] = {
        0x0A, 0x2A, 0x4A, 0x6A, 0x1A, 0x3A,                         /* accumulator RMW */
        0xE8, 0xC8, 0xCA, 0x88,                                     /* INX INY DEX DEY */
        0xAA, 0xA8, 0x8A, 0x98, 0x9B, 0xBB, 0xBA, 0x9A, 0x5B, 0x7B, 0x1B, 0x3B,
        0x18, 0x38, 0x58, 0x78, 0xD8, 0xF8, 0xB8, 0xFB, 0xEA, 0x42,
    };
    for (unsigned k = 0; k < sizeof two; k++)
        cyc(two[k], 2, 0);
    cyc(0x04, 5, P_DL | P_M2); cyc(0x0C, 6, P_M2);                 /* TSB */
    cyc(0x14, 5, P_DL | P_M2); cyc(0x1C, 6, P_M2);                 /* TRB */
    cyc(0x89, 2, P_M); cyc(0x24, 3, P_DL | P_M); cyc(0x2C, 4, P_M); /* BIT */
    cyc(0x34, 4, P_DL | P_M); cyc(0x3C, 4, P_M | P_IDX);
    cyc(0xA2, 2, P_X); cyc(0xA6, 3, P_DL | P_X); cyc(0xAE, 4, P_X); /* LDX */
    cyc(0xB6, 4, P_DL | P_X); cyc(0xBE, 4, P_X | P_IDX);
    cyc(0xA0, 2, P_X); cyc(0xA4, 3, P_DL | P_X); cyc(0xAC, 4, P_X); /* LDY */
    cyc(0xB4, 4, P_DL | P_X); cyc(0xBC, 4, P_X | P_IDX);
    cyc(0x86, 3, P_DL | P_X); cyc(0x8E, 4, P_X); cyc(0x96, 4, P_DL | P_X); /* STX */
    cyc(0x84, 3, P_DL | P_X); cyc(0x8C, 4, P_X); cyc(0x94, 4, P_DL | P_X); /* STY */
    cyc(0x64, 3, P_DL | P_M); cyc(0x74, 4, P_DL | P_M);             /* STZ */
    cyc(0x9C, 4, P_M); cyc(0x9E, 5, P_M);
    cyc(0xE0, 2, P_X); cyc(0xE4, 3, P_DL | P_X); cyc(0xEC, 4, P_X); /* CPX */
    cyc(0xC0, 2, P_X); cyc(0xC4, 3, P_DL | P_X); cyc(0xCC, 4, P_X); /* CPY */
    cyc(0xEB, 3, 0);                                                /* XBA */
    cyc(0x48, 3, P_M); cyc(0xDA, 3, P_X); cyc(0x5A, 3, P_X);        /* PHA PHX PHY */
    cyc(0x68, 4, P_M); cyc(0xFA, 4, P_X); cyc(0x7A, 4, P_X);        /* PLA PLX PLY */
    cyc(0x8B, 3, 0); cyc(0xAB, 4, 0); cyc(0x0B, 4, 0); cyc(0x2B, 5, 0);
    cyc(0x4B, 3, 0); cyc(0x08, 3, 0); cyc(0x28, 4, 0);
    cyc(0xF4, 5, 0); cyc(0xD4, 6, P_DL); cyc(0x62, 6, 0);           /* PEA PEI PER */
    cyc(0xC2, 3, 0); cyc(0xE2, 3, 0);                               /* REP SEP */
    static const uint8_t br[] = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0, 0x80};
    for (unsigned k = 0; k < sizeof br; k++)
        cyc(br[k], 2, P_BR);
    cyc(0x82, 4, 0);                                                /* BRL */
    cyc(0x4C, 3, 0); cyc(0x5C, 4, 0); cyc(0x6C, 5, 0); cyc(0x7C, 6, 0); cyc(0xDC, 6, 0);
    cyc(0x20, 6, 0); cyc(0xFC, 8, 0); cyc(0x22, 8, 0);
    cyc(0x60, 6, 0); cyc(0x6B, 6, 0); cyc(0x40, 6, P_RTI);
    cyc(0x54, 7, 0); cyc(0x44, 7, 0);                               /* MVN MVP: per byte */
    cyc(0xCB, 3, 0); cyc(0xDB, 3, 0);                               /* WAI STP */
}

unsigned cyc_master_per_cycle(uint8_t pb, uint16_t pc)
{
    return cyc_impl_master_per_cycle(pb, pc);
}

/* The instruction begun and not yet charged. */

static unsigned elapsed_fetches(void);
static void build_templates(void);

const uint8_t cyc_op_size[256] = {   /* bytes with M=X=1 (recomp/decode.py) */
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 4,
    3, 2, 4, 2, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 4,
    1, 2, 2, 2, 3, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 3, 2, 2, 2, 1, 3, 1, 1, 4, 3, 3, 4,
    1, 2, 3, 2, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 4,
    2, 2, 3, 2, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 4,
    2, 2, 2, 2, 3, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 4,
};

struct cyc_cur cur;

void cyc_begin(const CPU *c, uint32_t at, uint8_t op)
{
    cyc_impl_begin(c, at, op);
}

int cyc_in_progress(void) { return cur.pending || cur.interrupt; }

void cyc_done(void) { cur.interrupt = 0; }

void cyc_begin_interrupt(const CPU *c)
{
    cyc_begin(c, (uint32_t)c->PB << 16 | c->PC, 0);
    cur.pending = 0;   /* charged by the caller, not cyc_finish */
    cur.interrupt = 1;
}

void cyc_begin_compiled(const CPU *c, uint32_t at, uint8_t op)
{
    cyc_impl_begin_compiled(c, at, op);
}

static unsigned elapsed_fetches(void)
{
    return cur.compiled ? cur.size - 1 : 0;
}

unsigned cyc_elapsed(void)
{
    if (!cur.pending)
        return 0;
    return cur.speed * (1 + elapsed_fetches()) + ct_bus_clocks;
}

/* ---- cycle by cycle ----
   Each opcode's cycles in order (65C816 datasheet table 5-7):
     F opcode fetch            O operand fetch
     D data/stack/vector access (one bus access, in order)
     I internal cycle (6 clocks)
   and the conditional ones:
     d I if the low byte of DP is nonzero    x I if the index crossed a page or X=0
     b I if the branch was taken             m D if M=0 (16-bit data)
     w D if X=0 (16-bit index data)          p O if M=0 (16-bit immediate)
     q O if X=0 (16-bit index immediate)     n D in native mode (RTI's PB)
   Fetches run at the fetch speed; compiled code never makes its operand
   fetches, so they are synthesized. */
static const char *tmpl[256];

static void build_templates(void)
{
    static const struct { uint8_t lo; const char *t; } col[] = {
        {0x01, "FOdIDDDm"}, {0x03, "FOIDm"}, {0x05, "FOdDm"}, {0x07, "FOdDDDDm"},
        {0x09, "FOp"}, {0x0D, "FOODm"}, {0x0F, "FOOODm"}, {0x11, "FOdDDxDm"},
        {0x12, "FOdDDDm"}, {0x13, "FOIDDIDm"}, {0x15, "FOdIDm"}, {0x17, "FOdDDDDm"},
        {0x19, "FOOxDm"}, {0x1D, "FOOxDm"}, {0x1F, "FOOODm"},
    };
    for (unsigned hi = 0; hi < 8; hi++)
        for (unsigned k = 0; k < sizeof col / sizeof col[0]; k++)
            tmpl[hi << 5 | col[k].lo] = col[k].t;
    tmpl[0x91] = "FOdDDIDm";   /* STA (dp),Y / abs,Y / abs,X: the index cycle always */
    tmpl[0x99] = "FOOIDm";
    tmpl[0x9D] = "FOOIDm";
    static const uint8_t rmw_ops[] = {0x00, 0x20, 0x40, 0x60, 0xE0, 0xC0};
    for (unsigned k = 0; k < sizeof rmw_ops; k++) {   /* read, internal, write */
        tmpl[rmw_ops[k] | 0x06] = "FOdDmImD";
        tmpl[rmw_ops[k] | 0x0E] = "FOODmImD";
        tmpl[rmw_ops[k] | 0x16] = "FOdIDmImD";
        tmpl[rmw_ops[k] | 0x1E] = "FOOIDmImD";
    }
    static const uint8_t two[] = {
        0x0A, 0x2A, 0x4A, 0x6A, 0x1A, 0x3A, 0xE8, 0xC8, 0xCA, 0x88,
        0xAA, 0xA8, 0x8A, 0x98, 0x9B, 0xBB, 0xBA, 0x9A, 0x5B, 0x7B, 0x1B, 0x3B,
        0x18, 0x38, 0x58, 0x78, 0xD8, 0xF8, 0xB8, 0xFB, 0xEA,
    };
    for (unsigned k = 0; k < sizeof two; k++)
        tmpl[two[k]] = "FI";
    tmpl[0x42] = "FO";                                             /* WDM */
    tmpl[0x04] = tmpl[0x14] = "FOdDmImD";                          /* TSB TRB */
    tmpl[0x0C] = tmpl[0x1C] = "FOODmImD";
    tmpl[0x89] = "FOp"; tmpl[0x24] = "FOdDm"; tmpl[0x2C] = "FOODm"; /* BIT */
    tmpl[0x34] = "FOdIDm"; tmpl[0x3C] = "FOOxDm";
    tmpl[0xA2] = tmpl[0xA0] = "FOq";                               /* LDX LDY */
    tmpl[0xA6] = tmpl[0xA4] = "FOdDw";
    tmpl[0xAE] = tmpl[0xAC] = "FOODw";
    tmpl[0xB6] = tmpl[0xB4] = "FOdIDw";
    tmpl[0xBE] = tmpl[0xBC] = "FOOxDw";
    tmpl[0x86] = tmpl[0x84] = "FOdDw";                             /* STX STY */
    tmpl[0x8E] = tmpl[0x8C] = "FOODw";
    tmpl[0x96] = tmpl[0x94] = "FOdIDw";
    tmpl[0x64] = "FOdDm"; tmpl[0x74] = "FOdIDm";                   /* STZ */
    tmpl[0x9C] = "FOODm"; tmpl[0x9E] = "FOOIDm";
    tmpl[0xE0] = tmpl[0xC0] = "FOq";                               /* CPX CPY */
    tmpl[0xE4] = tmpl[0xC4] = "FOdDw";
    tmpl[0xEC] = tmpl[0xCC] = "FOODw";
    tmpl[0xEB] = "FII";                                            /* XBA */
    tmpl[0x48] = "FIDm"; tmpl[0xDA] = tmpl[0x5A] = "FIDw";         /* PHA PHX PHY */
    tmpl[0x68] = "FIIDm"; tmpl[0xFA] = tmpl[0x7A] = "FIIDw";       /* PLA PLX PLY */
    tmpl[0x8B] = tmpl[0x4B] = tmpl[0x08] = "FID";                  /* PHB PHK PHP */
    tmpl[0xAB] = tmpl[0x28] = "FIID";                              /* PLB PLP */
    tmpl[0x0B] = "FIDD"; tmpl[0x2B] = "FIIDD";                     /* PHD PLD */
    tmpl[0xF4] = "FOODD"; tmpl[0xD4] = "FOdDDDD"; tmpl[0x62] = "FOOIDD"; /* PEA PEI PER */
    tmpl[0xC2] = tmpl[0xE2] = "FOI";                               /* REP SEP */
    static const uint8_t br[] = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0, 0x80};
    for (unsigned k = 0; k < sizeof br; k++)
        tmpl[br[k]] = "FOb";
    tmpl[0x82] = "FOOI";                                           /* BRL */
    tmpl[0x4C] = "FOO"; tmpl[0x5C] = "FOOO"; tmpl[0x6C] = "FOODD"; /* JMP JML */
    tmpl[0x7C] = "FOOIDD"; tmpl[0xDC] = "FOODDD";
    tmpl[0x20] = "FOOIDD";                                         /* JSR: push PCH, PCL */
    tmpl[0xFC] = "FODDOIDD";                                       /* JSR (a,X) */
    tmpl[0x22] = "FOODIODD";                                       /* JSL: push PB, bank */
    tmpl[0x60] = "FIIDDI"; tmpl[0x6B] = "FIIDDD"; tmpl[0x40] = "FIIDDDn";   /* RTS RTL RTI */
    tmpl[0x54] = tmpl[0x44] = "FOODDII";                           /* MVN MVP: one byte */
    tmpl[0xCB] = tmpl[0xDB] = "FII";                               /* WAI STP */
}

void cyc_build_tables(void)
{
    build_cycle_table();
    build_templates();
    built = 1;
}

/* Interrupt entry: a fetch at PB:PC, an internal cycle, the pushes and the
   vector reads. */
static const char *const tmpl_int = "FIDDDDDD";

unsigned cyc_cycles(uint8_t *clk, uint8_t *kind, int8_t *idx, unsigned max, unsigned upto)
{
    const char *t = cur.interrupt ? tmpl_int : tmpl[cur.op];
    if (!t)
        return 0;
    unsigned n = 0, fe = 0, da = 0;   /* next fetch / data log entries */
    unsigned logged = ct_bus_n < CT_BUS_LOG ? ct_bus_n : CT_BUS_LOG;
    for (; *t && n < max; t++) {
        int c = *t, use;
        switch (c) {
        case 'd': use = cur.dl; c = 'I'; break;
        case 'x': use = ct_cyc_cross || cur.x16; c = 'I'; break;
        case 'b': use = ct_cyc_taken; c = 'I'; break;
        case 'm': use = cur.m16; c = 'D'; break;
        case 'w': use = cur.x16; c = 'D'; break;
        case 'n': use = cur.native; c = 'D'; break;
        case 'p': use = cur.m16; c = 'O'; break;
        case 'q': use = cur.x16; c = 'O'; break;
        default: use = 1;
        }
        if (!use)
            continue;
        idx[n] = -1;
        if (c == 'F' || (c == 'O' && cur.compiled)) {
            clk[n] = (uint8_t)cur.speed;
            kind[n++] = CY_READ;
        } else if (c == 'I') {
            clk[n] = 6;
            kind[n++] = CY_IDLE;
        } else {
            int fetch = c == 'O';
            unsigned *cursor = fetch ? &fe : &da;
            while (*cursor < logged && !(ct_bus_log[*cursor] & CT_BUS_FETCH) != !fetch)
                ++*cursor;
            if (*cursor >= logged) {
                if (upto != ~0u)
                    break;   /* not made yet */
                /* Compiled code reads some ROM tables (JMP/JSR (a,X)) without
                   the bus; the count charges those as 6-clock cycles. */
                clk[n] = 6;
                kind[n++] = CY_READ;
                continue;
            }
            uint8_t e = ct_bus_log[*cursor];
            clk[n] = e & 0x3F;
            idx[n] = (int8_t)*cursor;
            kind[n++] = (e & CT_BUS_WRITE) ? CY_WRITE : CY_READ;
            if (*cursor == upto)
                break;
            ++*cursor;
        }
    }
    return n;
}

void cyc_check(void)
{
    uint8_t clk[128], kind[128];
    int8_t idx[128];
    if (cur.interrupt || !tmpl[cur.op] || ct_bus_n > CT_BUS_LOG)
        return;
    unsigned n = cyc_cycles(clk, kind, idx, sizeof clk, ~0u), total = 0;
    for (unsigned k = 0; k < n; k++)
        total += clk[k];
    unsigned want = cyc_impl_model_clocks();
    if (total != want) {
        static uint32_t seen[64];
        static unsigned n_seen;
        uint32_t key = cur.op | cur.m16 << 8 | cur.x16 << 9 | cur.dl << 10 | cur.compiled << 11;
        for (unsigned k = 0; k < n_seen; k++)
            if (seen[k] == key)
                return;
        if (n_seen < 64)
            seen[n_seen++] = key;
        fprintf(stderr, "cyc_check $%06X op $%02X m16=%d x16=%d dl=%d compiled=%d: template %u "
                "clocks in %u cycles, model %u (bus %u accesses)\n", cur.at, cur.op, cur.m16,
                cur.x16, cur.dl, cur.compiled, total, n, want, ct_bus_n);
    }
}

/* Clocks of the instruction begun last, by the per-opcode count: the
   opcode fetch (and, for compiled code, the operand fetches it never
   makes) at the fetch speed, every counted bus access at its address's
   speed, the rest as 6-clock internal cycles. */
int check_mode = -1;

unsigned cyc_finish(void)
{
    return cyc_impl_finish();
}
