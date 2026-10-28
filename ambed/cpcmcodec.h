#ifndef AMBED_PCM_CODEC_H
#define AMBED_PCM_CODEC_H

#include "pcm_protocol.h"
#include <array>
#include <memory>

class CCodec2;
class imbe_vocoder;
class CSignalProcessor;

/** Hardware is leased exclusively by the owning stream. */
class IPcmAmbe {
public:
    virtual ~IPcmAmbe() {}
    virtual uint8_t Codec() const = 0;
    virtual bool Decode(const uint8_t frame[9], int16_t pcm[160]) = 0;
    virtual bool Encode(const int16_t pcm[160], uint8_t frame[9]) = 0;
};

/** One source decoder and an independent state for every requested encoder. */
class CPcmCodec {
public:
    static std::unique_ptr<CPcmCodec> Open(const pcmwire::Open &, IPcmAmbe *);
    ~CPcmCodec();
    bool Process(const uint8_t *, size_t, std::vector<std::vector<uint8_t> > &);
private:
    CPcmCodec(const pcmwire::Open &, IPcmAmbe *);
    pcmwire::Open m_Request;
    IPcmAmbe *m_Hardware;
    std::unique_ptr<CCodec2> m_Codec2Decoder;
    std::unique_ptr<imbe_vocoder> m_ImbeDecoder;
    std::array<std::unique_ptr<CCodec2>,4> m_Codec2Encoders;
    std::array<std::unique_ptr<imbe_vocoder>,4> m_ImbeEncoders;
    std::unique_ptr<CSignalProcessor> m_InputProcessor;
    std::array<std::unique_ptr<CSignalProcessor>,4> m_OutputProcessors;
};

#endif
