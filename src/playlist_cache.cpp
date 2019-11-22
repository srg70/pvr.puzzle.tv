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


#if (defined(_WIN32) || defined(_WIN64))
#define __STDC_FORMAT_MACROS
#endif
#define NOMINMAX
#include <algorithm>
#include <inttypes.h>
#include "p8-platform/os.h"
#include <memory>
#include <list>
#include "playlist_cache.hpp"
#include "Playlist.hpp"
#include "globals.hpp"

using namespace Globals;

namespace Buffers {
    PlaylistCache::PlaylistCache(const std::string &playlistUrl, PlaylistBufferDelegate delegate, bool seekForVod)
    : m_totalLength(0)
    , m_bitrate(0.0)
    , m_playlist(playlistUrl)
    , m_playlistTimeOffset(0.0)
    , m_delegate(delegate)
    , m_cacheSizeInBytes(0)
    , m_currentSegmentIndex(0)
    , m_currentSegmentPositionFactor(0.0)
    , m_seekForVod(seekForVod)
    {
        if(!ProcessPlaylist()){
            LogError("PlaylistCache: playlist initialization failed.");
            throw PlaylistException("PlaylistCache: playlist initialization failed.");
        }
        m_currentSegmentIndex = m_dataToLoad.size() > 0 ?  m_dataToLoad.front().index : 0;
        m_cacheSizeLimit = CanSeek()
            ? ((nullptr != delegate) ? delegate->SegmentsAmountToCache() : 20) * 6 * 1024 * 1024
            : 0; // ~6 MByte/chunck (usualy 6 sec)
    }
    
    PlaylistCache::~PlaylistCache() {
    }
    
    bool PlaylistCache::ReloadPlaylist() {
         
        if(!m_playlist.Reload()) {
            LogError("PlaylistCache: playlist is empty or missing.");
            return false;
        }
        return ProcessPlaylist();
    }

    bool PlaylistCache::ProcessPlaylist() {
        SegmentInfo info;
        bool hasMore = true;
        while(m_playlist.NextSegment(info, hasMore)) {
            m_dataToLoad.push_back(info);
            if(!hasMore)
                break;
        }
        
        // For VOD we can fill data offset for segments already.
        // Do it only first time, i.e. when m_bitrate == 0
        const bool shouldCalculateOffset = CanSeek() && m_bitrate == 0;
        if (shouldCalculateOffset) {
            MutableSegment::TimeOffset timeOffaset = 0.0;
            MutableSegment::DataOffset dataOfset = 0;
            
            // Stat first 3 segments to calculate bitrate
            auto it = m_dataToLoad.begin();
            auto last  = m_dataToLoad.end();
            int statCounter = 0;
            // Stat at least 3 segments for bitrate
            while(it != last && statCounter++ < 3) {
                struct __stat64 stat;
                if(0 != XBMC->StatFile(it->url.c_str(), &stat)){
                    LogError("PlaylistCache: failed to obtain file stat for %s. Total length %" PRId64 "(%f Bps)", it->url.c_str(), m_totalLength, Bitrate());
                    return false;
                }
                MutableSegment* segment = new MutableSegment(*it, timeOffaset);
                segment->_length = stat.st_size;
                m_segments[it->index] = std::unique_ptr<MutableSegment>(segment);
                timeOffaset += segment->Duration();
                m_totalLength += segment->_length;
                
                ++it;
            }
            
            // Define "virtual" bitrate.
            m_bitrate =  m_totalLength/timeOffaset;
            
            if(m_playlist.IsVod()) {
                while(it!=last) {
                    MutableSegment* segment = new MutableSegment(*it, timeOffaset);
                    segment->_length = m_bitrate * it->duration;
                    m_segments[it->index] = std::unique_ptr<MutableSegment>(segment);
                    timeOffaset += segment->Duration();
                    m_totalLength += segment->_length;
                    ++it;
                }
                
                // For VOD playlist we would like to load last segment just after first to be ready for seek to end of stream
                // Move last to second.
                m_dataToLoad.insert(m_dataToLoad.begin() + 1, m_dataToLoad.back());
                m_dataToLoad.pop_back();
            } else {
                m_totalLength = m_delegate->Duration() * Bitrate();
            }
            
            LogError("PlaylistCache: playlist reloaded. Total length %" PRId64 "(%f B/sec).", m_totalLength, Bitrate());
        }
        
        return true;
    }
    
