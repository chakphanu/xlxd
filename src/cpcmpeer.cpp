#include "cpcmpeer.h"
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
const uint16_t ControlPort=10100;
}

CPcmPeer::CPcmPeer():m_Fd(-1),m_Transaction(uint32_t(
    std::chrono::steady_clock::now().time_since_epoch().count())) {}
CPcmPeer::~CPcmPeer() { Reset(); }

bool CPcmPeer::ConnectUnlocked(const std::string &local,const std::string &server)
{
    if(m_Stop.load()) return false;
    // Configuration is immutable until Reset; no receiver can observe a new peer.
    if(m_Fd>=0) return local==m_Local && server==m_Server;
    sockaddr_in address={};
    address.sin_family=AF_INET;
    in_addr destination={};
    if(inet_pton(AF_INET,local.c_str(),&address.sin_addr)!=1 ||
        inet_pton(AF_INET,server.c_str(),&destination)!=1) return false;
    m_Fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(m_Fd<0) return false;
    if(bind(m_Fd,reinterpret_cast<sockaddr *>(&address),sizeof(address))<0) {
        ::close(m_Fd);m_Fd=-1;return false;
    }
    m_Local=local;m_Server=server;
    m_Thread=std::thread(&CPcmPeer::Run,this,m_Fd,destination.s_addr,m_Epoch);
    return true;
}

bool CPcmPeer::SendUnlocked(const uint8_t *bytes,size_t length)
{
    if(m_Fd<0 || !bytes) return false;
    sockaddr_in peer={};
    peer.sin_family=AF_INET;peer.sin_port=htons(ControlPort);
    if(inet_pton(AF_INET,m_Server.c_str(),&peer.sin_addr)!=1) return false;
    ssize_t sent;
    do { sent=sendto(m_Fd,bytes,length,0,reinterpret_cast<sockaddr *>(&peer),sizeof(peer)); }
    while(sent<0 && errno==EINTR);
    return sent==ssize_t(length);
}

void CPcmPeer::SendCloseUnlocked(uint16_t id)
{
    uint8_t request[9]={'A','M','B','E','D','C','S',0,0};
    std::memcpy(request+7,&id,2);
    SendUnlocked(request,sizeof(request));
}

bool CPcmPeer::WaitUnlocked(std::unique_lock<std::mutex> &guard,int64_t deadline,
    const std::function<bool()> &ready)
{
    while(!ready()) {
        const int64_t remaining=deadline-nowMs();
        if(remaining<=0) return false;
        m_Changed.wait_for(guard,std::chrono::milliseconds(remaining));
    }
    return true;
}

void CPcmPeer::FlushOrphansUnlocked()
{
    // A pending allocation may reuse an old ID before its descriptor arrives.
    for(auto it=m_Orphans.begin();it!=m_Orphans.end();) {
        if(m_Active.count(*it)) it=m_Orphans.erase(it);
        else if(m_Pending.empty()) { SendCloseUnlocked(*it);it=m_Orphans.erase(it); }
        else ++it;
    }
}

bool CPcmPeer::DiscoverUnlocked(std::unique_lock<std::mutex> &guard,int64_t deadline,uint64_t epoch)
{
    const auto stopped=[&]() { return m_Stop.load() || epoch!=m_Epoch; };
    const uint8_t ping[17]={'A','M','B','E','D','P','I','N','G','X','L','X','P','C','M',' ',' '};
    const uint64_t before=m_Pongs;
    if(!SendUnlocked(ping,sizeof(ping))) return false;
    if(!WaitUnlocked(guard,std::min(deadline,nowMs()+200),[&]() {
        return stopped() || m_Pongs!=before;
    }) || stopped()) return false;
    if(m_Capabilities.mask) return true;
    if(!SendUnlocked(reinterpret_cast<const uint8_t *>("AMBEDCAPS"),9)) return false;
    const bool received=WaitUnlocked(guard,std::min(deadline,nowMs()+200),[&]() {
        return stopped() || m_Capabilities.mask!=0;
    });
    if(!received && !stopped()) m_KnownPcm=false;
    return received && !stopped();
}

void CPcmPeer::DispatchUnlocked(const std::vector<uint8_t> &reply)
{
    if(reply.size()==9 && pcmwire::tagged(reply.data(),reply.size(),"AMBEDPONG",9)) {
        ++m_Pongs;return;
    }
    pcmwire::Capabilities capabilities;
    if(pcmwire::parseCapabilities(reply.data(),reply.size(),capabilities)) {
        m_Capabilities=capabilities;
        m_KnownPcm=(capabilities.mask&(1U<<pcmwire::PcmCodec))!=0;
        return;
    }
    const bool descriptor=reply.size()==18 && pcmwire::tagged(reply.data(),reply.size(),"AMBEDSTD",8);
    const bool busy=reply.size()==13 && pcmwire::tagged(reply.data(),reply.size(),"AMBEDBUSY",9);
    if(!descriptor && !busy) return;
    const uint32_t transaction=pcmwire::load32(reply.data()+(descriptor ? 14 : 9));
    auto pending=m_Pending.find(transaction);
    if(pending!=m_Pending.end()) {
        if(!pending->second->ready) {
            pending->second->reply=reply;pending->second->ready=true;
        }
    } else if(descriptor) {
        uint16_t id=0;std::memcpy(&id,reply.data()+8,2);
        if(id) { m_Orphans.insert(id);FlushOrphansUnlocked(); }
    }
}

