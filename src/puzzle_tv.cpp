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

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <assert.h>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <ctime>
#include <list>
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/timeutils.h"
#include "p8-platform/util/util.h"
#include "p8-platform/util/StringUtils.h"
#include "helpers.h"
#include "puzzle_tv.h"
#include "HttpEngine.hpp"
#include "XMLTV_loader.hpp"
#include "globals.hpp"
#include "base64.h"

using namespace Globals;
using namespace std;
using namespace ADDON;
using namespace rapidjson;
using namespace PuzzleEngine;
using namespace PvrClient;

static const int secondsPerHour = 60 * 60;
static const char* c_EpgCacheFile = "puzzle_epg_cache.txt";
//static PuzzleTV::TChannelStreams s_NoStreams;

struct PuzzleTV::ApiFunctionData
{
     ApiFunctionData(const char* _name,  uint16_t _port,  const ParamList* _params = nullptr)
    : name(_name) , port(_port), attempt(0)
    {
        if(_params){
            params = *_params;
        }
    }
    ~ApiFunctionData()
    {
    }
    const std::string name;
    const uint16_t port;
    ParamList params;
    mutable int attempt;
};

static bool IsAceUrl(const std::string& url, std::string& aceServerUrlBase)
{
    auto pos = url.find(":6878/ace/");
    const bool isAce = pos != std::string::npos;
    if(isAce) {
        const char* httpStr = "http://";
        auto startPos = url.find(httpStr);
        if(std::string::npos != startPos){
            startPos += strlen(httpStr);
            aceServerUrlBase = url.substr(startPos, pos - startPos);
        }
    }
    return isAce;
}

static std::string ToPuzzleChannelId(PvrClient::ChannelId channelId){
    string strId = n_to_string_hex(channelId);
    int leadingZeros = 8 - strId.size();
    while(leadingZeros--)
    strId = "0" + strId;
    return strId;
}

std::string PuzzleTV::EpgUrlForPuzzle3() const {
    return std::string("http://") + m_serverUri + ":" + n_to_string(m_serverPort) + "/epg/xmltv" ;
}

PuzzleTV::PuzzleTV(ServerVersion serverVersion, const char* serverUrl, uint16_t serverPort) :
    m_serverUri(serverUrl),
    m_serverPort(serverPort),
    m_epgServerPort(8085),
    m_epgUrl("https://iptvx.one/epg/epg.xml.gz"),
    m_serverVersion(serverVersion),
    m_isAceRunning(false)
{
}

void PuzzleTV::Init(bool clearEpgCache)
{
    RebuildChannelAndGroupList();
    if(clearEpgCache)
        ClearEpgCache(c_EpgCacheFile);
    else
        LoadEpgCache(c_EpgCacheFile);
    LoadEpg();
    UpdateArhivesAsync();
    
//        CallRpcAsync("{\"jsonrpc\": \"2.0\", \"method\": \"Files.GetDirectory\", \"params\": {\"directory\": \"plugin://plugin.video.pazl.arhive\"},\"id\": 1}",
//                     [&] (Document& jsonRoot)
//                     {
//                         LogDebug("Puzzle Files.GetDirectory respunse. RPC version: %s", jsonRoot["jsonrpc"].GetString());
//                         for (const auto& ch : jsonRoot["result"]["files"].GetArray()) {
//                             LogDebug("Channel content:");
//                             LogDebug("\t filetype = %s", ch["filetype"].GetString());
//                             LogDebug("\t type =  %s", ch["type"].GetString());
//                             LogDebug("\t file =  %s", ch["file"].GetString());
//                             LogDebug("\t label =  %s", ch["label"].GetString());
//    }
//                     },
//                     [&](const CActionQueue::ActionResult& s) {
//                     });
}

PuzzleTV::~PuzzleTV()
{
    Cleanup();
}
void PuzzleTV::Cleanup()
{
    LogNotice("PuzzleTV stopping...");

    if(m_httpEngine)
        SAFE_DELETE(m_httpEngine);
    
    LogNotice( "PuzzleTV stopped.");
}

