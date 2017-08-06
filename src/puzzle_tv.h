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

#ifndef __puzzle_tv_h__
#define __puzzle_tv_h__

#include "libXBMC_pvr.h"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <list>
#include <memory>

class HttpEngine;

namespace PuzzleEngine
{

    typedef unsigned int ChannelId;
    struct Channel
    {
        typedef std::vector<std::string> UrlList;
        ChannelId Id;
        std::string Name;
        std::string IconPath;
        bool IsRadio;
        UrlList Urls;
        
        bool operator <(const Channel &anotherChannel) const
        {
            return Id < anotherChannel.Id;
        }
    };


    struct Group
    {
        std::set<ChannelId> Channels;
    };

    //struct EpgCaheEntry
    //{
    //    int ChannelId;
    //    unsigned int UniqueBroadcastId;
    //    time_t StartTime;
    //    time_t EndTime;
    //};
    struct EpgEntry
    {
        ChannelId channelId;
        time_t StartTime;
        time_t EndTime;

        std::string Title;
        std::string Description;
    };

    typedef unsigned int UniqueBroadcastIdType;
    typedef UniqueBroadcastIdType  ArchiveEntry;

    struct EpgCaheEntry
    {
        EpgCaheEntry(ChannelId channelId, time_t startTime)
            : channelId(channelId)
            , StartTime(startTime)
        {}

        const int channelId;
        const time_t StartTime;
    };

    typedef std::map<int, Channel> ChannelList;
    typedef std::map<std::string, Group> GroupList;
    typedef std::map<UniqueBroadcastIdType, EpgEntry> EpgEntryList;
    //typedef std::vector<EpgCaheEntry> EpgCache;
    typedef std::map<std::string, std::string> ParamList;
    typedef std::set<int> FavoriteList;
    typedef std::vector<std::string> StreamerNamesList;
    typedef std::set<ArchiveEntry> ArchiveList;

    class ExceptionBase : public std::exception
    {
    public:
        const char* what() const noexcept {return reason.c_str();}
        const std::string reason;
        ExceptionBase(const std::string& r) : reason(r) {}
        ExceptionBase(const char* r = "") : reason(r) {}

    };

    class AuthFailedException : public ExceptionBase
    {
    };

    class BadSessionIdException : public ExceptionBase
    {
    public:
        BadSessionIdException() : ExceptionBase("Session ID es empty.") {}
    };

    class UnknownStreamerIdException : public ExceptionBase
    {
    public:
        UnknownStreamerIdException() : ExceptionBase("Unknown streamer ID.") {}
    };

    class MissingApiException : public ExceptionBase
    {
    public:
        MissingApiException(const char* r) : ExceptionBase(r) {}
    };

    class JsonParserException : public ExceptionBase
    {
    public:
        JsonParserException(const std::string& r) : ExceptionBase(r) {}
        JsonParserException(const char* r) : ExceptionBase(r) {}
    };

    class ServerErrorException : public ExceptionBase
    {
    public:
        ServerErrorException(const char* r, int c) : ExceptionBase(r), code(c) {}
        const int code;
    };



    class PuzzleTV
    {
    public:
        PuzzleTV(ADDON::CHelper_libXBMC_addon *addonHelper);
        ~PuzzleTV();

        const ChannelList &GetChannelList();
        const EpgEntryList& GetEpgList() const;
        
        void Apply(std::function<void(const ArchiveList&)>& action) const;
        bool StartArchivePollingWithCompletion(std::function<void(void)> action);

        
        //EpgEntryList GetEpg(int channelId, time_t day);
        void  GetEpg(ChannelId channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries);
        void GetEpgForAllChannels(time_t startTime, time_t endTime, EpgEntryList& epgEntries);
        bool FindEpg(unsigned int brodcastId, Group& epgEntry);
        std::string GetArchiveForEpg(const Group& epgEntry);

        const GroupList &GetGroupList();
        std::string GetUrl(ChannelId channelId);
        std::string GetNextStream(ChannelId channelId, int currentChannelIdx);

        void SetServerPort(uint16_t port) {m_serverPort = port;}
        uint16_t GetServerPort() const {return m_serverPort;}

        void SetServerUri(const char* uri) {m_serverUri = uri;}
        const std::string& GetServerUri() const {return m_serverUri;}

    private:
        typedef std::vector<std::string> StreamerIdsList;

        struct ApiFunctionData;
        class HelperThread;
        
        std::string GetArchive(ChannelId channelId, time_t startTime);
        
        template<class TFunc>
        void  GetEpgForAllChannelsForNHours(time_t startTime, short numberOfHours, TFunc func);
        void Cleanup();
        
        template <typename TParser>
        void CallApiFunction(const ApiFunctionData& data, TParser parser);
        template <typename TParser, typename TCompletion>
        void CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion);
        
        void BuildChannelAndGroupList();
        ChannelList &GetMutableChannelList();

        
        void LoadArchiveList();
        void ResetArchiveList();
        void Log(const char* massage) const;

        void LoadEpgCache();
        void SaveEpgCache();

        void BuildRecordingsFor(ChannelId channelId, time_t from, time_t to);

        template <typename TParser>
        void ParseJson(const std::string& response, TParser parser);

        
        uint16_t m_serverPort;
        std::string m_serverUri;
        
        ADDON::CHelper_libXBMC_addon *m_addonHelper;
        ChannelList m_channelList;
        ArchiveList m_archiveList;
        GroupList m_groupList;
        EpgEntryList m_epgEntries;
        time_t m_lastEpgRequestStartTime;
        time_t m_lastEpgRequestEndTime;
        unsigned int m_lastUniqueBroadcastId;
        long m_serverTimeShift;
        P8PLATFORM::CMutex m_epgAccessMutex;
        HelperThread* m_archiveLoader;
        HttpEngine* m_httpEngine;
    };
}
#endif //__puzzle_tv_h__