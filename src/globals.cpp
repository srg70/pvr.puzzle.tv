//
//  globals.cpp
//  comple.test
//
//  Created by Sergey Shramchenko on 29/05/2018.
//  Copyright © 2018 Home. All rights reserved.
//

#include "globals.hpp"
#include "p8-platform/util/util.h"
#include "p8-platform/util/StringUtils.h"

namespace Globals
{
    static CHelper_libXBMC_pvr* __pvr = nullptr;
    static ADDON::CHelper_libXBMC_addon* __xbmc = nullptr;
    CHelper_libXBMC_pvr* const PVR(__pvr);
    ADDON::CHelper_libXBMC_addon* const XBMC(__xbmc);

    bool CreateWithHandle(void* hdl)
    {
        __xbmc = new ADDON::CHelper_libXBMC_addon();
        if (!__xbmc->RegisterMe(hdl))
        {
            SAFE_DELETE(__xbmc);
            return false;
        }
        
        __pvr = new CHelper_libXBMC_pvr();
        if (!__pvr->RegisterMe(hdl))
        {
            SAFE_DELETE(__pvr);
            SAFE_DELETE(__xbmc);
            return false;
        }
        
        return ADDON_STATUS_OK;
    }
    
    void Cleanup()
    {
        SAFE_DELETE(__pvr);
        SAFE_DELETE(__xbmc);
    }
    
# define PrintToLog(loglevel) \
std::string strData; \
strData.reserve(16384); \
va_list va; \
va_start(va, format); \
strData = StringUtils::FormatV(format,va); \
va_end(va); \
XBMC->Log(loglevel, strData.c_str()); \

    
    void LogError(const char *format, ... )
    {
        PrintToLog(ADDON::LOG_ERROR);
    }
    void LogInfo(const char *format, ... )
    {
        PrintToLog(ADDON::LOG_INFO);
    }
    void LogNotice(const char *format, ... )
    {
        PrintToLog(ADDON::LOG_NOTICE);
    }
    void LogDebug(const char *format, ... )
    {
        PrintToLog(ADDON::LOG_DEBUG);
    }
    

}
