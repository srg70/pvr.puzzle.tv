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

namespace SharaTvEngine
{
    using namespace Globals;
    using namespace std;
    using namespace ADDON;
    using namespace rapidjson;
    using namespace PvrClient;

    
    static const char* c_EpgCacheFile = "sharatv_epg_cache.txt";

//    struct NoCaseComparator : binary_function<string, string, bool>
//    {
//        inline bool operator()(const string& x, const string& y) const
//        {
//            return StringUtils::CompareNoCase(x, y) < 0;
//        }
//    };

    typedef map<string, pair<Channel, string>/*, NoCaseComparator*/> PlaylistContent;
    
    static void ParseChannelAndGroup(const std::string& data, unsigned int plistIndex, PlaylistContent& channels, ArchiveInfos& archiveInfo);
    static void ParsePlaylist(const string& data, PlaylistContent& channels, ArchiveInfos& archiveInfo);
    static string GetEpgUrlFromPlaylist(const string& data);
    static string FindVar(const string& data, string::size_type pos, const char* varTag);
//    static void LoadPlaylist(const string& plistUrl, string& data);
    
    Core::Core(const std::string &playlistUrl, bool enableAdult)
    : m_enableAdult(enableAdult)
    , m_playListUrl(playlistUrl)
    {
        string data;
        // NOTE: shara TV does NOT check credentials
        // Just builds a playlist.
        // I.e. GetCachedFileContents alwais succeeded.
        if(0 == XMLTV::GetCachedFileContents(m_playListUrl, data)){
            LogError("SharaTvPlayer: failed to download playlist. URL: %s", playlistUrl.c_str());
            throw ServerErrorException("SharaTvPlayer: failed to download playlist.", -1);
        }
        m_epgUrl = GetEpgUrlFromPlaylist(data);
    }
    
    void Core::Init(bool clearEpgCache)
    {
        RebuildChannelAndGroupList();
        if(clearEpgCache) {
            ClearEpgCache(c_EpgCacheFile);
        } else {
            LoadEpgCache(c_EpgCacheFile);
        }
        LoadEpg();
    }
    
    
    Core::~Core()
    {
        Cleanup();
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

        ArchiveInfos archiveInfo;
        PlaylistContent plistContent;
        ParsePlaylist(data, plistContent, archiveInfo);
        
//        auto pThis = this;
//
//        ChannelCallback onNewChannel = [&pThis, &plistContent, &archiveInfo](const EpgChannel& newChannel){
//            if(plistContent.count(newChannel.strName) != 0) {
//                auto& plistChannel = plistContent[newChannel.strName].first;
//                if(plistChannel.HasArchive) {
//                    pThis->m_archiveInfo.emplace(newChannel.id, archiveInfo.at(plistChannel.Id));
//                    archiveInfo.erase(plistChannel.Id);
//                }
//                plistChannel.Id = newChannel.id;
//                if(!newChannel.strIcon.empty())
//                    plistChannel.IconPath = newChannel.strIcon;
//            }
//        };
//
//        XMLTV::ParseChannels(m_epgUrl, onNewChannel);

        for(const auto& channelWithGroup : plistContent)
        {
            const auto& channel = channelWithGroup.second.first;
            const auto& groupName = channelWithGroup.second.second;
            
            if(!m_enableAdult && groupName == "Взрослые")
                continue;

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
            AddChannelToGroup(itGroup->first, channel.UniqueId);
        }
        // add rest archives
        m_archiveInfo.insert(archiveInfo.begin(), archiveInfo.end());
    }
    
    std::string Core::GetArchiveUrl(ChannelId channelId, time_t startTime)
    {
        if(m_archiveInfo.count(channelId) == 0) {
            return string();
        }
        string url = m_archiveInfo.at(channelId).urlTemplate;
        if(url.empty())
            return url;
        const char* c_START = "${start}";
        size_t pos = url.find(c_START);
        if(string::npos == pos)
            return string();
        url.replace(pos, strlen(c_START), n_to_string(startTime));
//        const char* c_STAMP = "${timestamp}";
//        pos = url.find(c_STAMP);
//        if(string::npos == pos)
//            return string();
//        url.replace(pos, strlen(c_STAMP), n_to_string(time(nullptr)));
        return  url;
    }
        
    bool Core::AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
    {
        unsigned int id = xmlEpgEntry.startTime;
        
        EpgEntry epgEntry;
        epgEntry.ChannelId = xmlEpgEntry.iChannelId;
        epgEntry.Title = xmlEpgEntry.strTitle;
        epgEntry.Description = xmlEpgEntry.strPlot;
        epgEntry.StartTime = xmlEpgEntry.startTime;
        epgEntry.EndTime = xmlEpgEntry.endTime;
        epgEntry.IconPath = xmlEpgEntry.iconPath;
        return ClientCoreBase::AddEpgEntry(id, epgEntry);
    }
    
