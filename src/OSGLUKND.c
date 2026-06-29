/*
 * OSGLUKND.c
 * Bare-metal Platform Abstraction Layer for Kindle PW3 (i.MX6)
 */

#include "fbink.h"
#include <linux/input.h>
#include <linux/fb.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define TRUE_KINDLE_WIDTH  1072
#define TRUE_KINDLE_HEIGHT 1448
#define MAC_WIDTH  1440
#define MAC_HEIGHT 1056

// THE FIX: Forcing 8-byte alignment on ALL globals to prevent Cortex-A9 LDRD/STRD hardware faults
static int fbfd __attribute__((aligned(8))) = -1;
static int touch_fd __attribute__((aligned(8))) = -1;
static uint8_t *fb_mem __attribute__((aligned(8))) = NULL;
static size_t fb_size __attribute__((aligned(8))) = 0;
static FBInkConfig fb_cfg __attribute__((aligned(8))) = {0};

static int kindle_stride __attribute__((aligned(8))) = 1088;
static int physical_offset __attribute__((aligned(8))) = 0;

extern void EmulationReserveAlloc(void);
extern void ProgramMain(void);
extern uint8_t *VidMem;

char *ROM __attribute__((aligned(8))) = NULL;
FILE *mac_disk __attribute__((aligned(8))) = NULL;
static int frame_skip_counter __attribute__((aligned(8))) = 0;
static uint8_t *RawAllocBlock __attribute__((aligned(8))) = NULL;

static uint64_t dummy_audio_buffer[8192 / 8] __attribute__((aligned(8)));
char vSonyNewDiskName[256] __attribute__((aligned(8)));

// ---------------------------------------------------------
// 0. SIGNAL INTERCEPTOR
// ---------------------------------------------------------
void fatal_crash_handler(int sig, siginfo_t *si, void *unused) {
    fprintf(stderr, "\n=========================================\n");
    fprintf(stderr, "      FATAL EMULATOR CRASH CAUGHT!       \n");
    fprintf(stderr, "=========================================\n");
    fprintf(stderr, "Error: Signal %d\n", sig);
    fprintf(stderr, "Faulting Memory Address: %p\n", si->si_addr);
    
    if (fb_mem && (uint8_t*)si->si_addr >= fb_mem && (uint8_t*)si->si_addr < (fb_mem + fb_size)) {
        fprintf(stderr, "Diagnosis: Blitter out of bounds in /dev/fb0\n");
    } else if (VidMem && (uint8_t*)si->si_addr >= VidMem && (uint8_t*)si->si_addr < (VidMem + 190080)) {
        fprintf(stderr, "Diagnosis: Core crashed reading Mac VidMem\n");
    } else if (ROM && (uint8_t*)si->si_addr >= (uint8_t*)ROM && (uint8_t*)si->si_addr < ((uint8_t*)ROM + 131072)) {
        fprintf(stderr, "Diagnosis: Core crashed reading Mac ROM\n");
    } else if (si->si_addr == NULL) {
        fprintf(stderr, "Diagnosis: Null Pointer Dereference\n");
    } else {
        fprintf(stderr, "Diagnosis: Jump Table or LDRD Alignment Crash\n");
    }
    fprintf(stderr, "=========================================\n\n");
    fflush(stderr);
    exit(1);
}

// ---------------------------------------------------------
// 1. HARDWARE BLITTER
// ---------------------------------------------------------
void Kindle_Init(void) {
    fbfd = fbink_open();
    if (fbfd < 0) return;
    
    fbink_init(fbfd, &fb_cfg);
    fb_cfg.is_flashing = false;
    fb_cfg.wfm_mode = WFM_GC16; 

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == 0 && ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        fb_size = finfo.smem_len; 
        kindle_stride = finfo.line_length;
        vinfo.yoffset = 0;
        vinfo.xoffset = 0;
        ioctl(fbfd, FBIOPAN_DISPLAY, &vinfo);
        physical_offset = 0; 
    } else {
        fb_size = 1448 * 1088;
        kindle_stride = 1088;
        physical_offset = 0;
    }

    fb_mem = (uint8_t*)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    touch_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
}

void Kindle_RenderRegion(int top, int left, int bottom, int right) {
    if (!fb_mem || !VidMem) return;
    
    if (top < 0) top = 0;
    if (left < 0) left = 0;
    if (bottom > MAC_HEIGHT) bottom = MAC_HEIGHT;
    if (right > MAC_WIDTH) right = MAC_WIDTH;
    
    int mac_stride = MAC_WIDTH / 8; 

    for (int row = top; row < bottom; row++) {
        for (int col = left; col < right; col++) {
            int byte_idx = (row * mac_stride) + (col / 8);
            int bit_idx = 7 - (col % 8);
            
            int k_x = row;
            int k_y = MAC_WIDTH - 1 - col;
            int fb_idx = physical_offset + (k_y * kindle_stride) + k_x;
            
            fb_mem[fb_idx] = ((VidMem[byte_idx] >> bit_idx) & 1) ? 0x00 : 0xFF;
        }
    }

    frame_skip_counter++;
    if (frame_skip_counter >= 4) {
        fbink_refresh(fbfd, 0, 0, TRUE_KINDLE_WIDTH, TRUE_KINDLE_HEIGHT, &fb_cfg);
        frame_skip_counter = 0;
        if (fb_cfg.wfm_mode == WFM_GC16) fb_cfg.wfm_mode = WFM_A2;
    }
}

// ---------------------------------------------------------
// 2. RENDER HOOKS
// ---------------------------------------------------------
void HaveChangedScreenBuff(uint16_t top, uint16_t left, uint16_t bottom, uint16_t right) {
    Kindle_RenderRegion(top, left, bottom, right);
}
void DoneWithDrawingForTick(void) { 
    Kindle_RenderRegion(0, 0, MAC_HEIGHT, MAC_WIDTH); 
}
void Screen_OutputFrame(void) {}

