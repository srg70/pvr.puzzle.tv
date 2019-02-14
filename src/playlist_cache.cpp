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


#include <memory>
#include "playlist_cache.hpp"
#include "Playlist.hpp"
#include "globals.hpp"

using namespace Globals;

namespace Buffers {
    PlaylistCache::PlaylistCache(const std::string &playlistUrl)
    : m_totalLength(0)
    , m_totalDuration(0.0)
    , m_playlist(playlistUrl)
    , m_lastSegmentOffset(0.0)
    , m_lastSegmentPosition (0)
    { }
    
    PlaylistCache::~PlaylistCache()  {
    }
    
    void PlaylistCache::ReloadPlaylist(){
        m_playlist.Reload();
        SegmentInfo info;
        bool hasMore = true;
        while(m_playlist.NextSegment(info, hasMore)) {
            m_dataToLoad.push(info);
            if(!hasMore)
                break;
        }
    }
    
    MutableSegment* PlaylistCache::SegmentToFillAfter(int64_t position)  {
//        MutableSegment::TimeOffset idx = TimeOffsetFromProsition(position);
        if(m_dataToLoad.empty()) {
                return nullptr;
        }
        
        SegmentInfo info(m_dataToLoad.front());
        m_dataToLoad.pop();
        MutableSegment* retVal = new MutableSegment(info.url, info.duration, m_lastSegmentOffset);
        m_lastSegmentOffset += info.duration;
        m_segmentsSwamp.push_back(std::unique_ptr<MutableSegment>(retVal));
        return retVal;
    }
    
    void PlaylistCache::SegmentReady(MutableSegment* segment) {
        segment->Seek(0); // For position init
        m_totalLength += segment->Length();
        m_totalDuration += segment->Duration();
        m_segments[m_lastSegmentPosition] = segment;
        m_lastSegmentPosition += segment->BytesReady();
    }
    
    Segment* PlaylistCache::SegmentAt(int64_t position) {
//        MutableSegment::TimeOffset requestedTime = TimeOffsetFromProsition(position);
        for (const auto& s : m_segments) {
            if(position >= s.first && position< s.first + s.second->Length()) {
                s.second->Seek(position - s.first);
                return s.second;
            }
        }
        return nullptr;
    }

    bool PlaylistCache::HasSegmentsToFill() const {
        return !m_dataToLoad.empty();
    }

    bool PlaylistCache::IsEof(int64_t position) const {
        return m_playlist.IsVod() /*&& check position*/;
    }

#pragma mark - Segment
    
    Segment::Segment(float duration)
    :_data(NULL)
    , _size(0)
    , _begin(NULL)
    , _duration(duration)
    {
    }
        
//    const uint8_t* Segment::Pop(size_t requesred, size_t*  actual)
//    {
//        if(_begin == NULL)
//            _begin = &_data[0];
//
//        size_t available = _size - (_begin - &_data[0]);
//        *actual = std::min(requesred, available);
//        const uint8_t* retVal = _begin;
//        _begin += *actual;
//        return retVal;
//    }
    
    size_t Segment::Seek(size_t position)
    {
        _begin = &_data[0] + std::min(position, _size);
        return Position();
    }
    
    
    size_t Segment::Read(uint8_t* buffer, size_t size)
    {
        if(_begin == NULL)
            _begin = &_data[0];
        size_t actual = std::min(size, BytesReady());
        //    LogDebug(">>> Available: %d  Actual: %d", available, actual);
        memcpy(buffer, _begin, actual);
        _begin += actual;
        return actual;
    }
    
    
    Segment::~Segment()
    {
        if(_data != NULL)
            free( _data);
    }

    void MutableSegment::Push(const uint8_t* buffer, size_t size)
    {
        if(NULL == buffer || 0 == size)
            return;
        
        void * ptr = realloc(_data, _size + size);
        if(NULL == ptr)
            throw PlaylistCacheException("Faied to allocate segmwnt.");
        _data = (uint8_t*) ptr;
        memcpy(&_data[_size], buffer, size);
        _size += size;
        //    LogDebug(">>> Size: %d", _size);
    }

}

