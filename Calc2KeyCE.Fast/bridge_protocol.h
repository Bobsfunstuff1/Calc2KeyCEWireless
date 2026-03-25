#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bridge {

constexpr uint32_t kMagic = 0x43324B42;
constexpr uint16_t kVersion = 1;
constexpr std::size_t kCalcKeyBytes = 7;

enum class MessageType : uint16_t {
    Hello = 1,
    CalcKeys = 2,
    FrameStart = 3,
    FrameChunk = 4,
    FrameEnd = 5,
    Heartbeat = 6,
};

struct PacketHeader {
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    MessageType type = MessageType::Hello;
    uint32_t payloadSize = 0;
};

struct HelloPayload {
    uint16_t protocolVersion = kVersion;
    uint16_t flags = 0;
};

struct CalcKeysPayload {
    std::array<uint8_t, kCalcKeyBytes> keyMatrix{};
};

struct FrameStartPayload {
    uint32_t frameId = 0;
    int32_t calcPayloadSize = 0;
};

struct FrameChunkPayload {
    uint32_t frameId = 0;
    uint32_t offset = 0;
    std::vector<uint8_t> bytes;
};

struct FrameEndPayload {
    uint32_t frameId = 0;
};

std::vector<uint8_t> serializeHeader(const PacketHeader& header);
bool deserializeHeader(const uint8_t* data, std::size_t size, PacketHeader& header);

std::vector<uint8_t> serializeHello(const HelloPayload& payload);
bool deserializeHello(const uint8_t* data, std::size_t size, HelloPayload& payload);

std::vector<uint8_t> serializeCalcKeys(const CalcKeysPayload& payload);
bool deserializeCalcKeys(const uint8_t* data, std::size_t size, CalcKeysPayload& payload);

std::vector<uint8_t> serializeFrameStart(const FrameStartPayload& payload);
bool deserializeFrameStart(const uint8_t* data, std::size_t size, FrameStartPayload& payload);

std::vector<uint8_t> serializeFrameChunk(const FrameChunkPayload& payload);
bool deserializeFrameChunk(const uint8_t* data, std::size_t size, FrameChunkPayload& payload);

std::vector<uint8_t> serializeFrameEnd(const FrameEndPayload& payload);
bool deserializeFrameEnd(const uint8_t* data, std::size_t size, FrameEndPayload& payload);

} // namespace bridge
