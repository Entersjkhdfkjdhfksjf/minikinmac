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
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRUE_KINDLE_WIDTH  1072
#define TRUE_KINDLE_HEIGHT 1448
#define TRUE_KINDLE_STRIDE 1088

#define MAC_WIDTH  1440
#define MAC_HEIGHT 1056

static int fbfd = -1;
static int touch_fd = -1;
static uint8_t *fb_mem = NULL;
static size_t fb_size = 0;
static FBInkConfig fb_cfg = {0};

extern void EmulationReserveAlloc(void);
extern void ProgramMain(void);
extern uint8_t *VidMem;

char *ROM = NULL;
FILE *mac_disk = NULL;
static int frame_skip_counter = 0;

// ---------------------------------------------------------
// 1. HARDWARE BLITTER (15 FPS UNCONDITIONAL DECIMATOR)
// ---------------------------------------------------------
void Kindle_Init(void) {
    fbfd = fbink_open();
    if (fbfd < 0) {
        printf("ERROR: Failed to open fbink!\n");
        return;
    }
    fbink_init(fbfd, &fb_cfg);
    fb_cfg.is_flashing = false;
    fb_cfg.wfm_mode = WFM_A2; // Fast 1-bit waveform

    fb_size = TRUE_KINDLE_HEIGHT * TRUE_KINDLE_STRIDE;
    fb_mem = (uint8_t*)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);

    touch_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
}

void Kindle_UpdateScreenRect(int x, int y, int width, int height) {
    if (!fb_mem || !VidMem) return;
    
    int mac_stride = MAC_WIDTH / 8; 

    // Rotate and map to 1088 Stride (Memory-Safe)
    for (int row = y; row < y + height; row++) {
        for (int col = x; col < x + width; col++) {
            
            int byte_idx = (row * mac_stride) + (col / 8);
            int bit_idx = 7 - (col % 8);
            uint8_t mac_byte = VidMem[byte_idx];
            
            int k_x = row;
            int k_y = MAC_WIDTH - 1 - col;
            int fb_idx = (k_y * TRUE_KINDLE_STRIDE) + k_x;
            
            fb_mem[fb_idx] = ((mac_byte >> bit_idx) & 1) ? 0x00 : 0xFF;
        }
    }

    // Unconditionally decimate to 15 FPS so the e-ink controller doesn't choke
    frame_skip_counter++;
    if (frame_skip_counter >= 4) {
        fbink_refresh(fbfd, 0, 0, TRUE_KINDLE_WIDTH, TRUE_KINDLE_HEIGHT, &fb_cfg);
        frame_skip_counter = 0;
    }
}

void Kindle_PollInput(void) {
    if (touch_fd < 0) return;
    struct input_event ev;
    while (read(touch_fd, &ev, sizeof(struct input_event)) > 0) {}
}

void Kindle_CleanUp(void) {
    if (touch_fd >= 0) close(touch_fd);
    if (fbfd >= 0) {
        if (fb_mem) munmap(fb_mem, fb_size);
        fbink_close(fbfd);
    }
}

// ---------------------------------------------------------
// 2. CORE MEMORY ALLOCATOR
// ---------------------------------------------------------
static size_t ReserveAllocOffset = 0;
static uint8_t *ReserveAllocBigBlock = NULL;

void ReserveAllocOneBlock(void **p, size_t s, int align, int clear) {
    size_t alignment = 1 << align;
    size_t remainder = ReserveAllocOffset % alignment;
    if (remainder != 0) ReserveAllocOffset += (alignment - remainder);

    if (ReserveAllocBigBlock != NULL) {
        *p = (void*)(ReserveAllocBigBlock + ReserveAllocOffset);
        if (clear && *p) memset(*p, 0, s);
    }
    ReserveAllocOffset += s;
}

void AllocMacMemory(long rom_size) {
    ReserveAllocOffset = 0;
    ReserveAllocBigBlock = NULL;
    ReserveAllocOneBlock((void**)&ROM, rom_size, 5, 0);
    EmulationReserveAlloc();

    ReserveAllocBigBlock = (uint8_t*)calloc(1, ReserveAllocOffset);
    if (!ReserveAllocBigBlock) exit(1);

    ReserveAllocOffset = 0;
    ReserveAllocOneBlock((void**)&ROM, rom_size, 5, 0);
    EmulationReserveAlloc();
}

// ---------------------------------------------------------
// 3. PLATFORM TIMING & LOGGING (FIXED)
// ---------------------------------------------------------
unsigned int TrueEmulatedTime = 0;
unsigned int OnTrueTime = 1;
struct timeval last_time;

void InitTime(void) {
    gettimeofday(&last_time, NULL);
    TrueEmulatedTime = 0;
    OnTrueTime = 0;
}

