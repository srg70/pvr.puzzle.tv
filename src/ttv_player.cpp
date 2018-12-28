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

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/prettywriter.h"

#include <assert.h>
#include <algorithm>
#include <sstream>
#include <list>
#include <set>
#include <ctime>
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/util.h"
#include "p8-platform/util/StringUtils.h"
#include "helpers.h"
#include "ttv_player.h"
#include "HttpEngine.hpp"
#include "XMLTV_loader.hpp"
#include "globals.hpp"
#include "guid.hpp"

#define CATCH_API_CALL() \
catch (ServerErrorException& ex) { \
    auto err = ex.what(); \
    LogError("%s. TTV API error: %s", __FUNCTION__,  err); \
    if(strcmp(err, "noepg") != 0) { \
        char* message = XBMC->GetLocalizedString(32019); \
        XBMC->QueueNotification(QUEUE_ERROR, message, ex.what() ); \
        XBMC->FreeString(message); \
    } \
} catch(CurlErrorException& ex) { \
    LogError("%s. CURL error: %s", __FUNCTION__,  ex.what()); \
    XBMC->QueueNotification(QUEUE_ERROR, "CURL fatal error: %s", ex.what() ); \
} catch(std::exception& ex) { \
    LogError("%s. TTV generic error: %s", __FUNCTION__,  ex.what()); \
} \
catch(...) { \
    LogError("%s. Unknown TTV error.", __FUNCTION__); \
}

namespace TtvEngine
{
    using namespace Globals;
    using namespace std;
    using namespace ADDON;
    using namespace rapidjson;
    using namespace PvrClient;

    
    static const char* c_EpgCacheFile = "ttv_epg_cache.txt";
    static const char* c_SessionCacheDir = "special://temp/pvr-puzzle-tv";
    static const char* c_SessionCachePath = "special://temp/pvr-puzzle-tv/ttv_session.txt";

    static const char* c_TTV_API_URL_base = "http://1ttvapi.top/v3/";
    static const char* c_ACE_ENGINE_HLS_STREAM = "/ace/getstream?"; //"/ace/manifest.m3u8?";
    
    struct NoCaseComparator : binary_function<string, string, bool>
    {
        inline bool operator()(const string& x, const string& y) const
        {
            return StringUtils::CompareNoCase(x, y) < 0;
        }
    };

    typedef map<string, pair<Channel, string>, NoCaseComparator> PlaylistContent;
    static void ParseChannelAndGroup(const std::string& data, unsigned int plistIndex, PlaylistContent& channels, ArchiveInfos& archiveInfo);
    static void LoadPlaylist(const string& plistUrl, PlaylistContent& channels, ArchiveInfos& archiveInfo);

    
    struct Core::ApiFunctionData
    {
        ApiFunctionData(const char* _baseUrl,  const char* _name)
        : baseUrl(_baseUrl), name(_name) , priority(HttpEngine::RequestPriority_Low)
        {}
        std::string BuildRequest() const
        {
            ParamList::const_iterator runner = params.begin();
            ParamList::const_iterator first = runner;
            ParamList::const_iterator end = params.end();
            
            string query;
            for (; runner != end; ++runner)
            {
                query += runner == first ? "?" : "&";
                query += runner->first + '=' + runner->second;
            }
            std::string strRequest = baseUrl;
            strRequest += name + query;
            return strRequest;
        }
        std::string name;
        std::string baseUrl;
        ParamList params;
        HttpEngine::RequestPriority priority;
    };

    Core::Core(const CoreParams& coreParams)
    : m_hasTSProxy(false)
    , m_isVIP(false)
    , m_isRegistered(false)
    , m_needsAdult(false)
    , m_useApi(true)
    , m_coreParams(coreParams)
    , m_zoneId(0)
    {
        m_isRegistered = m_coreParams.user != "anonymous";
        LoadSessionCache();
        if(m_sessionId.empty()) {
            RenewSession();
        }
    }
    
    Core::Core(const std::string &playListUrl,  const std::string &epgUrl)
    : m_playListUrl(playListUrl)
    , m_epgUrl(epgUrl)
    , m_useApi(false)
    {
    }
    
    void Core::Init(bool clearEpgCache)
    {
        if(m_useApi && !m_sessionId.empty()) {
            GetUserInfo();
            m_isAceRunning = false;
            if(m_coreParams.useAce) {
                if(!CheckAceEngineRunning()) {
                    char* message = XBMC->GetLocalizedString(32021);
                    XBMC->QueueNotification(QUEUE_ERROR, message);
                    XBMC->FreeString(message); 
                }
            }
        }

        RebuildChannelAndGroupList();
        if(clearEpgCache) {
            ClearEpgCache(c_EpgCacheFile);
        } else {
            LoadEpgCache(c_EpgCacheFile);
        }
        if(m_useApi) {
            time_t now = time(nullptr);
            int archiveDays = 10;
            while(archiveDays--) {
                UpdateArchiveFor(now);
                now -= 24*60*60;
            }
        } else {
            LoadEpg();
        }
            
    }
    
    Core::~Core()
    {
        Cleanup();
    }
    
