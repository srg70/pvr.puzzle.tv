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

#include <rapidjson/error/en.h>
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/prettywriter.h"

#include <assert.h>
#include <algorithm>
#include <sstream>
#include <list>
#include <set>
#include <ctime>
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/util.h"
#include "p8-platform/util/StringUtils.h"
#include "helpers.h"
#include "ttv_player.h"
#include "HttpEngine.hpp"
#include "XMLTV_loader.hpp"
#include "globals.hpp"
#include "guid.hpp"
#include "base64.h"
//#include "libtar.h"

#define CATCH_API_CALL() \
catch (ServerErrorException& ex) { \
    auto err = ex.what(); \
    LogError("%s. TTV API error: %s", __FUNCTION__,  err); \
    if(strcmp(err, "noepg") != 0) { \
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32019), ex.what() ); \
    } \
} catch(CurlErrorException& ex) { \
    LogError("%s. CURL error: %s", __FUNCTION__,  ex.what()); \
    XBMC->QueueNotification(QUEUE_ERROR, "CURL fatal error: %s", ex.what() ); \
} catch(std::exception& ex) { \
    LogError("%s. TTV generic error: %s", __FUNCTION__,  ex.what()); \
} \
catch(...) { \
    LogError("%s. Unknown TTV error.", __FUNCTION__); \
}

namespace TtvEngine
{
    using namespace Globals;
    using namespace std;
    using namespace ADDON;
    using namespace rapidjson;
    using namespace PvrClient;

    
    static const char* c_EpgCacheFile = "trash_epg_cache.txt";
    static const char* c_SessionCacheDir = "special://temp/pvr-puzzle-tv";
    static const char* c_SessionCachePath = "special://temp/pvr-puzzle-tv/trash_session.txt";
    static const char* c_TrashDataCacheDir = "special://temp/pvr-puzzle-tv/trash-data-cache";

    static const char* c_ACE_ENGINE_HLS_STREAM = "/ace/getstream?"; //"/ace/manifest.m3u8?";
    
    struct NoCaseComparator : binary_function<string, string, bool>
    {
        inline bool operator()(const string& x, const string& y) const
        {
            return StringUtils::CompareNoCase(x, y) < 0;
        }
    };

    typedef map<string, pair<Channel, string>, NoCaseComparator> PlaylistContent;
    
    Core::Core(const CoreParams& coreParams)
    : m_coreParams(coreParams)
    {
        m_deviceId = CUSTOM_GUID::generate();
    }
    