void CPcmPeer::Run(int fd,uint32_t expected,uint64_t epoch)
{
    // One receive owner dispatches by transaction; waiters never compete for UDP replies.
    while(!m_Stop.load()) {
        pollfd socket={fd,POLLIN,0};
        const int ready=poll(&socket,1,10);
        if(ready<0 && errno==EINTR) continue;
        if(ready<=0) continue;
        uint8_t data[512];sockaddr_in source={};socklen_t size=sizeof(source);
        const ssize_t count=recvfrom(fd,data,sizeof(data),MSG_DONTWAIT|MSG_TRUNC,
            reinterpret_cast<sockaddr *>(&source),&size);
        if(count<0 || size!=sizeof(source) || source.sin_family!=AF_INET ||
            source.sin_addr.s_addr!=expected || source.sin_port!=htons(ControlPort) ||
            size_t(count)>sizeof(data)) continue;
        {
            std::lock_guard<std::mutex> guard(m_Mutex);
            if(m_Stop.load() || epoch!=m_Epoch) continue;
            DispatchUnlocked(std::vector<uint8_t>(data,data+count));
        }
        m_Changed.notify_all();
    }
}

bool CPcmPeer::Open(const std::string &local,const std::string &server,
    uint8_t input,uint32_t wanted,Stream &out)
{
    // Admission and network waits share one absolute budget, including contention.
    const int64_t deadline=nowMs()+1400;
    std::unique_lock<std::mutex> guard(m_Mutex);
    if(!pcmwire::width(input) || !ConnectUnlocked(local,server)) return false;
    const uint64_t epoch=m_Epoch;
    if(!m_Capabilities.mask && !DiscoverUnlocked(guard,deadline,epoch)) return false;
    if(m_Stop.load() || epoch!=m_Epoch || !(m_Capabilities.mask&(1U<<input))) return false;
    pcmwire::Open request;
    std::memcpy(request.callsign.data(),"XLXPCM  ",8);
    request.input=input;
    const uint32_t selected=wanted&m_Capabilities.mask&pcmwire::FormatMask&~(1U<<input);
    for(uint8_t codec=1;codec<=5;++codec)
        if(selected&(1U<<codec)) request.outputs[request.count++]=codec;
    if(!request.count || request.count>m_Capabilities.maxOutputs || nowMs()>=deadline) return false;
    do { if(++m_Transaction==0) ++m_Transaction; } while(m_Pending.count(m_Transaction));
    request.transaction=m_Transaction;
    const auto pending=std::make_shared<Pending>();
    m_Pending[request.transaction]=pending;
    const auto bytes=pcmwire::encodeOpen(request);
    if(!SendUnlocked(bytes.data(),bytes.size())) {
        m_Pending.erase(request.transaction);FlushOrphansUnlocked();return false;
    }
    const bool received=WaitUnlocked(guard,std::min(deadline,nowMs()+1000),[&]() {
        return m_Stop.load() || epoch!=m_Epoch || pending->ready;
    });
    if(m_Stop.load() || epoch!=m_Epoch) return false;
    m_Pending.erase(request.transaction);
    if(!received) { m_Capabilities=pcmwire::Capabilities();FlushOrphansUnlocked();return false; }
    const auto &reply=pending->reply;
    if(reply.size()!=18) { FlushOrphansUnlocked();return false; }
    Stream stream;
    std::memcpy(&stream.id,reply.data()+8,2);
    std::memcpy(&stream.port,reply.data()+10,2);
    if(!stream.id || stream.port<1024 || reply[12]!=input ||
        reply[13]!=request.outputs[0] || m_Active.count(stream.id)) {
        if(stream.id && !m_Active.count(stream.id)) SendCloseUnlocked(stream.id);
        FlushOrphansUnlocked();
        return false;
    }
    stream.transaction=request.transaction;
    stream.outputs.assign(request.outputs.begin(),request.outputs.begin()+request.count);
    m_Active.insert(stream.id);FlushOrphansUnlocked();out=stream;
    return true;
}

void CPcmPeer::Close(uint16_t id)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    if(m_Active.erase(id)) SendCloseUnlocked(id);
    m_Orphans.erase(id);FlushOrphansUnlocked();
}
void CPcmPeer::Reset()
{
    std::lock_guard<std::mutex> reset(m_ResetMutex);
    {
        std::lock_guard<std::mutex> guard(m_Mutex);
        m_Stop=true;++m_Epoch;
        for(uint16_t id:m_Active) SendCloseUnlocked(id);
        for(const auto &entry:m_Pending) {
            const auto &reply=entry.second->reply;
            if(reply.size()==18) {
                uint16_t id=0;std::memcpy(&id,reply.data()+8,2);
                if(id && !m_Active.count(id)) m_Orphans.insert(id);
            }
        }
        m_Active.clear();m_Pending.clear();m_Capabilities=pcmwire::Capabilities();
        m_KnownPcm=false;
        FlushOrphansUnlocked();
    }
    m_Changed.notify_all();
    if(m_Thread.joinable()) m_Thread.join();
    std::lock_guard<std::mutex> guard(m_Mutex);
    if(m_Fd>=0) ::close(m_Fd);
    m_Fd=-1;m_Local.clear();m_Server.clear();m_Stop=false;
}

bool CPcmPeer::SupportsPcm()
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    // Allocation failure is not proof that a negotiated peer became legacy.
    return m_KnownPcm;
}