    MutableSegment* PlaylistCache::SegmentToFill()  {

//        if(IsFull())
//            return nullptr;
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
        
        MutableSegment* retVal = nullptr;
        // VOD contains static segments.
        // No new segment needed, just return old "empty" segment
        if(m_playlist.IsVod() && m_segments.count(info.index) > 0) {
            retVal = m_segments[info.index].get();
        } else {
            // Calculate time and data offsets
            MutableSegment::TimeOffset timeOffaset = m_playlistTimeOffset;
            // Do we have previous segment?
            if(m_segments.count(info.index - 1) > 0){
                // Override playlist initial offsetts with actual values
                const auto& prevSegment = m_segments[info.index - 1];
                timeOffaset = prevSegment->timeOffset + prevSegment->Duration();
            }
            retVal = new MutableSegment(info, timeOffaset);
            m_segments[info.index] = std::unique_ptr<MutableSegment>(retVal);
        }
        LogDebug("PlaylistCache: start LOADING segment %" PRIu64 ".", info.index);

        retVal->_isLoading = true;
        return retVal;
    }
    
    void PlaylistCache::SegmentReady(MutableSegment* segment) {
        segment->DataReady();
        m_cacheSizeInBytes += segment->Size();
        LogDebug("PlaylistCache: segment %" PRIu64 " added. Cache size %d bytes", segment->info.index, m_cacheSizeInBytes);
    }
    
    void PlaylistCache::SegmentCanceled(MutableSegment* segment) {
        if(CanSeek()) {
            // Preserve stream length info for VOD segment
            m_segments[segment->info.index]->Free();
        } else {
            m_segments.erase(segment->info.index);
        }
        LogDebug("PlaylistCache: segment %" PRIu64 " canseled. Cache size %d bytes", segment->info.index, m_cacheSizeInBytes);
    }
    
    Segment* PlaylistCache::NextSegment(SegmentStatus& status) {
        
        if(m_segments.size() == 0) {
            status = k_SegmentStatus_CacheEmpty;
            return nullptr;
        }
        
        MutableSegment* retVal = nullptr;
        
        if(m_segments.count(m_currentSegmentIndex) > 0) {
            auto& seg = m_segments[m_currentSegmentIndex];
            // Segment found. Check data availability
            if(seg->IsValid()) {
                size_t posInSegment = m_currentSegmentPositionFactor * seg->Size();
                seg->Seek(posInSegment);
                retVal = seg.get();
                status = k_SegmentStatus_Ok;
                LogDebug("PlaylistCache: READING from segment %" PRIu64 ". Position in segment %d.", seg->info.index, posInSegment);
            } else {
                // Validate that current segmenet is loading
                if(!seg->IsLoading()){
                    LogDebug("PlaylistCache: segment %" PRIu64 " found, has been queued.", seg->info.index);
                    m_dataToLoad.push_front(seg->info);
                } else {
                    LogDebug("PlaylistCache: segment %" PRIu64 " found, loading data...", seg->info.index);
                }
                // Segment is not ready yet
                status = k_SegmentStatus_Loading;
                retVal = nullptr;
            }
        }
        else if(m_playlist.IsVod()){
            // With VOD all segments are knowon
            // So we are probably out of stream range, i.e. EOF
            int64_t lastIndex = -1;
            if(m_segments.size())
                lastIndex = (--m_segments.end())->first;
            LogDebug("PlaylistCache: wrong current segment index #%" PRIu64 ". Last #%" PRId64 " of %d segments.", m_currentSegmentIndex, lastIndex,  m_segments.size());
            status = k_SegmentStatus_EOF;
        } else if(nullptr != m_delegate){
            // Dynamic seekable stream (e.g. Edem)
            // Currnt segment may be unknown for now
            // Check duration and report EOF if needed
            float segmentTimeOffset = m_currentSegmentIndex * m_playlist.TargetDuration();
            if(segmentTimeOffset >= m_delegate->Duration()) {
                status = k_SegmentStatus_EOF;
                LogDebug("PlaylistCache: wrong current segment index #%" PRIu64 ". Requested time offset %f. Stream duration %d.", m_currentSegmentIndex, segmentTimeOffset, m_delegate->Duration());
            } else {
                status = k_SegmentStatus_Loading;
                LogDebug("PlaylistCache: segment with index #%" PRIu64 " should start loading shortly. Requested time offset %f. Stream duration %d.", m_currentSegmentIndex, segmentTimeOffset, m_delegate->Duration());
            }
        }else {
            // Live stream
            // TODO: check whether segment loading now
            status = k_SegmentStatus_Loading;
            LogDebug("PlaylistCache: segment with index #%" PRIu64 " should start loading shortly. Last known segment #%" PRIu64 ".", m_currentSegmentIndex, m_segments.size() > 0 ? m_segments.rbegin()->first : 0);
        }
        
        // Forward to nex segment only if we found current
        if(nullptr != retVal) {
            ++m_currentSegmentIndex;
            m_currentSegmentPositionFactor = 0.0;
        }
        return retVal;
    }