// ---------------------------------------------------------
// 3. CORE MEMORY ALLOCATOR 
// ---------------------------------------------------------
static size_t ReserveAllocOffset __attribute__((aligned(8))) = 0;
static uint8_t *ReserveAllocBigBlock __attribute__((aligned(8))) = NULL;

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

    RawAllocBlock = (uint8_t*)calloc(1, ReserveAllocOffset + 4096);
    if (!RawAllocBlock) exit(1);

    size_t rptr = (size_t)RawAllocBlock;
    size_t rem = rptr % 4096;
    ReserveAllocBigBlock = RawAllocBlock + (4096 - rem);

    ReserveAllocOffset = 0;
    ReserveAllocOneBlock((void**)&ROM, rom_size, 5, 0);
    EmulationReserveAlloc();
}

// ---------------------------------------------------------
// 4. PLATFORM TIMING & INPUT
// ---------------------------------------------------------
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
    if (RawAllocBlock) free(RawAllocBlock);
}

unsigned int TrueEmulatedTime __attribute__((aligned(8))) = 0;
unsigned int OnTrueTime __attribute__((aligned(8))) = 1;
static struct timespec last_time __attribute__((aligned(8)));

void InitTime(void) {
    clock_gettime(CLOCK_MONOTONIC, &last_time);
    TrueEmulatedTime = 0;
    OnTrueTime = 0;
}

void UpdateTime(void) {
    struct timespec now __attribute__((aligned(8)));
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    long long elapsed_usec = ((long long)(now.tv_sec - last_time.tv_sec) * 1000000LL) + 
                             ((now.tv_nsec - last_time.tv_nsec) / 1000LL);

    while (elapsed_usec >= 16666) { 
        TrueEmulatedTime++;
        elapsed_usec -= 16666;
        
        last_time.tv_nsec += 16666000;
        if (last_time.tv_nsec >= 1000000000) {
            last_time.tv_nsec -= 1000000000;
            last_time.tv_sec += 1;
        }
    }
}

int ExtraTimeNotOver(void) {
    UpdateTime();
    return (TrueEmulatedTime == OnTrueTime);
}

void WaitForNextTick(void) {
    Kindle_PollInput();
    while (TrueEmulatedTime == OnTrueTime) {
        usleep(1000); 
        UpdateTime();
    }
    OnTrueTime = TrueEmulatedTime;
}

// ---------------------------------------------------------
// 5. CORE STUBS & GLOBALS (BULLETPROOF ALIGNMENT)
// ---------------------------------------------------------
int QuietTime __attribute__((aligned(8))) = 0;
int QuietSubTicks __attribute__((aligned(8))) = 0;
int SpeedValue __attribute__((aligned(8))) = 1;
int WantNotAutoSlow __attribute__((aligned(8))) = 0;
int EmLagTime __attribute__((aligned(8))) = 0;
int ForceMacOff __attribute__((aligned(8))) = 0;
unsigned int CurMacDateInSeconds __attribute__((aligned(8))) = 3800000000;
int CurMacLatitude __attribute__((aligned(8))) = 0;
int CurMacLongitude __attribute__((aligned(8))) = 0;
int CurMacDelta __attribute__((aligned(8))) = 0;
int WantMacReset __attribute__((aligned(8))) = 0;
int WantMacInterrupt __attribute__((aligned(8))) = 0;
int CurMouseV __attribute__((aligned(8))) = 0;
int CurMouseH __attribute__((aligned(8))) = 0;
int EmVideoDisable __attribute__((aligned(8))) = 0;
int MyEvtQOutP __attribute__((aligned(8))) = 0;
int MyEvtQOutDone __attribute__((aligned(8))) = 0;

void* MySound_BeginWrite(uint32_t n, uint32_t *actL) { 
    *actL = (n < 8192) ? n : 8192; 
    return (void*)dummy_audio_buffer; 
}
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
// 6. FLOPPY DISK CONTROLLER
// ---------------------------------------------------------
int vSonyInsertedMask __attribute__((aligned(8))) = 0;
int vSonyRawMode __attribute__((aligned(8))) = 0;
int vSonyWritableMask __attribute__((aligned(8))) = 0;
int vSonyNewDiskWanted __attribute__((aligned(8))) = 0;
int vSonyNewDiskSize __attribute__((aligned(8))) = 0;

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
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = fatal_crash_handler;
    sigaction(SIGSEGV, &sa, NULL); 
    sigaction(SIGBUS, &sa, NULL);  
    
    printf("Mini vMac for Kindle: Initialization Begun.\n");
    
    FILE *f = fopen("vMac.ROM", "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    AllocMacMemory(rom_size);
    fread(ROM, 1, rom_size, f);
    fclose(f);

    printf("\n--- CRITICAL ADDRESS MAP ---\n");
    printf("CurMacLongitude      : %p\n", (void*)&CurMacLongitude);
    printf("last_time            : %p\n", (void*)&last_time);
    printf("dummy_audio_buffer   : %p\n", (void*)dummy_audio_buffer);
    printf("ROM                  : %p\n", (void*)ROM);
    printf("VidMem               : %p\n", (void*)VidMem);
    printf("----------------------------\n\n");

    mac_disk = fopen("disk.img", "r+b");
    if (mac_disk) {
        vSonyInsertedMask = 1;
        vSonyWritableMask = 1;
    }

    Kindle_Init();
    InitTime();
    
    printf("Handing execution over to 68k Core...\n");
    ProgramMain();

    Kindle_CleanUp();
    if (mac_disk) fclose(mac_disk);
    return 0;
}
