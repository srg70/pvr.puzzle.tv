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
        
        virtual std::shared_ptr<IPhase> GetPhase(Phase phase);
        
        virtual ~ClientCoreBase();
        
        const ChannelList &GetChannelList();
        const GroupList &GetGroupList();
        GroupId GroupForChannel(ChannelId chId);
        void RebuildChannelAndGroupList();

        void ReloadRecordings();
        int UpdateArchiveInfoAndCount();

        bool GetEpgEntry(UniqueBroadcastIdType i,  EpgEntry& enrty);
        void ForEachEpgLocked(const EpgEntryAction& action) const;
        void ForEachEpgUnlocked(const EpgEntryAction& predicate, const EpgEntryAction& action) const;
        void GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryAction& onEpgEntry);
        
        void SetRpcSettings(const RpcSettings& settings) {m_rpcSettings = settings;}
        void CheckRpcConnection();
        void CallRpcAsync(const std::string & data, std::function<void(rapidjson::Document&)>  parser,
                          ActionQueue::TCompletion completion);
        void IncludeCurrentEpgToArchive(AddCurrentEpgToArchive add) {m_addCurrentEpgToArchive = add;}
        void SetEpgCorrectionShift(int shift) {m_epgCorrectuonShift = shift; }
        void SetLocalLogosFolder(const std::string& logosFolder) {
            m_LocalLogosFolder = logosFolder;
            if(!m_LocalLogosFolder.empty() && m_LocalLogosFolder[m_LocalLogosFolder.size() - 1] == '/')
                m_LocalLogosFolder.pop_back();
        }
        
        static bool ReadFileContent(const char* cacheFile, std::string& buffer);
        // abstract methods
        virtual void UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled) = 0;
        virtual std::string GetUrl(ChannelId channelId) = 0;

    protected:
        ClientCoreBase(const RecordingsDelegate& didRecordingsUpadate = nullptr);
        
        // EPG methods
        P8PLATFORM::CTimeout m_epgUpdateInterval;

        static std::string MakeEpgCachePath(const char* cacheFile);
        void ClearEpgCache(const char* cacheFile, const char* epgUrl);
        void LoadEpgCache(const char* cacheFile);
        void SaveEpgCache(const char* cacheFile, unsigned int daysToPreserve = 7);
        UniqueBroadcastIdType AddEpgEntry(UniqueBroadcastIdType id, const EpgEntry& entry);
        bool AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry);
//        void UpdateEpgEntry(UniqueBroadcastIdType id, const EpgEntry& entry);

        // Channel & group lists
        void AddChannel(const Channel& channel);
        void AddGroup(GroupId groupId, const Group& group);
        void AddChannelToGroup(GroupId groupId, ChannelId channelId);
        void AddChannelToGroup(GroupId groupId, ChannelId channelId, int indexInGroup);
        void SetLocalPathForLogo(Channel& channel) const;
        
        void ParseJson(const std::string& response, std::function<void(rapidjson::Document&)> parser);
        
        // Should be called from destructor of dirived class
        void PrepareForDestruction();
        
        // Required methods to implement for derived classes
        virtual void Init(bool clearEpgCache) = 0;
        virtual void UpdateHasArchive(EpgEntry& entry) = 0;
        // Do not call directly, only through RebuildChannelAndGroupList()
        virtual void BuildChannelAndGroupList() = 0;

        
        HttpEngine* m_httpEngine;
        const ChannelList & m_channelList;
        const GroupList& m_groupList;
        AddCurrentEpgToArchive m_addCurrentEpgToArchive;
        int m_epgCorrectuonShift;

    private:
        
        // Recordings
        void OnEpgUpdateDone();
        void _UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled);
        void CallRpcAsyncImpl(const std::string & data, std::function<void(rapidjson::Document&)>  parser, ActionQueue::TCompletion completion);
        inline UniqueBroadcastIdType AddEpgEntryInternal(UniqueBroadcastIdType id, const EpgEntry& entry, EpgEntry** pAddedEntry = nullptr);

        ChannelList m_mutableChannelList;
        GroupList m_mutableGroupList;
        std::map<ChannelId, GroupId> m_channelToGroupLut;

        
        EpgEntryList m_epgEntries;
        mutable P8PLATFORM::CMutex m_epgAccessMutex;
        
        RecordingsDelegate m_didRecordingsUpadate;
        
        typedef std::map<IClientCore::Phase, std::shared_ptr<ClientPhase> > TPhases;
        TPhases m_phases;
        P8PLATFORM::CMutex m_phasesAccessMutex;

        
        time_t m_lastEpgRequestEndTime;
        RpcSettings m_rpcSettings;
        bool m_rpcWorks;
        bool m_destructionRequested;
        std::string m_LocalLogosFolder;
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
