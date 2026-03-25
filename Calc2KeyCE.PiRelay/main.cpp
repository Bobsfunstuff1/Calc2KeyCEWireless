#include <libusb-1.0/libusb.h>

#include "../Calc2KeyCE.Fast/bridge_client.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint16_t kCalcVid = 0x0451;
constexpr uint16_t kCalcPid = 0xE009;
constexpr unsigned char kCalcReadEndpoint = 0x81;
constexpr unsigned char kCalcWriteEndpoint = 0x02;
constexpr int kCalcInterface = 0;
constexpr int kUsbPollTimeoutMs = 10;
constexpr int kReconnectDelayMs = 1000;
constexpr std::size_t kCalcKeyBytes = 7;
constexpr int kFrameHeaderBytes = 64;
}

std::atomic<bool> gStop(false);

struct Options {
    std::string bridgeHost;
    uint16_t bridgePort = 28400;
};

void signalHandler(int) {
    gStop.store(true);
}

bool parseArgs(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bridge" && i + 1 < argc) {
            std::string value = argv[++i];
            const std::size_t colon = value.find(':');
            if (colon == std::string::npos) {
                options.bridgeHost = value;
            } else {
                options.bridgeHost = value.substr(0, colon);
                options.bridgePort = static_cast<uint16_t>(std::stoi(value.substr(colon + 1)));
            }
        } else if (arg == "--bridge-host" && i + 1 < argc) {
            options.bridgeHost = argv[++i];
        } else if (arg == "--bridge-port" && i + 1 < argc) {
            options.bridgePort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: Calc2KeyPiRelay --bridge <host[:port]>\n";
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    if (options.bridgeHost.empty()) {
        std::cerr << "Bridge host is required. Use --bridge <host[:port]>.\n";
        return false;
    }

    return true;
}

bool sendFrameToCalculator(libusb_device_handle* handle, const BridgeFrame& frame) {
    uint8_t sizeBuf[kFrameHeaderBytes] = {0};
    std::memcpy(sizeBuf, &frame.calcPayloadSize, sizeof(frame.calcPayloadSize));

    int sentLen = 0;
    if (libusb_bulk_transfer(handle, kCalcWriteEndpoint, sizeBuf, sizeof(sizeBuf), &sentLen, 0) != 0 || sentLen != sizeof(sizeBuf)) {
        std::cerr << "Failed to send frame size to calculator.\n";
        return false;
    }

    if (frame.calcPayloadSize <= 0) {
        return true;
    }

    if (frame.bytes.size() != static_cast<std::size_t>(frame.calcPayloadSize)) {
        std::cerr << "Bridge frame payload size mismatch.\n";
        return false;
    }

    for (int32_t offset = 0; offset < frame.calcPayloadSize; offset += 64) {
        if (libusb_bulk_transfer(
                handle,
                kCalcWriteEndpoint,
                const_cast<unsigned char*>(frame.bytes.data() + offset),
                64,
                &sentLen,
                0) != 0 || sentLen != 64) {
            std::cerr << "Failed while streaming frame payload to calculator.\n";
            return false;
        }
    }

    return true;
}

void receiveCalcKeysLoop(libusb_device_handle* handle, BridgeClient* client, std::atomic<bool>& sessionStop) {
    uint8_t keyBuf[kCalcKeyBytes] = {0};

    while (!gStop.load() && !sessionStop.load()) {
        int received = 0;
        const int rc = libusb_bulk_transfer(handle, kCalcReadEndpoint, keyBuf, sizeof(keyBuf), &received, kUsbPollTimeoutMs);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }

        if (rc != 0) {
            std::cerr << "USB read failed: " << libusb_error_name(rc) << "\n";
            sessionStop.store(true);
            break;
        }

        if (received != static_cast<int>(sizeof(keyBuf))) {
            continue;
        }

        if (!client->sendCalcKeys(keyBuf, sizeof(keyBuf))) {
            std::cerr << "Bridge send failed: " << client->lastError() << "\n";
            sessionStop.store(true);
            break;
        }
    }
}

