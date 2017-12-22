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

namespace OttEngine
{
    
    static const int secondsPerHour = 60 * 60;
    
    
    template< typename ContainerT, typename PredicateT >
    void erase_if( ContainerT& items, const PredicateT& predicate ) {
        for( auto it = items.begin(); it != items.end(); ) {
            if( predicate(*it) ) it = items.erase(it);
            else ++it;
        }
    };
   
    //
    static const char* c_EpgCacheDirPath = "special://temp/pvr-puzzle-tv";
    
    static const char* c_EpgCacheFilePath = "special://temp/pvr-puzzle-tv/ott_epg_cache.txt";
    
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
    
    class OttPlayer::HelperThread : public P8PLATFORM::CThread
    {
    public:
        HelperThread(ADDON::CHelper_libXBMC_addon *addonHelper, OttPlayer* engine, std::function<void(void)> action)
        : m_engine(engine), m_action(action)
        , m_epgActivityCounter(0)
        , m_addonHelper(addonHelper)
        {}
        
        void EpgActivityStarted() {++m_epgActivityCounter;}
        void EpgActivityDone() {++m_epgActivityCounter;}
        void* Process()
        {
            
            m_addonHelper->Log(LOG_NOTICE, "Archive thread started");
            unsigned int oldEpgActivity = (unsigned int)-1; //For frirst time let for EPG chance

            do
            {
                
                m_addonHelper->Log(LOG_NOTICE, "Archive thread iteraton started");
                // Wait for epg done before announce archives
                bool isStopped = IsStopped();
                while(!isStopped && (oldEpgActivity != m_epgActivityCounter)){
                    m_addonHelper->Log(LOG_NOTICE, "Archive thread iteraton pending (%d)", m_epgActivityCounter);
                    oldEpgActivity = m_epgActivityCounter;
                    isStopped = IsStopped(3);// 3sec
                }
                if(isStopped)
                    break;
                
                m_engine->ResetArchiveList();
                
                P8PLATFORM::CLockObject lock(m_engine->m_epgAccessMutex);
                time_t now = time(nullptr);
                for(const auto& i : m_engine->m_epgEntries) {
                    if(i.second.HasArchive &&  i.second.StartTime < now)
                        m_engine->m_archiveList.insert(i.first);
                }
                m_engine->SaveEpgCache();

                HelperThread* pThis = this;
                m_engine->m_httpEngine->RunOnCompletionQueueAsync([pThis]() {
                    pThis->m_action();
                },  [](const CActionQueue::ActionResult& s) {});
                m_addonHelper->Log(LOG_NOTICE, "Archive thread iteraton done");
                
            }while (!IsStopped(10*60));//10 min
            m_addonHelper->Log(LOG_NOTICE, "Archive thread exit");
            
            return NULL;
            
        }
        
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
        
        OttPlayer* m_engine;
        std::function<void(void)> m_action;
        unsigned int m_epgActivityCounter;
        ADDON::CHelper_libXBMC_addon *m_addonHelper;
        //    P8PLATFORM::CEvent m_stopEvent;
    };
    
    
    const ParamList OttPlayer::ApiFunctionData::s_EmptyParams;
    
    OttPlayer::OttPlayer(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                         const std::string &baseUrl, const std::string &key)
    : m_addonHelper(addonHelper)
    , m_pvrHelper(pvrHelper)
    , m_baseUrl(baseUrl)
    , m_key(key)
    , m_archiveLoader(NULL)
    {
        m_httpEngine = new HttpEngine(m_addonHelper);
        m_baseUrl = "http://" + m_baseUrl ;
        LoadPlaylist();
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
    
    void OttPlayer::LoadPlaylist()
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
            
            m_channelList.clear();
            m_groupList.clear();

            // Parse channels
            //#EXTINF:-1 tvg-id="131" tvg-logo="perviy.png" group-title="Общие" tvg-rec="1" ,Первый канал HD
            //http://ott.watch/stream/{KEY}/131.m3u8

            const char* c_INF = "#EXTINF:";
            pos = data.find(c_INF, pos);
            while(string::npos != pos){
                pos += strlen(c_M3U);
                auto pos_end = data.find(c_INF, pos);
                string::size_type tagLen = (std::string::npos == pos_end) ? std::string::npos : pos_end - pos;
                string tag = data.substr(pos, tagLen);
                ParseChannelAndGroup(tag);
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
    
    void OttPlayer::ParseChannelAndGroup(const string& data)
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
        
        OttChannel channel;
        channel.Id = stoul(FindVar(vars, 0, c_ID));
        channel.Name = name;
        channel.UrlTemplate = url;
        channel.HasArchive = hasArchive;
        channel.IconPath = m_logoUrl + FindVar(vars, 0, c_LOGO);
        m_channelList[channel.Id] = channel;
        
        std::string groupName = FindVar(vars, 0, c_GROUP);
        OttGroup &group = m_groupList[groupName];
        group.Channels.insert(channel.Id);
        
    }
    
   void OttPlayer::SaveEpgCache()
    {
        // Leave epg entries not older then 2 weeks from now
        time_t now = time(nullptr);
        auto oldest = now - 14*24*60*60;
        erase_if(m_epgEntries,  [oldest] (const EpgEntryList::value_type& i)
                 {
                     return i.second.StartTime < oldest;
                 });
        
        StringBuffer s;
        Writer<StringBuffer> writer(s);
        
        writer.StartObject();               // Between StartObject()/EndObject(),

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
        }
    }
    
