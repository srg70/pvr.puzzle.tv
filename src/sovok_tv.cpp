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
#include "sovok_tv.h"
#include "HttpEngine.hpp"
#include "globals.hpp"
#include "XMLTV_loader.hpp"

using namespace Globals;
using namespace std;
using namespace ADDON;
using namespace rapidjson;
using namespace PvrClient;
using namespace Helpers;

#define CATCH_API_CALL(msg) \
    catch (ServerErrorException& ex) { \
        XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32009), ex.reason.c_str() ); \
    } catch(CurlErrorException& ex) { \
        XBMC->QueueNotification(QUEUE_ERROR, "CURL fatal error: %s", ex.reason.c_str() ); \
    } catch (...) { \
        LogError(msg); \
    }

static const int secondsPerHour = 60 * 60;

static const char* c_EpgCacheFile = "sovok_epg_cache.txt";

static void BeutifyUrl(string& url);
struct SovokTV::ApiFunctionData
{

    ApiFunctionData(const char* _name)
    : ApiFunctionData(_name, ParamList())
    {}

    ApiFunctionData(const char* _name, const ParamList& _params)
    : name(_name) , params(_params)
    {}
    std::string name;
    const ParamList params;
};

//tatic         P8PLATFORM::CTimeout TEST_LOGIN_FAILED_timeout(30 * 1000);


SovokTV::SovokTV(const string &login, const string &password) :
    m_login(login),
    m_password(password)
{
    if (/*TEST_LOGIN_FAILED_timeout.TimeLeft() > 0 ||*/  !Login(true)) {
        Cleanup();
        throw AuthFailedException();
    }
    if(!LoadStreamers()) {
        Cleanup();
        throw MissingApiException("streamers");
    }
    LoadSettings();
}

void SovokTV::Init(bool clearEpgCache)
{
    InitArchivesInfo();
    RebuildChannelAndGroupList();
    if(clearEpgCache) {
        ClearEpgCache(c_EpgCacheFile);
     } else {
        LoadEpgCache(c_EpgCacheFile);
    }
}

SovokTV::~SovokTV()
{
    Cleanup();
    PrepareForDestruction();
}

void SovokTV::Cleanup()
{
    XBMC->Log(LOG_NOTICE, "SovokTV stopping...");

    if(m_httpEngine)
        m_httpEngine->CancelAllRequests();
    
    Logout();

    XBMC->Log(LOG_NOTICE, "SovokTV stopped.");
}

const StreamerNamesList& SovokTV::GetStreamersList() const
{
    return  m_streamerNames;
}

void SovokTV::SetCountryFilter(const CountryFilter& filter)
{
    m_countryFilter = filter;
    //RebuildChannelAndGroupList();
}
void SovokTV::BuildChannelAndGroupList()
{
  
    const bool adultContentDisabled = m_pinCode.empty();
    try {

        ApiFunctionData params("channel_list2");
        CallApiFunction(params, [&] (Document& jsonRoot)
        {
            int maxGroupId = 0;
            for(auto& gr : jsonRoot["groups"].GetArray())
            {
                auto id = atoi(gr["id"].GetString());
                if(maxGroupId < id)
                    maxGroupId = id;
                Group group;
                group.Name = gr["name"].GetString();
                AddGroup(id, group);
            }
            if(m_countryFilter.IsOn) {
                for(int i = 0; i < m_countryFilter.Filters.size(); ++i){
                    auto& f = m_countryFilter.Filters[i];
                    if(f.Hidden)
                        continue;
                    Group group;
                    group.Name = f.GroupName;
                    AddGroup(++maxGroupId, group);
                    m_countryFilter.Groups[i] = maxGroupId;
                }
            }
            for(auto& ch : jsonRoot["channels"].GetArray())
            {
                
                bool isProtected  = atoi(ch["protected"].GetString()) !=0;
                if(adultContentDisabled && isProtected)
                    continue;
                
                Channel channel;
                channel.UniqueId = channel.EpgId = atoi(ch["id"].GetString());
                channel.Number = channel.UniqueId;
                channel.Name = ch["name"].GetString();
                channel.IconPath = string("http://sovok.tv" )+ ch["icon"].GetString();
                channel.IsRadio = ch["is_video"].GetInt() == 0;//channel.Id > 1000;
                channel.HasArchive = atoi(ch["have_archive"].GetString()) != 0;
                
                bool hideChannel = false;
                if(m_countryFilter.IsOn) {
                    for(int i = 0; i < m_countryFilter.Filters.size(); ++i){
                        auto& f = m_countryFilter.Filters[i];
                        if(channel.Name.find(f.FilterPattern) != string::npos) {
                            hideChannel = f.Hidden;
                            if(hideChannel)
                                break;
                            AddChannelToGroup(m_countryFilter.Groups[i], channel.UniqueId);
                        }
                    }
                }
                if(hideChannel)
                    continue;
                
                AddChannel(channel);
 
                string groups = ch["groups"].GetString();
                while(!groups.empty()) {
                    auto pos = groups.find(',');
                    int id = stoi(groups.substr(0,pos));
                    if(string::npos == pos) {
                        groups = "";
                    } else {
                        groups = groups.substr(pos + 1);
                    }
                    AddChannelToGroup(id, channel.UniqueId);
                }
            }
        });
    }
    CATCH_API_CALL(">>>>  FAILED to build channel list <<<<<")
}


