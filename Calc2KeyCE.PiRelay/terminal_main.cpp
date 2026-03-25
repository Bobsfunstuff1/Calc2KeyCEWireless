#include <libusb-1.0/libusb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <pty.h>
#include <string>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint16_t kCalcVid = 0x0451;
constexpr uint16_t kCalcPid = 0xE00A;
constexpr unsigned char kCalcReadEndpoint = 0x81;
constexpr unsigned char kCalcWriteEndpoint = 0x02;
constexpr int kCalcInterface = 0;
constexpr int kReconnectDelayMs = 1000;
constexpr int kUsbPollTimeoutMs = 50;
constexpr int kScreenCols = 40;
constexpr int kScreenRows = 24;
constexpr int kScreenBytes = kScreenCols * kScreenRows;
constexpr int kPacketHeaderBytes = 8;
constexpr uint8_t kPacketTypeScreen = 1;
constexpr uint8_t kPacketTypeInput = 2;

struct PacketHeader {
    uint8_t type = 0;
    uint8_t reserved0 = 0;
    uint8_t reserved1 = 0;
    uint8_t reserved2 = 0;
    uint32_t size = 0;
};

struct Options {
    std::string shell = "/bin/bash";
};

struct ScreenState {
    std::vector<char> chars = std::vector<char>(kScreenBytes, ' ');
    int row = 0;
    int col = 0;
    int savedRow = 0;
    int savedCol = 0;
    bool dirty = true;
};

std::atomic<bool> gStop(false);

void signalHandler(int) {
    gStop.store(true);
}

