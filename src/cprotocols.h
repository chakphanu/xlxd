//
//  cprotocols.h
//  xlxd
//
//  Created by Jean-Luc Deltombe (LX3JL) on 01/11/2015.
//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.
//
// ----------------------------------------------------------------------------
//    This file is part of xlxd.
//
//    xlxd is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    xlxd is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Foobar.  If not, see <http://www.gnu.org/licenses/>. 
// ----------------------------------------------------------------------------

#ifndef cprotocols_h
#define cprotocols_h

#include "cprotocol.h"


////////////////////////////////////////////////////////////////////////////////////////
// define


////////////////////////////////////////////////////////////////////////////////////////
// class

class CProtocols
{
public:
    // constructors
    CProtocols();
    
    // destructors
    virtual ~CProtocols();
    
    // initialization
    bool Init(void);
    void Close(void);
    
    // get
    int Size(void) const            { return m_Count; }
    CProtocol *GetProtocol(int i)   { return m_Protocols[i]; }
    bool HasPcmModule(char module) const
    { return module>='A' && module<='Z' && (m_PcmModules & (1U<<(module-'A'))) != 0; }
    
protected:
    // data
    std::mutex                                m_Mutex;
    std::array<CProtocol *, NB_OF_PROTOCOLS+1>  m_Protocols;
    int m_Count=NB_OF_PROTOCOLS;
    uint32 m_PcmModules=0;
};


////////////////////////////////////////////////////////////////////////////////////////

#endif /* cprotocols_h */
