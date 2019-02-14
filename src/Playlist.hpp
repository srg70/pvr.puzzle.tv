/*
*
*   Copyright (C) 2018 Sergey Shramchenko
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

#ifndef Playlist_hpp
#define Playlist_hpp

#include <stdio.h>
#include <string>
#include <map>
#include <exception>

namespace Buffers{
    
    struct SegmentInfo {
        SegmentInfo () : duration(0.0) {}
        SegmentInfo(float d, std::string u) : url(u), duration(d){}
        SegmentInfo(const SegmentInfo& info) : SegmentInfo(info.duration, info.url) {}
        SegmentInfo&  operator=(const SegmentInfo&& s) { return *new (this)SegmentInfo(s.duration, s.url);}
        SegmentInfo&  operator=(const SegmentInfo& s) { return *new (this)SegmentInfo(s.duration, s.url);}
        const std::string url;
        const float duration;
    };
    
    class Playlist {
    public:
        Playlist(const std::string &url);
        bool NextSegment(SegmentInfo& info, bool& hasMoreSegments);
        bool Reload();
        bool IsVod() const {return m_isVod;}
    private:
        typedef std::map<uint64_t, SegmentInfo> TSegmentUrls;

        bool ParsePlaylist(const std::string& data);
        void SetBestPlaylist(const std::string& playlistUrl);
        void LoadPlaylist(std::string& data) const;

        
        TSegmentUrls m_segmentUrls;
        std::string  m_playListUrl;
        uint64_t m_loadIterator;
        bool m_isVod;

    };
    
    class PlaylistException :  public std::exception
    {
    public:
        PlaylistException(const char* reason = "")
        : m_reason(reason)
        {}
        virtual const char* what() const noexcept {return m_reason.c_str();}
        
    private:
        std::string m_reason;
    };

}

#endif /* Playlist_hpp */
