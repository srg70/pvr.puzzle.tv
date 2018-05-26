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



namespace OttEngine
{
    using namespace std;
    using namespace ADDON;
    using namespace rapidjson;
    using namespace PvrClient;

    
    static const int secondsPerHour = 60 * 60;
    static const char* c_EpgCacheFile = "ott_epg_cache.txt";

    struct OttPlayer::ApiFunctionData
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
    
    
    const ParamList OttPlayer::ApiFunctionData::s_EmptyParams;
    
    OttPlayer::OttPlayer(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                         const std::string &baseUrl, const std::string &key)
    : ClientCoreBase(addonHelper, pvrHelper)
    , m_baseUrl(baseUrl)
    , m_key(key)
    {
        m_httpEngine = new HttpEngine(m_addonHelper);
        m_baseUrl = "http://" + m_baseUrl ;
        BuildChannelAndGroupList();
        LoadEpgCache(c_EpgCacheFile);
        OnEpgUpdateDone();
    }
    
    OttPlayer::~OttPlayer()
    {
        Cleanup();
    }
    void OttPlayer::Cleanup()
    {
        m_addonHelper->Log(LOG_NOTICE, "OttPlayer stopping...");
        
        if(m_httpEngine){
            SAFE_DELETE(m_httpEngine);
        }
        
        m_addonHelper->Log(LOG_NOTICE, "OttPlayer stopped.");
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
    
    void OttPlayer::BuildChannelAndGroupList()
    {
        void* f = NULL;

        try {
            char buffer[1024];
            string data;

            // Download playlist
            string playlistUrl = m_baseUrl + "/ottplayer/playlist.m3u";
            auto f = m_addonHelper->OpenFile(playlistUrl.c_str(), 0);
            if (!f)
                throw BadPlaylistFormatException("Failed to obtain playlist from server.");
            bool isEof = false;
            do{
                auto bytesRead = m_addonHelper->ReadFile(f, buffer, sizeof(buffer));
                isEof = bytesRead <= 0;
                if(!isEof)
                    data.append(&buffer[0], bytesRead);
            }while(!isEof);
            m_addonHelper->CloseFile(f);
            f = NULL;
            
            //m_addonHelper->Log(LOG_ERROR, ">>> DUMP M3U : \n %s", data.c_str() );

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
            m_addonHelper->Log(LOG_ERROR, "OttPlayer: exception during playlist loading: %s", ex.what());
            if(NULL != f) {
                m_addonHelper->CloseFile(f);
                f = NULL;
            }

        }
    }
    
    void OttPlayer::ParseChannelAndGroup(const string& data, unsigned int plistIndex)
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
        channel.Id = stoul(FindVar(vars, 0, c_ID));
        channel.Name = name;
        channel.Number = plistIndex;
        channel.Urls.push_back(url);
        channel.HasArchive = hasArchive;
        channel.IconPath = m_logoUrl + FindVar(vars, 0, c_LOGO);
        channel.IsRadio = false;
        AddChannel(channel);
        
        std::string groupName = FindVar(vars, 0, c_GROUP);
        
        const auto& groupList = GetGroupList();
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
    
    std::string OttPlayer::GetArchiveUrl(ChannelId channelId, time_t startTime, int duration)
    {
        string url = GetUrl(channelId);
        if(url.empty())
            return url;
        url += "?archive=" + n_to_string(startTime)+"&archive_end=" + n_to_string(startTime + duration);
        return  url;
    }
    
    void OttPlayer::GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries)
    {
        bool needMore = true;
        time_t moreStartTime = startTime;
        IClientCore::EpgEntryAction action = [&needMore, &moreStartTime, &epgEntries, channelId, startTime, endTime] (const EpgEntryList::value_type& i)
        {
            auto entryStartTime = i.second.StartTime;
            if (i.second.ChannelId == channelId  &&
                entryStartTime >= startTime &&
                entryStartTime < endTime)
            {
                moreStartTime = i.second.EndTime;
                needMore = moreStartTime < endTime;
                epgEntries.insert(i);
            }
        };
        ForEachEpg(action);
 
        if(needMore) {
            GetEpgForAllChannels(channelId, moreStartTime, endTime);
        } else {
            OnEpgUpdateDone();
        }
    }
    
