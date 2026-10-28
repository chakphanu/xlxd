#ifndef XLXD_PCM_PROTOCOL_H
#define XLXD_PCM_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pcmwire {

static const uint8_t PcmCodec = 5;
static const uint32_t FormatMask = 0x3e;
static const size_t MaxOutputs = 4;
static const size_t MaxPayload = 320;
static const size_t MaxReply = 354;

inline size_t width(uint8_t codec)
{
    switch (codec) {
    case 1: case 2: return 9;
    case 3: return 8;
    case 4: return 11;
    case 5: return 320;
    default: return 0;
    }
}

inline uint32_t load32(const uint8_t *p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
        (uint32_t(p[2]) << 8) | p[3];
}

inline void store32(uint8_t *p, uint32_t value)
{
    p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
}

inline bool tagged(const uint8_t *p, size_t n, const char *tag, size_t length)
{
    return p && n >= length && std::memcmp(p,tag,length) == 0;
}

struct Capabilities {
    uint8_t maxOutputs = 0;
    uint32_t mask = 0;
};

inline bool parseCapabilities(const uint8_t *p, size_t n, Capabilities &out)
{
    if (n != 15 || !tagged(p,n,"AMBEDCAPS",9) || p[9] != 1 ||
        p[10] == 0 || p[10] > MaxOutputs) return false;
    const uint32_t mask = load32(p+11) & FormatMask;
    if (!mask) return false;
    out.maxOutputs = p[10]; out.mask = mask;
    return true;
}

inline std::vector<uint8_t> encodeCapabilities(uint32_t mask)
{
    std::vector<uint8_t> bytes(15);
    std::memcpy(bytes.data(),"AMBEDCAPS",9);
    bytes[9] = 1; bytes[10] = uint8_t(MaxOutputs);
    store32(bytes.data()+11,mask & FormatMask);
    return bytes;
}

struct Open {
    std::array<uint8_t,8> callsign = {{0}};
    uint8_t input = 0;
    std::array<uint8_t,4> outputs = {{0}};
    size_t count = 0;
    uint32_t transaction = 0;
};

inline bool validOutputs(const uint8_t *codecs, size_t count, uint8_t input = 0)
{
    if (!codecs || count == 0 || count > MaxOutputs) return false;
    uint32_t seen = 0;
    for (size_t i=0; i<count; ++i) {
        const uint8_t c = codecs[i];
        if (!width(c) || c == input || (seen & (1U << c))) return false;
        seen |= 1U << c;
    }
    return true;
}

inline bool parseOpen(const uint8_t *p, size_t n, Open &out)
{
    if (n != 24 || !tagged(p,n,"AMBEDOS",7) || !width(p[15])) return false;
    size_t count = 0;
    while (count < MaxOutputs && p[16+count]) ++count;
    for (size_t i=count; i<MaxOutputs; ++i) if (p[16+i]) return false;
    if (!validOutputs(p+16,count,p[15])) return false;
    Open next;
    std::memcpy(next.callsign.data(),p+7,8);
    next.input = p[15]; next.count = count;
    std::memcpy(next.outputs.data(),p+16,4);
    next.transaction = load32(p+20);
    out = next;
    return true;
}

inline std::vector<uint8_t> encodeOpen(const Open &open)
{
    if (!width(open.input) || !validOutputs(open.outputs.data(),open.count,open.input))
        return std::vector<uint8_t>();
    std::vector<uint8_t> bytes(24,0);
    std::memcpy(bytes.data(),"AMBEDOS",7);
    std::memcpy(bytes.data()+7,open.callsign.data(),8);
    bytes[15] = open.input;
    std::memcpy(bytes.data()+16,open.outputs.data(),open.count);
    store32(bytes.data()+20,open.transaction);
    return bytes;
}

inline std::vector<uint8_t> encodeReply(uint8_t pid, const uint8_t *codecs,
    const std::vector<uint8_t> *frames, size_t count)
{
    if (!frames || !validOutputs(codecs,count)) return std::vector<uint8_t>();
    size_t total = 1;
    for (size_t i=0; i<count; ++i) {
        if (frames[i].size() != width(codecs[i])) return std::vector<uint8_t>();
        total += 1+frames[i].size();
    }
    if (total > MaxReply) return std::vector<uint8_t>();
    std::vector<uint8_t> bytes;
    bytes.reserve(total);
    for (size_t i=0; i<count; ++i) {
        bytes.push_back(codecs[i]);
        if (i == 0) bytes.push_back(pid);
        bytes.insert(bytes.end(),frames[i].begin(),frames[i].end());
    }
    return bytes;
}

inline bool parseReply(const uint8_t *p, size_t n, const uint8_t *codecs,
    size_t count, uint8_t &pid, std::vector<std::vector<uint8_t> > &frames)
{
    if (!p || !validOutputs(codecs,count)) return false;
    size_t expected = 1;
    for (size_t i=0; i<count; ++i) expected += 1+width(codecs[i]);
    if (n != expected) return false;
    std::vector<std::vector<uint8_t> > next;
    size_t offset = 0;
    for (size_t i=0; i<count; ++i) {
        if (p[offset++] != codecs[i]) return false;
        if (i == 0) ++offset;
        const size_t length = width(codecs[i]);
        next.push_back(std::vector<uint8_t>(p+offset,p+offset+length));
        offset += length;
    }
    pid = p[1]; frames.swap(next);
    return true;
}

inline std::vector<uint8_t> encodeBusy(uint32_t transaction)
{
    std::vector<uint8_t> bytes(13);
    std::memcpy(bytes.data(),"AMBEDBUSY",9);
    store32(bytes.data()+9,transaction);
    return bytes;
}

inline std::vector<uint8_t> encodeDescriptor(uint16_t id, uint16_t port,
    uint8_t input, uint8_t firstOutput, uint32_t transaction)
{
    std::vector<uint8_t> bytes(18);
    std::memcpy(bytes.data(),"AMBEDSTD",8);
    std::memcpy(bytes.data()+8,&id,2);
    std::memcpy(bytes.data()+10,&port,2);
    bytes[12] = input; bytes[13] = firstOutput;
    store32(bytes.data()+14,transaction);
    return bytes;
}

} // namespace pcmwire
#endif