    void Core::Cleanup()
    {
        LogNotice("TtvPlayer stopping...");
        
        if(m_httpEngine){
            SAFE_DELETE(m_httpEngine);
        }
        
        LogNotice("TtvPlayer stopped.");
    }

#pragma mark - Core interface
    void Core::BuildChannelAndGroupList()
    {
        if(m_useApi) {
            BuildChannelAndGroupList_Api();
        } else {
            BuildChannelAndGroupList_Plist();
        }
    }
    
    std::string Core::GetArchiveUrl(ChannelId channelId, time_t startTime)
    {
        string url;
        if(m_useApi) {
            return GetArchiveUrl_Api(channelId, startTime);
        }
        return GetArchiveUrl_Plist(channelId, startTime);
        
    }

    void Core::UpdateHasArchive(PvrClient::EpgEntry& entry)
    {
        time_t now = time(nullptr);
        auto when = now - entry.StartTime;
        entry.HasArchive = false;
        if(when < 0)
            return;
        
        auto chId = entry.ChannelId;
        if(m_useApi) {
            if(m_ttvChannels.count(chId) == 0)
                return;
            if(!m_isAceRunning && !m_ttvChannels[chId].hasHTTPArchive)
                return;

//            if(m_epgIdToChannelId.count(m_ttvChannels.at(chId).epg_id) == 0)
//                return;
            
            entry.HasArchive = GetRecordId(chId, entry.StartTime) != 0;
        } else {
            if(m_archiveInfoPlist.count(chId) == 0)
                return;
            
            const time_t archivePeriod = m_archiveInfoPlist.at(entry.ChannelId).days * 24 * 60 * 60; //archive  days in secs
            entry.HasArchive = when < archivePeriod;
        }
    }
    
    void Core::UpdateEpgForAllChannels(time_t startTime, time_t endTime)
    {
        if(m_epgUpdateInterval.IsSet() && m_epgUpdateInterval.TimeLeft() > 0)
            return;
        
        m_epgUpdateInterval.Init(24*60*60*1000);
        
        if(m_useApi)
            UpdateEpgForAllChannels_Api(startTime, endTime);
        else
            UpdateEpgForAllChannels_Plist(startTime, endTime);

    }
    
    string Core::GetNextStream(ChannelId channelId, int currentChannelIdx)
    {
        auto& channelList = m_channelList;
        if(channelList.count( channelId ) != 1) {
            LogError(" >>>>   TTV::GetNextStream: unknown channel ID= %d <<<<<", channelId);
            return string();
        }
        auto& urls = channelList.at(channelId).Urls;
        if(urls.size() > currentChannelIdx + 1)
            return urls[currentChannelIdx + 1];
        return string();
        
    }
    
    string Core::GetUrl(ChannelId channelId)
    {
        if(m_useApi)
            return GetUrl_Api(channelId);
        return m_channelList.at(channelId).Urls[0];
    }

#pragma mark - API methods
    bool Core::CheckAceEngineRunning()
    {
        //http://127.0.0.1:6878/webui/api/service?method=get_version&format=jsonp
        ApiFunctionData apiData(m_coreParams.AceServerUrlBase().c_str(), "/webui/api/service");
        apiData.params["method"] = "get_version";
        apiData.params["format"] = "jsonp";
        apiData.params["callback"] = "mycallback";
        bool isRunning = false;
        P8PLATFORM::CEvent event;
        try{
            string strRequest = apiData.BuildRequest();
            m_httpEngine->CallApiAsync(strRequest,[&isRunning ] (const std::string& response)
                                       {
                                           LogDebug("Ace Engine version: %s", response.c_str());
                                           isRunning = response.find("version") != string::npos;
                                       }, [&isRunning, &event](const ActionQueue::ActionResult& s) {
                                           if(s.status != ActionQueue::kActionCompleted)
                                               isRunning = false;
                                           event.Signal();
                                       } , HttpEngine::RequestPriority_Hi);
            
        }
        CATCH_API_CALL();
        event.Wait();
        return m_isAceRunning = isRunning;
    }
    
    string Core::GetUrl_Api(ChannelId channelId)
    {
        if(m_channelList.count( channelId ) != 1) {
            LogError(" >>>>   TTVCore::GetUrl_Api: unknown channel ID= %d <<<<<", channelId);
            return string();
        }
        Channel ch = m_channelList.at(channelId);
        auto& urls = ch.Urls;
        if(urls.size() == 0) {
            auto url = GetUrl_Api_Ace(channelId);
            if(!url.empty())
                urls.push_back(url);
            url = GetUrl_Api_Http(channelId);
            if(!url.empty())
                urls.push_back(url);
            AddChannel(ch);
        }

        if(ch.Urls.size() == 0) {
            char* message  = XBMC->GetLocalizedString(32017);
            XBMC->QueueNotification(QUEUE_ERROR, message);
            XBMC->FreeString(message);
            return string();
        }
        return  ch.Urls[0];
    }
    
