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

#include <algorithm>
#include <rapidjson/error/en.h>
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include "p8-platform/util/StringUtils.h"
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/util.h"

#include "client_core_base.hpp"
#include "globals.hpp"
#include "HttpEngine.hpp"
#include "helpers.h"


namespace PvrClient{
    
    using namespace std;
    using namespace ADDON;
    using namespace rapidjson;
    using namespace Globals;
    
    static const char* c_EpgCacheDirPath = "special://temp/pvr-puzzle-tv";
    
    template< typename ContainerT, typename PredicateT >
    void erase_if( ContainerT& items, const PredicateT& predicate ) {
        for( auto it = items.begin(); it != items.end(); ) {
            if( predicate(*it) ) it = items.erase(it);
            else ++it;
        }
    }
    
    class ClientPhase : public IClientCore::IPhase
    {
    public:
        ClientPhase()
        : m_event(new P8PLATFORM::CEvent(false))
        , m_thread(NULL)
        , m_isDone(false)
        {}
        ~ClientPhase() {
            Cleanup();
        }
        void Wait() {
            if(m_isDone)
                return;
            m_event->Wait();
            Cleanup();
        }
        bool IsDone() { return m_isDone;}
        void Broadcast() {
            if(m_isDone)
                return;
            m_isDone = true;
            m_event->Broadcast();
            // m_thread calls Broadcast()
            // avoid to call Cleanup() since it deletes m_thread (deadlock)
            //Cleanup();
        }
        void RunAndSignalAsync(std::function<void(void)> action)
        {
            if(m_isDone)
                return;
            
            class Action : public P8PLATFORM::CThread
            {
                std::function<void(void)> m_action;
            public:
                Action(std::function<void(void)> action) : m_action(action){}
                void *Process(void) {
                    try {
                        m_action();
                    } catch (...) {
                        LogError("ClientPhase::Action() failed!");
                    }
                    return nullptr;
                };
            };
            
            m_thread = new Action([this, action] {
                if(action)
                    action();
                Broadcast();
            });
            m_thread->CreateThread();
        }
    private:
        void Cleanup() {
            m_isDone = true;
            
            if(m_thread)
                SAFE_DELETE(m_thread);
            if(m_event) {
                m_event->Broadcast();
                SAFE_DELETE(m_event);
            }
        }
        
        bool m_isDone;
        P8PLATFORM::CEvent* m_event;
        P8PLATFORM::CThread* m_thread;
    };
    
    
    ClientCoreBase::ClientCoreBase(const IClientCore::RecordingsDelegate& didRecordingsUpadate)
    : m_didRecordingsUpadate(didRecordingsUpadate)
    , m_groupList(m_mutableGroupList)
    , m_channelList(m_mutableChannelList)
    , m_lastEpgRequestEndTime(0)
    {
        if(nullptr == m_didRecordingsUpadate) {
            auto pvr = PVR;
            m_didRecordingsUpadate = [pvr](){ pvr->TriggerRecordingUpdate();};
        }
        m_httpEngine = new HttpEngine();
        m_phases[k_ChannelsLoadingPhase] =  new ClientPhase();
        m_phases[k_InitPhase] =  new ClientPhase();
        m_phases[k_EpgLoadingPhase] =  new ClientPhase();
        
    }
    
    void ClientCoreBase::InitAsync(bool clearEpgCache)
    {
        m_phases[k_InitPhase]->RunAndSignalAsync([this, clearEpgCache] {
            Init(clearEpgCache);
            std::time_t now = std::time(nullptr);
            now = std::mktime(std::gmtime(&now));
//            // Request EPG for all channels from max archive info to +1 days
//            int max_archive = 0;
//            for (const auto& ai  : m_archivesInfo) {
//                max_archive = std::max(ai.second, max_archive);
//            }
//            startTime -= max_archive *  60 * 60; // archive info in hours
            time_t startTime = now - 7 *24 *  60 * 60;
            startTime = std::max(startTime, m_lastEpgRequestEndTime);
            time_t endTime = now + 1 * 24 * 60 * 60;
            _UpdateEpgForAllChannels(startTime, endTime);
            m_recordingsUpdateDelay.Init(5 * 1000);
            ScheduleRecordingsUpdate();
        });
        
    }
    
    IClientCore::IPhase* ClientCoreBase::GetPhase(Phase phase)
    {
        if(m_phases.count(phase) > 0) {
            return m_phases[phase];
        }
        return nullptr;
    }
    
    ClientCoreBase::~ClientCoreBase()
    {
        for (auto& ph : m_phases) {
            if(ph.second) {
                delete ph.second;
            }
        }
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);
        m_epgEntries.clear();
    };
    