    bool PlaylistCache::HasSpaceForNewSegment() {
        // Current segment for read is m_currentSegmentIndex - 1
        uint64_t currentSegment = m_currentSegmentIndex -1;
        // Free older segments when cache is full
        // or we are on live stream (no caching requered)
        if(IsFull()) {
            int64_t idx = -1;
            auto runner = m_segments.begin();
            const auto end = m_segments.end();
            if(CanSeek()) {
                // Skip first segment, preserve in cache
                // Kodi seeks to 0 freaquently...
                ++runner;
            }
            // Search for oldest segment
            while(runner->first < currentSegment) {
                if(runner->second->IsValid()) {
                    idx = runner->first;
                    break;
                }
                ++runner;
            }
            // If we don't have a segment to free BEFORE current position,
            // and we are NOT live stream,
            // search for farest segment AFTER range of valid segments
            // from current position.
            // We'll need to reload it later,
            // but we have to free some memory now, for new data,
            // that probably waiting for memory
            if(-1 == idx && CanSeek()) {
                // Search for first invalid continues segment after current
                while(m_segments.count(++currentSegment) != 0) {
                    if(!m_segments[currentSegment]->IsValid()){
                        break;
                    }
                }
                auto rrunner = m_segments.rbegin();
                const auto rend = m_segments.rend();
                ++rrunner; // Skip last segment, preserve in cache
                while(rrunner != rend &&  rrunner->first > currentSegment) {
                    if(rrunner->second->IsValid()){
                        idx = rrunner->first;
                        break;
                    }
                    ++rrunner;
                }
            }
            // Remove or free memory of segment
            if( idx != -1) {
                m_cacheSizeInBytes -= m_segments[idx]->Size();
                if(CanSeek()) {
                    // Preserve stream length info for VOD segment
                    m_segments[idx]->Free();
                } else {
                    m_segments.erase(idx);
                }
                LogDebug("PlaylistCache: segment %" PRIu64 " removed. Cache size %d bytes", idx, m_cacheSizeInBytes);
            } else if(CanSeek()) {
                LogDebug("PlaylistCache: cache is full but no segments to free. Current idx %" PRIu64 " Size %d bytes", currentSegment, m_cacheSizeInBytes);
            } else {
                LogDebug("PlaylistCache: cache is full but no segments to free. Current idx %" PRIu64 " %d segments in cache.", currentSegment, m_segments.size());
            }
        }
        return !IsFull();
    }
    