    string Core::GetUrl_Api_Ace(ChannelId channelId)
    {
        if(!m_isAceRunning) {
            LogNotice("Ace stream enging is not running. Check PVR parameters..", channelId);
            return string();
        }
        if(!m_ttvChannels.at(channelId).canAceStream) {
            LogNotice("TTV channel %d has no Ace stream available.", channelId);
            return string();
        }
        
        string source, type;
        ApiFunctionData apiData(c_TTV_API_URL_base, "translation_stream.php");
        apiData.params["channel_id"] = n_to_string(channelId);
        apiData.priority = HttpEngine::RequestPriority_Hi;
        try {
            CallApiFunction(apiData, [&source, &type] (Document& jsonRoot)
                            {
                                source = jsonRoot["source"].GetString();
                                type = jsonRoot["type"].GetString();
                            });
        }
        CATCH_API_CALL();
        if(!source.empty())
            source = HttpEngine::Escape(source);
        if(type == "contentid")
            return m_coreParams.AceServerUrlBase() + c_ACE_ENGINE_HLS_STREAM + "id=" + source + "&pid=" + m_deviceId;
        if(type == "torrent")
            return m_coreParams.AceServerUrlBase() + c_ACE_ENGINE_HLS_STREAM +"url=" + source + "&pid=" + m_deviceId;
        
        return string();

    }
    
    string Core::GetUrl_Api_Http(ChannelId channelId)
    {
//        if(!m_ttvChannels.at(channelId).hasHTTPStream/*canTSProxy*/) {
//            LogNotice("TTV channel %d has no HTTP stream available.", channelId);
//            return string();
//        }
        
        string url;
        ApiFunctionData apiData(c_TTV_API_URL_base, "translation_http.php");
        apiData.params["channel_id"] = n_to_string(channelId);
        apiData.params["nohls"] = "0";
        apiData.params["hls_bandwidth_preset"] = "0";
        apiData.params["zone_id"] = n_to_string(m_zoneId);
        apiData.priority = HttpEngine::RequestPriority_Hi;
        try {
            CallApiFunction(apiData, [&url] (Document& jsonRoot)
                            {
                                url = jsonRoot["source"].GetString();
                            });
        }
        CATCH_API_CALL();
        return url;
        
    }

    int Core::GetRecordId(ChannelId channelId, time_t startTime)
    {
        int epdId = m_ttvChannels[channelId].epg_id;
        if(epdId == 0)
            return 0;

        P8PLATFORM::CLockObject lock(m_recordingsGuard);

        if(m_records.count(epdId) != 0) {
            if(m_records[epdId].count(startTime) != 0)
                return m_records[epdId][startTime].id;
        }
       UpdateArchiveFor(startTime);
//        if(m_records.count(epdId) != 0) {
//            if(m_records[epdId].count(startTime) != 0)
//                return m_records[epdId][startTime].id;
//        }
        return 0;
    }
    
    void Core::UpdateArchiveFor(time_t tm)
    {
        // Do not update future records.
        if(tm > time(nullptr))
            return;
        
        char mbstr[100];
        if (0 == std::strftime(mbstr, sizeof(mbstr), "X%d-X%m-%Y", std::localtime(&tm))) {
            return;
        }
        static set<string> s_updatingRecordingDays;

        string archiveDate(mbstr);
        StringUtils::Replace(archiveDate, "X0","X");
        StringUtils::Replace(archiveDate,"X","");
        auto it = s_updatingRecordingDays.insert( archiveDate);
        if(!it.second) // not inserted, i.e.exists
            return;
        auto dayToRemoveWhenDone = it.first;
        try {
            
            int record_id = 0;
            ApiFunctionData apiData(c_TTV_API_URL_base, "arc_records.php");
            apiData.params["date"] = archiveDate;
            apiData.params["epg_id"] = "all";
            apiData.params["hls"] = "1";
//            apiData.priority = HttpEngine::RequestPriority_Hi;
            auto pThis = this;
            CallApiAsync(apiData, [pThis] (Document& jsonRoot)
                            {
                                if(!jsonRoot.HasMember("records"))
                                    return;
                                auto& records = jsonRoot["records"];
                                if(!records.IsArray())
                                    return;
                                P8PLATFORM::CLockObject lock(pThis->m_recordingsGuard);
                                for (auto& r : records.GetArray()) {
                                    Record record;
                                    record.id = r["record_id"].GetInt();
                                    record.name = r["name"].GetString();
                                    record.hasScreen = r["screen"].GetInt() != 0;
                                    
                                    pThis->m_records[r["epg_id"].GetInt()][r["time"].GetInt()] = record;
                                }
                            }, [dayToRemoveWhenDone, pThis](const ActionQueue::ActionResult& s)  {
                                P8PLATFORM::CLockObject lock(pThis->m_recordingsGuard);
                                s_updatingRecordingDays.erase(dayToRemoveWhenDone);
                            });
        }
        CATCH_API_CALL();

    }
    
