#!/bin/bash
# build_kindle.sh
# Generates the X11 project, stubs X11 dependencies, injects E-Ink logic, and strips the UI.

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating Minimal Linux X11 configuration (-ui min)..."
# THE FIX: Added -ui min to completely rip out the Control Mode overlay!
./setup_t -t larm -api xwn -ui min -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Creating dummy X11 headers to bypass dependencies..."
mkdir -p X11/extensions
cat << 'EOF' > X11/Xlib.h
typedef unsigned long Window;
typedef unsigned long Pixmap;
typedef unsigned long Cursor;
typedef unsigned long Atom;
typedef unsigned long Time;
typedef unsigned long Colormap;
typedef void Display;
typedef void GC;
typedef void Visual;
typedef struct { int x; } XImage;
typedef struct { int type; } XEvent;
#define None 0L
EOF
touch X11/Xutil.h X11/Xos.h X11/keysym.h X11/keysymdef.h X11/cursorfont.h X11/Xatom.h X11/Xresource.h X11/extensions/XShm.h

echo "=> Injecting Kindle E-Ink Hybrid OS Glue..."
cat << 'EOF' > src/OSGLUXWN.c
#include "OSGCOMUI.h"
#include "OSGCOMUD.h"

#ifdef WantOSGLUXWN

#include "COMOSGLU.h"
#include "PBUFSTDC.h"
#include "CONTROLM.h"
#include "DATE2SEC.h"
#include "PROGMAIN.h"

#include "fbink.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdbool.h>

// ---------------------------------------------------------
// KINDLE HARDWARE GLOBALS
// ---------------------------------------------------------
static int fbfd = -1;
static int touch_fd = -1;
static uint8_t *fb_mem = NULL;
static size_t fb_size = 0;
static FBInkConfig fb_cfg = {0};

static int touch_x_min = 0, touch_x_max = 1448;
static int touch_y_min = 0, touch_y_max = 1072;
static int current_touch_x = 0, current_touch_y = 0;

// ---------------------------------------------------------
// VIDEO & E-INK BLITTER
// ---------------------------------------------------------
LOCALFUNC blnr Screen_Init(void) {
    fbfd = fbink_open();
    if (fbfd < 0) return falseblnr;
    
    fbink_init(fbfd, &fb_cfg);
    fb_cfg.is_flashing = false;
    fb_cfg.wfm_mode = WFM_A2;
    
    fb_size = vMacScreenWidth * vMacScreenHeight; 
    fb_mem = (uint8_t*)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    
    touch_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    return trueblnr;
}

LOCALPROC HaveChangedScreenBuff(ui4r top, ui4r left, ui4r bottom, ui4r right) {
    if (!fb_mem) return;
    ui3b *mac_vram = GetCurDrawBuff();
    if (!mac_vram) return;

    int mac_stride = vMacScreenWidth / 8;
    int fb_stride = vMacScreenWidth;

    for (int row = top; row < bottom; row++) {
        for (int col = left; col < right; col++) {
            int byte_idx = (row * mac_stride) + (col / 8);
            int bit_idx = 7 - (col % 8);
            uint8_t mac_byte = mac_vram[byte_idx];
            
            bool is_black = (mac_byte >> bit_idx) & 1;
            int fb_idx = (row * fb_stride) + col;
            fb_mem[fb_idx] = is_black ? 0x00 : 0xFF;
        }
    }
    fbink_refresh(fbfd, left, top, right - left, bottom - top, &fb_cfg);
}

LOCALPROC MyDrawChangesAndClear(void) {
    if (ScreenChangedBottom > ScreenChangedTop) {
        HaveChangedScreenBuff(ScreenChangedTop, ScreenChangedLeft, ScreenChangedBottom, ScreenChangedRight);
        ScreenClearChanges();
    }
}

GLOBALOSGLUPROC DoneWithDrawingForTick(void) { 
    MyDrawChangesAndClear(); 
}

// ---------------------------------------------------------
// MEMORY ALLOCATOR
// ---------------------------------------------------------
LOCALPROC ReserveAllocAll(void) {
    ReserveAllocOneBlock(&ROM, kROM_Size, 5, falseblnr);
    ReserveAllocOneBlock(&screencomparebuff, vMacScreenNumBytes, 5, trueblnr);
    EmulationReserveAlloc();
}

LOCALFUNC blnr AllocMyMemory(void) {
    uimr n;
    ReserveAllocOffset = 0;
    ReserveAllocBigBlock = nullpr;
    ReserveAllocAll();
    n = ReserveAllocOffset;
    
    ReserveAllocBigBlock = (ui3p)calloc(1, n);
    if (NULL == ReserveAllocBigBlock) return falseblnr;
    
    ReserveAllocOffset = 0;
    ReserveAllocAll();
    return trueblnr;
}

LOCALPROC UnallocMyMemory(void) {
    if (nullpr != ReserveAllocBigBlock) free((char *)ReserveAllocBigBlock);
}

