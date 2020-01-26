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

#ifndef client_core_base_hpp
#define client_core_base_hpp

#include "pvr_client_types.h"
#include <rapidjson/document.h>
#include "ActionQueueTypes.hpp"
#include <functional>
#include "globals.hpp"

class HttpEngine;

namespace XMLTV {
    struct EpgEntry;
    struct EpgChannel;
}

namespace PvrClient {
    
    class ClientPhase;
    class ClientCoreBase :  public IClientCore
    {
    public:
        void InitAsync(bool clearEpgCache, bool updateRecordings);
        
        virtual IPhase* GetPhase(Phase phase);
        
        virtual ~ClientCoreBase();
        
        const PvrClient::ChannelList &GetChannelList();
        const PvrClient::GroupList &GetGroupList();
        PvrClient::GroupId GroupForChannel(PvrClient::ChannelId chId);
        void RebuildChannelAndGroupList();

        void ReloadRecordings();
        int UpdateArchiveInfoAndCount();

        bool GetEpgEntry(UniqueBroadcastIdType i,  EpgEntry& enrty);
        void ForEachEpgLocked(const EpgEntryAction& action) const;
        void ForEachEpgUnlocked(const EpgEntryAction& predicate, const EpgEntryAction& action) const;
        void GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryAction& onEpgEntry);
        
        void SetRpcPort(int port) {m_rpcPort = port;}
        void CallRpcAsync(const std::string & data, std::function<void(rapidjson::Document&)>  parser,
                          ActionQueue::TCompletion completion);
        void IncludeCurrentEpgToArchive(bool add) {m_addCurrentEpgToArchive = add;}

        static bool ReadFileContent(const char* cacheFile, std::string& buffer);
        // abstract methods
        virtual void UpdateEpgForAllChannels(time_t startTime, time_t endTime) = 0;
        virtual std::string GetUrl(PvrClient::ChannelId channelId) = 0;

    protected:
        ClientCoreBase(const RecordingsDelegate& didRecordingsUpadate = nullptr);
        
        // EPG methods
        P8PLATFORM::CTimeout m_epgUpdateInterval;

        static std::string MakeEpgCachePath(const char* cacheFile);
        void ClearEpgCache(const char* cacheFile);
        void LoadEpgCache(const char* cacheFile);
        void SaveEpgCache(const char* cacheFile, unsigned int daysToPreserve = 7);
        UniqueBroadcastIdType AddEpgEntry(UniqueBroadcastIdType id, EpgEntry& entry);
        bool AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry);
//        void UpdateEpgEntry(UniqueBroadcastIdType id, const EpgEntry& entry);

        // Channel & group lists
        void AddChannel(const Channel& channel);
        void AddGroup(GroupId groupId, const Group& group);
        void AddChannelToGroup(GroupId groupId, ChannelId channelId);
        void AddChannelToGroup(GroupId groupId, ChannelId channelId, int indexInGroup);

        void ParseJson(const std::string& response, std::function<void(rapidjson::Document&)> parser);
        
        // Should be called from destructor of dirived class
        void PrepareForDestruction();
        
        // Required methods to implement for derived classes
        virtual void Init(bool clearEpgCache) = 0;
        virtual void UpdateHasArchive(PvrClient::EpgEntry& entry) = 0;
        // Do not call directly, only through RebuildChannelAndGroupList()
        virtual void BuildChannelAndGroupList() = 0;

        
        HttpEngine* m_httpEngine;
        const PvrClient::ChannelList & m_channelList;
        const PvrClient::GroupList& m_groupList;
        bool m_addCurrentEpgToArchive;

    private:
        
        // Recordings
        void OnEpgUpdateDone();
        void _UpdateEpgForAllChannels(time_t startTime, time_t endTime);


        PvrClient::ChannelList m_mutableChannelList;
        PvrClient::GroupList m_mutableGroupList;
        std::map<PvrClient::ChannelId, PvrClient::GroupId> m_channelToGroupLut;

        
        PvrClient::EpgEntryList m_epgEntries;
        mutable P8PLATFORM::CMutex m_epgAccessMutex;
        
        RecordingsDelegate m_didRecordingsUpadate;
        std::map<IClientCore::Phase, ClientPhase*> m_phases;
        time_t m_lastEpgRequestEndTime;

        int m_rpcPort;
    };
    
    class ExceptionBase : public std::exception
    {
    public:
        const char* what() const noexcept {return reason.c_str();}
        const std::string reason;
        ExceptionBase(const std::string& r) : reason(r) {}
        ExceptionBase(const char* r = "") : reason(r) {}
        
    };
    
    class JsonParserException : public ExceptionBase
    {
    public:
        JsonParserException(const std::string& r) : ExceptionBase(r) {}
        JsonParserException(const char* r) : ExceptionBase(r) {}
    };
    class RpcCallException : public ExceptionBase
    {
    public:
        RpcCallException(const std::string& r) : ExceptionBase(r) {}
        RpcCallException(const char* r) : ExceptionBase(r) {}
    };

    
}

#endif /* client_core_base_hpp */
