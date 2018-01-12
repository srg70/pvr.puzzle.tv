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
#include "helpers.h"
#include "puzzle_tv.h"
#include "HttpEngine.hpp"

using namespace std;
using namespace ADDON;
using namespace rapidjson;
using namespace PuzzleEngine;
using namespace PvrClient;

static const int secondsPerHour = 60 * 60;

//static const char c_EpgCacheDelimeter = '\n';
//
//static const char* c_EpgCacheDirPath = "special://temp/pvr.sovok.tv";
//
//static const char* c_EpgCacheFilePath = "special://temp/pvr.sovok.tv/EpgCache.txt";

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

class PuzzleTV::HelperThread : public P8PLATFORM::CThread
{
public:
    HelperThread(PuzzleTV* PuzzleTV, std::function<void(void)> action)
    : m_PuzzleTV(PuzzleTV), m_action(action)
    , m_epgActivityCounter(0)
//    , m_stopEvent(false) /*Manual event*/
    {}
    void EpgActivityStarted() {++m_epgActivityCounter;}
    void EpgActivityDone() {}
    void* Process()
    {
        do
        {
            unsigned int oldEpgActivity = m_epgActivityCounter;
            m_PuzzleTV->LoadArchiveList();
            
            // Wait for epg done before announce archives
            bool isStopped = IsStopped();
            while(!isStopped && (oldEpgActivity != m_epgActivityCounter)){
                oldEpgActivity = m_epgActivityCounter;
                isStopped = IsStopped(3000);// 2sec
            }
            if(isStopped)
                break;
            
            HelperThread* pThis = this;
            m_PuzzleTV->m_httpEngine->RunOnCompletionQueueAsync([pThis]() {
                pThis->m_action();
            },  [](const CActionQueue::ActionResult& s) {});

        }while (!IsStopped(10*60*1000));//10 min

        return NULL;
        
    }
//    bool StopThread(int iWaitMs = 5000)
//    {
//        m_stopEvent.Broadcast();
//        return this->P8PLATFORM::CThread::StopThread(iWaitMs);
//    }
private:
    
    bool IsStopped(uint32_t timeoutInSec = 0) {
        P8PLATFORM::CTimeout timeout(timeoutInSec * 1000);
        bool isStoppedOrTimeout = P8PLATFORM::CThread::IsStopped() || timeout.TimeLeft() == 0;
        while(!isStoppedOrTimeout) {
            isStoppedOrTimeout = P8PLATFORM::CThread::IsStopped() || timeout.TimeLeft() == 0;
            Sleep(1000);//1sec
        }
        return P8PLATFORM::CThread::IsStopped();
    }

    PuzzleTV* m_PuzzleTV;
    std::function<void(void)> m_action;
    unsigned int m_epgActivityCounter;
//    P8PLATFORM::CEvent m_stopEvent;
};


const ParamList PuzzleTV::ApiFunctionData::s_EmptyParams;

PuzzleTV::PuzzleTV(ADDON::CHelper_libXBMC_addon *addonHelper) :
    m_addonHelper(addonHelper),
    m_lastEpgRequestStartTime(0),
    m_lastEpgRequestEndTime(0),
    m_lastUniqueBroadcastId(0),
    m_archiveLoader(NULL)
{
    m_httpEngine = new HttpEngine(m_addonHelper);
    
    LoadEpgCache();
}

PuzzleTV::~PuzzleTV()
{
    Cleanup();
}
void PuzzleTV::Cleanup()
{
    m_addonHelper->Log(LOG_NOTICE, "PuzzleTV stopping...");

    if(m_archiveLoader) {
        m_archiveLoader->StopThread();
    }
    //Logout();
    if(m_httpEngine)
        SAFE_DELETE(m_httpEngine);
    
    if(m_archiveLoader)
        SAFE_DELETE(m_archiveLoader);

    m_addonHelper->Log(LOG_NOTICE, "PuzzleTV stopped.");
}

void PuzzleTV::SaveEpgCache()
{
}
void PuzzleTV::LoadEpgCache()
{
   
}

bool PuzzleTV::StartArchivePollingWithCompletion(std::function<void(void)> action)
{
    if(m_archiveLoader)
        return false;
    
    m_archiveLoader = new PuzzleTV::HelperThread(this, action);
    m_archiveLoader->CreateThread(false);
    return true;
}

ChannelList &PuzzleTV::GetMutableChannelList()
{
    if (m_channelList.empty())
    {
        BuildChannelAndGroupList();
    }
    
    return m_channelList;

}

const ChannelList &PuzzleTV::GetChannelList()
{
    return GetMutableChannelList();
}

const EpgEntryList& PuzzleTV::GetEpgList() const
{
    return  m_epgEntries;
}

