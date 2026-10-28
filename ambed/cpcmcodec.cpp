#include "main.h"
#include "cpcmcodec.h"
#include "codec2/codec2.h"
#include "imbe/imbe_vocoder.h"
#include "csignalprocessor.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace {
void gain(std::array<int16_t,160> &pcm,float db)
{
    const float factor=std::pow(10.0f,db/20.0f);
    for(size_t i=0;i<pcm.size();++i) {
        const int32_t value=static_cast<int32_t>(pcm[i]*factor);
        pcm[i]=static_cast<int16_t>(std::max(-32768,std::min(32767,value)));
    }
}
void process(CSignalProcessor *processor,std::array<int16_t,160> &pcm)
{
    if(!processor) return;
    uint8 bytes[320];
    for(size_t i=0;i<160;++i) {
        const uint16_t value=static_cast<uint16_t>(pcm[i]);
        bytes[2*i]=uint8(value>>8); bytes[2*i+1]=uint8(value);
    }
    processor->Process(bytes,320);
    for(size_t i=0;i<160;++i) {
        const uint16_t value=uint16_t((uint16_t(bytes[2*i])<<8)|bytes[2*i+1]);
        pcm[i]=static_cast<int16_t>(value<=INT16_MAX ? int32_t(value) : int32_t(value)-65536);
    }
}
}

CPcmCodec::CPcmCodec(const pcmwire::Open &request,IPcmAmbe *hardware)
    : m_Request(request),m_Hardware(hardware)
{
    if(request.input==3) m_Codec2Decoder.reset(new CCodec2(true));
    if(request.input==4) m_ImbeDecoder.reset(new imbe_vocoder);
    if((request.input==1 || request.input==2) &&
        (request.count>1 || request.outputs[0]!=5))
        m_InputProcessor.reset(new CSignalProcessor(request.input==1 ? CODECGAIN_AMBEPLUS : CODECGAIN_AMBE2PLUS));
    for(size_t i=0;i<request.count;++i) {
        const uint8_t out=request.outputs[i];
        if(out==3) m_Codec2Encoders[i].reset(new CCodec2(true));
        if(out==4) m_ImbeEncoders[i].reset(new imbe_vocoder);
        if((out==1 || out==2) && (request.input==3 || request.input==4)) {
            const int db=request.input==3 ?
                (out==1 ? CODEC2_TO_AMBEPLUS_GAIN : CODEC2_TO_AMBE2PLUS_GAIN) :
                (out==1 ? IMBE_TO_AMBEPLUS_GAIN : IMBE_TO_AMBE2PLUS_GAIN);
            m_OutputProcessors[i].reset(new CSignalProcessor(float(db)));
        }
    }
}
CPcmCodec::~CPcmCodec() {}

std::unique_ptr<CPcmCodec> CPcmCodec::Open(const pcmwire::Open &request,IPcmAmbe *hardware)
{
    if(!pcmwire::width(request.input) || !pcmwire::validOutputs(request.outputs.data(),request.count,request.input))
        return std::unique_ptr<CPcmCodec>();
    if(request.input<=2 && (!hardware || hardware->Codec()!=request.input))
        return std::unique_ptr<CPcmCodec>();
    for(size_t i=0;i<request.count;++i)
        if(request.outputs[i]<=2 && (!hardware || hardware->Codec()!=request.outputs[i]))
            return std::unique_ptr<CPcmCodec>();
    return std::unique_ptr<CPcmCodec>(new CPcmCodec(request,hardware));
}

bool CPcmCodec::Process(const uint8_t *input,size_t length,
    std::vector<std::vector<uint8_t> > &outputs)
{
    if(!input || length!=pcmwire::width(m_Request.input)) return false;
    // The bundled software decoder has shared synthesis randomness. Keep
    // extended-stream codec operations ordered rather than racing that state.
    static std::mutex softwareMutex;
    std::array<int16_t,160> pcm={{0}};
    if(m_Request.input==5) {
        for(size_t i=0;i<160;++i) {
            const uint16_t value=uint16_t(input[2*i]|(uint16_t(input[2*i+1])<<8));
            pcm[i]=static_cast<int16_t>(value<=INT16_MAX ? int32_t(value) : int32_t(value)-65536);
        }
    } else if(m_Request.input==3) {
        std::lock_guard<std::mutex> guard(softwareMutex);
        m_Codec2Decoder->codec2_decode(pcm.data(),input);
    } else if(m_Request.input==4) {
        uint8_t frame[11]; std::memcpy(frame,input,11);
        m_ImbeDecoder->decode_4400(pcm.data(),frame);
    } else {
        if(!m_Hardware->Decode(input,pcm.data())) return false;
    }
    const std::array<int16_t,160> native=pcm;
    if(m_Request.count>1 || m_Request.outputs[0]!=5) {
        if(m_Request.input==3) gain(pcm,CODEC2_DECODE_GAIN);
        else if(m_Request.input==4) gain(pcm,IMBE_DECODE_GAIN);
        else if(m_Request.input<=2) process(m_InputProcessor.get(),pcm);
    }
    std::vector<std::vector<uint8_t> > next;
    bool complete=true;
    for(size_t i=0;i<m_Request.count;++i) {
        const uint8_t codec=m_Request.outputs[i];
        std::vector<uint8_t> frame(pcmwire::width(codec));
        std::array<int16_t,160> samples=codec==5 ? native : pcm;
        if(codec==5) {
            for(size_t j=0;j<160;++j) {
                const uint16_t value=static_cast<uint16_t>(samples[j]);
                frame[2*j]=uint8_t(value); frame[2*j+1]=uint8_t(value>>8);
            }
        } else if(codec==3) {
            std::lock_guard<std::mutex> guard(softwareMutex);
            if(m_Request.input==4) gain(samples,IMBE_TO_CODEC2_GAIN);
            m_Codec2Encoders[i]->codec2_encode(frame.data(),samples.data());
        } else if(codec==4) {
            if(m_Request.input==3) gain(samples,CODEC2_TO_IMBE_GAIN);
            else if(m_Request.input<=2) gain(samples,IMBE_ENCODE_GAIN);
            m_ImbeEncoders[i]->encode_4400(samples.data(),frame.data());
        } else {
            process(m_OutputProcessors[i].get(),samples);
            if(!m_Hardware->Encode(samples.data(),frame.data())) complete=false;
        }
        next.push_back(frame);
    }
    if(!complete) return false;
    outputs.swap(next);
    return true;
}
