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

#ifndef sovok_tv_h
#define sovok_tv_h

#include "libXBMC_pvr.h"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <list>
#include "ActionQueue.hpp"

struct SovokChannel
{
    int Id;
    std::string Name;
    std::string IconPath;
    bool IsRadio;

    bool operator <(const SovokChannel &anotherChannel) const
    {
        return Id < anotherChannel.Id;
    }
};


struct SovokGroup
{
    std::set<int> Channels;
};

//struct SovokEpgCaheEntry
//{
//    int ChannelId;
//    unsigned int UniqueBroadcastId;
//    time_t StartTime;
//    time_t EndTime;
//};
struct SovokEpgEntry
{
    int ChannelId;
    time_t StartTime;
    time_t EndTime;

    std::string Title;
    std::string Description;
};

typedef unsigned int UniqueBroadcastIdType;
typedef UniqueBroadcastIdType  SovokArchiveEntry;

struct SovokEpgCaheEntry
{
    SovokEpgCaheEntry(int channelId, time_t startTime)
        : ChannelId(channelId)
        , StartTime(startTime)
    {}

    const int ChannelId;
    const time_t StartTime;
};

typedef std::map<int, SovokChannel> ChannelList;
typedef std::map<std::string, SovokGroup> GroupList;
typedef std::map<UniqueBroadcastIdType, SovokEpgEntry> EpgEntryList;
//typedef std::vector<SovokEpgCaheEntry> EpgCache;
typedef std::map<std::string, std::string> ParamList;
typedef std::set<int> FavoriteList;
typedef std::vector<std::string> StreamerNamesList;
typedef std::set<SovokArchiveEntry> ArchiveList;

class SovokExceptionBase : public std::exception
{
public:
    const char* what() const noexcept {return reason.c_str();}
    const std::string reason;
    SovokExceptionBase(const std::string& r) : reason(r) {}
    SovokExceptionBase(const char* r = "") : reason(r) {}

};

class AuthFailedException : public SovokExceptionBase
{
};

class BadSessionIdException : public SovokExceptionBase
{
public:
    BadSessionIdException() : SovokExceptionBase("Session ID es empty.") {}
};

class UnknownStreamerIdException : public SovokExceptionBase
{
public:
    UnknownStreamerIdException() : SovokExceptionBase("Unknown streamer ID.") {}
};

class MissingApiException : public SovokExceptionBase
{
public:
    MissingApiException(const char* r) : SovokExceptionBase(r) {}
};

class JsonParserException : public SovokExceptionBase
{
public:
    JsonParserException(const std::string& r) : SovokExceptionBase(r) {}
    JsonParserException(const char* r) : SovokExceptionBase(r) {}
};

class ServerErrorException : public SovokExceptionBase
{
public:
    ServerErrorException(const char* r, int c) : SovokExceptionBase(r), code(c) {}
    const int code;
};

class QueueNotRunningException : public SovokExceptionBase
{
public:
    QueueNotRunningException(const char* r) : SovokExceptionBase(r) {}
};


class CActionQueue;

class SovokTV
{
public:
    SovokTV(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &login, const std::string &password);
    ~SovokTV();

    const ChannelList &GetChannelList();
    const EpgEntryList& GetEpgList() const;
    const StreamerNamesList& GetStreamersList() const;
    
    void Apply(std::function<void(const ArchiveList&)>& action) const;
    bool StartArchivePollingWithCompletion(std::function<void(void)> action);

    
    //EpgEntryList GetEpg(int channelId, time_t day);
    void  GetEpg(int channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries);
    void GetEpgForAllChannels(time_t startTime, time_t endTime, EpgEntryList& epgEntries);
    bool FindEpg(unsigned int brodcastId, SovokEpgEntry& epgEntry);
    std::string GetArchiveForEpg(const SovokEpgEntry& epgEntry);

    const GroupList &GetGroupList();
    std::string GetUrl(int channelId);
    FavoriteList GetFavorites();

    int GetSreamerId() const { return m_streammerId; }
    void SetStreamerId(int streamerId);

private:
    typedef std::vector<std::string> StreamerIdsList;

    struct ApiFunctionData;
    class HelperThread;
    
    std::string GetArchive(int channelId, time_t startTime);
    
    template<class TFunc>
    void  GetEpgForAllChannelsForNHours(time_t startTime, short numberOfHours, TFunc func);
    bool Login(bool wait);
    void Logout();
    void Cleanup();
    
    template <typename TParser>
    void CallApiFunction(std::shared_ptr<const ApiFunctionData> data, TParser parser);
    template <typename TParser, typename TCompletion>
    void CallApiAsync(std::shared_ptr<const ApiFunctionData> data, TParser parser, TCompletion completion);
    template <typename TResultCallback, typename TCompletion>
    void SendHttpRequest(const std::string &url,const ParamList &cookie, TResultCallback result, TCompletion completion) const;
    
    static size_t CurlWriteData(void *buffer, size_t size, size_t nmemb, void *userp);
    void BuildChannelAndGroupList();
    void LoadSettings();
    void LoadArchiveList();
    void ResetArchiveList();
    bool LoadStreamers();
    void Log(const char* massage) const;

    void LoadEpgCache();
    void SaveEpgCache();

    void BuildRecordingsFor(int channelId, time_t from, time_t to);

    template <typename TParser>
    void ParseJson(const std::string& response, TParser parser);

    
    ADDON::CHelper_libXBMC_addon *m_addonHelper;
    std::string m_login;
    std::string m_password;
    ParamList m_sessionCookie;
    ChannelList m_channelList;
    ArchiveList m_archiveList;
    GroupList m_groupList;
    EpgEntryList m_epgEntries;
    time_t m_lastEpgRequestStartTime;
    time_t m_lastEpgRequestEndTime;
    unsigned int m_lastUniqueBroadcastId;
    int m_streammerId;
    long m_serverTimeShift;
    StreamerNamesList m_streamerNames;
    StreamerIdsList m_streamerIds;
    CActionQueue* m_apiCalls;
    CActionQueue* m_apiCallCompletions;
    P8PLATFORM::CMutex m_epgAccessMutex;
    HelperThread* m_archiveLoader;
};

#endif //sovok_tv_h
