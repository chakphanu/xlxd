#ifndef CUSRPPROTOCOL_H
#define CUSRPPROTOCOL_H
#include "cprotocol.h"
#include "cclient.h"
#include "usrp_protocol.h"
#include <memory>

class CUsrpClient : public CClient {
public:
    CUsrpClient(const CCallsign &call,const CIp &ip,char module):CClient(call,ip,module) {}
    int GetProtocol() const { return PROTOCOL_USRP; }
    int GetCodec() const { return CODEC_PCM_S16LE_8K; }
    const char *GetProtocolName() const { return "USRP"; }
    bool IsNode() const { return true; }
    bool IsAlive() const { return true; }
};

class CUsrpProtocol : public CProtocol {
public:
    explicit CUsrpProtocol(const std::vector<usrp::EndpointConfig> &);
    ~CUsrpProtocol();
    bool Init();
    void Close();
    void Task();
protected:
    void HandleQueue();
private:
    class Endpoint;
    std::vector<usrp::EndpointConfig> m_Config;
    std::vector<std::unique_ptr<Endpoint> > m_Endpoints;
};
#endif
