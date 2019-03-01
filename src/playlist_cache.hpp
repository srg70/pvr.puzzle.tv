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

#ifndef __playlist_cache_hpp__
#define __playlist_cache_hpp__

#include <map>
#include <list>
#include <deque>
#include <string>
#include <memory>
#include <exception>
#include "Playlist.hpp"
#include "plist_buffer_delegate.h"

namespace Buffers {

    class Segment
    {
    public:
        //            const uint8_t* Pop(size_t requesred, size_t*  actual);
        size_t Read(uint8_t* buffer, size_t size);
        size_t Position() const  {return  (nullptr == _data) ? 0 : _begin - &_data[0];}
        size_t BytesReady() const {return (nullptr == _data) ? 0 : Size() - Position();}
        float Bitrate() const { return  Duration() == 0.0 ? 0.0 : Size()/Duration();}
        float Duration() const {return _duration;}
        size_t Size() const {return _size;}

    protected:
        Segment(float duration);
        void Init();
        virtual ~Segment();
        uint8_t* _data;
        size_t _size;
        const uint8_t* _begin;
        const float _duration;
    };
    
    class MutableSegment : public Segment {
    public:
        typedef  float TimeOffset;
        typedef int64_t DataOffset;

        const TimeOffset timeOffset;
        const SegmentInfo info;
        void Push(const uint8_t* buffer, size_t size);
        bool IsValid() const {return _isValid;}
        bool IsLoading() const {return _isLoading;}
        void DataReady() {
            Seek(0);
            _isValid = true;
            _isLoading = false;
        }
        // Virtual segment size in bytes
        // may be not equal to actual _size
        size_t Length() const { return _length; }

        ~MutableSegment(){};
    private:
        friend class PlaylistCache;
        
        MutableSegment(const SegmentInfo& i, TimeOffset tOffset)
        : Segment(i.duration)
        , info(i)
        , timeOffset(tOffset)
        , _length(0)
        , _isValid (false)
        , _isLoading(false)
        {}

        size_t Seek(size_t position);
        void Free();

        size_t _length;
        bool _isValid;
        bool _isLoading;
    };
    

    class PlaylistCache {
        
    public:
        enum SegmentStatus{
            k_SegmentStatus_Ok = 0,
            k_SegmentStatus_CacheEmpty,
            k_SegmentStatus_Loading,
            k_SegmentStatus_EOF
        };
        PlaylistCache(const std::string &playlistUrl, PlaylistBufferDelegate delegate);
        ~PlaylistCache();
        MutableSegment* SegmentToFill();
        void SegmentReady(MutableSegment* segment);
        void SegmentCanceled(MutableSegment* segment);
        Segment* NextSegment(SegmentStatus& status);
        bool PrepareSegmentForPosition(int64_t position, uint64_t* nextSegmentIndex);
        bool HasSegmentsToFill() const;
//        bool IsEof() const;
        bool IsFull() const {return CanSeek() && m_cacheSizeInBytes > m_cacheSizeLimit; }
        int64_t Length() const { return CanSeek() ? m_totalLength : -1; }
        void ReloadPlaylist();
        bool CanSeek() const {return nullptr != m_delegate || m_playlist.IsVod(); }
    private:
       
        // key is segment index in m3u file
        typedef std::map<uint64_t, std::unique_ptr<MutableSegment>>  TSegments;
        typedef std::deque<SegmentInfo> TSegmentInfos;
        
        MutableSegment::TimeOffset TimeOffsetFromProsition(int64_t position) const {
            float bitrate = Bitrate();
            return (bitrate == 0.0) ? 0.0 : position/bitrate;
        }
        void CheckCacheSize();
        float Bitrate() const { return m_bitrate;}

        Playlist m_playlist;
        PlaylistBufferDelegate m_delegate;
        MutableSegment::TimeOffset m_playlistTimeOffset;
        TSegmentInfos m_dataToLoad;
        TSegments m_segments;
        int64_t m_totalLength;
        uint64_t m_currentSegmentIndex;
        float m_currentSegmentPositionFactor;
        float m_bitrate;
        int m_cacheSizeLimit;
        int m_cacheSizeInBytes;

    };
    
    class PlaylistCacheException : public std::exception
    {
    public:
        PlaylistCacheException(const char* reason = "")
        : m_reason(reason)
        {}
        virtual const char* what() const noexcept {return m_reason.c_str();}
        
    private:
        const std::string m_reason;
    };

    
}
#endif /* __playlist_cache_hpp__ */
