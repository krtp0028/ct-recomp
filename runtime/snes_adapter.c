#include <string.h>
#include "snes_adapter.h"

#include "bus.h"
#include "cpu.h"
#include "cycles.h"
#include "sched.h"
#include "spc700_host.h"
#include "hwlog.h"
#include "apu.h"
#include "dma.h"
#include "ppu.h"
#include "snes.h"

static Ppu *g_ppu;
static Dma *g_dma;
static Apu *g_apu;
static Snes g_snes_stub;

/* ---- Snes* stand-in dma.c expects (see third_party/snes/snes.h) ---- */

uint8_t snes_read(Snes *snes, uint32_t adr)
{
    (void)snes;
    return read8(adr);
}

void snes_write(Snes *snes, uint32_t adr, uint8_t val)
{
    (void)snes;
    write8(adr, val);
}

uint8_t snes_readBBus(Snes *snes, uint8_t adr)
{
    (void)snes;
    return read8(0x2100u + adr);
}

void snes_writeBBus(Snes *snes, uint8_t adr, uint8_t val)
{
    (void)snes;
    write8(0x2100u + adr, val);
}

/* ---- $2100-$213F PPU ---- */

/* Read side of the PPU, per bsnes sfc/ppu/io.cpp readIO and fullsnes:
 *   $2104-$2106 $2108-$210A $2114-$2116 $2118-$211A $2124-$2126
 *   $2128-$212A    write-only: PPU1 open bus (last value read from PPU1)
 *   other $2100-$2133  write-only: CPU open bus
 *   $2134-$2136    mode-7 product (vendored ppu_read)
 *   $2138          OAM data, byte address auto-increments; $200-$3FF
 *                  mirror the 32-byte high table
 *   $2139/$213A    VRAM data from a 16-bit prefetch latch: the latch loads
 *                  after a VMADD write and BEFORE the address increments
 *                  on a read, so the first word after VMADD comes twice
 *   $213B          CGRAM data, shares the write flip-flop; the high byte's
 *                  bit 7 is PPU2 open bus
 *   $213E          STAT77: PPU1 version 1, bit 4 PPU1 open bus; the OBJ
 *                  time/range overflow flags are not modeled (noted)
 *   $2137 $213C $213D $213F  counters, the frame scheduler's
 * $2134-$213F are read-only: writes are ignored and noted. VRAM address
 * remapping (VMAIN bits 2-3) is not modeled on reads, as on writes. */

static uint8_t ppu1_mdr, ppu2_mdr;
static uint16_t vram_latch;

uint8_t snes_ppu2_mdr(void) { return ppu2_mdr; }
void snes_set_ppu2_mdr(uint8_t v) { ppu2_mdr = v; }

static uint8_t oam_byte(unsigned b)
{
    unsigned w = b < 0x200 ? b >> 1 : 0x100 + ((b & 0x1F) >> 1);
    return (uint8_t)(g_ppu->oam[w] >> ((b & 1) * 8));
}

static void vram_prefetch_increment(void)
{
    vram_latch = g_ppu->vram[g_ppu->vramPointer & 0x7FFF];
    g_ppu->vramPointer += g_ppu->vramIncrement;
}

