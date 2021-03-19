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

#define NOMINMAX

#include <algorithm>
#include "TimersEngine.hpp"
#include "globals.hpp"

using namespace Globals;


static const char* c_TimersCacheDirPath = "special://temp/pvr-puzzle-tv/";
static const char* c_TimersCachePath = "special://temp/pvr-puzzle-tv/timers.dat";
namespace Engines
{
    static unsigned int s_LastTimerIndex = PVR_TIMER_NO_CLIENT_INDEX;
    
    static void DumpTimer(const kodi::addon::PVRTimer& timer);
    
    class Timer{
    public:
        Timer(const kodi::addon::PVRTimer& t)
        : m_pvrTimer(t)
        {
            m_pvrTimer.SetClientIndex(++s_LastTimerIndex);

        }
        ~Timer(){}
        
        inline time_t StartTime() const
        {
            return m_pvrTimer.GetStartTime() - m_pvrTimer.GetMarginStart() * 60;
        }
        inline time_t EndTime() const
        {
            return m_pvrTimer.GetEndTime() + m_pvrTimer.GetMarginEnd() * 60;
        }
        inline void Schedule()
        {
            m_pvrTimer.SetState(PVR_TIMER_STATE_SCHEDULED);
            LogDebug("Timer %s %s", m_pvrTimer.GetTitle().c_str(), "scheduled" );
        }
        inline bool StartRecording(ITimersEngineDelegate* delegate)
        {
            bool started = false;
            try {
                started = delegate->StartRecordingFor(m_pvrTimer);
            } catch (std::exception& ex) {
                LogError("Exception during recording creation: %s", ex.what());
            } catch (...) {
                LogError("Exception during recording creation: unknown");
            }
            m_pvrTimer.SetState(started ? PVR_TIMER_STATE_RECORDING : PVR_TIMER_STATE_ERROR);
            LogDebug("Timer %s %s", m_pvrTimer.GetTitle().c_str(), started ? "started" : "failed to start" );
            return started;
        }
        inline bool StopRecording(ITimersEngineDelegate* delegate)
        {
            bool stopped = delegate->StopRecordingFor(m_pvrTimer);

            if(stopped) {
                const bool isCompleted = EndTime() <= time(nullptr);
                m_pvrTimer.SetState(isCompleted ?  PVR_TIMER_STATE_COMPLETED : PVR_TIMER_STATE_CANCELLED);
                LogDebug("Timer %s %s", m_pvrTimer.GetTitle().c_str(), isCompleted ? "completed" : "cancelled" );
            } else {
                m_pvrTimer.SetState(PVR_TIMER_STATE_ERROR);
                LogDebug("Timer %s %s", m_pvrTimer.GetTitle().c_str(), "failed to stop" );
            }
            return stopped;
        }

        kodi::addon::PVRTimer m_pvrTimer;
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
        
        auto file = XBMC_OpenFile(c_TimersCachePath);
        if(NULL == file) {
            LogDebug("Timer Engine: no cache file found");
            return;
        }
        // Header
        int32_t size = 0;
        bool isEOF = file->Read(&size, sizeof(size)) < sizeof(size);
        // Content
        PVR_TIMER timer;
        while (size-- > 0 && !isEOF) {
            isEOF = file->Read(&timer, sizeof(timer)) > sizeof(timer);
            if(isEOF)
                continue;
            if(timer.state == PVR_TIMER_STATE_RECORDING)
                timer.state = PVR_TIMER_STATE_ABORTED;
            // Construct timer from serialized C-struct
            auto t = kodi::addon::PVRTimer();
            *(PVR_TIMER*)t = timer;
            m_timers.insert(Timers::value_type(new Timer(t)));
            
        }
        file->Close();
        delete file;
        file = nullptr;
        LogDebug("Timer Engine: loading cache done. %d timers found", m_timers.size());
    }
    
    void TimersEngine::SaveCache() const
    {

        LogDebug("Timer Engine: saving cache....");
        kodi::vfs::CreateDirectory(c_TimersCacheDirPath);
        
        kodi::vfs::CFile file;
        file.OpenFileForWrite(c_TimersCachePath, true);
        if(!file.IsOpen()) {
            LogError("Timer Engine: failed to open cache file for write. %s", c_TimersCachePath);
            return;
        }

        // Header
        int32_t size = m_timers.size();
        bool failed = file.Write(&size, sizeof(size))  != sizeof(size);
        // Content
        for (const auto& pTimer : m_timers) {
            if(failed)
                break;
            // Obtain timers' C-struc for serialization
            const auto& pvrTimer = *pTimer->m_pvrTimer.GetCStructure();
            failed = file.Write(&pvrTimer, sizeof(pvrTimer)) != sizeof(pvrTimer);
        }
        file.Close();
        
        if(failed){
            kodi::vfs::DeleteFile(c_TimersCachePath);
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
                double delta = difftime(endTime, now);
                // Check for recording timers to STOP recording
                if(pTimer->m_pvrTimer.GetState() == PVR_TIMER_STATE_RECORDING){
                    if(delta <= 0) {
                        pTimer->StopRecording(m_delegate);
                    } else {
                        nextWakeUpTime = nextWakeUpTime == now ? endTime :
                            difftime(nextWakeUpTime, endTime) > 0 ? endTime : nextWakeUpTime;
                    }
                }
            }
            // Start scheduled timers
            for (auto& pTimer : m_timers) {
                const time_t startTime = pTimer->StartTime();
                const time_t endTime = pTimer->EndTime();
                double delta = difftime(startTime, now);
                // Check for scheduled timers to START recording
                if(pTimer->m_pvrTimer.GetState() == PVR_TIMER_STATE_SCHEDULED && endTime > now){
                    if( delta <= 0 ) {
                        pTimer->StartRecording(m_delegate);
                        nextWakeUpTime = nextWakeUpTime == now ? endTime :
                            difftime(nextWakeUpTime, endTime) > 0 ? endTime : nextWakeUpTime;
                    } else {
                        nextWakeUpTime = nextWakeUpTime == now ? startTime :
                            difftime(nextWakeUpTime, startTime) > 0 ? startTime : nextWakeUpTime; 
                    }
                }

             }

            PVR->Addon_TriggerTimerUpdate();

            double waitTimeout = difftime(nextWakeUpTime, now);
            if(waitTimeout < 0) {
                waitTimeout = 1; // Shouldn't be true
            }
            LogDebug("Timer Engine: thread waiting %f sec (0 - forever)", waitTimeout);
            m_checkTimers.Wait(waitTimeout * 1000 );
        }
        LogDebug("Timer Engine: thread stopped.");
        
