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

#include "client_core_base.hpp"
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

    class AuthFailedException : public PvrClient::ExceptionBase
    {
    };

    class MissingApiException : public PvrClient::ExceptionBase
    {
    public:
        MissingApiException(const char* r) : ExceptionBase(r) {}
    };

    class ServerErrorException : public PvrClient::ExceptionBase
    {
    public:
        ServerErrorException(const char* r, int c) : ExceptionBase(r), code(c) {}
        const int code;
    };



    class PuzzleTV : public PvrClient::ClientCoreBase
    {
    public:
        PuzzleTV(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper, bool clearEpgCache);
        ~PuzzleTV();

        const PvrClient::EpgEntryList& GetEpgList() const;
        
        void Apply(std::function<void(const ArchiveList&)>& action) const;

        void  GetEpg(PvrClient::ChannelId channelId, time_t startTime, time_t endTime, PvrClient::EpgEntryList& epgEntries);
        void  UpdateEpgForAllChannels(time_t startTime, time_t endTime);

        std::string GetUrl(PvrClient::ChannelId channelId);
        std::string GetNextStream(PvrClient::ChannelId channelId, int currentChannelIdx);

        void SetServerPort(uint16_t port) {m_serverPort = port;}
        uint16_t GetServerPort() const {return m_serverPort;}

        void SetServerUri(const char* uri) {m_serverUri = uri;}
        const std::string& GetServerUri() const {return m_serverUri;}

    protected:
        virtual void UpdateHasArchive(PvrClient::EpgEntry& entry);
        void BuildChannelAndGroupList();
    private:
        typedef std::vector<std::string> StreamerIdsList;

        struct ApiFunctionData;
        class HelperThread;
        
        std::string GetArchive(PvrClient::ChannelId channelId, time_t startTime);
        
        bool AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry);
        void LoadEpg();

        void Cleanup();

        template <typename TParser>
        void CallApiFunction(const ApiFunctionData& data, TParser parser);
        template <typename TParser, typename TCompletion>
        void CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion);
        
        void LoadArchiveList();
        void ResetArchiveList();

        void BuildRecordingsFor(PvrClient::ChannelId channelId, time_t from, time_t to);

        uint16_t m_serverPort;
        std::string m_serverUri;
        
        ArchiveList m_archiveList;
        std::string m_epgUrl;
        time_t m_lastEpgRequestStartTime;
        time_t m_lastEpgRequestEndTime;
        unsigned int m_lastUniqueBroadcastId;
        long m_serverTimeShift;
        HttpEngine* m_httpEngine;
        P8PLATFORM::CTimeout m_epgUpdateInterval;

    };
}
#endif //__puzzle_tv_h__