// ---------------------------------------------------------
// TIMING & VBL SYNC
// ---------------------------------------------------------
LOCALVAR ui5b TrueEmulatedTime = 0;
LOCALVAR blnr HaveTimeDelta = falseblnr;
LOCALVAR ui5b TimeDelta;
LOCALVAR ui5b NewMacDateInSeconds;
LOCALVAR ui5b LastTimeSec;
LOCALVAR ui5b LastTimeUsec;
#define TicksPerSecond 1000000

LOCALPROC GetCurrentTicks(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    if (!HaveTimeDelta) {
        time_t Current_Time;
        struct tm *s;
        time(&Current_Time);
        s = localtime(&Current_Time);
        TimeDelta = Date2MacSeconds(s->tm_sec, s->tm_min, s->tm_hour, s->tm_mday, 1 + s->tm_mon, 1900 + s->tm_year) - t.tv_sec;
        HaveTimeDelta = trueblnr;
    }
    NewMacDateInSeconds = t.tv_sec + TimeDelta;
    LastTimeSec = (ui5b)t.tv_sec;
    LastTimeUsec = (ui5b)t.tv_usec;
}

#define MyInvTimeStep 16626
LOCALVAR ui5b NextTimeSec;
LOCALVAR ui5b NextTimeUsec;

LOCALPROC IncrNextTime(void) {
    NextTimeUsec += MyInvTimeStep;
    if (NextTimeUsec >= TicksPerSecond) { NextTimeUsec -= TicksPerSecond; NextTimeSec += 1; }
}
LOCALPROC InitNextTime(void) {
    NextTimeSec = LastTimeSec; NextTimeUsec = LastTimeUsec; IncrNextTime();
}
LOCALFUNC si5b GetTimeDiff(void) {
    return ((si5b)(LastTimeSec - NextTimeSec)) * TicksPerSecond + ((si5b)(LastTimeUsec - NextTimeUsec));
}
LOCALPROC UpdateTrueEmulatedTime(void) {
    si5b TimeDiff;
    GetCurrentTicks();
    TimeDiff = GetTimeDiff();
    if (TimeDiff >= 0) {
        if (TimeDiff > 16 * MyInvTimeStep) { ++TrueEmulatedTime; InitNextTime(); }
        else { do { ++TrueEmulatedTime; IncrNextTime(); TimeDiff -= TicksPerSecond; } while (TimeDiff >= 0); }
    } else if (TimeDiff < - 16 * MyInvTimeStep) { InitNextTime(); }
}
LOCALFUNC blnr CheckDateTime(void) {
    if (CurMacDateInSeconds != NewMacDateInSeconds) { CurMacDateInSeconds = NewMacDateInSeconds; return trueblnr; }
    return falseblnr;
}
LOCALFUNC blnr InitLocationDat(void) {
    GetCurrentTicks();
    CurMacDateInSeconds = NewMacDateInSeconds;
    return trueblnr;
}

GLOBALOSGLUFUNC blnr ExtraTimeNotOver(void) {
    UpdateTrueEmulatedTime();
    return TrueEmulatedTime == OnTrueTime;
}

// ---------------------------------------------------------
// FLOPPY DISK CONTROLLER
// ---------------------------------------------------------
LOCALVAR FILE *mac_disk = NULL;
LOCALVAR tDrive mac_drive_no = 0;

LOCALPROC InitDrives(void) {
    mac_disk = fopen("disk.img", "r+b");
    if (mac_disk && FirstFreeDisk(&mac_drive_no)) {
        DiskInsertNotify(mac_drive_no, falseblnr);
    }
}

GLOBALOSGLUFUNC tMacErr vSonyTransfer(blnr IsWrite, ui3p Buffer, tDrive Drive_No, ui5r Sony_Start, ui5r Sony_Count, ui5r *Sony_ActCount) {
    if (Drive_No == mac_drive_no && mac_disk) {
        fseek(mac_disk, Sony_Start, SEEK_SET);
        if (IsWrite) *Sony_ActCount = fwrite(Buffer, 1, Sony_Count, mac_disk);
        else *Sony_ActCount = fread(Buffer, 1, Sony_Count, mac_disk);
        return mnvm_noErr;
    }
    return mnvm_miscErr;
}

GLOBALOSGLUFUNC tMacErr vSonyGetSize(tDrive Drive_No, ui5r *Sony_Count) {
    if (Drive_No == mac_drive_no && mac_disk) {
        fseek(mac_disk, 0, SEEK_END);
        *Sony_Count = ftell(mac_disk);
        return mnvm_noErr;
    }
    return mnvm_miscErr;
}
GLOBALOSGLUFUNC tMacErr vSonyEject(tDrive Drive_No) { return mnvm_noErr; }
GLOBALOSGLUFUNC tMacErr vSonyEjectDelete(tDrive Drive_No) { return mnvm_noErr; }
GLOBALOSGLUFUNC tMacErr vSonyGetName(tDrive Drive_No, tPbuf *r) { return mnvm_miscErr; }

