#pragma once

#include <atomic>
#include <cstdint>

#include <X11/Xlib.h>
#include <libusb-1.0/libusb.h>

class BridgeClient;

extern uint32_t keyBindings[128];
extern int mouseSpeed;
extern uint8_t lastCalcKeyUp;
extern Display* dpy;

#define CUSTOM_MOUSE_MOVE 0xFF

void receiveThread(libusb_device_handle* devHandle, std::atomic<bool>& stop, bool bridgeMode, BridgeClient* bridgeClient);