    bool OttPlayer::StartArchivePollingWithCompletion(std::function<void(void)> action)
    {
        if(m_archiveLoader)
        return false;
        
        m_archiveLoader = new HelperThread(m_addonHelper, this, action);
        m_archiveLoader->CreateThread(false);
        return true;
    }
    
    const ChannelList &OttPlayer::GetChannelList()
    {
        if (m_channelList.empty())
        {
            LoadPlaylist();
        }
        
        return m_channelList;
    }
    
    const EpgEntryList& OttPlayer::GetEpgList() const
    {
        return  m_epgEntries;
    }
    
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
    
    bool OttPlayer::FindEpg(unsigned int brodcastId, OttEpgEntry& epgEntry)
    {
        if(m_epgEntries.count(brodcastId) == 0)
            return false;
        
        epgEntry = m_epgEntries[brodcastId];
         return true;
    }
    
    std::string OttPlayer::GetArchiveForEpg(const OttEpgEntry& epgEntry)
    {
        string url = GetUrl(epgEntry.ChannelId);
        if(url.empty())
            return url;
        url += "?archive=" + n_to_string(epgEntry.StartTime)+"&archive_end=" + n_to_string(epgEntry.EndTime);
        return  url;
    }
    
    void OttPlayer::Log(const char* massage) const
    {
        //char* msg = m_addonHelper->UnknownToUTF8(massage);
        m_addonHelper->Log(LOG_DEBUG, massage);
        //m_addonHelper->FreeString(msg);
        
    }
    
    void OttPlayer::GetEpg(OttChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries)
    {
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);

        bool needMore = true;
        time_t moreStartTime = startTime;
        for (const auto& i  : m_epgEntries)  {
            auto entryStartTime = i.second.StartTime;
            if (i.second.ChannelId == channelId  &&
                entryStartTime >= startTime &&
                entryStartTime < endTime)
            {
                moreStartTime = i.second.EndTime;
                needMore = moreStartTime < endTime;
                epgEntries.insert(i);
            }
        }

        if(needMore) {
            GetEpgForAllChannels(channelId, moreStartTime, endTime);
        }
    }
    
    //template<class TFunc>
    void OttPlayer::GetEpgForAllChannels(OttChannelId channelId, time_t startTime, time_t endTime)
    {
        try {
            bool hasArchive = GetChannelList().at(channelId).HasArchive;
            string call = string("channel/") + n_to_string(channelId);
            ApiFunctionData apiParams(call.c_str());
            bool* shouldUpdate = new bool(false);
            bool hasArchiveLoader =  m_archiveLoader != NULL;

            CallApiAsync(apiParams,  [=] (Document& jsonRoot)
            {
                if(hasArchiveLoader)
                    m_archiveLoader->EpgActivityStarted();
                P8PLATFORM::CLockObject lock(m_epgAccessMutex);
                for (auto& m : jsonRoot.GetObject()) {
                    if(std::stol(m.name.GetString()) < startTime) {
                        continue;
                    }
                    *shouldUpdate = true;
                    OttEpgEntry epgEntry;
                    epgEntry.ChannelId = stoul(m.value["ch_id"].GetString()) ;
                    epgEntry.Title = m.value["name"].GetString();
                    epgEntry.Description = m.value["descr"].GetString();
                    epgEntry.StartTime = m.value["time"].GetInt() ;
                    epgEntry.EndTime = m.value["time_to"].GetInt();
                    epgEntry.HasArchive = hasArchive;
                    unsigned int id = epgEntry.StartTime;
                    while( m_epgEntries.count(id) != 0) {
                        ++id;
                    }
                    m_epgEntries[id] =  epgEntry;
                }
            },
            [=](const CActionQueue::ActionResult& s)
            {
                if(hasArchiveLoader)
                    m_archiveLoader->EpgActivityDone();
                if(s.exception == NULL && *shouldUpdate)
                    m_pvrHelper->TriggerEpgUpdate(channelId);
                delete shouldUpdate;
            });
            
        } catch (ServerErrorException& ex) {
            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32002), ex.reason.c_str() );
        } catch (...) {
            Log(" >>>>  FAILED receive EPG <<<<<");
        }
    }
    
    const GroupList &OttPlayer::GetGroupList()
    {
        if (m_groupList.empty())
            LoadPlaylist();
        
        return m_groupList;
    }
    
    string OttPlayer::GetUrl(OttChannelId channelId)
    {
        string url = GetChannelList().at(channelId).UrlTemplate;
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
    
    void OttPlayer::ResetArchiveList()
    {
        m_archiveList.clear();
    }
    
    void OttPlayer::Apply(std::function<void(const ArchiveList&)>& action ) const {
        action(m_archiveList);
        return;
    }
    
}




