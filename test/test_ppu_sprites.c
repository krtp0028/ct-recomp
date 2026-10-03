/* ppu_sprite_candidates against a naive 128-sprite scan: for every line the
 * candidate list must be exactly the in-range sprites, in ascending OAM
 * order, for both objSize values, including Y wraparound and the yy=0xF0
 * exclusion. This guards the per-line Y buckets used by the whole-line
 * sprite renderer. */
#include "harness.h"
#include "ppu.h"
#include "snes_adapter.h"

/* kSpriteSizes is file-static in ppu.c; the SNES table is public. */
static const int sizes[8][2] = {
    {8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

static int naive(Ppu *p, int line, uint8_t *out)
{
    int n = 0;
    for (int i = 0; i <= 254; i += 2) {
        int yy = p->oam[i] >> 8;
        if (yy == 0xf0)
            continue;
        int high = p->oam[0x100 + (i >> 4)] >> (i & 15);
        int size = sizes[p->objSize][(high >> 1) & 1];
        if (((line - yy) & 0xff) < size)
            out[n++] = (uint8_t)i;
    }
    return n;
}

int main(void)
{
    th_bus_init();
    Ppu *p = snes_hw_ppu();
    ppu_reset(p);
    for (int trial = 0; trial < 8; trial++) {
        for (int i = 0; i <= 254; i += 2) {
            uint32_t r = rnd32();
            p->oam[i] = (uint16_t)(r & 0xFFFF);
            p->oam[i + 1] = (uint16_t)(rnd32() & 0xFFFF);
            if (trial == 0 && (i & 7) == 0)
                p->oam[i] = (uint16_t)(0xF000 | (r & 0xFF));   /* off */
        }
        for (int os = 0; os < 2; os++) {
            p->objSize = (uint8_t)os;
            p->oamGen++;
            for (int line = 0; line < 256; line++) {
                uint8_t want[128], got[128];
                int nw = naive(p, line, want);
                int ng = ppu_sprite_candidates(p, line, got);
                int ok = nw == ng;
                for (int k = 0; ok && k < nw; k++)
                    ok = want[k] == got[k];
                CHECK(ok, "trial %d objSize %d line %d: %d vs %d candidates, order/content",
                      trial, os, line, nw, ng);
            }
        }
    }

    /* The adapter's $2104 path writes OAM directly (bypassing ppu_write), so
       it must bump the generation itself or the cache stays stale: warm the
       cache with all sprites off, write one through the real bus route, and
       require it to appear. */
    p->objSize = 0;
    for (int i = 0; i <= 254; i += 2)
        p->oam[i] = 0xF000;
    for (int i = 1; i <= 255; i += 2)
        p->oam[i] = 0;
    p->oamGen++;
    uint8_t got[128], want[128];
    ppu_sprite_candidates(p, 10, got);                 /* warm the buckets */
    write8(0x2102, 0x02);                              /* sprite 1 low word = 2*1 */
    write8(0x2103, 0x00);
    write8(0x2104, 0x20);                              /* X */
    write8(0x2104, 0x0A);                              /* Y */
    CHECK(p->oam[2] == 0x0A20, "adapter write landed: oam[2]=%04X", p->oam[2]);
    int ng = ppu_sprite_candidates(p, 10, got);
    int nw = naive(p, 10, want);
    CHECK(ng == nw && ng == 1 && got[0] == 2, "adapter low-table write refreshes line 10 (%d/%d)",
          ng, nw);

    /* High-table (size) writes through the adapter must refresh too: with
       objSize 1 (8x16/8x32), make sprite 0 a tall sprite and require the
       added rows to appear. */
    p->objSize = 1;
    p->oamGen++;
    write8(0x2102, 0x00);                              /* sprite 0, low table */
    write8(0x2103, 0x00);
    write8(0x2104, 0x00);                              /* X */
    write8(0x2104, 0x14);                              /* Y */
    p->oamGen++;                                       /* rebuild before sizing */
    ppu_sprite_candidates(p, 0x14 + 20, got);
    write8(0x2102, 0x00);                              /* high table word $100 */
    write8(0x2103, 0x01);
    write8(0x2104, 0x02);                              /* sprite 0 size bit */
    write8(0x2104, 0x00);
    CHECK(p->oam[0] == 0x1400, "sprite0 low word: %04X", p->oam[0]);
    CHECK(p->oam[0x100] == 0x0002, "high word: %04X", p->oam[0x100]);
    ng = ppu_sprite_candidates(p, 0x14 + 20, got);
    nw = naive(p, 0x14 + 20, want);
    CHECK(ng == nw && ng == 1 && got[0] == 0, "adapter high-table write refreshes size (%d/%d)",
          ng, nw);

    return th_report("ppu_sprites");
}
