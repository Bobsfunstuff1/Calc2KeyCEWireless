#include "bridge_protocol.h"

#include <cstring>

namespace bridge {
namespace {

template <typename T>
void appendScalar(std::vector<uint8_t>& out, T value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool readScalar(const uint8_t*& cur, const uint8_t* end, T& value) {
    if (static_cast<std::size_t>(end - cur) < sizeof(T)) {
        return false;
    }

    std::memcpy(&value, cur, sizeof(T));
    cur += sizeof(T);
    return true;
}

} // namespace

std::vector<uint8_t> serializeHeader(const PacketHeader& header) {
    std::vector<uint8_t> out;
    out.reserve(sizeof(PacketHeader));
    appendScalar(out, header.magic);
    appendScalar(out, header.version);
    appendScalar(out, static_cast<uint16_t>(header.type));
    appendScalar(out, header.payloadSize);
    return out;
}

bool deserializeHeader(const uint8_t* data, std::size_t size, PacketHeader& header) {
    if (!data) {
        return false;
    }

    const uint8_t* cur = data;
    const uint8_t* end = data + size;
    uint16_t rawType = 0;

    if (!readScalar(cur, end, header.magic) ||
        !readScalar(cur, end, header.version) ||
        !readScalar(cur, end, rawType) ||
        !readScalar(cur, end, header.payloadSize)) {
        return false;
    }

    if (header.magic != kMagic) {
        return false;
    }

    header.type = static_cast<MessageType>(rawType);
    return true;
}

std::vector<uint8_t> serializeHello(const HelloPayload& payload) {
    std::vector<uint8_t> out;
    out.reserve(sizeof(HelloPayload));
    appendScalar(out, payload.protocolVersion);
    appendScalar(out, payload.flags);
    return out;
}

bool deserializeHello(const uint8_t* data, std::size_t size, HelloPayload& payload) {
    if (!data || size != sizeof(HelloPayload)) {
        return false;
    }

    const uint8_t* cur = data;
    const uint8_t* end = data + size;
    return readScalar(cur, end, payload.protocolVersion) &&
        readScalar(cur, end, payload.flags);
}

std::vector<uint8_t> serializeCalcKeys(const CalcKeysPayload& payload) {
    return std::vector<uint8_t>(payload.keyMatrix.begin(), payload.keyMatrix.end());
}

bool deserializeCalcKeys(const uint8_t* data, std::size_t size, CalcKeysPayload& payload) {
    if (!data || size != kCalcKeyBytes) {
        return false;
    }

    std::memcpy(payload.keyMatrix.data(), data, kCalcKeyBytes);
    return true;
}

std::vector<uint8_t> serializeFrameStart(const FrameStartPayload& payload) {
    std::vector<uint8_t> out;
    out.reserve(sizeof(FrameStartPayload));
    appendScalar(out, payload.frameId);
    appendScalar(out, payload.calcPayloadSize);
    return out;
}

bool deserializeFrameStart(const uint8_t* data, std::size_t size, FrameStartPayload& payload) {
    if (!data || size != sizeof(FrameStartPayload)) {
        return false;
    }

    const uint8_t* cur = data;
    const uint8_t* end = data + size;
    return readScalar(cur, end, payload.frameId) &&
        readScalar(cur, end, payload.calcPayloadSize);
}

std::vector<uint8_t> serializeFrameChunk(const FrameChunkPayload& payload) {
    std::vector<uint8_t> out;
    out.reserve(sizeof(payload.frameId) + sizeof(payload.offset) + payload.bytes.size());
    appendScalar(out, payload.frameId);
    appendScalar(out, payload.offset);
    out.insert(out.end(), payload.bytes.begin(), payload.bytes.end());
    return out;
}

bool deserializeFrameChunk(const uint8_t* data, std::size_t size, FrameChunkPayload& payload) {
    if (!data || size < sizeof(payload.frameId) + sizeof(payload.offset)) {
        return false;
    }

    const uint8_t* cur = data;
    const uint8_t* end = data + size;
    if (!readScalar(cur, end, payload.frameId) ||
        !readScalar(cur, end, payload.offset)) {
        return false;
    }

    payload.bytes.assign(cur, end);
    return true;
}

std::vector<uint8_t> serializeFrameEnd(const FrameEndPayload& payload) {
    std::vector<uint8_t> out;
    out.reserve(sizeof(FrameEndPayload));
    appendScalar(out, payload.frameId);
    return out;
}

bool deserializeFrameEnd(const uint8_t* data, std::size_t size, FrameEndPayload& payload) {
    if (!data || size != sizeof(FrameEndPayload)) {
        return false;
    }

    const uint8_t* cur = data;
    const uint8_t* end = data + size;
    return readScalar(cur, end, payload.frameId);
}

} // namespace bridge