void PuzzleTV::BuildChannelAndGroupList()
{
    struct NoCaseComparator : binary_function<string, string, bool>
    {
        inline bool operator()(const string& x, const string& y) const
        {
            return StringUtils::CompareNoCase(x, y) < 0;
        }
    };

    typedef map<string, pair<Channel, string>, NoCaseComparator> PlaylistContent;

    try {

        PlaylistContent plistContent;

        const bool isPuzzle2 = m_serverVersion == c_PuzzleServer2;
        // Get channels from server
        const char* cmd = isPuzzle2 ? "/get/json" : "/channels/json";
        ApiFunctionData params(cmd, m_serverPort);
        CallApiFunction(params, [&plistContent, isPuzzle2] (Document& jsonRoot)
        {
            const Value &channels = jsonRoot["channels"];
            Value::ConstValueIterator itChannel = channels.Begin();
            for(; itChannel != channels.End(); ++itChannel)
            {
                Channel channel;
                char* dummy;
                channel.Id = strtoul((*itChannel)["id"].GetString(), &dummy, 16);
                channel.Number = channel.Id;
                channel.Name = (*itChannel)["name"].GetString();
                channel.IconPath = (*itChannel)["icon"].GetString();
                channel.IsRadio = false ;
                channel.HasArchive = false;
                if(isPuzzle2)
                    channel.Urls.push_back((*itChannel)["url"].GetString());
                std::string groupName = (*itChannel)["group"].GetString();
                plistContent[channel.Name] = PlaylistContent::mapped_type(channel,groupName);
            }
        });

        if(m_epgType == c_EpgType_File) {

            m_epgToServerLut.clear();
            using namespace XMLTV;
            
            auto pThis = this;
            // Build LUT channels ID from EPG to Server
            ChannelCallback onNewChannel = [&plistContent, pThis](const EpgChannel& newChannel){
                if(plistContent.count(newChannel.strName) != 0) {
                    auto& plistChannel = plistContent[newChannel.strName].first;
                    pThis->m_epgToServerLut[newChannel.id] = plistChannel.Id;
                    if(plistChannel.IconPath.empty())
                        plistChannel.IconPath = newChannel.strIcon;
                }
            };
            
            XMLTV::ParseChannels(m_epgUrl, onNewChannel);
        }
        
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
       
    } catch (ServerErrorException& ex) {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32006), ex.reason.c_str());
    } catch (...) {
        LogError(">>>>  FAILED to build channel list <<<<<");
    }
    

}

//std::string PuzzleTV::GetArchive(    ChannelId channelId, time_t startTime)
//{
//    string url;
//    ParamList params;
//    params["cid"] = n_to_string(channelId);
//    params["time"] = n_to_string(startTime);
//     try {
//         ApiFunctionData apiParams("/get/archive_next", , m_serverPort, params);
//         CallApiFunction(apiParams, [&] (Document& jsonRoot)
//        {
//            const Value & archive = jsonRoot["archive"];
//
//            url = archive["url"].GetString();
//            //Log((string(" >>>>  URL: ") + url +  "<<<<<").c_str());
//        });
//     } catch (ServerErrorException& ex) {
//        char* message  = XBMC->GetLocalizedString(32006);
//        XBMC->QueueNotification(QUEUE_ERROR, message), ex.reason.c_str());
//        XBMC->FreeString(message);
//     } catch (...) {
//         LogError(" >>>>  FAILED receive archive <<<<<");
//    }
//    return url;
//}

UniqueBroadcastIdType PuzzleTV::AddXmlEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
{
    if(m_epgToServerLut.count(xmlEpgEntry.iChannelId) == 0) {
        //LogError("PuzzleTV::AddXmlEpgEntry(): XML EPG entry '%s' for unknown channel %d", xmlEpgEntry.strTitle.c_str(), xmlEpgEntry.iChannelId);
        return c_UniqueBroadcastIdUnknown;
    }
    
    unsigned int id = (unsigned int)xmlEpgEntry.startTime;

    EpgEntry epgEntry;
    epgEntry.ChannelId = m_epgToServerLut[xmlEpgEntry.iChannelId];
    epgEntry.Title = xmlEpgEntry.strTitle;
    epgEntry.Description = xmlEpgEntry.strPlot;
    epgEntry.StartTime = xmlEpgEntry.startTime;
    epgEntry.EndTime = xmlEpgEntry.endTime;
    return AddEpgEntry(id, epgEntry);
}