#pragma mark - Channels & Groups
    
    const ChannelList& ClientCoreBase::GetChannelList()
    {
        m_phases[k_ChannelsLoadingPhase]->Wait();
        return m_channelList;
    }
    
    const GroupList &ClientCoreBase::GetGroupList()
    {
        m_phases[k_ChannelsLoadingPhase]->Wait();
        return m_groupList;
    }
    
    void ClientCoreBase::RebuildChannelAndGroupList()
    {
        m_mutableChannelList.clear();
        m_mutableGroupList.clear();
        BuildChannelAndGroupList();
        m_phases[k_ChannelsLoadingPhase]->Broadcast();
    }
    
    void ClientCoreBase::AddChannel(const Channel& channel)
    {
        m_mutableChannelList[channel.Id] = channel;
    }
    
    void ClientCoreBase::AddGroup(GroupId groupId, const Group& group)
    {
        m_mutableGroupList[groupId] = group;
    }
    void ClientCoreBase::AddChannelToGroup(GroupId groupId, ChannelId channelId)
    {
        m_mutableGroupList[groupId].Channels.insert(channelId);
    }
    
#pragma mark - EPG
    string ClientCoreBase::MakeEpgCachePath(const char* cacheFile)
    {
        return string(c_EpgCacheDirPath) + "/" + cacheFile;
    }
    void ClientCoreBase::ClearEpgCache(const char* cacheFile)
    {
        XBMC->DeleteFile(MakeEpgCachePath(cacheFile).c_str());
    }
    
    void ClientCoreBase::LoadEpgCache(const char* cacheFile)
    {
        string cacheFilePath = MakeEpgCachePath(cacheFile);
        
        void* file = XBMC->OpenFile(cacheFilePath.c_str(), 0);
        if(NULL == file)
            return;
        int64_t fSize = XBMC->GetFileLength(file);
        
        char* rawBuf = new char[fSize + 1];
        if(0 == rawBuf)
            return;
        XBMC->ReadFile(file, rawBuf, fSize);
        XBMC->CloseFile(file);
        file = NULL;
        
        rawBuf[fSize] = 0;
        
        string ss(rawBuf);
        delete[] rawBuf;
        try {
            ParseJson(ss, [&] (Document& jsonRoot) {
                
                const Value& v = jsonRoot["m_epgEntries"];
                Value::ConstValueIterator it = v.Begin();
                for(; it != v.End(); ++it)
                {
                    EpgEntryList::key_type k = (*it)["k"].GetInt64();
                    EpgEntryList::mapped_type e;
                    e.Deserialize((*it)["v"]);
                    m_lastEpgRequestEndTime = std::max(m_lastEpgRequestEndTime, e.EndTime);
                    AddEpgEntry(k,e);
                }
            });
            
        } catch (...) {
            LogError(" >>>>  FAILED load EPG cache <<<<<");
            m_epgEntries.clear();
            m_lastEpgRequestEndTime = 0;
        }
    }
    
    void ClientCoreBase::SaveEpgCache(const char* cacheFile, unsigned int daysToPreserve)
    {
        string cacheFilePath = MakeEpgCachePath(cacheFile);
        
        // Leave epg entries not older then 1 weeks from now
        time_t now = time(nullptr);
        auto oldest = now - daysToPreserve*24*60*60;
        erase_if(m_epgEntries,  [oldest] (const EpgEntryList::value_type& i)
                 {
                     return i.second.StartTime < oldest;
                 });
        
        StringBuffer s;
        Writer<StringBuffer> writer(s);
        
        writer.StartObject();               // Between StartObject()/EndObject(),
        
        writer.Key("m_epgEntries");
        writer.StartArray();                // Between StartArray()/EndArray(),
        for_each(m_epgEntries.begin(), m_epgEntries.end(),[&](const EpgEntryList::value_type& i) {
            writer.StartObject();               // Between StartObject()/EndObject(),
            writer.Key("k");
            writer.Int64(i.first);
            writer.Key("v");
            i.second.Serialize(writer);
            writer.EndObject();
        });
        writer.EndArray();
        
        writer.EndObject();
        
        XBMC->CreateDirectory(c_EpgCacheDirPath);
        
        void* file = XBMC->OpenFileForWrite(cacheFilePath.c_str(), true);
        if(NULL == file)
            return;
        auto buf = s.GetString();
        XBMC->WriteFile(file, buf, s.GetSize());
        XBMC->CloseFile(file);
        
    }
    
    bool ClientCoreBase::AddEpgEntry(UniqueBroadcastIdType id, EpgEntry& entry)
    {
        // Do not add EPG for unknown channels
        if(m_mutableChannelList.count(entry.ChannelId) != 1)
            return false;
        
        UpdateHasArchive(entry);
        
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);
        while(m_epgEntries.count(id) != 0) {
            // Check duplicates.
            if(m_epgEntries[id].ChannelId == entry.ChannelId)
                return false;
            ++id;
        }
        m_epgEntries[id] =  entry;
        return true;
    }
    
    bool ClientCoreBase::GetEpgEntry(UniqueBroadcastIdType i,  EpgEntry& entry)
    {
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);
        bool result = m_epgEntries.count(i) > 0;
        if(result)
            entry = m_epgEntries[i];
        return result;
    }
    
    void ClientCoreBase::ForEachEpg(const EpgEntryAction& action) const
    {
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);
        for(const auto& i : m_epgEntries) {
            if(!action(i))
                return;
        }
    }
    
    void ClientCoreBase::GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries)
    {
        time_t lastEndTime = 0;
        IClientCore::EpgEntryAction action = [&lastEndTime, &epgEntries, channelId, startTime, endTime] (const EpgEntryList::value_type& i)
        {
            auto entryStartTime = i.second.StartTime;
            if (i.second.ChannelId == channelId  &&
                entryStartTime >= startTime &&
                entryStartTime < endTime)
            {
                lastEndTime = i.second.EndTime;
                epgEntries.insert(i);
            }
            return true;
        };
        ForEachEpg(action);
        
        if(lastEndTime < endTime) {
            
            LogDebug("GetEPG(%d): last Epg  %s -> requested by Kodi %s",
                     channelId, time_t_to_string(lastEndTime).c_str(), time_t_to_string(endTime).c_str());
            
            auto epgRequestStart = max(lastEndTime, m_lastEpgRequestEndTime);
            
            if(endTime > epgRequestStart) {
                // First EPG loading may be long. Delay recordings update for 90 sec
                m_recordingsUpdateDelay.Init(90 * 1000);
                _UpdateEpgForAllChannels(epgRequestStart, endTime);

                LogDebug("GetEPG(): m_lastEpgRequestEndTime (after) = %s", time_t_to_string(m_lastEpgRequestEndTime).c_str());
            }
        }
        m_recordingsUpdateDelay.Init(5 * 1000);
        //ScheduleRecordingsUpdate();
    }
    void ClientCoreBase::_UpdateEpgForAllChannels(time_t startTime, time_t endTime)
    {
        if(endTime <= m_lastEpgRequestEndTime || endTime <= startTime)
            return;
        
        startTime = std::max(startTime, m_lastEpgRequestEndTime);
        m_lastEpgRequestEndTime = endTime;
        
        char mbstr[100];
        if (std::strftime(mbstr, sizeof(mbstr), "%d/%m %H:%M - ", std::localtime(&startTime))) {
            int dec = strlen(mbstr);
            if (std::strftime(mbstr + dec, sizeof(mbstr) - dec, "%d/%m %H:%M", std::localtime(&endTime))) {
                LogDebug("Requested all cahnnel EPG update %s", mbstr);
            }
        }
        
        UpdateEpgForAllChannels(startTime, endTime);
    }
    
