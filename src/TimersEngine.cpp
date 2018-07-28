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

#include "TimersEngine.hpp"
#include "globals.hpp"

using namespace Globals;
using namespace ADDON;


static const char* c_TimersCacheDirPath = "special://temp/pvr-puzzle-tv/";
static const char* c_TimersCachePath = "special://temp/pvr-puzzle-tv/timers.dat";
namespace Engines
{
    static unsigned int s_LastTimerIndex = PVR_TIMER_NO_CLIENT_INDEX;
    
    static void DumpTimer(const PVR_TIMER& timer);
    
    class Timer{
    public:
        Timer(const PVR_TIMER& t)
        : m_pvrTimer(t)
        {
            m_pvrTimer.iClientIndex = ++s_LastTimerIndex;

        }
        ~Timer(){}
        
        inline time_t StartTime() const
        {
            return m_pvrTimer.startTime - m_pvrTimer.iMarginStart * 60;
        }
        inline time_t EndTime() const
        {
            return m_pvrTimer.endTime + - m_pvrTimer.iMarginEnd * 60;
        }
        inline void Schedule()
        {
            m_pvrTimer.state = PVR_TIMER_STATE_SCHEDULED;
            LogDebug("Timer %s %s", &m_pvrTimer.strTitle[0], "scheduled" );
        }
        inline bool StartRecording(ITimersEngineDelegate* delegate)
        {
            bool started = delegate->StartRecordingFor(m_pvrTimer);
            m_pvrTimer.state = started ? PVR_TIMER_STATE_RECORDING : PVR_TIMER_STATE_ERROR;
            LogDebug("Timer %s %s", &m_pvrTimer.strTitle[0], started ? "started" : "failed to start" );
            return started;
        }
        inline bool StopRecording(ITimersEngineDelegate* delegate)
        {
            bool stopped = delegate->StopRecordingFor(m_pvrTimer);

            if(stopped) {
                const bool isCompleted = EndTime() <= time(nullptr);
                m_pvrTimer.state =  isCompleted ?  PVR_TIMER_STATE_COMPLETED : PVR_TIMER_STATE_CANCELLED;
                LogDebug("Timer %s %s", &m_pvrTimer.strTitle[0], isCompleted ? "completed" : "cancelled" );
            } else {
                m_pvrTimer.state = PVR_TIMER_STATE_ERROR;
                LogDebug("Timer %s %s", &m_pvrTimer.strTitle[0], "failed to stop" );
            }
            return stopped;
        }

        PVR_TIMER m_pvrTimer;
    };
    
    bool TimersEngine::CompareTimerPtr(const Engines::TimersEngine::TimerPtr &left, const Engines::TimersEngine::TimerPtr &right)
    {
        return left->StartTime() < right->StartTime();
    }
    
    TimersEngine::TimersEngine(ITimersEngineDelegate* delegate)
    : m_timers(&CompareTimerPtr)
    , m_delegate(delegate)
    {
        LoadCache();
        CreateThread();
    }
    
    TimersEngine::~TimersEngine()
    {
        StopThread(-1);
        m_checkTimers.Signal();
        StopThread();
        SaveCache();
    }
    
    void TimersEngine::LoadCache()
    {
        LogDebug("Timer Engine: loading cache....");
        
        void* file = XBMC->OpenFile(c_TimersCachePath, 0);
        if(NULL == file) {
            LogDebug("Timer Engine: no cache file found");
            return;
        }
        // Header
        int32_t size = 0;
        bool isEOF = XBMC->ReadFile(file, &size, sizeof(size)) < sizeof(size);
        // Content
        PVR_TIMER timer;
        while (size-- > 0 && !isEOF) {
            isEOF = XBMC->ReadFile(file, &timer, sizeof(timer)) > sizeof(timer);
            if(isEOF)
                continue;
            if(timer.state == PVR_TIMER_STATE_RECORDING)
                timer.state = PVR_TIMER_STATE_ABORTED;
            m_timers.insert(Timers::value_type(new Timer(timer)));
            
        }
        XBMC->CloseFile(file);
        LogDebug("Timer Engine: loading cache done. %d timers found", m_timers.size());
    }
    
