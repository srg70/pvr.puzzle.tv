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

using namespace std;
using namespace ADDON;
using namespace rapidjson;

static const int secondsPerHour = 60 * 60;

const bool ASYNC_API = true;
const bool SYNC_API = true;

//
static const char* c_EpgCacheDirPath = "special://temp/pvr-puzzle-tv";

static const char* c_EpgCacheFilePath = "special://temp/pvr-puzzle-tv/ott_epg_cache.txt";

static void BeutifyUrl(string& url);
struct OttPlayer::ApiFunctionData
{
//    ApiFunctionData(const char* _name)
//    : name(_name) , params(s_EmptyParams), api_ver (API_2_2)
//    {}
//    
    ApiFunctionData(const char* _name, const ParamList& _params = s_EmptyParams, API_Version _version = API_2_2)
    : name(_name) , params(_params), api_ver (_version)
    {}
    std::string name;
    const ParamList params;
    API_Version api_ver;
    static const  ParamList s_EmptyParams;
};

class OttPlayer::HelperThread : public P8PLATFORM::CThread
{
public:
    HelperThread(OttPlayer* player, std::function<void(void)> action)
    : m_player(player), m_action(action)
    , m_epgActivityCounter(0), m_stopEvent(false) /*Manual event*/
    {}
    void EpgActivityStarted() {++m_epgActivityCounter;}
    void EpgActivityDone() {}
    void* Process()
    {
        do
        {
            unsigned int oldEpgActivity = m_epgActivityCounter;
            m_player->LoadArchiveList();
            
            // Wait for epg done before announce archives
            bool isStopped = IsStopped();
            while(!isStopped && (oldEpgActivity != m_epgActivityCounter)){
                oldEpgActivity = m_epgActivityCounter;
                isStopped = m_stopEvent.Wait(2000);// 2sec
            }
            if(isStopped)
                break;
            
            HelperThread* pThis = this;
            m_player->m_httpEngine->RunOnCompletionQueueAsync([pThis]() {
                pThis->m_action();
            },  [](const CActionQueue::ActionResult& s) {});
            
        }while (!IsStopped() && !m_stopEvent.Wait(10*60*1000)); //10 min

        return NULL;
        
    }
    bool StopThread(int iWaitMs = 5000)
    {
        m_stopEvent.Broadcast();
        return this->P8PLATFORM::CThread::StopThread(iWaitMs);
    }
private:
    OttPlayer* m_player;
    std::function<void(void)> m_action;
    unsigned int m_epgActivityCounter;
    P8PLATFORM::CEvent m_stopEvent;
};


const ParamList OttPlayer::ApiFunctionData::s_EmptyParams;

OttPlayer::OttPlayer(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &playlistUrl, const std::string &key) :
    m_addonHelper(addonHelper),
    m_playlistUrl(playlistUrl),
    m_key(key),
    m_lastEpgRequestStartTime(0),
    m_lastEpgRequestEndTime(0),
    m_archiveLoader(NULL)
{
    m_httpEngine = new HttpEngine(m_addonHelper);
    
//    if (!Login(true)) {
//        Cleanup();
//        throw AuthFailedException();
//    }
    LoadSettings();
    LoadEpgCache();
}

OttPlayer::~OttPlayer()
{
    Cleanup();
}
void OttPlayer::Cleanup()
{
    m_addonHelper->Log(LOG_NOTICE, "OttPlayer stopping...");

    if(m_archiveLoader) {
        m_archiveLoader->StopThread();
        SAFE_DELETE(m_archiveLoader);
    }
    if(m_httpEngine)
        SAFE_DELETE(m_httpEngine);
    
    m_addonHelper->Log(LOG_NOTICE, "OttPlayer stopped.");
}

