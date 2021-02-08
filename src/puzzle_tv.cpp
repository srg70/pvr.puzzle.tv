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
#include <time.h>
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
using namespace Helpers;

static const int secondsPerHour = 60 * 60;
static const char* c_EpgCacheFile = "puzzle_epg_cache.txt";
//static PuzzleTV::TChannelStreams s_NoStreams;


static void DumpStreams(const PuzzleTV::TPrioritizedSources& s)
{
    PuzzleTV::TPrioritizedSources sources(s);
    while (!sources.empty()) {
        const auto& source = sources.top()->second;
        const auto& streams = source.Streams;
        std::for_each(streams.begin(), streams.end(), [&source](PuzzleTV::PuzzleSource::TStreamsQuality::const_reference stream) {
            LogDebug("URL %s: %s",  source.Server.c_str(), stream.first.c_str());
        });
        sources.pop();
    }
    
}

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
    if(clearEpgCache)
        ClearEpgCache(c_EpgCacheFile, m_epgUrl.c_str());
    
    // Channels use EPG for name synch!
    RebuildChannelAndGroupList();
    
    if(!clearEpgCache)
        LoadEpgCache(c_EpgCacheFile);

    UpdateArhivesAsync();
}

PuzzleTV::~PuzzleTV()
{
    Cleanup();
    PrepareForDestruction();
}
void PuzzleTV::Cleanup()
{
//    LogNotice("PuzzleTV stopping...");
//
//    
//    LogNotice( "PuzzleTV stopped.");
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

    struct GroupWithIndex{
        GroupWithIndex(string g, int idx = 0) : name(g), index(idx) {}
        string name;
        int index;
    };
    typedef std::vector<GroupWithIndex> GroupsWithIndex;
    typedef map<string, pair<Channel, GroupsWithIndex >, NoCaseComparator> PlaylistContent;

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
            unsigned int channelNumber = 0;
            for(; itChannel != channels.End(); ++itChannel)
            {
                Channel channel;
                char* dummy;
                channel.UniqueId = channel.EpgId = strtoul((*itChannel)["id"].GetString(), &dummy, 16);
                if(itChannel->HasMember("num")){
                    channel.Number = (*itChannel)["num"].GetInt();
                } else {
                    channel.Number = ++channelNumber;
                }
                channel.Name = (*itChannel)["name"].GetString();
                channel.IconPath = (*itChannel)["icon"].GetString();
                channel.IsRadio = false ;
                channel.HasArchive = false;
                if(isPuzzle2)
                    channel.Urls.push_back((*itChannel)["url"].GetString());
                
                GroupsWithIndex groups;
                if(itChannel->HasMember("group_num")) {
                    const auto& jGroups = (*itChannel)["group_num"];
                    for(auto groupIt = jGroups.GetArray().Begin(); groupIt < jGroups.GetArray().End(); ++groupIt){
                        groups.push_back(GroupsWithIndex::value_type((*groupIt)["name"].GetString(), (*groupIt)["num"].GetInt()));
                    }

                } else {
                    // Groups (string or array)
                    const auto& jGroups = (*itChannel)["group"];
                    if(jGroups.IsString()) {
                        groups.push_back(GroupsWithIndex::value_type
                                         ((*itChannel)["group"].GetString()));
                    } else if(jGroups.IsArray()) {
                        for(auto groupIt = jGroups.GetArray().Begin(); groupIt < jGroups.GetArray().End(); ++groupIt){
                            groups.push_back(GroupsWithIndex::value_type(groupIt->GetString()));
                        }
                    }
                }
                plistContent[channel.Name] = PlaylistContent::mapped_type(channel,groups);
            }
        });
        
        if(m_epgType == c_EpgType_File) {

            m_epgToServerLut.clear();
            using namespace XMLTV;
            
            auto pThis = this;
            // Build LUT channels ID from EPG to Server
            ChannelCallback onNewChannel = [&plistContent, pThis](const EpgChannel& newChannel){
                for(const auto& epgChannelName : newChannel.displayNames) {
                    if(plistContent.count(epgChannelName) != 0) {
                        auto& plistChannel = plistContent[epgChannelName].first;
                        pThis->m_epgToServerLut[newChannel.id] = plistChannel.EpgId;
                        if(plistChannel.IconPath.empty())
                            plistChannel.IconPath = newChannel.strIcon;
                    }
                }
            };
            
            XMLTV::ParseChannels(m_epgUrl, onNewChannel);
        }

        for(const auto& channelWithGroup : plistContent)
        {
            const auto& channel = channelWithGroup.second.first;
            
            //TranslateMulticastUrls(channel);
            AddChannel(channel);
            
            for (const auto& groupWithIndex : channelWithGroup.second.second) {
                const auto& groupList = m_groupList;
                auto itGroup =  std::find_if(groupList.begin(), groupList.end(), [&](const GroupList::value_type& v ){
                    return groupWithIndex.name ==  v.second.Name;
                });
                if (itGroup == groupList.end()) {
                    Group newGroup;
                    newGroup.Name = groupWithIndex.name;
                    AddGroup(groupList.size(), newGroup);
                    itGroup = --groupList.end();
                }
                AddChannelToGroup(itGroup->first, channel.UniqueId, groupWithIndex.index);
            }
        }
       
    } catch (ServerErrorException& ex) {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32006), ex.reason.c_str());
    } catch (std::exception& ex) {
        LogError("PuzzleTV: FAILED to build channel list. Exception: %s", ex.what());
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
//     } 	 {
//         LogError(" >>>>  FAILED receive archive <<<<<");
//    }
//    return url;
//}

UniqueBroadcastIdType PuzzleTV::AddXmlEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
{
    if(m_epgToServerLut.count(xmlEpgEntry.EpgId) == 0) {
        //LogError("PuzzleTV::AddXmlEpgEntry(): XML EPG entry '%s' for unknown channel %d", xmlEpgEntry.strTitle.c_str(), xmlEpgEntry.iChannelId);
        return c_UniqueBroadcastIdUnknown;
    }
    
    unsigned int id = (unsigned int)xmlEpgEntry.startTime;

    EpgEntry epgEntry;
    epgEntry.UniqueChannelId = m_epgToServerLut[xmlEpgEntry.EpgId];
    epgEntry.Title = xmlEpgEntry.strTitle;
    epgEntry.Description = xmlEpgEntry.strPlot;
    epgEntry.StartTime = xmlEpgEntry.startTime;
    epgEntry.EndTime = xmlEpgEntry.endTime;
    epgEntry.IconPath = xmlEpgEntry.iconPath;
    return AddEpgEntry(id, epgEntry);
}

void PuzzleTV::UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled)
{
    try {
        LoadEpg(cancelled);
        if(!cancelled())
            SaveEpgCache(c_EpgCacheFile);
//        } catch (ServerErrorException& ex) {
//            XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32002), ex.reason.c_str() );
    } catch (std::exception& ex) {
        LogError("PuzzleTV: FAILED receive EPG. Exception: %s", ex.what());
    } catch (...) {
        LogError(" >>>>  FAILED receive EPG <<<<<");
    }
}

