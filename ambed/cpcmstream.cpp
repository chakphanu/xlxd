#include "main.h"
#include "cpcmstream.h"
#include "cpcmdevice.h"
#include <chrono>

PcmRuntimeConfig g_PcmRuntime;

namespace {
int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

CPcmStream::CPcmStream(const pcmwire::Open &request,uint16_t id,const CIp &peer)
    :m_Request(request),m_Id(id),m_Port(uint16_t(TRANSCODER_PORT+id)),
    m_Peer(peer),m_Stop(false),m_Last(nowUs()) {}

bool CPcmStream::IsHardware() const
{
    if(m_Request.input<=2) return true;
    for(size_t i=0;i<m_Request.count;++i) if(m_Request.outputs[i]<=2) return true;
    return false;
}

std::unique_ptr<CPcmStream> CPcmStream::Open(const pcmwire::Open &request,
    uint16_t id,const CIp &peer,const CIp &listen)
{
    std::unique_ptr<CPcmStream> stream(new CPcmStream(request,id,peer));
    if(stream->IsHardware()) {
        if(g_PcmRuntime.softwareOnly || g_PcmRuntime.serial.empty() ||
            g_PcmRuntime.profile!="dvstick30") return std::unique_ptr<CPcmStream>();
        if(request.input<=2 && request.input!=g_PcmRuntime.codec)
            return std::unique_ptr<CPcmStream>();
        for(size_t i=0;i<request.count;++i)
            if(request.outputs[i]<=2 && request.outputs[i]!=g_PcmRuntime.codec)
                return std::unique_ptr<CPcmStream>();
        stream->m_Hardware=OpenPcmDevice(g_PcmRuntime.serial,g_PcmRuntime.codec);
        if(!stream->m_Hardware) return std::unique_ptr<CPcmStream>();
    }
    stream->m_Codec=CPcmCodec::Open(request,stream->m_Hardware.get());
    if(!stream->m_Codec || !stream->m_Socket.Open(listen,stream->m_Port))
        return std::unique_ptr<CPcmStream>();
    stream->m_Thread=std::thread(&CPcmStream::Run,stream.get());
    return stream;
}

CPcmStream::~CPcmStream()
{
    m_Stop=true;
    if(m_Thread.joinable()) m_Thread.join();
    m_Socket.Close();
}

bool CPcmStream::IsActive() const { return nowUs()-m_Last.load()<3000000; }
bool CPcmStream::SameTransaction(const CIp &peer,uint32_t transaction) const
{ return peer==m_Peer && transaction==m_Request.transaction; }
bool CPcmStream::Matches(const pcmwire::Open &request) const
{
    return request.input==m_Request.input && request.count==m_Request.count &&
        request.outputs==m_Request.outputs && request.callsign==m_Request.callsign;
}
std::vector<uint8_t> CPcmStream::Descriptor() const
{
    return pcmwire::encodeDescriptor(m_Id,m_Port,m_Request.input,
        m_Request.outputs[0],m_Request.transaction);
}

void CPcmStream::Run()
{
    uint8_t lastPid=0;
    bool havePid=false;
    uint64_t sequence=0;
    while(!m_Stop.load()) {
        CBuffer buffer;
        CIp peer;
        if(m_Socket.Receive(&buffer,&peer,1)<0) continue;
        CIp expected=m_Peer;
        expected.GetSockAddr()->sin_port=htons(m_Port);
        if(!(peer==expected) || buffer.size()!=2+pcmwire::width(m_Request.input) ||
            buffer.data()[0]!=m_Request.input) continue;
        const uint8_t pid=buffer.data()[1];
        const uint8_t delta=uint8_t(pid-lastPid);
        if(havePid && (delta==0 || delta>128)) continue;
        havePid=true; lastPid=pid;
        ++sequence;
        m_Last=nowUs();
        std::vector<std::vector<uint8_t> > frames;
        if(!m_Codec->Process(buffer.data()+2,buffer.size()-2,frames)) continue;
        std::vector<uint8_t> bytes=pcmwire::encodeReply(pid,m_Request.outputs.data(),
            frames.data(),m_Request.count);
        if(bytes.empty()) continue;
        buffer.Set(bytes.data(),bytes.size());
        m_Socket.SendVoice(buffer,m_Peer,m_Port);
    }
    std::cout << "PCM stream " << m_Id << " closed after " << sequence << " input frames" << std::endl;
}
