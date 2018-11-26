/*
*
*   Copyright (C) 2017 Sergey Shramchenko
*   https://github.com/srg70/pvr.puzzle.tv
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

#ifndef pvr_client_types_h
#define pvr_client_types_h

#include <string>
#include <map>
#include <set>
#include <vector>
#include <vector>
#include <functional>
#include "ActionQueueTypes.hpp"
#include <rapidjson/document.h>

namespace PvrClient {
    
    typedef unsigned int ChannelId;
    const ChannelId UnknownChannelId = (ChannelId) -1;
    struct Channel
    {
        typedef std::vector<std::string> UrlList;
        
        ChannelId Id;
        unsigned int Number;
        std::string Name;
        std::string IconPath;
        UrlList Urls;
        bool HasArchive;
        bool IsRadio;
       
        bool operator <(const Channel &anotherChannel) const
        {
            return Id < anotherChannel.Id;
        }
    };
    
    
    struct Group
    {
        std::string Name;
        std::set<ChannelId> Channels;
    };
    
    typedef std::map<ChannelId, Channel> ChannelList;
    typedef int GroupId;
    typedef std::map<GroupId, Group> GroupList;
    typedef std::set<ChannelId> FavoriteList;

    
    struct EpgEntry
    {
        EpgEntry()
        : ChannelId(-1)
        , StartTime(0)
        , EndTime(0)
        , HasArchive (false)
        {}
        const char* ChannelIdName = "ch";
        PvrClient::ChannelId ChannelId;
        
        const char* StartTimeName = "st";
        time_t StartTime;
        
        const char* EndTimeName = "et";
        time_t EndTime;
        
        const char* TitileName = "ti";
        std::string Title;
        
        const char* DescriptionName = "de";
        std::string Description;
        
        const char* HasArchiveName = "ha";
        bool HasArchive;
        
        const char* IconPathName = "ic";
        std::string IconPath;
        
        template <class T>
        void Serialize(T& writer) const
        {
            writer.StartObject();               // Between StartObject()/EndObject(),
            writer.Key(ChannelIdName);
            writer.Uint(ChannelId);
            writer.Key(StartTimeName);
            writer.Int64(StartTime);
            writer.Key(EndTimeName);
            writer.Int64(EndTime);
            writer.Key(TitileName);
            writer.String(Title.c_str());
            writer.Key(DescriptionName);
            writer.String(Description.c_str());
            writer.Key(HasArchiveName);
            writer.Bool(HasArchive);
            writer.Key(IconPathName);
            writer.String(IconPath.c_str());

            writer.EndObject();
        }
        template <class T>
        void Deserialize(T& reader)
        {
            ChannelId = reader[ChannelIdName].GetUint();
            StartTime = reader[StartTimeName].GetInt64();
            EndTime = reader[EndTimeName].GetInt64();
            Title = reader[TitileName].GetString();
            Description = reader[DescriptionName].GetString();
            HasArchive = reader[HasArchiveName].GetBool();
        }
    };
    
    typedef unsigned int UniqueBroadcastIdType;
    typedef std::map<UniqueBroadcastIdType, EpgEntry> EpgEntryList;

    class IClientCorePhase {
        virtual void Wait() = 0;
        virtual bool IsDone() = 0;
        virtual ~IClientCorePhase(){}
    };
    
    class IClientCore
    {
    public:
        enum Phase{
            k_ChannelsLoadingPhase,
            k_InitPhase,
            k_EpgLoadingPhase
        };
        class IPhase {
        public:
            virtual void Wait() = 0;
            virtual bool IsDone() = 0;
        };

        typedef std::function<void(void)> RecordingsDelegate;
        typedef std::function<bool(const EpgEntryList::value_type&)> EpgEntryAction;
        
        virtual IPhase* GetPhase(Phase phase) = 0;

        virtual const ChannelList& GetChannelList() = 0;
        virtual const GroupList &GetGroupList() = 0;
        virtual void GetEpg(ChannelId  channelId, time_t startTime, time_t endTime, EpgEntryList& epgEntries) = 0;
        virtual bool GetEpgEntry(UniqueBroadcastIdType i,  EpgEntry& enrty) = 0;
        virtual void ForEachEpg(const EpgEntryAction& action) const = 0;
        virtual std::string GetUrl(PvrClient::ChannelId channelId) = 0;
        
        virtual void ReloadRecordings() = 0;
        virtual void CallRpcAsync(const std::string & data, std::function<void(rapidjson::Document&)>  parser,
                                  ActionQueue::TCompletion completion) = 0;
        
        virtual ~IClientCore(){}
    };
 }

#endif /* pvr_client_types_h */
