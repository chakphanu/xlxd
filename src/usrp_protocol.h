#ifndef XLXD_USRP_PROTOCOL_H
#define XLXD_USRP_PROTOCOL_H

#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

namespace usrp {

struct Frame {
    uint32_t sequence=0;
    bool voice=false;
    std::array<uint8_t,320> pcm={};
};

inline uint32_t load32(const uint8_t *p)
{
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
}
inline void store32(uint8_t *p,uint32_t v)
{
    p[0]=uint8_t(v>>24);p[1]=uint8_t(v>>16);p[2]=uint8_t(v>>8);p[3]=uint8_t(v);
}

// Fixed-channel voice and header-only unkey. Samples remain S16LE bytes.
inline bool parse(const uint8_t *bytes,size_t length,Frame &out)
{
    if(!bytes || (length!=32 && length!=352) || std::memcmp(bytes,"USRP",4)) return false;
    const uint32_t keyed=load32(bytes+12);
    if(load32(bytes+8) || load32(bytes+16) || load32(bytes+20) ||
        load32(bytes+24) || load32(bytes+28) || keyed>1 ||
        (length==352)!=(keyed==1)) return false;
    Frame next;
    next.sequence=load32(bytes+4);next.voice=keyed==1;
    if(next.voice) std::memcpy(next.pcm.data(),bytes+32,320);
    out=next;
    return true;
}

inline std::vector<uint8_t> encode(uint32_t sequence,bool keyed,const uint8_t *pcm)
{
    if(keyed && !pcm) return {};
    std::vector<uint8_t> bytes(keyed ? 352 : 32,0);
    std::memcpy(bytes.data(),"USRP",4);
    store32(bytes.data()+4,sequence);store32(bytes.data()+12,keyed ? 1 : 0);
    if(keyed) std::memcpy(bytes.data()+32,pcm,320);
    return bytes;
}

enum class RxState { Idle,Receiving,Draining,Suppressed };
enum class Event { Ignore,Start,Voice,End };

class Receiver {
public:
    RxState State() const { return m_State; }
    Event Accept(const Frame &frame,uint64_t now,bool busy)
    {
        if(m_HaveSequence) {
            const uint32_t delta=frame.sequence-m_Sequence;
            if(delta==0 || delta>=0x80000000U) return Event::Ignore;
        }
        m_Sequence=frame.sequence;m_HaveSequence=true;
        if(!frame.voice) {
            m_VoiceDuringDrain=false;
            if(m_State==RxState::Receiving) {
                m_State=RxState::Draining;
                return Event::End;
            }
            if(m_State==RxState::Suppressed) Reset();
            return Event::Ignore;
        }
        m_LastVoice=now;
        if(m_State==RxState::Draining) {
            m_VoiceDuringDrain=true;
            return Event::Ignore;
        }
        if(m_State==RxState::Suppressed) return Event::Ignore;
        if(m_State==RxState::Idle) {
            m_State=busy ? RxState::Suppressed : RxState::Receiving;
            return busy ? Event::Ignore : Event::Start;
        }
        return Event::Voice;
    }
    bool Expire(uint64_t now)
    {
        if(now-m_LastVoice<100) return false;
        if(m_State==RxState::Suppressed || m_State==RxState::Idle) Reset(true);
        else if(m_State==RxState::Receiving) {
            m_State=RxState::Draining;m_VoiceDuringDrain=false;
            return true;
        }
        return false;
    }
    void Suppress() { m_State=RxState::Suppressed; }
    void Drained(uint64_t now)
    {
        if(m_VoiceDuringDrain && now-m_LastVoice<100) m_State=RxState::Suppressed;
        else Reset(now-m_LastVoice>=100);
    }
private:
    void Reset(bool forgetSequence=false)
    {
        m_State=RxState::Idle;m_VoiceDuringDrain=false;
        if(forgetSequence) m_HaveSequence=false;
    }
    RxState m_State=RxState::Idle;
    bool m_HaveSequence=false,m_VoiceDuringDrain=false;
    uint32_t m_Sequence=0;
    uint64_t m_LastVoice=0;
};

struct EndpointConfig {
    char module=' ';
    std::string bindIp,peerIp,callsign;
    uint16_t bindPort=0,peerPort=0;
};

// The shared playout has already paced these frames. Keep at most two,
// discard residence over 40 ms, and conceal no more than 100 ms of a gap.
class Transmitter {
public:
    void Begin(uint16_t sid,uint64_t now)
    {
        if(m_Active && sid==m_Sid) return;
        m_PendingUnkey=m_PendingUnkey || m_Keyed;
        m_Keyed=false;m_Active=true;m_Ended=false;m_Sid=sid;m_Count=0;m_Head=0;
        if(m_NextDue<now) m_NextDue=now;
    }
    void Push(uint16_t sid,const uint8_t *pcm,uint64_t now)
    {
        if(!m_Active || sid!=m_Sid || m_Ended || !pcm) return;
        if(m_Count==2) { m_Head=(m_Head+1)%2;--m_Count; }
        Item &item=m_Items[(m_Head+m_Count)%2];
        std::memcpy(item.pcm.data(),pcm,320);item.arrival=now;++m_Count;
    }
    void End(uint16_t sid) { if(m_Active && sid==m_Sid) m_Ended=true; }
    bool Poll(uint64_t now,Frame &out)
    {
        if(now<m_NextDue) return false;
        if(m_PendingUnkey) {
            m_PendingUnkey=false;Emit(now,false,out);return true;
        }
        while(m_Count && now-m_Items[m_Head].arrival>40) Pop();
        if(m_Count) {
            Emit(now,true,out);out.pcm=m_Items[m_Head].pcm;Pop();
            m_Keyed=true;m_LastVoice=now;return true;
        }
        if(m_Keyed) {
            if(m_Ended || now-m_LastVoice>=100) {
                Emit(now,false,out);m_Keyed=false;
                if(m_Ended) m_Active=false;
                return true;
            }
            Emit(now,true,out);return true;
        }
        if(m_Ended) m_Active=false;
        return false;
    }
    bool Keyed() const { return m_Keyed || m_PendingUnkey; }
private:
    struct Item { uint64_t arrival=0;std::array<uint8_t,320> pcm={}; };
    void Pop() { m_Head=(m_Head+1)%2;--m_Count; }
    void Emit(uint64_t now,bool voice,Frame &out)
    {
        out=Frame();out.sequence=m_Sequence++;out.voice=voice;m_NextDue=now+20;
    }
    std::array<Item,2> m_Items;
    unsigned m_Head=0,m_Count=0;
    uint16_t m_Sid=0;
    uint32_t m_Sequence=0;
    uint64_t m_NextDue=0,m_LastVoice=0;
    bool m_Active=false,m_Keyed=false,m_Ended=false,m_PendingUnkey=false;
};

inline bool port(const std::string &text,uint16_t &value)
{
    uint32_t n=0;
    if(text.empty() || text.size()>5) return false;
    for(char c:text) {
        if(c<'0' || c>'9') return false;
        n=n*10+unsigned(c-'0');
    }
    if(n<1024 || n>65535) return false;
    value=uint16_t(n);return true;
}

inline bool address(const std::string &text,bool peer)
{
    in_addr ip={};
    if(inet_pton(AF_INET,text.c_str(),&ip)!=1) return false;
    const uint32_t host=ntohl(ip.s_addr);
    return host!=0xffffffffU && host<0xe0000000U && (!peer || host!=0);
}

inline bool callsign(const std::string &text)
{
    if(text.size()<3 || text.size()>8) return false;
    bool letter=false,digit=false;
    for(char c:text) {
        if(c>='A' && c<='Z') letter=true;
        else if(c>='0' && c<='9') digit=true;
        else return false;
    }
    return letter && digit;
}

// Parse all rows before opening any socket. Failure publishes no endpoints.
inline bool parseConfig(std::istream &input,std::vector<EndpointConfig> &out,std::string &error)
{
    out.clear();error.clear();
    std::vector<EndpointConfig> next;
    std::string line;
    unsigned lineNumber=0;
    while(std::getline(input,line)) {
        ++lineNumber;
        const size_t comment=line.find('#');
        if(comment!=std::string::npos) line.erase(comment);
        std::istringstream row(line);
        std::string module,bindPort,peerPort,extra;
        if(!(row>>module)) continue;
        EndpointConfig cfg;
        bool ok=bool(row>>cfg.bindIp>>bindPort>>cfg.peerIp>>peerPort>>cfg.callsign) && !(row>>extra);
        ok=ok && module.size()==1 && module[0]>='A' && module[0]<='Z' &&
            address(cfg.bindIp,false) && address(cfg.peerIp,true) &&
            port(bindPort,cfg.bindPort) && port(peerPort,cfg.peerPort) && callsign(cfg.callsign);
        if(ok) cfg.module=module[0];
        if(cfg.bindIp==cfg.peerIp && cfg.bindPort==cfg.peerPort) ok=false;
        for(const auto &old:next) {
            if(old.module==cfg.module ||
                (old.bindPort==cfg.bindPort && (old.bindIp==cfg.bindIp ||
                    old.bindIp=="0.0.0.0" || cfg.bindIp=="0.0.0.0")) ||
                (old.peerIp==cfg.peerIp && old.peerPort==cfg.peerPort)) ok=false;
        }
        if(!ok) { error="Invalid or duplicate USRP endpoint at line "+std::to_string(lineNumber);return false; }
        next.push_back(cfg);
    }
    if(input.bad()) { error="Cannot read USRP endpoint configuration";return false; }
    out.swap(next);return true;
}
}
#endif
