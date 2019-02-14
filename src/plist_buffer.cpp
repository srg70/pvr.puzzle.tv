/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
 *
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

#if (defined(_WIN32) || defined(__WIN32__))
#include <windows.h>
#ifdef GetObject
#undef GetObject
#endif
#endif

#include "helpers.h"
#include "plist_buffer.h"
#include "libXBMC_addon.h"
#include "globals.hpp"
#include "playlist_cache.hpp"
#include "p8-platform/util/util.h"

using namespace P8PLATFORM;
using namespace ADDON;
using namespace Globals;

namespace Buffers {
    
    PlaylistBuffer::PlaylistBuffer(const std::string &playListUrl,  PlaylistBufferDelegate delegate, int segmentsCacheSize)
    : m_delegate(delegate)
    , m_segmentsCacheSize(delegate == nullptr ? 0 : segmentsCacheSize) // No seek - no cache.
    , m_cache(nullptr)
    {
        Init(playListUrl);
    }
    
    PlaylistBuffer::~PlaylistBuffer()
    {
        StopThread();
    }
    
    void PlaylistBuffer::Init(const std::string &playlistUrl)
    {
        Init(playlistUrl, true, 0, 0, 0);
    }

    void PlaylistBuffer::Init(const std::string &playListUrl, bool clearContent, int64_t position, time_t readTimshift, time_t writeTimshift)
    {
        StopThread(20000);
        {
            CLockObject lock(m_syncAccess);

            m_writeEvent.Reset();
            if(m_cache)
                SAFE_DELETE(m_cache);
            m_cache = new PlaylistCache(playListUrl);
            m_position = position;
        }
        CreateThread();
    }
        
    bool PlaylistBuffer::FillSegment(MutableSegment* segment)
    {
        void* f = XBMC->OpenFile(segment->url.c_str(), XFILE::READ_NO_CACHE | XFILE::READ_CHUNKED); //XFILE::READ_AUDIO_VIDEO);
        if(!f)
            throw PlistBufferException("Failed to download playlist media segment.");
        
        unsigned char buffer[8196];
        ssize_t  bytesRead;
        do {
            bytesRead = XBMC->ReadFile(f, buffer, sizeof(buffer));
            segment->Push(buffer, bytesRead);
            //        LogDebug(">>> Write: %d", bytesRead);
        }while (bytesRead > 0 && !IsStopped());
        
        XBMC->CloseFile(f);
        
//        float bitrate = 0.0;
//        time_t segmentTimeshift = 0;
//        size_t segmantsSize = 0;
//        {
//            CLockObject lock(m_syncAccess);
//            if(!IsStopped()) {
//                m_segments[m_writeTimshift] = segmentData;
//                segmentTimeshift = m_writeTimshift;
//                m_writeTimshift += segmentData->Duration();
//                m_totalDuration += segmentData->Duration();
//                m_totalLength += segmentData->Length();
//                bitrate = Bitrate();
//                segmantsSize = m_segments.size();
//            }
//        }
//        // If added?
//        if(segmantsSize > 0) {
//            LogDebug(">>> Segment added at %d. Total %d segs. Bitrate %f", segmentTimeshift, segmantsSize, segmentData->Bitrate());
//            LogDebug(">>> Average bitrate: %f", bitrate);
//            // limit segments size to 20
//            // to avoid memory overflow
//            //if(m_playlist->IsVod())
//            {
//                bool segsCacheFull = segmantsSize > 20;
//                while(segsCacheFull && !IsStopped()){
//                    P8PLATFORM::CEvent::Sleep(1*1000);
//                    CLockObject lock(m_syncAccess);
//                    segsCacheFull = m_segments.size() > 20;
//                };
//
//            }
//        }

        return bytesRead < 0; // 0 (i.e. EOF) means no error, caller may continue with next chunk
    }
    
    bool PlaylistBuffer::IsStopped(uint32_t timeoutInSec) {
        P8PLATFORM::CTimeout timeout(timeoutInSec * 1000);
        do{
            bool isStopped = P8PLATFORM::CThread::IsStopped();
            if(isStopped || timeout.TimeLeft() == 0)
                return isStopped;
            Sleep(1000);//1sec
        }while (true);
        return false;
    }
    