    void TimersEngine::SaveCache() const
    {

        LogDebug("Timer Engine: saving cache....");
        XBMC->CreateDirectory(c_TimersCacheDirPath);
        
        void* file = XBMC->OpenFileForWrite(c_TimersCachePath, true);
        if(NULL == file) {
            LogError("Timer Engine: failed to open cache file for write. %s", c_TimersCachePath);
            return;
        }

        // Header
        int32_t size = m_timers.size();
        bool failed = XBMC->WriteFile(file, &size, sizeof(size))  != sizeof(size);
        // Content
        for (const auto& pTimer : m_timers) {
            if(failed)
                break;
            const auto& pvrTimer = pTimer->m_pvrTimer;
            failed = XBMC->WriteFile(file, &pvrTimer, sizeof(pvrTimer)) != sizeof(pvrTimer);
        }
        XBMC->CloseFile(file);
        if(failed){
            XBMC->DeleteFile(c_TimersCachePath);
            LogError("Timer Engine: failed to save all timers to cache.");
        } else {
            LogDebug("Timer Engine: saving cache done");
        }

    }
    
    void *TimersEngine::Process()
    {
        LogDebug("Timer Engine: thread started.");
        while(!IsStopped()) {
            LogDebug("Timer Engine: thread iteration started...");

            const time_t now = time(nullptr);
            time_t nextWakeUpTime = now;
            
            // Stop active recordings first
            for (auto& pTimer : m_timers) {
                const time_t startTime = pTimer->StartTime();
                const time_t endTime = pTimer->EndTime();
                // Check for recording timers to STOP recording
                if(pTimer->m_pvrTimer.state == PVR_TIMER_STATE_RECORDING){
                    if(endTime <= now) {
                        pTimer->StopRecording(m_delegate);
                    } else {
                        nextWakeUpTime = nextWakeUpTime == now ? endTime : std::min(nextWakeUpTime, endTime);
                    }
                }
            }
            // Start scheduled timers
            for (auto& pTimer : m_timers) {
                const time_t startTime = pTimer->StartTime();
                const time_t endTime = pTimer->EndTime();
                // Check for scheduled timers to START recording
                if(pTimer->m_pvrTimer.state == PVR_TIMER_STATE_SCHEDULED && endTime > now){
                    if( startTime <= now ) {
                        pTimer->StartRecording(m_delegate);
                        nextWakeUpTime = nextWakeUpTime == now ? endTime : std::min(nextWakeUpTime, endTime);
                    } else {
                        nextWakeUpTime = nextWakeUpTime == now ? startTime : std::min(nextWakeUpTime, startTime);
                    }
                }

             }

            PVR->TriggerTimerUpdate();

            time_t waitTimeout = (nextWakeUpTime - now);
            LogDebug("Timer Engine: thread waiting %d sec (0 - forever)", waitTimeout);
            m_checkTimers.Wait(waitTimeout * 1000 );
        }
        LogDebug("Timer Engine: thread stopped.");
        
        return nullptr;
    }
    
    int TimersEngine::GetTimersAmount(void)
    {
        return m_timers.size();
    }
    
