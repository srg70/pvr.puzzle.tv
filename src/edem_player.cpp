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
#include <vector>
#include <ctime>
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/util.h"
#include "p8-platform/util/StringUtils.h"
#include "helpers.h"
#include "edem_player.h"
#include "HttpEngine.hpp"
#include "XMLTV_loader.hpp"


namespace EdemEngine
{
    using namespace std;
    using namespace ADDON;
    using namespace rapidjson;
    using namespace PvrClient;

    
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
    static const char* c_EpgCacheFilePath = "special://temp/pvr-puzzle-tv/edem_epg_cache.txt";
    
    
    struct NoCaseComparator : binary_function<string, string, bool>
    {
        inline bool operator()(const string& x, const string& y) const
        {
            return StringUtils::CompareNoCase(x, y) < 0;
        }
    };

    typedef map<string, pair<Channel, string>, NoCaseComparator> PlaylistContent;
    static void ParseChannelAndGroup(const std::string& data, unsigned int plistIndex, PlaylistContent& channels);
    static void LoadPlaylist(const string& plistUrl, PlaylistContent& channels, CHelper_libXBMC_addon *XBMC);

    
    class Core::HelperThread : public P8PLATFORM::CThread
    {
    public:
        HelperThread(ADDON::CHelper_libXBMC_addon *addonHelper, Core* engine, std::function<void(void)> action)
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
                const time_t archivePeriod = 4 * 24 * 60 * 60; //4 days in secs
                for(const auto& i : m_engine->m_epgEntries) {
                    auto when = now - i.second.StartTime;
                    if(i.second.HasArchive &&   when > 0 && when < archivePeriod)
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
        
        Core* m_engine;
        std::function<void(void)> m_action;
        unsigned int m_epgActivityCounter;
        ADDON::CHelper_libXBMC_addon *m_addonHelper;
        //    P8PLATFORM::CEvent m_stopEvent;
    };
    
    
    Core::Core(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper, const std::string &playListUrl,  const std::string &epgUrl)
    : m_addonHelper(addonHelper)
    , m_pvrHelper(pvrHelper)
    , m_playListUrl(playListUrl)
    , m_epgUrl(epgUrl)
    , m_archiveLoader(NULL)
    {
        m_httpEngine = new HttpEngine(m_addonHelper);
        LoadPlaylist();
        LoadEpgCache();
        LoadEpg();
    }
    
    Core::~Core()
    {
        Cleanup();
    }
    void Core::Cleanup()
    {
        m_addonHelper->Log(LOG_NOTICE, "EdemPlayer stopping...");
        
        if(m_archiveLoader) {
            m_archiveLoader->StopThread();
        }
        if(m_httpEngine){
            SAFE_DELETE(m_httpEngine);
        }
        
        if(m_archiveLoader)
            SAFE_DELETE(m_archiveLoader);

        m_addonHelper->Log(LOG_NOTICE, "EdemPlayer stopped.");
    }
    