    std::string Core::GetArchiveUrl_Api(ChannelId channelId, time_t startTime)
    {
        string url;
        int record_id = GetRecordId(channelId, startTime);
        if(0 == record_id)
            return url;
        try{
            if(m_isAceRunning) {
                string source, type;
                ApiFunctionData apiData(c_TTV_API_URL_base, "arc_stream.php");
                apiData.params["record_id"] = n_to_string(record_id);
                apiData.priority = HttpEngine::RequestPriority_Hi;
                try {
                    CallApiFunction(apiData, [&source, &type] (Document& jsonRoot)
                                    {
                                        source = jsonRoot["source"].GetString();
                                        type = jsonRoot["type"].GetString();
                                    });
                }
                CATCH_API_CALL();
                if(!source.empty())
                    source = HttpEngine::Escape(source);
                if(type == "contentid")
                    return m_coreParams.AceServerUrlBase() + c_ACE_ENGINE_HLS_STREAM + "id=" + source + "&pid=" + m_deviceId;
                if(type == "torrent")
                    return m_coreParams.AceServerUrlBase() + c_ACE_ENGINE_HLS_STREAM + "url=" + source + "&pid=" + m_deviceId;
            }
            
            // check HTTP archive assess
            if(!m_ttvChannels[channelId].hasHTTPArchive)
                return url;
            
            ApiFunctionData apiData(c_TTV_API_URL_base, "arc_http.php");
            apiData.params["record_id"] = n_to_string(record_id);
            apiData.priority = HttpEngine::RequestPriority_Hi;
            CallApiFunction(apiData, [&url] (Document& jsonRoot)
                            {
                                url = jsonRoot["source"].GetString();
                                LogDebug(">> !!! >> TTV archive URL: %s", url.c_str());
                            });
            
        }
        CATCH_API_CALL();
        return url;
    }
    // Linux compilation issue workarround
    // Call protected base method from private method
    // instead of directly from lambda.
    void Core::AddEpgEntry(PvrClient::EpgEntry& epg)
    {
        UniqueBroadcastIdType id = epg.StartTime;
        ClientCoreBase::AddEpgEntry(id, epg);
    }
    void Core::UpdateEpgForAllChannels_Api(time_t startTime, time_t endTime)
    {
        auto epgOffset = - XMLTV::LocalTimeOffset();
        
        // Validate epg_id
        std::list<ChannelId> validCahnnels;
        for (const auto& ch : m_channelList) {
            auto chId = ch.second.Id;
            const auto& ttvCh = m_ttvChannels[chId];
            if(ttvCh.epg_id != 0) {
                validCahnnels.push_back(chId);
            }
        }
        
        size_t channelsToProcess = validCahnnels.size();
        
        for (const auto& chId : validCahnnels) {
            const auto& ttvCh = m_ttvChannels[chId];
            const bool isLast = --channelsToProcess == 0;
            if(isLast)
                LogDebug("Last EPG channel ID = %d", chId);
            ApiFunctionData apiData(c_TTV_API_URL_base, "translation_epg.php");
            apiData.params["btime"] = n_to_string(startTime);
            apiData.params["etime"] = n_to_string(endTime);
            apiData.params["epg_id"] = n_to_string(ttvCh.epg_id);
            
            auto pThis = this;
            CallApiAsync(apiData, [pThis, chId, epgOffset] (Document& jsonRoot)
             {
                 if(!jsonRoot.HasMember("data"))
                     return;
                 
                 auto& data = jsonRoot["data"];
                 if(!data.IsArray())
                     return;
                 
                 for(auto& epg : data.GetArray()) {
                     EpgEntry epgEntry;
                     epgEntry.ChannelId = chId ;
                     epgEntry.Title = epg["name"].GetString();
                     //epgEntry.Description = m.value["descr"].GetString();
                     epgEntry.StartTime = epg["btime"].GetInt();//  + epgOffset;
                     epgEntry.EndTime = epg["etime"].GetInt();// + epgOffset;
                     pThis->AddEpgEntry(epgEntry);
                 }
                 
             },
             [pThis, isLast, chId](const ActionQueue::ActionResult& s) {
                 try {
                     if(isLast)
                         pThis->SaveEpgCache(c_EpgCacheFile, 11);
                     if(s.exception)
                         std::rethrow_exception(s.exception);
                     PVR->TriggerEpgUpdate(chId);
                 }
                 CATCH_API_CALL();
             });
        }
    }
    
    void Core::GetUserInfo()
    {
        string url;
        ParamList params;

        try {
            ApiFunctionData apiData(c_TTV_API_URL_base,"userinfo.php");
            
            auto pThis = this;
            CallApiFunction(apiData, [&pThis] (Document& jsonRoot)
                            {
                                pThis->m_hasTSProxy = jsonRoot["tsproxy_access"].GetInt() == 1;
                                pThis->m_isVIP = jsonRoot["vip_status"].GetInt() == 1;
                                pThis->m_needsAdult = jsonRoot["adult"].GetInt() == 1;
                                pThis->m_zoneId = jsonRoot["zone_id"].GetInt() == 1;
                            });
        }
        CATCH_API_CALL();
    }
    
    std::string Core::GetPlaylistUrl()
    {
        string url;
       
        ApiFunctionData apiData(c_TTV_API_URL_base, "user_playlist_url.php");
//        apiData.params["cat_group"] = "0";
//        apiData.params["cat"] = "0";
//        apiData.params["ts"] = "0";
//        apiData.params["fav"] = "0";
//        apiData.params["cp1251"] = "0";
        apiData.params["hls"] = "1";
//        apiData.params["cat_fav"] = "0";
//        apiData.params["cat_tag"] = "0";
        apiData.params["format"] = "m3u";
        apiData.params["tag"] = "1";
        apiData.params["archive"] = "1";
        try {

            CallApiFunction(apiData, [&url] (Document& jsonRoot)
                            {
                                url = jsonRoot["url"].GetString();
                                LogDebug(">> !!! >> TTV plist URL: %s", url.c_str());
                            });
        }
        CATCH_API_CALL();

        return url;
    }

