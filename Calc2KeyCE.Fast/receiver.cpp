#include "receiver.h"

#include "bridge_client.h"
#include "keypad.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>

extern bool bindingKeyState;
extern uint8_t bindingKey;
extern uint32_t newBinding;
const char* getCalcKeyName(uint8_t i);

static uint8_t moveState = 0;
int mouseSpeed = 10;

using namespace std::chrono;

uint32_t keyBindings[128];
uint8_t lastCalcKeyUp = static_cast<uint8_t>(-1);

uint8_t recData[7];
Display* dpy = nullptr;

bool keyDownState[128] = { false };
bool osKeyDown[256] = { false };
bool osMouseDown[10] = { false };
static steady_clock::time_point lastPressTime[128];

static void simulateKey(uint32_t keysym, bool press) {
    if (!dpy) return;

    const KeyCode kc = XKeysymToKeycode(dpy, keysym);
    if (!kc) return;

    if (press) {
        if (!osKeyDown[kc]) {
            XTestFakeKeyEvent(dpy, kc, True, CurrentTime);
            osKeyDown[kc] = true;
        }
    }
    else {
        if (osKeyDown[kc]) {
            XTestFakeKeyEvent(dpy, kc, False, CurrentTime);
            osKeyDown[kc] = false;
        }
    }

    XFlush(dpy);
}

static void simulateMouse(int button, bool press) {
    if (!dpy || button < 0 || button >= static_cast<int>(sizeof(osMouseDown))) return;

    if (press && !osMouseDown[button]) {
        XTestFakeButtonEvent(dpy, button, True, CurrentTime);
        osMouseDown[button] = true;
    }
    else if (!press && osMouseDown[button]) {
        XTestFakeButtonEvent(dpy, button, False, CurrentTime);
        osMouseDown[button] = false;
    }

    XFlush(dpy);
}

static void simulateMouseMove(int dx, int dy) {
    if (!dpy) return;
    XTestFakeRelativeMotionEvent(dpy, dx, dy, CurrentTime);
}

static void handleLocalBindings() {
    for (uint16_t i = 0; i < 128; ++i) {
        const uint8_t dataIdx = (i >> 4) & 7;
        const uint8_t bit = i & 0xF;
        const uint8_t mask = 1 << bit;

        if (dataIdx >= 7) {
            continue;
        }

        const bool pressedNow = (recData[dataIdx] & mask) != 0;
        const bool pressedBefore = keyDownState[i];
        if (pressedNow == pressedBefore) {
            continue;
        }

        keyDownState[i] = pressedNow;

        if (pressedNow && bindingKeyState && bindingKey == static_cast<uint8_t>(-1)) {
            bindingKey = static_cast<uint8_t>(i);
            newBinding = keyBindings[i] ? keyBindings[i] : 0x10000;
            printf("[BIND] Calculator key %d captured for binding.\n", i);
        }

        if (!keyBindings[i]) {
            continue;
        }

        const uint32_t binding = keyBindings[i];
        if (binding & 0x10000) {
            const KeySym keysym = static_cast<KeySym>(binding & 0xFFFF);
            if (pressedNow) {
                lastPressTime[i] = steady_clock::now();
                simulateKey(keysym, true);
            }
            else {
                const auto heldDuration = duration_cast<milliseconds>(steady_clock::now() - lastPressTime[i]).count();
                if (heldDuration < 150) {
                    simulateKey(keysym, true);
                    simulateKey(keysym, false);
                }
                else {
                    simulateKey(keysym, false);
                }
            }
        }
        else {
            const uint8_t action = binding & 0xFF;
            if (action == CUSTOM_MOUSE_MOVE) {
                const uint8_t dir = (binding >> 8) & 0xFF;
                if (pressedNow) moveState |= (1 << dir);
                else moveState &= ~(1 << dir);
            }
            else {
                simulateMouse(action, pressedNow);
            }
        }

        if (!pressedNow) {
            lastCalcKeyUp = static_cast<uint8_t>(i);
        }
    }
}

void receiveThread(libusb_device_handle* devHandle, std::atomic<bool>& stop, bool bridgeMode, BridgeClient* bridgeClient) {
    int received = 0;

    if (!bridgeMode) {
        dpy = XOpenDisplay(nullptr);
        if (!dpy) {
            fprintf(stderr, "Failed to open X display\n");
            stop.store(true);
            return;
        }
    }

    while (!stop.load()) {
        const int rc = libusb_bulk_transfer(devHandle, 0x81, recData, sizeof(recData), &received, 10);
        if (rc == 0 && received == 7) {
            if (bridgeMode) {
                if (!bridgeClient || !bridgeClient->sendCalcKeys(recData, sizeof(recData))) {
                    fprintf(stderr, "Bridge send failed: %s\n", bridgeClient ? bridgeClient->lastError().c_str() : "bridge client missing");
                    stop.store(true);
                    break;
                }
            }
            else {
                handleLocalBindings();
            }
        }

        if (!bridgeMode && moveState) {
            const int dy = ((moveState & 0b0001) ? -1 : 0) + ((moveState & 0b0010) ? 1 : 0);
            const int dx = ((moveState & 0b0100) ? -1 : 0) + ((moveState & 0b1000) ? 1 : 0);
            simulateMouseMove(dx * mouseSpeed, dy * mouseSpeed);
        }

        usleep(1000);
    }

    if (dpy) {
        XCloseDisplay(dpy);
        dpy = nullptr;
    }

    printf("receive thread exited.\n");
}