#pragma  mark - Recordings
    void ClientCoreBase::ScheduleRecordingsUpdate()
    {
        m_httpEngine->RunOnCompletionQueueAsync([this] {
            if(m_recordingsUpdateDelay.TimeLeft()) {
                ScheduleRecordingsUpdate();
            } else {
                if(!m_phases[k_EpgLoadingPhase]->IsDone()) {
                    m_phases[k_EpgLoadingPhase]->Broadcast();
                    ReloadRecordings();
                }
            }
        }, [] (CActionQueue::ActionResult ){});
    }
    
    void ClientCoreBase::OnEpgUpdateDone()
    {
        LogNotice("Archive thread iteraton started");
        // Localize EPG lock
        {
            P8PLATFORM::CLockObject lock(m_epgAccessMutex);
            LogDebug("Archive thread: EPG size %d", m_epgEntries.size());
            int recCounter = 0;
            for(auto& i : m_epgEntries) {
                UpdateHasArchive(i.second);
                if(i.second.HasArchive)
                    ++recCounter;
            }
            LogDebug("Archive thread: Recordings size %d", recCounter);
        }
        if(m_didRecordingsUpadate)
            m_didRecordingsUpadate();
        LogNotice("Archive thread iteraton done");
    }
    void ClientCoreBase::ReloadRecordings()
    {
        OnEpgUpdateDone();
    }
    
#pragma mark -
    void ClientCoreBase::ParseJson(const std::string& response, std::function<void(Document&)> parser)
    {
        Document jsonRoot;
        jsonRoot.Parse(response.c_str());
        if(jsonRoot.HasParseError()) {
            
            ParseErrorCode error = jsonRoot.GetParseError();
            auto strError = string("Rapid JSON parse error: ");
            strError += GetParseError_En(error);
            strError += " (" ;
            strError += error;
            strError += ").";
            LogError(strError.c_str());
            throw JsonParserException(strError);
        }
        parser(jsonRoot);
        return;
    }
}