void PuzzleTV::UpdateHasArchive(PvrClient::EpgEntry& entry)
{
    auto channel = m_channelList.find(entry.ChannelId);
    entry.HasArchive = channel != m_channelList.end() &&  channel->second.HasArchive;
    
    if(!entry.HasArchive)
        return;

    time_t now = time(nullptr);
    time_t epgTime = m_addCurrentEpgToArchive ? entry.StartTime : entry.EndTime;
    const time_t archivePeriod = 3 * 24 * 60 * 60; //3 days in secs
    time_t from = now - archivePeriod;
    entry.HasArchive = epgTime > from && epgTime < now;


}

void PuzzleTV::UpdateEpgForAllChannels(time_t startTime, time_t endTime)
{
    // Assuming server provides EPG at least fo next 12 hours
    // To reduce amount of API calls, allow next EPG update
    // after either 12 hours or  endTime
    time_t now = time(nullptr);
    time_t nextUpdateAt = std::min(now + 12*60*60, endTime);
    int32_t interval = nextUpdateAt - now;
    if(interval > 0)
        m_epgUpdateInterval.Init(interval*1000);

    try {
        LoadEpg();
        SaveEpgCache(c_EpgCacheFile);
//        } catch (ServerErrorException& ex) {
//            XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32002), ex.reason.c_str() );
    } catch (...) {
        LogError(" >>>>  FAILED receive EPG <<<<<");
    }
}

static bool time_compare (const EpgEntry& first, const EpgEntry& second)
{
    return first.StartTime < second.StartTime;
}

void PuzzleTV::LoadEpg()
{
    //    using namespace XMLTV;
    auto pThis = this;
    
    if(m_epgType == c_EpgType_File) {
        
        XMLTV::EpgEntryCallback onEpgEntry = [pThis] (const XMLTV::EpgEntry& newEntry) {pThis->AddXmlEpgEntry(newEntry);};
        XMLTV::ParseEpg(m_epgUrl, onEpgEntry);
        
    } else if(m_serverVersion == c_PuzzleServer2){ // Puzzle 2 server
        auto pThis = this;
        // Assuming Moscow time EPG +3:00
        long offset = -(3 * 60 * 60) - XMLTV::LocalTimeOffset();
        
        ApiFunctionData apiParams("/channel/json/id=all", m_epgServerPort);
        try {
            CallApiFunction(apiParams, [pThis, offset] (Document& jsonRoot) {
                if(!jsonRoot.IsObject()) {
                    LogError("PuzzleTV: wrong JSON format of EPG ");
                    return;
                }
                
                std::for_each(jsonRoot.MemberBegin(), jsonRoot.MemberEnd(), [pThis, offset]  ( Value::ConstMemberIterator::Reference & i) {
                    // Find channel object
                    if(i.value.IsObject() && i.value.HasMember("title") && !i.value.HasMember("plot")) {
                        auto channelIdStr = i.name.GetString();
                        char* dummy;
                        ChannelId channelId  = strtoul(channelIdStr, &dummy, 16);
                        LogDebug("Found channel %s (0x%X)", i.name.GetString(), channelId);
                        
                        // Parse EPG items of channel object
                        auto& epgObj = i.value;
                        list<EpgEntry> serverEpg;
                        std::for_each(epgObj.MemberBegin(), epgObj.MemberEnd(), [pThis, offset, channelId, &serverEpg]  ( Value::ConstMemberIterator::Reference & epgItem) {
                            // Find EPG item
                            if(epgItem.value.IsObject() && epgItem.value.HasMember("plot")  && epgItem.value.HasMember("img")  && epgItem.value.HasMember("title")) {
                                EpgEntry epgEntry;
                                epgEntry.ChannelId = channelId;
                                string s = epgItem.name.GetString();
                                s = s.substr(0, s.find('.'));
                                unsigned long l = stoul(s.c_str());
                                epgEntry.StartTime = (time_t)l + offset;
                                epgEntry.Title = epgItem.value["title"].GetString();
                                epgEntry.Description = epgItem.value["plot"].GetString();
                                // Causes Kodi crash on reload epg
                                //                                epgEntry.IconPath = epgItem.value["img"].GetString();
                                serverEpg.push_back(epgEntry);
                            }
                        });
                        serverEpg.sort(time_compare);
                        auto runner = serverEpg.begin();
                        auto end = serverEpg.end();
                        if(runner != end){
                            auto pItem = runner++;
                            while(runner != end) {
                                pItem->EndTime = runner->StartTime;
                                pThis->AddEpgEntry(pItem->StartTime, *pItem);
                                runner++; pItem++;
                            }
                            LogDebug(" Puzzle Server: channel ID=%X has %d EPGs. From %s to %s",
                                     channelId,
                                     serverEpg.size(),
                                     time_t_to_string(serverEpg.front().StartTime).c_str(),
                                     time_t_to_string(serverEpg.back().StartTime).c_str());
                        }
                    }
                });
            });
        } catch (...) {
            LogError("PuzzleTV: exception on lodaing JSON EPG");
        }
    } else {
        LogError("PuzzleTV: unknown EPG source type %d", m_epgType);
    }
}