std::string SovokTV::GetArchiveUrl(ChannelId channelId, time_t startTime)
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
     }
    CATCH_API_CALL(" >>>>  FAILED receive archive <<<<<")

    return url;
}


void SovokTV::UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled)
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

    int64_t totalNumberOfHours = (endTime - startTime) / secondsPerHour;
    int64_t hoursRemaining = totalNumberOfHours;
    const int64_t hours24 = 24;
    
    while (hoursRemaining > 0 && !cancelled())
    {
        // Query EPG for max 24 hours per single request.
        const int64_t requestNumberOfHours = min(hours24, hoursRemaining);

        GetEpgForAllChannelsForNHours(startTime, requestNumberOfHours);
        
        hoursRemaining -= requestNumberOfHours;
        startTime += requestNumberOfHours * secondsPerHour;
    }
    if(!cancelled())
        SaveEpgCache(c_EpgCacheFile, 14);

}

void SovokTV::GetEpgForAllChannelsForNHours(time_t startTime, short numberOfHours)
{
    // For queries over 24 hours Sovok.TV returns incomplete results.
    assert(numberOfHours > 0 && numberOfHours <= 24);
    
    ParamList params;
    params["dtime"] = n_to_string(startTime + m_serverTimeShift);
    params["period"] = n_to_string(numberOfHours);
    
    
    char       buf[80];
    struct tm  tstruct;
    tstruct = *localtime(&startTime);
    // Visit http://en.cppreference.com/w/cpp/chrono/c/strftime
    // for more information about date/time format
    strftime(buf, sizeof(buf), "%Y-%m-%d.%X", &tstruct);
    
    LogDebug("Scheduled EPG upfdate (all channels) from %s for %d hours.", buf, numberOfHours);
    
    ApiFunctionData apiParams("epg3", params);
    unsigned int epgActivityCounter = ++m_epgActivityCounter;
    try {
        CallApiFunction(apiParams, [this, numberOfHours, startTime] (Document& jsonRoot) {
            const Value &channels = jsonRoot["epg3"];
            for (const auto& channel : channels.GetArray())
            {
                const auto currentChannelId = stoul(channel["id"].GetString());
                // Check last EPG entrie for missing end time
                const EpgEntry* lastEpgForChannel = nullptr;
                IClientCore::EpgEntryAction action = [&lastEpgForChannel, currentChannelId] (const EpgEntryList::value_type& i)
                {
                    if(!lastEpgForChannel) {
                        lastEpgForChannel = &i.second;
                    } else if(currentChannelId == i.second.UniqueChannelId  &&
                              lastEpgForChannel->StartTime < i.second.StartTime) {
                        lastEpgForChannel = &i.second;
                    }
                    return true;
                };
                ForEachEpgLocked(action);
                
                
                const Value& jsonChannelEpg = channel["epg"];
                Value::ConstValueIterator itJsonEpgEntry1 = jsonChannelEpg.Begin();
                Value::ConstValueIterator itJsonEpgEntry2  = itJsonEpgEntry1;
                itJsonEpgEntry2++;
                // Fix end time of last enrty for the channel
                // It can't be calculated during previous iteation.
                if(lastEpgForChannel) {
                    EpgEntry* fixMe = const_cast<EpgEntry*>(lastEpgForChannel);
                    fixMe->EndTime = stol((*itJsonEpgEntry1)["ut_start"].GetString()) - m_serverTimeShift;
                }
                for (; itJsonEpgEntry2 != jsonChannelEpg.End(); ++itJsonEpgEntry1, ++itJsonEpgEntry2)
                {
                    EpgEntry epgEntry;
                    epgEntry.UniqueChannelId = currentChannelId;
                    epgEntry.Title = (*itJsonEpgEntry1)["progname"].GetString();
                    epgEntry.Description = (*itJsonEpgEntry1)["description"].GetString();
                    epgEntry.StartTime = stol((*itJsonEpgEntry1)["ut_start"].GetString()) - m_serverTimeShift;
                    epgEntry.EndTime = stol((*itJsonEpgEntry2)["ut_start"].GetString()) - m_serverTimeShift;
                    
                    UniqueBroadcastIdType id = epgEntry.StartTime;
                    AddEpgEntry(id, epgEntry);
                }
                // Last EPG entrie  missing end time.
                // Put end of requested interval
                EpgEntry epgEntry;
                epgEntry.UniqueChannelId = currentChannelId;
                epgEntry.Title = (*itJsonEpgEntry1)["progname"].GetString();
                epgEntry.Description = (*itJsonEpgEntry1)["description"].GetString();
                epgEntry.StartTime = stol((*itJsonEpgEntry1)["ut_start"].GetString()) - m_serverTimeShift;
                epgEntry.EndTime = startTime + numberOfHours * 60 * 60;
                
                UniqueBroadcastIdType id = epgEntry.StartTime;
                AddEpgEntry(id, epgEntry);
               // PVR->TriggerEpgUpdate(currentChannelId);
            }
        });
    } catch (ServerErrorException& ex) {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32009), ex.reason.c_str() );
    } catch (...) {
        LogError(" >>>>  FAILED receive EPG for N hours<<<<<");
    }
}

