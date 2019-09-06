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
#include <ctime>
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/timeutils.h"
#include "p8-platform/util/util.h"
#include "helpers.h"
#include "ott_player.h"
#include "HttpEngine.hpp"
#include "globals.hpp"
#include "XMLTV_loader.hpp"

namespace OttEngine
{
    
    using namespace Globals;
    using namespace std;
    using namespace ADDON;
    using namespace rapidjson;
    using namespace PvrClient;

    
    static const int secondsPerHour = 60 * 60;
    static const char* c_EpgCacheFile = "ott_epg_cache.txt";

    struct Core::ApiFunctionData
    {
        //    ApiFunctionData(const char* _name)
        //    : name(_name) , params(s_EmptyParams), api_ver (API_2_2)
        //    {}
        //
        ApiFunctionData(const char* _name, const ParamList& _params = s_EmptyParams)
        : name(_name) , params(_params)
        {}
        std::string name;
        const ParamList params;
        static const  ParamList s_EmptyParams;
    };
    
    
    const ParamList Core::ApiFunctionData::s_EmptyParams;
    
    Core::Core(const std::string &baseUrl, const std::string &key)
    : m_baseUrl(baseUrl)
    , m_key(key)
    {
        m_baseUrl = "http://" + m_baseUrl ;
    }
    
    void Core::Init(bool clearEpgCache)
    {
        RebuildChannelAndGroupList();
        if(clearEpgCache) {
            ClearEpgCache(c_EpgCacheFile);
        } else {
            LoadEpgCache(c_EpgCacheFile);
        }
    }
    
    Core::~Core()
    {
        Cleanup();
        PrepareForDestruction();
    }
    void Core::Cleanup()
    {
//        LogNotice("OttPlayer stopping...");
//
//
//        LogNotice("OttPlayer stopped.");
    }
    
    
    static string FindVar(const string& data, string::size_type pos, const char* varTag)
    {
        pos = data.find(varTag, pos);
        if(string::npos == pos)
            throw BadPlaylistFormatException((string("Invalid playlist format: missing variable ") + varTag).c_str());
        pos += strlen(varTag);
        
        auto pos_end = data.find("\"", pos);
        if(string::npos == pos)
            throw BadPlaylistFormatException((string("Invalid playlist format: missing end of variable ") + varTag).c_str());
        return data.substr(pos, pos_end - pos);

    }
    
    void Core::BuildChannelAndGroupList()
    {
        void* f = NULL;

        try {
            char buffer[1024];
            string data;

            // Download playlist
            string playlistUrl = m_baseUrl + "/ottplayer/playlist.m3u";
            auto f = XBMC->OpenFile(playlistUrl.c_str(), 0);
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
            
            //LogError(">>> DUMP M3U : \n %s", data.c_str() );

            // Parse gloabal variables
            //#EXTM3U url-epg="http://example.com/epg_url/" url-logo="http://example.com/images/"
            const char* c_M3U = "#EXTM3U";
            const char* c_EPG = "url-epg=\"";
            const char* c_LOGO = "url-logo=\"";
            
            auto pos = data.find(c_M3U);
            if(string::npos == pos)
                throw BadPlaylistFormatException("Invalid playlist format: missing #EXTM3U tag.");
            pos += strlen(c_M3U);
            m_epgUrl = FindVar(data, pos, c_EPG);
            m_logoUrl = FindVar(data, pos, c_LOGO);

            // Parse channels
            //#EXTINF:-1 tvg-id="131" tvg-logo="perviy.png" group-title="Общие" tvg-rec="1" ,Первый канал HD
            //http://ott.watch/stream/{KEY}/131.m3u8

            const char* c_INF = "#EXTINF:";
            pos = data.find(c_INF, pos);
            unsigned int plistIndex = 0;
            while(string::npos != pos){
                pos += strlen(c_INF);
                auto pos_end = data.find(c_INF, pos);
                string::size_type tagLen = (std::string::npos == pos_end) ? std::string::npos : pos_end - pos;
                string tag = data.substr(pos, tagLen);
                ParseChannelAndGroup(tag, plistIndex++);
                pos = pos_end;
            }
        } catch (std::exception& ex) {
            LogError("OttPlayer: exception during playlist loading: %s", ex.what());
            if(NULL != f) {
                XBMC->CloseFile(f);
                f = NULL;
            }

        }
    }
    
