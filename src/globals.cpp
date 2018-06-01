/*
*
*   Copyright (C) 2017 Sergey Shramchenko
*   https://github.com/srg70/pvr.puzzle.tv
*
*  This Program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.
*
*  This Program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with XBMC; see the file COPYING.  If not, write to
*  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*  http://www.gnu.org/copyleft/gpl.html
*
*/


#include "globals.hpp"
#include "p8-platform/util/util.h"
#include "p8-platform/util/StringUtils.h"
#include "TimersEngine.hpp"

namespace Globals
{
    static CHelper_libXBMC_pvr* __pvr = nullptr;
    static ADDON::CHelper_libXBMC_addon* __xbmc = nullptr;
    CHelper_libXBMC_pvr* const& PVR(__pvr);
    ADDON::CHelper_libXBMC_addon* const& XBMC(__xbmc);
    ITimersEngine* const TIMERS(new Engines::TimersEngine());
    
    static ADDON::addon_log_t  __debugLogLevel = ADDON::LOG_DEBUG;
    void Cleanup();
    
    bool CreateWithHandle(void* hdl)
    {
        Cleanup();
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

        return true;
    }
    
    void Cleanup()
    {
        if(__pvr)
            SAFE_DELETE(__pvr);
        if(__xbmc)
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
        PrintToLog(__debugLogLevel);
    }
    

}
