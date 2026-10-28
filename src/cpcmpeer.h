#ifndef XLXD_PCM_PEER_H
#define XLXD_PCM_PEER_H

#include "pcm_protocol.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

/** Opt-in control channel; legacy transcoder traffic uses its original socket. */
class CPcmPeer {
public:
    struct Stream {
        uint16_t id=0;
        uint16_t port=0;
        uint32_t transaction=0;
        std::vector<uint8_t> outputs;
    };
    CPcmPeer();
    ~CPcmPeer();
    bool Open(const std::string &local,const std::string &server,uint8_t input,
        uint32_t wanted,Stream &out);
    void Close(uint16_t id);
    void Reset();
    bool SupportsPcm();
private:
    struct Pending { bool ready=false;std::vector<uint8_t> reply; };
    bool ConnectUnlocked(const std::string &,const std::string &);
    bool DiscoverUnlocked(std::unique_lock<std::mutex> &,int64_t,uint64_t);
    bool WaitUnlocked(std::unique_lock<std::mutex> &,int64_t,const std::function<bool()> &);
    bool SendUnlocked(const uint8_t *,size_t);
    void SendCloseUnlocked(uint16_t);
    void FlushOrphansUnlocked();
    void Run(int,uint32_t,uint64_t);
    void DispatchUnlocked(const std::vector<uint8_t> &);
    int m_Fd;
    std::string m_Local,m_Server;
    pcmwire::Capabilities m_Capabilities;
    bool m_KnownPcm=false;
    uint32_t m_Transaction;
    uint64_t m_Epoch=0,m_Pongs=0;
    std::set<uint16_t> m_Active,m_Orphans;
    std::map<uint32_t,std::shared_ptr<Pending> > m_Pending;
    std::mutex m_Mutex,m_ResetMutex;
    std::condition_variable m_Changed;
    std::atomic<bool> m_Stop{false};
    std::thread m_Thread;
};

#endif
