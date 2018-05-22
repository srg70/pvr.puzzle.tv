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

#include "pvr_client_types.h"
#include "libXBMC_pvr.h"
#include <vector>
#include <functional>
#include <list>
#include <memory>


class HttpEngine;
namespace OttEngine
{
    typedef std::map<std::string, std::string> ParamList;

    typedef PvrClient::UniqueBroadcastIdType  OttArchiveEntry;
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
    
    
    
    class OttPlayer : public PvrClient::IClientCore
    {
    public:
        OttPlayer(ADDON::CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper, const std::string &baseUrl, const std::string &key);
        ~OttPlayer();
        
        const PvrClient::ChannelList &GetChannelList();
        const PvrClient::EpgEntryList& GetEpgList() const;
        
        void Apply(std::function<void(const ArchiveList&)>& action) const;
        bool StartArchivePollingWithCompletion(std::function<void(void)> action);
        
        void  GetEpg(PvrClient::ChannelId channelId, time_t startTime, time_t endTime, PvrClient::EpgEntryList& epgEntries);
        std::string GetArchiveUrl(PvrClient::ChannelId channelId, time_t startTime, int duration);
        
        const PvrClient::GroupList &GetGroupList();
        std::string GetUrl(PvrClient::ChannelId channelId);
        
    private:
        
        struct ApiFunctionData;
        class HelperThread;
        
//        template<class TFunc>
        void  GetEpgForAllChannels(PvrClient::ChannelId channelId,  time_t startTime, time_t endTime);

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
        PvrClient::ChannelList m_channelList;
        ArchiveList m_archiveList;
        PvrClient::GroupList m_groupList;
        PvrClient::EpgEntryList m_epgEntries;
        P8PLATFORM::CMutex m_epgAccessMutex;
        HelperThread* m_archiveLoader;
        HttpEngine* m_httpEngine;
    };
}
#endif //_ott_player_h_
