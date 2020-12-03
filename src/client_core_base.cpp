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
#include <cstdio>
#include <rapidjson/error/en.h>
#include "rapidjson/writer.h"
#include "rapidjson/filewritestream.h"

#include "p8-platform/util/StringUtils.h"
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/util.h"

#include "client_core_base.hpp"
#include "globals.hpp"
#include "HttpEngine.hpp"
#include "helpers.h"
#include "XMLTV_loader.hpp"
#include "base64.h"
#include "JsonSaxHandler.h"

namespace PvrClient{

using namespace std;
using namespace ADDON;
using namespace rapidjson;
using namespace Globals;
using namespace Helpers;

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
    // timeout 0 == INFINITE
    bool Wait(uint32_t iTimeout = 0) {
        if(m_isDone)
            return true;
        bool res = m_event->Wait(iTimeout);
        if(res) {
            Cleanup();
        }
        return res;
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
    void Join(int iWaitMs)
    {
        if(nullptr == m_thread)
            return;
        if(!m_thread->StopThread(iWaitMs))
            return;
    }
private:
    void Cleanup() {
        m_isDone = true;
        
        static P8PLATFORM::CMutex s_CleanupMutex;
        P8PLATFORM::CLockObject lock(s_CleanupMutex);
        if(m_thread){
            // All time-consuming operations should be cancelled
            // when thread is stopped. Use 5 sec timeout.
            if(!m_thread->StopThread(5*1000))
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
, m_rpcWorks(false)
, m_destructionRequested(false)
, m_epgCorrectuonShift(0)
{
    if(nullptr == m_didRecordingsUpadate) {
        auto pvr = PVR;
        m_didRecordingsUpadate = [pvr](){ pvr->TriggerRecordingUpdate();};
    }
    m_httpEngine = new HttpEngine();
    m_phases.emplace(k_ChannelsLoadingPhase, std::move(TPhases::mapped_type(new ClientPhase())));
    m_phases.emplace(k_ChannelsIdCreatingPhase, std::move(TPhases::mapped_type(new ClientPhase())));
    m_phases.emplace(k_EpgCacheLoadingPhase, std::move(TPhases::mapped_type(new ClientPhase())));
    m_phases.emplace(k_RecordingsInitialLoadingPhase, std::move(TPhases::mapped_type(new ClientPhase())));
    m_phases.emplace(k_InitPhase, std::move(TPhases::mapped_type(new ClientPhase())));
    m_phases.emplace(k_EpgLoadingPhase, std::move(TPhases::mapped_type(new ClientPhase())));
}

void ClientCoreBase::InitAsync(bool clearEpgCache, bool updateRecordings)
{
    m_phases[k_InitPhase]->RunAndSignalAsync([this, clearEpgCache, updateRecordings] (std::function<bool(void)> isAborted){
        do {
            if(isAborted())
                break;
            // Load channels, groups and EPG cache
            Init(clearEpgCache);
            // Broadcast EPG cache loaded
            GetPhase(k_EpgCacheLoadingPhase)->Broadcast();

            if(isAborted())
                break;
            
            if(!updateRecordings){
                GetPhase(k_RecordingsInitialLoadingPhase)->Broadcast();
            }
            
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
                break;
            
            // Wait for recodings transfering to Kodi.
//            auto phase =  GetPhase(k_RecordingsInitialLoadingPhase);
//            if(phase) {
//                bool wait = true;
//                do {
//                    wait = !isAborted() && !phase->Wait(100);
//                } while(wait);
//            }
//
//            if(isAborted())
//                break;
            
            _UpdateEpgForAllChannels(startTime, endTime,isAborted);
 
            
        } while(false);
        // Broadcast EPG loaded in any case (to avoid deadlocks)
        GetPhase(k_EpgLoadingPhase)->Broadcast();
    });
    
}

std::shared_ptr<IClientCore::IPhase> ClientCoreBase::GetPhase(Phase phase)
{
    class DummyPhase : public IPhase
    {
    public:
        virtual bool Wait(uint32_t iTimeout = 0){return false;}
        virtual bool IsDone() {return false; }
        virtual void Broadcast() {};
        
    };
    
    P8PLATFORM::CTryLockObject lock(m_phasesAccessMutex);
    if(lock.IsLocked()) {
        if(m_phases.count(phase) > 0) {
            return m_phases[phase];
        }
    }
    return shared_ptr<IPhase>(new DummyPhase());
}

// Should be called from destructor of dirived class
// During engine destruction vitrual functions may be called...
void ClientCoreBase::PrepareForDestruction() {
    m_destructionRequested = true;
    if(NULL == m_httpEngine)
        return ;
    {
        P8PLATFORM::CLockObject lock(m_phasesAccessMutex);
        for (auto& ph : m_phases) {
            if(ph.second) {
                ph.second->Join(5000);
            }
        }
        m_phases.clear();
    }
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
    // NOTE: do NOT change phase name on this function
    // It is in-use by client_pvr_base. Other phase may cause to deadlock.
    // The pahse should be k_ChannelsIdCreatingPhase
    // but for technical reasons it is done by client_pvr_base (see usage)
    auto phase =  GetPhase(k_ChannelsLoadingPhase);   
    if(phase) {
        phase->Wait();
    }
    return m_channelList;
}

const GroupList &ClientCoreBase::GetGroupList()
{
    auto phase =  GetPhase(k_ChannelsIdCreatingPhase);
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
    auto phase =  GetPhase(k_ChannelsLoadingPhase);
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
void ClientCoreBase::ClearEpgCache(const char* cacheFile, const char* epgUrl)
{
    string cacheFilePath = MakeEpgCachePath(cacheFile);
    if(!XBMC->DeleteFile(cacheFilePath.c_str()))
       LogError("ClearEpgCache(): failed to delete EPG cache %s", cacheFilePath.c_str());

    if(nullptr == epgUrl)
        return;
    
    string compressedFilePath = XMLTV::GetCachedPathFor(epgUrl);
    if(!XBMC->DeleteFile(compressedFilePath.c_str()))
       LogError("ClearEpgCache(): failed to delete compressed EPG cache %s", compressedFilePath.c_str());
}

bool ClientCoreBase::ReadFileContent(const char* cacheFile, std::string& buffer)
{
    void* file = XBMC_OpenFile(cacheFile, 0);
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


namespace epg_cache{
const char* const EpgEntryIdName  = "k";
const char* const UniqueChannelIdName  = "ch";
const char* const StartTimeName  = "st";
const char* const EndTimeName  = "et";
const char* const TitleName  = "ti";
const char* const DescriptionName  = "de";
const char* const HasArchiveName  = "ar";
const char* const IconPathName  = "ip";
const char* const ProgramIdName  = "pi";
const char* const CategoryName  = "ca";

struct CachedEpgEntry : EpgEntry
{
    UniqueBroadcastIdType Key;
    CachedEpgEntry()
    : EpgEntry()
    , Key(c_UniqueBroadcastIdUnknown)
    {}
};
}

void ClientCoreBase::LoadEpgCache(const char* cacheFile)
{

    try {
        using namespace Helpers::Json;
        using namespace epg_cache;
        
        string cacheFilePath = XBMC->TranslateSpecialProtocol(MakeEpgCachePath(cacheFile).c_str());

        auto parser = ParserForObject<CachedEpgEntry>()
        .WithField(EpgEntryIdName, &CachedEpgEntry::Key)
        .WithField(UniqueChannelIdName, &CachedEpgEntry::UniqueChannelId)
        .WithField(StartTimeName, &CachedEpgEntry::StartTime)
        .WithField(EndTimeName, &CachedEpgEntry::EndTime)
        .WithField(TitleName, &CachedEpgEntry::Title)
        .WithField(DescriptionName, &CachedEpgEntry::Description, false)
        .WithField(HasArchiveName, &CachedEpgEntry::HasArchive, false)
        .WithField(IconPathName, &CachedEpgEntry::IconPath, false)
        .WithField(ProgramIdName, &CachedEpgEntry::ProgramId, false)
        .WithField(CategoryName, &CachedEpgEntry::Category, false);
        
        string parserError;
        bool succeded = ParseJsonFile(cacheFilePath.c_str(), parser, [&](const CachedEpgEntry& e){
                if(difftime(e.EndTime, m_lastEpgRequestEndTime) > 0 ) { // e.EndTime >  m_lastEpgRequestEndTime
                    m_lastEpgRequestEndTime = e.EndTime;
                }
                // Just add cached entry to list without any additional actions
                AddEpgEntryInternal(e.Key, e);
            return true;

        } , &parserError);
        if(!succeded){
            LogDebug("ClientCoreBase: parsing of EPG cache faled with error %s.", parserError.c_str());
            throw 1;
        }
        LogDebug("ClientCoreBase: parsing of EPG cache done.");
    } catch (...) {
        LogError("ClientCoreBase: FAILED load EPG cache.");
        m_epgEntries.clear();
        m_lastEpgRequestEndTime = 0;
    }
}

void ClientCoreBase::SaveEpgCache(const char* cacheFile, unsigned int daysToPreserve)
{
    XBMC->CreateDirectory(c_EpgCacheDirPath);
    string cacheFilePath = XBMC->TranslateSpecialProtocol(MakeEpgCachePath(cacheFile).c_str());
    FILE* fp = fopen(cacheFilePath.c_str(), "w"); // non-Windows use "w"
    if(NULL == fp) {
        LogError("ClientCoreBase: failed to open EPG cashe file for write. Path %s", cacheFilePath.c_str());
        return;
    }

    char writeBuffer[65536];
    FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
     
    {

        P8PLATFORM::CLockObject lock(m_epgAccessMutex);
        
        // Leave epg entries not older then 1 weeks from now
        time_t now = time(nullptr);
        auto oldest = now - daysToPreserve*24*60*60;
        erase_if(m_epgEntries,  [oldest] (const EpgEntryList::value_type& i)
        {
            return i.second.StartTime < oldest;
        });
        
        Writer<FileWriteStream> writer(os);
        writer.StartArray();                // Between StartArray()/EndArray(),
        for_each(m_epgEntries.begin(), m_epgEntries.end(),[&writer](const EpgEntryList::value_type& i) {
            using namespace epg_cache;
            
            writer.StartObject();               // Between StartObject()/EndObject(),
            // ID
            writer.Key(EpgEntryIdName);
            writer.Uint64(i.first);
            // Entry
            auto& entry = i.second;
            writer.Key(UniqueChannelIdName);
            writer.Uint(entry.UniqueChannelId);
            
            writer.Key(StartTimeName);
            writer.Int64(entry.StartTime);
            
            writer.Key(EndTimeName);
            writer.Int64(entry.EndTime);
            
            writer.Key(TitleName);
            writer.String(entry.Title.c_str());
            if(!entry.Description.empty()) {
                writer.Key(DescriptionName);
                writer.String(entry.Description.c_str());
            }
            if(entry.HasArchive) {
                writer.Key(HasArchiveName);
                writer.Bool(entry.HasArchive);
            }
            if(!entry.IconPath.empty()) {
                writer.Key(IconPathName);
                writer.String(entry.IconPath.c_str());
            }
            if(!entry.ProgramId.empty()) {
                writer.Key(ProgramIdName);
                writer.String(entry.ProgramId.c_str());
            }
            if(!entry.Category.empty()) {
                writer.Key(CategoryName);
                writer.String(entry.Category.c_str());
            }
            writer.EndObject();
        });
        writer.EndArray();
    }
    fclose(fp);
}

inline UniqueBroadcastIdType ClientCoreBase::AddEpgEntryInternal(UniqueBroadcastIdType id, const EpgEntry& entry, EpgEntry** pAddedEntry)
{
    bool isUnknownChannel = m_channelList.count(entry.UniqueChannelId) == 0;
    // Do not add EPG for unknown channels
    if(isUnknownChannel)
        return c_UniqueBroadcastIdUnknown;
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    while(m_epgEntries.count(id) != 0) {
        // Check duplicates.
        if(m_epgEntries[id].UniqueChannelId == entry.UniqueChannelId)
            return id;
        ++id;
    }
    if(nullptr != pAddedEntry)
        *pAddedEntry = &(m_epgEntries[id] = entry);
    else
        m_epgEntries[id] = entry;
    return id;
}


UniqueBroadcastIdType ClientCoreBase::AddEpgEntry(UniqueBroadcastIdType id, const EpgEntry& entry)
{
    EpgEntry* mutableEntry = nullptr;
    
    id = AddEpgEntryInternal(id, entry, &mutableEntry);
    if(c_UniqueBroadcastIdUnknown == id || mutableEntry == nullptr)
        return c_UniqueBroadcastIdUnknown;
    
    mutableEntry->StartTime += m_epgCorrectuonShift;
    mutableEntry->EndTime += m_epgCorrectuonShift;
    UpdateHasArchive(*mutableEntry);

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
    // Search for ALL channels with same EpgId
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


bool ClientCoreBase::GetEpgEntry(UniqueBroadcastIdType i,  EpgEntry& entry)
{
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    bool result = m_epgEntries.count(i) > 0;
    if(result)
        entry = m_epgEntries[i];
    return result;
}

void ClientCoreBase::ForEachEpgLocked(const EpgEntryAction& action) const
{
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    for(const auto& i : m_epgEntries) {
        if(m_destructionRequested || !action(i))
            return;
    }
}

void ClientCoreBase::ForEachEpgUnlocked(const EpgEntryAction& predicate, const EpgEntryAction& action) const
{
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    
    EpgEntryList::const_iterator it = m_epgEntries.begin();
    const EpgEntryList::const_iterator end = m_epgEntries.end();
    

    while(it != end){
        // Find relevat EPG item
        while (!predicate(*it)) {
            ++it;
            if(it == end || m_destructionRequested)
                return;
        }
        EpgEntryList::key_type key = it->first;
        lock.Unlock();
        // Perform action while EPG is unlocked
        // to avoid possible deadlock (Kodi access EPG during i.g. recording transfer)
        if(m_destructionRequested || !action(*it))
            return;
        // Lock EPG for future search
        // and renew iterator after potential EPG changes
        lock.Lock();
        it = m_epgEntries.find(key);
        if(it == end || m_destructionRequested)
            return;
        ++it;
    }
}

void ClientCoreBase::GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryAction& onEpgEntry)
{
    time_t lastEndTime = startTime;
    
    auto phase =  GetPhase(IClientCore::k_EpgLoadingPhase);
    const bool  isFastEpgLoopAvailable = !phase->IsDone();


    IClientCore::EpgEntryAction predicate = [channelId, startTime, endTime] (const EpgEntryList::value_type& i) {
        auto entryStartTime = i.second.StartTime;
        return  i.second.UniqueChannelId == channelId  &&
                entryStartTime >= startTime && entryStartTime < endTime;

    };
    IClientCore::EpgEntryAction action = [&lastEndTime, &onEpgEntry, isFastEpgLoopAvailable, &predicate] (const EpgEntryList::value_type& i)
    {
        // Optimisation: for first time we'll call ForEachEpgLocked()
        //  Check predicate in this case
        if(isFastEpgLoopAvailable) {
            if(!predicate(i)) {
                return true;
            }
        }
        lastEndTime = i.second.EndTime;
        onEpgEntry(i);
        return true;
    };
    if(isFastEpgLoopAvailable){
        ForEachEpgLocked(action);
    } else {
        ForEachEpgUnlocked(predicate, action);
    }
    
    // Do NOT request EPG from server until it become loaded in background.
    if(lastEndTime >= endTime || isFastEpgLoopAvailable) {
        return;
    }
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
    _UpdateEpgForAllChannels(/*epgRequestStart*/lastEndTime, endTime, [](){return false;});
        
}
void ClientCoreBase::_UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled)
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
    UpdateEpgForAllChannels(startTime, endTime, cancelled);
}

#pragma  mark - Recordings

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
    if(!m_rpcWorks){
         completion(ActionQueue::ActionResult(ActionQueue::kActionFailed, std::make_exception_ptr(RpcCallException("No RPC connection (wrong PRC port?)."))));
         return;
     }
    CallRpcAsyncImpl(data, parser, completion);
}

void ClientCoreBase::CallRpcAsyncImpl(const std::string & data,
                                  std::function<void(rapidjson::Document&)>  parser,
                                  ActionQueue::TCompletion completion)
{
    if(nullptr == m_httpEngine){
        completion(ActionQueue::ActionResult(ActionQueue::kActionFailed, std::make_exception_ptr(RpcCallException("m_httpEngine is NULL."))));
        return;
    }
    
    // Build HTTP request
    std::string strRequest = m_rpcSettings.is_secure ? "https://" : "http://";
    strRequest += "127.0.0.1:" + n_to_string(m_rpcSettings.port) + "/jsonrpc";
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
    headers.push_back("Content-Type:application/json");
    if(!m_rpcSettings.password.empty()) {
        std::string auth (m_rpcSettings.user + std::string(":") + m_rpcSettings.password);
        auth = base64_encode(reinterpret_cast<const unsigned char*>(auth.c_str()), auth.size());
        auth = "Authorization:Basic " + auth;
        headers.push_back(auth);
    }

    m_httpEngine->CallApiAsync(HttpEngine::Request(strRequest, data, headers), parserWrapper,  [=](const ActionQueue::ActionResult& ss){completion(ss);});
}

void ClientCoreBase::CheckRpcConnection()
{
    if(m_rpcWorks)
        return;
    
    LogDebug("ClientCoreBase: ping JSON-RPC...");

    std::string rpcPingCommand = R"({ "jsonrpc": "2.0", "method": "JSONRPC.Ping", "id": 1 })";
    CallRpcAsyncImpl(rpcPingCommand, [&](rapidjson::Document& jsonRoot){
        m_rpcWorks = true;
    }, [&](const ActionQueue::ActionResult& s){
        m_rpcWorks =  s.status == ActionQueue::kActionCompleted;
        LogDebug("ClientCoreBase: JSON-RPC %s.", (m_rpcWorks) ? "works" : "failed");
        if(!m_rpcWorks){
            XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32028));
        }
    });
}

}
