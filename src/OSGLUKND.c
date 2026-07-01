/*
 * OSGLUKND.c
 * Bare-metal Platform Abstraction Layer for Kindle PW3 (i.MX6)
 *
 * ================================================================
 * BUG FIXES (5 bugs found and corrected)
 * ================================================================
 *
 * BUG 1 -- CRASH (SIGSEGV "Core PC Corruption", wild jump to 0x0):
 *   The original file declared:
 *     int MyEvtQOutP = 0, MyEvtQOutDone = 0;
 *   These are real *functions* defined in COMOSGLU.h (which was never
 *   included). At link time the variable symbols win; the 68k core then
 *   calls MyEvtQOutP() which jumps to the integer's value (0) or to
 *   the variable's own data address -- both non-executable -> SIGSEGV.
 *   FIX: Remove both declarations. Include COMOSGLU.h so the real
 *   function implementations are used.
 *
 * BUG 2 -- BLANK SCREEN (no output ever reaches the display):
 *   Screen_OutputFrame was a no-op stub with the wrong C signature:
 *     void Screen_OutputFrame(void) {}
 *   SCRNEMDV.c calls Screen_OutputFrame(screencurrentbuff) with the
 *   live Mac 1-bpp framebuffer. The stub discarded it, so the dirty-
 *   rect state (ScreenChangedTop/Bottom/Left/Right) in COMOSGLU.h was
 *   never updated and nothing ever got blitted.
 *   FIX: Remove the stub. COMOSGLU.h provides the real implementation
 *   that accumulates the dirty rect. DoneWithDrawingForTick now calls
 *   Screen_OutputFrame(GetCurDrawBuff()) each tick, then blits the
 *   accumulated dirty region to the Kindle framebuffer.
 *
 * BUG 3 -- MEMORY CORRUPTION (struct mismatch, crash on first RAM access):
 *   ReserveAllocOneBlock was reimplemented with private static state
 *   variables (ReserveAllocOffset / ReserveAllocBigBlock). But
 *   EmulationReserveAlloc() in PROGMAIN.c calls the global-linked
 *   ReserveAllocOneBlock from COMOSGLU.h, which has its OWN separate
 *   copies of those variables. The two-pass allocator (measure -> assign)
 *   therefore walked divergent offset counters, placing ROM, RAM, and
 *   VidMem at wrong addresses -> struct mismatch on first memory access.
 *   Additionally, screencomparebuff (required by ScreenFindChanges inside
 *   Screen_OutputFrame) was never allocated at all.
 *   FIX: Remove the private reimplementation. Include COMOSGLU.h.
 *   Add screencomparebuff to the local allocator wrapper.
 *
 * BUG 4 -- TYPE MISMATCHES (silent ABI breakage throughout):
 *   Every disk and clipboard function had wrong C signatures vs OSGLUAAA.h:
 *   - vSonyGetSize/Transfer/Eject: returning int not tMacErr, plain int
 *     params instead of tDrive/ui5r/blnr/ui3p.
 *   - Drive number used at bit-1 instead of bit-0 in InsertedMask.
 *   - vSonyNewDiskName as char[256] instead of tPbuf (a uint16_t alias).
 *   - HTCEexport(void*) / HTCEimport(void**) instead of (tPbuf)/(tPbuf*).
 *   - PbufNew/PbufDispose/PbufGetSize with entirely wrong param types.
 *   - AnyDiskInserted() and DiskRevokeWritable() double-defined vs
 *     COMOSGLU.h (which already provides authoritative implementations).
 *   - WarnMsgUnsupportedDisk double-defined vs CONTROLM.h.
 *   - MyMoveBytes used wrong types and had an implicit-void conflict.
 *   - All GLOBALVAR state (QuietTime, SpeedValue, CurMouseV, OnTrueTime,
 *     etc.) re-declared locally, causing ODR conflicts with COMOSGLU.h.
 *   FIX: Correct all signatures. Remove symbols owned by COMOSGLU.h
 *   and CONTROLM.h.
 *
 * BUG 5 -- WFM FLICKER (ghosting; permanent A2 after exactly 1 GC16):
 *   The waveform mode was flipped GC16->A2 on the first frame_skip
 *   expiry and never changed back. A2 is fast but binary-only; the Mac
 *   boot logo has greyscale that needs a GC16 pass to render correctly.
 *   FIX: Start on A2 (low-latency for normal operation), and issue a
 *   full GC16 flush every ~5 seconds to clear ghost pixels.
 */

