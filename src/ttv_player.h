/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
 *
 *  Copyright (C) 2013 Alex Deryskyba (alex@codesnake.com)
 *  https://bitbucket.org/codesnake/pvr.sovok.tv_xbmc_addon
 *
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

#ifndef _ttv_player_h_
#define _ttv_player_h_

#include "client_core_base.hpp"
#include <vector>
#include <functional>
#include <list>
#include <set>
#include <map>
#include <memory>
#include "p8-platform/util/timeutils.h"
#include "p8-platform/threads/mutex.h"
#include "ActionQueue.hpp"
#include "helpers.h"

namespace XMLTV {
    struct EpgEntry;
    struct EpgChannel;
}

namespace TtvEngine
{
    typedef std::map<std::string, std::string> ParamList;

    class AuthFailedException : public PvrClient::ExceptionBase
    {
    };
    
    class BadPlaylistFormatException : public PvrClient::ExceptionBase
    {
    public:
        BadPlaylistFormatException(const char* r) : ExceptionBase(r) {}
    };
    
    class UnknownStreamerIdException : public PvrClient::ExceptionBase
    {
    public:
        UnknownStreamerIdException() : ExceptionBase("Unknown streamer ID.") {}
    };
    
    class MissingApiException : public PvrClient::ExceptionBase
    {
    public:
        MissingApiException(const char* r) : ExceptionBase(r) {}
    };
    
    class ServerErrorException : public PvrClient::ExceptionBase
    {
    public:
        ServerErrorException(const char* code) : ExceptionBase(code) {}
    };
    
    struct ArchiveInfo{
        ArchiveInfo(unsigned int d, const std::string& t)
        : days(d)
        , urlTemplate(t)
        {}
        ArchiveInfo(const ArchiveInfo & rf)
        : ArchiveInfo(rf.days, rf.urlTemplate)
        {}
        ArchiveInfo(ArchiveInfo && rv)
        : ArchiveInfo(rv.days, rv.urlTemplate)
        {}
        const unsigned int days;
        const std::string urlTemplate;
    };
    typedef std::map<PvrClient::ChannelId, ArchiveInfo> ArchiveInfos;
    
    class Core : public PvrClient::ClientCoreBase
    {
    public:
        struct CoreParams {
            CoreParams()
            : useAce(false)
            , enableAdult(false)
            , aceServerPort(0)
            {}
            std::string user;
            std::string password;
            bool useAce;
            bool enableAdult;
            std::string aceServerUri;
            int aceServerPort;
            std::string AceServerUrlBase() const
            {
                return std::string ("http://") + aceServerUri +":" + n_to_string(aceServerPort);
            }
        };
        Core(const CoreParams& coreParams);
        Core(const std::string &playListUrl, const std::string &epgUrl);
        ~Core();
        
        std::string GetArchiveUrl(PvrClient::ChannelId channelId, time_t startTime);
        void  UpdateEpgForAllChannels(time_t startTime, time_t endTime);

        std::string GetUrl(PvrClient::ChannelId channelId);
        std::string GetNextStream(PvrClient::ChannelId channelId, int currentChannelIdx);

        void ClearSession();
    protected:
        virtual void Init(bool clearEpgCache);
        virtual void UpdateHasArchive(PvrClient::EpgEntry& entry);
        virtual void BuildChannelAndGroupList();

    private:
        enum TTVChannelType{
            TTVChannelType_channel,
            TTVChannelType_moderation,
            TTVChannelType_translation
        };
        enum TTVChannelSource{
            TTVChannelSource_contentid,
            TTVChannelSource_torrent
        };
        enum TTVChannelAccess{
            TTVChannelAccess_all,
            TTVChannelAccess_registred,
            TTVChannelAccess_vip
        };
        struct TTVChanel {
            int epg_id;
            TTVChannelType type;
            TTVChannelSource source;
            bool isFavorite;
            TTVChannelAccess access;
            bool hasTsHQ;
            bool hasTsLQ;
            bool hasArchive;
            bool isFree;
            bool isAccessable;
            bool hasHTTPArchive;
            bool hasHTTPStream;
            bool isAdult;
            bool canTSProxy;
            bool canAceStream;
            bool canNoxbit;
            bool isHD;
        };
        
        struct Record {
            int id;
            int time;
            std::string name;
            bool hasScreen;
        };
        
        typedef std::map<PvrClient::ChannelId, TTVChanel> TTVChannels;
        TTVChannels m_ttvChannels;
        
        std::map<int, PvrClient::ChannelId> m_epgIdToChannelId;
        // map<epgId, map<startTime, record>>
        std::map<int, std::map<int, Record> > m_records;
        typedef std::set<const PvrClient::EpgEntryList::key_type> EpgForDetails;
        
        EpgForDetails m_epgForDetails;
        const CoreParams m_coreParams;
        std::string m_deviceId;
        std::string m_sessionId;

        bool m_hasTSProxy;
        bool m_isVIP;
        bool m_isRegistered;
        bool m_needsAdult;
        int m_zoneId;
        const bool m_useApi;

        // API helper methods
        typedef std::function<void(const ActionQueue::ActionResult&)> TApiCallCompletion;
        struct ApiFunctionData;
        template <typename TParser>
        void CallApiFunction(const ApiFunctionData& data, TParser parser);
        template <typename TParser>
        void CallApiAsync(const ApiFunctionData& data, TParser parser, TApiCallCompletion completion);
        
        // Epg management
        void LoadEpg();
        void ScheduleEpgDetails();
        PvrClient::UniqueBroadcastIdType AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry);
        PvrClient::UniqueBroadcastIdType AddEpgEntry(PvrClient::EpgEntry& epg);
        void GetEpgDetailsAsync();
//        void UpdateEpgEntry(PvrClient::UniqueBroadcastIdType id, const PvrClient::EpgEntry* entry);


        // Session
        void LoadSessionCache();
        void SaveSessionCache();
        void RenewSession();
        
        //API
        bool CheckAceEngineRunning();
        std::string GetPlaylistUrl();
        void GetUserInfo();
        int GetRecordId(PvrClient::ChannelId channelId, time_t startTime);
        void UpdateArchiveFor(time_t time);
        void BuildChannelAndGroupList_Api();
        std::string GetArchiveUrl_Api(PvrClient::ChannelId channelId, time_t startTime);
        std::string GetUrl_Api_Ace(PvrClient::ChannelId channelId);
        std::string GetUrl_Api_Http(PvrClient::ChannelId channelId);
        void UpdateEpgForAllChannels_Api(time_t startTime, time_t endTime);
        std::string GetUrl_Api(PvrClient::ChannelId channelId);

        // TSProxy plist
        void BuildChannelAndGroupList_Plist();
        std::string GetArchiveUrl_Plist(PvrClient::ChannelId channelId, time_t startTime);
        void UpdateEpgForAllChannels_Plist(time_t startTime, time_t endTime);

        void Cleanup();

        std::string m_playListUrl;
        std::string m_epgUrl;
        P8PLATFORM::CMutex m_recordingsGuard;
        ArchiveInfos m_archiveInfoPlist;
        bool m_isAceRunning;
        bool m_isRunnig;
   };
}
#endif //_ttv_player_h_