    void *PlaylistBuffer::Process()
    {
        bool isEof = false;
        try {
            m_cache->ReloadPlaylist();
            while (!isEof && !IsStopped()) {

                MutableSegment* segment =  nullptr;
                {
                    CLockObject lock(m_syncAccess);
                    segment = m_cache->SegmentToFillAfter(m_position);
                }
    
                float sleepTime = 1; //Min sleep time 1 sec
                if(nullptr != segment) {
                    LogDebug(">>> Start fill segment.");
                    FillSegment(segment);
                    LogDebug(">>> End fill segment.");
                    
                    auto duration = segment->Duration();
                    LogDebug(">>> Segment duration: %f", duration);
                    if(!IsStopped()){
                        CLockObject lock(m_syncAccess);
                        m_cache->SegmentReady(segment);
                        m_writeEvent.Signal();
                    }
                    sleepTime = std::max(duration / 2.0, 1.0);
                } else {
                    isEof = m_cache->IsEof(m_position);
                }
                if(!isEof && !m_cache->HasSegmentsToFill()){
                    IsStopped(sleepTime);
                }
                // We should update playlist often, disregarding to amount of data to load
                // Even if we have several segment to load
                // it can take a time, and playlist will be out of sync.
                m_cache->ReloadPlaylist();

            }
            
        } catch (InputBufferException& ex ) {
            LogError("Playlist download thread failed with error: %s", ex.what());
        }
        
        if(isEof) {
            LogDebug(">>> PlaylistBuffer: received all VOD stream. Write is done.");
        }
        return NULL;
    }
    
