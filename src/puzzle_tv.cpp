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
#include <sstream>
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

using namespace Globals;
using namespace std;
using namespace ADDON;
using namespace rapidjson;
using namespace PuzzleEngine;
using namespace PvrClient;

static const int secondsPerHour = 60 * 60;
static const char* c_EpgCacheFile = "puzzle_epg_cache.txt";

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

PuzzleTV::PuzzleTV(const char* serverUrl, uint16_t serverPort) :
    m_serverUri(serverUrl),
    m_serverPort(serverPort),
    m_epgServerPort(8085),
    m_epgUrl("http://api.torrent-tv.ru/ttv.xmltv.xml.gz")
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

        // Get channels from server
        ApiFunctionData params("/get/json", m_serverPort);
        CallApiFunction(params, [&plistContent] (Document& jsonRoot)
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
            ChannelCallback onNewChannel = [&plistContent, &pThis](const EpgChannel& newChannel){
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
        XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32006), ex.reason.c_str() );
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
//         XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32006), ex.reason.c_str() );
//     } catch (...) {
//         LogError(" >>>>  FAILED receive archive <<<<<");
//    }
//    return url;
//}

bool PuzzleTV::AddXmlEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
{
    unsigned int id = xmlEpgEntry.startTime;

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
    const time_t archivePeriod = 3 * 24 * 60 * 60; //3 days in secs
    time_t from = now - archivePeriod;
    entry.HasArchive = entry.StartTime > from && entry.StartTime < now;


}

void PuzzleTV::UpdateEpgForAllChannels(time_t startTime, time_t endTime)
{
    if(m_epgUpdateInterval.IsSet() && m_epgUpdateInterval.TimeLeft() > 0)
        return;
    
    m_epgUpdateInterval.Init(24*60*60*1000);
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
        
        XMLTV::EpgEntryCallback onEpgEntry = [&pThis] (const XMLTV::EpgEntry& newEntry) {pThis->AddXmlEpgEntry(newEntry);};
        
        XMLTV::ParseEpg(m_epgUrl, onEpgEntry);
    } else if(m_epgType == c_EpgType_Server) {
        
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
                                epgEntry.IconPath = epgItem.value["img"].GetString();
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
    }else {
        LogError("PuzzleTV: unknown EPG source type %d", m_epgType);
    }
}

string PuzzleTV::GetNextStream(ChannelId channelId, int currentChannelIdx)
{
    auto& channelList = m_channelList;
    if(channelList.count( channelId ) != 1) {
        LogError(" >>>>   PuzzleTV::GetNextStream: Unknown channel ID= %d <<<<<", channelId);
        return string();
    }
    auto& urls = channelList.at(channelId).Urls;
    if(urls.size() > currentChannelIdx + 1)
        return urls[currentChannelIdx + 1];
    return string();

}
string PuzzleTV::GetUrl(ChannelId channelId)
{
    auto& channelList = m_channelList;
    if(channelList.count( channelId ) != 1) {
        LogError(" >>>>   PuzzleTV::GetNextStream: Unknown channel ID= %d <<<<<", channelId);
        return string();
    }
    Channel ch = channelList.at(channelId);
    auto& urls = ch.Urls;
    if(urls.size() <2)
    {
        urls.clear();
            try {
                string cmd = "/get/streams/";
                cmd += n_to_string_hex(channelId);
                
                ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
                CallApiFunction(apiParams, [&] (Document& jsonRoot)
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
            } catch (ServerErrorException& ex) {
                XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32006), ex.reason.c_str() );
            } catch (...) {
               LogError(" >>>>  FAILED to get URL for channel ID=%d <<<<<", channelId);
           }
        
    }
    AddChannel(ch);
    return ch.Urls[0];
}

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
            XBMC->QueueNotification(QUEUE_INFO, XBMC->GetLocalizedString(32013), data.attempt);
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
    auto start = P8PLATFORM::GetTimeMs();

    LogDebug("Calling '%s'.",  data.name.c_str());

    auto pThis = this;
    std::function<void(const std::string&)> parserWrapper = [pThis, start, parser](const std::string& response) {
        LogDebug("Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);
        
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

void PuzzleTV::UpdateArhivesAsync()
{
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

