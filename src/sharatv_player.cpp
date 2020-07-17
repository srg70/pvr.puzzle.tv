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

#include <assert.h>
#include <algorithm>
#include <sstream>
#include <vector>
#include <ctime>
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/util.h"
#include "p8-platform/util/StringUtils.h"
#include "helpers.h"
#include "sharatv_player.h"
#include "HttpEngine.hpp"
#include "XMLTV_loader.hpp"
#include "globals.hpp"

namespace SharaTvEngine {

    using namespace Globals;
    using namespace std;
    using namespace ADDON;
    using namespace rapidjson;
    using namespace PvrClient;
    using namespace Helpers;
    
    // Tags for archive template
    static const char* const c_START = "${start}";
    static const char* const c_DURATION = "${duration}";
    static const char* const c_OFFSET = "${offset}";
    static const char* const c_LUTC = "${lutc}";

    
    static const char* c_EpgCacheFile = "sharatv_epg_cache.txt";
    
    //    struct NoCaseComparator : binary_function<string, string, bool>
    //    {
    //        inline bool operator()(const string& x, const string& y) const
    //        {
    //            return StringUtils::CompareNoCase(x, y) < 0;
    //        }
    //    };
    
    typedef struct {
        // Cahnnels with group IDs, sorted by name.
        typedef map<string, pair<Channel, GroupId>/*, NoCaseComparator*/> TChannels;
        TChannels channels;
        // Group name to group Id LUT
        map<string, GroupId> groups;
    }PlaylistContent;
    
    static void ParseChannelAndGroup(const std::string& data, const GloabalTags& globalTags, unsigned int plistIndex, PlaylistContent& content, ArchiveInfos& archiveInfo);
    static void ParsePlaylist(const string& data, const GloabalTags& globalTags, PlaylistContent& content, ArchiveInfos& archiveInfo);
    static void GetGlobalTagsFromPlaylist(const string& data, GloabalTags& globalTags);
    static string FindVar(const string& data, string::size_type pos, const char* varTag);
    //    static void LoadPlaylist(const string& plistUrl, string& data);
    
    Core::Core(const std::string &playlistUrl,  const std::string &epgUrl, bool enableAdult)
    : m_enableAdult(enableAdult)
    , m_supportMulticastUrls(false)
    , m_playListUrl(playlistUrl)
    , m_epgUrl(epgUrl)
    {
        string data;
        // NOTE: shara TV does NOT check credentials
        // Just builds a playlist.
        // I.e. GetCachedFileContents alwais succeeded.
        if(0 == XMLTV::GetCachedFileContents(m_playListUrl, data)){
            LogError("SharaTvPlayer: failed to download playlist. URL: %s", playlistUrl.c_str());
            throw ServerErrorException("SharaTvPlayer: failed to download playlist.", -1);
        }
        
        GetGlobalTagsFromPlaylist(data, m_globalTags);
        
        if(m_epgUrl.empty() && !m_globalTags.m_epgUrl.empty()){
            m_epgUrl = m_globalTags.m_epgUrl;
            LogInfo("SharaTvPlayer: no external EPG link provided. Will use playlist's url-tvg tag %s", m_epgUrl.c_str());
        }
    }
    
    void Core::Init(bool clearEpgCache)
    {
        if(clearEpgCache)
            ClearEpgCache(c_EpgCacheFile, m_epgUrl.c_str());
        
        // Channels use EPG for name synch!
        RebuildChannelAndGroupList();
        
        if(!clearEpgCache)
            LoadEpgCache(c_EpgCacheFile);
    }
    
    
    Core::~Core()
    {
        Cleanup();
        PrepareForDestruction();
    }
    void Core::Cleanup()
    {
        //        LogNotice("SharaTvPlayer stopping...");
        //
        //
        //        LogNotice("SharaTvPlayer stopped.");
    }
    