    ssize_t PlaylistBuffer::Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs)
    {
        
        size_t totalBytesRead = 0;
        //    int32_t timeout = c_commonTimeoutMs;
        while (totalBytesRead < bufferSize)
        {
            Segment* segment = NULL;
            {
                CLockObject lock(m_syncAccess);
                segment = m_cache->SegmentAt(m_position);
            }
            // Retry 1 time after write operation
            if(NULL == segment && m_writeEvent.Wait(timeoutMs))
            {
                CLockObject lock(m_syncAccess);
                segment = m_cache->SegmentAt(m_position);
            }
            if(NULL == segment)
            {
                //            StopThread();
                LogNotice("PlaylistBuffer: no segment for read.");
                break;
            }
            size_t bytesToRead;
            size_t bytesRead;
            do
            {
                bytesToRead = bufferSize - totalBytesRead;
                bytesRead = segment->Read( buffer + totalBytesRead, bytesToRead);
                totalBytesRead += bytesRead;
                m_position += bytesRead;
            }while(bytesToRead > 0 && bytesRead > 0);

        }
        
        //m_position += totalBytesRead;
//        LogDebug(">>> PlaylistBuffer::Read(): totalBytesRead %d, position %lld", totalBytesRead, m_position);

//        bool hasMoreData = false;
//        {
//            CLockObject lock(m_syncAccess);
//            hasMoreData = m_segments.count(m_readTimshift) > 0  ||  (!IsStopped() && IsRunning());
//        }
//        if(!hasMoreData)
//            LogDebug(">>> PlaylistBuffer: read is done. No more data.");
//
//        if (hasMoreData)
            return totalBytesRead;
//        return -1;
    }
    
    bool PlaylistBuffer::SwitchStream(const std::string &newUrl)
    {
        bool succeeded = false;
        try {
            Init(newUrl);
            succeeded = true;
        } catch (const InputBufferException& ex) {
            XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32004), ex.what());
            LogError("PlaylistBuffer: Failed to switch streams to %s.\n Error: %s", newUrl.c_str(), ex.what());
        }
        
        return succeeded;
    }
    
    int64_t PlaylistBuffer::GetLength() const
    {
        return -1;
        if(m_delegate == nullptr) {
            LogDebug("Plist archive lenght -1");
            return -1;
        }
        float bitrate = 0.0;
        {
            CLockObject lock(m_syncAccess);
            bitrate = m_cache->Bitrate();
         }
        int64_t retVal = m_delegate->Duration() * bitrate;
       LogDebug("Plist archive lenght %lld (bitrate %f)", retVal, bitrate);
        return retVal;
    }
    
    int64_t PlaylistBuffer::GetPosition() const
    {
        return -1;
        if(m_delegate == nullptr) {
            LogDebug("Plist archive position =1");
            return -1;
        }
        LogDebug("Plist archive position %lld", m_position);
        return m_position;
    }
    
    int64_t PlaylistBuffer::Seek(int64_t iPosition, int iWhence)
    {
        return -1;
        
//        if(m_delegate == nullptr)
//            return -1;
//
//        LogDebug("PlaylistBuffer::Seek. >>> Requested pos %lld, from %d", iPosition, iWhence);
//
//        // Translate position to offset from start of buffer.
//        int64_t length = GetLength();
//        int64_t begin = 0;
//
//        if(iWhence == SEEK_CUR) {
//            iPosition = m_position + iPosition;
//        } else if(iWhence == SEEK_END) {
//            iPosition = length + iPosition;
//        }
//        if(iPosition < 0 )
//            LogDebug("PlaylistBuffer::Seek. Can't be pos %lld", iPosition);
//
//        if(iPosition > length) {
//            iPosition = length;
//        }
//        if(iPosition < begin) {
//            iPosition = begin;
//        }
//        iWhence = SEEK_SET;
//        LogDebug("PlaylistBuffer::Seek. Calculated pos %lld", iPosition);
//
//        int64_t seekDelta =  iPosition - m_position;
//        if(seekDelta == 0)
//            return m_position;
//
//        // Do we have the data in cache already?
//        float bitrate = 0.0;
//        time_t requestedTimeshit, curTimeshift;
//        time_t writeTimeshift = 0, readTimshift;
//        bool haveCachedSeg = false, shouldReinit = false;
//        {
//            CLockObject lock(m_syncAccess);
//            bitrate = Bitrate();
//            requestedTimeshit = bitrate == 0.0 ? 0 : iPosition / bitrate;
//            curTimeshift = bitrate == 0.0 ? 0 : m_position / bitrate;
//
//            for (auto& seg: m_segments) {
//                if(seg.first > requestedTimeshit || seg.first + seg.second->Duration()  < requestedTimeshit)
//                    continue;
//                // We have a segment
//                haveCachedSeg = true;
//                readTimshift = seg.first;
//                LogDebug("PlaylistBuffer::Seek. Found cached segment. timeshift %d", readTimshift);
//                size_t posInSegment = requestedTimeshit - seg.first * bitrate;
//                seg.second->Seek(posInSegment);
//
//                // continue write after continuesly last cahed segment after the found one
//                writeTimeshift = readTimshift;
//                TSegments::mapped_type segData;
//                do{
//                    segData = m_segments.at(writeTimeshift);
//                    writeTimeshift += segData->Duration();
//                } while(m_segments.count(writeTimeshift) > 0);
//                shouldReinit = m_writeTimshift != writeTimeshift;
//                break;
//            }
//        }
//        if(haveCachedSeg &&  shouldReinit) {
//            time_t adjustedTimeshift = writeTimeshift;
//            std::string newUrl = m_delegate->UrlForTimeshift(writeTimeshift, &adjustedTimeshift);
//            if(writeTimeshift != adjustedTimeshift ) {
//                writeTimeshift = adjustedTimeshift;
//            }
//
//            LogDebug("PlaylistBuffer::Seek. Restart write from %d sec. Read at %d sec", writeTimeshift, readTimshift);
//
//            Init(newUrl, false,  iPosition, readTimshift, writeTimeshift);
//            return m_position;
//        }
//
//        // No data in cache - reinit writer
//        try {
//
//            time_t adjustedTimeshift = requestedTimeshit;
//            std::string newUrl = m_delegate->UrlForTimeshift(requestedTimeshit, &adjustedTimeshift);
//            if(requestedTimeshit != adjustedTimeshift ) {
//                iPosition = adjustedTimeshift * bitrate;
//                requestedTimeshit = adjustedTimeshift;
//            }
//            LogDebug("PlaylistBuffer::Seek. Delta %f Mb, %lld(%lld) sec", seekDelta/1024.0/1024.0, (requestedTimeshit - curTimeshift), requestedTimeshit);
//
//            // Flour to segment duration (10 sec)
//            time_t segTimeshift = requestedTimeshit / 10 * 10;
//            Init(newUrl,  false, iPosition, segTimeshift, segTimeshift);
//
//            // If we can't read from strean in 60 sec - report error
//            if(!m_writeEvent.Wait(60000)) {
//                LogError("PlaylistBuffer::Seek. failed to read after seek in 60 sec.");
//                return -1;
//            }
//            {
//                CLockObject lock(m_syncAccess);
//                auto& seg = m_segments.at(segTimeshift);
//                size_t posInSegment = iPosition - segTimeshift * bitrate;
//                seg->Seek(posInSegment);
//                m_position = iPosition;
//            }
//            LogDebug("PlaylistBuffer::Seek. pos after seek %lld", m_position);
//            return m_position;
//
//        } catch (std::exception&  ex) {
//            LogError("PlaylistBuffer::Seek. Exception thrown. Reason: %s. Reset position!", ex.what());
//            m_position = 0;
//            return -1;
//
//        }catch (...) {
//            LogError("PlaylistBuffer::Seek. Unknown exception. Reset position!");
//            m_position = 0;
//            return -1;
//        }
    }
    
    bool PlaylistBuffer::StopThread(int iWaitMs)
    {
        int stopCounter = 0;
        bool retVal = false;
        while(!(retVal = this->CThread::StopThread(iWaitMs))){
            if(++stopCounter > 3)
                break;
            LogNotice("PlaylistBuffer: can't stop thread in %d ms (%d)", iWaitMs, stopCounter);
        }
        if(!retVal)
            LogError("PlaylistBuffer: failed to stop thread in %d ms", stopCounter*iWaitMs);
        
        return retVal;
    }
    
}