void sendFramesLoop(libusb_device_handle* handle, BridgeClient* client, std::atomic<bool>& sessionStop) {
    std::mutex pendingMutex;
    std::condition_variable pendingCv;
    BridgeFrame pendingFrame{};
    bool hasPendingFrame = false;
    bool receiverDone = false;

    std::thread receiver([&]() {
        while (!gStop.load() && !sessionStop.load()) {
            BridgeFrame frame{};
            if (!client->receiveFrame(frame, sessionStop)) {
                if (!sessionStop.load()) {
                    std::cerr << "Bridge receive failed: " << client->lastError() << "\n";
                }
                sessionStop.store(true);
                break;
            }

            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                pendingFrame = std::move(frame);
                hasPendingFrame = true;
            }
            pendingCv.notify_one();
        }

        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            receiverDone = true;
        }
        pendingCv.notify_all();
    });

    while (!gStop.load() && !sessionStop.load()) {
        BridgeFrame frameToSend{};
        {
            std::unique_lock<std::mutex> lock(pendingMutex);
            pendingCv.wait(lock, [&]() {
                return hasPendingFrame || receiverDone || gStop.load() || sessionStop.load();
            });

            if (!hasPendingFrame) {
                if (receiverDone || gStop.load() || sessionStop.load()) {
                    break;
                }
                continue;
            }

            frameToSend = std::move(pendingFrame);
            hasPendingFrame = false;
        }

        if (!sendFrameToCalculator(handle, frameToSend)) {
            sessionStop.store(true);
            pendingCv.notify_all();
            break;
        }
    }

    if (receiver.joinable()) {
        receiver.join();
    }

    BridgeFrame endSignal{};
    endSignal.calcPayloadSize = -1;
    sendFrameToCalculator(handle, endSignal);
}

int runSession(libusb_context* usbContext, const Options& options) {
    std::cout << "Looking for calculator..." << std::endl;
    libusb_device_handle* handle = libusb_open_device_with_vid_pid(usbContext, kCalcVid, kCalcPid);
    if (!handle) {
        std::cout << "Calculator not found yet." << std::endl;
        return 1;
    }

    std::cout << "Calculator opened, claiming USB interface..." << std::endl;
    libusb_set_auto_detach_kernel_driver(handle, 1);
    const int claimRc = libusb_claim_interface(handle, kCalcInterface);
    if (claimRc != 0) {
        std::cerr << "Failed to claim calculator USB interface: " << libusb_error_name(claimRc) << "\n";
        libusb_close(handle);
        return 1;
    }

    std::cout << "USB interface claimed, connecting to bridge host..." << std::endl;
    BridgeClient client;
    if (!client.connectTo(options.bridgeHost, options.bridgePort)) {
        std::cerr << "Failed to connect to bridge host " << options.bridgeHost << ":" << options.bridgePort
                  << " - " << client.lastError() << "\n";
        libusb_release_interface(handle, kCalcInterface);
        libusb_close(handle);
        return 1;
    }

    std::cout << "Connected to calculator and bridge host " << options.bridgeHost << ":" << options.bridgePort << "\n";

    std::atomic<bool> sessionStop(false);
    std::thread recvThread(receiveCalcKeysLoop, handle, &client, std::ref(sessionStop));
    std::thread sendThread(sendFramesLoop, handle, &client, std::ref(sessionStop));

    recvThread.join();
    sendThread.join();

    client.close();
    libusb_release_interface(handle, kCalcInterface);
    libusb_close(handle);
    return 0;
}

int main(int argc, char* argv[]) {
    Options options;
    if (!parseArgs(argc, argv, options)) {
        return options.bridgeHost.empty() ? 1 : 0;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    libusb_context* usbContext = nullptr;
    const int initRc = libusb_init(&usbContext);
    if (initRc != 0) {
        std::cerr << "Failed to initialize libusb: " << libusb_error_name(initRc) << "\n";
        return 1;
    }

    while (!gStop.load()) {
        const int sessionRc = runSession(usbContext, options);
        if (gStop.load()) {
            break;
        }

        if (sessionRc != 0) {
            std::cout << "Waiting for calculator/bridge and retrying...\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
    }

    libusb_exit(usbContext);
    return 0;
}