    void Core::BuildChannelAndGroupList()
    {
        using namespace XMLTV;
        
        string data;
        XMLTV::GetCachedFileContents(m_playListUrl, data);
        
        m_archiveInfo.Reset();
        PlaylistContent plistContent;
        ParsePlaylist(data, m_globalTags, plistContent, m_archiveInfo);
        
        PlaylistContent::TChannels& channels = plistContent.channels;
        // Verify tvg-id from EPG file
        ChannelCallback onNewChannel = [&channels](const EpgChannel& newChannel){
            for(const auto& epgChannelName : newChannel.strNames) {
                 if(channels.count(epgChannelName) != 0) {
                     auto& plistChannel = channels[epgChannelName].first;
                     plistChannel.EpgId = newChannel.id;
                     if(plistChannel.IconPath.empty())
                         plistChannel.IconPath = newChannel.strIcon;
                 }
             }
        };
        
        XMLTV::ParseChannels(m_epgUrl, onNewChannel);
        
        // Add groups
        GroupId adultChannelsGroupId = -1;
        for(const auto& group :  plistContent.groups) {
            Group newGroup;
            newGroup.Name = group.first;
            AddGroup(group.second, newGroup);
            if(-1 == adultChannelsGroupId &&  newGroup.Name == "Взрослые")
                adultChannelsGroupId = group.second;
        }
        // Add channels
        for(auto& channelWithGroup : plistContent.channels)
        {
            auto& channel = channelWithGroup.second.first;
            const auto& groupId = channelWithGroup.second.second;
            
            if(!m_enableAdult && adultChannelsGroupId == groupId)
                continue;
            
            if (m_supportMulticastUrls) {
                decltype(channel.Urls) urls;
                for (auto& url : channel.Urls) {
                    urls.push_back(TransformMultucastUrl(url));
                }
                channel.Urls = urls;
            }
            
            AddChannel(channel);
            AddChannelToGroup(groupId, channel.UniqueId);
        }
     }
    
    std::string Core::TransformMultucastUrl(const std::string& url) const {
        static const std::string UDP_MULTICAST_PREFIX = "udp://@";
        static const std::string RTP_MULTICAST_PREFIX = "rtp://@";
                
        auto pos = url.find(UDP_MULTICAST_PREFIX);
        if(StringUtils::StartsWith(url, UDP_MULTICAST_PREFIX))
        {
            return m_multicastProxyAddress + "/udp/" + url.substr(UDP_MULTICAST_PREFIX.length());

        }
        if(StringUtils::StartsWith(url, RTP_MULTICAST_PREFIX))
        {
            
            return m_multicastProxyAddress + "/rtp/" + url.substr(RTP_MULTICAST_PREFIX.length());
        }
        return url;
    }

    std::string Core::GetArchiveUrl(ChannelId channelId, time_t startTime, time_t duration)
    {
        if(m_archiveInfo.info.count(channelId) == 0) {
            return string();
        }
        string url = m_archiveInfo.info.at(channelId).urlTemplate;
        if(url.empty())
            return url;
        
        std::string debugTimes = "SharaTvPlayer: requested archive";
        
        // Optinal start tag
        size_t pos = url.find(c_START);
        if(string::npos != pos){
            url.replace(pos, strlen(c_START), n_to_string(startTime));
            debugTimes += " from " + time_t_to_string(startTime);
        }
        
        // Optional duration tag
        pos = url.find(c_DURATION);
        if(string::npos != pos){
            time_t endTime = startTime + duration;
            // If recording ends less then 5 seconds before NOW
            // assume current broadcast, i.e. use "now" keyword
            std::string strDuration;
            if(difftime(time(NULL), endTime) < 5) {
                strDuration =  "now";
                debugTimes += " to now";
            } else {
                strDuration =  n_to_string(duration);
                debugTimes += " to " + time_t_to_string(duration);
            }
            url.replace(pos, strlen(c_DURATION), strDuration);
            
        }
        
        // Optional offset tag
        pos = url.find(c_OFFSET);
        if(string::npos != pos){
            auto offset = time(NULL) - startTime;
            url.replace(pos, strlen(c_OFFSET), n_to_string(offset));
            debugTimes += " with offset " + time_t_to_string(offset);
        }
        
        // Optional UTC and LUTC params
        pos = url.find(c_LUTC);
        if(string::npos != pos) {
            auto lutc = time(NULL) - startTime;
            url.replace(pos, strlen(c_LUTC), n_to_string(lutc));
            debugTimes += " local UTC " + time_t_to_string(lutc);
        }
                            
        LogDebug(debugTimes.c_str());
        
        return  url;
    }
    