    void Core::LoadPlaylist()
    {
        using namespace XMLTV;
        PlaylistContent plistContent;
        EdemEngine::LoadPlaylist(m_playListUrl, plistContent, m_addonHelper);
        
        
        ChannelCallback onNewChannel = [&plistContent](const EpgChannel& newChannel){
            if(plistContent.count(newChannel.strName) != 0) {
                auto& plistChannel = plistContent[newChannel.strName].first;
                plistChannel.Id = stoul(newChannel.strId.c_str());
                plistChannel.IconPath = newChannel.strIcon;
            }
        };
        
        XMLTV::ParseChannels(m_epgUrl, onNewChannel, m_addonHelper);
//        if(shouldUpdateChannels) {
//            m_pvrHelper->TriggerChannelUpdate();
//        }

        m_channelList.clear();
        m_groupList.clear();
        
        for(const auto& channelWithGroup : plistContent)
        {
            const auto& channel = channelWithGroup.second.first;
            const auto& groupName = channelWithGroup.second.second;
            
            m_channelList[channel.Id] = channel;
            
            auto itGroup =  std::find_if(m_groupList.begin(), m_groupList.end(), [&](const GroupList::value_type& v ){
                return groupName ==  v.second.Name;
            });
            if (itGroup == m_groupList.end()) {
                m_groupList[m_groupList.size()].Name = groupName;
                itGroup = --m_groupList.end();
            }
            
            itGroup->second.Channels.insert(channel.Id);
        }
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
   
   void Core::SaveEpgCache()
    {
        // Leave epg entries not older then 1 weeks from now
        time_t now = time(nullptr);
        auto oldest = now - 7*24*60*60;
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
    void Core::LoadEpgCache()
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
                    if(m_channelList.count(e.ChannelId) != 1)
                        continue;
                    m_epgEntries[k] = e;
                }
            });
            
        } catch (...) {
            Log(" >>>>  FAILED load EPG cache <<<<<");
            m_epgEntries.clear();
        }
    }
    
    bool Core::StartArchivePollingWithCompletion(std::function<void(void)> action)
    {
        if(m_archiveLoader)
            return false;
        
        m_archiveLoader = new HelperThread(m_addonHelper, this, action);
        m_archiveLoader->CreateThread(false);
        return true;
    }
    
    const ChannelList &Core::GetChannelList()
    {
        if (m_channelList.empty())
        {
            LoadPlaylist();
        }
        
        return m_channelList;
    }
    
    const EpgEntryList& Core::GetEpgList() const
    {
        return  m_epgEntries;
    }
    
    std::string Core::GetArchiveUrl(ChannelId channelId, time_t startTime)
    {
        string url = GetUrl(channelId);
        if(url.empty())
            return url;
        url += "?utc=" + n_to_string(startTime)+"&lutc=" + n_to_string(time(nullptr));
        return  url;
    }
    
    void Core::Log(const char* massage) const
    {
        //char* msg = m_addonHelper->UnknownToUTF8(massage);
        m_addonHelper->Log(LOG_DEBUG, massage);
        //m_addonHelper->FreeString(msg);
        
    }
    
    void Core::GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries)
    {
        if(m_archiveLoader)
            m_archiveLoader->EpgActivityStarted();
        
        P8PLATFORM::CLockObject lock(m_epgAccessMutex);

        time_t moreStartTime = startTime;
        bool needMore = true;
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
        
        if(needMore ) {
            UpdateEpgForAllChannels(channelId, moreStartTime, endTime);
        }
        if(m_archiveLoader)
            m_archiveLoader->EpgActivityDone();
    }
    
    bool Core::AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry)
    {
        unsigned int id = xmlEpgEntry.startTime;
        
        // Do not add EPG for unknown channels
        if(m_channelList.count(xmlEpgEntry.iChannelId) != 1)
           return false;

        while( m_epgEntries.count(id) != 0) {
            // Do not override existing EPG.
            if(m_epgEntries[id].ChannelId == xmlEpgEntry.iChannelId)
                return false;
            ++id;
        }
        EpgEntry epgEntry;
        epgEntry.ChannelId = xmlEpgEntry.iChannelId;
        epgEntry.Title = xmlEpgEntry.strTitle;
        epgEntry.Description = xmlEpgEntry.strPlot;
        epgEntry.StartTime = xmlEpgEntry.startTime;
        epgEntry.EndTime = xmlEpgEntry.endTime;
        epgEntry.HasArchive = true;
        m_epgEntries[id] =  epgEntry;

        return true;
    }
    
    void Core::UpdateEpgForAllChannels(ChannelId channelId, time_t startTime, time_t endTime)
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
//        } catch (ServerErrorException& ex) {
//            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32002), ex.reason.c_str() );
        } catch (...) {
            Log(" >>>>  FAILED receive EPG <<<<<");
        }
    }
    
    void Core::LoadEpg()
    {
        using namespace XMLTV;
        auto pThis = this;

        bool shouldUpdateChannels = false;
        EpgEntryCallback onEpgEntry = [&pThis] (const XMLTV::EpgEntry& newEntry) {pThis->AddEpgEntry(newEntry);};
        
        XMLTV::ParseEpg(m_epgUrl, onEpgEntry, m_addonHelper);
    }
    
    const GroupList &Core::GetGroupList()
    {
        if (m_groupList.empty())
            LoadPlaylist();
        
        return m_groupList;
    }
    
    string Core::GetUrl(ChannelId channelId)
    {
        string url = GetChannelList().at(channelId).Urls[0];
        return url;
    }
    
    
    void Core::ResetArchiveList()
    {
        m_archiveList.clear();
    }
    
    void Core::Apply(std::function<void(const ArchiveList&)>& action ) const {
        action(m_archiveList);
        return;
    }
    
    template <typename TParser>
    void Core::ParseJson(const std::string& response, TParser parser)
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
    
    static void LoadPlaylist(const string& plistUrl, PlaylistContent& channels, CHelper_libXBMC_addon *XBMC)
    {
        void* f = NULL;
        
        try {
            char buffer[1024];
            string data;
            
            // Download playlist
            auto f = XBMC->OpenFile(plistUrl.c_str(), 0);
            if (!f)
                throw BadPlaylistFormatException("Failed to obtain playlist from server.");
            bool isEof = false;
            do{
                auto bytesRead = XBMC->ReadFile(f, buffer, sizeof(buffer));
                isEof = bytesRead <= 0;
                if(!isEof)
                    data.append(&buffer[0], bytesRead);
            }while(!isEof);
            XBMC->CloseFile(f);
            f = NULL;
            
            //XBMC->Log(LOG_ERROR, ">>> DUMP M3U : \n %s", data.c_str() );
            
            // Parse gloabal variables
            //#EXTM3U
            const char* c_M3U = "#EXTM3U";
            
            auto pos = data.find(c_M3U);
            if(string::npos == pos)
                throw BadPlaylistFormatException("Invalid playlist format: missing #EXTM3U tag.");
            pos += strlen(c_M3U);
            
            // Parse channels
            //#EXTINF:0,РБК-ТВ
            //#EXTGRP:новости
            //http://882406a1.iptvspy.me/iptv/9A7PTRGZWEA336/106/index.m3u8
            
            const char* c_INF = "#EXTINF:";
            pos = data.find(c_INF, pos);
            unsigned int plistIndex = 1;
            while(string::npos != pos){
                pos += strlen(c_INF);
                auto pos_end = data.find(c_INF, pos);
                string::size_type tagLen = (std::string::npos == pos_end) ? std::string::npos : pos_end - pos;
                string tag = data.substr(pos, tagLen);
                ParseChannelAndGroup(tag, plistIndex++, channels);
                pos = pos_end;
            }
        } catch (std::exception& ex) {
            XBMC->Log(LOG_ERROR, "EdemPlayer: exception during playlist loading: %s", ex.what());
            if(NULL != f) {
                XBMC->CloseFile(f);
                f = NULL;
            }
            
        }
    }
    
    static void ParseChannelAndGroup(const string& data, unsigned int plistIndex, PlaylistContent& channels)
    {
        //0,РБК-ТВ
        //#EXTGRP:новости
        //http://882406a1.iptvspy.me/iptv/9A7PTRGZWEA336/106/index.m3u8
        
        auto pos = data.find(',');
        if(string::npos == pos)
            throw BadPlaylistFormatException("Invalid channel block format: missing ','  delinmeter.");
        pos += 1;
        auto endlLine = data.find('\n');
        string name = data.substr(pos, endlLine - pos);
        rtrim(name);
        
        string tail = data.substr(endlLine + 1);
        const char* c_GROUP = "#EXTGRP:";
        pos = tail.find(c_GROUP);
        if(string::npos == pos)
            throw BadPlaylistFormatException("Invalid channel block format: missing '#EXTGRP:'  tag.");
        pos += strlen(c_GROUP);
        endlLine = tail.find('\n', pos);
        if(std::string::npos == pos)
            throw BadPlaylistFormatException("Invalid channel block format: missing NEW LINE after #EXTGRP.");
        string groupName = tail.substr(pos, endlLine - pos);
        rtrim(groupName);
        
        string url = tail.substr(++endlLine);
        rtrim(url);
        
        Channel channel;
        channel.Id = plistIndex;
        channel.Name = name;
        channel.Number = plistIndex;
        channel.Urls.push_back(url);
        channel.HasArchive = true;
        //channel.IconPath = m_logoUrl + FindVar(vars, 0, c_LOGO);
        channel.IsRadio = false;
        channels[channel.Name] = PlaylistContent::mapped_type(channel,groupName);
    }
    
    
}