    // Find segment in playlist by time offset,caalculated from position
    bool PlaylistCache::PrepareSegmentForPosition(int64_t position, uint64_t* nextSegmentIndex) {
        // Can't seek without delegate
        if(!CanSeek())
            return false;
        
        MutableSegment::TimeOffset timePosition = TimeOffsetFromProsition(position);
        MutableSegment::TimeOffset segmentTime = 0.0;
        float segmentDuration = 0.0;
        bool found = false;
        
        // Plailist may contain required segments already.
        // We'll search for segment in loding queue and in loaded segments list.
        // If found, just move loading iterator to position
        for (const auto& pSeg : m_segments) {
            segmentTime = pSeg.second->timeOffset;
            segmentDuration = pSeg.second->Duration();
            if( segmentTime <= timePosition && timePosition < segmentTime + segmentDuration) {
                found = true;
                *nextSegmentIndex = m_currentSegmentIndex = pSeg.first;
                m_playlist.SetNextSegmentIndex(m_currentSegmentIndex);
                // Calculate position inside segment
                // as rational part of time offset
                m_currentSegmentPositionFactor = (timePosition - segmentTime) / segmentDuration;
                break;
            }
        }

        if(!found) {
            for (const auto& pData : m_dataToLoad) {
                segmentTime = pData.index * m_playlist.TargetDuration();
                segmentDuration = pData.duration;
                if( segmentTime <= timePosition && timePosition < segmentTime + segmentDuration) {
                    found = true;
                    *nextSegmentIndex = m_currentSegmentIndex = pData.index;
                    m_playlist.SetNextSegmentIndex(m_currentSegmentIndex);
                    // Calculate position inside segment
                    // as rational part of time offset
                    m_currentSegmentPositionFactor = (timePosition - segmentTime) / segmentDuration;
                    break;
                }
            }
        }
        // Note: Clean loading queue in any case!
        m_dataToLoad = TSegmentInfos();

        if(!found) {
            // For VOD: if the segment was not found, return an EOF immediately.
            if(m_playlist.IsVod()) {
                LogError("PlaylistCache: position %" PRId64 " can't be seek. Time offset %f. Total duration %f."  , position, timePosition, (nullptr != m_delegate ? m_delegate->Duration() : -1.0));
                // Can't be
                return false;
            } else {
                if(timePosition > m_delegate->Duration()) {
                    LogError("PlaylistCache: requested time offset %f exits stream duration %d.", timePosition, (nullptr != m_delegate ? m_delegate->Duration() : -1.0));
                    // Can't be
                    return false;
                }
                segmentTime = timePosition;
                segmentDuration = m_playlist.TargetDuration();
                
                time_t adjustedTimeOffset = timePosition;
                auto url = m_delegate->UrlForTimeshift(timePosition, &adjustedTimeOffset);
                uint64_t indexOffset = timePosition / m_playlist.TargetDuration();
                try {
                    m_playlist = Playlist(url, indexOffset);
                    if(!ReloadPlaylist())
                        throw PlaylistCacheException("ReloadPlaylist() failed.");
                } catch (std::exception& ex) {
                    LogError( "PlaylistCache: %s", ex.what());
                    return false;
                }
                
                m_playlistTimeOffset = indexOffset * m_playlist.TargetDuration();
                *nextSegmentIndex = m_currentSegmentIndex = indexOffset;
                m_currentSegmentPositionFactor = (timePosition - m_playlistTimeOffset) / m_playlist.TargetDuration();
            }
        }
        if(m_currentSegmentPositionFactor > 1.0) {
            LogError("PlaylistCache: segment position factor can't be > 1.0. Requested offset %f, segment timestamp %f, duration %f", timePosition, segmentTime, segmentDuration);
        } else {
            LogDebug("PlaylistCache:  seek segment index %" PRIu64 " and positon ratio %f", m_currentSegmentIndex, m_currentSegmentPositionFactor);
        }
        return true;
    }
    
    bool PlaylistCache::HasSegmentsToFill() const {
        return !m_dataToLoad.empty();
    }

//    bool PlaylistCache::IsEof() const {
//        return m_playlist.IsVod() && m_segments.count(m_currentSegmentIndex) == 0;
//    }

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
    
    size_t Segment::Read(uint8_t* buffer, size_t size)
    {
        if(nullptr == _data)
            return 0;

        if(_begin == nullptr)
            _begin = &_data[0];
        size_t actual = std::min(size, BytesReady());
        if(actual < 0)
            LogDebug("Segment::Read: error size %d.  Bytes ready: %d", actual, BytesReady());
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
        _isLoading = false;
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
//        if(_size > _length) {
//            _length = _size;
//        }
    }
    
    size_t MutableSegment::Seek(size_t position)
    {
        if(nullptr != _data) {
            _begin = &_data[0] + std::min(position, _size);
        }
        return Position();
    }
    


}

