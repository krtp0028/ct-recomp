/* PSP frontend: the game in ms0:/PSP/GAME/CTRECOMP/game.sfc from reset,
 * under the frame scheduler. 480x272 display (4:3-ish 363-wide scaled
 * image, nearest), 32040 Hz stereo through the hardware SRC channel, PSP
 * pad mapped to SNES buttons. HOME exits. The progress log goes to
 * ms0:/PSP/GAME/CTRECOMP/ct_psp.log.
 *
 * The ROM is not part of this build: copy your own US 1.0 dump (headerless
 * 4 MB) to that path. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspthreadman.h>

#include "bus.h"
#include "interp.h"
#include "sched.h"

PSP_MODULE_INFO("CTRecomp", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-2048);   /* all but 2 MB: the engine mallocs its stacks */

#define LOG_PATH "ms0:/PSP/GAME/CTRECOMP/ct_psp.log"

#define FB_W 512
#define FB_H 272
#define DISP_W 480
#define DISP_H 272
#define SCALE_W 363              /* 4:3 in 272 high, centered */
#define SCALE_X ((DISP_W - SCALE_W) / 2)
#define AUDIO_BLOCK 512          /* SRC block: 512 samples at 32040 Hz */

static const char *rom_paths[] = {
    "ms0:/PSP/GAME/CTRECOMP/game.sfc",
    "ms0:/PSP/GAME/CT/game.sfc",
    "ms0:/game.sfc",
};

static uint32_t __attribute__((aligned(64))) fbmem[2][FB_W * FB_H];
static uint16_t xmap[SCALE_W], ymap[DISP_H];
static int fb_index, audio_ch = -1;
static int16_t audio_block[AUDIO_BLOCK * 2];
static int audio_n;
static volatile int running = 1;
static FILE *log_file;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, 0);
}

static void logf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_file) {
        fputs(buf, log_file);
        fflush(log_file);
    }
}

/* ct_fatal exits after this hook: log the message, paint the screen red,
   and stay put. */