static uint8_t ppu_reg_read(uint16_t reg)
{
    switch (reg) {
    case 0x2104: case 0x2105: case 0x2106: case 0x2108: case 0x2109: case 0x210A:
    case 0x2114: case 0x2115: case 0x2116: case 0x2118: case 0x2119: case 0x211A:
    case 0x2124: case 0x2125: case 0x2126: case 0x2128: case 0x2129: case 0x212A:
        return ppu1_mdr;
    case 0x2134: case 0x2135: case 0x2136:
        return ppu1_mdr = ppu_read(g_ppu, (uint8_t)(reg - 0x2100u));
    case 0x2138: {
        unsigned b = (unsigned)(g_ppu->oamAdr << 1 | g_ppu->oamSecondWrite) & 0x3FF;
        ppu1_mdr = oam_byte(b);
        b = (b + 1) & 0x3FF;
        g_ppu->oamAdr = (uint16_t)(b >> 1);
        g_ppu->oamSecondWrite = b & 1;
        return ppu1_mdr;
    }
    case 0x2139:
        ppu1_mdr = (uint8_t)vram_latch;
        if (!g_ppu->vramIncrementOnHigh)
            vram_prefetch_increment();
        return ppu1_mdr;
    case 0x213A:
        ppu1_mdr = (uint8_t)(vram_latch >> 8);
        if (g_ppu->vramIncrementOnHigh)
            vram_prefetch_increment();
        return ppu1_mdr;
    case 0x213B: {
        uint16_t c = g_ppu->cgram[g_ppu->cgramPointer];
        if (!g_ppu->cgramSecondWrite) {
            ppu2_mdr = (uint8_t)c;
        } else {
            ppu2_mdr = (uint8_t)((ppu2_mdr & 0x80) | ((c >> 8) & 0x7F));
            g_ppu->cgramPointer++;
        }
        g_ppu->cgramSecondWrite = !g_ppu->cgramSecondWrite;
        return ppu2_mdr;
    }
    case 0x213E:
        hw_note("$213E STAT77: OBJ time/range overflow flags not modeled (read as 0)");
        return ppu1_mdr = (uint8_t)((ppu1_mdr & 0x10) | 0x01);
    case 0x2137: case 0x213C: case 0x213D: case 0x213F:
        ct_fatal("read8 $%04X: H/V counters need the frame scheduler", reg);
    default:
        return bus_open_bus(reg);   /* other write-only PPU registers */
    }
}

/* $2104 OAMDATA (replaces the vendored write, which buffered every pair
   and stopped at word $110): byte address = OAMADD*2 + flip-flop. The low
   table ($000-$1FF) latches the even byte and writes the pair on the odd
   one; the high table ($200-$21F, mirrored through $3FF) is written a
   byte at a time. The address wraps at $3FF. */
/* OAMADD ($2102/$2103): a 9-bit reload value (word address) apart from the
   internal address. A write to either half changes that half of the reload
   value and copies the whole value to the address (fullsnes 2102h/2103h;
   bsnes oamBaseAddress/oamAddressReset); VBlank reloads it too. */
static uint16_t oamadd_reload;

static void oam_write(uint8_t v)
{
    unsigned b = (unsigned)(g_ppu->oamAdr << 1 | g_ppu->oamSecondWrite) & 0x3FF;
    if (b < 0x200) {
        if (!(b & 1))
            g_ppu->oamBuffer = v;
        else {
            g_ppu->oam[b >> 1] = (uint16_t)(v << 8 | g_ppu->oamBuffer);
            g_ppu->oamGen++;   /* the sprite Y buckets must refresh */
        }
    } else {
        uint16_t *w = &g_ppu->oam[0x100 + ((b & 0x1F) >> 1)];
        *w = b & 1 ? (uint16_t)((*w & 0x00FF) | v << 8) : (uint16_t)((*w & 0xFF00) | v);
        g_ppu->oamGen++;
    }
    b = (b + 1) & 0x3FF;
    g_ppu->oamAdr = (uint16_t)(b >> 1);
    g_ppu->oamSecondWrite = b & 1;
}

void snes_oam_vblank_reload(void)
{
    if (g_ppu->forcedBlank)
        return;
    g_ppu->oamAdr = oamadd_reload;
    g_ppu->oamSecondWrite = false;
}

/* A write lands partway through the line being drawn: the line is drawn up
 * to that point first (Mesen 2 SnesPpu::RenderScanline: through pixel
 * dot - 22 when dot > 22, with dot = the access's H clock / 4). */
int (*snes_ppu_dot)(void);
void (*snes_ppu_write_hook)(uint16_t reg, uint8_t v);