bool PuzzleTV::CheckChannelId(ChannelId channelId)
{
    if(m_channelList.count( channelId ) != 1) {
        LogError("PuzzleTV::CheckChannelId: Unknown channel ID= %d", channelId);
        return false;
    }
    return true;
}

#pragma mark - Streams

string PuzzleTV::GetUrl(ChannelId channelId)
{
    if(!CheckChannelId(channelId)) {
        return string();
    }
    
    if(m_channelList.at(channelId).Urls.size() == 0){
        UpdateChannelSources(channelId);
    }
    //auto& urls = m_channelList.at(channelId).Urls;

    string url;

    auto sources = GetSourcesForChannel(channelId);
    while (!sources.empty()) {
        const auto streams = sources.top()->second.Streams;
        auto goodStream = std::find_if(streams.begin(), streams.end(), [](PuzzleSource::TStreamsQuality::const_reference stream) {return stream.second;});
        if(goodStream != streams.end()) {
            url = goodStream->first;
            break;
        }
        sources.pop();
    }
    
    if(url.empty()) {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32017));
        return url;
    }
    string aceServerUrlBase;
    if(IsAceUrl(url, aceServerUrlBase)){
        if(!CheckAceEngineRunning(aceServerUrlBase.c_str()))  {
            XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32021));
            return string();
        }
    }
    return  url;
}

string PuzzleTV::GetNextStream(ChannelId channelId, int currentChannelIdx)
{
    if(!CheckChannelId(channelId))
        return string();

    // NOTE: assuming call to PuzzleTV::OnOpenStremFailed before GetNextStream()
    // where bad stream should be marked as bad. So return first good stream
    string url;
    auto sources = GetSourcesForChannel(channelId);
    while (!sources.empty()) {
        const auto streams = sources.top()->second.Streams;
        auto goodStream = std::find_if(streams.begin(), streams.end(), [](PuzzleSource::TStreamsQuality::const_reference stream) {return stream.second;});
        if(goodStream != streams.end()) {
            url = goodStream->first;
            // Check whether Ace Engine is running for ace link
            string aceServerUrlBase;
            if(IsAceUrl(url, aceServerUrlBase)){
                if(CheckAceEngineRunning(aceServerUrlBase.c_str()))  {
                    break;
                }
            }
        }
        sources.pop();
    }
    
    return url;
}

