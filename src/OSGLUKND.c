/*
 * OSGLUKND.c
 * Bare-metal Platform Abstraction Layer for Kindle PW3 (i.MX6)
 * FIXED: Proper memory alignment, bounds checking, and error handling
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
#include <pthread.h>

#define TRUE_KINDLE_WIDTH  1072
#define TRUE_KINDLE_HEIGHT 1448
#define MAC_WIDTH  1440
#define MAC_HEIGHT 1056

#define ALLOC_PAGE_SIZE 4096
#define MAX_EMULATION_SIZE (64 * 1024 * 1024)  /* 64MB max allocation */

static int fbfd = -1;
static int touch_fd = -1;
static uint8_t *fb_mem = NULL;
static size_t fb_size = 0;
static FBInkConfig fb_cfg = {0};

static int kindle_stride = 1088;
static int physical_offset = 0;

extern void EmulationReserveAlloc(void);
extern void ProgramMain(void);
extern uint8_t *VidMem;

char *ROM = NULL;
FILE *mac_disk = NULL;
static int frame_skip_counter = 0;
static uint8_t *RawAllocBlock = NULL;

static uint64_t dummy_audio_buffer[8192 / 8];
char vSonyNewDiskName[256];

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
        fprintf(stderr, "Diagnosis: Core PC Corruption or Struct Mismatch\n");
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
    if (fb_mem == MAP_FAILED) {
        fprintf(stderr, "ERROR: Failed to mmap framebuffer\n");
        fb_mem = NULL;
        return;
    }
    
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
            
            /* Bounds check framebuffer access */
            if (fb_idx >= 0 && fb_idx < (int)fb_size) {
                fb_mem[fb_idx] = ((VidMem[byte_idx] >> bit_idx) & 1) ? 0x00 : 0xFF;
            }
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
// 3. CORE MEMORY ALLOCATOR (FIXED)
// ---------------------------------------------------------
static size_t ReserveAllocOffset = 0;
static uint8_t *ReserveAllocBigBlock = NULL;
static size_t ReserveAllocTotalSize = 0;

/* First pass: calculate required size */
void ReserveAllocOneBlock(void **p, size_t s, int align, int clear) {
    if (p == NULL) {
        fprintf(stderr, "ERROR: ReserveAllocOneBlock called with NULL pointer\n");
        exit(1);
    }
    
    /* Calculate alignment requirement */
    size_t alignment = 1 << align;
    if (alignment == 0 || alignment > ALLOC_PAGE_SIZE) {
        fprintf(stderr, "ERROR: Invalid alignment value: %d\n", align);
        exit(1);
    }
    
    /* Calculate padding needed for alignment */
    size_t remainder = ReserveAllocOffset % alignment;
    if (remainder != 0) {
        ReserveAllocOffset += (alignment - remainder);
    }
    
    /* Track requested allocation for bounds checking */
    if (ReserveAllocBigBlock != NULL) {
        /* Second pass: actually allocate from the big block */
        if (ReserveAllocOffset + s > ReserveAllocTotalSize) {
            fprintf(stderr, "ERROR: Allocation overflow! Requested %zu + %zu, but only %zu available\n",
                    ReserveAllocOffset, s, ReserveAllocTotalSize);
            exit(1);
        }
        
        *p = (void*)(ReserveAllocBigBlock + ReserveAllocOffset);
        if (clear && *p) {
            memset(*p, 0, s);
        }
    } else {
        /* First pass: just track size, set pointer to NULL for validation */
        *p = NULL;
    }
    
    ReserveAllocOffset += s;
}

void AllocMacMemory(long rom_size) {
    if (rom_size <= 0 || rom_size > MAX_EMULATION_SIZE) {
        fprintf(stderr, "ERROR: Invalid ROM size: %ld\n", rom_size);
        exit(1);
    }

    /* ===== PASS 1: Calculate total size needed ===== */
    fprintf(stderr, "[Allocator] Pass 1: Calculating total allocation size...\n");
    ReserveAllocOffset = 0;
    ReserveAllocBigBlock = NULL;
    
    ReserveAllocOneBlock((void**)&ROM, rom_size, 5, 0);
    EmulationReserveAlloc();
    
    /* Store the calculated total size */
    size_t total_needed = ReserveAllocOffset;
    fprintf(stderr, "[Allocator] Pass 1 complete: Need %zu bytes (0x%zx)\n", 
            total_needed, total_needed);
    
    if (total_needed == 0) {
        fprintf(stderr, "ERROR: Zero bytes allocated in pass 1\n");
        exit(1);
    }
    
    if (total_needed > MAX_EMULATION_SIZE) {
        fprintf(stderr, "ERROR: Required allocation (%zu) exceeds max (%u)\n",
                total_needed, MAX_EMULATION_SIZE);
        exit(1);
    }

    /* ===== ALLOCATE: Get the memory block ===== */
    fprintf(stderr, "[Allocator] Allocating %zu bytes + %u byte page alignment buffer...\n",
            total_needed, ALLOC_PAGE_SIZE);
    
    RawAllocBlock = (uint8_t*)calloc(1, total_needed + ALLOC_PAGE_SIZE);
    if (!RawAllocBlock) {
        fprintf(stderr, "ERROR: calloc failed for %zu bytes\n", total_needed + ALLOC_PAGE_SIZE);
        exit(1);
    }
    
    fprintf(stderr, "[Allocator] Allocation successful, raw block at %p\n", (void*)RawAllocBlock);

    /* ===== ALIGN: Calculate aligned start pointer ===== */
    size_t rptr = (size_t)RawAllocBlock;
    size_t rem = rptr % ALLOC_PAGE_SIZE;
    size_t alignment_offset = (rem == 0) ? 0 : (ALLOC_PAGE_SIZE - rem);
    
    ReserveAllocBigBlock = RawAllocBlock + alignment_offset;
    ReserveAllocTotalSize = total_needed;
    
    fprintf(stderr, "[Allocator] Aligned block at %p (offset +%zu)\n", 
            (void*)ReserveAllocBigBlock, alignment_offset);

    /* ===== PASS 2: Actually allocate from the aligned block ===== */
    fprintf(stderr, "[Allocator] Pass 2: Assigning pointers and clearing memory...\n");
    ReserveAllocOffset = 0;
    
    ReserveAllocOneBlock((void**)&ROM, rom_size, 5, 0);
    fprintf(stderr, "[Allocator] ROM: %p (size %ld)\n", (void*)ROM, rom_size);
    
    EmulationReserveAlloc();
    
    fprintf(stderr, "[Allocator] Pass 2 complete: Final offset %zu bytes\n", ReserveAllocOffset);
    
    if (ReserveAllocOffset != total_needed) {
        fprintf(stderr, "WARNING: Pass 2 offset mismatch! Expected %zu, got %zu\n",
                total_needed, ReserveAllocOffset);
    }
    
    fprintf(stderr, "[Allocator] 68k core memory initialization SUCCESS\n");
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
        if (fb_mem && fb_mem != MAP_FAILED) munmap(fb_mem, fb_size);
        fbink_close(fbfd);
    }
    if (RawAllocBlock) free(RawAllocBlock);
    RawAllocBlock = NULL;
    ReserveAllocBigBlock = NULL;
}