    //template<class TFunc>
    void OttPlayer::GetEpgForAllChannels(ChannelId channelId, time_t startTime, time_t endTime)
    {
        try {
            string call = string("channel/") + n_to_string(channelId);
            ApiFunctionData apiParams(call.c_str());
            bool* shouldUpdate = new bool(false);

            unsigned int epgActivityCounter = ++m_epgActivityCounter;

            CallApiAsync(apiParams,  [this, startTime, shouldUpdate] (Document& jsonRoot)
            {
                for (auto& m : jsonRoot.GetObject()) {
                    if(std::stol(m.name.GetString()) < startTime) {
                        continue;
                    }
                    *shouldUpdate = true;
                    EpgEntry epgEntry;
                    epgEntry.ChannelId = stoul(m.value["ch_id"].GetString()) ;
                    epgEntry.Title = m.value["name"].GetString();
                    epgEntry.Description = m.value["descr"].GetString();
                    epgEntry.StartTime = m.value["time"].GetInt() ;
                    epgEntry.EndTime = m.value["time_to"].GetInt();
                    UniqueBroadcastIdType id = epgEntry.StartTime;
                    AddEpgEntry(id, epgEntry);
 
                }
            },
            [this, shouldUpdate, channelId, epgActivityCounter](const CActionQueue::ActionResult& s)
            {
                if(s.exception == NULL && *shouldUpdate){
                    m_pvrHelper->TriggerEpgUpdate(channelId);
                }
                delete shouldUpdate;
                if(epgActivityCounter == m_epgActivityCounter){
                    OnEpgUpdateDone();
                    SaveEpgCache(c_EpgCacheFile);
                }
            });
            
        } catch (ServerErrorException& ex) {
            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32002), ex.reason.c_str() );
        } catch (...) {
            LogError(" >>>>  FAILED receive EPG <<<<<");
        }
    }
    
    void OttPlayer::UpdateHasArchive(PvrClient::EpgEntry& entry)
    {
        auto channel = GetChannelList().find(entry.ChannelId);
        entry.HasArchive = channel != GetChannelList().end() &&  channel->second.HasArchive;
        
        if(!entry.HasArchive)
            return;
        
        time_t to = time(nullptr);
        entry.HasArchive = entry.StartTime < to;

    }
    
    string OttPlayer::GetUrl(ChannelId channelId)
    {
        string url = GetChannelList().at(channelId).Urls[0];
        const char* c_KEY = "{KEY}";
        auto pos = url.find(c_KEY);
        if(string::npos == pos) {
            return string();
        }
        url.replace(pos, strlen(c_KEY), m_key);
        return url;
    }
    
    template <typename TParser>
    void OttPlayer::CallApiFunction(const ApiFunctionData& data, TParser parser)
    {
        P8PLATFORM::CEvent event;
        std::exception_ptr ex = nullptr;
        CallApiAsync(data, parser, [&](const CActionQueue::ActionResult& s) {
            ex = s.exception;
            event.Signal();
        });
        event.Wait();
        if(ex)
        std::rethrow_exception(ex);
    }
    
    template <typename TParser, typename TCompletion>
    void OttPlayer::CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion)
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
        m_addonHelper->Log(LOG_DEBUG, "Calling '%s'.",  data.name.c_str());
        
        std::function<void(const std::string&)> parserWrapper = [=](const std::string& response)
        {
            m_addonHelper->Log(LOG_DEBUG, "Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);
            
            //            if(data.name.compare( "get_url") == 0)
            //                m_addonHelper->Log(LOG_DEBUG, response.substr(0, 16380).c_str());
            
            ParseJson(response, [&] (Document& jsonRoot)
            {
                if (jsonRoot.IsObject()) {
                    parser(jsonRoot);
                }
            });
        };
        m_httpEngine->CallApiAsync(strRequest, parserWrapper, [=](const CActionQueue::ActionResult& s)
       {
           completion(s);
       });
    }
}




