/*
 * OSGLUKND.c
 * Bare-metal Platform Abstraction Layer for Kindle PW3 (i.MX6)
 * Handles FBInk A2 Waveform e-ink refresh and absolute evdev touch input.
 */

#include "fbink.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// Kindle PW3 target dimensions
#define KINDLE_WIDTH  1440
#define KINDLE_HEIGHT 1056
#define KINDLE_BPP    1    // 8-bit grayscale on PW3 fb0

// Globals for Hardware Handles
static int fbfd = -1;
static int touch_fd = -1;
static uint8_t *fb_mem = NULL;
static size_t fb_size = 0;
static FBInkConfig fb_cfg = {0};

// Touchscreen Coordinate Scaling
static int touch_x_min = 0, touch_x_max = 1448;
static int touch_y_min = 0, touch_y_max = 1072;
static int current_touch_x = 0;
static int current_touch_y = 0;

// ---------------------------------------------------------
// FUNCTION PROTOTYPES (Satisfies -Wmissing-prototypes)
// ---------------------------------------------------------
void Kindle_Init(void);
void Kindle_UpdateScreenRect(int x, int y, int width, int height);
void Kindle_PollInput(void);
void Kindle_CleanUp(void);

/* * External hooks into Mini vMac's core 
 */
extern uint8_t* GetMacVRAMPointer(void); 
extern uint16_t ReadMacMemoryShort(uint32_t address);
extern void InjectMacMouseDelta(int dx, int dy);
extern void InjectMacMouseButton(bool is_down);

// ---------------------------------------------------------
// 1. HARDWARE INITIALIZATION
// ---------------------------------------------------------
void Kindle_Init(void) {
    fbfd = fbink_open();
    if (fbfd < 0) {
        fprintf(stderr, "Failed to open framebuffer.\n");
        return;
    }
    fbink_init(fbfd, &fb_cfg);
    
    // Configure for smooth A2 (fast black & white) refresh
    // Removed is_partial (FBInk infers this from the coordinates passed to fbink_refresh)
    fb_cfg.is_flashing = false;
    fb_cfg.wfm_mode = WFM_A2; // Corrected member and macro name

    // Map Kindle framebuffer to memory
    fb_size = KINDLE_WIDTH * KINDLE_HEIGHT * KINDLE_BPP;
    fb_mem = (uint8_t*)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);

    // Initialize Touch Input
    touch_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (touch_fd >= 0) {
        struct input_absinfo abs_x, abs_y;
        if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) >= 0) {
            touch_x_min = abs_x.minimum;
            touch_x_max = abs_x.maximum;
        }
        if (ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) >= 0) {
            touch_y_min = abs_y.minimum;
            touch_y_max = abs_y.maximum;
        }
    }
}

// ---------------------------------------------------------
// 2. VIDEO RENDERING (THE BLITTER)
// ---------------------------------------------------------
void Kindle_UpdateScreenRect(int x, int y, int width, int height) {
    if (!fb_mem) return;

    uint8_t *mac_vram = GetMacVRAMPointer();
    int mac_stride = KINDLE_WIDTH / 8; 
    int fb_stride = KINDLE_WIDTH * KINDLE_BPP; 

    // Translate 1-bit Mac VRAM into 8-bit Kindle Framebuffer memory
    for (int row = y; row < y + height; row++) {
        for (int col = x; col < x + width; col++) {
            
            int byte_idx = (row * mac_stride) + (col / 8);
            int bit_idx = 7 - (col % 8); 
            uint8_t mac_byte = mac_vram[byte_idx];
            bool is_black = (mac_byte >> bit_idx) & 1; 
            
            int fb_idx = (row * fb_stride) + col;
            fb_mem[fb_idx] = is_black ? 0x00 : 0xFF;
        }
    }

    // Trigger hardware e-ink refresh for this specific dirty rect
    fbink_refresh(fbfd, x, y, width, height, &fb_cfg);
}