    void Core::UpdateHasArchive(PvrClient::EpgEntry& entry)
    {
        entry.HasArchive = false;
        if(m_archiveInfo.count(entry.ChannelId) == 0)
            return;
        
        time_t now = time(nullptr);
        const time_t archivePeriod = m_archiveInfo.at(entry.ChannelId).days * 24 * 60 * 60; //archive  days in secs
        time_t epgTime = m_addCurrentEpgToArchive ? entry.StartTime : entry.EndTime;
        auto when = now - epgTime;
        entry.HasArchive = when > 0 && when < archivePeriod;
    }
   
    static PvrClient::KodiChannelId EpgChannelIdToKodi(const std::string& strId)
    {
        return stoi(strId.c_str());
    }
    void Core::UpdateEpgForAllChannels(time_t startTime, time_t endTime)
    {
        if(m_epgUpdateInterval.IsSet() && m_epgUpdateInterval.TimeLeft() > 0)
            return;
        
        m_epgUpdateInterval.Init(24*60*60*1000);
        
        using namespace XMLTV;
        try {
            auto pThis = this;
 
            set<ChannelId> channelsToUpdate;
            EpgEntryCallback onEpgEntry = [&pThis, &channelsToUpdate,  startTime] (const XMLTV::EpgEntry& newEntry) {
                if(pThis->AddEpgEntry(newEntry) && newEntry.startTime >= startTime)
                    channelsToUpdate.insert(newEntry.iChannelId);
            };
            
            XMLTV::ParseEpg(m_epgUrl, onEpgEntry, EpgChannelIdToKodi);
            
//            for (auto channel : channelsToUpdate) {
//                PVR->TriggerEpgUpdate(channel);
//            }

            SaveEpgCache(c_EpgCacheFile, 11);
//        } catch (ServerErrorException& ex) {
//            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32002), ex.reason.c_str() );
        } catch (...) {
            LogError(" >>>>  FAILED receive EPG <<<<<");
        }
    }
    
    void Core::LoadEpg()
    {
        using namespace XMLTV;
        auto pThis = this;

        EpgEntryCallback onEpgEntry = [&pThis] (const XMLTV::EpgEntry& newEntry) {pThis->AddEpgEntry(newEntry);};
        
        XMLTV::ParseEpg(m_epgUrl, onEpgEntry);
    }
    
    string Core::GetUrl(ChannelId channelId)
    {
        string url = m_channelList.at(channelId).Urls[0];
        return url;
    }
    
//    static void LoadPlaylist(const string& plistUrl, string& data)
//    {
//        void* f = NULL;
//
//        try {
//            char buffer[1024];
//
//            // Download playlist
//            XBMC->Log(LOG_DEBUG, "SharaTvPlayer: loading playlist: %s", plistUrl.c_str());
//
//            auto f = XBMC->OpenFile(plistUrl.c_str(), 0);
//            if (!f)
//                throw BadPlaylistFormatException("Failed to obtain playlist from server.");
//                bool isEof = false;
//                do{
//                    auto bytesRead = XBMC->ReadFile(f, buffer, sizeof(buffer));
//                    isEof = bytesRead <= 0;
//                    if(!isEof)
//                        data.append(&buffer[0], bytesRead);
//                        }while(!isEof);
//            XBMC->CloseFile(f);
//            f = NULL;
//
//         } catch (std::exception& ex) {
//            XBMC->Log(LOG_ERROR, "SharaTvPlayer: exception during playlist loading: %s", ex.what());
//            if(NULL != f) {
//                XBMC->CloseFile(f);
//                f = NULL;
//            }
//
//        }
//    }
    
    static string GetEpgUrlFromPlaylist(const string& data)
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
        try { epgUrl = FindVar(data.substr(pos, range), 0, "url-tvg");} catch (...) {}
        
        return epgUrl;

    }
    static void ParsePlaylist(const string& data, PlaylistContent& channels, ArchiveInfos& archiveInfo)
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
                ParseChannelAndGroup(tag, plistIndex++, channels, archiveInfo);
                pos = pos_end;
            }
            XBMC->Log(LOG_DEBUG, "SharaTvPlayer: added %d channels from playlist." , channels.size());

        } catch (std::exception& ex) {
            XBMC->Log(LOG_ERROR, "SharaTvPlayer: exception during playlist loading: %s", ex.what());           
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
        channel.UniqueId = channel.EpgId = EpgChannelIdToKodi(tvgId);
        channel.Name = name;
        channel.Number = plistIndex;
        channel.Urls.push_back(url);
        channel.HasArchive = hasArchive;
        channel.IconPath = iconPath;
        channel.IsRadio = false;
        channels[tvgId] = PlaylistContent::mapped_type(channel,groupName);
        if(hasArchive) {
            archiveInfo.emplace(channel.UniqueId, std::move(ArchiveInfo(archiveDays, archiveUrl)));
        }
    }
    
    
}