template <typename TParser>
void PuzzleTV::ParseJson(const std::string& response, TParser parser)
{
    Document jsonRoot;
    jsonRoot.Parse(response.c_str());
    if(jsonRoot.HasParseError()) {
        
        ParseErrorCode error = jsonRoot.GetParseError();
        auto strError = string("Rapid JSON parse error: ");
        strError += GetParseError_En(error);
        strError += " (" ;
        strError += n_to_string(error);
        strError += ").";
        Log(strError.c_str());
        throw JsonParserException(strError);
    }
    parser(jsonRoot);
    return;

}

void PuzzleTV::BuildChannelAndGroupList()
{
    m_channelList.clear();
    m_groupList.clear();

    try {

        ApiFunctionData params("json");
        CallApiFunction(params, [&] (Document& jsonRoot)
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
                m_channelList[channel.Id] = channel;

                std::string groupName = (*itChannel)["group"].GetString();
                auto itGroup =  std::find_if(m_groupList.begin(), m_groupList.end(), [&](const GroupList::value_type& v ){
                    return groupName ==  v.second.Name;
                });
                if (itGroup == m_groupList.end()) {
                    m_groupList[m_groupList.size()].Name = groupName;
                    itGroup = --m_groupList.end();
                }

                itGroup->second.Channels.insert(channel.Id);
            }
        });
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32006), ex.reason.c_str() );
    } catch (...) {
        Log(">>>>  FAILED to build channel list <<<<<");
    }
    

}


//bool PuzzleTV::FindEpg(unsigned int brodcastId, EpgEntry& epgEntry)
//{
//    if(m_epgEntries.count(brodcastId) == 0)
//        return false;
//    
//    epgEntry = m_epgEntries[brodcastId];
//    //Log((string(" >>>> Pogramm:") + epgEntry.Title + "<<<<<").c_str());
//    
//    //string url = GetArchiveForEpg(*result);
//    
//    return true;
//}

//std::string PuzzleTV::GetArchiveForEpg(const EpgEntry& epgEntry)
//{
//    return  GetArchive(epgEntry.ChannelId, epgEntry.StartTime + m_serverTimeShift);
//}

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
         Log(" >>>>  FAILED receive archive <<<<<");
    }
    return url;
}

void PuzzleTV::Log(const char* massage) const
{
    //char* msg = m_addonHelper->UnknownToUTF8(massage);
    m_addonHelper->Log(LOG_DEBUG, massage);
    //m_addonHelper->FreeString(msg);
    
}

void PuzzleTV::GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries)
{
     m_addonHelper->Log(LOG_DEBUG, "Get EPG for channel %d [%d - %d]", channelId, startTime, endTime);
    P8PLATFORM::CLockObject lock(m_epgAccessMutex);
    if(m_archiveLoader)
        m_archiveLoader->EpgActivityStarted();
    
    if (m_lastEpgRequestStartTime == 0)
    {
        m_lastEpgRequestStartTime = startTime;
    }
    else
    {
        startTime = max(startTime, m_lastEpgRequestEndTime);
    }
    if(endTime > startTime)
    {
        GetEpgForAllChannels(startTime, endTime, m_epgEntries);
        m_lastEpgRequestEndTime = endTime;
        SaveEpgCache();
    }

    EpgEntryList::const_iterator itEpgEntry = m_epgEntries.begin();
    for (; itEpgEntry != m_epgEntries.end(); ++itEpgEntry)
    {
        if (itEpgEntry->second.channelId == channelId)
            epgEntries.insert(*itEpgEntry);
    }
    if(m_archiveLoader)
        m_archiveLoader->EpgActivityDone();

}


void PuzzleTV::GetEpgForAllChannels(time_t startTime, time_t endTime, EpgEntryList& epgEntries)
{

    int64_t totalNumberOfHours = (endTime - startTime) / secondsPerHour;
    int64_t hoursRemaining = totalNumberOfHours;
    const int64_t hours24 = 24;
    
    while (hoursRemaining > 0)
    {
        // Query EPG for max 24 hours per single request.
        int64_t requestNumberOfHours = min(hours24, hoursRemaining);

        //EpgEntryList epgEntries24Hours;
        GetEpgForAllChannelsForNHours(startTime, requestNumberOfHours, [&](EpgEntryList::key_type key, const EpgEntryList::mapped_type& val) {epgEntries[key] = val; });
        
        hoursRemaining -= requestNumberOfHours;
        startTime += requestNumberOfHours * secondsPerHour;
    }

}

