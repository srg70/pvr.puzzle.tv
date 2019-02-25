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


#include "p8-platform/os.h"
#include <memory>
#include <list>
#include <inttypes.h>
#include "playlist_cache.hpp"
#include "Playlist.hpp"
#include "globals.hpp"

using namespace Globals;

namespace Buffers {
    PlaylistCache::PlaylistCache(const std::string &playlistUrl, PlaylistBufferDelegate delegate)
    : m_totalLength(0)
    , m_totalDuration(0.0)
    , m_playlist(playlistUrl)
    , m_playlistTimeOffset(0.0)
    , m_playlistDataOffset(0)
    , m_delegate(delegate)
    , m_cacheSizeInBytes(0)
    , m_cacheSizeLimit((nullptr != delegate) ? delegate->SegmentsAmountToCache() * 3 * 1024 * 1024 : 0) // ~3MByte/chunck (usualy 6 sec)
    {
        ReloadPlaylist();
    }
    
    PlaylistCache::~PlaylistCache()  {
    }
    
    void PlaylistCache::ReloadPlaylist(){
        
        MutableSegment::TimeOffset timeOffaset = 0.0;
        MutableSegment::DataOffset dataOfset = 0;
        
        m_playlist.Reload();
        {
            SegmentInfo info;
            bool hasMore = true;
            while(m_playlist.NextSegment(info, hasMore)) {
                m_dataToLoad.push_back(info);
                if(!hasMore)
                    break;
            }
        }
        
        
        // For VOD we can fill data offset for segments already.
        // Do it only one time, i.e. when m_totalDuration == 0
        bool shouldCalculateOffset = m_playlist.IsVod() && m_totalDuration == 0;
        
        if (shouldCalculateOffset) {
            for (const auto& info : m_dataToLoad) {
                struct __stat64 stat;
                // try to stat segment.
                shouldCalculateOffset &= 0 == XBMC->StatFile(info.url.c_str(), &stat);
                if(shouldCalculateOffset) {
                    MutableSegment* retVal = new MutableSegment(info, timeOffaset, dataOfset);
                    retVal->_length = stat.st_size;
                    m_segments[info.index] = std::unique_ptr<MutableSegment>(retVal);
                    m_totalDuration = timeOffaset += info.duration;
                    m_totalLength = dataOfset += stat.st_size;
                } else {
                    LogError("PlaylistCache: failed to obtain file stat for %s. Total length %" PRId64 "(%f Bps)", info.url.c_str(), m_totalLength, Bitrate());
                }
            }
        }
        if(shouldCalculateOffset){
            LogError("PlaylistCache: playlist reloaded. Total length %" PRId64 "(%f kB/sec)", m_totalLength, Bitrate()/1024);
        }
    }
    
    MutableSegment* PlaylistCache::SegmentToFillAfter(int64_t position)  {

        if(IsFull())
            return nullptr;
        if(m_dataToLoad.empty()) {
            return nullptr;
        }
        
        SegmentInfo info;
        bool found = false;
        // Skip all valid segment
        do{
            info = m_dataToLoad.front();
            m_dataToLoad.pop_front();
            if(m_segments.count(info.index) > 0) {
                found = !m_segments[info.index]->IsValid();
            } else {
                found = true;
            }
        }while(!found && !m_dataToLoad.empty());
        // Do we have segment info to fill?
        if(!found) {
            return nullptr;
        }
        // VOD contains static segments.
        // No new segment needed, just return old "empty" segment
        if(m_playlist.IsVod() && m_segments.count(info.index) > 0) {
            return m_segments[info.index].get();
        }
        // Calculate time and data offsets
        MutableSegment::TimeOffset timeOffaset = m_playlistTimeOffset;
        MutableSegment::DataOffset dataOfset = m_playlistDataOffset;
        // Do we have previous segment?
        if(m_segments.count(info.index - 1) > 0){
            // Override playlist initial offsetts with actual values
            const auto& prevSegment = m_segments[info.index - 1];
            timeOffaset = prevSegment->timeOffset + prevSegment->Duration();
            dataOfset = prevSegment->dataOffset + prevSegment->Length();
        }
        MutableSegment* retVal = new MutableSegment(info, timeOffaset, dataOfset);
        m_segments[info.index] = std::unique_ptr<MutableSegment>(retVal);
        LogDebug("PlaylistCache: start LOADING segment %" PRIu64 ". Data offset %" PRId64 ".", info.index, dataOfset);
        return retVal;
    }
    
    void PlaylistCache::SegmentReady(MutableSegment* segment) {
        segment->DataReady();
        m_cacheSizeInBytes += segment->Size();
        // For VOD we should have bitrate ready
        // In case of failing m_totalLength should not be initialized
        if(!m_playlist.IsVod() ||  m_totalLength == 0)
        {
            m_totalLength += segment->Length();
            m_totalDuration += segment->Duration();
        }
        LogDebug("PlaylistCache: segment %" PRIu64 " added. Cache seze %d bytes", segment->info.index, m_cacheSizeInBytes);
    }
    