template< typename ContainerT, typename PredicateT >
void erase_if( ContainerT& items, const PredicateT& predicate ) {
    for( auto it = items.begin(); it != items.end(); ) {
        if( predicate(*it) ) it = items.erase(it);
        else ++it;
    }
};
void OttPlayer::SaveEpgCache()
{
    // Leave epg entries not older then 2 weeks from now
    time_t now = time(nullptr);
    auto oldest = m_lastEpgRequestStartTime = max(m_lastEpgRequestStartTime, now - 14*24*60*60);
    erase_if(m_epgEntries,  [oldest] (const EpgEntryList::value_type& i)
             {
                 return i.second.StartTime < oldest;
             });

    StringBuffer s;
    Writer<StringBuffer> writer(s);
    
    writer.StartObject();               // Between StartObject()/EndObject(),
    writer.Key("m_lastEpgRequestStartTime");
    writer.Int64(m_lastEpgRequestStartTime);
    writer.Key("m_lastEpgRequestEndTime");
    writer.Int64(m_lastEpgRequestEndTime);
    
    writer.Key("m_epgEntries");
    writer.StartArray();                // Between StartArray()/EndArray(),
    for_each(m_epgEntries.begin(), m_epgEntries.end(),[&](const EpgEntryList::value_type& i) {
        writer.StartObject();               // Between StartObject()/EndObject(),
        writer.Key("k");
        writer.Int64(i.first);
        writer.Key("v");
        i.second.Serialize(writer);
        writer.EndObject();
    });
    writer.EndArray();
             
    writer.EndObject();

    m_addonHelper->CreateDirectory(c_EpgCacheDirPath);
    
    void* file = m_addonHelper->OpenFileForWrite(c_EpgCacheFilePath, true);
    if(NULL == file)
        return;
    auto buf = s.GetString();
    m_addonHelper->WriteFile(file, buf, s.GetSize());
    m_addonHelper->CloseFile(file);
    
}
void OttPlayer::LoadEpgCache()
{
    void* file = m_addonHelper->OpenFile(c_EpgCacheFilePath, 0);
    if(NULL == file)
        return;
    int64_t fSize = m_addonHelper->GetFileLength(file);
    
    char* rawBuf = new char[fSize + 1];
    if(0 == rawBuf)
        return;
    m_addonHelper->ReadFile(file, rawBuf, fSize);
    m_addonHelper->CloseFile(file);
    file = NULL;
    
    rawBuf[fSize] = 0;
    
    string ss(rawBuf);
    delete[] rawBuf;
    try {
        ParseJson(ss, [&] (Document& jsonRoot) {
            m_lastEpgRequestStartTime = jsonRoot["m_lastEpgRequestStartTime"].GetInt();
            m_lastEpgRequestEndTime = jsonRoot["m_lastEpgRequestEndTime"].GetInt();
            
            const Value& v = jsonRoot["m_epgEntries"];
            Value::ConstValueIterator it = v.Begin();
            for(; it != v.End(); ++it)
            {
                EpgEntryList::key_type k = (*it)["k"].GetInt64();
                EpgEntryList::mapped_type e;
                e.Deserialize((*it)["v"]);
                m_epgEntries[k] = e;
            }
        });

    } catch (...) {
        Log(" >>>>  FAILED load EPG cache <<<<<");
        m_epgEntries.clear();
        m_lastEpgRequestStartTime = m_lastEpgRequestEndTime = 0;
    }
}

bool OttPlayer::StartArchivePollingWithCompletion(std::function<void(void)> action)
{
    if(m_archiveLoader)
        return false;
    
    m_archiveLoader = new OttPlayer::HelperThread(this, action);
    m_archiveLoader->CreateThread(false);
    return true;
}

const ChannelList &OttPlayer::GetChannelList()
{
    if (m_channelList.empty())
    {
        BuildChannelAndGroupList();
    }

    return m_channelList;
}

const EpgEntryList& OttPlayer::GetEpgList() const
{
    return  m_epgEntries;
}
const StreamerNamesList& OttPlayer::GetStreamersList() const
{
    return  m_streamerNames;
}

//const ArchiveList& OttPlayer::GetArchiveList()
//{
//    if (m_archiveList.empty())
//    {
//        LoadArchiveList();
//    }
//    return  m_archiveList;
//}

template <typename TParser>
void OttPlayer::ParseJson(const std::string& response, TParser parser)
{
    Document jsonRoot;
    jsonRoot.Parse(response.c_str());
    if(jsonRoot.HasParseError()) {
        
        ParseErrorCode error = jsonRoot.GetParseError();
        auto strError = string("Rapid JSON parse error: ");
        strError += GetParseError_En(error);
        strError += " (" ;
        strError += error;
        strError += ").";
        Log(strError.c_str());
        throw JsonParserException(strError);
    }
    parser(jsonRoot);
    return;

}