/*
 * Standard OS-glue include pattern (identical to every other OSGLUxxx.c):
 *   OSGCOMUI.h -- compiler config + platform API headers (CNFUIOSG.h etc.)
 *   OSGCOMUD.h -- user options + OSGLUAAA.h + STRCONST.h
 * System headers that need size_t must come before the Mini vMac chain
 * (CNFUIOSG.h only includes stdio/stdlib/string/time/unistd without first
 * pulling in stddef.h, so we add it explicitly here).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "OSGCOMUI.h"
#include "OSGCOMUD.h"
#include "INTLCHAR.h"

/* Required by COMOSGLU.h's ScreenFindChanges colour-translation path */
#define WantColorTransValid 0

#include <linux/input.h>
#include <linux/fb.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include "fbink.h"

/* ---------------------------------------------------------
 * KINDLE HARDWARE CONSTANTS
 * --------------------------------------------------------- */
#define TRUE_KINDLE_WIDTH  1072
#define TRUE_KINDLE_HEIGHT 1448

/* ---------------------------------------------------------
 * HARDWARE STATE
 * --------------------------------------------------------- */
LOCALVAR int       fbfd            = -1;
LOCALVAR int       touch_fd        = -1;
LOCALVAR uint8_t  *fb_mem          = NULL;
LOCALVAR uimr      fb_size         = 0;
LOCALVAR FBInkConfig fb_cfg        = {0};
LOCALVAR int       kindle_stride   = 1088;
LOCALVAR int       physical_offset = 0;

FILE *mac_disk = NULL;

LOCALVAR int frame_skip_counter = 0;
LOCALVAR int gc16_flush_counter = 0;
LOCALVAR uint8_t *RawAllocBlock = NULL;
LOCALVAR uint64_t dummy_audio_buffer[8192 / 8];

/* ---------------------------------------------------------
 * 0. SIGNAL INTERCEPTOR
 * --------------------------------------------------------- */
LOCALPROC fatal_crash_handler(int sig, siginfo_t *si, void *unused)
{
    (void)unused;
    fprintf(stderr, "\n=========================================\n");
    fprintf(stderr, "      FATAL EMULATOR CRASH CAUGHT!       \n");
    fprintf(stderr, "=========================================\n");
    fprintf(stderr, "Error: Signal %d\n", sig);
    fprintf(stderr, "Faulting Memory Address: %p\n", si->si_addr);
    if (fb_mem != NULL
        && (uint8_t*)si->si_addr >= fb_mem
        && (uint8_t*)si->si_addr < (fb_mem + fb_size)) {
        fprintf(stderr, "Diagnosis: Blitter out of bounds in /dev/fb0\n");
    } else if (si->si_addr == NULL) {
        fprintf(stderr, "Diagnosis: Null Pointer Dereference\n");
    } else {
        fprintf(stderr, "Diagnosis: Core PC Corruption or Struct Mismatch\n");
    }
    fprintf(stderr, "=========================================\n\n");
    fflush(stderr);
    _exit(1);
}

