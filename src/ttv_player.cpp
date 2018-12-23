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
#include <vector>
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
    LogError("%s. TTV API error: %s", __FUNCTION__,  ex.what()); \
    char* message = XBMC->GetLocalizedString(32019); \
    XBMC->QueueNotification(QUEUE_ERROR, message, ex.what() ); \
    XBMC->FreeString(message); \
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
        ApiFunctionData(const char* _name)
        : ApiFunctionData(_name, ParamList())
        {}
        
        ApiFunctionData(const char* _name, const ParamList& _params, bool hiPriority = false)
        : name(_name) , params(_params), isHiPriority(hiPriority)
        {}
        std::string name;
        const ParamList params;
        bool isHiPriority;
    };

    Core::Core(const UserInfo& userInfo)
    : m_hasTSProxy(false)
    , m_isVIP(false)
    , m_needsAdult(false)
    , m_useApi(true)
    , m_userInfo(userInfo)
    {
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
        }

        RebuildChannelAndGroupList();
        if(clearEpgCache) {
            ClearEpgCache(c_EpgCacheFile);
        } else {
            LoadEpgCache(c_EpgCacheFile);
        }
        if(!m_useApi)
            LoadEpg();
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
        
        auto chId = entry.ChannelId;
        if(m_useApi) {
            if(m_ttvChannels.count(chId) == 0)
            return;
            if(m_epgIdToChannelId.count(m_ttvChannels.at(chId).epg_id) == 0)
            return;
            entry.HasArchive = when > 0;
        } else {
            entry.HasArchive = false;
            if(m_archiveInfo.count(chId) == 0)
            return;
            
            const time_t archivePeriod = m_archiveInfo.at(entry.ChannelId).days * 24 * 60 * 60; //archive  days in secs
            entry.HasArchive = when > 0 && when < archivePeriod;
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
    
    string Core::GetUrl(ChannelId channelId)
    {
        if(m_useApi)
            return GetUrl_Api(channelId);
        return m_channelList.at(channelId).Urls[0];
    }

#pragma mark - API methods
    string Core::GetUrl_Api(ChannelId channelId)
    {
        string url;
        ParamList params;
        params["channel_id"] = n_to_string(channelId);
        params["nohls"] = "0";
        params["hls_bandwidth_preset"] = "0";
        try {
            ApiFunctionData apiParams("translation_http.php", params, true);
            CallApiFunction(apiParams, [&url] (Document& jsonRoot)
                            {
                                url = jsonRoot["source"].GetString();
                            });
        }
        CATCH_API_CALL();
        return url;
    }
    
    std::string Core::GetArchiveUrl_Api(ChannelId channelId, time_t startTime)
    {
        string url;
        auto epg_id = m_ttvChannels.at(channelId).epg_id;
        
        char mbstr[100];
        if (0 == std::strftime(mbstr, sizeof(mbstr), "X%d-X%m-%Y", std::localtime(&startTime))) {
            return url;
        }
        string archiveDate(mbstr);
        StringUtils::Replace(archiveDate, "X0","X");
        StringUtils::Replace(archiveDate,"X","");
        try {
            
            int record_id = 0;
            ParamList params;
            params["date"] = archiveDate;
            params["epg_id"] = n_to_string(epg_id);
            CallApiFunction(ApiFunctionData("arc_records.php", params), [startTime, &record_id] (Document& jsonRoot)
                            {
                                if(!jsonRoot.HasMember("records"))
                                return;
                                auto& records = jsonRoot["records"];
                                if(!records.IsArray())
                                return;
                                for (auto& r : records.GetArray()) {
                                    if(r["time"].GetInt() == startTime){
                                        record_id = r["record_id"].GetInt();
                                        return;
                                    }
                                }
                            });
            if(0 == record_id)
            return url;
            
            params.clear();
            params["record_id"] = n_to_string(record_id);
            CallApiFunction(ApiFunctionData ("arc_http.php", params), [&url] (Document& jsonRoot)
                            {
                                url = jsonRoot["source"].GetString();
                                LogDebug(">> !!! >> TTV archive URL: %s", url.c_str());
                            });
            
        }
        CATCH_API_CALL();
        return url;
    }

    void Core::UpdateEpgForAllChannels_Api(time_t startTime, time_t endTime)
    {
        auto pThis = this;
        size_t channelsToProcess = m_channelList.size();
        
        for (const auto& ch : m_channelList) {
            
            auto chId = ch.second.Id;
            const auto& ttvCh = m_ttvChannels[chId];
            bool isLast = --channelsToProcess == 0;
            
            if(ttvCh.epg_id == 0)
            continue;
            
            auto epgOffset = - XMLTV::LocalTimeOffset();
            
            ParamList params;
            params["btime"] = n_to_string(startTime);
            params["etime"] = n_to_string(endTime);
            params["epg_id"] = n_to_string(ttvCh.epg_id);
            ApiFunctionData apiParams("translation_epg.php", params);
            
            CallApiAsync(apiParams, [pThis, chId, epgOffset] (Document& jsonRoot)
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
                                 UniqueBroadcastIdType id = epgEntry.StartTime;
                                 pThis->ClientCoreBase::AddEpgEntry(id, epgEntry);
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
            ApiFunctionData apiParams("userinfo.php", params);
            
            auto pThis = this;
            CallApiFunction(apiParams, [&pThis] (Document& jsonRoot)
                            {
                                pThis->m_hasTSProxy = jsonRoot["tsproxy_access"].GetInt() == 1;
                                pThis->m_isVIP = jsonRoot["vip_status"].GetInt() == 1;
                                pThis->m_needsAdult = jsonRoot["adult"].GetInt() == 1;
                            });
        }
        CATCH_API_CALL();
    }
    
    std::string Core::GetPlaylistUrl()
    {
        string url;
        ParamList params;
//        params["cat_group"] = "0";
//        params["cat"] = "0";
//        params["ts"] = "0";
//        params["fav"] = "0";
//        params["cp1251"] = "0";
        params["hls"] = "1";
//        params["cat_fav"] = "0";
//        params["cat_tag"] = "0";
        params["format"] = "m3u";
        params["tag"] = "1";
        params["archive"] = "1";
        try {
            ApiFunctionData apiParams("user_playlist_url.php", params);
            
            CallApiFunction(apiParams, [&url] (Document& jsonRoot)
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

            ParamList params;
            params["type"] = "all";
            ApiFunctionData apiParams("translation_list.php", params);
            
            auto pThis = this;
            CallApiFunction(apiParams, [pThis] (Document& jsonRoot)
            {
                for(auto& gr : jsonRoot["categories"].GetArray())
                {
                    auto id = gr["id"].GetInt();
                    Group group;
                    group.Name = gr["name"].GetString();
                    pThis->AddGroup(id, group);
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

                    pThis->AddChannel(channel);
                    pThis->AddChannelToGroup(ch["group"].GetInt(), channel.Id);
                    
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
                    pThis->m_ttvChannels[channel.Id] = ttvChannel;
                    
//                    LogDebug("Channel %s: Stream - %s, Archive - %s", channel.Name.c_str(),
//                             ttvChannel.hasHTTPStream ? "YES" : "NO",
//                             ttvChannel.hasHTTPArchive ? "YES" : "NO");
                }
            });
            // Fill archive info
            InitializeArchiveInfo();
            
        }
        CATCH_API_CALL();

    }
    void Core::InitializeArchiveInfo()
    {
        try {
            m_epgIdToChannelId.clear();

            ParamList params;
            ApiFunctionData apiParams("arc_list.php", params);
            
            auto pThis = this;
            CallApiFunction(apiParams, [pThis] (Document& jsonRoot)
            {
                for(auto& ch : jsonRoot["channels"].GetArray())
                {
                    bool hasArchiveAssess = ch["access_user"].IsBool() && ch["access_user"].IsTrue();
                    if(!hasArchiveAssess) {
                        LogDebug("You do not have access to archive for channel %s", ch["name"].GetString());
                        continue;
                    }
                    pThis->m_epgIdToChannelId[ch["epg_id"].GetInt()] = ch["id"].GetInt();

                }
            });
        }
        CATCH_API_CALL();

    }
    
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
        string query;
        ParamList localParams(data.params);
        localParams["typeresult"] = "json";
        if(!isLoginCommand) {
            localParams["session"] = m_sessionId;
        }
        ParamList::const_iterator runner = localParams.begin();
        ParamList::const_iterator first = runner;
        ParamList::const_iterator end = localParams.end();
        
        for (; runner != end; ++runner)
        {
            query += runner == first ? "?" : "&";
            query += runner->first + '=' + runner->second;
        }
        std::string strRequest = c_TTV_API_URL_base;
        strRequest += data.name + query;
        
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
        m_httpEngine->CallApiAsync(strRequest, parserWrapper, comp, data.isHiPriority);
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
            m_deviceId = GUID::generate();
        }
        
        // Authorize to TTV
        ParamList params;
        params["username"] = m_userInfo.user;
        params["password"] = m_userInfo.password;
        params["application"] = "xbmc";
        params["guid"] = m_deviceId;
        try {
            ApiFunctionData apiParams("auth.php", params);
            
            CallApiFunction(apiParams, [&] (Document& jsonRoot)
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
        if(m_archiveInfo.count(channelId) == 0) {
            return url;
        }
        url = m_archiveInfo.at(channelId).urlTemplate;
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
                    pThis->m_archiveInfo.emplace(newChannel.id, archiveInfo.at(plistChannel.Id));
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
        m_archiveInfo.insert(archiveInfo.begin(), archiveInfo.end());

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