    void Core::BuildChannelAndGroupList_Api()
    {
        try {

            ApiFunctionData apiData(c_TTV_API_URL_base, "translation_list.php");
            apiData.params["type"] = "all";

            auto pThis = this;
            CallApiFunction(apiData, [pThis] (Document& jsonRoot)
            {
                int maxGroupId = 0;
                for(auto& gr : jsonRoot["categories"].GetArray())
                {
                    auto id = gr["id"].GetInt();
                    if(maxGroupId < id)
                        maxGroupId = id;
                    Group group;
                    group.Name = gr["name"].GetString();
                    pThis->AddGroup(id, group);
                }
                const int c_favoritesGroupId = ++maxGroupId;
                const int c_hdGroupId = ++maxGroupId;
                {
                    Group group;
                    char* groupName = XBMC->GetLocalizedString(32020);
                    group.Name = groupName;
                    XBMC->FreeString(groupName);
                    pThis->AddGroup(c_favoritesGroupId, group);

                    group.Name = "HD";
                    pThis->AddGroup(c_hdGroupId, group);
                }
                unsigned int channelNumber = 1;
                for(auto& ch : jsonRoot["channels"].GetArray())
                {
                    TTVChanel ttvChannel;
                    Channel channel;
                    
                    channel.Id = ch["id"].GetInt();
                    channel.Number = channelNumber++;// channel.Id;
                    channel.Name = ch["name"].GetString();
                    channel.IconPath = string("http://1ttv.org/uploads/") + ch["logo"].GetString();
                    channel.IsRadio = false;
                    channel.HasArchive = ttvChannel.hasArchive = ch["access_archive"].GetInt() != 0;


                    ttvChannel.epg_id = ch["epg_id"].GetInt();
                    auto chType = string(ch["type"].GetString());
                    ttvChannel.type = chType == "channel" ? TTVChannelType_channel :
                                        chType == "moderation" ? TTVChannelType_moderation : TTVChannelType_translation;
                    auto chSource = string(ch["source"].GetString());
                    ttvChannel.source = chSource == "contentid" ? TTVChannelSource_contentid : TTVChannelSource_torrent;
                    ttvChannel.isFavorite = ch["favourite_position"].GetInt() != 0;
                    auto chAccess = string(ch["access_translation"].GetString());
                    ttvChannel.access = chAccess == "registred" ? TTVChannelAccess_registred :
                                    chAccess == "vip" ? TTVChannelAccess_vip : TTVChannelAccess_all;
                    ttvChannel.hasTsHQ = ch["quality_ts_hq"].GetInt() != 0;
                    ttvChannel.hasTsLQ = ch["quality_ts_lq"].GetInt() != 0;
                    ttvChannel.isFree = ch["access_free"].GetInt() != 0;
                    ttvChannel.isAccessable = ch["access_user"].GetInt() != 0;
                    ttvChannel.hasHTTPArchive = ch["access_user_archive_http"].GetInt() != 0;
                    ttvChannel.hasHTTPStream = ch["access_user_http_stream"].GetInt() != 0;
                    ttvChannel.isAdult = ch["adult"].GetInt() != 0;
                    ttvChannel.canNoxbit = ch["noxbit_on_air"].GetInt() != 0;
                    ttvChannel.canTSProxy = ch["ts_on_air"].GetInt() != 0;
                    ttvChannel.canAceStream = ch["as_on_air"].GetInt() != 0;
                    ttvChannel.isHD = ch["hd_flag"].GetInt() != 0;

                    // filter channels
                    if(!pThis->m_isAceRunning &&  !ttvChannel.hasHTTPStream)
                        continue;
                    if(ttvChannel.access == TTVChannelAccess_vip)
                        if(!pThis->m_isVIP)
                            continue;
                    if(ttvChannel.access == TTVChannelAccess_registred)
                        if(!pThis->m_isVIP && ! pThis->m_isRegistered)
                            continue;
                    if(ttvChannel.isAdult && !pThis->m_needsAdult)
                        continue;

                    dump_json(ch);

                    if(ttvChannel.epg_id != 0)
                        pThis->m_epgIdToChannelId[ttvChannel.epg_id] = channel.Id;

                    // Add channel
                    pThis->AddChannel(channel);
                    pThis->m_ttvChannels[channel.Id] = ttvChannel;
                  
                    // Manage groups
                    pThis->AddChannelToGroup(ch["group"].GetInt(), channel.Id);
                    if(ttvChannel.isFavorite)
                        pThis->AddChannelToGroup(c_favoritesGroupId, channel.Id);
                    if(ttvChannel.isHD)
                        pThis->AddChannelToGroup(c_hdGroupId, channel.Id);

                    
//                    LogDebug("Channel %s: Stream - %s, Archive - %s", channel.Name.c_str(),
//                             ttvChannel.hasHTTPStream ? "YES" : "NO",
//                             ttvChannel.hasHTTPArchive ? "YES" : "NO");
                }
            });
            // Fill archive info
            
        }
        CATCH_API_CALL();

    }
//    void Core::InitializeArchiveInfo()
//    {
//        try {
//            m_epgIdToChannelId.clear();
//
//            ApiFunctionData apiData(c_TTV_API_URL_base, "arc_list.php");
//
//            auto pThis = this;
//            CallApiFunction(apiData, [pThis] (Document& jsonRoot)
//            {
//                dump_json(jsonRoot);
//                int total = 0, available = 0;
//                for(auto& ch : jsonRoot["channels"].GetArray())
//                {
//                    ++total;
//                    bool hasArchiveAssess = ch["access_user"].IsBool() && ch["access_user"].IsTrue();
////                    if(!hasArchiveAssess) {
////                        LogDebug("You do not have access to archive for channel %s", ch["name"].GetString());
////                        continue;
////                    }
//                    ++available;
//                    pThis->m_epgIdToChannelId[ch["epg_id"].GetInt()] = ch["id"].GetInt();
//                }
//                LogDebug("Total channel with archive %d. Available %d", total, available);
//            });
//        }
//        CATCH_API_CALL();
//
//    }
//
    template <typename TParser>
    void Core::CallApiFunction(const ApiFunctionData& data, TParser parser)
    {
        P8PLATFORM::CEvent event;
        std::exception_ptr ex = nullptr;
        CallApiAsync(data, parser, [&](const ActionQueue::ActionResult& s) {
            ex = s.exception;
            event.Signal();
        });
        event.Wait();
        if(ex)
        std::rethrow_exception(ex);
    }
    
