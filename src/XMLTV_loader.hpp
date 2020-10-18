/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
 *
 *   Copyright (C) 2013-2015 Anton Fedchin
 *   http://github.com/afedchin/xbmc-addon-iptvsimple/
 *
 *   Copyright (C) 2011 Pulse-Eight
 *   http://www.pulse-eight.com/
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

#ifndef XMLTV_loader_hpp
#define XMLTV_loader_hpp

#include <string>
#include <list>
#include <functional>
#include "pvr_client_types.h"

namespace XMLTV {
  
    struct EpgChannel
    {
        PvrClient::ChannelId     id;
        std::list<std::string>   displayNames;
        std::string         strIcon;
    };

    struct EpgEntry
    {
        PvrClient::ChannelId         EpgId;
        time_t      startTime;
        time_t      endTime;
        std::string strTitle;
        std::string strPlot;
        std::string strGenreString;
        std::string iconPath;
    };
    typedef std::function<void(const EpgChannel& newChannel)> ChannelCallback;
    typedef std::function<bool(const EpgEntry& newEntry)> EpgEntryCallback;

    PvrClient::KodiChannelId ChannelIdForChannelName(const std::string& strId);
    PvrClient::KodiChannelId EpgChannelIdForXmlEpgId(const std::string& strId);

    bool ParseChannels(const std::string& url,  const ChannelCallback& onChannelFound);
    bool ParseEpg(const std::string& url,  const EpgEntryCallback& onEpgEntryFound);

    long LocalTimeOffset();
    bool IsDataCompressed(const std::string& data);
    bool GzipInflate( const std::string& compressedBytes, std::string& uncompressedBytes);
    int GetCachedFileContents(const std::string &filePath, std::string &strContents);
    std::string GetCachedFilePath(const std::string &filePath);
    std::string GetCachedPathFor(const std::string& original);
}

#endif /* XMLTV_loader_hpp */