void PuzzleTV::RateStream(ChannelId channelId, const std::string& streamUrl, bool isGood)
{
    if(streamUrl.empty())
        return;
    if(!isGood) {
        string aceServerUrlBase;
        if(IsAceUrl(streamUrl, aceServerUrlBase)){
            if(!CheckAceEngineRunning(aceServerUrlBase.c_str()))  {
                // Don't rate bad ace stream link when engine is not running.
                return;
            }
        }
    }
    string encoded = base64_encode(reinterpret_cast<const unsigned char*>(streamUrl.c_str()), streamUrl.length());
    std::string strRequest = string("http://") + m_serverUri + ":" + n_to_string(m_serverPort) + "/ratio/" + encoded + (isGood ? "/good" : "/bad");

    m_httpEngine->CallApiAsync(strRequest, [](const std::string& responce) {},  [](const ActionQueue::ActionResult& ss){
        if(ss.exception != nullptr){
            try {
                std::rethrow_exception(ss.exception);
            } catch (std::exception& ex) {
                LogError("PuzzleTV::RateStream(): FAILED with error %s", ex.what());
           }
        }
    });
}

void PuzzleTV::OnOpenStremFailed(ChannelId channelId, const std::string& streamUrl)
{
    // Mark stream as bad and optionaly disable bad source
    TChannelSources& sources = m_sources[channelId];
    for(auto& source : sources) {
        if(source.second.Streams.count(streamUrl) != 0){
            source.second.Streams.at(streamUrl) = false;
            if ( std::none_of(source.second.Streams.begin(), source.second.Streams.end(), [](const PuzzleSource::TStreamsQuality::value_type& isGood){return isGood.second;}) ) {
                DisableSource(channelId, source.first);
            }
            break;
        }
    }
}

void PuzzleTV::GetSourcesMetadata(TChannelSources& channelSources)
{
    return;
    
    if(m_serverVersion == c_PuzzleServer2)
        return;
    
    try {
        for (auto& source : channelSources) {
            string encoded = base64_encode(reinterpret_cast<const unsigned char*>(source.first.c_str()), source.first.length());
            std::string cmd =  string("/ratio/") + encoded + "/json";
            
            ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
            CallApiFunction(apiParams, [&] (Document& jsonRoot)
                            {
                                if(jsonRoot.HasMember("good")){
                                    source.second.RatingGood = jsonRoot["good"].GetInt64();
                                }
                                if(jsonRoot.HasMember("bad")){
                                    source.second.RatingBad = jsonRoot["bad"].GetInt64();
                                }
                            });
        }
    } catch (ServerErrorException& ex) {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32006), ex.reason.c_str());
    } catch (...) {
        LogError("PuzzleTV::GetSourceMetadata:  FAILED to get metadata");
    }
}


void PuzzleTV::UpdateUrlsForChannel(PvrClient::ChannelId channelId)
{
    if(!CheckChannelId(channelId))
        return;
 
    Channel ch = m_channelList.at(channelId);
    auto& urls = ch.Urls;
    //if(urls.size() <2)
    {
        urls.clear();
        try {
            const string  strId = ToPuzzleChannelId(channelId);
            if(m_serverVersion == c_PuzzleServer2){
                std::string cmd = string("/get/streams/") + strId;
                ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
                CallApiFunction(apiParams, [&urls] (Document& jsonRoot)
                                {
                                    if(!jsonRoot.IsArray())
                                        return;
                                    std::for_each(jsonRoot.Begin(), jsonRoot.End(), [&]  (const Value & i) mutable
                                                  {
                                                      auto url = i.GetString();
                                                      urls.push_back(url);
                                                      LogDebug(" >>>>  URL: %s <<<<<",  url);
                                                      
                                                  });
                                });
            } else {
                auto& cacheSources = m_sources[channelId];
                std::string cmd = string("/streams/json_ds/") + strId;
                ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
                CallApiFunction(apiParams, [&urls, &cacheSources] (Document& jsonRoot)
                                {
                                    if(!jsonRoot.IsArray())
                                        return;
                                    
                                    //dump_json(jsonRoot);

                                    std::for_each(jsonRoot.Begin(), jsonRoot.End(), [&]  (const Value & s) mutable
                                                  {
                                                      if(!s.HasMember("cache")) {
                                                          throw MissingApiException("Missing 'cache' field");
                                                      }
                                                      if(!s.HasMember("streams")) {
                                                          throw MissingApiException("Missing 'streams' field");
                                                      }
                                                      auto cacheUrl = s["cache"].GetString();
                                                      PuzzleSource& source =  cacheSources[cacheUrl];

 
                                                      auto streams = s["streams"].GetArray();
                                                      std::for_each(streams.Begin(), streams.End(), [&]  (const Value & st){
                                                          auto url = st.GetString();
                                                          urls.push_back(url);
                                                          source.Streams[url] = true;
                                                          LogDebug(" >>>>  URL: %s <<<<<",  url);
                                                      });
                                                  });
                                });
            }
            
            AddChannel(ch);
        } catch (ServerErrorException& ex) {
            XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32006), ex.reason.c_str());
        } catch (MissingApiException& ex){
            LogError(" Bad JSON responce for '/streams/json_ds/': %s", ex.what());
        } catch (...) {
            LogError(" >>>>  FAILED to get URL for channel ID=%d <<<<<", channelId);
        }
    }
}

