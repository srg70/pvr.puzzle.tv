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

#ifndef _ttv_player_h_
#define _ttv_player_h_

#include "client_core_base.hpp"
#include <vector>
#include <functional>
#include <list>
#include <set>
#include <map>
#include <memory>
#include "p8-platform/util/timeutils.h"
#include "p8-platform/threads/mutex.h"
#include "ActionQueue.hpp"
#include "helpers.h"
#include <rapidjson/document.h>

namespace XMLTV {
    struct EpgEntry;
    struct EpgChannel;
}

namespace TtvEngine
{
    typedef std::map<std::string, std::string> ParamList;
    
    class ServerErrorException : public PvrClient::ExceptionBase
    {
    public:
        ServerErrorException(const char* code) : ExceptionBase(code) {}
    };
    
    class IoErrorException : public PvrClient::ExceptionBase
    {
    public:
        IoErrorException(const char* code) : ExceptionBase(code) {}
    };
    
    class Core : public PvrClient::ClientCoreBase
    {
    public:
        struct CoreParams {
            CoreParams()
            : enableAdult(false)
            , aceServerPort(0)
            , filterByAlexElec (true)
            {}
            bool enableAdult;
            bool filterByAlexElec;
            std::string aceServerUri;
            int aceServerPort;
            std::string epgUrl;
            std::string AceServerUrlBase() const
            {
                return std::string ("http://") + aceServerUri +":" + n_to_string(aceServerPort);
            }
        };
        Core(const CoreParams& coreParams);
        ~Core();
        
        void UpdateEpgForAllChannels(time_t startTime, time_t endTime, std::function<bool(void)> cancelled);

        std::string GetUrl(PvrClient::ChannelId channelId);
        std::string GetNextStream(PvrClient::ChannelId channelId, int currentChannelIdx);

        void ClearSession();
    protected:
        virtual void Init(bool clearEpgCache);
        virtual void UpdateHasArchive(PvrClient::EpgEntry& entry) { entry.HasArchive = false; }
        virtual void BuildChannelAndGroupList();

    private:
        
        typedef int EpgChannelId;
//        struct TTVChanel {
//            EpgChannelId epg_id;
//            std::string fname;
//            std::string update_date;
//            std::string cid;
//            std::string name;
//            std::string category;
//            std::string source;
//            //"tracker": "hls:9.rarbg.me",
//            std::string provider;
//        };
//
//        typedef std::map<PvrClient::ChannelId, TTVChanel> TTVChannels;
//        TTVChannels m_ttvChannels;
        
        std::map<EpgChannelId, PvrClient::ChannelId> m_epgIdToChannelId;
        
        const CoreParams m_coreParams;
        std::string m_deviceId;

        // Epg management
        void LoadEpg(std::function<bool(void)> cancelled);
        void ScheduleEpgDetails();
//        PvrClient::UniqueBroadcastIdType AddEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry);
//        PvrClient::UniqueBroadcastIdType AddEpgEntry(PvrClient::EpgEntry& epg);

        bool CheckAceEngineRunning();

        // TSProxy plist
        void BuildChannelAndGroupList_Plist();
        void UpdateEpgForAllChannels_Plist(time_t startTime, time_t endTime, std::function<bool(void)> cancelled);
        void LoadPlaylist(std::function<void(const rapidjson::Document::ValueType&)> onChannel);
        struct  AlexElecChannel {
            std::string name;
            std::string cid;
            std::string cat;
        };
        void LoadPlaylistAlexelec(std::function<void(const AlexElecChannel &)> onChannel);
        

        void Cleanup();

        bool m_isAceRunning;
   };
}
#endif //_ttv_player_h_
