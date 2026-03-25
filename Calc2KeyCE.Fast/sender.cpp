#include "sender.h"

#include "bridge_client.h"
#include "compression.h"

#include <leptonica/allheaders.h>
#include <libusb-1.0/libusb.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

#define CALC_WIDTH 320
#define CALC_HEIGHT 240
#define BITS_PER_PIXEL 32
#define BUF_LEN 512 + CALC_WIDTH * CALC_HEIGHT
#define COMPRESSION_TIMEOUT (CLOCKS_PER_SEC / 10)

constexpr int STATS_BUF_SIZE = 64;

bool running = false;
float redMult = 1.0f;
float greenMult = 1.0f;
float blueMult = 1.0f;

static float timeBuf[STATS_BUF_SIZE] = { 0 };
static int curBufIdx = 0;

static uint16_t convertColor(uint32_t color) {
    uint8_t r = color & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = (color >> 16) & 0xFF;

    uint16_t r5 = static_cast<uint16_t>((r >> 3) * redMult);
    uint16_t g6 = static_cast<uint16_t>((g >> 2) * greenMult);
    uint16_t b5 = static_cast<uint16_t>((b >> 3) * blueMult);

    if (r5 > 31) r5 = 31;
    if (g6 > 63) g6 = 63;
    if (b5 > 31) b5 = 31;

    return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

static void pixFunc(PIX* pix, uint8_t* sendBuf) {
    PIX* qPic = pixMedianCutQuantGeneral(pix, 0, 8, 240, 5, 1, 0);
    PIX* rPic = pixRotate90(qPic, 1);

    uint32_t* picData = pixGetData(rPic);
    PIXCMAP* cmap = pixGetColormap(rPic);
    if (!cmap) {
        fprintf(stderr, "Error: no colormap after quantization.\n");
        pixDestroy(&qPic);
        pixDestroy(&rPic);
        return;
    }

    uint32_t* colors = reinterpret_cast<uint32_t*>(cmap->array);
    for (int i = 0; i < 256; ++i) {
        *reinterpret_cast<uint16_t*>(sendBuf + 2 * i) = convertColor(colors[i]);
    }

    for (size_t i = 0; i < CALC_WIDTH * CALC_HEIGHT / 4; ++i) {
        *reinterpret_cast<uint32_t*>(sendBuf + 512 + 4 * i) =
            ((picData[i] & 0xFF) << 24) |
            (((picData[i] >> 8) & 0xFF) << 16) |
            (((picData[i] >> 16) & 0xFF) << 8) |
            ((picData[i] >> 24) & 0xFF);
    }

    pixDestroy(&qPic);
    pixDestroy(&rPic);
}

float getFrameTime(void* data, int idx) {
    (void)data;
    return timeBuf[(curBufIdx + idx) % STATS_BUF_SIZE];
}

bool sendFrameToCalculator(libusb_device_handle* devHandle, int32_t sendLen, const uint8_t* payload, std::atomic<bool>& stop) {
    uint8_t sizeBuf[64] = { 0 };
    std::memcpy(sizeBuf, &sendLen, sizeof(int32_t));

    int sentLen = 0;
    if (libusb_bulk_transfer(devHandle, 2, sizeBuf, 64, &sentLen, 0) != 0 || sentLen != 64) {
        stop.store(true);
        fprintf(stderr, "Failed to send frame size to calculator.\n");
        return false;
    }

    if (sendLen <= 0 || !payload) {
        return true;
    }

    for (int32_t offset = 0; offset < sendLen; offset += 64) {
        if (libusb_bulk_transfer(devHandle, 2, const_cast<uint8_t*>(payload + offset), 64, &sentLen, 0) != 0 || sentLen != 64) {
            stop.store(true);
            fprintf(stderr, "Failed while streaming frame payload to calculator.\n");
            return false;
        }
    }

    return true;
}

static void sendDirectFrames(libusb_device_handle* devHandle, std::atomic<bool>& stop) {
    LinuxDesktopDup dup;
    dup.Initialize();

    uint8_t* sendBuf = static_cast<uint8_t*>(std::malloc(BUF_LEN));
    if (!sendBuf) {
        throw std::bad_alloc();
    }

    PIX* picture = pixCreate(CALC_WIDTH, CALC_HEIGHT, BITS_PER_PIXEL);
    uint8_t* bits = reinterpret_cast<uint8_t*>(pixGetData(picture));

    while (!stop.load()) {
        if (!dup.CaptureNext(bits)) {
            fprintf(stderr, "Error: failed to capture frame.\n");
            usleep(10 * 1000);
            dup.Close();
            usleep(10 * 1000);
            dup.Initialize();
            continue;
        }

        timespec t1{};
        timespec t3{};
        clock_gettime(CLOCK_MONOTONIC, &t1);

        pixFunc(picture, sendBuf);

        int outSize = 0;
        int sendLen = BUF_LEN;
        uint8_t* compBuf = compress(
            optimize(sendBuf + 512, BUF_LEN - 512, 0, 1, COMPRESSION_TIMEOUT),
            sendBuf + 512,
            BUF_LEN - 512,
            0, 0, 1,
            &outSize);

        if (compBuf != nullptr) {
            if (outSize <= 60032) {
                sendLen = outSize + 512;
                if (sendLen % 64 != 0) {
                    sendLen += 64 - (sendLen % 64);
                }

                std::memcpy(sendBuf + 512, compBuf, outSize);
                std::memset(sendBuf + outSize + 512, 0, static_cast<size_t>(sendLen - outSize - 512));
            }

            std::free(compBuf);
        }

        if (!sendFrameToCalculator(devHandle, sendLen, sendBuf, stop)) {
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &t3);
        const float elapsedMs = static_cast<float>((t3.tv_sec - t1.tv_sec) * 1000.0 + (t3.tv_nsec - t1.tv_nsec) / 1e6);
        timeBuf[curBufIdx++] = elapsedMs;
        if (curBufIdx == STATS_BUF_SIZE) {
            curBufIdx = 0;
        }
    }

    int32_t endSignal = -1;
    sendFrameToCalculator(devHandle, endSignal, nullptr, stop);

    pixDestroy(&picture);
    std::free(sendBuf);
    dup.Close();
}

static void sendBridgeFrames(libusb_device_handle* devHandle, std::atomic<bool>& stop, BridgeClient* bridgeClient) {
    if (!bridgeClient) {
        fprintf(stderr, "Bridge mode requested without bridge client.\n");
        stop.store(true);
        return;
    }

    while (!stop.load()) {
        BridgeFrame frame{};
        if (!bridgeClient->receiveFrame(frame, stop)) {
            fprintf(stderr, "Bridge receive failed: %s\n", bridgeClient->lastError().c_str());
            stop.store(true);
            break;
        }

        if (!sendFrameToCalculator(devHandle, frame.calcPayloadSize, frame.bytes.empty() ? nullptr : frame.bytes.data(), stop)) {
            break;
        }
    }

    int32_t endSignal = -1;
    sendFrameToCalculator(devHandle, endSignal, nullptr, stop);
}

void sendThread(libusb_device_handle* devHandle, std::atomic<bool>& stop, bool bridgeMode, BridgeClient* bridgeClient) {
    running = true;

    if (bridgeMode) {
        sendBridgeFrames(devHandle, stop, bridgeClient);
    }
    else {
        sendDirectFrames(devHandle, stop);
    }

    running = false;
}
