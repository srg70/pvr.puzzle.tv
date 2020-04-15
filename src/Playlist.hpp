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

typedef  float TimeOffset;

struct SegmentInfo {
    SegmentInfo () : startTime(0.0), duration(0.0) , index (-1){}
    SegmentInfo(float t, float d, const std::string& u, uint64_t i) : startTime(t), url(u), duration(d), index(i){}
    SegmentInfo(const SegmentInfo& info) : SegmentInfo(info.startTime, info.duration, info.url, info.index) {}
    SegmentInfo&  operator=(const SegmentInfo&& s) { this->~SegmentInfo(); return *new (this)SegmentInfo(s.startTime, s.duration, s.url, s.index);}
    SegmentInfo&  operator=(const SegmentInfo& s) { this->~SegmentInfo(); return *new (this)SegmentInfo(s.startTime, s.duration, s.url, s.index);}
    const std::string url;
    // Calculated from playlist's index offset
    const TimeOffset startTime;
    const float duration;
    uint64_t index;
};

class Playlist {
public:
    Playlist(const std::string &url, uint64_t indexOffset = 0);
//    Playlist(const Playlist& playlist);
    bool NextSegment(SegmentInfo& info, bool& hasMoreSegments);
    bool SetNextSegmentIndex(uint64_t offset);
    bool Reload();
    bool IsVod() const {return m_isVod;}
    int TargetDuration() const {return m_targetDuration;}
    TimeOffset GetTimeOffset() const {return m_targetDuration * m_indexOffset;}
private:
    typedef std::map<uint64_t, SegmentInfo> TSegmentUrls;
    
    bool ParsePlaylist(const std::string& data);
    void SetBestPlaylist(const std::string& playlistUrl);
    void LoadPlaylist(std::string& data) const;
    
    
    TSegmentUrls m_segmentUrls;
    std::string  m_playListUrl;
    mutable std::string  m_effectivePlayListUrl;
    uint64_t m_loadIterator;
    bool m_isVod;
    const uint64_t m_indexOffset;
    uint64_t m_initialInternalIndex;
    int m_targetDuration;
    std::string m_httplHeaders;
    
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