void UpdateTime(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    long elapsed_usec = (now.tv_sec - last_time.tv_sec) * 1000000L + (now.tv_usec - last_time.tv_usec);

    if (elapsed_usec >= 16666) { 
        TrueEmulatedTime += (elapsed_usec / 16666);
        last_time.tv_sec = now.tv_sec;
        last_time.tv_usec = now.tv_usec - (elapsed_usec % 16666);
    }
}

int ExtraTimeNotOver(void) {
    UpdateTime();
    return (TrueEmulatedTime == OnTrueTime);
}

void WaitForNextTick(void) {
    Kindle_PollInput();
    
    // THE FIX: Allow natural time to pass so the Mac VIA interrupts don't panic
    while (TrueEmulatedTime == OnTrueTime) {
        usleep(1000); 
        UpdateTime();
    }
    OnTrueTime = TrueEmulatedTime;
}

// ---------------------------------------------------------
// 4. CORE STUBS & VARIABLES
// ---------------------------------------------------------
int QuietTime = 0, QuietSubTicks = 0, SpeedValue = 1, WantNotAutoSlow = 0;
int EmLagTime = 0, ForceMacOff = 0;
unsigned int CurMacDateInSeconds = 3800000000;
int CurMacLatitude = 0, CurMacLongitude = 0, CurMacDelta = 0;
int WantMacReset = 0, WantMacInterrupt = 0;
int CurMouseV = 0, CurMouseH = 0, EmVideoDisable = 0;
int MyEvtQOutP = 0, MyEvtQOutDone = 0;

void DoneWithDrawingForTick(void) { Kindle_UpdateScreenRect(0, 0, MAC_WIDTH, MAC_HEIGHT); }
void Screen_OutputFrame(void) {}
void* MySound_BeginWrite(uint32_t n, uint32_t *actL) { *actL = 0; return NULL; }
void MySound_EndWrite(uint32_t actL) {}
void MyMoveBytes(void *src, void *dst, int len) { memmove(dst, src, len); }
void CheckPbuf(void) {}
int HTCEexport(void *i) { return -1; }
int HTCEimport(void **r) { return -1; }
void PbufTransfer(void) {}
void *PbufNew(int s) { return NULL; }
void PbufDispose(void *p) {}
int PbufGetSize(void *p) { return 0; }

// ---------------------------------------------------------
// 5. FLOPPY DISK CONTROLLER
// ---------------------------------------------------------
int vSonyInsertedMask = 0, vSonyRawMode = 0, vSonyWritableMask = 0;
int vSonyNewDiskWanted = 0, vSonyNewDiskSize = 0;
char vSonyNewDiskName[256];

int AnyDiskInserted(void) { return vSonyInsertedMask != 0; }

int vSonyGetSize(int Drive_No, uint32_t *Sony_Count) {
    if (Drive_No == 1 && mac_disk) {
        fseek(mac_disk, 0, SEEK_END);
        *Sony_Count = (uint32_t)ftell(mac_disk);
        return 0;
    }
    return -1;
}

int vSonyTransfer(int IsWrite, uint8_t *Buffer, int Drive_No, uint32_t Sony_Start, uint32_t Sony_Count, uint32_t *Sony_ActCount) {
    if (Drive_No == 1 && mac_disk) {
        fseek(mac_disk, Sony_Start, SEEK_SET);
        uint32_t bytes = 0;
        if (IsWrite) bytes = fwrite(Buffer, 1, Sony_Count, mac_disk);
        else bytes = fread(Buffer, 1, Sony_Count, mac_disk);
        if (Sony_ActCount) *Sony_ActCount = bytes;
        return 0;
    }
    return -1;
}

int vSonyEject(int Drive_No) {
    if (Drive_No == 1) vSonyInsertedMask = 0;
    return 0;
}
int vSonyEjectDelete(int Drive_No) { return vSonyEject(Drive_No); }
int vSonyGetName(int Drive_No, void *r) { return -1; }
void WarnMsgUnsupportedDisk(void) {}
void DiskRevokeWritable(int d) {}

// ---------------------------------------------------------
// MAIN ENTRY POINT
// ---------------------------------------------------------
int main(int argc, char *argv[]) {
    freopen("vmac_debug.log", "w", stdout);
    freopen("vmac_debug.log", "a", stderr);
    
    FILE *f = fopen("vMac.ROM", "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    AllocMacMemory(rom_size);
    fread(ROM, 1, rom_size, f);
    fclose(f);

    mac_disk = fopen("disk.img", "r+b");
    if (mac_disk) {
        vSonyInsertedMask = 1;
        vSonyWritableMask = 1;
    }

    Kindle_Init();
    InitTime();
    ProgramMain();

    Kindle_CleanUp();
    if (mac_disk) fclose(mac_disk);
    return 0;
}