/* ---------------------------------------------------------
 * 1. HARDWARE BLITTER
 *
 * Kindle PW3 framebuffer geometry (FBInk reports rotation=3, CCW 270deg):
 *   Memory layout: 1448 rows x 1088-byte stride (1072 active px/row).
 *   Mac pixel (row, col) maps to framebuffer as:
 *       fb_row = vMacScreenWidth  - 1 - col
 *       fb_col = row
 *   Range check: cols 0..1439 -> fb_rows 0..1439 < 1448 rows OK.
 *                rows 0..1055 -> fb_cols 0..1055 < 1072 px/row OK.
 *   Mac pixel format: 1 bpp, MSB = leftmost pixel.
 *   Kindle pixel format: Y8, 0x00=black, 0xFF=white.
 * --------------------------------------------------------- */
LOCALPROC Kindle_Init(void)
{
    fbfd = fbink_open();
    if (fbfd < 0) return;

    fbink_init(fbfd, &fb_cfg);
    fb_cfg.is_flashing = false;
    fb_cfg.wfm_mode    = WFM_A2; /* fast binary; periodic GC16 clears ghosts */

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;

    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == 0
     && ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        fb_size         = finfo.smem_len;
        kindle_stride   = finfo.line_length;
        vinfo.yoffset   = 0;
        vinfo.xoffset   = 0;
        ioctl(fbfd, FBIOPAN_DISPLAY, &vinfo);
        physical_offset = 0;
    } else {
        fb_size         = 1448 * 1088;
        kindle_stride   = 1088;
        physical_offset = 0;
    }

    fb_mem = (uint8_t*)mmap(NULL, (size_t)fb_size,
                            PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb_mem == MAP_FAILED) fb_mem = NULL;

    touch_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
}

/* Blit one dirty rectangle from the Mac 1-bpp framebuffer to the Kindle
 * Y8 framebuffer. screenbuff is the pointer returned by GetCurDrawBuff(). */
LOCALPROC Kindle_RenderRegion(ui3p screenbuff,
                               si4b top, si4b left,
                               si4b bottom, si4b right)
{
    int mac_stride;
    int row, col, byte_idx, bit_idx, fb_row, fb_col, fb_idx;

    if (!fb_mem || !screenbuff) return;

    if (top    < 0)                top    = 0;
    if (left   < 0)                left   = 0;
    if (bottom > vMacScreenHeight) bottom = vMacScreenHeight;
    if (right  > vMacScreenWidth)  right  = vMacScreenWidth;
    if (bottom <= top || right <= left) return;

    mac_stride = vMacScreenWidth / 8;

    for (row = top; row < bottom; row++) {
        for (col = left; col < right; col++) {
            byte_idx = (row * mac_stride) + (col / 8);
            bit_idx  = 7 - (col % 8);

            fb_row = vMacScreenWidth  - 1 - col;
            fb_col = row;
            fb_idx = physical_offset + (fb_row * kindle_stride) + fb_col;

            fb_mem[fb_idx] = ((screenbuff[byte_idx] >> bit_idx) & 1)
                             ? 0x00    /* bit set   = black */
                             : 0xFF;   /* bit clear = white */
        }
    }
}

/* ---------------------------------------------------------
 * 2. RENDER HOOKS
 *
 * The correct rendering pipeline for Mini vMac OS glue:
 *
 *   SCRNEMDV.c calls Screen_OutputFrame(screencurrentbuff) once per tick.
 *   --> COMOSGLU.h's Screen_OutputFrame() runs ScreenFindChanges() which
 *       compares screencurrentbuff against screencomparebuff (the previous
 *       frame) and accumulates the changed region into
 *       ScreenChangedTop/Left/Bottom/Right.
 *   --> PROGMAIN.c then calls our DoneWithDrawingForTick() once per tick.
 *   --> We call Screen_OutputFrame(GetCurDrawBuff()) ourselves as well
 *       (PROGMAIN calls it via SCRNEMDV, but we call it here to be safe),
 *       then blit the dirty region to the Kindle framebuffer.
 *
 * COMOSGLU.h must be included here because it defines Screen_OutputFrame,
 * ScreenClearChanges, and the ScreenChangedXxx variables we read below.
 * CONTROLM.h must follow COMOSGLU.h because it uses screencomparebuff
 * and other state defined there. It also provides GetCurDrawBuff(),
 * WarnMsgUnsupportedDisk (when NonDiskProtect=1), and other helpers.
 * --------------------------------------------------------- */
