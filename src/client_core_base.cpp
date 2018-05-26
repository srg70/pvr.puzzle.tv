//
//  client_core_base.cpp
//  comple.test
//
//  Created by Sergey Shramchenko on 22/05/2018.
//  Copyright © 2018 Home. All rights reserved.
//

#include <algorithm>
#include <rapidjson/error/en.h>
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include "p8-platform/util/StringUtils.h"
#include "p8-platform/threads/mutex.h"

#include "client_core_base.hpp"

namespace PvrClient{
    
    using namespace std;
    using namespace ADDON;
    using namespace rapidjson;
    
    static const char* c_EpgCacheDirPath = "special://temp/pvr-puzzle-tv";

    
    template< typename ContainerT, typename PredicateT >
    void erase_if( ContainerT& items, const PredicateT& predicate ) {
        for( auto it = items.begin(); it != items.end(); ) {
            if( predicate(*it) ) it = items.erase(it);
            else ++it;
        }
    };
    
    ClientCoreBase::ClientCoreBase(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                                   const IClientCore::RecordingsDelegate& didRecordingsUpadate)
    : m_addonHelper(addonHelper)
    , m_pvrHelper(pvrHelper)
    , m_isRebuildingChannelsAndGroups(false)
    , m_didRecordingsUpadate(didRecordingsUpadate)
    {
        if(nullptr == m_didRecordingsUpadate) {
            auto pvr = m_pvrHelper;
            m_didRecordingsUpadate = [pvr](){ pvr->TriggerRecordingUpdate();};
        }
    }
    
    ClientCoreBase::~ClientCoreBase()
    {
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);
        m_epgEntries.clear();
    };
    
#pragma mark - Channels & Groups

    const ChannelList& ClientCoreBase::GetChannelList()
    {
        if (m_channelList.empty() && !m_isRebuildingChannelsAndGroups)
        {
            RebuildChannelAndGroupList();
        }
        
        return m_channelList;
    }
    
    const GroupList &ClientCoreBase::GetGroupList()
    {
        if (m_groupList.empty()&& !m_isRebuildingChannelsAndGroups)
            RebuildChannelAndGroupList();
        
        return m_groupList;
    }
    
    void ClientCoreBase::RebuildChannelAndGroupList()
    {
        m_isRebuildingChannelsAndGroups = true;
        m_channelList.clear();
        m_groupList.clear();
        BuildChannelAndGroupList();
        m_isRebuildingChannelsAndGroups = false;
    }
    
    void ClientCoreBase::AddChannel(const Channel& channel)
    {
        m_channelList[channel.Id] = channel;
    }

    void ClientCoreBase::AddGroup(GroupId groupId, const Group& group)
    {
        m_groupList[groupId] = group;
    }
    void ClientCoreBase::AddChannelToGroup(GroupId groupId, ChannelId channelId)
    {
        m_groupList[groupId].Channels.insert(channelId);
    }
    
#pragma mark - EPG
    string ClientCoreBase::MakeEpgCachePath(const char* cacheFile)
    {
        return string(c_EpgCacheDirPath) + "/" + cacheFile;
    }
    void ClientCoreBase::ClearEpgCache(const char* cacheFile)
    {
        m_addonHelper->DeleteFile(MakeEpgCachePath(cacheFile).c_str());
    }
    
    void ClientCoreBase::LoadEpgCache(const char* cacheFile)
    {
        string cacheFilePath = MakeEpgCachePath(cacheFile);
        
        void* file = m_addonHelper->OpenFile(cacheFilePath.c_str(), 0);
        if(NULL == file)
            return;
        int64_t fSize = m_addonHelper->GetFileLength(file);
        
        char* rawBuf = new char[fSize + 1];
        if(0 == rawBuf)
            return;
        m_addonHelper->ReadFile(file, rawBuf, fSize);
        m_addonHelper->CloseFile(file);
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
                    AddEpgEntry(k,e);
                }
            });
            
        } catch (...) {
            LogError(" >>>>  FAILED load EPG cache <<<<<");
            m_epgEntries.clear();
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
        
        m_addonHelper->CreateDirectory(c_EpgCacheDirPath);

        void* file = m_addonHelper->OpenFileForWrite(cacheFilePath.c_str(), true);
        if(NULL == file)
            return;
        auto buf = s.GetString();
        m_addonHelper->WriteFile(file, buf, s.GetSize());
        m_addonHelper->CloseFile(file);
        
    }
    
    bool ClientCoreBase::AddEpgEntry(UniqueBroadcastIdType id, EpgEntry& entry)
    {
        // Do not add EPG for unknown channels
        if(m_channelList.count(entry.ChannelId) != 1)
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

    bool ClientCoreBase::GetEpgEpgEntry(UniqueBroadcastIdType i,  EpgEntry& entry)
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
            action(i);
        }
    }

#pragma  mark - Recordings
    
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
    

#pragma  mark - Logging
    
    # define PrintToLog(loglevel) \
    std::string strData; \
    strData.reserve(16384); \
    va_list va; \
    va_start(va, format); \
    strData = StringUtils::FormatV(format,va); \
    va_end(va); \
    m_addonHelper->Log(loglevel, strData.c_str()); \

    
    void ClientCoreBase::LogError(const char *format, ... )
    {
        PrintToLog(LOG_ERROR);
    }
    void ClientCoreBase::LogInfo(const char *format, ... )
    {
        PrintToLog(LOG_INFO);
    }
    void ClientCoreBase::LogNotice(const char *format, ... )
    {
        PrintToLog(LOG_NOTICE);
    }
    void ClientCoreBase::LogDebug(const char *format, ... )
    {
        PrintToLog(LOG_DEBUG);
    }
    
}