static void ppu_reg_write(uint16_t reg, uint8_t v)
{
    if (snes_ppu_write_hook)
        snes_ppu_write_hook(reg, v);
    if (reg >= 0x2134) {
        bus_readonly_write(reg, v);
        return;
    }
    if (snes_ppu_dot && g_ppu->drawX < 256) {
        int dot = snes_ppu_dot();
        if (dot > 22)
            ppu_drawTo(g_ppu, dot - 21);
    }
    if (reg == 0x2104) {
        oam_write(v);
        return;
    }
    if (reg == 0x2102 || reg == 0x2103) {
        ppu_write(g_ppu, (uint8_t)(reg - 0x2100u), v);   /* priority bit: not modeled */
        oamadd_reload = reg == 0x2102 ? (uint16_t)((oamadd_reload & 0x100) | v)
                                      : (uint16_t)((oamadd_reload & 0xFF) | (v & 1) << 8);
        g_ppu->oamAdr = oamadd_reload;   /* not the old address's other half */
        g_ppu->oamSecondWrite = false;
        return;
    }
    ppu_write(g_ppu, (uint8_t)(reg - 0x2100u), v);
    if (reg == 0x2116 || reg == 0x2117)
        vram_latch = g_ppu->vram[g_ppu->vramPointer & 0x7FFF];   /* prefetch after VMADD */
}

/* ---- $2140-$2143 APU communication ports, mirrored through $217F ----
 * CPU side: writes land in the SPC700's input ports (what it reads at
 * $F4-$F7), reads return its output ports (what it wrote there).
 * apu_cpuRead/apu_cpuWrite are the SPC700's own memory map, not this.
 *
 * CPU/SPC700 timing follows Mesen 2, the reference the boot is compared
 * against (#33); bsnes/ares resolve a handshake within a cycle
 * differently. SPC700 time is counted in half-cycles of its 2.05 MHz
 * input clock (2 per SPC700 cycle), at master * 32040 * 64 / 21477270
 * (Mesen's NTSC master rate):
 * - Before any CPU port access the SPC700 runs whole cycles while its
 *   count is below that value (truncated) minus 1.
 * - A CPU write that changes a port lands in the SPC700's latch at once
 *   if the SPC700 is within one half-cycle of the CPU's time, otherwise
 *   after the SPC700's next cycle (all four latches together).
 * - SPC700 accesses land at the end of their cycle (spc700_host.cpp). */

void (*snes_apu_sync)(unsigned early);
uint64_t (*snes_master_clock)(void);
uint64_t (*snes_access_clock)(unsigned early);
unsigned snes_ref_quirks;

#define SPC_HALF_PER_MASTER (32040.0 * 64 / 21477270.0)

static uint8_t port_new[4];   /* last CPU write per port */
static uint8_t port_vis[4];   /* what the SPC700 reads at $F4-$F7 */
static int port_pending;

void snes_apu_catch_up(uint64_t master)
{
    int64_t target = (int64_t)((double)master * SPC_HALF_PER_MASTER) - 1;
    int64_t half = 2 * (int64_t)spc_host_cycle();
    if (half < target)
        spc_host_run((uint32_t)((target - half + 1) / 2));
}

static uint8_t apu_reg_read(uint16_t reg)
{
    if (snes_apu_sync)
        snes_apu_sync(4);
    return g_apu->outPorts[reg & 3];
}

static void apu_reg_write(uint16_t reg, uint8_t v)
{
    if (snes_apu_sync)
        snes_apu_sync(0);
    int p = reg & 3;
    if (port_new[p] == v)
        return;
    port_new[p] = v;
    g_apu->inPorts[p] = v;
    uint64_t master = snes_access_clock ? snes_access_clock(0) : cyc_elapsed();
    if ((double)master * SPC_HALF_PER_MASTER - 2.0 * (double)spc_host_cycle() <= 1.0)
        port_vis[p] = v;
    else
        port_pending = 1;
}

static uint8_t inport_read(Apu *apu, int port)
{
    (void)apu;
    return port_vis[port];
}

static void spc_cycle_end(void)
{
    if (port_pending) {
        memcpy(port_vis, port_new, sizeof port_vis);
        port_pending = 0;
    }
}

/* $F1 bits 4 and 5 clear input ports 0-1 and 2-3: the latch and the
   pending value both. */
static void spc_write(uint16_t address, uint8_t data)
{
    if (address != 0xF1)
        return;
    if (data & 0x10)
        port_new[0] = port_new[1] = port_vis[0] = port_vis[1] = 0;
    if (data & 0x20)
        port_new[2] = port_new[3] = port_vis[2] = port_vis[3] = 0;
}

void snes_apu_run(uint32_t spc_cycles)
{
    spc_host_run(spc_cycles);
}

/* ---- $4300-$437F DMA channel registers ---- */