    //    bool Core::AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
    //    {
    //        unsigned int id = xmlEpgEntry.startTime;
    //
    //        EpgEntry epgEntry;
    //        epgEntry.ChannelId = xmlEpgEntry.iChannelId;
    //        epgEntry.Title = xmlEpgEntry.strTitle;
    //        epgEntry.Description = xmlEpgEntry.strPlot;
    //        epgEntry.StartTime = xmlEpgEntry.startTime;
    //        epgEntry.EndTime = xmlEpgEntry.endTime;
    //        epgEntry.IconPath = xmlEpgEntry.iconPath;
    //        return ClientCoreBase::AddEpgEntry(id, epgEntry);
    //    }
    //
    void Core::UpdateHasArchive(PvrClient::EpgEntry& entry)
    {
        entry.HasArchive = false;
        const ChannelId& chId = entry.UniqueChannelId; // pCahnnel->second.UniqueId;
        if(m_archiveInfo.info.count(chId) == 0)
            return;
        
        time_t now = time(nullptr);
        const time_t archivePeriod = m_archiveInfo.info.at(chId).days * 24 * 60 * 60; //archive  days in secs
        time_t epgTime = entry.EndTime;
        switch(m_addCurrentEpgToArchive) {
            case PvrClient::k_AddCurrentEpgToArchive_Yes:
                epgTime = entry.StartTime;
                break;
            case PvrClient::k_AddCurrentEpgToArchive_AfterInit:
            {
                auto phase = GetPhase(k_RecordingsInitialLoadingPhase);
                epgTime = phase->IsDone() ? entry.StartTime : entry.EndTime;
                break;
            }
            default:
                break;
        }
        auto when = now - epgTime;
        entry.HasArchive = when > 0 && when < archivePeriod;
    }
    
    static PvrClient::KodiChannelId EpgChannelIdToKodi(const std::string& strId)
    {
        string strToHash(strId);
        StringUtils::ToLower(strToHash);
        return std::hash<std::string>{}(strToHash);
    }
    
