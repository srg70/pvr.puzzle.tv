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


namespace PvrClient {
    typedef unsigned int ChannelId;
    
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

    class IClientCore
    {
    public:
        virtual const ChannelList& GetChannelList() = 0;
        virtual const GroupList &GetGroupList() = 0;

    };
}

#endif /* pvr_client_types_h */