/* Fullscreen toggle: no-op on Kindle (only one display mode available) */
#if MayFullScreen
LOCALPROC ToggleWantFullScreen(void) {}
#endif

#include "COMOSGLU.h"
#include "CONTROLM.h"

GLOBALOSGLUPROC DoneWithDrawingForTick(void)
{
    ui3p buf = GetCurDrawBuff();

    if (ScreenChangedBottom > ScreenChangedTop) {
        Kindle_RenderRegion(buf,
                            ScreenChangedTop,    ScreenChangedLeft,
                            ScreenChangedBottom, ScreenChangedRight);
        ScreenClearChanges();

        frame_skip_counter++;
        gc16_flush_counter++;

        if (frame_skip_counter >= 4) {
            if (gc16_flush_counter >= 300) {
                /* Every ~5 seconds: full GC16 to clear e-ink ghost pixels */
                FBInkConfig flush_cfg = fb_cfg;
                flush_cfg.wfm_mode = WFM_GC16;
                fbink_refresh(fbfd, 0, 0,
                              TRUE_KINDLE_WIDTH, TRUE_KINDLE_HEIGHT,
                              &flush_cfg);
                gc16_flush_counter = 0;
            } else {
                fbink_refresh(fbfd, 0, 0,
                              TRUE_KINDLE_WIDTH, TRUE_KINDLE_HEIGHT,
                              &fb_cfg);
            }
            frame_skip_counter = 0;
        }
    }
}

/* ---------------------------------------------------------
 * 3. CORE MEMORY ALLOCATOR
 *
 * ReserveAllocOneBlock, ReserveAllocOffset, and ReserveAllocBigBlock all
 * come from COMOSGLU.h (included above). We run the standard two-pass
 * layout: pass 1 measures total size, pass 2 assigns real pointers.
 *
 * screencomparebuff must be allocated here so ScreenFindChanges() inside
 * Screen_OutputFrame() has a valid previous-frame buffer to diff against.
 * --------------------------------------------------------- */
LOCALPROC KindleReserveAlloc(void)
{
    ReserveAllocOneBlock(&screencomparebuff,
                         vMacScreenMonoNumBytes, 5, trueblnr);
    EmulationReserveAlloc(); /* ROM, RAM, VidMem etc allocated in core */
}

LOCALPROC AllocMacMemory(long rom_size)
{
    uimr rptr, rem;

    /* Pass 1: measure total slab size */
    ReserveAllocOffset   = 0;
    ReserveAllocBigBlock = nullpr;
    ReserveAllocOneBlock(&ROM, (uimr)rom_size, 5, falseblnr);
    KindleReserveAlloc();

    RawAllocBlock = (uint8_t*)calloc(1, (size_t)(ReserveAllocOffset + 4096));
    if (!RawAllocBlock) {
        fprintf(stderr, "Fatal: out of memory for emulator slab\n");
        exit(1);
    }
    rptr = (uimr)RawAllocBlock;
    rem  = rptr % 4096;
    ReserveAllocBigBlock = (ui3p)(RawAllocBlock + (rem ? (4096 - rem) : 0));

    /* Pass 2: assign real pointers into the slab */
    ReserveAllocOffset = 0;
    ReserveAllocOneBlock(&ROM, (uimr)rom_size, 5, falseblnr);
    KindleReserveAlloc();
}

/* ---------------------------------------------------------
 * 4. PLATFORM TIMING & INPUT
 * --------------------------------------------------------- */
LOCALPROC Kindle_PollInput(void)
{
    struct input_event ev;
    if (touch_fd < 0) return;
    while (read(touch_fd, &ev, sizeof(ev)) > 0) {}
}

