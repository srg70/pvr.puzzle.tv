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
#include "kodi/General.h"

#include <assert.h>
#include <algorithm>
#include <sstream>
#include <vector>
#include <ctime>
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/util.h"
#include "p8-platform/util/StringUtils.h"
#include "helpers.h"
#include "edem_player.h"
#include "HttpEngine.hpp"
#include "XMLTV_loader.hpp"
#include "globals.hpp"

namespace EdemEngine
{
    using namespace Globals;
    using namespace std;
    using namespace rapidjson;
    using namespace PvrClient;
    using namespace Helpers;

    
    static const int secondsPerHour = 60 * 60;
   
    //
    static const char* c_EpgCacheFile = "edem_epg_cache.txt";
    
    
    struct NoCaseComparator : binary_function<string, string, bool>
    {
        inline bool operator()(const string& x, const string& y) const
        {
            return StringUtils::CompareNoCase(x, y) < 0;
        }
    };

    typedef map<string, pair<Channel, string>, NoCaseComparator> PlaylistContent;
    
    typedef std::function<void(Channel&, const string&)> TChannelParserDelegate;

    static void ParseChannelAndGroup(const std::string& data, unsigned int plistIndex, TChannelParserDelegate onChannelFound);
    static void LoadPlaylist(const string& plistUrl,  TChannelParserDelegate onChannelFound);

    
    Core::Core(const std::string &playListUrl,  const std::string &epgUrl, bool enableAdult)
    : m_playListUrl(playListUrl)
    , m_epgUrl(epgUrl)
    , m_enableAdult(enableAdult)
    {
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
//        LogNotice("EdemPlayer stopping...");
//        
//        
//        LogNotice("EdemPlayer stopped.");
    }
    
