//
//  globals.hpp
//  comple.test
//
//  Created by Sergey Shramchenko on 29/05/2018.
//  Copyright © 2018 Home. All rights reserved.
//

#ifndef __globals_hpp__
#define __globals_hpp__

#include "kodi/libXBMC_pvr.h"
#include "kodi/libXBMC_addon.h"

namespace Globals {

    extern CHelper_libXBMC_pvr* const PVR;
    extern ADDON::CHelper_libXBMC_addon* const XBMC;
    
    void LogError(const char *format, ... );
    void LogInfo(const char *format, ... );
    void LogNotice(const char *format, ... );
    void LogDebug(const char *format, ... );

}
#endif /* __globals_hpp__ */