void OttPlayer::BuildChannelAndGroupList()
{
    m_channelList.clear();
    m_groupList.clear();

    try {
//        ApiFunctionData params2("channel_list2");
//        CallApiFunction(params2, [&] (Document& jsonRoot){});
        
        ApiFunctionData params("channel_list");
        CallApiFunction(params, [&] (Document& jsonRoot)
        {
            const Value &groups = jsonRoot["groups"];
            Value::ConstValueIterator itGroup = groups.Begin();
            for(; itGroup != groups.End(); ++itGroup)
            {
                const Value &channels = (*itGroup)["channels"];
                Value::ConstValueIterator itChannel = channels.Begin();
                for(; itChannel != channels.End(); ++itChannel)
                {
                    OttChannel channel;
                    channel.Id = (*itChannel)["id"].GetInt();
                    channel.Name = (*itChannel)["name"].GetString();
                    channel.IconPath = (*itChannel)["icon"].GetString();
                    channel.IsRadio = (*itChannel)["is_video"].GetInt() == 0;//channel.Id > 1000;
                    m_channelList[channel.Id] = channel;

                    std::string groupName = (*itGroup)["name"].GetString();
                    SovokGroup &group = m_groupList[groupName];
                    group.Channels.insert(channel.Id);
                }
            }
        });
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
    } catch (...) {
        Log(">>>>  FAILED to build channel list <<<<<");
    }
    

}


bool OttPlayer::FindEpg(unsigned int brodcastId, OttEpgEntry& epgEntry)
{
    if(m_epgEntries.count(brodcastId) == 0)
        return false;
    
    epgEntry = m_epgEntries[brodcastId];
    //Log((string(" >>>> Pogramm:") + epgEntry.Title + "<<<<<").c_str());
    
    //string url = GetArchiveForEpg(*result);
    
    return true;
}

std::string OttPlayer::GetArchiveForEpg(const OttEpgEntry& epgEntry)
{
    return  GetArchive(epgEntry.ChannelId, epgEntry.StartTime + m_serverTimeShift);
}

std::string OttPlayer::GetArchive(OttChannelId channelId, time_t startTime)
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
         m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
     } catch (...) {
         Log(" >>>>  FAILED receive archive <<<<<");
    }
    return url;
}

void OttPlayer::Log(const char* massage) const
{
    //char* msg = m_addonHelper->UnknownToUTF8(massage);
    m_addonHelper->Log(LOG_DEBUG, massage);
    //m_addonHelper->FreeString(msg);
    
}

void OttPlayer::GetEpg(OttChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries)
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
        if (itEpgEntry->second.ChannelId == channelId)
            epgEntries.insert(*itEpgEntry);
    }
    if(m_archiveLoader)
        m_archiveLoader->EpgActivityDone();

}


void OttPlayer::GetEpgForAllChannels(time_t startTime, time_t endTime, EpgEntryList& epgEntries)
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
void OttPlayer::GetEpgForAllChannelsForNHours(time_t startTime, short numberOfHours, TFunc func)
{
    // For queries over 24 hours Sovok.TV returns incomplete results.
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
                    OttEpgEntry epgEntry;
                    epgEntry.ChannelId = strtoi((*itChannel)["id"].GetString());
                    epgEntry.Title = (*itJsonEpgEntry1)["progname"].GetString();
                    epgEntry.Description = (*itJsonEpgEntry1)["description"].GetString();
                    epgEntry.StartTime = strtoi((*itJsonEpgEntry1)["ut_start"].GetString()) - m_serverTimeShift;
                    epgEntry.EndTime = strtoi((*itJsonEpgEntry2)["ut_start"].GetString()) - m_serverTimeShift;
                    
                    unsigned int id = epgEntry.StartTime;
                    while( m_epgEntries.count(id) != 0)
                        ++id;
                    func(id, epgEntry);
                }
            }
        });
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
    } catch (...) {
        Log(" >>>>  FAILED receive EPG for N hours<<<<<");
    }
}

const GroupList &OttPlayer::GetGroupList()
{
    if (m_groupList.empty())
        BuildChannelAndGroupList();

    return m_groupList;
}

string OttPlayer::GetUrl(OttChannelId channelId)
{
    string url;

    ParamList params;
    params["cid"] = n_to_string(channelId);
    params["protect_code"] = m_pinCode.c_str();
    try {
        ApiFunctionData apiParams("get_url", params);
        CallApiFunction(apiParams, [&] (Document& jsonRoot)
       {
            url = jsonRoot["url"].GetString();
            BeutifyUrl(url);
       });
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
    } catch (...) {
       Log((string(" >>>>  FAILED to get URL for channel ID=" ) + n_to_string(channelId) + " <<<<<") .c_str());
   }

    return url;
}

void BeutifyUrl(string& url)
{
    url.replace(0, 7, "http");  // replace http/ts with http
    url = url.substr(0, url.find(" ")); // trim VLC params at the end
}