void SovokTV::UpdateHasArchive(EpgEntry& entry)
{
    auto pCahnnel = std::find_if(m_channelList.begin(), m_channelList.end(), [&entry] (const ChannelList::value_type& ch) {
        return ch.second.UniqueId == entry.UniqueChannelId;
    });
    entry.HasArchive = pCahnnel != m_channelList.end() &&  pCahnnel->second.HasArchive;
    
    if(!entry.HasArchive)
        return;
    
    time_t now = time(nullptr);
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
    time_t from = now - m_archivesInfo.at(entry.UniqueChannelId) * 60 * 60;
    entry.HasArchive = epgTime >= from && epgTime < now;
}

string SovokTV::GetUrl(ChannelId channelId)
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
    }
    CATCH_API_CALL((string(" >>>>  FAILED to get URL for channel ID=" ) + n_to_string(channelId) + " <<<<<") .c_str())

    return url;
}

void BeutifyUrl(string& url)
{
    url.replace(0, 7, "http");  // replace http/ts with http
    url = url.substr(0, url.find(" ")); // trim VLC params at the end
}

//FavoriteList SovokTV::GetFavorites()
//{
//    FavoriteList favorites;
//    try {
//        ApiFunctionData apiParams("favorites");
//        CallApiFunction(apiParams, [&] (Document& jsonRoot)
//        {
//            const Value &jsonFavorites = jsonRoot["favorites"];
//            Value::ConstValueIterator itFavorite = jsonFavorites.Begin();
//            for(; itFavorite != jsonFavorites.End(); ++itFavorite)
//                favorites.insert((*itFavorite)["channel_id"].GetInt());
//        });
//    }
//    CATCH_API_CALL(" >>>>  FAILED to get favorites <<<<<")
//
//    return favorites;
//}


bool SovokTV::Login(bool wait)
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
            XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32009), ex.reason.c_str() );
            return false;
        } catch(CurlErrorException& ex) {
            XBMC->QueueNotification(QUEUE_ERROR, "CURL fatal error: %s", ex.reason.c_str() );
            return false;
        } catch (...) {
            LogError(" >>>>  FAILED to LOGIN!!! <<<<<");
            return false;
            
        }
        return true;
    } else {
        ApiFunctionData apiParams("login", params);
        CallApiAsync(apiParams, parser, [=] (const ActionQueue::ActionResult& s){
            if(s.exception) {
                try {
                    std::rethrow_exception(s.exception);
                }
                CATCH_API_CALL(" >>>>  FAILED to LOGIN!!! <<<<<")
               
            }
            
        });
        return false;
    }
    
}

