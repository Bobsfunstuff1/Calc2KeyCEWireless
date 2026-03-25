#include "bridge_client.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
constexpr std::size_t kHeaderSize = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t);
constexpr int kSocketTimeoutMs = 250;
constexpr std::array<uint8_t, 8> kStrayFrameEndPrefix = { 0x01, 0x00, 0x05, 0x00, 0x04, 0x00, 0x00, 0x00 };

std::string errnoMessage(const std::string& prefix) {
    return prefix + ": " + std::strerror(errno);
}

bool looksLikeStrayFrameEnd(const uint8_t* data, std::size_t size) {
    if (!data || size < kHeaderSize) {
        return false;
    }

    return std::equal(kStrayFrameEndPrefix.begin(), kStrayFrameEndPrefix.end(), data);
}

bool recvHeaderResync(int socketFd, uint8_t* headerBuffer, std::atomic<bool>& stop) {
    if (!headerBuffer) {
        return false;
    }

    std::size_t filled = 0;
    while (filled < kHeaderSize && !stop.load()) {
        const ssize_t rc = ::recv(socketFd, reinterpret_cast<char*>(headerBuffer + filled), static_cast<int>(kHeaderSize - filled), 0);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            return false;
        }
        filled += static_cast<std::size_t>(rc);
    }

    while (!stop.load()) {
        bridge::PacketHeader probe{};
        if (bridge::deserializeHeader(headerBuffer, kHeaderSize, probe)) {
            return true;
        }

        for (std::size_t i = 0; i + 4 <= kHeaderSize; ++i) {
            uint32_t possibleMagic = 0;
            std::memcpy(&possibleMagic, headerBuffer + i, sizeof(uint32_t));
            if (possibleMagic == bridge::kMagic) {
                const std::size_t keep = kHeaderSize - i;
                std::memmove(headerBuffer, headerBuffer + i, keep);
                filled = keep;
                while (filled < kHeaderSize && !stop.load()) {
                    const ssize_t rc = ::recv(socketFd, reinterpret_cast<char*>(headerBuffer + filled), static_cast<int>(kHeaderSize - filled), 0);
                    if (rc < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                            continue;
                        }
                        return false;
                    }
                    if (rc == 0) {
                        return false;
                    }
                    filled += static_cast<std::size_t>(rc);
                }
                break;
            }
        }

        bridge::PacketHeader retry{};
        if (bridge::deserializeHeader(headerBuffer, kHeaderSize, retry)) {
            return true;
        }

        std::memmove(headerBuffer, headerBuffer + 1, kHeaderSize - 1);
        const ssize_t rc = ::recv(socketFd, reinterpret_cast<char*>(headerBuffer + kHeaderSize - 1), 1, 0);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            return false;
        }
    }

    return false;
}
}

BridgeClient::BridgeClient() : socketFd(-1), connected(false) {}

BridgeClient::~BridgeClient() {
    close();
}

bool BridgeClient::connectTo(const std::string& host, uint16_t port) {
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string portString = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), portString.c_str(), &hints, &result);
    if (rc != 0) {
        setError(gai_strerror(rc));
        return false;
    }

    bool didConnect = false;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        socketFd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (socketFd < 0) {
            continue;
        }

        if (::connect(socketFd, it->ai_addr, it->ai_addrlen) == 0) {
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = kSocketTimeoutMs * 1000;
            setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            didConnect = true;
            break;
        }

        ::close(socketFd);
        socketFd = -1;
    }

    freeaddrinfo(result);

    if (!didConnect) {
        setError(errnoMessage("connect failed"));
        return false;
    }

    connected.store(true);
    errorMessage.clear();
    return sendHello();
}

void BridgeClient::close() {
    connected.store(false);
    if (socketFd >= 0) {
        ::shutdown(socketFd, SHUT_RDWR);
        ::close(socketFd);
        socketFd = -1;
    }
}

bool BridgeClient::isConnected() const {
    return connected.load();
}

std::string BridgeClient::lastError() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return errorMessage;
}

bool BridgeClient::sendHello(uint16_t flags) {
    bridge::HelloPayload hello{};
    hello.protocolVersion = bridge::kVersion;
    hello.flags = flags;
    return sendPacket(bridge::MessageType::Hello, bridge::serializeHello(hello));
}

bool BridgeClient::sendCalcKeys(const uint8_t* data, std::size_t len) {
    if (!data || len != bridge::kCalcKeyBytes) {
        setError("invalid calc key packet size");
        return false;
    }

    bridge::CalcKeysPayload payload{};
    std::copy(data, data + len, payload.keyMatrix.begin());
    return sendPacket(bridge::MessageType::CalcKeys, bridge::serializeCalcKeys(payload));
}

