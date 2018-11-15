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
#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <list>
#include <memory>
#include "p8-platform/util/timeutils.h"


namespace XMLTV {
    struct EpgEntry;
    struct EpgChannel;
}

namespace PuzzleEngine
{

    typedef std::map<std::string, std::string> ParamList;

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
        PuzzleTV(const char* serverUrl, int serverPort);
        ~PuzzleTV();

        const PvrClient::EpgEntryList& GetEpgList() const;
        void  UpdateEpgForAllChannels(time_t startTime, time_t endTime);

        std::string GetUrl(PvrClient::ChannelId channelId);
        std::string GetNextStream(PvrClient::ChannelId channelId, int currentChannelIdx);

        void SetMaxServerRetries(int maxServerRetries) {m_maxServerRetries = maxServerRetries;}
        void SetEpgUrl(const std::string& epgUrl) {m_epgUrl = epgUrl;}
    protected:
        void Init(bool clearEpgCache);
        virtual void UpdateHasArchive(PvrClient::EpgEntry& entry);
        void BuildChannelAndGroupList();

    private:
        typedef std::vector<std::string> StreamerIdsList;

        struct ApiFunctionData;
        bool AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry);
        void LoadEpg();
        void UpdateArhivesAsync();

        void Cleanup();

        template <typename TParser>
        void CallApiFunction(const ApiFunctionData& data, TParser parser);
        template <typename TParser, typename TCompletion>
        void CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion);

        const uint16_t m_serverPort;
        const std::string m_serverUri;
        int m_maxServerRetries;

        std::string m_epgUrl;
        long m_serverTimeShift;
        P8PLATFORM::CTimeout m_epgUpdateInterval;
        std::map<PvrClient::ChannelId, PvrClient::ChannelId> m_epgToServerLut;

    };
}
#endif //__puzzle_tv_h__
