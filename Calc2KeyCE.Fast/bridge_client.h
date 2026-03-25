#pragma once

#include "bridge_protocol.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct BridgeFrame {
    int32_t calcPayloadSize = 0;
    std::vector<uint8_t> bytes;
};

class BridgeClient {
public:
    BridgeClient();
    ~BridgeClient();

    bool connectTo(const std::string& host, uint16_t port);
    void close();

    bool isConnected() const;
    std::string lastError() const;

    bool sendHello(uint16_t flags = 0);
    bool sendCalcKeys(const uint8_t* data, std::size_t len);
    bool receiveFrame(BridgeFrame& frame, std::atomic<bool>& stop);

private:
    bool sendPacket(bridge::MessageType type, const std::vector<uint8_t>& payload);
    bool sendAll(const uint8_t* data, std::size_t len);
    bool recvAll(uint8_t* data, std::size_t len, std::atomic<bool>& stop);
    void setError(const std::string& message);

    int socketFd;
    std::atomic<bool> connected;
    mutable std::mutex stateMutex;
    std::mutex sendMutex;
    std::string errorMessage;
};