void SovokTV::Logout()
{
    ApiFunctionData apiParams("logout");

    try {CallApiFunction(apiParams, [&] (Document& jsonRoot){});}catch (...) {}
}

template <typename TParser>
void SovokTV::CallApiFunction(const ApiFunctionData& data, TParser parser)
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

template <typename TParser>
void SovokTV::CallApiAsync(const ApiFunctionData& data, TParser parser, TApiCallCompletion completion)
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
    std::string strRequest = "http://api.sovok.tv/v2.3";
    //strRequest += (data.api_ver == ApiFunctionData::API_2_2) ? "v2.2" : "v2.3";
    strRequest += "/json/";
    strRequest += data.name + query;
    auto start = P8PLATFORM::GetTimeMs();
    const bool isLoginCommand = data.name == "login" || data.name == "logout";
    LogDebug("Calling '%s'.",  data.name.c_str());

    std::function<void(const std::string&)> parserWrapper = [=](const std::string& response) {
        LogDebug("Response in %d ms.",  P8PLATFORM::GetTimeMs() - start);
        
        //            if(data.name.compare( "get_url") == 0)
        //                LogDebug(response.substr(0, 16380).c_str());
        
        ParseJson(response, [&] (Document& jsonRoot)
                  {
                      if (!jsonRoot.HasMember("error"))
                      {
                          parser(jsonRoot);
                          return;
                      }
                      const Value & errObj = jsonRoot["error"];
                      auto err = errObj["message"].GetString();
                      const Value & errCode = errObj["code"];
                      auto code = errCode.IsInt() ? errCode.GetInt() : errCode.IsString() ? atoi(errCode.GetString()) : -100;
                      XBMC->Log(LOG_ERROR, "Sovok TV server responses error:");
                      XBMC->Log(LOG_ERROR, err);
                      throw ServerErrorException(err,code);
                  });
    };
    m_httpEngine->CallApiAsync(strRequest, parserWrapper, [=](const ActionQueue::ActionResult& s)
                 {
                     // Do not re-login within login/logout command.
                     if(s.status == ActionQueue::kActionCompleted || isLoginCommand) {
                         completion(s);
                         return;
                     }
                     // In case of error try to re-login and repeat the API call.
                     Login(false);
                    m_httpEngine->CallApiAsync(strRequest, parserWrapper,  [=](const ActionQueue::ActionResult& ss){
                                     completion(ss);
                                 });
                 });
}

bool SovokTV::LoadStreamers()
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
                LogDebug("Streamer %s - %s",
                         m_streamerIds[m_streamerIds.size() -1].c_str(),
                         m_streamerNames[m_streamerNames.size() -1].c_str());
            }
        });
    } catch (ServerErrorException& ex) {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32009), ex.reason.c_str() );
    } catch(CurlErrorException& ex) {
        XBMC->QueueNotification(QUEUE_ERROR, "CURL fatal error: %s", ex.reason.c_str() );
        return false;
    } catch (...) {
        LogError(" >>>>  FAILED to load streamers <<<<<");
        return false;
    }
    return true;
}


void SovokTV::LoadSettings()
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
                LogError(" >>>>  Unknown streamer ID <<<<<");
                throw UnknownStreamerIdException();
            }
        });
    }
    CATCH_API_CALL(" >>>>  FAILED to load settings <<<<<")
}

void SovokTV::SetStreamerId(int streamerId)
{
    if (streamerId < 0 ||  streamerId >= m_streamerIds.size())
    {
        LogError(" >>>>  Bad streamer ID <<<<<");
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
    }
    CATCH_API_CALL(" >>>>  FAILED to set streamer ID <<<<<")
}

void SovokTV::InitArchivesInfo()
{
    m_archivesInfo.clear();
    
    try {
        ApiFunctionData apiParams("archive_channels_list");
        
        CallApiFunction(apiParams, [&] (Document& jsonRoot)
        {
            const Value &jsonList = jsonRoot["have_archive_list"];
            if(!jsonList.IsArray())
            throw  JsonParserException("'have_archive_list list' is not array");
            for(auto& i : jsonList.GetArray()) {
                m_archivesInfo[atoi(i["id"].GetString())] = atoi(i["archive_hours"].GetString());
            };
            return true;
        });
        LogDebug("Received %d channels with archive.", m_archivesInfo.size());
    }
    CATCH_API_CALL(" >>>>  FAILED to obtain archive channel list <<<<<")

}






