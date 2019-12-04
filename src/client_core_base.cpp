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
#include "XMLTV_loader.hpp"


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
    void RunAndSignalAsync(std::function<void(std::function<bool(void)>)> action)
    {
        if(m_isDone)
            return;
        
        class Action : public P8PLATFORM::CThread
        {
            std::function<void(void)> m_action;
        public:
            Action(std::function<void(void)> act) : m_action(act){}
            void *Process(void) {
                try {
                    m_action();
                } catch (exception& ex) {
                    LogError("ClientPhase::Action() exception thrown: %s", ex.what());
                } catch (...) {
                    LogError("ClientPhase::Action() failed. Unknown exception");
                }
                return nullptr;
            };
        };
        
        m_thread = new Action([this, action] {
            if(m_isDone)
                return;
            if(action)
                action([this]{ return m_thread->IsStopped();});
            Broadcast();
        });
        m_thread->CreateThread();
    }
private:
    void Cleanup() {
        m_isDone = true;
        
        static P8PLATFORM::CMutex s_CleanupMutex;
        P8PLATFORM::CLockObject lock(s_CleanupMutex);
        if(m_thread){
            // Use HUGE timeout (2 minutes) for phase thread
            // because it can perform some "heavy" operation, like EPG loading...
            if(!m_thread->StopThread(120*1000))
                LogError("ClientPhase::Cleanup(): FAILED to stop phase thread in 2 minutes!");
            SAFE_DELETE(m_thread);
        }
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
, m_rpcPort(8080)
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

void ClientCoreBase::InitAsync(bool clearEpgCache, bool updateRecordings)
{
    m_phases[k_InitPhase]->RunAndSignalAsync([this, clearEpgCache, updateRecordings] (std::function<bool(void)> isAborted){
        if(isAborted())
            return;
        Init(clearEpgCache);
        if(isAborted())
            return;
        std::time_t now = std::time(nullptr);
        now = std::mktime(std::gmtime(&now));
        //            // Request EPG for all channels from max archive info to +1 days
        //            int max_archive = 0;
        //            for (const auto& ai  : m_archivesInfo) {
        //                max_archive = std::max(ai.second, max_archive);
        //            }
        //            startTime -= max_archive *  60 * 60; // archive info in hours
        time_t startTime = now - 7 *24 *  60 * 60;
        if(difftime(m_lastEpgRequestEndTime, startTime) > 0 ) { // i.e.  m_lastEpgRequestEndTime > startTime
            startTime = m_lastEpgRequestEndTime;
        }
        time_t endTime = now + 7 * 24 * 60 * 60;
        if(isAborted())
            return;
        _UpdateEpgForAllChannels(startTime, endTime);
        if(isAborted())
            return;
        m_recordingsUpdateDelay.Init(5 * 1000);
        
        if(!updateRecordings || isAborted())
            return;
        
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

// Should be called from destructor of dirived class
// During engine destruction vitrual functions may be called...
void ClientCoreBase::PrepareForDestruction() {
    if(NULL == m_httpEngine)
        return ;
    for (auto& ph : m_phases) {
        if(ph.second) {
            delete ph.second;
        }
    }
    m_phases.clear();
    SAFE_DELETE(m_httpEngine);
    
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    m_epgEntries.clear();
}

ClientCoreBase::~ClientCoreBase()
{
    PrepareForDestruction();
};

#pragma mark - Channels & Groups

const ChannelList& ClientCoreBase::GetChannelList()
{
    auto phase =  GetPhase(k_ChannelsLoadingPhase);
    if(phase) {
        phase->Wait();
    }
    return m_channelList;
}

const GroupList &ClientCoreBase::GetGroupList()
{
    auto phase =  GetPhase(k_ChannelsLoadingPhase);
    if(phase) {
        phase->Wait();
    }
    return m_groupList;
}

PvrClient::GroupId ClientCoreBase::GroupForChannel(PvrClient::ChannelId chId) {
    if(m_channelToGroupLut.count(chId) > 0) {
        return m_channelToGroupLut[chId];
    }
    return -1;
}

void ClientCoreBase::RebuildChannelAndGroupList()
{
    m_mutableChannelList.clear();
    m_mutableGroupList.clear();
    m_channelToGroupLut.clear();
    BuildChannelAndGroupList();
    auto phase =  static_cast<ClientPhase*> (GetPhase(k_ChannelsLoadingPhase));
    if(phase) {
        phase->Broadcast();
    }
}

void ClientCoreBase::AddChannel(const Channel& channel)
{
    m_mutableChannelList[channel.UniqueId] = channel;
}

void ClientCoreBase::AddGroup(GroupId groupId, const Group& group)
{
    m_mutableGroupList[groupId] = group;
}
void ClientCoreBase::AddChannelToGroup(GroupId groupId, ChannelId channelId)
{
    int idx = m_mutableGroupList[groupId].Channels.size();
    AddChannelToGroup(groupId, channelId, idx);
}
void ClientCoreBase::AddChannelToGroup(GroupId groupId, ChannelId channelId, int indexInGroup)
{
    m_mutableGroupList[groupId].Channels[indexInGroup] = channelId;
    m_channelToGroupLut[channelId] = groupId;
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

bool ClientCoreBase::ReadFileContent(const char* cacheFile, std::string& buffer)
{
    void* file = XBMC->OpenFile(cacheFile, 0);
    if(NULL == file)
        return false;
    int64_t fSize = XBMC->GetFileLength(file);
    
    char* rawBuf = new char[fSize + 1];
    if(0 == rawBuf)
        return false;
    XBMC->ReadFile(file, rawBuf, fSize);
    XBMC->CloseFile(file);
    file = NULL;
    
    rawBuf[fSize] = 0;
    
    buffer.assign(rawBuf);
    delete[] rawBuf;
    return true;
}

void ClientCoreBase::LoadEpgCache(const char* cacheFile)
{
    string cacheFilePath = MakeEpgCachePath(cacheFile);
    
    string ss;
    if(!ReadFileContent(cacheFilePath.c_str(), ss))
        return;
    
    try {
        ParseJson(ss, [&] (Document& jsonRoot) {
            
            const Value& v = jsonRoot["m_epgEntries"];
            Value::ConstValueIterator it = v.Begin();
            for(; it != v.End(); ++it)
            {
                EpgEntryList::key_type k = (*it)["k"].GetInt64();
                EpgEntryList::mapped_type e;
                if(!e.Deserialize((*it)["v"]))
                    continue;
                if(difftime(e.EndTime, m_lastEpgRequestEndTime) > 0 ) { // e.EndTime >  m_lastEpgRequestEndTime
                    m_lastEpgRequestEndTime = e.EndTime;
                }
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
    
    StringBuffer s;
    {
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);
        
        // Leave epg entries not older then 1 weeks from now
        time_t now = time(nullptr);
        auto oldest = now - daysToPreserve*24*60*60;
        erase_if(m_epgEntries,  [oldest] (const EpgEntryList::value_type& i)
                 {
            return i.second.StartTime < oldest;
        });
        
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
    }
    XBMC->CreateDirectory(c_EpgCacheDirPath);
    
    void* file = XBMC->OpenFileForWrite(cacheFilePath.c_str(), true);
    if(NULL == file)
        return;
    auto buf = s.GetString();
    XBMC->WriteFile(file, buf, s.GetSize());
    XBMC->CloseFile(file);
    
}

UniqueBroadcastIdType ClientCoreBase::AddEpgEntry(UniqueBroadcastIdType id, EpgEntry& entry)
{
    bool isUnknownChannel = m_channelList.count(entry.UniqueChannelId) == 0;
    // Do not add EPG for unknown channels
    if(isUnknownChannel)
        return c_UniqueBroadcastIdUnknown;
    
    UpdateHasArchive(entry);
    
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    while(m_epgEntries.count(id) != 0) {
        // Check duplicates.
        if(m_epgEntries[id].UniqueChannelId == entry.UniqueChannelId)
            return id;
        ++id;
    }
    m_epgEntries[id] =  entry;
    return id;
}

bool ClientCoreBase::AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
{
    
    EpgEntry epgEntry;
    epgEntry.Title = xmlEpgEntry.strTitle;
    epgEntry.Description = xmlEpgEntry.strPlot;
    epgEntry.StartTime = xmlEpgEntry.startTime;
    epgEntry.EndTime = xmlEpgEntry.endTime;
    epgEntry.IconPath = xmlEpgEntry.iconPath;
    
    bool isAdded = false;
    UniqueBroadcastIdType id = xmlEpgEntry.startTime;
    for(const auto& ch : m_channelList) {
        if(ch.second.EpgId != xmlEpgEntry.EpgId)
            continue;
        epgEntry.UniqueChannelId = ch.first;
        id = AddEpgEntry(id, epgEntry);
        if(c_UniqueBroadcastIdUnknown == id) {
            id = xmlEpgEntry.startTime;
        } else {
            isAdded = true;
        }
    }
    return isAdded;
}

//    void ClientCoreBase::UpdateEpgEntry(UniqueBroadcastIdType id, const EpgEntry& entry)
//    {
//        EPG_TAG tag = { 0 };
//        {
//            P8PLATFORM::CLockObject lock(m_epgAccessMutex);
//            auto & oldEntry = m_epgEntries[id];
//            bool hasArchive = oldEntry.HasArchive;
//            oldEntry =  entry;
//            oldEntry.HasArchive = hasArchive;
//
//            // Update EPG tag
//            tag.iUniqueBroadcastId = id;
//            entry.FillEpgTag(tag);
//        }
//        PVR->EpgEventStateChange(&tag, EPG_EVENT_UPDATED);
//
//    }

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

void ClientCoreBase::GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryAction& onEpgEntry)
{
    time_t lastEndTime = startTime;
    IClientCore::EpgEntryAction action = [&lastEndTime, &onEpgEntry, channelId, startTime, endTime] (const EpgEntryList::value_type& i)
    {
        auto entryStartTime = i.second.StartTime;
        if (i.second.UniqueChannelId == channelId  &&
            entryStartTime >= startTime &&
            entryStartTime < endTime)
        {
            lastEndTime = i.second.EndTime;
            //epgEntries.insert(i);
            onEpgEntry(i);
        }
        return true;
    };
    ForEachEpg(action);
    
    if(lastEndTime < endTime) {
        string channelName;
        if(m_channelList.count(channelId)) {
            channelName = m_channelList.at(channelId).Name;
        } else {
            channelName = to_string(channelId);
        }
        
        if(lastEndTime == startTime) {
            LogDebug("GetEPG(%s): channel has NO loaded EPG for interval [%s -- %s].",
                     channelName.c_str(), time_t_to_string(lastEndTime).c_str(), time_t_to_string(endTime).c_str());
            
        } else {
            LogDebug("GetEPG(%s): last for channel %s -> requested by Kodi %s",
                     channelName.c_str(), time_t_to_string(lastEndTime).c_str(), time_t_to_string(endTime).c_str());
        }
        // First EPG loading may be long. Delay recordings update for 90 sec
        m_recordingsUpdateDelay.Init(90 * 1000);
        _UpdateEpgForAllChannels(/*epgRequestStart*/lastEndTime, endTime);
        
    }
    m_recordingsUpdateDelay.Init(5 * 1000);
    //ScheduleRecordingsUpdate();
}
void ClientCoreBase::_UpdateEpgForAllChannels(time_t startTime, time_t endTime)
{
    if(/*endTime <= m_lastEpgRequestEndTime || */endTime <= startTime)
        return;
    
    if(m_epgUpdateInterval.IsSet() && m_epgUpdateInterval.TimeLeft() > 0){
        LogDebug("Can update EPG after %d sec",  m_epgUpdateInterval.TimeLeft()/1000);
        return;
    }
    
    //        startTime = std::max(startTime, m_lastEpgRequestEndTime);
    //        m_lastEpgRequestEndTime = endTime;
    
    LogDebug("Requested EPG update (all channel) for interval [%s -- %s]", time_t_to_string(startTime).c_str(), time_t_to_string(endTime).c_str());
    UpdateEpgForAllChannels(startTime, endTime);
}

#pragma  mark - Recordings
void ClientCoreBase::ScheduleRecordingsUpdate()
{
    if(nullptr == m_httpEngine)
        return;
    m_httpEngine->RunOnCompletionQueueAsync([this] {
        if(m_recordingsUpdateDelay.TimeLeft()) {
            ScheduleRecordingsUpdate();
        } else {
            auto phase =  static_cast<ClientPhase*> (GetPhase(k_EpgLoadingPhase));
            if(nullptr == phase) {
                return;
            }
            if(!phase->IsDone()) {
                phase->Broadcast();
                ReloadRecordings();
            }
        }
    }, [] (ActionQueue::ActionResult ){});
}

void ClientCoreBase::OnEpgUpdateDone()
{
    LogNotice("Archive iteraton started");
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
    LogNotice("Archive iteraton done");
}
void ClientCoreBase::ReloadRecordings()
{
    OnEpgUpdateDone();
}

int ClientCoreBase::UpdateArchiveInfoAndCount()
{
    int size = 0;
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    for(auto& i : m_epgEntries) {
        UpdateHasArchive(i.second);
        if(i.second.HasArchive)
            ++size;
    }
    return size;
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
        strError += n_to_string(error);
        strError += ").";
        LogError(strError.c_str());
        throw JsonParserException(strError);
    }
    parser(jsonRoot);
    return;
}

void ClientCoreBase::CallRpcAsync(const std::string & data,
                                  std::function<void(rapidjson::Document&)>  parser,
                                  ActionQueue::TCompletion completion)
{
    if(nullptr == m_httpEngine)
        return;
    
    // Build HTTP request
    std::string strRequest = string("http://") + "127.0.0.1" + ":" + n_to_string(m_rpcPort) + "/jsonrpc";
    auto start = P8PLATFORM::GetTimeMs();
    
    //    LogDebug("Calling '%s'.",  data.name.c_str());
    
    std::function<void(const std::string&)> parserWrapper = [=](const std::string& response) {
        LogDebug("Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);
        
        //            if(data.name.compare( "get_url") == 0)
        //                LogDebug(response.substr(0, 16380).c_str());
        
        ParseJson(response, [&] (Document& jsonRoot)
                  {
            if (!jsonRoot.HasMember("error"))
            {
                parser(jsonRoot);
                return;
            }
            const Value & errObj = jsonRoot["error"];
            auto err = errObj["message"].GetString();
            auto code = errObj["code"].GetInt();
            LogError("RPC call failed with error (%d):", code);
            LogError(err);
            throw RpcCallException(err);
        });
    };
    
    std::vector<std::string> headers;
    headers.push_back("Content-Type: application/json");
    //            headers = curl_slist_append(headers, "Accept: application/json");
    //headers = curl_slist_append(headers, "charsets: utf-8");
    
    m_httpEngine->CallApiAsync(HttpEngine::Request(strRequest, data, headers), parserWrapper,  [=](const ActionQueue::ActionResult& ss){completion(ss);});
}


}