        return nullptr;
    }
    
    int TimersEngine::GetTimersAmount(void)
    {
        return m_timers.size();
    }
    
    PVR_ERROR TimersEngine::AddTimer(const kodi::addon::PVRTimer& timer)
    {
        DumpTimer(timer);
        m_timers.insert(Timers::value_type(new Timer(timer)));
        m_checkTimers.Signal();

        SaveCache();
        PVR->Addon_TriggerTimerUpdate();
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR TimersEngine::GetTimers(kodi::addon::PVRTimersResultSet& results)
    {
        for (const auto& t : m_timers) {
            results.Add(t->m_pvrTimer);
        }
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR TimersEngine::DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete)
    {
        for (const auto& t : m_timers) {
            if(t->m_pvrTimer.GetClientIndex() == timer.GetClientIndex()) {
                if(t->m_pvrTimer.GetState() == PVR_TIMER_STATE_RECORDING)
                {
                    if(!forceDelete)
                        return PVR_ERROR_RECORDING_RUNNING;
                    t->StopRecording(m_delegate);
                }
                m_timers.erase(t);
                m_checkTimers.Signal();

                SaveCache();
                PVR->Addon_TriggerTimerUpdate();
                return PVR_ERROR_NO_ERROR;
            }
        }
        // Timer not found
        return PVR_ERROR_INVALID_PARAMETERS;//PVR_ERROR_FAILED;
    }
    
    PVR_ERROR TimersEngine::UpdateTimer(const kodi::addon::PVRTimer& timer)
    {
        for (const auto& t : m_timers) {
            if(t->m_pvrTimer.GetClientIndex() == timer.GetClientIndex()) {
                if(t->m_pvrTimer.GetState() == PVR_TIMER_STATE_RECORDING)
                    t->StopRecording(m_delegate);
                t->m_pvrTimer = kodi::addon::PVRTimer(timer);
                DumpTimer(timer);
                SaveCache();
                m_checkTimers.Signal();

                PVR->Addon_TriggerTimerUpdate();
                return PVR_ERROR_NO_ERROR;
            }
        }
        // Timer not found
        return PVR_ERROR_INVALID_PARAMETERS;//PVR_ERROR_FAILED;
    }
    
    void DumpTimer(const kodi::addon::PVRTimer& timer) {
        LogDebug("%s: iClientIndex = %d", __FUNCTION__, timer.GetClientIndex());
        LogDebug("%s: iParentClientIndex = %d", __FUNCTION__, timer.GetParentClientIndex());
        LogDebug("%s: iClientChannelUid = %d", __FUNCTION__, timer.GetClientChannelUid());
        LogDebug("%s: startTime = %ld", __FUNCTION__, timer.GetStartTime());
        LogDebug("%s: endTime = %ld", __FUNCTION__, timer.GetEndTime());
        LogDebug("%s: state = %d", __FUNCTION__, timer.GetState());
        LogDebug("%s: iTimerType = %d", __FUNCTION__, timer.GetTimerType());
        LogDebug("%s: strTitle = %s", __FUNCTION__, timer.GetTitle().c_str());
        LogDebug("%s: strEpgSearchString = %s", __FUNCTION__, timer.GetEPGSearchString().c_str());
        LogDebug("%s: bFullTextEpgSearch = %d", __FUNCTION__, timer.GetFullTextEpgSearch());
        LogDebug("%s: strDirectory = %s", __FUNCTION__, timer.GetDirectory().c_str());
        LogDebug("%s: strSummary = %s", __FUNCTION__, timer.GetSummary().c_str());
        LogDebug("%s: iPriority = %d", __FUNCTION__, timer.GetPriority());
        LogDebug("%s: iLifetime = %d", __FUNCTION__, timer.GetLifetime());
        LogDebug("%s: firstDay = %d", __FUNCTION__, timer.GetFirstDay());
        LogDebug("%s: iWeekdays = %d", __FUNCTION__, timer.GetWeekdays());
        LogDebug("%s: iPreventDuplicateEpisodes = %d", __FUNCTION__, timer.GetPreventDuplicateEpisodes());
        LogDebug("%s: iEpgUid = %d", __FUNCTION__, timer.GetEPGUid());
        LogDebug("%s: iMarginStart = %d", __FUNCTION__, timer.GetMarginStart());
        LogDebug("%s: iMarginEnd = %d", __FUNCTION__, timer.GetMarginEnd());
        LogDebug("%s: iGenreType = %d", __FUNCTION__, timer.GetGenreType());
        LogDebug("%s: iGenreSubType = %d", __FUNCTION__, timer.GetGenreSubType());
        LogDebug("%s: iRecordingGroup = %d", __FUNCTION__, timer.GetRecordingGroup());
    }
    
}