    Segment* PlaylistCache::SegmentAt(int64_t position) {
//        MutableSegment::TimeOffset requestedTime = TimeOffsetFromProsition(position);
        
        if(m_segments.size() == 0) {
            return nullptr;
        }
        
        Segment* retVal = nullptr;

        std::list<TSegments::key_type> segmentsToRemove;
        
        for (const auto& s : m_segments) {
            if(position >= s.second->dataOffset && position< s.second->dataOffset + s.second->Length()) {
                // Segment found. Check data availability
                if(s.second->IsValid()) {
                    s.second->Seek(position - s.second->dataOffset);
                    retVal = s.second.get();
                    LogDebug("PlaylistCache: READING from segment %" PRIu64 ".", s.second->info.index);
                } else {
                    LogDebug("PlaylistCache: segment %" PRIu64 " found, but has NO data.", s.second->info.index);
                    // Segment is not ready yet
                    retVal = nullptr;
                }
                break;
            }
            
            // Free older segments when cache is full
            // or we are on live stream (no caching requered)
            if((IsFull() && s.second->IsValid()) || !CanSeek()) {
                if(position > s.second->dataOffset + s.second->Length()) {
                    segmentsToRemove.push_back(s.first);
                }
            }
        }
        for (const auto& idx : segmentsToRemove) {
            
            m_cacheSizeInBytes -= m_segments[idx]->Size();
            if(!CanSeek()) {
                m_segments.erase(idx);
            } else {
                // Preserve stream length info for VOD segment
                m_segments[idx]->Free();
            }
            LogDebug("PlaylistCache: segment %" PRIu64 " removed. Cache seze %d bytes", idx, m_cacheSizeInBytes);

        }

        return retVal;
    }

    bool PlaylistCache::PrepareForSeek(int64_t position) {
        // Can't seek without delegate
        if(!CanSeek())
            return false;

    

        m_dataToLoad = TSegmentInfos();
        // VOD plailist contains all segments already
        // So just move loading iterator to position
        MutableSegment::TimeOffset timeOffset = 0.0;
        if(m_playlist.IsVod()) {
            bool found = false;
            for (const auto& pSeg : m_segments) {
                auto start = pSeg.second->dataOffset;
                auto end = start + pSeg.second->Length();
                if( start <= position && position < end) {
                    m_playlist.SetNextSegmentIndex(pSeg.first);
                    found = true;
                    break;
                }
                timeOffset += pSeg.second->Duration();
            }
            if(!found){
                LogDebug("PlaylistCache: position %" PRId64 " can't be seek. Total length %" PRIu64 "."  , position, Length());
                // Can't be
                return false;
            }
        } else {
            timeOffset = TimeOffsetFromProsition(position);
            auto url = m_delegate->UrlForTimeshift(timeOffset, nullptr);
            m_playlist = Playlist(url);
            ReloadPlaylist();
        }
//        if(succeeded)
        {
            m_playlistTimeOffset = timeOffset;
            m_playlistDataOffset = position;
        }
        return true;
    }
    
    bool PlaylistCache::HasSegmentsToFill() const {
        return !m_dataToLoad.empty();
    }

    bool PlaylistCache::IsEof(int64_t position) const {
        return m_playlist.IsVod() /*&& check position*/;
    }

#pragma mark - Segment
    
    Segment::Segment(float duration)
    : _duration(duration)
    , _data(nullptr)
    {
        Init();
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
    
    void Segment::Init() {
        if(_data != nullptr)
            free( _data);
        _data = nullptr;
        _size = 0;
        _begin = nullptr;
    }
    size_t Segment::Seek(size_t position)
    {
        if(nullptr != _data) {
            _begin = &_data[0] + std::min(position, _size);
        }
        return Position();
    }
    
    
    size_t Segment::Read(uint8_t* buffer, size_t size)
    {
        if(nullptr == _data)
            return 0;

        if(_begin == nullptr)
            _begin = &_data[0];
        size_t actual = std::min(size, BytesReady());
        //    LogDebug(">>> Available: %d  Actual: %d", available, actual);
        memcpy(buffer, _begin, actual);
        _begin += actual;
        return actual;
    }
    
    
    Segment::~Segment()
    {
        if(_data != nullptr)
            free( _data);
    }

    void MutableSegment::Free(){
        Init();
        _isValid = false;
    }
    
    void MutableSegment::Push(const uint8_t* buffer, size_t size)
    {
        if(nullptr == buffer || 0 == size)
            return;
        
        void * ptr = realloc(_data, _size + size);
        if(NULL == ptr)
            throw PlaylistCacheException("Failed to re-allocate segment.");
        _data = (uint8_t*) ptr;
        memcpy(&_data[_size], buffer, size);
        _size += size;
        //    LogDebug(">>> Size: %d", _size);
        if(_size > _length) {
            _length = _size;
        }
    }

}

