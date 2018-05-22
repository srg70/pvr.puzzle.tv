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

#include "pvr_client_types.h"
#include "libXBMC_pvr.h"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <list>
#include <memory>
#include "ActionQueue.hpp"

class HttpEngine;

typedef std::map<std::string, std::string> ParamList;
typedef std::vector<std::string> StreamerNamesList;

typedef PvrClient::UniqueBroadcastIdType  SovokArchiveEntry;
typedef std::map<PvrClient::ChannelId , int> SovokArchivesInfo;

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

class MissingHttpsSupportException : public SovokExceptionBase
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



class SovokTV : public PvrClient::IClientCore
{
public:
    typedef struct {
        bool Hidden;
        std::string GroupName;
        std::string FilterPattern;
    }CountryTemplate;
    
    typedef struct {
        bool IsOn;
        std::vector<CountryTemplate> Filters;
    } CountryFilter;
    
    SovokTV(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
            const std::string &login, const std::string &password,
            const std::function<void(void)>& archiveUpdateDone = nullptr, bool cleanEpgCache = false);
    ~SovokTV();

    const PvrClient::ChannelList &GetChannelList();
    const PvrClient::GroupList &GetGroupList();
    
    const PvrClient::EpgEntryList& GetEpgList() const;
    const StreamerNamesList& GetStreamersList() const;
    
    void ForEach(std::function<void(const SovokArchiveEntry&)>& action) const;
    
    void GetEpg(PvrClient::ChannelId  channelId, time_t startTime, time_t endTime, PvrClient::EpgEntryList& epgEntries);
    void GetEpgForAllChannels(time_t startTime, time_t endTime);

    std::string GetUrl(PvrClient::ChannelId  channelId);
    std::string GetArchiveUrl(PvrClient::ChannelId  channelId, time_t startTime);

    PvrClient::FavoriteList GetFavorites();

    int GetSreamerId() const { return m_streammerId; }
    void SetStreamerId(int streamerId);
    
    void SetPinCode(const std::string& code) {m_pinCode = code;}
    void SetCountryFilter(const CountryFilter& filter);
        
    void OnEpgUpdateDone();
private:
    typedef std::vector<std::string> StreamerIdsList;
    typedef std::function<void(const CActionQueue::ActionResult&)> TApiCallCompletion;
    
    struct ApiFunctionData;
        
    void GetEpgForAllChannelsForNHours(time_t startTime, short numberOfHours);
    void AddEpgEntry(PvrClient::UniqueBroadcastIdType id, PvrClient::EpgEntry& entry);
    void UpdateHasArchive(PvrClient::EpgEntry& entry) const;

    bool Login(bool wait);
    void Logout();
    void Cleanup();
    
    template <typename TParser>
    void CallApiFunction(const ApiFunctionData& data, TParser parser);
    template <typename TParser>
    void CallApiAsync(const ApiFunctionData& data, TParser parser, TApiCallCompletion completion);
    
    void BuildChannelAndGroupList();
    void LoadSettings();
    void InitArchivesInfo();
    bool LoadStreamers();
    void Log(const char* massage) const;

    void LoadEpgCache();
    void SaveEpgCache();

    template <typename TParser>
    void ParseJson(const std::string& response, TParser parser);

    
    ADDON::CHelper_libXBMC_addon *m_addonHelper;
    CHelper_libXBMC_pvr *m_pvrHelper;

    std::string m_login;
    std::string m_password;
    
    PvrClient::ChannelList m_channelList;
    SovokArchivesInfo  m_archivesInfo;
    PvrClient::GroupList m_groupList;
    struct CountryFilterPrivate : public CountryFilter {
        CountryFilterPrivate() {IsOn = false;}
        CountryFilterPrivate(const CountryFilter& filter)
        : CountryFilter (filter)
        {
            Groups.resize(Filters.size());
        }
        CountryFilterPrivate& operator=(const CountryFilter& filter)
        {
           return *this = CountryFilterPrivate(filter);
        }
        std::vector<PvrClient::GroupList::key_type>  Groups;
    }m_countryFilter;
    
    PvrClient::EpgEntryList m_epgEntries;
    
    time_t m_lastEpgRequestEndTime;
    int m_streammerId;
    long m_serverTimeShift;
    StreamerNamesList m_streamerNames;
    StreamerIdsList m_streamerIds;
    mutable P8PLATFORM::CMutex m_epgAccessMutex;
    unsigned int m_epgActivityCounter;
    std::function<void(void)> m_archiveUpdateDone;
    std::string m_pinCode;
    HttpEngine* m_httpEngine;
};

#endif //sovok_tv_h
