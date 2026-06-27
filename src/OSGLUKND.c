#include "vMacApp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>

// 1. Physical Kindle screen dimensions (From your FBInk logs)
#define KINDLE_WIDTH 1072
#define KINDLE_HEIGHT 1448
#define KINDLE_STRIDE 1088

// 2. Logical Mac screen dimensions (-hres 1440 -vres 1056)
#define MAC_WIDTH 1440
#define MAC_HEIGHT 1056

static int fbfd = -1;
static unsigned char *fbp = NULL;
static long int screensize = 0;
static int input_fd = -1;

/*
 * HARDWARE INITIALIZATION
 * Call this when the emulator starts
 */
void Init_Kindle_Hardware(void) {
    printf("Mini vMac for Kindle: Starting up...\n");

    // Open the raw Linux framebuffer
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd == -1) {
        printf("FATAL: Cannot open /dev/fb0\n");
        exit(1);
    }

    // Calculate exact memory footprint using the true 1088 hardware stride
    screensize = KINDLE_HEIGHT * KINDLE_STRIDE;
    
    // Map it directly to RAM
    fbp = (unsigned char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbp == MAP_FAILED) {
        printf("FATAL: Failed to mmap framebuffer\n");
        exit(1);
    }
    
    printf("Framebuffer mapped successfully. Stride: %d\n", KINDLE_STRIDE);
    
    // Open touch input device (Non-blocking)
    input_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
}

/*
 * THE SOFTWARE ROTATION BLITTER
 * Call this inside Mini vMac's screen update hook.
 * 'mac_buffer' is the pointer to Mini vMac's internal 8-bit screen array.
 */
void Update_Kindle_Screen(unsigned char *mac_buffer) {
    if (!fbp) return;

    // Loop strictly within the bounds of the logical Mac OS screen
    for (int y = 0; y < MAC_HEIGHT; y++) {
        for (int x = 0; x < MAC_WIDTH; x++) {
            
            // 90-degree Counter-Clockwise Rotation 
            // Maps the Landscape Mac desktop to the Portrait Kindle screen
            int k_x = y;
            int k_y = MAC_WIDTH - 1 - x;
            
            // Map logical coordinates to physical memory using the exact 1088 stride
            int fb_index = (k_y * KINDLE_STRIDE) + k_x;
            int mac_index = (y * MAC_WIDTH) + x;
            
            // Blast the 8-bit grayscale pixel directly to RAM
            fbp[fb_index] = mac_buffer[mac_index];
        }
    }
}

/*
 * CLEANUP
 * Call this when the emulator quits
 */
void Cleanup_Kindle_Hardware(void) {
    if (fbp && fbp != MAP_FAILED) {
        munmap(fbp, screensize);
    }
    if (fbfd != -1) close(fbfd);
    if (input_fd != -1) close(input_fd);
    printf("Hardware released cleanly.\n");
}