    void Core::BuildChannelAndGroupList()
    {
        using namespace XMLTV;
        PlaylistContent plistContent;
        LoadPlaylist(m_playListUrl, [&](Channel& channel, const string& groupName){
            // Override channel logo with local icon when set
            SetLocalPathForLogo(channel);
            plistContent[channel.Name] = PlaylistContent::mapped_type(channel,groupName);
        });
        
        ChannelCallback onNewChannel = [&plistContent](const EpgChannel& newChannel){
            for(const auto& epgChannelName : newChannel.displayNames) {
                 if(plistContent.count(epgChannelName) != 0) {
                     auto& plistChannel = plistContent[epgChannelName].first;
                     plistChannel.EpgId = newChannel.id;
                     if(plistChannel.IconPath.empty())
                         plistChannel.IconPath = newChannel.strIcon;
                 }
             }
        };
        
        XMLTV::ParseChannels(m_epgUrl, onNewChannel);

        for(const auto& channelWithGroup : plistContent)
        {
            const auto& channel = channelWithGroup.second.first;
            const auto& groupName = channelWithGroup.second.second;
            
            if(!m_enableAdult && groupName == "взрослые")
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
   
    std::string Core::GetArchiveUrl(ChannelId channelId, time_t startTime)
    {
        string url = GetUrl(channelId);
        if(url.empty())
            return url;
        url += "?utc=" + n_to_string(startTime)+"&lutc=" + n_to_string(time(nullptr));
        return  url;
    }
        
//    UniqueBroadcastIdType Core::AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
//    {
//        UniqueBroadcastIdType id = xmlEpgEntry.startTime;
//        
//        EpgEntry epgEntry;
//        epgEntry.ChannelId = xmlEpgEntry.iChannelId;
//        epgEntry.Title = xmlEpgEntry.strTitle;
//        epgEntry.Description = xmlEpgEntry.strPlot;
//        epgEntry.StartTime = xmlEpgEntry.startTime;
//        epgEntry.EndTime = xmlEpgEntry.endTime;
//        epgEntry.IconPath = xmlEpgEntry.iconPath;
//       return ClientCoreBase::AddEpgEntry(id, epgEntry);
//    }
    
    void Core::UpdateHasArchive(PvrClient::EpgEntry& entry)
    {
        time_t now = time(nullptr);
        const time_t archivePeriod = 3 * 24 * 60 * 60; //3 days in secs
        
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
   
    void Core::UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled)
    {
        // Assuming server provides EPG at least fo next 12 hours
        // To reduce amount of API calls, allow next EPG update
        // after either 12 hours or  endTime
        //        time_t now = time(nullptr);
        //        time_t nextUpdateAt = now + 12*60*60;
        ////        if(difftime(endTime, now) > 0){
        //            nextUpdateAt = std::min(nextUpdateAt, endTime);
        //        }
        //        int32_t interval = nextUpdateAt - now;
        //        if(interval > 0)

        using namespace XMLTV;
        try {
            LoadEpg(cancelled);
            if(!cancelled())
                SaveEpgCache(c_EpgCacheFile);
//        } catch (ServerErrorException& ex) {
//            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32002), ex.reason.c_str() );
        } catch (...) {
            LogError("EdemPlayer: failed to update EPG");
        }
    }
    
    void Core::LoadEpg(std::function<bool(void)> cancelled)
    {
        using namespace XMLTV;

        m_epgUpdateInterval.Init(12*60*60*1000);

        EpgEntryCallback onEpgEntry = [this, cancelled] (const XMLTV::EpgEntry& newEntry) {
            AddEpgEntry(newEntry);
            return !cancelled();
        };
        
        XMLTV::ParseEpg(m_epgUrl, onEpgEntry);
    }
    
    string Core::GetUrl(ChannelId channelId)
    {
        string url = m_channelList.at(channelId).Urls[0];
        return url;
    }
    
    
    static void LoadPlaylist(const string& plistUrl, TChannelParserDelegate onChannelFound)
    {
        kodi::vfs::CFile* f = NULL;
        
        try {
            char buffer[1024];
            string data;
            
            // Download playlist
            LogDebug("EdemPlayer: loading playlist: %s", plistUrl.c_str());

            f = XBMC_OpenFile(plistUrl);
            if (!f)
                throw BadPlaylistFormatException("Failed to obtain playlist from server.");
            bool isEof = false;
            do{
                auto bytesRead = f->Read(buffer, sizeof(buffer));
                isEof = bytesRead <= 0;
                if(!isEof)
                    data.append(&buffer[0], bytesRead);
            }while(!isEof);
            f->Close();
            delete f;
            f = NULL;
            
            //LogError(">>> DUMP M3U : \n %s", data.c_str() );
            
            LogDebug("EdemPlayer: parsing playlist.");

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
                ParseChannelAndGroup(tag, plistIndex++, onChannelFound);
                pos = pos_end;
            }
            LogDebug("EdemPlayer: added %d channels from playlist." , plistIndex - 1);

        } catch (std::exception& ex) {
            LogError("EdemPlayer: exception during playlist loading: %s", ex.what());
            if(NULL != f) {
                f->Close();
                delete f;
                f = NULL;
            }
            
        }
    }
    
    static void ParseChannelAndGroup(const string& data, unsigned int plistIndex, TChannelParserDelegate onChannelFound)
    {
        // 0,РБК-ТВ
        // #EXTGRP:новости
        // http://882406a1.iptvspy.me/iptv/xxx/106/index.m3u8
        //
        // or
        //
        // #EXTINF:-1 catchup group-title="Познавательные" tvg-id="738589577doktor",Доктор
        // http://192.168.1.69:9094/InternationalID=386

        const char* c_REC = "tvg-rec";

        auto pos = data.find(',');
        if(string::npos == pos)
            throw BadPlaylistFormatException("Invalid channel block format: missing ','  delinmeter.");
        
        pos += 1;
        auto endlLine = data.find('\n');
        string name = data.substr(pos, endlLine - pos);
        rtrim(name);
        
        string groupName("Без группы");
        try { groupName = FindVar(data, 0, "group-title");} catch (...) {}

        string tail = data.substr(endlLine + 1);
        const char* c_GROUP = "#EXTGRP:";
        pos = tail.find(c_GROUP);
        // Optional channel group
        endlLine = 0;
        if(string::npos != pos) {
            //  throw BadPlaylistFormatException("Invalid channel block format: missing '#EXTGRP:'  tag.");
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
        
        bool hasArchive = true;
        try {
            hasArchive = stoul(FindVar(data, 0, c_REC)) > 0;
        } catch (...) {
            // Not mandatory var
        }

        
        Channel channel;
        channel.UniqueId = plistIndex;
        channel.Name = name;
        channel.Number = plistIndex;
        channel.Urls.push_back(url);
        channel.HasArchive = hasArchive;
        //channel.IconPath = m_logoUrl + FindVar(vars, 0, c_LOGO);
        channel.IsRadio = false;
        onChannelFound(channel,groupName);
    }
    
    
}