static bool time_compare (const EpgEntry& first, const EpgEntry& second)
{
    return first.StartTime < second.StartTime;
}

void PuzzleTV::LoadEpg(std::function<bool(void)> cancelled)
{
    //    using namespace XMLTV;
    auto pThis = this;
    m_epgUpdateInterval.Init(12*60*60*1000);

    if(m_epgType == c_EpgType_File) {
        
        XMLTV::EpgEntryCallback onEpgEntry = [pThis, cancelled] (const XMLTV::EpgEntry& newEntry) {
            pThis->AddXmlEpgEntry(newEntry);
            return !cancelled();
        };
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
                                epgEntry.UniqueChannelId = channelId;
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
        } catch (std::exception& ex) {
            LogError("PuzzleTV: exception on lodaing JSON EPG: %s", ex.what());
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

#pragma mark - Archive
void PuzzleTV::UpdateHasArchive(PvrClient::EpgEntry& entry)
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
    const time_t archivePeriod = 3 * 24 * 60 * 60; //3 days in secs
    time_t from = now - archivePeriod;
    entry.HasArchive = epgTime > from && epgTime < now;
}

void PuzzleTV::UpdateArhivesAsync()
{
    if(m_serverVersion == c_PuzzleServer2) {
        return; // No archive support in version 2
    }
    
    auto pThis = this;
    TArchiveInfo* localArchiveInfo = new TArchiveInfo();
    ApiFunctionData data("/archive/json/list", m_serverPort);
    CallApiAsync(data ,
                 [localArchiveInfo, pThis] (Document& jsonRoot) {
                     if(!jsonRoot.IsArray()) {
                         LogError("PuzzleTV::UpdateArhivesAsync(): bad Puzzle Ser ver response (not array).");
                         return;
                     }
                     LogDebug("PuzzleTV::UpdateArhivesAsync(): recivrd %d channels with archive.", jsonRoot.GetArray().Size());
                     for (const auto& ch : jsonRoot.GetArray()) {
                         if(!ch.IsObject() || !ch.HasMember("name")){
                             LogError("PuzzleTV::UpdateArhivesAsync(): channnel not an object or unnamed.");
                             continue;
                         }
                         const auto& chName = ch["name"].GetString();

                          if(!ch.HasMember("id") || !ch.HasMember("cid")) {
                             LogError("PuzzleTV::UpdateArhivesAsync(): channnel %s does not have ID or archive ID.", chName);
                             continue;
                         }
                         const auto& archiveChId = ch["id"].GetString();
                         const auto& chId = ch["cid"].GetString();

                         char* dummy;
                         ChannelId id  = strtoul(chId, &dummy, 16);
                         try {
                             Channel channelWithArcive(pThis->m_channelList.at(id));
                             channelWithArcive.HasArchive = true;
                             pThis->AddChannel(channelWithArcive);
                             LogDebug("PuzzleTV::UpdateArhivesAsync(): channel with archive %s.", channelWithArcive.Name.c_str());

                             ChannelArchiveInfo chInfo;
                             chInfo.archiveId = archiveChId;
                             (*localArchiveInfo)[id] = chInfo;
                         } catch (std::out_of_range) {
                             LogError("PuzzleTV::UpdateArhivesAsync():Unknown archive channel %s (%s)", chName, chId);
                         }
                         catch (...){
                             LogError("PuzzleTV::UpdateArhivesAsync(): unknown error for channel %s (%s)", chName, chId);
                         }
                     }
                 },[localArchiveInfo, pThis](const ActionQueue::ActionResult& s) {
                     if(s.status == ActionQueue::kActionCompleted) {
                        P8PLATFORM::CLockObject lock(pThis->m_archiveAccessMutex);
                        pThis->m_archiveInfo = *localArchiveInfo;
                        //pThis->ValidateArhivesAsync();
                     }
                     delete localArchiveInfo;
                 });

}

std::string PuzzleTV::GetArchiveUrl(ChannelId channelId, time_t startTime)
{
    string url;
    string recordId = GetRecordId(channelId, startTime);
    
    if(recordId.empty())
        return url;
    
    // http://127.0.0.1:8185/archive/json/records/674962D4|4d76800bfc3a4a5b97b0ab8efc6603ce|1
    string command("/archive/json/records/");
    command += recordId;
    
    ApiFunctionData data(command.c_str(), m_serverPort);
    try {
        CallApiFunction(data ,
                        [&url, recordId] (Document& jsonRoot) {
                            if(!jsonRoot.IsArray()) {
                                LogError("PuzzleTV::GetArchiveUrl(): wrong JSON format (not array). RID=%s", recordId.c_str());
                                return;
                            }
                            url = jsonRoot.Begin()->GetString();
                        });
    }
    catch(...){
        
    }
    
    return url;
}


std::string PuzzleTV::GetRecordId(ChannelId channelId, time_t startTime) {
    
    // Assuming Moscow time EPG +3:00
    long offset = -(3 * 60 * 60) - XMLTV::LocalTimeOffset();
    startTime += offset ;//+ 1 * 60 * 60;
    
    if(m_serverVersion == c_PuzzleServer2) {
        return string(); // No archive support in version 2
    }
    
    string archiveId;
    {
        P8PLATFORM::CLockObject lock(m_archiveAccessMutex);
        if(m_archiveInfo.count(channelId) == 0) {
            LogDebug("PuzzleTV::ValidateArhiveFor(): requested archive URL for unknown channel");
            return string();
        }
        
        ChannelArchiveInfo& channelArchiveInfo = m_archiveInfo[channelId];
        archiveId = channelArchiveInfo.archiveId;

       // Do we have an ID of the record already?
        if(channelArchiveInfo.records.count(startTime) != 0) {
            return channelArchiveInfo.records[startTime].id;
        }
    }
    
    // Request archive info from server for recording's day.
    time_t now = time(nullptr);
    // Check for start time in the future
    if(startTime > now )
        return string();
    
    struct tm* t = localtime(&now);
    int day_now = t->tm_yday;
    t = localtime(&startTime);
    int day_start = t->tm_yday;
    
    // Over the year
    if(day_now < day_start) {
        day_now += 365;
    }
    int day = (day_now - day_start);
    
    auto pThis = this;
    
    string command("/archive/json/id/");
    command += archiveId + "/day/" + n_to_string(day);

    ApiFunctionData data(command.c_str(), m_serverPort);
    TArchiveRecords* records = new TArchiveRecords();
    try {
        CallApiFunction(data ,
                        [records, archiveId, pThis] (Document& jsonRoot) {
                            //dump_json(jsonRoot);

                            if(!jsonRoot.IsObject()) {
                                LogError("PuzzleTV: wrong JSON format of archive info. AID=%s", archiveId.c_str());
                                return;
                            }
                            
                            std::for_each(jsonRoot.MemberBegin(), jsonRoot.MemberEnd(), [records, archiveId]  ( Value::ConstMemberIterator::Reference & i) {
                                
                                if(i.value.IsObject() && i.value.HasMember("id") && i.value.HasMember("s_time")) {
                                    auto& arObj = i.value;
                                    double t = arObj["s_time"].GetDouble();
                                    auto& record = (*records)[t];
                                    record.id = arObj["id"].GetString();
                                }
                            });
                        });
        
        {
            P8PLATFORM::CLockObject lock(pThis->m_archiveAccessMutex);
            pThis->m_archiveInfo[channelId].records.insert(std::begin(*records), std::end(*records));
            delete records;
            records = nullptr;
            
            // Did we obtain an ID of the record?
            if(m_archiveInfo[channelId].records.count(startTime) != 0) {
                return m_archiveInfo[channelId].records[startTime].id;
            }
        }
    } catch (...) {
        LogError("PuzzleTV::GetRecordId(): FAILED to obtain recordings for channel %d, day %d", channelId, day);
        if(nullptr != records)
            delete records;
    }
    return string();

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
        const auto& streams = sources.top()->second.Streams;
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

string PuzzleTV::GetNextStream(ChannelId channelId, int currentStreamIdx)
{
    if(!CheckChannelId(channelId))
        return string();

    // NOTE: currentStreamIdx is an offset of current stream within "good" treams
    // When call to PuzzleTV::OnOpenStremFailed() been done before GetNextStream(),
    // and where bad stream should be marked as bad, the index used to be zero.
    // We'll return next after currentStreamIdx a "good" stream
    string url;
    auto sources = GetSourcesForChannel(channelId);
    DumpStreams(sources);
    bool isFound = false;
    int goodStreamsIdx = -1;
    
    while (!isFound && !sources.empty()) {
        const auto& streams = sources.top()->second.Streams;
        auto goodStream = std::find_if(streams.begin(), streams.end(), [](PuzzleSource::TStreamsQuality::const_reference stream) {return stream.second;});
        if(goodStream != streams.end()) {
            url = goodStream->first;
            ++goodStreamsIdx;
            // Check whether Ace Engine is running for ace link
            string aceServerUrlBase;
            if(!IsAceUrl(url, aceServerUrlBase)){
                isFound =  goodStreamsIdx > currentStreamIdx; // return non ase stream immideatelly
            }else if(CheckAceEngineRunning(aceServerUrlBase.c_str()))  {
                isFound = goodStreamsIdx > currentStreamIdx; // ase engin running, so return ase stream.
            }
            LogDebug("PuzzleTV::GetNextStream(): found good stream. Idx %d. Current %d", goodStreamsIdx, currentStreamIdx);
        }
        sources.pop();
    }
    
    return isFound ? url : string();
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
        } else {
            LogError("PuzzleTV::OnOpenStremFailed(): failed to open unknown stream!");
        }
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
            auto pThis = this;
            const string  strId = ToPuzzleChannelId(channelId);
            if(m_serverVersion == c_PuzzleServer2){
                std::string cmd = string("/get/streams/") + strId;
                ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
                CallApiFunction(apiParams, [&urls, pThis] (Document& jsonRoot)
                                {
                                    if(!jsonRoot.IsArray())
                                        return;
                                    std::for_each(jsonRoot.Begin(), jsonRoot.End(), [&]  (const Value & i) mutable
                                                  {
                                                      auto url = pThis->TranslateMultucastUrl(i.GetString());
                                                      urls.push_back(url);
                                                      LogDebug(" >>>>  URL: %s <<<<<",  url.c_str());
                                                      
                                                  });
                                });
            } else {
                auto& cacheSources = m_sources[channelId];
                std::string cmd = string("/streams/json_ds/") + strId;
                ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
                CallApiFunction(apiParams, [&urls, &cacheSources, pThis] (Document& jsonRoot)
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
                                                          auto url = pThis->TranslateMultucastUrl(st.GetString());
                                                          urls.push_back(url);
                                                          source.Streams[url] = true;
                                                          //LogDebug("URL %s: %s",  source.Server.c_str(), url);
                                                      });
                                                  });
                                });
            }
            
            AddChannel(ch);
        } catch (ServerErrorException& ex) {
            XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32006), ex.reason.c_str());
        } catch (MissingApiException& ex){
            LogError("PuzzleTV: Bad JSON responce for '/streams/json_ds/': %s", ex.what());
        } catch (std::exception& ex) {
            LogError("PuzzleTV: FAILED to get URL for channel ID=%d. Exception: %s", channelId, ex.what());
        }  catch (...) {
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
    } catch (std::exception& ex) {
        LogError("PuzzleTV: FAILED to get sources list for channel ID=%d. Exception: %s", channelId, ex.what());
    } catch (...) {
        LogError(" >>>>  FAILED to get sources list for channel ID=%d <<<<<", channelId);
    }
    

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
            //http://127.0.0.1:8185/black_list/base64url/unlock/CID
            const string  strId = ToPuzzleChannelId(channelId);
            string encoded = base64_encode(reinterpret_cast<const unsigned char*>(cacheUrl.c_str()), cacheUrl.length());
            string cmd = string("/black_list/")+ encoded + "/unlock/" + strId + "/nofollow";
            
            
            ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
            CallApiAsync(apiParams, [](Document&){}, [channelId, strId](const ActionQueue::ActionResult& s) {
                if(s.exception){
                    LogError("PuzzleTV: FAILED to disble source for channel %s", strId.c_str());
                }
            });
        
            UpdateChannelSources(channelId);
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
            //http://127.0.0.1:8185/black_list/base64url/lock/CID
            const string  strId = ToPuzzleChannelId(channelId);
            string encoded = base64_encode(reinterpret_cast<const unsigned char*>(cacheUrl.c_str()), cacheUrl.length());
            string cmd = string("/black_list/")+ encoded + "/lock/" + strId + "/nofollow";
            
            
            ApiFunctionData apiParams(cmd.c_str(), m_serverPort);
            CallApiAsync(apiParams, [](Document&){}, [channelId, strId](const ActionQueue::ActionResult& s) {
                if(s.exception){
                    LogError("PuzzleTV: FAILED to disble source for channel %s", strId.c_str());
                }
            });
            
            UpdateChannelSources(channelId);
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