template<class TFunc>
void PuzzleTV::GetEpgForAllChannelsForNHours(time_t startTime, short numberOfHours, TFunc func)
{
    // For queries over 24 hours Puzzle.TV returns incomplete results.
    assert(numberOfHours > 0 && numberOfHours <= 24);

    ParamList params;
    params["dtime"] = n_to_string(startTime + m_serverTimeShift);
    params["period"] = n_to_string(numberOfHours);
    try {
        ApiFunctionData apiParams("epg3", params);

        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &channels = jsonRoot["epg3"];
            Value::ConstValueIterator itChannel = channels.Begin();
            for (; itChannel != channels.End(); ++itChannel)
            {
                const Value& jsonChannelEpg = (*itChannel)["epg"];
                Value::ConstValueIterator itJsonEpgEntry1 = jsonChannelEpg.Begin();
                Value::ConstValueIterator itJsonEpgEntry2  = itJsonEpgEntry1;
                itJsonEpgEntry2++;
                for (; itJsonEpgEntry2 != jsonChannelEpg.End(); ++itJsonEpgEntry1, ++itJsonEpgEntry2)
                {
                    EpgEntry epgEntry;
                    epgEntry.channelId = stoul((*itChannel)["id"].GetString());
                    epgEntry.Title = (*itJsonEpgEntry1)["progname"].GetString();
                    epgEntry.Description = (*itJsonEpgEntry1)["description"].GetString();
                    epgEntry.StartTime = stol((*itJsonEpgEntry1)["ut_start"].GetString()) - m_serverTimeShift;
                    epgEntry.EndTime = stol((*itJsonEpgEntry2)["ut_start"].GetString()) - m_serverTimeShift;
                    
                    unsigned int id = epgEntry.StartTime;
                    while( m_epgEntries.count(id) != 0)
                        ++id;
                    func(id, epgEntry);
                }
            }
        });
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32006), ex.reason.c_str() );
    } catch (...) {
        Log(" >>>>  FAILED receive EPG for N hours<<<<<");
    }
}

const GroupList &PuzzleTV::GetGroupList()
{
    if (m_groupList.empty())
        BuildChannelAndGroupList();

    return m_groupList;
}

string PuzzleTV::GetNextStream(ChannelId channelId, int currentChannelIdx)
{
    auto& channelList = GetChannelList();
    if(channelList.count( channelId ) != 1) {
        Log((string(" >>>>   PuzzleTV::GetNextStream: Unknown channel ID=" ) + n_to_string(channelId) + " <<<<<") .c_str());
        return string();
    }
    auto& urls = channelList.at(channelId).Urls;
    if(urls.size() > currentChannelIdx + 1)
        return urls[currentChannelIdx + 1];
    return string();

}
string PuzzleTV::GetUrl(ChannelId channelId)
{
    auto& channelList = GetMutableChannelList();
    if(channelList.count( channelId ) != 1) {
        Log((string(" >>>>   PuzzleTV::GetUrl: Unknown channel ID=" ) + n_to_string(channelId) + " <<<<<") .c_str());
        return string();
    }
    auto& urls = channelList.at(channelId).Urls;
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
                                    Log((string(" >>>>  URL: ") + url +  "<<<<<").c_str());

                                });
               });
            } catch (ServerErrorException& ex) {
                m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32006), ex.reason.c_str() );
            } catch (...) {
               Log((string(" >>>>  FAILED to get URL for channel ID=" ) + n_to_string(channelId) + " <<<<<") .c_str());
           }
        
    }
    return channelList[channelId].Urls[0];
//    string url;
//
//    ParamList params;
//    params["cid"] = n_to_string_hex(channelId);
//    try {
//        ApiFunctionData apiParams("streams", params);
//        CallApiFunction(apiParams, [&] (Document& jsonRoot)
//       {
//            url = jsonRoot["url"].GetString();
//            BeutifyUrl(url);
//       });
//    } catch (ServerErrorException& ex) {
//        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32006), ex.reason.c_str() );
//    } catch (...) {
//       Log((string(" >>>>  FAILED to get URL for channel ID=" ) + n_to_string(channelId) + " <<<<<") .c_str());
//   }
//
//    return url;
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
            if(data.attempt > 0)
                throw jex;
            // Probably server doesn'r start yet
            // Wait and retry
            P8PLATFORM::CEvent::Sleep(2000);
           
             data.attempt += 1;
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

    m_addonHelper->Log(LOG_DEBUG, "Calling '%s'.",  data.name.c_str());

    std::function<void(const std::string&)> parserWrapper = [=](const std::string& response) {
        m_addonHelper->Log(LOG_DEBUG, "Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);
        
        //            if(data.name.compare( "get_url") == 0)
        //                m_addonHelper->Log(LOG_DEBUG, response.substr(0, 16380).c_str());
        
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
                              if(m_channelList.count(arch.ChannelId) != 0)
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
         if(epgTag.channelId == channelId
            && epgTag.StartTime >= from
            && epgTag.StartTime < to
            && m_archiveList.count(i.first) == 0)
         {
             ++cnt;
             m_archiveList.insert(i.first);
             
         }
     }
    //m_addonHelper->Log(LOG_DEBUG, "Found %d EPG records for %d .", cnt, channelId);

}
void PuzzleTV::Apply(std::function<void(const ArchiveList&)>& action ) const {
    action(m_archiveList);
    return;
}






