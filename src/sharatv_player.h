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

#ifndef _shara_tv_player_h_
#define _shara_tv_player_h_

#include "client_core_base.hpp"
#include <vector>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include "p8-platform/util/timeutils.h"


namespace XMLTV {
    struct EpgEntry;
    struct EpgChannel;
}

namespace SharaTvEngine
{
    typedef std::map<std::string, std::string> ParamList;

    class AuthFailedException : public PvrClient::ExceptionBase
    {
    };
    
    class BadPlaylistFormatException : public PvrClient::ExceptionBase
    {
    public:
        BadPlaylistFormatException(const char* r) : ExceptionBase(r) {}
    };
    
    class ServerErrorException : public PvrClient::ExceptionBase
    {
    public:
        ServerErrorException(const char* r, int c) : ExceptionBase(r), code(c) {}
        const int code;
    };
    
    struct ArchiveInfo{
        ArchiveInfo(unsigned int d, const std::string& t)
        : days(d)
        , urlTemplate(t)
        {}
        ArchiveInfo(const ArchiveInfo & rf)
        : ArchiveInfo(rf.days, rf.urlTemplate)
        {}
        ArchiveInfo(ArchiveInfo && rv)
        : ArchiveInfo(rv.days, rv.urlTemplate)
        {}
        const unsigned int days;
        const std::string urlTemplate;
    };

    struct ArchiveInfos {
        ArchiveInfos() : archiveDays(3) {}
        void Reset() {
            archiveDays = 3;
            info.clear();
        }
        std::map<PvrClient::ChannelId, ArchiveInfo> info;
        unsigned int archiveDays;
    };
    
    struct GloabalTags
    {
        GloabalTags() : m_catchupDays(0) {}
        std::string m_epgUrl;
        std::string m_catchupType;
        std::string m_catchupSource;
        unsigned long m_catchupDays;

    };

    class Core : public PvrClient::ClientCoreBase
    {
    public:
        Core(const std::string &playlistUrl,  const std::string &epgUrl, bool enableAdult);
        ~Core();
        
        std::string GetArchiveUrl(PvrClient::ChannelId channelId, time_t startTime, time_t duration);
        void UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled);
        // -1 to remove limitation
        void SetMaxArchiveDuration(int duration) { m_maxArchiveDuration = duration; }

        std::string GetUrl(PvrClient::ChannelId channelId);
    protected:
        virtual void Init(bool clearEpgCache);
        virtual void UpdateHasArchive(PvrClient::EpgEntry& entry);
        virtual void BuildChannelAndGroupList();

    private:
        void LoadEpg(std::function<bool(void)> cancelled);
//        bool AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry);

        void Cleanup();

        std::string m_playListUrl;
        std::string m_epgUrl;
        GloabalTags m_globalTags;
        ArchiveInfos m_archiveInfo;
        bool m_enableAdult;
        // Shara TV supports archive length up to 2 hours
        // Split EPG items longer than m_maxArchiveDuration to avoid this limitation
        int m_maxArchiveDuration;
    };
}
#endif //_shara_tv_player_h_
