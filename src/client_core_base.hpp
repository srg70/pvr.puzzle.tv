//
//  client_core_base.hpp
//  comple.test
//
//  Created by Sergey Shramchenko on 22/05/2018.
//  Copyright © 2018 Home. All rights reserved.
//

#ifndef client_core_base_hpp
#define client_core_base_hpp

#include "pvr_client_types.h"
#include <rapidjson/document.h>
#include <functional>
#include "globals.hpp"

namespace PvrClient {
    
   class ClientCoreBase :  public IClientCore
    {
    public:
        
        virtual ~ClientCoreBase();
        
        const PvrClient::ChannelList &GetChannelList();
        const PvrClient::GroupList &GetGroupList();
        
        void ReloadRecordings();
        
        bool GetEpgEpgEntry(UniqueBroadcastIdType i,  EpgEntry& enrty);
        void ForEachEpg(const EpgEntryAction& action) const;

    protected:
        ClientCoreBase(const RecordingsDelegate& didRecordingsUpadate = nullptr);
        
        // EPG methods
        static std::string MakeEpgCachePath(const char* cacheFile);
        void ClearEpgCache(const char* cacheFile);
        void LoadEpgCache(const char* cacheFile);
        void SaveEpgCache(const char* cacheFile, unsigned int daysToPreserve = 7);
        bool AddEpgEntry(UniqueBroadcastIdType id, EpgEntry& entry);

        // Channel & group lists
        void RebuildChannelAndGroupList();
        void AddChannel(const Channel& channel);
        void AddGroup(GroupId groupId, const Group& group);
        void AddChannelToGroup(GroupId groupId, ChannelId channelId);

        // Recordings
        void OnEpgUpdateDone();

        void ParseJson(const std::string& response, std::function<void(rapidjson::Document&)> parser);


        // Required methods to implement for derived classes
        virtual void UpdateHasArchive(PvrClient::EpgEntry& entry) = 0;
        virtual void BuildChannelAndGroupList() = 0;

    private:

        PvrClient::ChannelList m_channelList;
        PvrClient::GroupList m_groupList;
        bool m_isRebuildingChannelsAndGroups;
        
        PvrClient::EpgEntryList m_epgEntries;
        mutable P8PLATFORM::CMutex m_epgAccessMutex;

        
        RecordingsDelegate m_didRecordingsUpadate;

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
    
    
}

#endif /* client_core_base_hpp */