bool parseArgs(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shell" && i + 1 < argc) {
            options.shell = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: Calc2PiConsoleRelay [--shell /bin/bash]\n";
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

void clampCursor(ScreenState& screen) {
    if (screen.row < 0) screen.row = 0;
    if (screen.row >= kScreenRows) screen.row = kScreenRows - 1;
    if (screen.col < 0) screen.col = 0;
    if (screen.col >= kScreenCols) screen.col = kScreenCols - 1;
}

void scrollUp(ScreenState& screen, int lines = 1) {
    if (lines <= 0) return;
    if (lines >= kScreenRows) {
        std::fill(screen.chars.begin(), screen.chars.end(), ' ');
    } else {
        std::memmove(screen.chars.data(), screen.chars.data() + lines * kScreenCols, kScreenBytes - lines * kScreenCols);
        std::fill(screen.chars.end() - lines * kScreenCols, screen.chars.end(), ' ');
    }
    screen.row = kScreenRows - 1;
    screen.col = 0;
    screen.dirty = true;
}

void putChar(ScreenState& screen, char ch) {
    if (ch >= 32 && ch <= 126) {
        screen.chars[screen.row * kScreenCols + screen.col] = ch;
        screen.dirty = true;
        ++screen.col;
        if (screen.col >= kScreenCols) {
            screen.col = 0;
            ++screen.row;
            if (screen.row >= kScreenRows) {
                scrollUp(screen);
            }
        }
        return;
    }

    switch (ch) {
        case '\r':
            screen.col = 0;
            break;
        case '\n':
            ++screen.row;
            if (screen.row >= kScreenRows) {
                scrollUp(screen);
            }
            break;
        case '\b':
        case 0x7F:
            if (screen.col > 0) {
                --screen.col;
                screen.chars[screen.row * kScreenCols + screen.col] = ' ';
                screen.dirty = true;
            }
            break;
        case '\t':
            do {
                putChar(screen, ' ');
            } while ((screen.col % 4) != 0);
            break;
        default:
            break;
    }
}

int parseNumber(const std::string& seq, std::size_t& pos, int defaultValue) {
    int value = 0;
    bool found = false;
    while (pos < seq.size() && seq[pos] >= '0' && seq[pos] <= '9') {
        value = value * 10 + (seq[pos] - '0');
        ++pos;
        found = true;
    }
    return found ? value : defaultValue;
}

void clearLineFrom(ScreenState& screen, int col) {
    if (col < 0) col = 0;
    if (col >= kScreenCols) return;
    std::fill(screen.chars.begin() + screen.row * kScreenCols + col,
              screen.chars.begin() + (screen.row + 1) * kScreenCols,
              ' ');
    screen.dirty = true;
}

void clearLineTo(ScreenState& screen, int col) {
    if (col < 0) return;
    if (col >= kScreenCols) col = kScreenCols - 1;
    std::fill(screen.chars.begin() + screen.row * kScreenCols,
              screen.chars.begin() + screen.row * kScreenCols + col + 1,
              ' ');
    screen.dirty = true;
}

void clearScreenFrom(ScreenState& screen, int row, int col) {
    int start = row * kScreenCols + col;
    if (start < 0) start = 0;
    if (start >= kScreenBytes) return;
    std::fill(screen.chars.begin() + start, screen.chars.end(), ' ');
    screen.dirty = true;
}

void clearScreenTo(ScreenState& screen, int row, int col) {
    int end = row * kScreenCols + col;
    if (end < 0) return;
    if (end >= kScreenBytes) end = kScreenBytes - 1;
    std::fill(screen.chars.begin(), screen.chars.begin() + end + 1, ' ');
    screen.dirty = true;
}

void applyCsi(ScreenState& screen, const std::string& seq) {
    std::size_t pos = 0;
    std::vector<int> args;
    const char command = seq.empty() ? '\0' : seq.back();

    while (pos + 1 < seq.size()) {
        if (seq[pos] == '?') {
            ++pos;
            continue;
        }
        args.push_back(parseNumber(seq, pos, 0));
        if (pos + 1 < seq.size() && seq[pos] == ';') {
            ++pos;
        } else {
            break;
        }
    }

    switch (command) {
        case 'A': screen.row -= args.empty() || args[0] == 0 ? 1 : args[0]; break;
        case 'B': screen.row += args.empty() || args[0] == 0 ? 1 : args[0]; break;
        case 'C': screen.col += args.empty() || args[0] == 0 ? 1 : args[0]; break;
        case 'D': screen.col -= args.empty() || args[0] == 0 ? 1 : args[0]; break;
        case 'G': screen.col = (args.empty() ? 1 : args[0]) - 1; break;
        case 'H':
        case 'f':
            screen.row = (args.size() > 0 && args[0] > 0 ? args[0] : 1) - 1;
            screen.col = (args.size() > 1 && args[1] > 0 ? args[1] : 1) - 1;
            break;
        case 'J': {
            const int mode = args.empty() ? 0 : args[0];
            if (mode == 0) clearScreenFrom(screen, screen.row, screen.col);
            else if (mode == 1) clearScreenTo(screen, screen.row, screen.col);
            else if (mode == 2) {
                std::fill(screen.chars.begin(), screen.chars.end(), ' ');
                screen.dirty = true;
            }
            break;
        }
        case 'K': {
            const int mode = args.empty() ? 0 : args[0];
            if (mode == 0) clearLineFrom(screen, screen.col);
            else if (mode == 1) clearLineTo(screen, screen.col);
            else if (mode == 2) clearLineFrom(screen, 0);
            break;
        }
        case 's':
            screen.savedRow = screen.row;
            screen.savedCol = screen.col;
            break;
        case 'u':
            screen.row = screen.savedRow;
            screen.col = screen.savedCol;
            break;
        case 'm':
            break;
        default:
            break;
    }

    clampCursor(screen);
}

void applyOutput(ScreenState& screen, const uint8_t* bytes, std::size_t size) {
    enum class State { Normal, Esc, Csi };
    static State state = State::Normal;
    static std::string csi;

    for (std::size_t i = 0; i < size; ++i) {
        const char ch = static_cast<char>(bytes[i]);
        switch (state) {
            case State::Normal:
                if (ch == 0x1B) state = State::Esc;
                else putChar(screen, ch);
                break;
            case State::Esc:
                if (ch == '[') {
                    csi.clear();
                    state = State::Csi;
                } else {
                    state = State::Normal;
                }
                break;
            case State::Csi:
                csi.push_back(ch);
                if ((ch >= '@' && ch <= '~') || csi.size() > 32) {
                    applyCsi(screen, csi);
                    state = State::Normal;
                }
                break;
        }
    }
}

bool sendAll(libusb_device_handle* handle, unsigned char endpoint, const uint8_t* data, int size) {
    int offset = 0;
    while (offset < size && !gStop.load()) {
        int transferred = 0;
        const int rc = libusb_bulk_transfer(handle, endpoint, const_cast<unsigned char*>(data + offset), size - offset, &transferred, 0);
        if (rc != 0 || transferred <= 0) {
            return false;
        }
        offset += transferred;
    }
    return offset == size;
}

bool receiveAll(libusb_device_handle* handle, unsigned char endpoint, uint8_t* data, int size, int timeoutMs) {
    int offset = 0;
    while (offset < size && !gStop.load()) {
        int transferred = 0;
        const int rc = libusb_bulk_transfer(handle, endpoint, data + offset, size - offset, &transferred, timeoutMs);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            return false;
        }
        if (rc != 0 || transferred <= 0) {
            return false;
        }
        offset += transferred;
    }
    return offset == size;
}

bool sendScreen(libusb_device_handle* handle, const ScreenState& screen) {
    std::vector<uint8_t> packet(kPacketHeaderBytes + kScreenBytes + 2, 0);
    auto* header = reinterpret_cast<PacketHeader*>(packet.data());
    header->type = kPacketTypeScreen;
    header->size = kScreenBytes + 2;
    std::memcpy(packet.data() + kPacketHeaderBytes, screen.chars.data(), kScreenBytes);
    packet[kPacketHeaderBytes + kScreenBytes] = static_cast<uint8_t>(screen.col);
    packet[kPacketHeaderBytes + kScreenBytes + 1] = static_cast<uint8_t>(screen.row);
    return sendAll(handle, kCalcWriteEndpoint, packet.data(), static_cast<int>(packet.size()));
}

