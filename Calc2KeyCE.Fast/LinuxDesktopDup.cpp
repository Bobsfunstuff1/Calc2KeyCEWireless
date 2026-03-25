#include "LinuxDesktopDup.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstring>
#include <unistd.h>
#include <cstdio>

static Display* display = nullptr;
static Window root;
static XImage* img = nullptr;
static XShmSegmentInfo shminfo;

LinuxDesktopDup::~LinuxDesktopDup() {
    Close();
}

bool LinuxDesktopDup::Initialize() {
    display = XOpenDisplay(nullptr);
    if (!display)
        return false;

    root = DefaultRootWindow(display);
    return true;
}

bool LinuxDesktopDup::CaptureNext(uint8_t* dest) {
    if (!display || !dest)
        return false;

    XWindowAttributes gwa;
    XGetWindowAttributes(display, root, &gwa);
    int screen_w = gwa.width;
    int screen_h = gwa.height;

    const int target_w = 320;
    const int target_h = 240;

    if (img) {
        XShmDetach(display, &shminfo);
        XDestroyImage(img);
        shmdt(shminfo.shmaddr);
        shmctl(shminfo.shmid, IPC_RMID, 0);
        img = nullptr;
    }

    img = XShmCreateImage(display,
        DefaultVisual(display, 0),
        DefaultDepth(display, 0),
        ZPixmap,
        nullptr,
        &shminfo,
        screen_w,
        screen_h);

    if (!img)
        return false;

    shminfo.shmid = shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT | 0777);
    if (shminfo.shmid < 0)
        return false;

    shminfo.shmaddr = (char*)shmat(shminfo.shmid, nullptr, 0);
    if (shminfo.shmaddr == (char*)-1)
        return false;

    img->data = shminfo.shmaddr;
    shminfo.readOnly = False;

    if (!XShmAttach(display, &shminfo))
        return false;

    if (!XShmGetImage(display, root, img, 0, 0, AllPlanes)) {
        fprintf(stderr, "XShmGetImage failed.\n");
        return false;
    }

    for (int y = 0; y < target_h; ++y) {
        for (int x = 0; x < target_w; ++x) {
            int src_x = x * screen_w / target_w;
            int src_y = y * screen_h / target_h;

            unsigned long pixel = XGetPixel(img, src_x, src_y);
            uint8_t r = (pixel & img->red_mask) >> (__builtin_ffs(img->red_mask) - 1);
            uint8_t g = (pixel & img->green_mask) >> (__builtin_ffs(img->green_mask) - 1);
            uint8_t b = (pixel & img->blue_mask) >> (__builtin_ffs(img->blue_mask) - 1);

            size_t idx = (y * target_w + x) * 4;
            dest[idx + 0] = b;      // blue
            dest[idx + 1] = g;      // green
            dest[idx + 2] = r;      // red
            dest[idx + 3] = 0xFF;   // alpha

            //if (x == 0 && y == 0) {
            //    printf("Capture pixel (r,g,b): %02x %02x %02x\n", r, g, b);
            //}
        }
    }

    return true;
}

void LinuxDesktopDup::Close() {
    if (img) {
        XShmDetach(display, &shminfo);
        XDestroyImage(img);
        shmdt(shminfo.shmaddr);
        shmctl(shminfo.shmid, IPC_RMID, 0);
        img = nullptr;
    }
    if (display) {
        XCloseDisplay(display);
        display = nullptr;
    }
}