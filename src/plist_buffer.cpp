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
#define __STDC_FORMAT_MACROS
#ifdef GetObject
#undef GetObject
#endif
#endif

#include <inttypes.h>
#include <chrono>
#include "ThreadPool.h"
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
    int PlaylistBuffer::s_numberOfHlsThreads = 1;
    
    int PlaylistBuffer::SetNumberOfHlsTreads(int numOfTreads) {
        const auto numOfCpu = std::thread::hardware_concurrency();
        if(numOfTreads < 0)
            numOfTreads = 1;
        else if(numOfTreads > numOfCpu)
            numOfTreads = numOfCpu;
        return s_numberOfHlsThreads = numOfTreads;
    }

    PlaylistBuffer::PlaylistBuffer(const std::string &playListUrl,  PlaylistBufferDelegate delegate, bool seekForVod)
    : m_delegate(delegate)
    , m_cache(nullptr)
    , m_url(playListUrl)
    , m_seekForVod(seekForVod)
    , m_isWaitingForRead(false)
    {
        Init(playListUrl);
    }
    
    PlaylistBuffer::~PlaylistBuffer()
    {
        StopThread();
    }
    
    void PlaylistBuffer::Init(const std::string &playlistUrl)
    {
         StopThread(20000);
        {
            CLockObject lock(m_syncAccess);

            m_writeEvent.Reset();
            if(m_cache)
                SAFE_DELETE(m_cache);
            try {
                m_cache = new PlaylistCache(playlistUrl, m_delegate, m_seekForVod);
            } catch (PlaylistException ex) {
                XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32024));
                throw PlistBufferException((std::string("Playlist exception: ") + ex.what()).c_str());
            }
            m_position = 0;
            m_currentSegment = nullptr;
            m_segmentIndexAfterSeek = 0;
        }
        CreateThread();
    }
        
    static bool FillSegmentFromPlaylist(MutableSegment* segment, const std::string& content, std::function<bool(const MutableSegment&)> IsCanceled)
    {
        Playlist plist(content);

        bool hasMoreSegments = false;
        bool isCanceled = false;
        SegmentInfo info;
        while(plist.NextSegment(info, hasMoreSegments)) {
            void* f = XBMC->OpenFile(info.url.c_str(), XFILE::READ_NO_CACHE | XFILE::READ_CHUNKED); //XFILE::READ_AUDIO_VIDEO);
            if(!f)
                throw PlistBufferException("Failed to open media segment of sub-playlist.");

            unsigned char buffer[8196];
            ssize_t  bytesRead;
            do {
                bytesRead = XBMC->ReadFile(f, buffer, sizeof(buffer));
                segment->Push(buffer, bytesRead);
                isCanceled = IsCanceled(*segment);
            }while (bytesRead > 0 && !isCanceled);
            
            XBMC->CloseFile(f);
            if(!hasMoreSegments || isCanceled)
                break;
        }
        
        if(isCanceled){
             LogDebug("PlaylistBuffer: segment #%" PRIu64 " CANCELED.", segment->info.index);
             return false;
         } else if(segment->BytesReady() == 0) {
             LogDebug("PlaylistBuffer: segment #%" PRIu64 " FAILED.", segment->info.index);
             return false;
         }

         LogDebug("PlaylistBuffer: segment #%" PRIu64 " FINISHED.", segment->info.index);
         return true;

    }

    static bool FillSegment(MutableSegment* segment, std::function<bool(const MutableSegment&)> IsCanceled, std::function<void(bool,MutableSegment*)> segmentDone)
    {
        std::hash<std::thread::id> hasher;
        LogDebug("PlaylistBuffer: segment #%" PRIu64 " STARTED. (thread 0x%X).", segment->info.index, hasher(std::this_thread::get_id()));

        bool isCanceled = IsCanceled(*segment);
        bool result = !isCanceled;

        do {
            // Do not bother the server with canceled segments
            if(isCanceled)
                break;
            
            void* f = XBMC->OpenFile(segment->info.url.c_str(), XFILE::READ_NO_CACHE | XFILE::READ_CHUNKED | XFILE::READ_TRUNCATED); //XFILE::READ_AUDIO_VIDEO);
            if(!f)
                throw PlistBufferException("Failed to download playlist media segment.");
            
            // Some content type should be treated as playlist
            // https://tools.ietf.org/html/draft-pantos-http-live-streaming-08#section-3.1
            //char* contentType = XBMC->GetFilePropertyValue(f, XFILE::FILE_PROPERTY_CONTENT_TYPE, "");
            const bool contentIsPlaylist =  false;//NULL != contentType && ( 0 == strcmp("application/vnd.apple.mpegurl", contentType)  || 0 == strcmp("audio/mpegurl", contentType));
            //XBMC->FreeString(contentType);
            
            unsigned char buffer[8196];
            std::string contentForPlaylist;
            ssize_t  bytesRead;
            do {
                bytesRead = XBMC->ReadFile(f, buffer, sizeof(buffer));
                if(contentIsPlaylist) {
                    contentForPlaylist.append ((char *) buffer, bytesRead);
                } else{
                    segment->Push(buffer, bytesRead);
                }
                isCanceled = IsCanceled(*segment);
                //        LogDebug(">>> Write: %d", bytesRead);
            }while (bytesRead > 0 && !isCanceled);
            
            XBMC->CloseFile(f);
            
            if(contentIsPlaylist && !isCanceled) {
                result = FillSegmentFromPlaylist(segment, contentForPlaylist, [&isCanceled, &IsCanceled](const MutableSegment& seg){
                    return isCanceled = IsCanceled(seg);
                });
            }
            
        } while(false);
        if(isCanceled){
            LogDebug("PlaylistBuffer: segment #%" PRIu64 " CANCELED.", segment->info.index);
            result = false;
        } else if(segment->BytesReady() == 0) {
            LogDebug("PlaylistBuffer: segment #%" PRIu64 " FAILED.", segment->info.index);
            result = false;
        } else {
            LogDebug("PlaylistBuffer: segment #%" PRIu64 " FINISHED.", segment->info.index);
        }
        segmentDone(result, segment);

        return result;
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
        using namespace progschj;

        bool isEof = false;
        ThreadPool pool(s_numberOfHlsThreads);
        pool.set_queue_size_limit(s_numberOfHlsThreads);

        try {
            int current_loader = 0;
            while (/*!isEof && */ !IsStopped()) {
                
                bool cacheIsFull = false;
                MutableSegment* segment =  nullptr;
                uint64_t segmentIdx (-1);
                {
                    CLockObject lock(m_syncAccess);
                    segment = m_cache->SegmentToFill();
                    
                    if(nullptr != segment) {
                        segmentIdx = segment->info.index;
                        std::hash<std::thread::id> hasher;
                        LogDebug("PlaylistBuffer: segment #%" PRIu64 " INITIALIZED.", segmentIdx);
                    }
                    // to avoid double lock in following while() loop
                    cacheIsFull = !m_cache->HasSpaceForNewSegment(segmentIdx);
                }
                bool isStopped = IsStopped();

                const uint64_t segmentIndexAfterSeek = m_segmentIndexAfterSeek;
                std::function<bool(const MutableSegment&)> isSegmentCanceled = [this, segmentIndexAfterSeek](const MutableSegment& seg) {
                    return IsStopped() || (m_segmentIndexAfterSeek != segmentIndexAfterSeek && seg.info.index != m_segmentIndexAfterSeek);
                };
                // No reason to download next segment when cache is full
                while(cacheIsFull && !isStopped){
                    {
                        CLockObject lock(m_syncAccess);
                        // NOTE: this method frees space in cache
                        // Must be called in any case
                        cacheIsFull = !m_cache->HasSpaceForNewSegment(segmentIdx);
                    }
                    // When we waiting for room in cache (e.g stream is on pause)
                    // try to ping server to avoid connection lost
                    if(cacheIsFull) {
                        if(nullptr != segment) {
                            if(isSegmentCanceled(*segment))
                                break;
                        }
                        isStopped = IsStopped(1);
                        if(!isStopped) {
                            LogDebug("PlaylistBuffer: waiting for space in cache...");
                            if(nullptr != segment) {
                                struct __stat64 stat;
                                XBMC->StatFile(segment->info.url.c_str(), &stat);
                                LogDebug("Stat segment #%" PRIu64 ".", segment->info.index);
                            }
                        }
                    }
                };
                
                if(nullptr != segment){
                    if(!IsStopped() /*&& !isSegmentCanceled(*segment)*/) {
                        // Load segment data
                        
                        auto startLoadingAt = std::chrono::system_clock::now();
                        std::function<void(bool,MutableSegment*)> segmentDone = [this, startLoadingAt](bool segmentReady, MutableSegment* seg) {
                            // Populate loaded segment
                            if(!IsStopped()){
                                CLockObject lock(m_syncAccess);
                                if(segmentReady) {
                                    m_cache->SegmentReady(seg);
                                    m_writeEvent.Signal();
                                    auto endLoadingAt = std::chrono::system_clock::now();
                                    std::chrono::duration<float> loatTime = endLoadingAt-startLoadingAt;
                                    LogDebug("PlaylistBuffer: segment #%" PRIu64 " loaded in %0.2f sec. Duration %0.2f", seg->info.index, loatTime.count(), seg->Duration());
                                } else {
                                    m_cache->SegmentCanceled(seg);
                                }
                            }
                        };

                        pool.enqueue(FillSegment, segment, isSegmentCanceled, segmentDone);
                        
                    }
                } else {
                    IsStopped(1);
                }
                // We should update playlist often, disregarding to amount of data to load
                // Even if we have several segment to load
                // it can take a time, and playlist will be out of sync.
                if(!IsStopped())
                {
                    CLockObject lock(m_syncAccess);
                    if(!m_cache->ReloadPlaylist()) {
                        LogError("PlaylistBuffer: playlist update failed.");
                        break;
                    }
                }
                
                //                if(!m_cache->HasSegmentsToFill()){
                //                    IsStopped(sleepTime);
                //                }
                
                
            }
            
        } catch (InputBufferException& ex ) {
            LogError("PlaylistBuffer: download thread failed with error: %s", ex.what());
        }

        LogDebug("PlaylistBuffer: finilizing loaders pool...");
        pool.wait_until_empty();
        pool.wait_until_nothing_in_flight ();

        LogDebug("PlaylistBuffer: write thread is done.");

        return NULL;
    }
    
    ssize_t PlaylistBuffer::Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs)
    {
     
        if(IsStopped()) {
            LogError("PlaylistBuffer: write thread is not running.");
            return -1;
        }
        m_isWaitingForRead = true;
        
        size_t totalBytesRead = 0;
        bool isEof = false;
        PlaylistCache::SegmentStatus segmentStatus;
        //    int32_t timeout = c_commonTimeoutMs;
        while (totalBytesRead < bufferSize)
        {
            while(nullptr == m_currentSegment)
            {
                {
                    CLockObject lock(m_syncAccess);
                    m_currentSegment = m_cache->NextSegment(segmentStatus);
                }
                if( nullptr == m_currentSegment) {
                    if((isEof = PlaylistCache::k_SegmentStatus_EOF == segmentStatus)) {
                        LogNotice("PlaylistBuffer: EOF reported.");
                        break;
                    }
                    if(PlaylistCache::k_SegmentStatus_Loading == segmentStatus ||
                       PlaylistCache::k_SegmentStatus_CacheEmpty == segmentStatus)
                    {
                        if(!IsRunning()){
                            LogError("PlaylistBuffer: not running (aka stopping) ...");
                            break;
                        }
                        LogDebug("PlaylistBuffer: waiting for segment loading (max %d ms)...", timeoutMs);
                        // NOTE: timeout is set by Timeshift buffer
                        // Do not change it! May cause long waiting on stopping/exit.
                        bool hasNewSegment = false;
                        do {
                            auto startAt = std::chrono::system_clock::now();
                            hasNewSegment = m_writeEvent.Wait(1000);
                            auto waitingMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - startAt);
                            timeoutMs -= waitingMs.count();
                            
                        } while (IsRunning() && !hasNewSegment && timeoutMs > 1000);
                        if(timeoutMs < 1000)
                        {
                            LogError("PlaylistBuffer: segment loading  timeout!");
                            break;
                        }
                    } else {
                        LogError("PlaylistBuffer: segment not found. Reason %d.", segmentStatus);
                        break;
                    }
                }
                
            }
 
            if(NULL == m_currentSegment)
            {
                // StopThread();
                LogNotice("PlaylistBuffer: no segment for read.");
                break;
            }
            size_t bytesToRead = bufferSize - totalBytesRead;
            size_t bytesRead;
            do {
                bytesRead = m_currentSegment->Read( buffer + totalBytesRead, bytesToRead);
                totalBytesRead += bytesRead;
                m_position += bytesRead;
                bytesToRead = bufferSize - totalBytesRead;

            } while(bytesToRead > 0 && bytesRead > 0);
            if(m_currentSegment->BytesReady() <= 0) {
                LogDebug("PlaylistBuffer: read all data from segment. Moving next...");
                m_currentSegment = nullptr;
            }
            

        }
        m_isWaitingForRead = false;
        return !isEof && IsRunning() ?  totalBytesRead : -1;
    }
    
    void PlaylistBuffer::AbortRead(){
        StopThread();
        while(m_isWaitingForRead) {
            LogDebug("PlaylistBuffer: waiting for readidng abort 100 ms...");
            P8PLATFORM::CEvent::Sleep(100);
            m_writeEvent.Signal();
        }
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
        return m_cache->Length();
    }
    
    int64_t PlaylistBuffer::GetPosition() const
    {
        if(!m_cache->CanSeek()) {
            LogDebug("PlaylistBuffer: Plist archive position -1");
            return -1;
        }
        LogDebug("PlaylistBuffer: Plist archive position %" PRId64 "", m_position);
        return m_position;
    }
    
    int64_t PlaylistBuffer::Seek(int64_t iPosition, int iWhence)
    {
        if(!m_cache->CanSeek())
            return -1;

        LogDebug("PlaylistBuffer: Seek requested pos %" PRId64 ", from %d", iPosition, iWhence);

        // Translate position to offset from start of buffer.
        int64_t length = GetLength();
        int64_t begin = 0;

        if(iWhence == SEEK_CUR) {
            iPosition = m_position + iPosition;
        } else if(iWhence == SEEK_END) {
            iPosition = length + iPosition;
        }
        if(iPosition < 0 )
            LogDebug("PlaylistBuffer: Seek can't be pos %" PRId64 "", iPosition);

        if(iPosition > length) {
            iPosition = length;
        }
        if(iPosition < begin) {
            iPosition = begin;
        }
        iWhence = SEEK_SET;
        LogDebug("PlaylistBuffer: Seek calculated pos %" PRId64 "", iPosition);

        if(iPosition == m_position)
            return m_position;
        // If cahce failed to prepare for seek operation
        // return failed code
        {
            CLockObject lock(m_syncAccess);
            uint64_t nextSegmentIndex;
            if(!m_cache->PrepareSegmentForPosition(iPosition, &nextSegmentIndex)) {
                LogDebug("PlaylistBuffer: cache failed to prepare for seek to pos %" PRId64 "", iPosition);
                return -1;
            }
            m_segmentIndexAfterSeek = nextSegmentIndex;
        }
        
        m_currentSegment = nullptr;
        m_position = iPosition;
        return m_position;
    }
    
    bool PlaylistBuffer::StopThread(int iWaitMs)
    {
        LogDebug("PlaylistBuffer: terminating loading thread...");
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