bool BridgeClient::receiveFrame(BridgeFrame& frame, std::atomic<bool>& stop) {
    frame = {};

    uint32_t activeFrameId = 0;
    bool haveFrame = false;

    while (!stop.load()) {
        uint8_t headerBuffer[kHeaderSize] = { 0 };
        if (!recvHeaderResync(socketFd, headerBuffer, stop)) {
            connected.store(false);
            setError(errnoMessage("recv failed"));
            return false;
        }

        bridge::PacketHeader header{};
        if (!bridge::deserializeHeader(headerBuffer, sizeof(headerBuffer), header)) {
            if (looksLikeStrayFrameEnd(headerBuffer, sizeof(headerBuffer))) {
                // Some sessions still surface a trailing FrameEnd remainder without
                // the 4-byte magic prefix. Ignore it and keep reading from the next
                // packet boundary instead of tearing the bridge down.
                continue;
            }
            std::fprintf(stderr, "Invalid bridge header bytes:");
            for (std::size_t i = 0; i < sizeof(headerBuffer); ++i) {
                std::fprintf(stderr, " %02X", headerBuffer[i]);
            }
            std::fprintf(stderr, "\n");
            setError("invalid bridge header");
            return false;
        }

        std::vector<uint8_t> payload(header.payloadSize);
        if (header.payloadSize > 0 && !recvAll(payload.data(), payload.size(), stop)) {
            return false;
        }

        switch (header.type) {
        case bridge::MessageType::Hello:
        case bridge::MessageType::Heartbeat:
            break;
        case bridge::MessageType::FrameStart: {
            bridge::FrameStartPayload start{};
            if (!bridge::deserializeFrameStart(payload.data(), payload.size(), start)) {
                setError("invalid frame start payload");
                return false;
            }

            frame = {};
            frame.calcPayloadSize = start.calcPayloadSize;
            activeFrameId = start.frameId;
            haveFrame = true;
            if (frame.calcPayloadSize > 0) {
                frame.bytes.resize(static_cast<std::size_t>(frame.calcPayloadSize));
            }
            else if (frame.calcPayloadSize < 0) {
                return true;
            }
            break;
        }
        case bridge::MessageType::FrameChunk: {
            if (!haveFrame) {
                break;
            }

            bridge::FrameChunkPayload chunk{};
            if (!bridge::deserializeFrameChunk(payload.data(), payload.size(), chunk)) {
                setError("invalid frame chunk payload");
                return false;
            }

            if (chunk.frameId != activeFrameId || frame.calcPayloadSize < 0) {
                break;
            }

            const std::size_t offset = static_cast<std::size_t>(chunk.offset);
            const std::size_t end = offset + chunk.bytes.size();
            if (end > frame.bytes.size()) {
                setError("frame chunk overflow");
                return false;
            }

            std::copy(chunk.bytes.begin(), chunk.bytes.end(), frame.bytes.begin() + static_cast<std::ptrdiff_t>(offset));

            break;
        }
        case bridge::MessageType::FrameEnd: {
            if (!haveFrame) {
                break;
            }

            bridge::FrameEndPayload end{};
            if (!bridge::deserializeFrameEnd(payload.data(), payload.size(), end)) {
                setError("invalid frame end payload");
                return false;
            }

            if (end.frameId == activeFrameId) {
                return true;
            }
            break;
        }
        default:
            break;
        }
    }

    return false;
}

bool BridgeClient::sendPacket(bridge::MessageType type, const std::vector<uint8_t>& payload) {
    if (socketFd < 0) {
        setError("bridge socket is not connected");
        return false;
    }

    bridge::PacketHeader header{};
    header.type = type;
    header.payloadSize = static_cast<uint32_t>(payload.size());
    const std::vector<uint8_t> serializedHeader = bridge::serializeHeader(header);

    std::lock_guard<std::mutex> lock(sendMutex);
    if (!sendAll(serializedHeader.data(), serializedHeader.size())) {
        return false;
    }

    if (!payload.empty() && !sendAll(payload.data(), payload.size())) {
        return false;
    }

    return true;
}

bool BridgeClient::sendAll(const uint8_t* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t rc = ::send(socketFd, data + sent, len - sent, 0);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }

            connected.store(false);
            setError(errnoMessage("send failed"));
            return false;
        }

        if (rc == 0) {
            connected.store(false);
            setError("bridge connection closed during send");
            return false;
        }

        sent += static_cast<std::size_t>(rc);
    }

    return true;
}

bool BridgeClient::recvAll(uint8_t* data, std::size_t len, std::atomic<bool>& stop) {
    std::size_t received = 0;
    while (received < len && !stop.load()) {
        const ssize_t rc = ::recv(socketFd, data + received, len - received, 0);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }

            connected.store(false);
            setError(errnoMessage("recv failed"));
            return false;
        }

        if (rc == 0) {
            connected.store(false);
            setError("bridge connection closed during recv");
            return false;
        }

        received += static_cast<std::size_t>(rc);
    }

    return received == len;
}

void BridgeClient::setError(const std::string& message) {
    std::lock_guard<std::mutex> lock(stateMutex);
    errorMessage = message;
}
