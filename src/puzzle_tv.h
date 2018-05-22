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

#include "pvr_client_types.h"
#include "libXBMC_pvr.h"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <list>
#include <memory>
#include "p8-platform/util/timeutils.h"

class HttpEngine;

namespace XMLTV {
    struct EpgEntry;
    struct EpgChannel;
}

namespace PuzzleEngine
{

    typedef std::map<std::string, std::string> ParamList;

    typedef PvrClient::UniqueBroadcastIdType  ArchiveEntry;
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



    class PuzzleTV : public PvrClient::IClientCore
    {
    public:
        PuzzleTV(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper);
        ~PuzzleTV();

        const PvrClient::ChannelList &GetChannelList();
        const PvrClient::GroupList &GetGroupList();
       
        const PvrClient::EpgEntryList& GetEpgList() const;
        
        void Apply(std::function<void(const ArchiveList&)>& action) const;
        bool StartArchivePollingWithCompletion(std::function<void(void)> action);

        void  GetEpg(PvrClient::ChannelId channelId, time_t startTime, time_t endTime, PvrClient::EpgEntryList& epgEntries);

        std::string GetUrl(PvrClient::ChannelId channelId);
        std::string GetNextStream(PvrClient::ChannelId channelId, int currentChannelIdx);

        void SetServerPort(uint16_t port) {m_serverPort = port;}
        uint16_t GetServerPort() const {return m_serverPort;}

        void SetServerUri(const char* uri) {m_serverUri = uri;}
        const std::string& GetServerUri() const {return m_serverUri;}

    private:
        typedef std::vector<std::string> StreamerIdsList;

        struct ApiFunctionData;
        class HelperThread;
        
        std::string GetArchive(PvrClient::ChannelId channelId, time_t startTime);
        
        bool AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry);
        void  UpdateEpgForAllChannels(PvrClient::ChannelId channelId,  time_t startTime, time_t endTime);
        void LoadEpg();

        void Cleanup();
        
              
        template <typename TParser>
        void CallApiFunction(const ApiFunctionData& data, TParser parser);
        template <typename TParser, typename TCompletion>
        void CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion);
        
        void BuildChannelAndGroupList();
        PvrClient::ChannelList &GetMutableChannelList();

        
        void LoadArchiveList();
        void ResetArchiveList();
        void Log(const char* massage) const;

        void LoadEpgCache();
        void SaveEpgCache();

        void BuildRecordingsFor(PvrClient::ChannelId channelId, time_t from, time_t to);

        template <typename TParser>
        void ParseJson(const std::string& response, TParser parser);

        
        uint16_t m_serverPort;
        std::string m_serverUri;
        
        ADDON::CHelper_libXBMC_addon *m_addonHelper;
        CHelper_libXBMC_pvr *m_pvrHelper;

        PvrClient::ChannelList m_channelList;
        ArchiveList m_archiveList;
        PvrClient::GroupList m_groupList;
        std::string m_epgUrl;
        PvrClient::EpgEntryList m_epgEntries;
        time_t m_lastEpgRequestStartTime;
        time_t m_lastEpgRequestEndTime;
        unsigned int m_lastUniqueBroadcastId;
        long m_serverTimeShift;
        P8PLATFORM::CMutex m_epgAccessMutex;
        HelperThread* m_archiveLoader;
        HttpEngine* m_httpEngine;
        P8PLATFORM::CTimeout m_epgUpdateInterval;

    };
}
#endif //__puzzle_tv_h__
