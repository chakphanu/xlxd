#include "main.h"
#include "cusrpprotocol.h"
#include "creflector.h"
#include <chrono>
#include <cerrno>
#include <future>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
uint64_t nowMs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

class CUsrpProtocol::Endpoint {
public:
    explicit Endpoint(const usrp::EndpointConfig &config):m_Config(config) {}
    ~Endpoint() { Stop(); }
    bool Open()
    {
        m_Fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
        if(m_Fd<0) return false;
        sockaddr_in local={};local.sin_family=AF_INET;local.sin_port=htons(m_Config.bindPort);
        inet_pton(AF_INET,m_Config.bindIp.c_str(),&local.sin_addr);
        m_Peer.sin_family=AF_INET;m_Peer.sin_port=htons(m_Config.peerPort);
        inet_pton(AF_INET,m_Config.peerIp.c_str(),&m_Peer.sin_addr);
        if(bind(m_Fd,reinterpret_cast<sockaddr *>(&local),sizeof(local))<0) return false;
#if DSCP_MARKING_ENABLE == 1
        const int tos=DSCP_VALUE<<2;
        setsockopt(m_Fd,IPPROTO_IP,IP_TOS,&tos,sizeof(tos));
#endif
        return true;
    }
    void Start()
    {
        CClients *clients=g_Reflector.GetClients();
        clients->AddClient(new CUsrpClient(CCallsign(m_Config.callsign.c_str()),Peer(),m_Config.module));
        g_Reflector.ReleaseClients();
        m_Registered=true;
        m_Rx=std::thread(&Endpoint::RunRx,this);
        m_Tx=std::thread(&Endpoint::RunTx,this);
    }
    void Stop()
    {
        m_StopRx=true;
        if(m_Rx.joinable()) m_Rx.join();
        m_StopTx=true;
        if(m_Tx.joinable()) m_Tx.join();
        if(m_Registered) {
            CClients *clients=g_Reflector.GetClients();
            CClient *client=clients->FindClient(Peer(),PROTOCOL_USRP,m_Config.module);
            if(client) clients->RemoveClient(client);
            g_Reflector.ReleaseClients();m_Registered=false;
        }
        if(m_Fd>=0) { ::close(m_Fd);m_Fd=-1; }
    }
    void Queue(const CPacket &packet)
    {
        if(packet.GetModuleId()!=uint8(m_Config.module)) return;
        std::lock_guard<std::mutex> guard(m_TxMutex);
        if(packet.IsDvHeader()) {
            m_TxSid=packet.GetStreamId();m_Transmit.Begin(m_TxSid,nowMs());
        } else if(packet.IsDvFrame()) {
            const CDvFramePacket &frame=static_cast<const CDvFramePacket &>(packet);
            if(frame.HasPcmData() && frame.GetPcmOrigin()!=Origin())
                m_Transmit.Push(frame.GetStreamId(),frame.GetPcm(),nowMs());
            if(frame.IsLastPacket()) m_Transmit.End(frame.GetStreamId());
        }
    }
private:
    CIp Peer() const { return CIp(&m_Peer); }
    uint32 Origin() const { return uint32(m_Config.module-'A'+1); }
    bool StartStream()
    {
        CClients *clients=g_Reflector.GetClients();
        CClient *client=clients->FindClient(Peer(),PROTOCOL_USRP,m_Config.module);
        if(!client) { g_Reflector.ReleaseClients();return false; }
        CCallsign rpt=g_Reflector.GetCallsign();rpt.SetModule(m_Config.module);
        CCallsign source(m_Config.callsign.c_str());
        CDvHeaderPacket *header=new CDvHeaderPacket(source,CCallsign("CQCQCQ"),source,rpt,
            CUsrpProtocol::AllocateGlobalStreamIdNonce(),0);
        header->SetMySuffix("USRP");
        m_Stream=g_Reflector.OpenStream(header,client);
        if(!m_Stream) delete header;
        g_Reflector.ReleaseClients();
        return m_Stream!=NULL;
    }
    void Voice(const usrp::Frame &frame)
    {
        if(!m_Stream) return;
        CDvFramePacket *packet=new CDvFramePacket(m_Stream->GetStreamId(),frame.pcm.data(),Origin());
        m_Stream->Lock();m_Stream->Push(packet);m_Stream->Unlock();
    }
    void EndStream()
    {
        if(!m_Stream || m_Drain.valid()) return;
        CDvLastFramePacket *last=new CDvLastFramePacket(m_Stream->GetStreamId());
        m_Stream->Lock();m_Stream->Push(last);m_Stream->Unlock();
        CPacketStream *stream=m_Stream;
        m_Drain=std::async(std::launch::async,[stream]() { return g_Reflector.CloseStream(stream); });
    }
    void RunRx()
    {
        while(!m_StopRx.load()) {
            if(m_Drain.valid() && m_Drain.wait_for(std::chrono::milliseconds(0))==std::future_status::ready) {
                m_Drain.get();m_Stream=NULL;m_Receive.Drained(nowMs());
            }
            pollfd fd={m_Fd,POLLIN,0};
            const int ready=poll(&fd,1,5);
            bool opened=false;
            if(ready>0) {
                uint8_t bytes[512];sockaddr_in source={};socklen_t size=sizeof(source);
                const ssize_t count=recvfrom(m_Fd,bytes,sizeof(bytes),MSG_TRUNC,
                    reinterpret_cast<sockaddr *>(&source),&size);
                usrp::Frame frame;
                if(count>=0 && size==sizeof(source) && source.sin_family==AF_INET &&
                    source.sin_addr.s_addr==m_Peer.sin_addr.s_addr && source.sin_port==m_Peer.sin_port &&
                    usrp::parse(bytes,size_t(count),frame)) {
                    const usrp::Event event=m_Receive.Accept(frame,nowMs(),false);
                    if(event==usrp::Event::Start) {
                        opened=StartStream();
                        if(opened) Voice(frame);
                        else m_Receive.Suppress();
                    } else if(event==usrp::Event::Voice) Voice(frame);
                    else if(event==usrp::Event::End) EndStream();
                }
            }
            if(!opened && m_Receive.Expire(nowMs())) EndStream();
        }
        EndStream();
        if(m_Drain.valid()) { m_Drain.get();m_Stream=NULL; }
    }
    void Send(const usrp::Frame &frame)
    {
        const auto bytes=usrp::encode(frame.sequence,frame.voice,frame.voice ? frame.pcm.data() : NULL);
        sendto(m_Fd,bytes.data(),bytes.size(),0,reinterpret_cast<const sockaddr *>(&m_Peer),sizeof(m_Peer));
    }
    void RunTx()
    {
        while(!m_StopTx.load()) {
            usrp::Frame frame;bool send=false;
            { std::lock_guard<std::mutex> guard(m_TxMutex);send=m_Transmit.Poll(nowMs(),frame); }
            if(send) Send(frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const uint64_t deadline=nowMs()+80;
        { std::lock_guard<std::mutex> guard(m_TxMutex);m_Transmit.End(m_TxSid); }
        while(nowMs()<deadline) {
            usrp::Frame frame;bool send=false,keyed=false;
            { std::lock_guard<std::mutex> guard(m_TxMutex);
                send=m_Transmit.Poll(nowMs(),frame);keyed=m_Transmit.Keyed(); }
            if(send) Send(frame);
            if(!keyed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    usrp::EndpointConfig m_Config;
    int m_Fd=-1;
    sockaddr_in m_Peer={};
    std::atomic<bool> m_StopRx{false},m_StopTx{false};
    bool m_Registered=false;
    std::thread m_Rx,m_Tx;
    std::mutex m_TxMutex;
    usrp::Receiver m_Receive;
    usrp::Transmitter m_Transmit;
    uint16 m_TxSid=0;
    CPacketStream *m_Stream=NULL;
    std::future<bool> m_Drain;
};

CUsrpProtocol::CUsrpProtocol(const std::vector<usrp::EndpointConfig> &config):m_Config(config) {}
CUsrpProtocol::~CUsrpProtocol() { Close(); }
bool CUsrpProtocol::Init()
{
    for(const auto &config:m_Config) {
        std::unique_ptr<Endpoint> endpoint(new Endpoint(config));
        if(!endpoint->Open()) { m_Endpoints.clear();return false; }
        m_Endpoints.push_back(std::move(endpoint));
    }
    for(auto &endpoint:m_Endpoints) endpoint->Start();
    return CProtocol::Init();
}
void CUsrpProtocol::Close()
{
    CProtocol::Close();
    m_Endpoints.clear();
}
void CUsrpProtocol::Task()
{
    HandleQueue();std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
void CUsrpProtocol::HandleQueue()
{
    for(;;) {
        m_Queue.Lock();
        CPacket *packet=NULL;
        if(!m_Queue.empty()) { packet=m_Queue.front();m_Queue.pop(); }
        m_Queue.Unlock();
        if(!packet) return;
        for(auto &endpoint:m_Endpoints) endpoint->Queue(*packet);
        delete packet;
    }
}