void PuzzleTV::UpdateChannelSources(ChannelId channelId)
{
    if(m_serverVersion == c_PuzzleServer2 || !CheckChannelId(channelId))
        return;
    
    TChannelSources sources;
    try {
        string cmd = string("/cache_url/") + ToPuzzleChannelId(channelId) + "/json";
        ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
        CallApiFunction(apiParams, [&sources] (Document& jsonRoot)
                        {
                            if(!jsonRoot.IsArray())
                                return;
                            //dump_json(jsonRoot);

                            std::for_each(jsonRoot.Begin(), jsonRoot.End(), [&]  (const Value & i) mutable
                                          {
                                              if(!i.HasMember("url")) {
                                                  return;
                                              }
                                              TCacheUrl cacheUrl = i["url"].GetString();
                                              PuzzleSource& source =  sources[cacheUrl];

                                              if(i.HasMember("serv")) {
                                                  source.Server = i["serv"].GetString();
                                              } else {
                                                  source.Server = cacheUrl.find("acesearch") != string::npos ?  "ASE" : "HTTP";
                                              }
                                              if(i.HasMember("lock")) {
                                                  source.IsChannelLocked = i["lock"].GetBool();
                                              }
                                              if(i.HasMember("serv_on")) {
                                                  source.IsServerOn = i["serv_on"].GetBool();
                                              }
                                              if(i.HasMember("priority")) {
                                                  auto s = i["priority"].GetString();
                                                  if(strlen(s) > 0) {
                                                      source.Priority = atoi(s);
                                                  }
                                              }
                                              if(i.HasMember("id")) {
                                                  auto s = i["id"].GetString();
                                                  if(strlen(s) > 0) {
                                                      source.Id = atoi(s);
                                                  }
                                              }
                                          });
                        });
    } catch (ServerErrorException& ex) {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32006), ex.reason.c_str());
    } catch (...) {
        LogError(" >>>>  FAILED to get sources list for channel ID=%d <<<<<", channelId);
    }
    

    GetSourcesMetadata(sources);
    m_sources[channelId] = sources;
    
    UpdateUrlsForChannel(channelId);

}

PuzzleTV::TPrioritizedSources PuzzleTV::GetSourcesForChannel(ChannelId channelId)
{
    if(m_sources.count(channelId) == 0) {
        UpdateChannelSources(channelId);
    }
    
    TPrioritizedSources result;
    const auto& sources = m_sources[channelId];

    for (const auto& source : sources) {
        result.push(&source);
    }
    return result;
}

