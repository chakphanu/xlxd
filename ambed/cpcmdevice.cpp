#include "main.h"
#include "cpcmdevice.h"
#include "cusb3000interface.h"
#include "cambepacket.h"
#include "cvoicepacket.h"
#include <chrono>
#include <mutex>

namespace {
std::mutex deviceLease;

class CSoloAmbe : public CUsb3000Interface,public IPcmAmbe {
public:
    CSoloAmbe(const std::string &serial,uint8_t codec)
        :CUsb3000Interface(0x0403,0x6015,"DVstick-30",serial.c_str()),
        m_Lease(deviceLease,std::try_to_lock),m_Failed(false)
    { m_uiChCodec=codec; }
    ~CSoloAmbe()
    {
        if(m_FtdiHandle) { FT_Close(m_FtdiHandle);m_FtdiHandle=NULL; }
    }
    bool Start()
    {
        if(!m_Lease.owns_lock()) return false;
        if(!OpenDevice()) { m_FtdiHandle=NULL;return false; }
        return ResetDevice() && ReadDeviceVersion() && DisableParity() && ConfigureDevice();
    }
    uint8_t Codec() const { return m_uiChCodec; }
    bool Decode(const uint8_t frame[9],int16_t pcm[160])
    {
        if(m_Failed) return false;
        CAmbePacket packet(0,m_uiChCodec,frame);
        CBuffer request;
        EncodeChannelPacket(&request,0,&packet);
        if(!WriteBuffer(request)) return Fail();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(600);
        while(std::chrono::steady_clock::now()<deadline) {
            CBuffer reply;CVoicePacket voice;int channel=-1;
            if(!ReadBuffer(&reply)) continue;
            if(!IsValidSpeechPacket(reply,&channel,&voice) || voice.GetVoiceSize()!=320) continue;
            const uint8 *bytes=voice.GetVoice();
            for(size_t i=0;i<160;++i) {
                const uint16_t value=uint16_t((uint16_t(bytes[2*i])<<8)|bytes[2*i+1]);
                pcm[i]=static_cast<int16_t>(value<=INT16_MAX ? int32_t(value) : int32_t(value)-65536);
            }
            return true;
        }
        return Fail();
    }
    bool Encode(const int16_t pcm[160],uint8_t frame[9])
    {
        if(m_Failed) return false;
        uint8 bytes[320];
        for(size_t i=0;i<160;++i) {
            const uint16_t value=static_cast<uint16_t>(pcm[i]);
            bytes[2*i]=uint8(value>>8);bytes[2*i+1]=uint8(value);
        }
        CVoicePacket voice(bytes,320);CBuffer request;
        EncodeSpeechPacket(&request,0,&voice);
        if(!WriteBuffer(request)) return Fail();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(600);
        while(std::chrono::steady_clock::now()<deadline) {
            CBuffer reply;CAmbePacket encoded;int channel=-1;
            if(!ReadBuffer(&reply)) continue;
            if(!IsValidChannelPacket(reply,&channel,&encoded)) continue;
            std::memcpy(frame,encoded.GetAmbe(),9);
            return true;
        }
        return Fail();
    }
private:
    bool Fail()
    {
        // Untagged late replies must never be reused for a later frame.
        m_Failed=true;
        std::cerr << "PCM device transfer failed; close the stream before reopening" << std::endl;
        return false;
    }
    std::unique_lock<std::mutex> m_Lease;
    bool m_Failed;
};
}

std::unique_ptr<IPcmAmbe> OpenPcmDevice(const std::string &serial,uint8_t codec)
{
    if(serial.empty() || (codec!=1 && codec!=2)) return std::unique_ptr<IPcmAmbe>();
    std::unique_ptr<CSoloAmbe> device(new CSoloAmbe(serial,codec));
    if(!device->Start()) return std::unique_ptr<IPcmAmbe>();
    return std::unique_ptr<IPcmAmbe>(device.release());
}
