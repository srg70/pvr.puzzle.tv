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

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <assert.h>
#include <algorithm>
#include <sstream>
#include <ctime>
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/timeutils.h"
#include "p8-platform/util/util.h"
#include "helpers.h"
#include "sovok_tv.h"

using namespace std;
using namespace ADDON;
using namespace rapidjson;

static const int secondsPerHour = 60 * 60;

const bool ASYNC_API = true;
const bool SYNC_API = true;

//static const char c_EpgCacheDelimeter = '\n';
//
//static const char* c_EpgCacheDirPath = "special://temp/pvr.sovok.tv";
//
//static const char* c_EpgCacheFilePath = "special://temp/pvr.sovok.tv/EpgCache.txt";

static void BeutifyUrl(string& url);
struct SovokTV::ApiFunctionData
{
    enum API_Version
    {
        API_2_2,
        API_2_3
    };
    
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

class SovokTV::HelperThread : public P8PLATFORM::CThread
{
public:
    HelperThread(SovokTV* sovokTV, std::function<void(void)> action)
    : m_sovokTV(sovokTV), m_action(action)
    , m_epgActivityCounter(0), m_stopEvent(false) /*Manual event*/
    {}
    void EpgActivityStarted() {++m_epgActivityCounter;}
    void EpgActivityDone() {}
    void* Process()
    {
        do
        {
            unsigned int oldEpgActivity = m_epgActivityCounter;
            m_sovokTV->LoadArchiveList();
            
            // Wait for epg done before announce archives
            bool isStopped = IsStopped();
            while(!isStopped && (oldEpgActivity != m_epgActivityCounter)){
                oldEpgActivity = m_epgActivityCounter;
                isStopped = m_stopEvent.Wait(2000);// 2sec
            }
            if(isStopped)
                break;
            
            HelperThread* pThis = this;
            m_sovokTV->m_apiCallCompletions->PerformAsync([pThis]() {
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
    SovokTV* m_sovokTV;
    std::function<void(void)> m_action;
    unsigned int m_epgActivityCounter;
    P8PLATFORM::CEvent m_stopEvent;
};


const ParamList SovokTV::ApiFunctionData::s_EmptyParams;

SovokTV::SovokTV(ADDON::CHelper_libXBMC_addon *addonHelper, const string &login, const string &password) :
    m_addonHelper(addonHelper),
    m_login(login),
    m_password(password),
    m_lastEpgRequestStartTime(0),
    m_lastEpgRequestEndTime(0),
    m_lastUniqueBroadcastId(0),
    m_apiCalls(new CActionQueue()),
    m_apiCallCompletions(new CActionQueue()),
    m_archiveLoader(NULL)
{
    m_apiCalls->CreateThread();
    m_apiCallCompletions->CreateThread();

    if (!Login(true)) {
        Cleanup();
        throw AuthFailedException();
    }
    if(!LoadStreamers()) {
        Cleanup();
        throw MissingApiException("streamers");
    }
    LoadSettings();
    LoadEpgCache();
}

SovokTV::~SovokTV()
{
    Cleanup();
}
void SovokTV::Cleanup()
{
    if(m_archiveLoader) {
        m_archiveLoader->StopThread();
        delete m_archiveLoader; m_archiveLoader = NULL;
    }
    Logout();
    if(m_apiCalls) {
        m_apiCalls->StopThread();
        delete m_apiCalls; m_apiCalls = NULL;
    }
    if(m_apiCallCompletions) {
        m_apiCallCompletions->StopThread();
        delete m_apiCallCompletions; m_apiCallCompletions = NULL;
    }
    m_addonHelper->Log(LOG_NOTICE, "SovokTV stopped.");
}

void SovokTV::SaveEpgCache()
{
//    // Leave epg entries not older then 2 weeks from now
//    time_t now = time(nullptr);
//    m_lastEpgRequestStartTime = max(m_lastEpgRequestStartTime, now - 14*24*60*60);
//    m_epgEntries.erase(remove_if(m_epgEntries.begin(), m_epgEntries.end(), [=] (EpgEntryList::value_type i)
//            {
//                return i.StartTime < m_lastEpgRequestStartTime;
//            }),
//            m_epgEntries.end());
//    
//    ostringstream ss;
//    ss << m_lastEpgRequestStartTime << c_EpgCacheDelimeter;
//    ss << m_lastEpgRequestEndTime << c_EpgCacheDelimeter;
//    ss << m_lastUniqueBroadcastId << c_EpgCacheDelimeter;
//    ss << m_epgEntries.size() << c_EpgCacheDelimeter;
//    for_each(m_epgEntries.begin(), m_epgEntries.end(),[&](EpgEntryList::value_type i)
//    {
//        const SovokEpgCaheEntry& cacheItem = i;
//        ss << cacheItem.ChannelId << c_EpgCacheDelimeter;
//        ss << cacheItem.UniqueBroadcastId << c_EpgCacheDelimeter;
//        ss << cacheItem.StartTime << c_EpgCacheDelimeter;
//        ss << cacheItem.EndTime << c_EpgCacheDelimeter;
//        ss << c_EpgCacheDelimeter;
//    });
//    
//    m_addonHelper->CreateDirectory(c_EpgCacheDirPath);
//    
//    void* file = m_addonHelper->OpenFileForWrite(c_EpgCacheFilePath, true);
//    if(NULL == file)
//        return;
//    string buf = ss.rdbuf()->str();
//    m_addonHelper->WriteFile(file, buf.c_str(), buf.size());
//    m_addonHelper->CloseFile(file);
//    
}
void SovokTV::LoadEpgCache()
{
//    void* file = m_addonHelper->OpenFile(c_EpgCacheFilePath, 0);
//    if(NULL == file)
//        return;
//    int64_t fSize = m_addonHelper->GetFileLength(file);
//    
//    char* rawBuf = new char[fSize];
//    if(0 == rawBuf)
//        return;
//    m_addonHelper->ReadFile(file, rawBuf, fSize);
//    
//    istringstream ss(rawBuf);
//    delete[] rawBuf;
//    
//    char delimeter;
//    size_t entriesSize;
//    
//    ss >> m_lastEpgRequestStartTime;
//    ss >> m_lastEpgRequestEndTime;
//    ss >> m_lastUniqueBroadcastId;
//    ss >> entriesSize;
//    
//    while(!ss.eof() && entriesSize-- > 0)
//    {
//        SovokEpgEntry cacheItem;
//        cacheItem.Title = "Cached item";
//        ss >> cacheItem.ChannelId;
//        ss >> cacheItem.UniqueBroadcastId;
//        ss >> cacheItem.StartTime;
//        ss >> cacheItem.EndTime;
////        ss >> delimeter;
//        m_epgEntries.push_back(cacheItem);
//    }
    
}

bool SovokTV::StartArchivePollingWithCompletion(std::function<void(void)> action)
{
    if(m_archiveLoader)
        return false;
    
    m_archiveLoader = new SovokTV::HelperThread(this, action);
    m_archiveLoader->CreateThread(false);
    return true;
}

const ChannelList &SovokTV::GetChannelList()
{
    if (m_channelList.empty())
    {
        BuildChannelAndGroupList();
    }

    return m_channelList;
}

const EpgEntryList& SovokTV::GetEpgList() const
{
    return  m_epgEntries;
}
const StreamerNamesList& SovokTV::GetStreamersList() const
{
    return  m_streamerNames;
}

//const ArchiveList& SovokTV::GetArchiveList()
//{
//    if (m_archiveList.empty())
//    {
//        LoadArchiveList();
//    }
//    return  m_archiveList;
//}

template <typename TParser>
void SovokTV::ParseJson(const std::string& response, TParser parser)
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

void SovokTV::BuildChannelAndGroupList()
{
    m_channelList.clear();
    m_groupList.clear();

    try {
        std::shared_ptr<const ApiFunctionData> params(new ApiFunctionData("channel_list"));
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
                    SovokChannel channel;
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


bool SovokTV::FindEpg(unsigned int brodcastId, SovokEpgEntry& epgEntry)
{
    if(m_epgEntries.count(brodcastId) == 0)
        return false;
    
    epgEntry = m_epgEntries[brodcastId];
    //Log((string(" >>>> Pogramm:") + epgEntry.Title + "<<<<<").c_str());
    
    //string url = GetArchiveForEpg(*result);
    
    return true;
}

std::string SovokTV::GetArchiveForEpg(const SovokEpgEntry& epgEntry)
{
    return  GetArchive(epgEntry.ChannelId, epgEntry.StartTime + m_serverTimeShift);
}

std::string SovokTV::GetArchive(    int channelId, time_t startTime)
{
    string url;
    ParamList params;
    params["cid"] = n_to_string(channelId);
    params["time"] = n_to_string(startTime);
     try {
         std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("archive_next", params));
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

void SovokTV::Log(const char* massage) const
{
    //char* msg = m_addonHelper->UnknownToUTF8(massage);
    m_addonHelper->Log(LOG_DEBUG, massage);
    //m_addonHelper->FreeString(msg);
    
}

void SovokTV::GetEpg(int channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries)
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


void SovokTV::GetEpgForAllChannels(time_t startTime, time_t endTime, EpgEntryList& epgEntries)
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
void SovokTV::GetEpgForAllChannelsForNHours(time_t startTime, short numberOfHours, TFunc func)
{
    // For queries over 24 hours Sovok.TV returns incomplete results.
    assert(numberOfHours > 0 && numberOfHours <= 24);

    ParamList params;
    params["dtime"] = n_to_string(startTime + m_serverTimeShift);
    params["period"] = n_to_string(numberOfHours);
    try {
        std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("epg3", params));

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
                    SovokEpgEntry epgEntry;
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

const GroupList &SovokTV::GetGroupList()
{
    if (m_groupList.empty())
        BuildChannelAndGroupList();

    return m_groupList;
}

string SovokTV::GetUrl(int channelId)
{
    string url;

    ParamList params;
    params["cid"] = n_to_string(channelId);
    params["protect_code"] = m_pinCode.c_str();
    try {
        std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("get_url", params));
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

FavoriteList SovokTV::GetFavorites()
{
    FavoriteList favorites;
    try {
        std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("favorites"));
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


bool SovokTV::Login(bool wait)
{

    if (m_login.empty() || m_password.empty())
        return false;

    ParamList params;
    params["login"] = m_login;
    params["pass"] = m_password;

    auto parser = [=] (Document& jsonRoot)
    {
        m_sessionCookie.clear();
        
        string sid = jsonRoot["sid"].GetString();
        string sidName = jsonRoot["sid_name"].GetString();
        
        if (sid.empty() || sidName.empty())
            throw BadSessionIdException();
        m_sessionCookie[sidName] = sid;
    };
    
    if(wait) {
        try {
            std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("login", params));
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
        std::shared_ptr<ApiFunctionData> apiParams(new ApiFunctionData("login", params));
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

void SovokTV::Logout()
{
    std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("logout"));

    try {CallApiFunction(apiParams, [&] (Document& jsonRoot){});}catch (...) {}
}

template <typename TParser>
void SovokTV::CallApiFunction(std::shared_ptr<const ApiFunctionData> data, TParser parser)
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
void SovokTV::CallApiAsync(std::shared_ptr<const ApiFunctionData> data, TParser parser, TCompletion completion)
{
    if(!m_apiCalls->IsRunning())
        throw QueueNotRunningException("API request queue in not running.");
    m_apiCalls->PerformAsync([=](){
        string query;
        ParamList::const_iterator runner = data->params.begin();
        ParamList::const_iterator first = runner;
        ParamList::const_iterator end = data->params.end();
        
        for (; runner != end; ++runner)
        {
            query += runner == first ? "?" : "&";
            query += runner->first + '=' + runner->second;
        }
        std::string strRequest = "http://api.sovok.tv/";
        strRequest += (data->api_ver == ApiFunctionData::API_2_2) ? "v2.2" : "v2.3";
        strRequest += "/json/";
        strRequest += data->name + query;
        auto start = P8PLATFORM::GetTimeMs();
        const bool isLoginCommand = data->name == "login";
        SendHttpRequest(strRequest, m_sessionCookie, [=](std::string& response) {
            m_addonHelper->Log(LOG_DEBUG, "Calling '%s'. Response in %d ms.",  data->name.c_str(), P8PLATFORM::GetTimeMs() - start);

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
        },[=](const CActionQueue::ActionResult& s) {
            // Do not re-login within login command.
            if(s.status == CActionQueue::kActionCompleted || isLoginCommand) {
                completion(s);
                return;
            }
            // In case of error try to re-login and repeat the API call.
            Login(false);
            SendHttpRequest(strRequest, m_sessionCookie,[&](std::string& response) {
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
            }, [&](const CActionQueue::ActionResult& ss){
                completion(ss);
            });

        });
    },[=](const CActionQueue::ActionResult& s) {
        if(s.status != CActionQueue::kActionCompleted)
            completion(s);
    });
}
template <typename TResultCallback, typename TCompletion>
void SovokTV::SendHttpRequest(const std::string &url, const ParamList &cookie,  TResultCallback result, TCompletion completion) const
{
    char *errorMessage[CURL_ERROR_SIZE];
    std::string* response = new std::string();
    CURL *curl = curl_easy_init();
    if (curl)
    {
        auto start = P8PLATFORM::GetTimeMs();
        m_addonHelper->Log(LOG_INFO, "Sending request: %s", url.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorMessage);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

        string cookieStr;
        ParamList::const_iterator itCookie = cookie.begin();
        for(; itCookie != cookie.end(); ++itCookie)
        {
            if (itCookie != cookie.begin())
                cookieStr += "; ";
            cookieStr += itCookie->first + "=" + itCookie->second;
        }
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookieStr.c_str());

        long httpCode = 0;
        int retries = 5;
        while (retries > 0)
        {
            CURLcode curlCode = curl_easy_perform(curl);
            if (curlCode != CURLE_OK)
            {
                m_addonHelper->Log(LOG_ERROR, "%s: %s", __FUNCTION__, errorMessage);
                break;
            }

            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

            if (httpCode != 503) // temporarily unavailable
                break;

            m_addonHelper->Log(LOG_INFO, "%s: %s", __FUNCTION__, "HTTP error 503 (temporarily unavailable)");

            P8PLATFORM::CEvent::Sleep(1000);
        }
        m_addonHelper->Log(LOG_INFO, "Got HTTP response in %d ms", P8PLATFORM::GetTimeMs() - start);

        if (httpCode != 200)
            *response = "";

        curl_easy_cleanup(curl);
        if(!m_apiCallCompletions->IsRunning())
            throw QueueNotRunningException("API call completion queue in not running.");

        m_apiCallCompletions->PerformAsync([=]() {
            result(*response);
        }, [=](const CActionQueue::ActionResult& s) {
            delete response;
            completion(s);
        });
    }
}

size_t SovokTV::CurlWriteData(void *buffer, size_t size, size_t nmemb, void *userp)
{
    string *response = (string *)userp;
    response->append((char *)buffer, size * nmemb);
    return size * nmemb;
}

bool SovokTV::LoadStreamers()
{
    try {
        std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("streamers"));

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


void SovokTV::LoadSettings()
{
    // Load streamers
    try {
        std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("settings"));

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

void SovokTV::SetStreamerId(int streamerId)
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
        std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("settings_set", params));

        CallApiFunction(apiParams, [&] (Document& jsonRoot){});
    } catch (ServerErrorException& ex) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV error: %s", ex.reason.c_str() );
    } catch (...) {
        Log(" >>>>  FAILED to set streamer ID <<<<<");
    }
}


void SovokTV::ResetArchiveList()
{
    m_archiveList.clear();
}


void SovokTV::LoadArchiveList()
{
    struct SovokChannelArchive
    {
        int ChannelId;
        int ArchiveHours;
    };
    

    // Load list of channels with archive
    ResetArchiveList();
    try {
        
        std::shared_ptr<const ApiFunctionData> apiParams(new ApiFunctionData("archive_channels_list", ApiFunctionData::s_EmptyParams, ApiFunctionData::API_2_3));

        std::vector<SovokChannelArchive> archives;
        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &jsonList = jsonRoot["have_archive_list"];
            if(!jsonList.IsArray())
                throw  JsonParserException("'have_archive_list list' is not array");
            std::for_each(jsonList.Begin(), jsonList.End(), [&]  (const Value & i) mutable
                          {
                              SovokChannelArchive arch = {strtoi(i["id"].GetString()), strtoi(i["archive_hours"].GetString())};
                              // Remeber only valid channels
                              if(m_channelList.count(arch.ChannelId) != 0)
                                  archives.push_back(arch);
                          });
            return true;
        });
        m_addonHelper->Log(LOG_DEBUG,"Received %d channels with archive.", archives.size());
        time_t now = time(nullptr);
        std::for_each(archives.begin(), archives.end(), [&]  (std::vector<SovokChannelArchive>::value_type & i) {
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

void SovokTV::BuildRecordingsFor(int channelId, time_t from, time_t to)
{
    EpgEntryList epgEntries;
    GetEpg(channelId, from, to, epgEntries);
    m_addonHelper->Log(LOG_DEBUG,"Building archives for channel %d (%d hours).", channelId, (to - from)/(60*60));
    
     int cnt = 0;
    for(const auto & i : epgEntries)
    {
         const SovokEpgEntry& epgTag = i.second;
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
void SovokTV::Apply(std::function<void(const ArchiveList&)>& action ) const {
    action(m_archiveList);
    return;
}