    void Core::ParseChannelAndGroup(const string& data, unsigned int plistIndex)
    {
        //-1 tvg-id="131" tvg-logo="perviy.png" group-title="Общие" tvg-rec="1" ,Первый канал HD
        //http://ott.watch/stream/{KEY}/131.m3u8
       
        auto pos = data.find(",");
        if(string::npos == pos)
            throw BadPlaylistFormatException("Invalid channel block format: missing ','  delinmeter.");

        const char* c_ID = "tvg-id=\"";
        const char* c_LOGO = "tvg-logo=\"";
        const char* c_GROUP = "group-title=\"";
        const char* c_REC = "tvg-rec=\"";
        
        string vars = data.substr(0, pos);
        string tail = data.substr(pos + 1);
        pos = tail.find('\n');
        if(std::string::npos == pos)
            throw BadPlaylistFormatException("Invalid channel block format: missing NEW LINE.");
        string name = tail.substr(0, pos);
        string url = tail.substr(++pos);
        rtrim(url);

        bool hasArchive = false;
        try {
            hasArchive = stoul(FindVar(vars, 0, c_REC)) == 1;
        } catch (...) {
            // Not mandatory var
        }
        
        Channel channel;
        channel.UniqueId = channel.EpgId = stoul(FindVar(vars, 0, c_ID));
        channel.Name = name;
        channel.Number = plistIndex;
        channel.Urls.push_back(url);
        channel.HasArchive = hasArchive;
        channel.IconPath = m_logoUrl + FindVar(vars, 0, c_LOGO);
        channel.IsRadio = false;
        AddChannel(channel);
        
        std::string groupName = FindVar(vars, 0, c_GROUP);
        
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
    
    std::string Core::GetArchiveUrl(ChannelId channelId, time_t startTime, int duration)
    {
        string url = GetUrl(channelId);
        if(url.empty())
            return url;
        url += "?archive=" + n_to_string(startTime)+"&archive_end=" + n_to_string(startTime + duration);
        return  url;
    }
    
    void Core::UpdateEpgForAllChannels(time_t startTime, time_t endTime)
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
        m_epgUpdateInterval.Init(12*60*60*1000);

        for (const auto& ch : m_channelList) {
            GetEpgForChannel(ch.second.EpgId, startTime, endTime);
        }
        //SaveEpgCache(c_EpgCacheFile);
    }

    void Core::GetEpgForChannel(ChannelId channelId, time_t startTime, time_t endTime)
    {
        try {
            string call = string("channel/") + n_to_string(channelId);
            ApiFunctionData apiParams(call.c_str());
            bool* shouldUpdate = new bool(false);
            
            unsigned int epgActivityCounter = ++m_epgActivityCounter;
            
            //CallApiFunction(apiParams,  [this, startTime, shouldUpdate] (Document& jsonRoot)
            CallApiAsync(apiParams,  [this, startTime, shouldUpdate] (Document& jsonRoot)
                         {
                             for (auto& m : jsonRoot.GetObject()) {
                                 if(std::stol(m.name.GetString()) < startTime) {
                                     continue;
                                 }
                                 *shouldUpdate = true;
                                 EpgEntry epgEntry;
                                 epgEntry.UniqueChannelId = stoul(m.value["ch_id"].GetString()) ;
                                 epgEntry.Title = m.value["name"].GetString();
                                 epgEntry.Description = m.value["descr"].GetString();
                                 epgEntry.StartTime = m.value["time"].GetInt() ;
                                 epgEntry.EndTime = m.value["time_to"].GetInt();
                                 UniqueBroadcastIdType id = epgEntry.StartTime;
                                 AddEpgEntry(id, epgEntry);
                                 
                             }
                         },
                         [this, shouldUpdate, channelId, epgActivityCounter](const ActionQueue::ActionResult& s)
                         {
                             if(s.exception == NULL && *shouldUpdate){
                                 PVR->TriggerEpgUpdate(channelId);
                             }
                             delete shouldUpdate;
                             if(epgActivityCounter == m_epgActivityCounter){
//                                 OnEpgUpdateDone();
                                 SaveEpgCache(c_EpgCacheFile);
                             }
                         });
            
        } catch (ServerErrorException& ex) {
            XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32002), ex.reason.c_str());
        } catch (...) {
            LogError(" >>>>  FAILED receive EPG <<<<<");
        }
    }
    
    void Core::UpdateHasArchive(PvrClient::EpgEntry& entry)
    {
        auto pCahnnel = std::find_if(m_channelList.begin(), m_channelList.end(), [&entry] (const ChannelList::value_type& ch) {
            return ch.second.UniqueId == entry.UniqueChannelId;
        });

        entry.HasArchive = pCahnnel != m_channelList.end() &&  pCahnnel->second.HasArchive;
        
        if(!entry.HasArchive)
            return;
        
        time_t now = time(nullptr);
        time_t epgTime = m_addCurrentEpgToArchive ? entry.StartTime : entry.EndTime;
        entry.HasArchive = epgTime < now;

    }
    
    string Core::GetUrl(ChannelId channelId)
    {
        string url = m_channelList.at(channelId).Urls[0];
        const char* c_KEY = "{KEY}";
        auto pos = url.find(c_KEY);
        if(string::npos == pos) {
            return string();
        }
        url.replace(pos, strlen(c_KEY), m_key);
        return url;
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
    
    template <typename TParser, typename TCompletion>
    void Core::CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion)
    {
        
        // Build HTTP request
        string query;
        ParamList::const_iterator runner = data.params.begin();
        ParamList::const_iterator first = runner;
        ParamList::const_iterator end = data.params.end();
        
        for (; runner != end; ++runner)  {
            query += runner == first ? "?" : "&";
            query += runner->first + '=' + runner->second;
        }
        std::string strRequest = m_epgUrl;
        strRequest += data.name + query;
        auto start = P8PLATFORM::GetTimeMs();
        LogDebug("Calling '%s'.",  data.name.c_str());
        
        std::function<void(const std::string&)> parserWrapper = [=](const std::string& response)
        {
            LogDebug("Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);
            
            //            if(data.name.compare( "get_url") == 0)
            //                LogDebug(response.substr(0, 16380).c_str());
            
            ParseJson(response, [&] (Document& jsonRoot)
            {
                if (jsonRoot.IsObject()) {
                    parser(jsonRoot);
                }
            });
        };
        m_httpEngine->CallApiAsync(strRequest, parserWrapper, [=](const ActionQueue::ActionResult& s)
       {
           completion(s);
       });
    }
}