void PuzzleTV::EnableSource(PvrClient::ChannelId channelId, const TCacheUrl& cacheUrl)
{
    if(m_sources.count(channelId) == 0)
        return;
    for (auto& source : m_sources[channelId]) {
        if(source.first == cacheUrl && !source.second.IsOn()){
            source.second.IsChannelLocked = false;
            //http://127.0.0.1:8185/back_list/base64url/unlock/CID
            const string  strId = ToPuzzleChannelId(channelId);
            string encoded = base64_encode(reinterpret_cast<const unsigned char*>(cacheUrl.c_str()), cacheUrl.length());
            string cmd = string("/back_list/")+ encoded + "/unlock/" + strId;
            
            
            ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
            CallApiAsync(apiParams, [](Document&){}, [channelId, strId](const ActionQueue::ActionResult& s) {
                if(s.exception){
                    LogError("PuzzleTV: FAILED to disble source for channel %s", strId.c_str());
                }
            });

            UpdateUrlsForChannel(channelId);
           break;
        }
    }
}
void PuzzleTV::DisableSource(PvrClient::ChannelId channelId, const TCacheUrl& cacheUrl)
{
    if(m_sources.count(channelId) == 0)
        return;
    for (auto& source : m_sources[channelId]) {
        if(source.first == cacheUrl && source.second.IsOn()){
            source.second.IsChannelLocked = true;
            //http://127.0.0.1:8185/back_list/base64url/lock/CID
            const string  strId = ToPuzzleChannelId(channelId);
            string encoded = base64_encode(reinterpret_cast<const unsigned char*>(cacheUrl.c_str()), cacheUrl.length());
            string cmd = string("/back_list/")+ encoded + "/lock/" + strId;
            
            
            ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
            CallApiAsync(apiParams, [](Document&){}, [channelId, strId](const ActionQueue::ActionResult& s) {
                if(s.exception){
                    LogError("PuzzleTV: FAILED to disble source for channel %s", strId.c_str());
                }
            });
            
            UpdateUrlsForChannel(channelId);
            break;
        }
    }

}


#pragma mark - API Call

template <typename TParser>
void PuzzleTV::CallApiFunction(const ApiFunctionData& data, TParser parser)
{
    P8PLATFORM::CEvent event;
    std::exception_ptr ex = nullptr;
    CallApiAsync(data, parser, [&ex, &event](const ActionQueue::ActionResult& s) {
        ex = s.exception;
        event.Signal();
    });
    event.Wait();
    if(ex)
        try {
            std::rethrow_exception(ex);
        } catch (JsonParserException jex) {
            LogError("Puzzle server JSON error: %s",  jex.what());
        } catch (CurlErrorException cex) {
            if(data.attempt >= m_maxServerRetries -1)
                throw cex;
            // Probably server doesn't start yet
            // Wait and retry
            data.attempt += 1;
            XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32013), data.attempt);
            P8PLATFORM::CEvent::Sleep(4000);
           
            CallApiFunction(data, parser);
        }
}

template <typename TParser, typename TCompletion>
void PuzzleTV::CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion)
{
    
    // Build HTTP request
    string query;
    ParamList::const_iterator runner = data.params.begin();
    ParamList::const_iterator first = runner;
    ParamList::const_iterator end = data.params.end();
    
    for (; runner != end; ++runner)
    {
        query += runner == first ? "?" : "&";
        query += runner->first + '=' + runner->second;
    }
    std::string strRequest = string("http://") + m_serverUri + ":";
    strRequest += n_to_string(data.port);
    strRequest += data.name + query;

    CallApiAsync(strRequest, data.name, parser, completion);
}

template <typename TParser, typename TCompletion>
void PuzzleTV::CallApiAsync(const std::string& strRequest, const std::string& name, TParser parser, TCompletion completion)
{
    auto start = P8PLATFORM::GetTimeMs();

    LogDebug("Calling '%s'.",  name.c_str());

    auto pThis = this;
    
    std::function<void(const std::string&)> parserWrapper = [pThis, start, parser, name](const std::string& response) {
        LogDebug("Response for %s in %d ms.", name.c_str(),  P8PLATFORM::GetTimeMs() - start);
        
        //            if(data.name.compare( "get_url") == 0)
        //                LogDebug(response.substr(0, 16380).c_str());
        
//        LogDebug("Response length %d", response.size());
//        int pos = 0;
//        while(pos < response.size() ) {
//            LogDebug("%s", response.substr(pos,  16383).c_str());
//            pos +=  16383;
//        }

        pThis->ParseJson(response, [&parser] (Document& jsonRoot)
                  {
                      //if (!jsonRoot.HasMember("error"))
                      {
                          parser(jsonRoot);
                          return;
                      }
//                      const Value & errObj = jsonRoot["error"];
//                      auto err = errObj["message"].GetString();
//                      auto code = errObj["code"].GetInt();
//                      LogError("Puzzle TV server responses error:");
//                      LogError(err);
//                      throw ServerErrorException(err,code);
                  });
    };

    m_httpEngine->CallApiAsync(strRequest, parserWrapper,  [completion](const ActionQueue::ActionResult& ss){completion(ss);});
}