/* $43x0-$43xA per channel; $43xB and $43xF are the same unused R/W byte
   (vendored); $43xC-$43xE are unused: CPU open bus, writes ignored. */
static uint8_t dma_reg_read(uint16_t reg)
{
    if ((reg & 0xF) >= 0xC && (reg & 0xF) <= 0xE)
        return bus_open_bus(reg);
    return dma_read(g_dma, (uint16_t)(reg - 0x4300u));
}

static void dma_reg_write(uint16_t reg, uint8_t v)
{
    if ((reg & 0xF) >= 0xC && (reg & 0xF) <= 0xE) {
        bus_unused_write(reg, v);
        return;
    }
    dma_write(g_dma, (uint16_t)(reg - 0x4300u), v);
}

/* ---- $420B MDMAEN / $420C HDMAEN ----
 * No cycle-driven main loop exists yet (see #11), so general DMA runs to
 * completion synchronously on the MDMAEN write instead of byte-at-a-time
 * over real cycles. HDMAEN only arms hdmaActive; dma_initHdma/dma_doHdma
 * need the scanline loop #11 will add, so HDMA registers hold correct
 * state but do nothing until then. */


/* General DMA started by a $420B write: the transfer runs at once (its
   effects), and the scheduler times the CPU's pause, which starts at a
   later CPU cycle (snes_dma_start). */
void (*snes_dma_start)(const uint32_t sizes[8]);

static void mdmaen_write(uint16_t reg, uint8_t v)
{
    (void)reg;
    uint32_t sizes[8] = {0};
    for (int c = 0; c < 8; c++)
        if (v & (1 << c)) {
            uint32_t size = dma_read(g_dma, (uint16_t)(c * 16 + 5)) |
                            (uint32_t)dma_read(g_dma, (uint16_t)(c * 16 + 6)) << 8;
            sizes[c] = size ? size : 0x10000;
        }
    if (v && snes_dma_start)
        snes_dma_start(sizes);
    unsigned clocks = ct_bus_clocks, n = ct_bus_n;   /* DMA's accesses aren't the CPU's */
    dma_startDma(g_dma, v, false);
    while (g_dma->dmaBusy)
        dma_doDma(g_dma);
    ct_bus_clocks = clocks;
    ct_bus_n = n;
}

/* ---- HDMA CPU time (Mesen 2 SnesDmaController, fullsnes) ----
   Clocks after the 8-clock sync and before the wait back to the CPU:
   8 overhead, then per enabled channel not finished for the frame: its
   transfer bytes (8 each) when it transfers this line, 8 to read the next
   line counter, and when the counter runs out, 16 more to load an
   indirect address (8 if the new counter is 0 and no later channel is
   still active: only the high byte is read). */
static const unsigned hdma_len[8] = {1, 2, 2, 4, 4, 4, 2, 4};

int snes_hdma_enabled(void)
{
    for (int c = 0; c < 8; c++)
        if (g_dma->channel[c].hdmaActive)
            return 1;
    return 0;
}

unsigned snes_hdma_cost(void)
{
    unsigned n = 8;
    for (int c = 0; c < 8; c++) {
        const DmaChannel *ch = &g_dma->channel[c];
        if (ch->hdmaActive && !ch->terminated && ch->doTransfer)
            n += 8 * hdma_len[ch->mode & 7];
    }
    for (int c = 0; c < 8; c++) {
        const DmaChannel *ch = &g_dma->channel[c];
        if (!ch->hdmaActive || ch->terminated)
            continue;
        n += 8;
        if (((ch->repCount - 1) & 0x7F) != 0 || !ch->indirect)
            continue;
        uint16_t at = (uint16_t)(ch->tableAdr + (ch->doTransfer && !ch->indirect ? hdma_len[ch->mode & 7] : 0));
        uint8_t next = bus_peek((uint32_t)ch->aBank << 16 | at);
        int last = 1;
        for (int d = c + 1; d < 8; d++)
            if (g_dma->channel[d].hdmaActive && !g_dma->channel[d].terminated)
                last = 0;
        n += next == 0 && last ? 8 : 16;
    }
    return n;
}

