#ifndef AMBED_PCM_STREAM_H
#define AMBED_PCM_STREAM_H

#include "cpcmcodec.h"
#include "cip.h"
#include "cudpsocket.h"
#include <atomic>
#include <string>
#include <thread>

struct PcmRuntimeConfig {
    bool softwareOnly=false;
    std::string serial;
    std::string profile;
    uint8_t codec=0;
};
extern PcmRuntimeConfig g_PcmRuntime;

class CPcmStream {
public:
    static std::unique_ptr<CPcmStream> Open(const pcmwire::Open &,uint16_t,
        const CIp &,const CIp &);
    ~CPcmStream();
    uint16_t Id() const { return m_Id; }
    uint16_t Port() const { return m_Port; }
    bool IsActive() const;
    bool IsHardware() const;
    bool SameTransaction(const CIp &,uint32_t) const;
    bool HasOwner(const CIp &peer) const { return peer==m_Peer; }
    bool Matches(const pcmwire::Open &) const;
    std::vector<uint8_t> Descriptor() const;
private:
    CPcmStream(const pcmwire::Open &,uint16_t,const CIp &);
    void Run();
    pcmwire::Open m_Request;
    uint16_t m_Id;
    uint16_t m_Port;
    CIp m_Peer;
    CUdpSocket m_Socket;
    std::unique_ptr<IPcmAmbe> m_Hardware;
    std::unique_ptr<CPcmCodec> m_Codec;
    std::atomic<bool> m_Stop;
    std::atomic<int64_t> m_Last;
    std::thread m_Thread;
};

#endif