FavoriteList OttPlayer::GetFavorites()
{
    FavoriteList favorites;
    try {
        ApiFunctionData apiParams("favorites");
        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &jsonFavorites = jsonRoot["favorites"];
            Value::ConstValueIterator itFavorite = jsonFavorites.Begin();
            for(; itFavorite != jsonFavorites.End(); ++itFavorite)
                favorites.insert((*itFavorite)["channel_id"].GetInt());
        });
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
    } catch (...) {
        Log(" >>>>  FAILED to get favorites <<<<<");
    }

    return favorites;
}


bool OttPlayer::Login(bool wait)
{

    if (m_login.empty() || m_password.empty())
        return false;

    ParamList params;
    params["login"] = m_login;
    params["pass"] = m_password;

    auto parser = [=] (Document& jsonRoot)
    {
        m_httpEngine->m_sessionCookie.clear();
        
        string sid = jsonRoot["sid"].GetString();
        string sidName = jsonRoot["sid_name"].GetString();
        
        if (sid.empty() || sidName.empty())
            throw BadSessionIdException();
        m_httpEngine->m_sessionCookie[sidName] = sid;
    };
    
    if(wait) {
        try {
            ApiFunctionData apiParams("login", params);
            CallApiFunction(apiParams, parser);
        } catch (ServerErrorException& ex) {
            m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
            return false;
        } catch (...) {
            Log(" >>>>  FAILED to LOGIN!!! <<<<<");
            return false;
            
        }
        return true;
    } else {
        ApiFunctionData apiParams("login", params);
        CallApiAsync(apiParams, parser, [=] (const CActionQueue::ActionResult& s){
            if(s.exception) {
                try {
                    std::rethrow_exception(s.exception);
                } catch (ServerErrorException& ex) {
                    m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
                } catch (...) {
                    Log(" >>>>  FAILED to LOGIN!!! <<<<<");
                }
            }
            
        });
        return false;
    }
    
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
    
    for (; runner != end; ++runner)
    {
        query += runner == first ? "?" : "&";
        query += runner->first + '=' + runner->second;
    }
    std::string strRequest = "http://api.sovok.tv/";
    strRequest += "/json/";
    strRequest += data.name + query;
    auto start = P8PLATFORM::GetTimeMs();
    const bool isLoginCommand = data.name == "login";
    m_addonHelper->Log(LOG_DEBUG, "Calling '%s'.",  data.name.c_str());

    std::function<void(const std::string&)> parserWrapper = [=](const std::string& response) {
        m_addonHelper->Log(LOG_DEBUG, "Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);
        
        //            if(data.name.compare( "get_url") == 0)
        //                m_addonHelper->Log(LOG_DEBUG, response.substr(0, 16380).c_str());
        
        ParseJson(response, [&] (Document& jsonRoot)
                  {
                      if (!jsonRoot.HasMember("error"))
                      {
                          parser(jsonRoot);
                          return;
                      }
                      const Value & errObj = jsonRoot["error"];
                      auto err = errObj["message"].GetString();
                      auto code = errObj["code"].GetInt();
                      m_addonHelper->Log(LOG_ERROR, "Sovok TV server responses error:");
                      m_addonHelper->Log(LOG_ERROR, err);
                      throw ServerErrorException(err,code);
                  });
    };
    m_httpEngine->CallApiAsync(strRequest, parserWrapper, [=](const CActionQueue::ActionResult& s)
                 {
                     // Do not re-login within login command.
                     if(s.status == CActionQueue::kActionCompleted || isLoginCommand) {
                         completion(s);
                         return;
                     }
                     // In case of error try to re-login and repeat the API call.
                     Login(false);
                    m_httpEngine->CallApiAsync(strRequest, parserWrapper,  [=](const CActionQueue::ActionResult& ss){
                                     completion(ss);
                                 });
                 });
}

bool OttPlayer::LoadStreamers()
{
    try {
        ApiFunctionData apiParams("streamers");

        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            
            const Value &jsonStreamers = jsonRoot["streamers"];
            if(!jsonStreamers.IsArray())
                throw  JsonParserException("'streamers' is not array");
            m_streamerIds.clear();
            m_streamerNames.clear();
            Value::ConstValueIterator runner = jsonStreamers.Begin();
            Value::ConstValueIterator end = jsonStreamers.End();
            while(runner != end)
            {
                m_streamerNames.push_back((*runner)["name"].GetString());
                m_streamerIds.push_back((*runner)["id"].GetString());
                ++runner;
            }
            m_addonHelper->Log(LOG_DEBUG,"Loaded %d streamers.", m_streamerNames.size());
        });
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
    } catch (...) {
        Log(" >>>>  FAILED to load streamers <<<<<");
        return false;
    }
    return true;
}