// ---------------------------------------------------------
// 3. INPUT POLLING (THE DELTA HACK)
// ---------------------------------------------------------
void Kindle_PollInput(void) {
    if (touch_fd < 0) return;

    struct input_event ev;
    bool touch_moved = false;

    // Drain the evdev event queue
    while (read(touch_fd, &ev, sizeof(struct input_event)) > 0) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X) {
                float scale_x = (float)(ev.value - touch_x_min) / (touch_x_max - touch_x_min);
                current_touch_x = (int)(scale_x * KINDLE_WIDTH);
                touch_moved = true;
            } 
            else if (ev.code == ABS_MT_POSITION_Y) {
                float scale_y = (float)(ev.value - touch_y_min) / (touch_y_max - touch_y_min);
                current_touch_y = (int)(scale_y * KINDLE_HEIGHT);
                touch_moved = true;
            }
        } 
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            InjectMacMouseButton(ev.value == 1);
        }
    }

    if (touch_moved) {
        int current_mac_y = ReadMacMemoryShort(0x0828);
        int current_mac_x = ReadMacMemoryShort(0x082A);

        int delta_x = current_touch_x - current_mac_x;
        int delta_y = current_touch_y - current_mac_y;

        InjectMacMouseDelta(delta_x, delta_y);
    }
}

// ---------------------------------------------------------
// 4. TEARDOWN
// ---------------------------------------------------------
void Kindle_CleanUp(void) {
    if (touch_fd >= 0) close(touch_fd);
    if (fbfd >= 0) {
        if (fb_mem) munmap(fb_mem, fb_size);
        fbink_close(fbfd);
    }
}
// =========================================================
// BARE-METAL PLATFORM STUBS
// Satisfies linker expectations for the core 68k emulator
// =========================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Core emulator entry point (defined in PROGMAIN.c)
extern void ProgMain(void);

// 1. Timing and State Variables
int QuietTime = 0, QuietSubTicks = 0, SpeedValue = 1, ExtraTimeNotOver = 1, WantNotAutoSlow = 0;
int DoneWithDrawingForTick = 0, OnTrueTime = 1, EmLagTime = 0;
unsigned int CurMacDateInSeconds = 0;
int CurMacLatitude = 0, CurMacLongitude = 0, CurMacDelta = 0;
int WantMacReset = 0, WantMacInterrupt = 0, ForceMacOff = 0;

// 2. Mouse and Video Variables
int CurMouseV = 0, CurMouseH = 0, EmVideoDisable = 0;

// 3. Sound (Disabled for Bare-Metal)
void MySound_BeginWrite(void) {}
void MySound_EndWrite(void) {}

// 4. Event Queue and Timing Loop
int MyEvtQOutP = 0, MyEvtQOutDone = 0;
void WaitForNextTick(void) { 
    usleep(16000);     // Roughly 60Hz timing
    Kindle_PollInput(); // Inject our evdev touch hack here
}

// 5. Memory Management
void MyMoveBytes(void *src, void *dst, int len) { memmove(dst, src, len); }
char *ROM = NULL;

// 6. Host Clipboard Integration (Disabled)
void CheckPbuf(void) {} void HTCEexport(void) {} void HTCEimport(void) {}
void PbufTransfer(void) {} void *PbufNew(int s) { return NULL; }
void PbufDispose(void *p) {} int PbufGetSize(void *p) { return 0; }

// 7. Sony Floppy Disk Controller (Stubbed to appear empty)
int vSonyInsertedMask = 0, vSonyRawMode = 0, vSonyWritableMask = 0, AnyDiskInserted = 0;
int vSonyNewDiskWanted = 0, vSonyNewDiskSize = 0;
char vSonyNewDiskName[256];
int vSonyGetSize(int d) { return 0; }
void WarnMsgUnsupportedDisk(void) {}
void vSonyEject(int d) {}
void vSonyTransfer(int d, int op, int track, void *buf) {}
void DiskRevokeWritable(int d) {}
void vSonyEjectDelete(int d) {}
char* vSonyGetName(int d) { return ""; }

// 8. Video Output Hook
void Screen_OutputFrame(void) { 
    // Called by the core when the Mac VRAM is dirty
    Kindle_UpdateScreenRect(0, 0, KINDLE_WIDTH, KINDLE_HEIGHT); 
}

// =========================================================
// MAIN ENTRY POINT
// =========================================================
int main(int argc, char *argv[]) {
    // 1. Load the ROM file into memory
    // (Expects vMac.ROM to be in the same directory as the executable)
    FILE *f = fopen("vMac.ROM", "rb");
    if (!f) { 
        fprintf(stderr, "FATAL: vMac.ROM not found in current directory.\n"); 
        return 1; 
    }
    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    ROM = (char*)malloc(rom_size);
    fread(ROM, 1, rom_size, f);
    fclose(f);

    // 2. Initialize FBInk and Touch
    Kindle_Init();

    // 3. Boot the Macintosh Emulator
    ProgMain(); 

    // 4. Teardown
    Kindle_CleanUp();
    if (ROM) free(ROM);
    
    return 0;
}

