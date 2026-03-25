#pragma once

#include "LinuxDesktopDup.h"

#include <atomic>
#include <cstdint>
#include <vector>

struct libusb_device_handle;
class BridgeClient;

void sendThread(libusb_device_handle* devHandle, std::atomic<bool>& stop, bool bridgeMode, BridgeClient* bridgeClient);
float getFrameTime(void* data, int idx);
bool sendFrameToCalculator(libusb_device_handle* devHandle, int32_t sendLen, const uint8_t* payload, std::atomic<bool>& stop);

extern float redMult;
extern float greenMult;
extern float blueMult;
extern bool running;
