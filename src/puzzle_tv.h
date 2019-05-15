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
#include <queue>
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


    typedef enum {
        c_EpgType_File = 0,
        c_EpgType_Server = 1
    } EpgType;

    typedef enum {
        c_PuzzleServer2 = 0,
        c_PuzzleServer3 = 1
    } ServerVersion;
    
    class PuzzleTV : public PvrClient::ClientCoreBase
    {
    public:
        
        struct PuzzleSource
        {
            typedef std::map<std::string, bool> TStreamsQuality;
            
            PuzzleSource()
            : RatingGood(0)
            , RatingBad(0)
            , IsServerOn (true)
            , IsChannelLocked (false)
            , Priority (0)
            , Id (0)
            {}
            
            int64_t RatingGood;
            int64_t RatingBad;
            bool IsServerOn;
            bool IsChannelLocked;
            int Id;
            int Priority;
            std::string Server;
            TStreamsQuality Streams;
            
            bool IsOn() const {return IsServerOn && !IsChannelLocked; }
            bool CanBeOn() const {return IsServerOn && IsChannelLocked; }
            bool IsEmpty() const {return Streams.size() == 0; }
        };
        typedef std::string TCacheUrl;

        typedef std::map<TCacheUrl, PuzzleSource> TChannelSources;
        typedef const TChannelSources::value_type* TPrioritizedSource;
        
        struct PriorityComparator{
          bool operator()(const TPrioritizedSource& left, const TPrioritizedSource& right) { return left->second.Priority > right->second.Priority; }
        };
        typedef std::priority_queue<TPrioritizedSource, std::vector<TPrioritizedSource>, PriorityComparator >  TPrioritizedSources;

        
        PuzzleTV(ServerVersion serverVersion, const char* serverUrl, uint16_t serverPort);
        ~PuzzleTV();

        const PvrClient::EpgEntryList& GetEpgList() const;
        void  UpdateEpgForAllChannels(time_t startTime, time_t endTime);

        std::string GetUrl(PvrClient::ChannelId channelId);
        std::string GetNextStream(PvrClient::ChannelId channelId, int currentChannelIdx);
        void RateStream(PvrClient::ChannelId channelId, const std::string& streamUrl, bool isGood);
        void OnOpenStremFailed(PvrClient::ChannelId channelId, const std::string& streamUrl);

        void SetMaxServerRetries(int maxServerRetries) {m_maxServerRetries = maxServerRetries;}
        void SetEpgParams(EpgType epgType, const std::string& epgUrl, uint16_t serverPort) {
            
            if(m_serverVersion == c_PuzzleServer3 && epgType == c_EpgType_Server) {
                // Override settings for Puzzle 3 server (like a file)
                m_epgUrl = EpgUrlForPuzzle3();
                m_epgType = c_EpgType_File;
            } else {
                m_epgUrl = epgUrl;
                m_epgType = epgType;
             }
            m_epgServerPort = serverPort;
        }
        TPrioritizedSources GetSourcesForChannel(PvrClient::ChannelId channelId);
        void EnableSource(PvrClient::ChannelId channelId, const TCacheUrl& source);
        void DisableSource(PvrClient::ChannelId channelId, const TCacheUrl& source);
        void UpdateChannelSources(PvrClient::ChannelId channelId);
    protected:
        void Init(bool clearEpgCache);
        virtual void UpdateHasArchive(PvrClient::EpgEntry& entry);
        void BuildChannelAndGroupList();

    private:
        typedef std::map<PvrClient::ChannelId, TChannelSources> TChannelSourcesMap;

        struct ApiFunctionData;
        PvrClient::UniqueBroadcastIdType AddXmlEpgEntry(const XMLTV::EpgEntry& xmlEpgEntry);
        void LoadEpg();
        void UpdateArhivesAsync();
        
        bool CheckChannelId(PvrClient::ChannelId channelId);
        void UpdateUrlsForChannel(PvrClient::ChannelId channelId);
        void Cleanup();

        template <typename TParser>
        void CallApiFunction(const ApiFunctionData& data, TParser parser);
        template <typename TParser, typename TCompletion>
        void CallApiAsync(const ApiFunctionData& data, TParser parser, TCompletion completion);
        template <typename TParser, typename TCompletion>
        void CallApiAsync(const std::string& strRequest, const std::string& name, TParser parser, TCompletion completion);

        bool CheckAceEngineRunning(const char* aceServerUrlBase);
        std::string EpgUrlForPuzzle3() const;
        void GetSourcesMetadata(TChannelSources& channelSources);

        const uint16_t m_serverPort;
        const std::string m_serverUri;
        uint16_t m_epgServerPort;
        EpgType m_epgType;
        int m_maxServerRetries;

        std::string m_epgUrl;

        std::map<PvrClient::ChannelId, PvrClient::ChannelId> m_epgToServerLut;
        const ServerVersion m_serverVersion;

        TChannelSourcesMap m_sources;
        
        bool m_isAceRunning;
    };
}
#endif //__puzzle_tv_h__