struct PtySession {
    int masterFd = -1;
    pid_t childPid = -1;

    bool start(const std::string& shellPath) {
        struct winsize winsz{};
        winsz.ws_col = kScreenCols;
        winsz.ws_row = kScreenRows;
        childPid = forkpty(&masterFd, nullptr, nullptr, &winsz);
        if (childPid < 0) {
            return false;
        }
        if (childPid == 0) {
            setenv("TERM", "vt100", 1);
            setenv("COLUMNS", "40", 1);
            setenv("LINES", "24", 1);
            execl(shellPath.c_str(), shellPath.c_str(), "-i", static_cast<char*>(nullptr));
            _exit(127);
        }
        const int flags = fcntl(masterFd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
        }
        return true;
    }

    void stop() {
        if (masterFd >= 0) {
            close(masterFd);
            masterFd = -1;
        }
        if (childPid > 0) {
            kill(childPid, SIGTERM);
            waitpid(childPid, nullptr, 0);
            childPid = -1;
        }
    }
};

void receiveInputLoop(libusb_device_handle* handle, int ptyFd, std::atomic<bool>& sessionStop) {
    uint8_t headerBytes[kPacketHeaderBytes] = {};
    std::vector<uint8_t> payload;

    while (!gStop.load() && !sessionStop.load()) {
        int transferred = 0;
        const int rc = libusb_bulk_transfer(handle, kCalcReadEndpoint, headerBytes, sizeof(headerBytes), &transferred, kUsbPollTimeoutMs);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }
        if (rc != 0) {
            sessionStop.store(true);
            break;
        }
        if (transferred != kPacketHeaderBytes) {
            continue;
        }

        PacketHeader header{};
        std::memcpy(&header, headerBytes, sizeof(header));
        if (header.type != kPacketTypeInput || header.size == 0 || header.size > 64) {
            continue;
        }

        payload.resize(header.size);
        if (!receiveAll(handle, kCalcReadEndpoint, payload.data(), static_cast<int>(payload.size()), kUsbPollTimeoutMs)) {
            sessionStop.store(true);
            break;
        }

        if (!payload.empty() && write(ptyFd, payload.data(), payload.size()) < 0) {
            sessionStop.store(true);
            break;
        }
    }
}

void ptyLoop(libusb_device_handle* handle, int ptyFd, std::atomic<bool>& sessionStop) {
    ScreenState screen;
    std::array<uint8_t, 512> buffer{};
    auto lastSend = std::chrono::steady_clock::now();

    sendScreen(handle, screen);

    while (!gStop.load() && !sessionStop.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(ptyFd, &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000;

        const int ready = select(ptyFd + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready < 0) {
            sessionStop.store(true);
            break;
        }

        if (ready > 0 && FD_ISSET(ptyFd, &readSet)) {
            const ssize_t readCount = read(ptyFd, buffer.data(), buffer.size());
            if (readCount <= 0) {
                sessionStop.store(true);
                break;
            }
            applyOutput(screen, buffer.data(), static_cast<std::size_t>(readCount));
        }

        const auto now = std::chrono::steady_clock::now();
        if (screen.dirty || now - lastSend > std::chrono::milliseconds(250)) {
            if (!sendScreen(handle, screen)) {
                sessionStop.store(true);
                break;
            }
            screen.dirty = false;
            lastSend = now;
        }
    }
}

int runSession(libusb_context* usbContext, const Options& options) {
    libusb_device_handle* handle = libusb_open_device_with_vid_pid(usbContext, kCalcVid, kCalcPid);
    if (!handle) {
        return 1;
    }

    libusb_set_auto_detach_kernel_driver(handle, 1);
    if (libusb_claim_interface(handle, kCalcInterface) != 0) {
        libusb_close(handle);
        return 1;
    }

    PtySession pty;
    if (!pty.start(options.shell)) {
        libusb_release_interface(handle, kCalcInterface);
        libusb_close(handle);
        return 1;
    }

    std::atomic<bool> sessionStop(false);
    std::thread inputThread(receiveInputLoop, handle, pty.masterFd, std::ref(sessionStop));
    std::thread outputThread(ptyLoop, handle, pty.masterFd, std::ref(sessionStop));

    inputThread.join();
    outputThread.join();

    pty.stop();
    libusb_release_interface(handle, kCalcInterface);
    libusb_close(handle);
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parseArgs(argc, argv, options)) {
        return 0;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    libusb_context* usbContext = nullptr;
    if (libusb_init(&usbContext) != 0) {
        return 1;
    }

    while (!gStop.load()) {
        runSession(usbContext, options);
        if (gStop.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
    }

    libusb_exit(usbContext);
    return 0;
}