unsigned int TrueEmulatedTime = 0;
unsigned int OnTrueTime = 1;
static struct timespec last_time;

void InitTime(void) {
    clock_gettime(CLOCK_MONOTONIC, &last_time);
    TrueEmulatedTime = 0;
    OnTrueTime = 0;
}

void UpdateTime(void) {
    struct timespec now;
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
// 5. CORE STUBS & GLOBALS
// ---------------------------------------------------------
int QuietTime = 0, QuietSubTicks = 0, SpeedValue = 1;
int WantNotAutoSlow = 0, EmLagTime = 0, ForceMacOff = 0;
unsigned int CurMacDateInSeconds = 3800000000;
int CurMacLatitude = 0, CurMacLongitude = 0, CurMacDelta = 0;
int WantMacReset = 0, WantMacInterrupt = 0;
int CurMouseV = 0, CurMouseH = 0, EmVideoDisable = 0;
int MyEvtQOutP = 0, MyEvtQOutDone = 0;

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
int vSonyInsertedMask = 0, vSonyRawMode = 0, vSonyWritableMask = 0;
int vSonyNewDiskWanted = 0, vSonyNewDiskSize = 0;

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
        if (IsWrite) fwrite(Buffer, 1, Sony_Count, mac_disk);
        else fread(Buffer, 1, Sony_Count, mac_disk);
        if (Sony_ActCount) *Sony_ActCount = Sony_Count;
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
// MAIN ENTRY POINT (THE THREAD BYPASS)
// ---------------------------------------------------------
void* emulator_thread(void* arg) {
    printf("Spawning 68k Core in 4MB Worker Thread...\n");
    ProgramMain();
    return NULL;
}

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
    if (!f) {
        fprintf(stderr, "ERROR: Could not open vMac.ROM\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (rom_size <= 0) {
        fprintf(stderr, "ERROR: Invalid ROM size: %ld\n", rom_size);
        fclose(f);
        return 1;
    }

    printf("Loading ROM (%ld bytes)...\n", rom_size);
    AllocMacMemory(rom_size);
    
    if (!ROM) {
        fprintf(stderr, "ERROR: ROM allocation failed\n");
        fclose(f);
        return 1;
    }
    
    size_t bytes_read = fread(ROM, 1, rom_size, f);
    fclose(f);
    
    if ((long)bytes_read != rom_size) {
        fprintf(stderr, "ERROR: ROM read failed (expected %ld, got %zu)\n", rom_size, bytes_read);
        return 1;
    }
    
    printf("ROM loaded successfully.\n");

    mac_disk = fopen("disk.img", "r+b");
    if (mac_disk) {
        vSonyInsertedMask = 1;
        vSonyWritableMask = 1;
        printf("Disk image loaded.\n");
    }

    Kindle_Init();
    if (!fb_mem) {
        fprintf(stderr, "ERROR: Kindle display initialization failed\n");
        return 1;
    }
    
    InitTime();
    
    /* THE KERNEL BYPASS: Launch the emulator inside a custom POSIX thread 
       to guarantee it has enough stack space, ignoring the Kindle's limits. */
    pthread_t emu_thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024); 
    
    if (pthread_create(&emu_thread, &attr, emulator_thread, NULL) != 0) {
        fprintf(stderr, "Fatal Error: Failed to spawn emulator thread!\n");
        exit(1);
    }
    
    /* Wait for the emulator to finish */
    pthread_join(emu_thread, NULL);
    pthread_attr_destroy(&attr);

    Kindle_CleanUp();
    if (mac_disk) fclose(mac_disk);
    
    printf("Emulator cleanly shut down.\n");
    return 0;
}