void OttPlayer::LoadSettings()
{
    // Load streamers
    try {
        ApiFunctionData apiParams("settings");

        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &jsonSettings = jsonRoot["settings"];
            std::string streamerID(jsonSettings["streamer"].GetString());
           
            auto timeString = jsonSettings["timezone"].GetString();
            int hours, minutes, seconds;
            sscanf(timeString, "%d:%d:%d", &hours, &minutes, &seconds);
            m_serverTimeShift = seconds + 60 * minutes + 60*60*hours;
            
            m_streammerId = 0;
            auto streamer = std::find_if (m_streamerIds.begin(), m_streamerIds.end(), [=](StreamerIdsList::value_type i){
                if(i == streamerID)
                    return true;
                ++m_streammerId;
                return false;
            });
            if(m_streamerIds.end() == streamer)
            {
                Log(" >>>>  Unknown streamer ID <<<<<");
                throw UnknownStreamerIdException();
            }
        });
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
    } catch (...) {
        Log(" >>>>  FAILED to load settings <<<<<");
    }
}

void OttPlayer::SetStreamerId(int streamerId)
{
    if (streamerId < 0 ||  streamerId >= m_streamerIds.size())
    {
        Log(" >>>>  Bad streamer ID <<<<<");
        return;
    }
    if(m_streammerId == streamerId )
        return;
    m_streammerId = streamerId;
    auto it = m_streamerIds.begin();
    while (streamerId--) it++;
    ParamList params;
    params["streamer"] = *it;
    try {
        ApiFunctionData apiParams("settings_set", params);

        CallApiFunction(apiParams, [&] (Document& jsonRoot){});
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
    } catch (...) {
        Log(" >>>>  FAILED to set streamer ID <<<<<");
    }
}


void OttPlayer::ResetArchiveList()
{
    m_archiveList.clear();
}


void OttPlayer::LoadArchiveList()
{
    struct OttChannelArchive
    {
        int ChannelId;
        int ArchiveHours;
    };
    

    // Load list of channels with archive
    ResetArchiveList();
    try {
        
        ApiFunctionData apiParams("archive_channels_list", ApiFunctionData::s_EmptyParams, ApiFunctionData::API_2_3);

        std::vector<OttChannelArchive> archives;
        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &jsonList = jsonRoot["have_archive_list"];
            if(!jsonList.IsArray())
                throw  JsonParserException("'have_archive_list list' is not array");
            std::for_each(jsonList.Begin(), jsonList.End(), [&]  (const Value & i) mutable
                          {
                              OttChannelArchive arch = {strtoi(i["id"].GetString()), strtoi(i["archive_hours"].GetString())};
                              // Remeber only valid channels
                              if(m_channelList.count(arch.ChannelId) != 0)
                                  archives.push_back(arch);
                          });
            return true;
        });
        m_addonHelper->Log(LOG_DEBUG,"Received %d channels with archive.", archives.size());
        time_t now = time(nullptr);
        std::for_each(archives.begin(), archives.end(), [&]  (std::vector<OttChannelArchive>::value_type & i) {
          BuildRecordingsFor(i.ChannelId, now - i.ArchiveHours * 60 * 60, now);
        });

    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );    
    } catch (std::exception ex) {
        m_addonHelper->Log(LOG_ERROR, "Failed to load archive list. Reason: %s", ex.what());
    }
    catch (...){
        m_addonHelper->Log(LOG_ERROR, "Failed to load archive list. Reason unknown");
    }
}

void OttPlayer::BuildRecordingsFor(OttChannelId channelId, time_t from, time_t to)
{
    EpgEntryList epgEntries;
    GetEpg(channelId, from, to, epgEntries);
    m_addonHelper->Log(LOG_DEBUG,"Building archives for channel %d (%d hours).", channelId, (to - from)/(60*60));
    
     int cnt = 0;
    for(const auto & i : epgEntries)
    {
         const OttEpgEntry& epgTag = i.second;
         if(epgTag.ChannelId == channelId
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
void OttPlayer::Apply(std::function<void(const ArchiveList&)>& action ) const {
    action(m_archiveList);
    return;
}