static void on_fatal(const char *msg)
{
    logf("FATAL: %s\n", msg);
    uint32_t *dst = fbmem[fb_index];
    for (int k = 0; k < FB_W * FB_H; k++)
        dst[k] = 0xFF0000FFu;
    sceDisplaySetFrameBuf(dst, FB_W, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
    for (;;)
        sceKernelDelayThread(1000000);
}

static void present(void)
{
    /* A flip handed to the display lands at the next vblank (59.94 Hz: a
       16.683 ms field). Before drawing into the buffer it will retire, wait
       when the last flip may still be pending; at a frame time of a field or
       more it has certainly landed, and waiting would only burn emulation
       time (the engine is far slower than the display). The first call has
       no flip outstanding: it draws into the buffer that was never shown. */
    enum { FIELD_US = 16684 };   /* one 59.94 Hz field, rounded up */
    static uint64_t last_flip;
    uint64_t now = sceKernelGetSystemTimeWide();
    if (last_flip && now - last_flip < FIELD_US)
        sceDisplayWaitVblankStart();
    const uint8_t *fb = sched_frame();
    uint32_t *dst = fbmem[fb_index];
    for (int y = 0; y < DISP_H; y++) {
        const uint8_t *row = fb + (size_t)ymap[y] * SCHED_WIDTH * 4;
        uint32_t *out = dst + (size_t)y * FB_W + SCALE_X;
        for (int x = 0; x < SCALE_W; x++) {
            const uint8_t *s = row + (size_t)xmap[x] * 4;
            /* PSP 8888 display is A B G R in memory: red in the low byte. */
            *out++ = 0xFF000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
        }
    }
    sceDisplaySetFrameBuf(dst, FB_W, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    fb_index ^= 1;
    last_flip = sceKernelGetSystemTimeWide();
}

static void queue_audio(void)
{
    static int16_t in[4096 * 2];
    int n = sched_audio_take(in, 4096);
    for (int k = 0; k < n;) {
        int take = AUDIO_BLOCK - audio_n;
        if (take > n - k)
            take = n - k;
        memcpy(&audio_block[audio_n * 2], &in[k * 2], (size_t)take * 4);
        audio_n += take;
        k += take;
        if (audio_n == AUDIO_BLOCK) {
            if (audio_ch >= 0)
                sceAudioSRCOutputBlocking(audio_ch, audio_block);
            audio_n = 0;
        }
    }
}

static uint16_t pad_buttons(void)
{
    static const struct {
        unsigned psp, snes;
    } map[] = {
        { PSP_CTRL_UP, 1u << 11 },     { PSP_CTRL_DOWN, 1u << 10 },
        { PSP_CTRL_LEFT, 1u << 9 },    { PSP_CTRL_RIGHT, 1u << 8 },
        { PSP_CTRL_CROSS, 1u << 7 },   /* A */
        { PSP_CTRL_CIRCLE, 1u << 15 }, /* B */
        { PSP_CTRL_SQUARE, 1u << 14 }, /* Y */
        { PSP_CTRL_TRIANGLE, 1u << 6 },/* X */
        { PSP_CTRL_LTRIGGER, 1u << 5 },
        { PSP_CTRL_RTRIGGER, 1u << 4 },
        { PSP_CTRL_START, 1u << 12 },
        { PSP_CTRL_SELECT, 1u << 13 },
    };
    static SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);
    uint16_t b = 0;
    for (unsigned k = 0; k < sizeof map / sizeof map[0]; k++)
        if (pad.Buttons & map[k].psp)
            b |= (uint16_t)map[k].snes;
    return b;
}

/* Runs at every frame edge, inside the frame; present, play audio, then
   read input for the next frame. */
static uint64_t f0_us;

static void on_frame(long f)
{
    present();
    queue_audio();
    sched_set_joypad(0, pad_buttons());
    if (f % 600 == 0) {
        static uint64_t last_ins;
        uint64_t us = sceKernelGetSystemTimeWide() - f0_us;
        uint64_t ins = sched_native_insns() + sched_interp_insns();
        logf("frame %ld  NMI %ld  insns/frame %llu  wall %.2fs (%.1f fps)\n", f, sched_nmi_count(),
             (ins - last_ins) / 600, us / 1e6, us ? (double)f * 1e6 / (double)us : 0.0);
        last_ins = ins;
    }
}

int main(void)
{
    pspDebugScreenInit();
    log_file = fopen(LOG_PATH, "w");
    ct_fatal_hook = on_fatal;
    setup_callbacks();

    const char *rom = NULL;
    for (unsigned k = 0; k < sizeof rom_paths / sizeof rom_paths[0]; k++) {
        FILE *f = fopen(rom_paths[k], "rb");
        if (f) {
            fclose(f);
            rom = rom_paths[k];
            break;
        }
    }
    if (!rom) {
        logf("no ROM: copy your US 1.0 dump to %s\n", rom_paths[0]);
        pspDebugScreenPrintf("no ROM: copy your US 1.0 dump to\n  %s\n", rom_paths[0]);
        for (;;)
            sceKernelDelayThread(1000000);
    }
    logf("ROM: %s\n", rom);

    for (int x = 0; x < SCALE_W; x++)
        xmap[x] = (uint16_t)((unsigned)x * SCHED_WIDTH / SCALE_W);
    for (int y = 0; y < DISP_H; y++)
        ymap[y] = (uint16_t)((unsigned)y * SCHED_HEIGHT / DISP_H);

    sceDisplaySetMode(0, DISP_W, DISP_H);
    sceDisplaySetFrameBuf(fbmem[0], FB_W, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_IMMEDIATE);
    fb_index = 1;

    sceCtrlSetSamplingCycle(0);
    /* The DSP runs at 32040 Hz; 32000 is the closest rate the SRC channel
       accepts (0.125% flat pitch, inaudible). */
    audio_ch = sceAudioSRCChReserve(AUDIO_BLOCK, 32000, 2);
    logf("audio ch: %d\n", audio_ch);

    static CPU cpu;
    bus_init(rom);
    char title[22];
    memcpy(title, bus_rom() + 0xFFC0, 21);
    title[21] = 0;
    logf("title: %s\n", title);

    interp_reset(&cpu);
    sched_init(&cpu);
    sched_set_frame_hook(on_frame);
    logf("sched init ok, running\n");
    f0_us = sceKernelGetSystemTimeWide();

    while (running)
        sched_run_frame();

    logf("exit at frame %ld: PC $%02X%04X\n", sched_frame_count(), cpu.PB, cpu.PC);
    if (audio_ch >= 0)
        sceAudioSRCChRelease();
    if (log_file)
        fclose(log_file);
    return 0;
}