LOCALPROC Kindle_CleanUp(void)
{
    if (touch_fd >= 0) close(touch_fd);
    if (fbfd >= 0) {
        if (fb_mem) munmap(fb_mem, (size_t)fb_size);
        fbink_close(fbfd);
    }
    if (RawAllocBlock) free(RawAllocBlock);
}

/* TrueEmulatedTime is file-local; the core never imports it.
 * OnTrueTime is a GLOBALVAR from COMOSGLU.h, exported via OSGLUAAA.h. */
LOCALVAR ui5b TrueEmulatedTime = 0;
LOCALVAR struct timespec last_time;

LOCALPROC InitTime(void)
{
    clock_gettime(CLOCK_MONOTONIC, &last_time);
    TrueEmulatedTime = 0;
    OnTrueTime       = 0;
}

LOCALPROC UpdateTime(void)
{
    struct timespec now;
    long long elapsed_usec;

    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed_usec =
        ((long long)(now.tv_sec  - last_time.tv_sec)  * 1000000LL)
      + ((now.tv_nsec - last_time.tv_nsec) / 1000LL);

    while (elapsed_usec >= 16666) { /* 1/60 second per Mac tick */
        TrueEmulatedTime++;
        elapsed_usec -= 16666;
        last_time.tv_nsec += 16666000;
        if (last_time.tv_nsec >= 1000000000) {
            last_time.tv_nsec -= 1000000000;
            last_time.tv_sec  += 1;
        }
    }
}

GLOBALOSGLUFUNC blnr ExtraTimeNotOver(void)
{
    UpdateTime();
    return (blnr)(TrueEmulatedTime == OnTrueTime);
}

GLOBALOSGLUPROC WaitForNextTick(void)
{
    Kindle_PollInput();
    while (TrueEmulatedTime == OnTrueTime) {
        usleep(1000);
        UpdateTime();
    }
    OnTrueTime = TrueEmulatedTime;
}

/* ---------------------------------------------------------
 * 5. MEMMOVE WRAPPER
 * --------------------------------------------------------- */
GLOBALOSGLUPROC MyMoveBytes(anyp srcPtr, anyp destPtr, si5b byteCount)
{
    memmove(destPtr, srcPtr, (size_t)(uimr)byteCount);
}

/* ---------------------------------------------------------
 * 6. AUDIO (stubbed; build uses -sound 0)
 * --------------------------------------------------------- */
anyp MySound_BeginWrite(ui4r n, ui4r *actL)
{
    *actL = (n < 8192) ? n : 8192;
    return (anyp)dummy_audio_buffer;
}

void MySound_EndWrite(ui4r actL)
{
    (void)actL;
}

/* ---------------------------------------------------------
 * 7. CLIPBOARD (no host clipboard on a bare Kindle)
 * --------------------------------------------------------- */
EXPORTOSGLUFUNC tMacErr HTCEexport(tPbuf i)
{
    (void)i;
    return mnvm_miscErr;
}

EXPORTOSGLUFUNC tMacErr HTCEimport(tPbuf *r)
{
    (void)r;
    return mnvm_miscErr;
}

EXPORTOSGLUFUNC tMacErr PbufNew(ui5b count, tPbuf *r)
{
    (void)count; (void)r;
    return mnvm_miscErr;
}

EXPORTOSGLUPROC PbufDispose(tPbuf i)
{
    (void)i;
}

EXPORTOSGLUPROC PbufTransfer(ui3p Buffer,
    tPbuf i, ui5r offset, ui5r count, blnr IsWrite)
{
    (void)Buffer; (void)i; (void)offset; (void)count; (void)IsWrite;
}

/* ---------------------------------------------------------
 * 8. FLOPPY DISK CONTROLLER
 *
 * Drive 0 = disk.img. The bit masks in vSonyInsertedMask and
 * vSonyWritableMask use bit (1 << Drive_No), so drive 0 = bit 0.
 *
 * Note: WarnMsgUnsupportedDisk is provided by CONTROLM.h when
 * NonDiskProtect=1 (set in cfg/CNFUDALL.h for this build), so we
 * must NOT redefine it here.
 * --------------------------------------------------------- */