    void Core::Init(bool clearEpgCache)
    {
        m_isAceRunning = false;
        if(!CheckAceEngineRunning()) {
            XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32021));
        }

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
//        LogNotice("TtvPlayer stopping...");
//
//        LogNotice("TtvPlayer stopped.");
    }

    #pragma mark - Core interface
    void Core::BuildChannelAndGroupList()
    {
        BuildChannelAndGroupList_Plist();
    }
    
    void Core::UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled)
    {
        UpdateEpgForAllChannels_Plist(startTime, endTime, cancelled);
    }
    
    string Core::GetNextStream(ChannelId channelId, int currentChannelIdx)
    {
        auto& channelList = m_channelList;
        if(channelList.count( channelId ) != 1) {
            LogError(" >>>>   TTV::GetNextStream: unknown channel ID= %d <<<<<", channelId);
            return string();
        }
        auto& urls = channelList.at(channelId).Urls;
        if(urls.size() > currentChannelIdx + 1)
            return urls[currentChannelIdx + 1];
        return string();
        
    }
    
    string Core::GetUrl(ChannelId channelId)
    {
        if(!CheckAceEngineRunning())  {
            XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32021));
            return string();
        }
        return m_coreParams.AceServerUrlBase() + c_ACE_ENGINE_HLS_STREAM + /*(m_coreParams.filterByAlexElec ? "infohash=" : */"id="/*)*/ + m_channelList.at(channelId).Urls[0] + "&pid=" + m_deviceId;
    }

    bool Core::CheckAceEngineRunning()
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
            string strRequest = m_coreParams.AceServerUrlBase() + + "/webui/api/service?method=get_version&format=jsonp&callback=mycallback";
            m_httpEngine->CallApiAsync(strRequest,[&isRunning ] (const std::string& response)
                                       {
                                           LogDebug("Ace Engine version: %s", response.c_str());
                                           isRunning = response.find("version") != string::npos;
                                       }, [&isRunning, &event](const ActionQueue::ActionResult& s) {
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
    
//    string Core::GetUrl_Api_Ace(ChannelId channelId)
//    {
//        if(!m_isAceRunning) {
//            LogNotice("Ace stream enging is not running. Check PVR parameters..", channelId);
//            return string();
//        }
//        if(!m_ttvChannels.at(channelId).canAceStream) {
//            LogNotice("TTV channel %d has no Ace stream available.", channelId);
//            return string();
//        }
//        
//        string source, type;
//        ApiFunctionData apiData(c_TTV_API_URL_base, "translation_stream.php");
//        apiData.params["channel_id"] = n_to_string(channelId);
//        apiData.priority = HttpEngine::RequestPriority_Hi;
//        try {
//            CallApiFunction(apiData, [&source, &type] (Document& jsonRoot)
//                            {
//                                source = jsonRoot["source"].GetString();
//                                type = jsonRoot["type"].GetString();
//                            });
//        }
//        CATCH_API_CALL();
//        if(!source.empty())
//            source = HttpEngine::Escape(source);
//        if(type == "contentid")
//            return m_coreParams.AceServerUrlBase() + c_ACE_ENGINE_HLS_STREAM + "id=" + source + "&pid=" + m_deviceId;
//        if(type == "torrent")
//            return m_coreParams.AceServerUrlBase() + c_ACE_ENGINE_HLS_STREAM +"url=" + source + "&pid=" + m_deviceId;
//        
//        return string();
//
//    }

#pragma mark - Playlist methods
//    UniqueBroadcastIdType Core::AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
//    {
//        UniqueBroadcastIdType id = xmlEpgEntry.startTime;
//        
//        EpgEntry epgEntry;
//        epgEntry.ChannelId = xmlEpgEntry.iChannelId;
//        epgEntry.Title = xmlEpgEntry.strTitle;
//        epgEntry.Description = xmlEpgEntry.strPlot;
//        epgEntry.StartTime = xmlEpgEntry.startTime;
//        epgEntry.EndTime = xmlEpgEntry.endTime;
//        epgEntry.IconPath = xmlEpgEntry.iconPath;
//       return ClientCoreBase::AddEpgEntry(id, epgEntry);
//    }
    
    void Core::UpdateEpgForAllChannels_Plist(time_t startTime, time_t endTime, std::function<bool(void)> cancelled)
    {
        using namespace XMLTV;
        try {
            LoadEpg(cancelled);
            if(!cancelled())
                SaveEpgCache(c_EpgCacheFile);
        } catch (...) {
            LogError("TtvPlayer: failed to update EPG. ");
        }
    }

    
    static const char* GroupMap(const string& orig) {
        static std::map<const string, const char*, NoCaseComparator> lut = {
            {"erotic_18_plus", "Эротика"},
            {"sport", "Спорт"},
            {"movies", "Фильмы"},
            {"series", "Фильмы"},
            {"кино", "Фильмы"},
            {"informational", "Новости"},
            {"Новостные", "Новости"},
            {"music", "Музыка"},
            {"educational", "Познавательные"},
            {"documentaries", "Познавательные"},
            {"kids", "Детские"},
            {"regional", "Региональные"},
            {"religion", "Религиозные"},
            {"entertaining", "Общие"},
            {"webcam", "Другие"},
            {"other", "Другие"},
            {"tv", "Другие"},
            {"NotSet", "Другие"},
            {"Allfon", "Другие"}
        };
        try {
            return lut.at(orig);
        } catch (...) {
            return orig.c_str();
        }
    }
    
    void Core::BuildChannelAndGroupList_Plist()
    {
        using namespace XMLTV;
        PlaylistContent plistContent;
        unsigned int plistIndex = 1;
        
        LoadPlaylist([&plistIndex, &plistContent] (const Document::ValueType& ch)
                     {
                         //TTVChanel ttvChannel;
                         
                         Channel channel;
                         channel.UniqueId = plistIndex;
                         channel.Name = ch["name"].GetString();
                         channel.Number = plistIndex++;
                         channel.Urls.push_back(ch["cid"].GetString());
                         channel.HasArchive = false;
                         channel.IsRadio = false;
                         plistContent[channel.Name] = PlaylistContent::mapped_type(channel,ch["cat"].GetString());
                         
                     });
        if(m_coreParams.filterByAlexElec) {
            PlaylistContent alexPlistContent;
            LoadPlaylistAlexelec([&plistIndex, &plistContent, &alexPlistContent] (const AlexElecChannel & ch)
            {
//                Channel channel;
//                channel.UniqueId = plistIndex;
//                channel.Name = ch.name;
//                channel.Number = plistIndex++;
//                channel.Urls.push_back(ch.cid);
//                channel.HasArchive = false;
//                channel.IsRadio = false;
//                plistContent[channel.Name] = PlaylistContent::mapped_type(channel, TtvEngine::GroupMap(ch.cat));
                if(plistContent.count(ch.name) == 1) {
                    alexPlistContent[ch.name] = plistContent.at(ch.name);
                }
            });
            plistContent = alexPlistContent;
        }
        auto pThis = this;
        
        ChannelCallback onNewChannel = [pThis, &plistContent](const EpgChannel& newChannel){
            for(const auto& epgChannelName : newChannel.strNames) {
                 if(plistContent.count(epgChannelName) != 0) {
                     auto& plistChannel = plistContent[epgChannelName].first;
                     plistChannel.EpgId = newChannel.id;
                     if(plistChannel.IconPath.empty())
                         plistChannel.IconPath = newChannel.strIcon;
                 }
             }
        };
        
        XMLTV::ParseChannels(m_coreParams.epgUrl, onNewChannel);
        
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
            AddChannelToGroup(itGroup->first, channel.UniqueId);
        }
    }
    
    void Core::LoadEpg(std::function<bool(void)> cancelled)
    {
        using namespace XMLTV;
        auto pThis = this;
        
        m_epgUpdateInterval.Init(12*60*60*1000);

        EpgEntryCallback onEpgEntry = [pThis, cancelled] (const XMLTV::EpgEntry& newEntry) {
            pThis->AddEpgEntry(newEntry);
            return !cancelled();
        };
        
        XMLTV::ParseEpg(m_coreParams.epgUrl, onEpgEntry);
    }
    
    void Core::LoadPlaylist(std::function<void(const Document::ValueType &)> onChannel)
    {
        try {
            string trashUrl("http://91.92.66.82/trash/ttv-list/acelive.json|encoding=gzip");
            const string& plistUrl = trashUrl;
            // Download playlist
            XBMC->Log(LOG_DEBUG, "TtvPlayer: loading playlist: %s", plistUrl.c_str());

            string zipContent;
            string unzipedContent;
            XMLTV::GetCachedFileContents(plistUrl, zipContent);
            string& data = zipContent;

            if(XMLTV::IsDataCompressed(zipContent)) {
                if(!XMLTV::GzipInflate(zipContent, unzipedContent))
                    throw IoErrorException("Failed to unzip playlist.");
                data = unzipedContent;
            }
            
//            string compressedFile = XMLTV::GetCachedFilePath(plistUrl);
//            if(compressedFile.empty())
//                throw ServerErrorException("Failed to obtain playlist from server.");
//
//            if(!XBMC->CreateDirectory(c_TrashDataCacheDir))
//                    throw IoErrorException((string("Failed to create cahce directory ") + c_TrashDataCacheDir).c_str());
//
//            char* realPath = XBMC->TranslateSpecialProtocol(compressedFile.c_str());
//
//            TAR *pTar;
//            int i=0;
//            if(0 != tar_open(&pTar, realPath, NULL, O_RDONLY, 0, TAR_IGNORE_CRC)) {
//                XBMC->FreeString(realPath);
//                throw IoErrorException((string("Bad TAR archive ") + compressedFile).c_str());
//            }
//            XBMC->FreeString(realPath);
//            realPath = XBMC->TranslateSpecialProtocol(c_TrashDataCacheDir);
//
//            if(0 != tar_extract_all(pTar, (char*) realPath))
//            {
//                XBMC->FreeString(realPath);
//                tar_close(pTar);
//                LogError("Failed to extract files from TAR archive %s. Error %d", compressedFile.c_str(), errno);
//                throw IoErrorException("Failed to extract files from TAR archive");
//            }
//            XBMC->FreeString(realPath);
//            tar_close(pTar);
            
            //XBMC->Log(LOG_ERROR, ">>> DUMP M3U : \n %s", data.c_str() );
            ParseJson(data, [onChannel] (Document& jsonRoot)
            {
                bool isArray = jsonRoot.IsArray();
                if(isArray) {
                    for(const auto& ch : jsonRoot.GetArray()){
                        onChannel(ch);
                    }
                } else {
                    LogError("TtvPlayer: bad playlist format. No channel will be loaded");
                }
            });
            
            XBMC->Log(LOG_DEBUG, "TtvPlayer: parsing of playlist done.");

        } catch (std::exception& ex) {
            LogError("TtvPlayer: exception during playlist loading: %s", ex.what());
            
        }
    }
    void Core::LoadPlaylistAlexelec(std::function<void(const AlexElecChannel &)> onChannel)
    {
        try {
            const string baseUrl("http://playlist.alexelec.in.ua");
            const string url = baseUrl + "/ChList-Done.html";
            
            std::vector<std::string> headers;
            headers.push_back(string("Referer: ") + baseUrl);
            
            string auth("test:test");
            string encoded = base64_encode(reinterpret_cast<const unsigned char*>(auth.c_str()), auth.length());
            headers.push_back(string("Authorization:Basic ") + encoded);

            string channels;
            P8PLATFORM::CEvent event;
            m_httpEngine->CallApiAsync(
                HttpEngine::Request(url, string(), headers),
                [&channels] (const std::string& response) {
                    channels = response;
                }, [&event](const ActionQueue::ActionResult& s) {
                    event.Signal();
                },
                HttpEngine::RequestPriority_Hi);

            event.Wait();
            // Parse AlexElec response.
            auto posStart = channels.find("<pre>\n");
            if(string::npos == posStart) {
                LogError("TtvPlayer: bad alexelec responce. No <pre> tag found");
                return;
            }
            posStart += strlen("<pre>\n");
            auto posEnd = channels.find("</pre>", posStart);
            if(string::npos == posEnd) {
                LogError("TtvPlayer: bad alexelec responce. No </pre> tag found");
                return;
            }
            channels = channels.substr(posStart, posEnd - posStart);
            for(const auto& ch : StringUtils::Tokenize(channels, "\n")) {
                std::vector<std::string> tokens;
                StringUtils::Tokenize(ch, tokens, "\t");
                if(tokens.size() < 2) {
                    LogError("TtvPlayer: bad alexelec channel: %s", ch.c_str());
                    continue;
                }
                AlexElecChannel alCh;
                alCh.name = StringUtils::Trim(tokens[1]);
//                alCh.cat = StringUtils::Trim(tokens[1]);
//                alCh.cid = StringUtils::Trim(tokens[2]);
                onChannel(alCh);
            }
            XBMC->Log(LOG_DEBUG, "TtvPlayer: parsing of alexelec playlist done.");
            

        } catch (std::exception& ex) {
            LogError("TtvPlayer: exception during alexelec playlist loading: %s", ex.what());
        } catch (...) {
            LogError("TtvPlayer:  unknown exception during alexelec playlist loading.");
        }

    }

}