bool PuzzleTV::CheckAceEngineRunning(const char* aceServerUrlBase)
{
    static time_t last_check = 0;
    if (m_isAceRunning || difftime(time(NULL), last_check) < 60) {
        // Checke ace engine one time per minute
        return m_isAceRunning;
    }
    
    //http://127.0.0.1:6878/webui/api/service?method=get_version&format=jsonp
    bool isRunning = false;
    P8PLATFORM::CEvent event;
    try{
        string strRequest = string("http://") + aceServerUrlBase + ":6878/webui/api/service?method=get_version&format=jsonp&callback=mycallback";
        m_httpEngine->CallApiAsync(strRequest,[&isRunning ] (const std::string& response)
                                   {
                                       LogDebug("Ace Engine version: %s", response.c_str());
                                       isRunning = response.find("version") != string::npos;
                                   }, [&isRunning, &event](const ActionQueue::ActionResult& s) {
                                       if(s.status != ActionQueue::kActionCompleted)
                                           isRunning = false;
                                       event.Signal();
                                   } , HttpEngine::RequestPriority_Hi);
        
    }catch (std::exception ex) {
        LogError("Puzzle TV:  CheckAceEngineRunning() STD exception: %s",  ex.what());
    }catch (...) {
        LogError("Puzzle TV:  CheckAceEngineRunning() unknown exception.");
    }
    event.Wait();
    
    last_check = time(NULL);
    
    return m_isAceRunning = isRunning;
}


void PuzzleTV::UpdateArhivesAsync()
{
    return;
    
    const auto& channelList = m_channelList;
    std::set<ChannelId>* cahnnelsWithArchive = new std::set<ChannelId>();
    auto pThis = this;
    ApiFunctionData data("/arh/json", m_serverPort);
    CallApiAsync(data ,
                 [channelList, cahnnelsWithArchive] (Document& jsonRoot) {
                     for (const auto& ch : jsonRoot["channels"].GetArray()) {
                         const auto& chName = ch["name"].GetString();
                         const auto& chIdStr = ch["id"].GetString();
//                         LogDebug("Channels with arhive:");
//                         LogDebug("\t name = %s", chName);
//                         LogDebug("\t id =  %s", chIdStr);
                         
                         char* dummy;
                         ChannelId id  = strtoul(chIdStr, &dummy, 16);
                         try {
                             Channel ch(channelList.at(id));
                             cahnnelsWithArchive->insert(ch.Id);
//                             ch.HasArchive = true;
//                             pThis->AddChannel(ch);
                         } catch (std::out_of_range) {
                             LogError("Unknown archive channel %s (%d)", chName, id);
                         }
                     }
                 },
                 [pThis, cahnnelsWithArchive](const ActionQueue::ActionResult& s) {

//                     if(s.status != CActionQueue::kActionCompleted) {
//                         return;
//                     }
//                     for(auto chId : *cahnnelsWithArchive) {
//                         string cmd = "/arh/channel/";
//                         cmd += n_to_string_hex(chId);
//                         ApiFunctionData data(cmd.c_str(), m_serverPort);
//                         pThis->CallApiAsync(data ,
//                                      [pThis] (Document& jsonRoot){
//                                          for (const auto& ch : jsonRoot["channels"].GetArray()) {
//                                              const auto& epgName = ch["name"].GetString();
//                                              string epgTimeStr = string(epgName).substr(5);
//                                              const auto& chIdStr = ch["id"].GetString();
//                                               LogDebug("EPG with arhive:");
//                                               LogDebug("\t name = %s", epgName);
//                                              LogDebug("\t id =  %s", chIdStr);
//                                              LogDebug("\t time =  %s", epgTimeStr.c_str());
//                                          }
//                                      },
//                                      [pThis, cahnnelsWithArchive](const CActionQueue::ActionResult& ss) {
//                                      });
//
//                     }
                     delete cahnnelsWithArchive;
                 });
    
}

