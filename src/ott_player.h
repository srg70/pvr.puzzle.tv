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

#include "client_core_base.hpp"
#include "libXBMC_pvr.h"
#include <vector>
#include <functional>
#include <list>
#include <memory>


class HttpEngine;
namespace OttEngine
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
    
    class UnknownStreamerIdException : public PvrClient::ExceptionBase
    {
    public:
        UnknownStreamerIdException() : ExceptionBase("Unknown streamer ID.") {}
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
    
    
    
    class OttPlayer : public PvrClient::ClientCoreBase
    {
    public:
        OttPlayer(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper, const std::string &baseUrl, const std::string &key, bool clearEpgCache);
        ~OttPlayer();
        
        void  GetEpg(PvrClient::ChannelId channelId, time_t startTime, time_t endTime, PvrClient::EpgEntryList& epgEntries);
        void UpdateEpgForAllChannels(time_t startTime, time_t endTime);
       std::string GetArchiveUrl(PvrClient::ChannelId channelId, time_t startTime, int duration);
        
        std::string GetUrl(PvrClient::ChannelId channelId);
        
    protected:
        virtual void UpdateHasArchive(PvrClient::EpgEntry& entry);
        virtual void BuildChannelAndGroupList();

    private:
        
        struct ApiFunctionData;
        
        void  GetEpgForChannel(PvrClient::ChannelId channelId,  time_t startTime, time_t endTime);

        void Cleanup();
        
        template <typename TParser>
        void CallApiFunction(const ApiFunctionData& data, TParser parser);
        template <typename TParser, typename TCompletion>
        void CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion);
        
        void ParseChannelAndGroup(const std::string& data, unsigned int plistIndex);
        
        std::string m_baseUrl;
        std::string m_epgUrl;
        std::string m_logoUrl;
        std::string m_key;
        HttpEngine* m_httpEngine;
        unsigned int m_epgActivityCounter;
    };
}
#endif //_ott_player_h_