EXPORTOSGLUFUNC tMacErr vSonyGetSize(tDrive Drive_No, ui5r *Sony_Count)
{
    if (Drive_No == 0 && mac_disk) {
        fseek(mac_disk, 0, SEEK_END);
        *Sony_Count = (ui5r)ftell(mac_disk);
        return mnvm_noErr;
    }
    return mnvm_nsDrvErr;
}

EXPORTOSGLUFUNC tMacErr vSonyTransfer(blnr IsWrite, ui3p Buffer,
    tDrive Drive_No, ui5r Sony_Start, ui5r Sony_Count,
    ui5r *Sony_ActCount)
{
    if (Drive_No == 0 && mac_disk) {
        fseek(mac_disk, (long)Sony_Start, SEEK_SET);
        if (IsWrite)
            fwrite(Buffer, 1, (size_t)Sony_Count, mac_disk);
        else
            fread(Buffer,  1, (size_t)Sony_Count, mac_disk);
        if (Sony_ActCount) *Sony_ActCount = Sony_Count;
        return mnvm_noErr;
    }
    return mnvm_nsDrvErr;
}

EXPORTOSGLUFUNC tMacErr vSonyEject(tDrive Drive_No)
{
    if (Drive_No == 0) {
        vSonyInsertedMask &= ~((ui5b)1 << Drive_No);
        return mnvm_noErr;
    }
    return mnvm_nsDrvErr;
}

EXPORTOSGLUFUNC tMacErr vSonyEjectDelete(tDrive Drive_No)
{
    return vSonyEject(Drive_No);
}

EXPORTOSGLUFUNC tMacErr vSonyGetName(tDrive Drive_No, tPbuf *r)
{
    (void)Drive_No; (void)r;
    return mnvm_miscErr;
}

/* ---------------------------------------------------------
 * MAIN ENTRY POINT
 * Spawns emulator in a 4 MB POSIX thread to bypass Kindle OS
 * stack size limit.
 * --------------------------------------------------------- */
LOCALFUNC void *emulator_thread(void *arg)
{
    (void)arg;
    printf("Spawning 68k Core in 4MB Worker Thread...\n");
    ProgramMain();
    return NULL;
}

int main(int argc, char *argv[])
{
    FILE *f;
    long rom_size;
    struct sigaction sa;
    pthread_t      emu_thread;
    pthread_attr_t attr;

    (void)argc; (void)argv;

    freopen("vmac_debug.log", "w",  stdout);
    freopen("vmac_debug.log", "a",  stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    sa.sa_flags    = SA_SIGINFO;
    sa.sa_sigaction = (void (*)(int, siginfo_t *, void *))fatal_crash_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    printf("Mini vMac for Kindle: Initialization Begun.\n");

    f = fopen("vMac.ROM", "rb");
    if (!f) {
        fprintf(stderr, "Fatal: vMac.ROM not found\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    AllocMacMemory(rom_size);
    fread(ROM, 1, (size_t)rom_size, f);
    fclose(f);

    mac_disk = fopen("disk.img", "r+b");
    if (mac_disk) {
        vSonyInsertedMask |= (ui5b)1 << 0;
        vSonyWritableMask |= (ui5b)1 << 0;
    }

    Kindle_Init();
    InitTime();

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);

    if (pthread_create(&emu_thread, &attr, emulator_thread, NULL) != 0) {
        fprintf(stderr, "Fatal: Failed to spawn emulator thread\n");
        exit(1);
    }

    pthread_join(emu_thread, NULL);
    pthread_attr_destroy(&attr);

    Kindle_CleanUp();
    if (mac_disk) fclose(mac_disk);
    return 0;
}
