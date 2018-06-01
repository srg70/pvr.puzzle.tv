//
//  TimersEngine.cpp
//  comple.test
//
//  Created by Sergey Shramchenko on 01/06/2018.
//  Copyright © 2018 Home. All rights reserved.
//

#include "TimersEngine.hpp"

using namespace Engines;

TimersEngine::~TimersEngine()
{
    
}

int TimersEngine::GetTimersAmount(void)
{
    return -1;
}
PVR_ERROR TimersEngine::AddTimer(const PVR_TIMER &timer)
{
    return PVR_ERROR_NOT_IMPLEMENTED;
}
PVR_ERROR TimersEngine::GetTimers(ADDON_HANDLE handle)
{
    return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR TimersEngine::DeleteTimer(const PVR_TIMER &timer, bool bForceDelete)
{
    return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR TimersEngine::UpdateTimer(const PVR_TIMER &timer)
{
    return PVR_ERROR_NOT_IMPLEMENTED;
}
