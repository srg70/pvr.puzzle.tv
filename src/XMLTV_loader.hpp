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

#include "libXBMC_pvr.h"
#include <string>
#include <functional>

namespace XMLTV {
  
    struct EpgChannel
    {
        std::string                  strId;
        std::string                  strName;
        std::string                  strIcon;
    };

    struct EpgEntry
    {
        int         iChannelId;
        time_t      startTime;
        time_t      endTime;
        std::string strTitle;
        std::string strPlot;
        std::string strGenreString;
    };
    typedef std::function<void(const EpgChannel& newChannel)> ChannelCallback;
    typedef std::function<void(const EpgEntry& newEntry)> EpgEntryCallback;

    bool ParseChannels(const std::string& url,  const ChannelCallback& onChannelFound, ADDON::CHelper_libXBMC_addon * XBMC);
    bool ParseEpg(const std::string& url,  const EpgEntryCallback& onEpgEntryFound ,ADDON::CHelper_libXBMC_addon * XBMC);
    
}

#endif /* XMLTV_loader_hpp */
