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

#ifndef _ott_player_h_
#define _ott_player_h_

#include "libXBMC_pvr.h"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <list>
#include <memory>

class HttpEngine;
namespace OttEngine
{
    
    
    typedef unsigned int OttChannelId;
    
    struct OttChannel
    {
        OttChannelId Id;
        unsigned int PlistIndex;
        std::string Name;
        std::string IconPath;
        std::string UrlTemplate;
        bool HasArchive;
        
        bool operator <(const OttChannel &anotherChannel) const
        {
            return Id < anotherChannel.Id;
        }
    };
    
    
    struct OttGroup
    {
        std::set<OttChannelId> Channels;
    };
    
    struct OttEpgEntry
    {
        const char* ChannelIdName = "ch";
        OttChannelId ChannelId;
        const char* StartTimeName = "st";
        time_t StartTime;
        const char* EndTimeName = "et";
       time_t EndTime;
        
        const char* TitileName = "ti";
        std::string Title;
        const char* DescriptionName = "de";
        std::string Description;
        
        const char* HasArchiveName = "ha";
        bool HasArchive;
        
        template <class T>
        void Serialize(T& writer) const
        {
            writer.StartObject();               // Between StartObject()/EndObject(),
            writer.Key(ChannelIdName);
            writer.Uint(ChannelId);
            writer.Key(StartTimeName);
            writer.Int64(StartTime);
            writer.Key(EndTimeName);
            writer.Int64(EndTime);
            writer.Key(TitileName);
            writer.String(Title.c_str());
            writer.Key(DescriptionName);
            writer.String(Description.c_str());
            writer.Key(HasArchiveName);
            writer.Bool(HasArchive);
            writer.EndObject();
        }
        template <class T>
        void Deserialize(T& reader)
        {
            ChannelId = reader[ChannelIdName].GetUint();
            StartTime = reader[StartTimeName].GetInt64();
            EndTime = reader[EndTimeName].GetInt64();
            Title = reader[TitileName].GetString();
            Description = reader[DescriptionName].GetString();
            HasArchive = reader[HasArchiveName].GetBool();
        }
    };
    
    typedef unsigned int UniqueBroadcastIdType;
    typedef UniqueBroadcastIdType  OttArchiveEntry;
    
    typedef std::map<OttChannelId, OttChannel> ChannelList;
    typedef std::map<std::string, OttGroup> GroupList;
    typedef std::map<UniqueBroadcastIdType, OttEpgEntry> EpgEntryList;
    typedef std::map<std::string, std::string> ParamList;
    typedef std::set<OttChannelId> FavoriteList;
    typedef std::vector<std::string> StreamerNamesList;
    typedef std::set<OttArchiveEntry> ArchiveList;
    
    class OttExceptionBase : public std::exception
    {
    public:
        const char* what() const noexcept {return reason.c_str();}
        const std::string reason;
        OttExceptionBase(const std::string& r) : reason(r) {}
        OttExceptionBase(const char* r = "") : reason(r) {}
        
    };
    
    class AuthFailedException : public OttExceptionBase
    {
    };
    
    class BadPlaylistFormatException : public OttExceptionBase
    {
    public:
        BadPlaylistFormatException(const char* r) : OttExceptionBase(r) {}
    };
    
    class UnknownStreamerIdException : public OttExceptionBase
    {
    public:
        UnknownStreamerIdException() : OttExceptionBase("Unknown streamer ID.") {}
    };
    
    class MissingApiException : public OttExceptionBase
    {
    public:
        MissingApiException(const char* r) : OttExceptionBase(r) {}
    };
    
    class JsonParserException : public OttExceptionBase
    {
    public:
        JsonParserException(const std::string& r) : OttExceptionBase(r) {}
        JsonParserException(const char* r) : OttExceptionBase(r) {}
    };
    
    class ServerErrorException : public OttExceptionBase
    {
    public:
        ServerErrorException(const char* r, int c) : OttExceptionBase(r), code(c) {}
        const int code;
    };
    
    
    
    class OttPlayer
    {
    public:
        OttPlayer(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper, const std::string &baseUrl, const std::string &key);
        ~OttPlayer();
        
        const ChannelList &GetChannelList();
        const EpgEntryList& GetEpgList() const;
        
        void Apply(std::function<void(const ArchiveList&)>& action) const;
        bool StartArchivePollingWithCompletion(std::function<void(void)> action);
        
        void  GetEpg(OttChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries);
        bool FindEpg(OttChannelId brodcastId, OttEpgEntry& epgEntry);
        std::string GetArchiveForEpg(const OttEpgEntry& epgEntry);
        
        const GroupList &GetGroupList();
        std::string GetUrl(OttChannelId channelId);
        
    private:
        
        struct ApiFunctionData;
        class HelperThread;
        
//        template<class TFunc>
        void  GetEpgForAllChannels(OttChannelId channelId,  time_t startTime, time_t endTime);

        void Cleanup();
        
        template <typename TParser>
        void CallApiFunction(const ApiFunctionData& data, TParser parser);
        template <typename TParser, typename TCompletion>
        void CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion);
        
        void ParseChannelAndGroup(const std::string& data, unsigned int plistIndex);
        void LoadPlaylist();
        void ResetArchiveList();
        void Log(const char* massage) const;
        
        void LoadEpgCache();
        void SaveEpgCache();
        
        template <typename TParser>
        void ParseJson(const std::string& response, TParser parser);
        
        
        ADDON::CHelper_libXBMC_addon *m_addonHelper;
        CHelper_libXBMC_pvr *m_pvrHelper;

        std::string m_baseUrl;
        std::string m_epgUrl;
        std::string m_logoUrl;
        std::string m_key;
        ChannelList m_channelList;
        ArchiveList m_archiveList;
        GroupList m_groupList;
        EpgEntryList m_epgEntries;
        P8PLATFORM::CMutex m_epgAccessMutex;
        HelperThread* m_archiveLoader;
        HttpEngine* m_httpEngine;
    };
}
#endif //_ott_player_h_