// ---------------------------------------------------------
// ROM LOADER & MINIMAL UI OVERRIDES
// ---------------------------------------------------------
GLOBALOSGLUFUNC blnr WaitTillWantRun(void) { return trueblnr; }
GLOBALOSGLUPROC SysMsgDisplayWait(char *s) { fprintf(stderr, "%s\n", s); }
GLOBALOSGLUPROC MacMsgDisplayWait(char *s) { fprintf(stderr, "%s\n", s); }

LOCALFUNC blnr LoadMacRom(void) {
    FILE *ROM_File = fopen("vMac.ROM", "rb");
    if (ROM_File) {
        if (fread(ROM, 1, kROM_Size, ROM_File) == kROM_Size) {
            ROM_IsValid();
            fclose(ROM_File);
            return trueblnr;
        }
        fclose(ROM_File);
    }
    return falseblnr;
}

// ---------------------------------------------------------
// MAIN LOOP & TOUCH INPUT
// ---------------------------------------------------------
LOCALPROC CheckForSystemEvents(void) {
    if (touch_fd < 0) return;
    struct input_event ev;
    blnr touch_moved = falseblnr;
    
    while (read(touch_fd, &ev, sizeof(struct input_event)) > 0) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X) {
                float scale_x = (float)(ev.value - touch_x_min) / (touch_x_max - touch_x_min);
                current_touch_x = (int)(scale_x * vMacScreenWidth);
                touch_moved = trueblnr;
            } else if (ev.code == ABS_MT_POSITION_Y) {
                float scale_y = (float)(ev.value - touch_y_min) / (touch_y_max - touch_y_min);
                current_touch_y = (int)(scale_y * vMacScreenHeight);
                touch_moved = trueblnr;
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            MyMouseButtonSet(ev.value == 1);
        }
    }
    if (touch_moved) MyMousePositionSet(current_touch_x, current_touch_y);
}

GLOBALOSGLUPROC WaitForNextTick(void) {
label_retry:
    CheckForSystemEvents();
    if (ForceMacOff) return;
    if (ExtraTimeNotOver()) {
        usleep(1000); 
        goto label_retry;
    }
    CheckDateTime();
    OnTrueTime = TrueEmulatedTime;
}

GLOBALOSGLUPROC MyMoveBytes(anyp srcPtr, anyp destPtr, si5b byteCount) { memmove((char *)destPtr, (char *)srcPtr, byteCount); }

#if IncludeHostTextClipExchange
GLOBALOSGLUFUNC tMacErr HTCEexport(tPbuf i) { return mnvm_miscErr; }
GLOBALOSGLUFUNC tMacErr HTCEimport(tPbuf *r) { return mnvm_miscErr; }
#endif
#if MySoundEnabled
GLOBALOSGLUFUNC tpSoundSamp MySound_BeginWrite(ui4r n, ui4r *actL) { *actL = 0; return NULL; }
GLOBALOSGLUPROC MySound_EndWrite(ui4r actL) {}
#endif

// ---------------------------------------------------------
// INITIALIZATION
// ---------------------------------------------------------
LOCALPROC ZapOSGLUVars(void) { InitDrives(); }

LOCALFUNC blnr InitOSGLU(void) {
    if (AllocMyMemory())
    if (LoadMacRom())
    if (InitLocationDat())
    if (Screen_Init()) {
        InitNextTime();
        return trueblnr;
    }
    return falseblnr;
}

LOCALPROC UnInitOSGLU(void) {
    if (mac_disk) fclose(mac_disk);
    if (touch_fd >= 0) close(touch_fd);
    if (fbfd >= 0) {
        if (fb_mem) munmap(fb_mem, fb_size);
        fbink_close(fbfd);
    }
    UnallocMyMemory();
}

int main(int argc, char **argv) {
    ZapOSGLUVars();
    if (InitOSGLU()) {
        ProgramMain();
    }
    UnInitOSGLU();
    return 0;
}
#endif /* WantOSGLUXWN */
EOF

echo "=> Patching Makefile to drop X11 dependencies and link FBInk..."
sed -i 's/gcc /arm-linux-gnueabihf-gcc /g' Makefile
sed -i 's/strip --strip-unneeded/arm-linux-gnueabihf-strip --strip-unneeded/g' Makefile

sed -i 's|-Icfg/|-Icfg/ -I./ |g' Makefile
sed -i 's|-L/usr/X11R6/lib -lX11|-L/usr/arm-linux-gnueabihf/lib -lfbink -lm|g' Makefile
sed -i 's|-lXext||g' Makefile
sed -i 's|-I/usr/X11R6/include||g' Makefile

echo "=> Cross-compiling the emulator..."
make

echo "=> Build successful! Binary is ready for deployment."