    PVR_ERROR TimersEngine::AddTimer(const PVR_TIMER &t)
    {
        PVR_TIMER timer(t);
        
        DumpTimer(t);
        m_timers.insert(Timers::value_type(new Timer(timer)));
        m_checkTimers.Signal();

        SaveCache();
        PVR->TriggerTimerUpdate();
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR TimersEngine::GetTimers(ADDON_HANDLE handle)
    {
        for (const auto& t : m_timers) {
            PVR->TransferTimerEntry(handle, &t->m_pvrTimer);
        }
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR TimersEngine::DeleteTimer(const PVR_TIMER &timer, bool bForceDelete)
    {
        for (const auto& t : m_timers) {
            if(t->m_pvrTimer.iClientIndex == timer.iClientIndex) {
                if(t->m_pvrTimer.state == PVR_TIMER_STATE_RECORDING)
                {
                    if(!bForceDelete)
                        return PVR_ERROR_RECORDING_RUNNING;
                    t->StopRecording(m_delegate);
                }
                m_timers.erase(t);
                m_checkTimers.Signal();

                SaveCache();
                PVR->TriggerTimerUpdate();
                return PVR_ERROR_NO_ERROR;
            }
        }
        // Timer not found
        return PVR_ERROR_INVALID_PARAMETERS;//PVR_ERROR_FAILED;
    }
    
    PVR_ERROR TimersEngine::UpdateTimer(const PVR_TIMER &timer)
    {
        for (const auto& t : m_timers) {
            if(t->m_pvrTimer.iClientIndex == timer.iClientIndex) {
                if(t->m_pvrTimer.state == PVR_TIMER_STATE_RECORDING)
                    t->StopRecording(m_delegate);
                t->m_pvrTimer = timer;
                DumpTimer(timer);
                SaveCache();
                m_checkTimers.Signal();

                PVR->TriggerTimerUpdate();
                return PVR_ERROR_NO_ERROR;
            }
        }
        // Timer not found
        return PVR_ERROR_INVALID_PARAMETERS;//PVR_ERROR_FAILED;
    }
    
    void DumpTimer(const PVR_TIMER& timer) {
        LogDebug("%s: iClientIndex = %d", __FUNCTION__, timer.iClientIndex);
        LogDebug("%s: iParentClientIndex = %d", __FUNCTION__, timer.iParentClientIndex);
        LogDebug("%s: iClientChannelUid = %d", __FUNCTION__, timer.iClientChannelUid);
        LogDebug("%s: startTime = %ld", __FUNCTION__, timer.startTime);
        LogDebug("%s: endTime = %ld", __FUNCTION__, timer.endTime);
        LogDebug("%s: state = %d", __FUNCTION__, timer.state);
        LogDebug("%s: iTimerType = %d", __FUNCTION__, timer.iTimerType);
        LogDebug("%s: strTitle = %s", __FUNCTION__, timer.strTitle);
        LogDebug("%s: strEpgSearchString = %s", __FUNCTION__, timer.strEpgSearchString);
        LogDebug("%s: bFullTextEpgSearch = %d", __FUNCTION__, timer.bFullTextEpgSearch);
        LogDebug("%s: strDirectory = %s", __FUNCTION__, timer.strDirectory);
        LogDebug("%s: strSummary = %s", __FUNCTION__, timer.strSummary);
        LogDebug("%s: iPriority = %d", __FUNCTION__, timer.iPriority);
        LogDebug("%s: iLifetime = %d", __FUNCTION__, timer.iLifetime);
        LogDebug("%s: firstDay = %d", __FUNCTION__, timer.firstDay);
        LogDebug("%s: iWeekdays = %d", __FUNCTION__, timer.iWeekdays);
        LogDebug("%s: iPreventDuplicateEpisodes = %d", __FUNCTION__, timer.iPreventDuplicateEpisodes);
        LogDebug("%s: iEpgUid = %d", __FUNCTION__, timer.iEpgUid);
        LogDebug("%s: iMarginStart = %d", __FUNCTION__, timer.iMarginStart);
        LogDebug("%s: iMarginEnd = %d", __FUNCTION__, timer.iMarginEnd);
        LogDebug("%s: iGenreType = %d", __FUNCTION__, timer.iGenreType);
        LogDebug("%s: iGenreSubType = %d", __FUNCTION__, timer.iGenreSubType);
        LogDebug("%s: iRecordingGroup = %d", __FUNCTION__, timer.iRecordingGroup);
    }
    
}