/* HDMA init at the start of the frame: 8 overhead, then per enabled
   channel 8 for its first line counter and, if indirect, 16 for the
   address (8 if that counter is 0). The counter is read from the table
   itself, so this holds whether or not the vendored dma_initHdma (run at
   line 0's start) has run yet. */
unsigned snes_hdma_init_cost(void)
{
    unsigned n = 8;
    for (int c = 0; c < 8; c++) {
        const DmaChannel *ch = &g_dma->channel[c];
        if (!ch->hdmaActive)
            continue;
        n += 8;
        if (ch->indirect)
            n += bus_peek((uint32_t)ch->aBank << 16 | ch->aAdr) == 0 ? 8 : 16;
    }
    return n;
}

static void hdmaen_write(uint16_t reg, uint8_t v)
{
    (void)reg;
    dma_startDma(g_dma, v, true);
}

void snes_hw_init(void)
{
    if (!g_ppu) {
        g_ppu = ppu_init();
        g_apu = apu_init();
        g_dma = dma_init(&g_snes_stub);
    }
    apu_inport_read = inport_read;
    spc_host_cycle_end = spc_cycle_end;
    spc_host_write_hook = spc_write;

    for (uint16_t r = 0x2100; r <= 0x213F; r++)
        bus_hook(r, ppu_reg_read, ppu_reg_write);
    for (uint16_t r = 0x2140; r <= 0x217F; r++)   /* $2144-$217F mirror $2140-$2143 */
        bus_hook(r, apu_reg_read, apu_reg_write);
    for (uint16_t r = 0x4300; r <= 0x437F; r++)
        bus_hook(r, dma_reg_read, dma_reg_write);
    bus_hook(0x420B, bus_open_bus, mdmaen_write);   /* write-only: open bus */
    bus_hook(0x420C, bus_open_bus, hdmaen_write);
}

void snes_hw_reset(void)
{
    if (!g_ppu)
        return;
    ppu_reset(g_ppu);
    ppu1_mdr = ppu2_mdr = 0;
    vram_latch = 0;
    oamadd_reload = 0;
    memset(port_new, 0, sizeof port_new);
    memset(port_vis, 0, sizeof port_vis);
    port_pending = 0;
    dma_reset(g_dma);
    apu_reset(g_apu);
    spc_host_reset(g_apu);
}

/* The reset button (Mesen 2 SnesConsole::Reset): the PPU is forced blank
 * and keeps everything else; DMA/HDMA stop (channel registers kept); the
 * APU ports clear and the SPC700 restarts; ARAM, the DSP (registers and
 * voices: Mesen 2 keeps FLG as it was, where hardware sets it to $E0), the
 * DSP address and the timers (divider phase, enables, targets) are kept,
 * but the timers' outputs clear (Mesen 2 SpcTimer::Reset). */
void snes_hw_soft_reset(void)
{
    if (!g_ppu)
        return;
    g_ppu->forcedBlank = true;
    g_ppu->drawX = 256;
    for (int c = 0; c < 8; c++) {
        g_dma->channel[c].dmaActive = false;
        g_dma->channel[c].hdmaActive = false;
    }
    g_dma->dmaBusy = false;
    memset(port_new, 0, sizeof port_new);
    memset(port_vis, 0, sizeof port_vis);
    port_pending = 0;
    static uint8_t aram[sizeof g_apu->ram];
    static Dsp dsp;
    memcpy(aram, g_apu->ram, sizeof aram);
    dsp = *g_apu->dsp;
    uint8_t dsp_adr = g_apu->dspAdr;
    Timer timer[3];
    memcpy(timer, g_apu->timer, sizeof timer);
    uint64_t cycles = g_apu->cycles;
    apu_reset(g_apu);
    memcpy(g_apu->ram, aram, sizeof aram);
    *g_apu->dsp = dsp;
    g_apu->dspAdr = dsp_adr;
    memcpy(g_apu->timer, timer, sizeof timer);
    g_apu->cycles = cycles;   /* the timers' divider phase */
    for (int t = 0; t < 3; t++)
        g_apu->timer[t].counter = 0;
    spc_host_reset(g_apu);
}

Ppu *snes_hw_ppu(void) { return g_ppu; }
Dma *snes_hw_dma(void) { return g_dma; }
Apu *snes_hw_apu(void) { return g_apu; }
