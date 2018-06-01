//
//  TimersEngine.hpp
//  comple.test
//
//  Created by Sergey Shramchenko on 01/06/2018.
//  Copyright © 2018 Home. All rights reserved.
//

#ifndef __TimersEngine_hpp__
#define __TimersEngine_hpp__

#include "addon.h"

namespace Engines {
    
    class TimersEngine : public ITimersEngine
    {
    public:
        int GetTimersAmount(void);
        PVR_ERROR AddTimer(const PVR_TIMER &timer);
        PVR_ERROR GetTimers(ADDON_HANDLE handle);
        PVR_ERROR DeleteTimer(const PVR_TIMER &timer, bool bForceDelete);
        PVR_ERROR UpdateTimer(const PVR_TIMER &timer);
        virtual ~TimersEngine();
    };
    
}

#endif /* TimersEngine_hpp */
