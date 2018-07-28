/*
*
*   Copyright (C) 2018 Sergey Shramchenko
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

#ifndef __TimersEngine_hpp__
#define __TimersEngine_hpp__

#include <memory>
#include <set>
#include "addon.h"
#include "p8-platform/threads/threads.h"

namespace Engines {
    
    class Timer;
    
    class TimersEngine : public ITimersEngine, public P8PLATFORM::CThread
    {
    public:
        TimersEngine(ITimersEngineDelegate* delegate);
        int GetTimersAmount(void);
        PVR_ERROR AddTimer(const PVR_TIMER &timer);
        PVR_ERROR GetTimers(ADDON_HANDLE handle);
        PVR_ERROR DeleteTimer(const PVR_TIMER &timer, bool bForceDelete);
        PVR_ERROR UpdateTimer(const PVR_TIMER &timer);
        virtual ~TimersEngine();
    private:
        typedef std::unique_ptr<Timer> TimerPtr;
        static bool CompareTimerPtr(const TimerPtr &left, const TimerPtr &right);
        typedef std::multiset<TimerPtr, bool (*)(const TimerPtr &left, const TimerPtr &right)> Timers;
        
        void LoadCache();
        void SaveCache() const;
        
        void *Process();
        
        P8PLATFORM::CEvent m_checkTimers;
        Timers  m_timers;
        ITimersEngineDelegate* m_delegate;
    };
    
}

#endif /* TimersEngine_hpp */
