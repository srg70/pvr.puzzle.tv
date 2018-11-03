//
//  Playlist.hpp
//  comple.test
//
//  Created by Sergey Shramchenko on 30/10/2018.
//  Copyright © 2018 Home. All rights reserved.
//

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
        SegmentInfo&  operator=(const SegmentInfo&& s) { return *new (this)SegmentInfo(s.duration, s.url);}
        const std::string url;
        const float duration;
    };
    
    class Playlist {
    public:
        Playlist(const std::string &url);
        bool NextSegment(const SegmentInfo** ppInfo, bool& hasMoreSegments);
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
