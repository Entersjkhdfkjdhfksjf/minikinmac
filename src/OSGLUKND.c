/*
 * OSGLUKND.c
 * Bare-metal Platform Abstraction Layer for Kindle PW3 (i.MX6)
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
#include <stdlib.h>
#include <string.h>

#define KINDLE_WIDTH  1440
#define KINDLE_HEIGHT 1056
#define KINDLE_BPP    1

static int fbfd = -1;
static int touch_fd = -1;
static uint8_t *fb_mem = NULL;
static size_t fb_size = 0;
static FBInkConfig fb_cfg = {0};

static int touch_x_min = 0, touch_x_max = 1448;
static int touch_y_min = 0, touch_y_max = 1072;
static int current_touch_x = 0;
static int current_touch_y = 0;
static bool mac_has_booted = false;

// ---------------------------------------------------------
// FUNCTION PROTOTYPES
// ---------------------------------------------------------
void Kindle_Init(void);
void Kindle_UpdateScreenRect(int x, int y, int width, int height);
void Kindle_PollInput(void);
void Kindle_CleanUp(void);

// Stub Prototypes
void MySound_BeginWrite(void);
void MySound_EndWrite(void);
void WaitForNextTick(void);
void MyMoveBytes(void *src, void *dst, int len);
void CheckPbuf(void);
void HTCEexport(void);
void HTCEimport(void);
void PbufTransfer(void);
void *PbufNew(int s);
void PbufDispose(void *p);
int PbufGetSize(void *p);
int vSonyGetSize(int d);
void WarnMsgUnsupportedDisk(void);
void vSonyEject(int d);
void vSonyTransfer(int d, int op, int track, void *buf);
void DiskRevokeWritable(int d);
void vSonyEjectDelete(int d);
char* vSonyGetName(int d);
void Screen_OutputFrame(void);
void *ReserveAllocOneBlock(int s);

// Mini vMac Core Entry & Raw Video RAM
extern void ProgramMain(void);
extern uint8_t *VidMem;

// Temporary Mouse Stubs
uint16_t ReadMacMemoryShort(uint32_t address) { return 0; }
void InjectMacMouseDelta(int dx, int dy) {}
void InjectMacMouseButton(bool is_down) {}

// ---------------------------------------------------------
// 1. HARDWARE INITIALIZATION
// ---------------------------------------------------------
void Kindle_Init(void) {
    fbfd = fbink_open();
    if (fbfd < 0) return;
    
    fbink_init(fbfd, &fb_cfg);
    fb_cfg.is_flashing = false;
    fb_cfg.wfm_mode = WFM_A2; 

    fb_size = KINDLE_WIDTH * KINDLE_HEIGHT * KINDLE_BPP;
    fb_mem = (uint8_t*)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);

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
    if (!fb_mem || !VidMem) return;

    int mac_stride = KINDLE_WIDTH / 8; 
    int fb_stride = KINDLE_WIDTH * KINDLE_BPP; 
    bool frame_has_content = false;

    for (int row = y; row < y + height; row++) {
        for (int col = x; col < x + width; col++) {
            int byte_idx = (row * mac_stride) + (col / 8);
            int bit_idx = 7 - (col % 8); 
            uint8_t mac_byte = VidMem[byte_idx];
            
            // Mac RAM 0x00 is pure white. If it draws anything else, it is booting.
            if (mac_byte != 0x00) {
                frame_has_content = true;
            }

            bool is_black = (mac_byte >> bit_idx) & 1; 
            int fb_idx = (row * fb_stride) + col;
            fb_mem[fb_idx] = is_black ? 0x00 : 0xFF;
        }
    }

    // Only force a physical e-ink refresh if the Mac has drawn the desktop or floppy
    if (frame_has_content) {
        mac_has_booted = true;
        fbink_refresh(fbfd, x, y, width, height, &fb_cfg);
    }
}

// ---------------------------------------------------------
// 3. INPUT POLLING
// ---------------------------------------------------------
void Kindle_PollInput(void) {
    if (touch_fd < 0) return;
    struct input_event ev;
    bool touch_moved = false;

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
        InjectMacMouseDelta(current_touch_x - current_mac_x, current_touch_y - current_mac_y);
    }
}

void Kindle_CleanUp(void) {
    if (touch_fd >= 0) close(touch_fd);
    if (fbfd >= 0) {
        if (fb_mem) munmap(fb_mem, fb_size);
        fbink_close(fbfd);
    }
}

// =========================================================
// BARE-METAL PLATFORM STUBS
// =========================================================
int QuietTime = 0, QuietSubTicks = 0, SpeedValue = 1, ExtraTimeNotOver = 1, WantNotAutoSlow = 0;
int DoneWithDrawingForTick = 0, OnTrueTime = 1, EmLagTime = 0;
unsigned int CurMacDateInSeconds = 0;
int CurMacLatitude = 0, CurMacLongitude = 0, CurMacDelta = 0;
int WantMacReset = 0, WantMacInterrupt = 0, ForceMacOff = 0;
int CurMouseV = 0, CurMouseH = 0, EmVideoDisable = 0;

void MySound_BeginWrite(void) {}
void MySound_EndWrite(void) {}
int MyEvtQOutP = 0, MyEvtQOutDone = 0;

void WaitForNextTick(void) { 
    // Warp-speed the CPU until the Mac memory test finishes, then cap to 60 FPS
    if (mac_has_booted) {
        usleep(16000); 
    }
    Kindle_PollInput(); 
}

void MyMoveBytes(void *src, void *dst, int len) { memmove(dst, src, len); }
char *ROM = NULL;
void *ReserveAllocOneBlock(int s) { return malloc(s); }

void CheckPbuf(void) {} void HTCEexport(void) {} void HTCEimport(void) {}
void PbufTransfer(void) {} void *PbufNew(int s) { return NULL; }
void PbufDispose(void *p) {} int PbufGetSize(void *p) { return 0; }

// =========================================================
// FLOPPY DISK CONTROLLER (Mounted to disk.img)
// =========================================================
FILE *mac_disk = NULL;
int vSonyInsertedMask = 0, vSonyRawMode = 0, vSonyWritableMask = 0, AnyDiskInserted = 0;
int vSonyNewDiskWanted = 0, vSonyNewDiskSize = 0;
char vSonyNewDiskName[256];

int vSonyGetSize(int d) { 
    if (d == 1 && mac_disk) {
        fseek(mac_disk, 0, SEEK_END);
        return ftell(mac_disk);
    }
    return 0; 
}
void vSonyTransfer(int d, int op, int track, void *buf) {
    if (d == 1 && mac_disk) {
        fseek(mac_disk, track * 512, SEEK_SET);
        if (op == 0) fread(buf, 1, 512, mac_disk);
        else fwrite(buf, 1, 512, mac_disk);
    }
}
void vSonyEject(int d) { if (d == 1) vSonyInsertedMask = 0; }
void WarnMsgUnsupportedDisk(void) {}
void DiskRevokeWritable(int d) {}
void vSonyEjectDelete(int d) {}
char* vSonyGetName(int d) { return "disk.img"; }

void Screen_OutputFrame(void) { 
    Kindle_UpdateScreenRect(0, 0, KINDLE_WIDTH, KINDLE_HEIGHT); 
}

// =========================================================
// MAIN ENTRY POINT
// =========================================================
int main(int argc, char *argv[]) {
    FILE *f = fopen("vMac.ROM", "rb");
    if (!f) return 1; 
    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    ROM = (char*)malloc(rom_size);
    fread(ROM, 1, rom_size, f);
    fclose(f);

    // Mount the OS Disk if available
    mac_disk = fopen("disk.img", "r+b");
    if (mac_disk) {
        vSonyInsertedMask = 1; 
        vSonyWritableMask = 1; 
        AnyDiskInserted = 1;
    }

    Kindle_Init();
    ProgramMain(); // Launch Core
    Kindle_CleanUp();

    if (ROM) free(ROM);
    if (mac_disk) fclose(mac_disk);
    return 0;
}
