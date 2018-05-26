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
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/timeutils.h"
#include "p8-platform/util/util.h"
#include "p8-platform/util/StringUtils.h"
#include "helpers.h"
#include "puzzle_tv.h"
#include "HttpEngine.hpp"
#include "XMLTV_loader.hpp"


using namespace std;
using namespace ADDON;
using namespace rapidjson;
using namespace PuzzleEngine;
using namespace PvrClient;

static const int secondsPerHour = 60 * 60;
static const char* c_EpgCacheFile = "puzzle_epg_cache.txt";

static void BeutifyUrl(string& url);
struct PuzzleTV::ApiFunctionData
{
     ApiFunctionData(const char* _name, const ParamList& _params = s_EmptyParams)
    : name(_name) , params(_params), attempt(0)
    {}
    std::string name;
    const ParamList params;
    static const  ParamList s_EmptyParams;
    mutable int attempt;
};

const ParamList PuzzleTV::ApiFunctionData::s_EmptyParams;

PuzzleTV::PuzzleTV(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper) :
    ClientCoreBase(addonHelper, pvrHelper),
    m_lastEpgRequestStartTime(0),
    m_lastEpgRequestEndTime(0),
    m_lastUniqueBroadcastId(0),
    m_epgUrl("http://api.torrent-tv.ru/ttv.xmltv.xml.gz")
{
    m_httpEngine = new HttpEngine(m_addonHelper);
    
    BuildChannelAndGroupList();
    LoadEpgCache(c_EpgCacheFile);
    LoadEpg();
    OnEpgUpdateDone();
}

PuzzleTV::~PuzzleTV()
{
    Cleanup();
}
void PuzzleTV::Cleanup()
{
    m_addonHelper->Log(LOG_NOTICE, "PuzzleTV stopping...");

    if(m_httpEngine)
        SAFE_DELETE(m_httpEngine);
    
    m_addonHelper->Log(LOG_NOTICE, "PuzzleTV stopped.");
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
        ApiFunctionData params("json");
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
                channel.IsRadio = false ;//(*itChannel)["is_video"].GetInt() == 0;//channel.Id > 1000;
                channel.Urls.push_back((*itChannel)["url"].GetString());
                std::string groupName = (*itChannel)["group"].GetString();
                plistContent[channel.Name] = PlaylistContent::mapped_type(channel,groupName);
            }
        });
        
        using namespace XMLTV;
        
        // Update channels ID from EPG
        ChannelCallback onNewChannel = [&plistContent](const EpgChannel& newChannel){
            if(plistContent.count(newChannel.strName) != 0) {
                auto& plistChannel = plistContent[newChannel.strName].first;
                plistChannel.Id = stoul(newChannel.strId.c_str());
                if(plistChannel.IconPath.empty())
                    plistChannel.IconPath = newChannel.strIcon;
            }
        };
        
        XMLTV::ParseChannels(m_epgUrl, onNewChannel, m_addonHelper);

        
        for(const auto& channelWithGroup : plistContent)
        {
            const auto& channel = channelWithGroup.second.first;
            const auto& groupName = channelWithGroup.second.second;
            
            AddChannel(channel);
            
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
       
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32006), ex.reason.c_str() );
    } catch (...) {
        LogError(">>>>  FAILED to build channel list <<<<<");
    }
    

}

std::string PuzzleTV::GetArchive(    ChannelId channelId, time_t startTime)
{
    string url;
    ParamList params;
    params["cid"] = n_to_string(channelId);
    params["time"] = n_to_string(startTime);
     try {
         ApiFunctionData apiParams("archive_next", params);
         CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value & archive = jsonRoot["archive"];
            
            url = archive["url"].GetString();
            BeutifyUrl(url);
            //Log((string(" >>>>  URL: ") + url +  "<<<<<").c_str());
        });
     } catch (ServerErrorException& ex) {
         m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32006), ex.reason.c_str() );
     } catch (...) {
         LogError(" >>>>  FAILED receive archive <<<<<");
    }
    return url;
}

void PuzzleTV::GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries)
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

    
    if(needMore ) {
        UpdateEpgForAllChannels(channelId, moreStartTime, endTime);
    }
    OnEpgUpdateDone();

}

bool PuzzleTV::AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
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

void PuzzleTV::UpdateHasArchive(PvrClient::EpgEntry& entry)
{
    entry.HasArchive = false;
}

void PuzzleTV::UpdateEpgForAllChannels(ChannelId channelId, time_t startTime, time_t endTime)
{
    if(m_epgUpdateInterval.IsSet() && m_epgUpdateInterval.TimeLeft() > 0)
        return;
    
    m_epgUpdateInterval.Init(24*60*60*1000);
    
    using namespace XMLTV;
    try {
        auto pThis = this;
        
        bool shouldUpdateEpg = false;
        EpgEntryCallback onEpgEntry = [&pThis, &shouldUpdateEpg, channelId,  startTime] (const XMLTV::EpgEntry& newEntry) {
            if(pThis->AddEpgEntry(newEntry))
                shouldUpdateEpg = shouldUpdateEpg || (newEntry.iChannelId == channelId && newEntry.startTime >= startTime);
        };
        
        XMLTV::ParseEpg(m_epgUrl, onEpgEntry, m_addonHelper);
        
        if(shouldUpdateEpg)
            m_pvrHelper->TriggerEpgUpdate(channelId);

        SaveEpgCache(c_EpgCacheFile);
        //        } catch (ServerErrorException& ex) {
        //            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32002), ex.reason.c_str() );
    } catch (...) {
        LogError(" >>>>  FAILED receive EPG <<<<<");
    }
}