    template <typename TParser>
    void Core::CallApiAsync(const ApiFunctionData& data, TParser parser, TApiCallCompletion completion)
    {
        
        const bool isLoginCommand = data.name == "auth.php";// || data.name == "logout";
        // Build HTTP request
        ApiFunctionData localData(data);
        localData.params["typeresult"] = "json";
        if(!isLoginCommand) {
            localData.params["session"] = m_sessionId;
        }
        string strRequest = localData.BuildRequest();
        auto start = P8PLATFORM::GetTimeMs();
        LogDebug("Calling '%s'.",  data.name.c_str());
        
        auto pThis = this;
        
        std::function<void(const std::string&)> parserWrapper = [pThis, start, parser, strRequest, isLoginCommand](const std::string& response) {
            LogDebug("Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);

            pThis->ParseJson(response, [parser] (Document& jsonRoot)
                             {
                                 if (jsonRoot.HasMember("success") && jsonRoot["success"].GetInt() == 1)
                                 {
                                     parser(jsonRoot);
                                     return;
                                 }
                                 const char* code = jsonRoot["error"].GetString();
                                 LogError("Torrent TV server responses error: %s", code);
                                 throw ServerErrorException(code);
                             });
        };
        ActionQueue::TCompletion comp =  [pThis, data, parser, completion, isLoginCommand](const ActionQueue::ActionResult& s)
        {
            // Do not re-login within login/logout command.
            if(s.status == ActionQueue::kActionFailed && !isLoginCommand) {
                try{
                    std::rethrow_exception(s.exception);
                }
                catch(ServerErrorException& ex) {
                    if(ex.reason == "incorrect") {
                        // In case of error try to re-login and repeat the API call.
                        pThis->RenewSession();
                        pThis->CallApiAsync(data, parser,  [completion](const ActionQueue::ActionResult& ss){completion(ss);});
                        return;
                    }
                }
                catch(...) {
                    // Let caller to handle exceptions
                }
            }
            completion(s);
        };
        m_httpEngine->CallApiAsync(strRequest, parserWrapper, comp, data.priority);
    }
    
#pragma mark - API Session
    void Core::LoadSessionCache()
    {
        string ss;
        if(!ReadFileContent(c_SessionCachePath, ss))
        return;
        
        try {
            ParseJson(ss, [&] (Document& jsonRoot) {
                m_deviceId =  jsonRoot["m_deviceId"].GetString();
                m_sessionId = jsonRoot["m_sessionId"].GetString();
            });
            
        } catch (...) {
            LogError(" TTV Engine: FAILED load session cache.");
            // Do not clear deviceId if loaded.
            // it should be generated one time for device.
            //m_deviceId.clear();
            m_sessionId.clear();
        }
    }
    
    void Core::SaveSessionCache()
    {
        
        StringBuffer s;
        Writer<StringBuffer> writer(s);
        
        writer.StartObject();
        writer.Key("m_deviceId");
        writer.String(m_deviceId.c_str(), m_deviceId.size());
        writer.Key("m_sessionId");
        writer.String(m_sessionId.c_str(), m_sessionId.size());
        writer.EndObject();
        
        XBMC->CreateDirectory(c_SessionCacheDir);
        
        void* file = XBMC->OpenFileForWrite(c_SessionCachePath, true);
        if(NULL == file)
        return;
        auto buf = s.GetString();
        XBMC->WriteFile(file, buf, s.GetSize());
        XBMC->CloseFile(file);
    }
    
    void Core::ClearSession()
    {
        if(XBMC->FileExists(c_SessionCachePath, false)) {
            XBMC->DeleteFile(c_SessionCachePath);
            ClearEpgCache(c_EpgCacheFile);
        }
    }
    
    void Core::RenewSession(){
        // Check device ID
        if(m_deviceId.empty()) {
            m_deviceId = CUSTOM_GUID::generate();
        }
        
        // Authorize to TTV
        ApiFunctionData apiData(c_TTV_API_URL_base, "auth.php");
        apiData.params["username"] = m_coreParams.user;
        apiData.params["password"] = m_coreParams.password;
        apiData.params["application"] = "xbmc";
        apiData.params["guid"] = m_deviceId;
        try {
            CallApiFunction(apiData, [&] (Document& jsonRoot)
                            {
                                m_sessionId = jsonRoot["session"].GetString();
                            });
            SaveSessionCache();
        }
        catch(std::exception& ex) {
            LogError("TTV server authorisation error: %s", ex.what());
            //            if(strcmp(ex.what(), "incorrect") == 0)
            throw AuthFailedException();
        }
        catch(...) {
            LogError("Unknown TTV server authorisation error.");
        }
    }
    
#pragma mark - Playlist methods
    bool Core::AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
    {
        unsigned int id = xmlEpgEntry.startTime;
        
        EpgEntry epgEntry;
        epgEntry.ChannelId = xmlEpgEntry.iChannelId;
        epgEntry.Title = xmlEpgEntry.strTitle;
        epgEntry.Description = xmlEpgEntry.strPlot;
        epgEntry.StartTime = xmlEpgEntry.startTime;
        epgEntry.EndTime = xmlEpgEntry.endTime;
        return ClientCoreBase::AddEpgEntry(id, epgEntry);
    }
    
    std::string Core::GetArchiveUrl_Plist(ChannelId channelId, time_t startTime)
    {
        string url;
        if(m_archiveInfoPlist.count(channelId) == 0) {
            return url;
        }
        url = m_archiveInfoPlist.at(channelId).urlTemplate;
        if(url.empty())
        return url;
        const char* c_START = "${start}";
        size_t pos = url.find(c_START);
        if(string::npos == pos)
        return string();
        url.replace(pos, strlen(c_START), n_to_string(startTime));
        const char* c_STAMP = "${timestamp}";
        pos = url.find(c_STAMP);
        if(string::npos == pos)
        return string();
        url.replace(pos, strlen(c_STAMP), n_to_string(time(nullptr)));
        return  url;
    }

    void Core::UpdateEpgForAllChannels_Plist(time_t startTime, time_t endTime)
    {
        using namespace XMLTV;
        try {
            auto pThis = this;
            
            set<ChannelId> channelsToUpdate;
            EpgEntryCallback onEpgEntry = [&pThis, &channelsToUpdate,  startTime] (const XMLTV::EpgEntry& newEntry) {
                if(pThis->AddEpgEntry(newEntry) && newEntry.startTime >= startTime)
                channelsToUpdate.insert(newEntry.iChannelId);
            };
            
            XMLTV::ParseEpg(m_epgUrl, onEpgEntry);
            
            SaveEpgCache(c_EpgCacheFile, 11);
        } catch (...) {
            LogError(" >>>>  FAILED receive EPG <<<<<");
        }
    }

    void Core::BuildChannelAndGroupList_Plist()
    {
        using namespace XMLTV;
        ArchiveInfos archiveInfo;
        PlaylistContent plistContent;
        LoadPlaylist(m_playListUrl, plistContent, archiveInfo);
        
        auto pThis = this;
        
        ChannelCallback onNewChannel = [&pThis, &plistContent, &archiveInfo](const EpgChannel& newChannel){
            if(plistContent.count(newChannel.strName) != 0) {
                auto& plistChannel = plistContent[newChannel.strName].first;
                if(plistChannel.HasArchive) {
                    pThis->m_archiveInfoPlist.emplace(newChannel.id, archiveInfo.at(plistChannel.Id));
                    archiveInfo.erase(plistChannel.Id);
                }
                plistChannel.Id = newChannel.id;
                if(!newChannel.strIcon.empty())
                    plistChannel.IconPath = newChannel.strIcon;
            }
        };
        
        XMLTV::ParseChannels(m_epgUrl, onNewChannel);
        
        for(const auto& channelWithGroup : plistContent)
        {
            const auto& channel = channelWithGroup.second.first;
            const auto& groupName = channelWithGroup.second.second;
            
            AddChannel(channel);
            
            const auto& groupList = m_groupList;
            auto itGroup =  std::find_if(groupList.begin(), groupList.end(), [&](const GroupList::value_type& v ){
                return groupName ==  v.second.Name;
            });
            if (itGroup == groupList.end()) {
                Group newGroup;
                newGroup.Name = groupName;
                AddGroup(groupList.size(), newGroup);
                itGroup = --groupList.end();
            }
            AddChannelToGroup(itGroup->first, channel.Id);
        }
        // add rest archives
        m_archiveInfoPlist.insert(archiveInfo.begin(), archiveInfo.end());

    }
    
    void Core::LoadEpg()
    {
        using namespace XMLTV;
        auto pThis = this;
        
        EpgEntryCallback onEpgEntry = [&pThis] (const XMLTV::EpgEntry& newEntry) {pThis->AddEpgEntry(newEntry);};
        
        XMLTV::ParseEpg(m_epgUrl, onEpgEntry);
    }
    
    static void LoadPlaylist(const string& plistUrl, PlaylistContent& channels, ArchiveInfos& archiveInfo)
    {
        void* f = NULL;
        
        try {
            char buffer[1024];
            string data;
            
            // Download playlist
            XBMC->Log(LOG_DEBUG, "TtvPlayer: loading playlist: %s", plistUrl.c_str());

            auto f = XBMC->OpenFile(plistUrl.c_str(), 0);
            if (!f)
                throw BadPlaylistFormatException("Failed to obtain playlist from server.");
            bool isEof = false;
            do{
                auto bytesRead = XBMC->ReadFile(f, buffer, sizeof(buffer));
                isEof = bytesRead <= 0;
                if(!isEof)
                    data.append(&buffer[0], bytesRead);
            }while(!isEof);
            XBMC->CloseFile(f);
            f = NULL;
            
            //XBMC->Log(LOG_ERROR, ">>> DUMP M3U : \n %s", data.c_str() );
            
            XBMC->Log(LOG_DEBUG, "TtvPlayer: parsing playlist.");

            // Parse gloabal variables
            //#EXTM3U
            const char* c_M3U = "#EXTM3U";
            
            auto pos = data.find(c_M3U);
            if(string::npos == pos)
                throw BadPlaylistFormatException("Invalid playlist format: missing #EXTM3U tag.");
            pos += strlen(c_M3U);
            
            // Parse channels
            //#EXTINF:0,РБК-ТВ
            //#EXTGRP:новости
            //http://882406a1.iptvspy.me/iptv/xxx/106/index.m3u8
            
            const char* c_INF = "#EXTINF:";
            pos = data.find(c_INF, pos);
            unsigned int plistIndex = 1;
            while(string::npos != pos){
                pos += strlen(c_INF);
                auto pos_end = data.find(c_INF, pos);
                string::size_type tagLen = (std::string::npos == pos_end) ? std::string::npos : pos_end - pos;
                string tag = data.substr(pos, tagLen);
                ParseChannelAndGroup(tag, plistIndex++, channels, archiveInfo);
                pos = pos_end;
            }
            LogDebug("TtvPlayer: added %d channels from playlist." , channels.size());

        } catch (std::exception& ex) {
            LogError("TtvPlayer: exception during playlist loading: %s", ex.what());
            if(NULL != f) {
                XBMC->CloseFile(f);
                f = NULL;
            }
            
        }
    }

    static string FindVar(const string& data, string::size_type pos, const char* varTag)
    {
        pos = data.find(varTag, pos);
        if(string::npos == pos)
            throw BadPlaylistFormatException((string("Invalid playlist format: missing variable ") + varTag).c_str());
        pos += strlen(varTag);
        
        //check whether tag is missing = and "
        if(data[pos] == '=') ++pos;
        if(data[pos] == '"') ++pos;

        auto pos_end = data.find("\"", pos);
        if(string::npos == pos)
            throw BadPlaylistFormatException((string("Invalid playlist format: missing end of variable ") + varTag).c_str());
        return data.substr(pos, pos_end - pos);
        
    }
    
    static void ParseChannelAndGroup(const string& data, unsigned int plistIndex, PlaylistContent& channels, ArchiveInfos& archiveInfo)
    {
//#EXTINF:124 group-title="Фильмы" catchup="default" catchup-source="http://1ttvauth.top/xxx/0/playlist.m3u8?utc=${start}&lutc=${timestamp}" catchup-days="10" tvg-logo="http://1ttv.org/uploads/XGC77wQNeEyaJ2z2mDipyIPsoF0xc1.png",Fox
//#EXTGRP:Фильмы
//    http://1ttvauth.top/xxxx/playlist.m3u8

        auto pos = data.find(',');
        if(string::npos == pos)
            throw BadPlaylistFormatException("Invalid channel block format: missing ','  delinmeter.");
        pos += 1;
        auto endlLine = data.find('\n');
        string name = data.substr(pos, endlLine - pos);
        rtrim(name);
        string tail = data.substr(endlLine + 1);
        
        string groupName;
        try { groupName = FindVar(data, 0, "group-title");} catch (...) {}
        
        string iconPath;
        try { iconPath = FindVar(data, 0, "tvg-logo");} catch (...) {}

        unsigned int archiveDays;
        string archiveUrl;
        bool hasArchive = false;
        try { hasArchive = !FindVar(data, 0, "catchup").empty();} catch (...) {}
        
        if(hasArchive) {
            try {  archiveDays = stoul(FindVar(data, 0, "catchup-days"));} catch (...) { hasArchive = false;}
        }
        if(hasArchive) {
            try {  archiveUrl = FindVar(data, 0, "catchup-source");} catch (...) { hasArchive = false;}
        }

        endlLine = 0;
        const char* c_GROUP = "#EXTGRP:";
        pos = tail.find(c_GROUP);
        // Optional channel group
        if(string::npos != pos) {
            pos += strlen(c_GROUP);
            endlLine = tail.find('\n', pos);
            if(std::string::npos == pos)
                throw BadPlaylistFormatException("Invalid channel block format: missing NEW LINE after #EXTGRP.");
            groupName = tail.substr(pos, endlLine - pos);
            rtrim(groupName);
            ++endlLine;
        }
        string url = tail.substr(endlLine);
        rtrim(url);
        
        Channel channel;
        channel.Id = plistIndex;
        channel.Name = name;
        channel.Number = plistIndex;
        channel.Urls.push_back(url);
        channel.HasArchive = hasArchive;
        channel.IconPath = iconPath;
        channel.IsRadio = false;
        channels[channel.Name] = PlaylistContent::mapped_type(channel,groupName);
        if(hasArchive) {
            archiveInfo.emplace(channel.Id, std::move(ArchiveInfo(archiveDays, archiveUrl)));
        }
    }
    
    
}

