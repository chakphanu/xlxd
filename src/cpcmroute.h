#ifndef CPCMROUTE_H
#define CPCMROUTE_H
#include "main.h"
#include "cdvframepacket.h"

// Configured PCM modules and PCM-bearing voice use actual codec presence.
// Digital-only modules and control/end markers retain their existing policy.
inline bool PcmRouteAllows(const CPacket &packet,uint32 needed,bool pcmModule=false)
{
    if(!packet.IsDvFrame() || packet.IsLastPacket()) return true;
    const auto &frame=static_cast<const CDvFramePacket &>(packet);
    if(!pcmModule && !frame.HasPcmData()) return true;
    uint32 present=frame.HasPcmData() ? (1U<<CODEC_PCM_S16LE_8K) : 0U;
    if(frame.HasAmbeData()) present|=1U<<CODEC_AMBEPLUS;
    if(frame.HasAmbePlusData()) present|=1U<<CODEC_AMBE2PLUS;
    if(frame.HasCodec2Data()) present|=1U<<CODEC_CODEC2;
    if(frame.HasImbeData()) present|=1U<<CODEC_IMBE;
    return (needed & ~present)==0;
}
#endif