void PuzzleTV::LoadEpg()
{
    using namespace XMLTV;
    auto pThis = this;
    
    EpgEntryCallback onEpgEntry = [&pThis] (const XMLTV::EpgEntry& newEntry) {pThis->AddEpgEntry(newEntry);};
    
    XMLTV::ParseEpg(m_epgUrl, onEpgEntry, m_addonHelper);
}

string PuzzleTV::GetNextStream(ChannelId channelId, int currentChannelIdx)
{
    auto& channelList = GetChannelList();
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
    auto& channelList = GetChannelList();
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
                string cmd = "streams/";
                cmd += n_to_string_hex(channelId);
                
                ApiFunctionData apiParams(cmd.c_str());
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
                m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32006), ex.reason.c_str() );
            } catch (...) {
               LogError(" >>>>  FAILED to get URL for channel ID=%d <<<<<", channelId);
           }
        
    }
    AddChannel(ch);
    return ch.Urls[0];
}

void BeutifyUrl(string& url)
{
//    url.replace(0, 7, "http");  // replace http/ts with http
//    url = url.substr(0, url.find(" ")); // trim VLC params at the end
}


template <typename TParser>
void PuzzleTV::CallApiFunction(const ApiFunctionData& data, TParser parser)
{
    P8PLATFORM::CEvent event;
    std::exception_ptr ex = nullptr;
    CallApiAsync(data, parser, [&](const CActionQueue::ActionResult& s) {
        ex = s.exception;
        event.Signal();
    });
    event.Wait();
    if(ex)
        try {
            std::rethrow_exception(ex);
        } catch (JsonParserException jex) {
            LogError("Puzzle server JSON error: %s",  jex.what());
            if(data.attempt > 2)
                throw jex;
            // Probably server doesn'r start yet
            // Wait and retry
            data.attempt += 1;
            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32013), data.attempt);
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
    strRequest += n_to_string(m_serverPort);
    strRequest +="/get/";
    strRequest += data.name + query;
    auto start = P8PLATFORM::GetTimeMs();

    LogDebug("Calling '%s'.",  data.name.c_str());

    std::function<void(const std::string&)> parserWrapper = [=](const std::string& response) {
        LogDebug("Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);
        
        //            if(data.name.compare( "get_url") == 0)
        //                LogDebug(response.substr(0, 16380).c_str());
        
        ParseJson(response, [&] (Document& jsonRoot)
                  {
                      //if (!jsonRoot.HasMember("error"))
                      {
                          parser(jsonRoot);
                          return;
                      }
                      const Value & errObj = jsonRoot["error"];
                      auto err = errObj["message"].GetString();
                      auto code = errObj["code"].GetInt();
                      m_addonHelper->Log(LOG_ERROR, "Puzzle TV server responses error:");
                      m_addonHelper->Log(LOG_ERROR, err);
                      throw ServerErrorException(err,code);
                  });
    };

    m_httpEngine->CallApiAsync(strRequest, parserWrapper,  [=](const CActionQueue::ActionResult& ss){completion(ss);});
}



void PuzzleTV::ResetArchiveList()
{
    m_archiveList.clear();
}


void PuzzleTV::LoadArchiveList()
{
    struct ChannelArchive
    {
        int ChannelId;
        int ArchiveHours;
    };
    

    // Load list of channels with archive
    ResetArchiveList();
    try {
        
        ApiFunctionData apiParams("archive_channels_list", ApiFunctionData::s_EmptyParams);

        std::vector<ChannelArchive> archives;
        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &jsonList = jsonRoot["have_archive_list"];
            if(!jsonList.IsArray())
                throw  JsonParserException("'have_archive_list list' is not array");
            std::for_each(jsonList.Begin(), jsonList.End(), [&]  (const Value & i) mutable
                          {
                              ChannelArchive arch = {
                                  stoi(i["id"].GetString()),
                                  stoi(i["archive_hours"].GetString())
                              };
                              // Remeber only valid channels
                              if(GetChannelList().count(arch.ChannelId) != 0)
                                  archives.push_back(arch);
                          });
            return true;
        });
        m_addonHelper->Log(LOG_DEBUG,"Received %d channels with archive.", archives.size());
        time_t now = time(nullptr);
        std::for_each(archives.begin(), archives.end(), [&]  (std::vector<ChannelArchive>::value_type & i) {
          BuildRecordingsFor(i.ChannelId, now - i.ArchiveHours * 60 * 60, now);
        });

    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32006), ex.reason.c_str() );
    } catch (std::exception ex) {
        m_addonHelper->Log(LOG_ERROR, "Failed to load archive list. Reason: %s", ex.what());
    }
    catch (...){
        m_addonHelper->Log(LOG_ERROR, "Failed to load archive list. Reason unknown");
    }
}

void PuzzleTV::BuildRecordingsFor(ChannelId channelId, time_t from, time_t to)
{
    EpgEntryList epgEntries;
    GetEpg(channelId, from, to, epgEntries);
    m_addonHelper->Log(LOG_DEBUG,"Building archives for channel %d (%d hours).", channelId, (to - from)/(60*60));
    
     int cnt = 0;
    for(const auto & i : epgEntries)
    {
         const EpgEntry& epgTag = i.second;
         if(epgTag.ChannelId == channelId
            && epgTag.StartTime >= from
            && epgTag.StartTime < to
            && m_archiveList.count(i.first) == 0)
         {
             ++cnt;
             m_archiveList.insert(i.first);
             
         }
     }
    //LogDebug("Found %d EPG records for %d .", cnt, channelId);

}
void PuzzleTV::Apply(std::function<void(const ArchiveList&)>& action ) const {
    action(m_archiveList);
    return;
}






