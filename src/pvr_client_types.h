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

#ifndef pvr_client_types_h
#define pvr_client_types_h

#include <string>
#include <map>
#include <set>
#include <vector>
#include <memory>
#include <functional>
#include "ActionQueueTypes.hpp"
#include <rapidjson/document.h>

struct EPG_TAG;

namespace PvrClient {
    
    typedef unsigned int ChannelId;
    const ChannelId UnknownChannelId = (ChannelId) -1;
    // Although Kodi defines unique channel ID as unsigned integer
    // some Kodi modules require signed int internaly and reject negative values.
    typedef int KodiChannelId;

    struct Channel
    {
        typedef std::vector<std::string> UrlList;
        
        ChannelId UniqueId;
        ChannelId EpgId;
        unsigned int Number;
        std::string Name;
        std::string IconPath;
        UrlList Urls;
        bool HasArchive;
        bool IsRadio;
        int TvgShift;
        int PreloadingInterval;
       
        bool operator <(const Channel &anotherChannel) const
        {
            return UniqueId < anotherChannel.UniqueId;
        }
        Channel()
        : UniqueId(UnknownChannelId), EpgId (UnknownChannelId)
        , Number(0), TvgShift(0), PreloadingInterval(0)
        , HasArchive(false), IsRadio(false)
        {}
    };
    
    
    struct Group
    {
        std::string Name;
        std::map<int, ChannelId> Channels;
    };
    
    typedef std::map<ChannelId, Channel> ChannelList;
    typedef int GroupId;
    typedef std::map<GroupId, Group> GroupList;
    typedef std::set<ChannelId> FavoriteList;

    
    struct EpgEntry
    {
        ChannelId UniqueChannelId;
        unsigned int StartTime;
        unsigned int EndTime;
        std::string Title;
        std::string Description;
        bool HasArchive;
        std::string IconPath;
        // Used by TTV for async EPG details update
        std::string ProgramId;
        std::string Category;

        EpgEntry()
        : UniqueChannelId(UnknownChannelId)
        , StartTime(0)
        , EndTime(0)
        , HasArchive (false)
        {}
        
        void FillEpgTag(EPG_TAG& tag) const;
    };
    
    typedef unsigned int UniqueBroadcastIdType;
    const UniqueBroadcastIdType c_UniqueBroadcastIdUnknown = (UniqueBroadcastIdType)-1;
    typedef std::map<UniqueBroadcastIdType, EpgEntry> EpgEntryList;

    class IClientCorePhase {
        virtual void Wait() = 0;
        virtual bool IsDone() = 0;
        virtual ~IClientCorePhase(){}
    };
    
    class IClientCore
    {
    public:
        enum Phase{
            k_ChannelsLoadingPhase,
            k_ChannelsIdCreatingPhase,
            k_EpgCacheLoadingPhase,
            k_RecordingsInitialLoadingPhase,
            k_InitPhase,
            k_EpgLoadingPhase
        };
        class IPhase {
        public:
            virtual bool Wait(uint32_t iTimeout = 0) = 0;
            virtual bool IsDone() = 0;
            virtual void Broadcast() = 0;
        };

        struct RpcSettings {
            int port;
            std::string user;
            std::string password;
            bool is_secure;
        };

        typedef std::function<void(void)> RecordingsDelegate;
        typedef std::function<bool(const EpgEntryList::value_type&)> EpgEntryAction;
        
        virtual std::shared_ptr<IPhase> GetPhase(Phase phase) = 0;

        virtual const ChannelList& GetChannelList() = 0;
        virtual const GroupList &GetGroupList() = 0;
        virtual PvrClient::GroupId GroupForChannel(PvrClient::ChannelId chId) = 0;
        virtual void GetEpg(ChannelId  channelId, time_t startTime, time_t endTime, EpgEntryAction& onEpgEntry) = 0;
        virtual bool GetEpgEntry(UniqueBroadcastIdType i,  EpgEntry& enrty) = 0;
        virtual void ForEachEpgLocked(const EpgEntryAction& action) const = 0;
        virtual void ForEachEpgUnlocked(const EpgEntryAction& predicate, const EpgEntryAction& action) const = 0;
        virtual std::string GetUrl(PvrClient::ChannelId channelId) = 0;

        virtual void ReloadRecordings() = 0;
        virtual int UpdateArchiveInfoAndCount() = 0;
        virtual void SetRpcSettings(const RpcSettings& settings) = 0;
        virtual void CheckRpcConnection() = 0;
        virtual void SupportMuticastUrls(bool shouldSupport, const std::string& udpProxyHost = std::string(), uint32_t udpProxyPort = 0) = 0;
        virtual void CallRpcAsync(const std::string & data, std::function<void(rapidjson::Document&)>  parser,
                                  ActionQueue::TCompletion completion) = 0;
        
        virtual ~IClientCore(){}
    };
    
    enum AddCurrentEpgToArchive{
        k_AddCurrentEpgToArchive_No = 0,
        k_AddCurrentEpgToArchive_Yes = 1,
        k_AddCurrentEpgToArchive_AfterInit = 2
    } ;
 }

#endif /* pvr_client_types_h */