    void Core::UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled)
    {
        using namespace XMLTV;
        try {
            LoadEpg(cancelled);
            if(!cancelled())
                SaveEpgCache(c_EpgCacheFile, m_archiveInfo.archiveDays);
        } catch (...) {	
            LogError("SharaTvPlayer:: failed to update EPG.");
        }
    }
    
    void Core::LoadEpg(std::function<bool(void)> cancelled)
    {
        using namespace XMLTV;
        auto pThis = this;
        
        m_epgUpdateInterval.Init(24*60*60*1000);

        EpgEntryCallback onEpgEntry = [&pThis, cancelled] (const XMLTV::EpgEntry& newEntry) {
            pThis->AddEpgEntry(newEntry);
            return !cancelled();
        };
        
        XMLTV::ParseEpg(m_epgUrl, onEpgEntry, EpgChannelIdToKodi);
    }
    
    string Core::GetUrl(ChannelId channelId)
    {
        string url = m_channelList.at(channelId).Urls[0];
        return url;
    }
    
    static void GetGlobalTagsFromPlaylist(const string& data, GloabalTags& globalTags)
    {
        string epgUrl;
        
        const char* c_M3U = "#EXTM3U";
        
        auto pos = data.find(c_M3U);
        if(string::npos == pos)
            throw BadPlaylistFormatException("Invalid playlist format: missing #EXTM3U tag.");
        
        pos += strlen(c_M3U);
        
        const char* c_INF = "#EXTINF:";
        auto range = data.find(c_INF, pos);
        if(string::npos != range) {
            range -= pos;
        }
        string header = data.substr(pos, range);
        try { globalTags.m_epgUrl = FindVar(header, 0, "url-tvg");} catch (...) {}
        try { globalTags.m_catchupDays = stoul(FindVar(header, 0, "catchup-days"));} catch (...) {}
        try {
            globalTags.m_catchupType = FindVar(header, 0, "catchup");
            LogDebug("SharaTvPlayer: gloabal catchup type: %s", globalTags.m_catchupType.c_str());
        } catch (...) {}
        try {
            globalTags.m_catchupSource = FindVar(header, 0, "catchup-source");
            LogDebug("SharaTvPlayer: gloabal catchup-source: %s", globalTags.m_catchupSource.c_str());
        } catch (...) {}
        
    }
    
    
    static void ParsePlaylist(const string& data, const GloabalTags& globalTags,  PlaylistContent& content, ArchiveInfos& archiveInfo)
    {
        
        try {
            XBMC->Log(LOG_DEBUG, "SharaTvPlayer: parsing playlist.");
            // Parse gloabal variables
            //#EXTM3U
            const char* c_M3U = "#EXTM3U";
            
            auto pos = data.find(c_M3U);
            if(string::npos == pos)
                throw BadPlaylistFormatException("Invalid playlist format: missing #EXTM3U tag.");
            pos += strlen(c_M3U);
            
            // Parse channels
            //#EXTINF:0 audio-track="ru" tvg-id="1147" tvg-name="Первый канал" tvg-logo="https://shara-tv.org/img/images/Chanels/perviy_k.png" group-title="Базовые" catchup="default" catchup-days="3" catchup-source="http://oa5iy59taouocss24ctr.mine.nu:8099/Perviykanal/index-${start}-3600.m3u8?token=oa123456+12345678", Первый канал
            //#EXTGRP:Базовые
            //http://oa5iy59taouocss24ctr.mine.nu:8000/Perviykanal?auth=oa123456+12345678
            const char* c_INF = "#EXTINF:";
            pos = data.find(c_INF, pos);
            unsigned int plistIndex = 1;
            while(string::npos != pos){
                pos += strlen(c_INF);
                auto pos_end = data.find(c_INF, pos);
                string::size_type tagLen = (std::string::npos == pos_end) ? std::string::npos : pos_end - pos;
                string tag = data.substr(pos, tagLen);
                ParseChannelAndGroup(tag, globalTags, plistIndex++, content, archiveInfo);
                pos = pos_end;
            }
            XBMC->Log(LOG_DEBUG, "SharaTvPlayer: added %d channels from playlist." , content.channels.size());
            
        } catch (std::exception& ex) {
            XBMC->Log(LOG_ERROR, "SharaTvPlayer: exception during playlist loading: %s", ex.what());
        }
    }
    
    static string FindVar(const string& data, string::size_type pos, const char* varTag)
    {
        string strTag(varTag);
        strTag += '=';
        pos = data.find(strTag, pos);
        if(string::npos == pos)
            throw BadPlaylistFormatException((string("Invalid playlist format: missing variable ") + varTag).c_str());
        pos += strTag.size();
        
        //check whether tag is missing = and "
        if(data[pos] == '=') ++pos;
        if(data[pos] == '"') ++pos;
        
        auto pos_end = data.find("\"", pos);
        if(string::npos == pos)
            throw BadPlaylistFormatException((string("Invalid playlist format: missing end of variable ") + varTag).c_str());
        return data.substr(pos, pos_end - pos);
        
    }
    
    static void ParseChannelAndGroup(const string& data, const GloabalTags& globalTags, unsigned int plistIndex, PlaylistContent& content, ArchiveInfos& archiveInfo)
    {
        //#EXTINF:0 audio-track="ru" tvg-id="1147" tvg-name="Первый канал" tvg-logo="https://shara-tv.org/img/images/Chanels/perviy_k.png" group-title="Базовые" catchup="default" catchup-days="3" catchup-source="http://oa5iy59taouocss24ctr.mine.nu:8099/Perviykanal/index-${start}-3600.m3u8?token=oa123456+12345678", Первый канал
        //#EXTGRP:Базовые
        //http://oa5iy59taouocss24ctr.mine.nu:8000/Perviykanal?auth=oa123456+12345678
        
        auto pos = data.find(',');
        if(string::npos == pos)
            throw BadPlaylistFormatException("Invalid channel block format: missing ','  delinmeter.");
        pos += 1;
        auto endlLine = data.find('\n');
        string name = data.substr(pos, endlLine - pos);
        rtrim(name);
        string tail = data.substr(endlLine + 1);
        
        string tvgId;
        try { tvgId = FindVar(data, 0, "tvg-id");} catch (...) {}
        
        string groupName;
        try { groupName = FindVar(data, 0, "group-title");} catch (...) {}
        
        string iconPath;
        try { iconPath = FindVar(data, 0, "tvg-logo");} catch (...) {}
        
        int tvgShift = 0;
        try { tvgShift = static_cast<int>(std::atof(FindVar(data, 0, "tvg-shift").c_str()) * 3600);} catch (...) {}
        
        int preloadingInterval = 0;
        try { preloadingInterval = static_cast<int>(std::atof(FindVar(data, 0, "preload-interval").c_str()));} catch (...) {}

        
        unsigned int archiveDays;
        string archiveUrl;
        string archiveType;
        try {
            archiveType = FindVar(data, 0, "catchup");
            StringUtils::ToLower(archiveType);
        } catch (...) {}
        
        // archive type tag name options: "catchup", "catchup-type"
        if(archiveType.empty()) {
            try {
                archiveType = FindVar(data, 0, "catchup-type");
                StringUtils::ToLower(archiveType);
            } catch (...) {}
        }
        
        // Channel URL. Should be before archive!
        // Flussonic archives depend from channel URL.
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
        
        // Has archive support
        if(!archiveType.empty()) {
            try {  archiveDays = stoul(FindVar(data, 0, "catchup-days"));} catch (...) { archiveType.clear();}
            if(archiveType == "default") {
                try {  archiveUrl = FindVar(data, 0, "catchup-source");} catch (...) { archiveType.clear();}
            } else if(archiveType == "flussonic" || archiveType == "fs" || archiveType == "flussonic-ts") {
                auto lastSlash = url.find_last_of('/');
                auto firstAmp = url.find_first_of('?');
                if(string::npos == lastSlash)
                    throw BadPlaylistFormatException((string("Invalid channel URL: ") + url).c_str());
//                if(archiveType == "flussonic-ts") {
//                    archiveUrl = url.substr(0, lastSlash + 1) + "timeshift_abs-" + c_START + ".ts" + url.substr(firstAmp);
//                } else
                {
                    //archiveUrl = url.substr(0, lastSlash + 1) + "timeshift_abs_video-" + c_START + ".m3u8" + url.substr(firstAmp);
                    archiveUrl = url.substr(0, lastSlash + 1) + "video-" + c_START + "-" + c_DURATION + ".m3u8" + url.substr(firstAmp);
                }
            } else if(archiveType == "shift"){
                archiveUrl = url + "?utc=" + c_START +"&lutc=" + c_LUTC;
            }
        }
        // probably we have global archive tag
        else if(globalTags.m_catchupType == "append"){
            archiveUrl = url;
            if(archiveUrl.find("?") == string::npos) {
                // Just add catchup tag a parameters block
                archiveUrl += globalTags.m_catchupSource;
            } else {
                // Add catchup tag to parameters block
                StringUtils::Replace(archiveUrl, "?", globalTags.m_catchupSource + "&");
            }
            if(globalTags.m_catchupDays > 0) {
                archiveDays = globalTags.m_catchupDays;
            } else {
                // catchup-days is mandatory tag.
                try {  archiveDays = stoul(FindVar(data, 0, "catchup-days"));} catch (...) { archiveUrl.clear();}
            }
        }
        if(!archiveUrl.empty())
            LogDebug("SharaTvPlayer: %s channe's archive url %s", name.c_str(), archiveUrl.c_str());
                
        Channel channel;
        channel.UniqueId = XMLTV::ChannelIdForChannelName(name);
        channel.EpgId = EpgChannelIdToKodi(tvgId);
        // LogDebug("Channe: TVG id %s => EPG id %d", tvgId.c_str(), channel.EpgId);
        channel.Name = name;
        channel.Number = plistIndex;
        channel.Urls.push_back(url);
        channel.HasArchive = !archiveUrl.empty();
        channel.IconPath = iconPath;
        channel.IsRadio = false;
        channel.TvgShift = tvgShift;
        channel.PreloadingInterval = preloadingInterval;
        
        // Get group ID for channel by group name.
        // Add new group if missing.
        if(content.groups.count(groupName) == 0) {
            GroupId newGroupId = content.groups.size() + 1;
            content.groups[groupName] = newGroupId;
        }

        content.channels[name] = PlaylistContent::TChannels::mapped_type(channel, content.groups[groupName]);
        if(channel.HasArchive) {
            archiveInfo.info.emplace(channel.UniqueId, std::move(ArchiveInfo(archiveDays, archiveUrl)));
            if(archiveInfo.archiveDays < archiveDays)
                archiveInfo.archiveDays = archiveDays;
        }
    }
    
    
    }

