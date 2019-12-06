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
    
    PlaylistBuffer::PlaylistBuffer(const std::string &playListUrl,  PlaylistBufferDelegate delegate, bool seekForVod)
    : m_delegate(delegate)
    , m_cache(nullptr)
    , m_url(playListUrl)
    , m_seekForVod(seekForVod)
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
            m_loadingSegmentIndex = 0;
        }
        CreateThread();
    }
        
    static bool FillSegmentFromPlaylist(MutableSegment* segment, const std::string& content, std::function<bool()> IsCanceled)
    {
        Playlist plist(content);

        bool hasMoreSegments = false;
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
            }while (bytesRead > 0 && !IsCanceled());
            
            XBMC->CloseFile(f);
            if(!hasMoreSegments)
                break;
        }
        return !IsCanceled() && segment->BytesReady() > 0;
    }

    static bool FillSegment(MutableSegment* segment, std::function<bool()> IsCanceled)
    {
        void* f = XBMC->OpenFile(segment->info.url.c_str(), XFILE::READ_NO_CACHE | XFILE::READ_CHUNKED); //XFILE::READ_AUDIO_VIDEO);
        if(!f)
            throw PlistBufferException("Failed to download playlist media segment.");

        // Some content type should be treated as playlist
        // https://tools.ietf.org/html/draft-pantos-http-live-streaming-08#section-3.1
        char* contentType = XBMC->GetFilePropertyValue(f, XFILE::FILE_PROPERTY_CONTENT_TYPE, "");
        const bool contentIsPlaylist =  NULL != contentType && ( 0 == strcmp("application/vnd.apple.mpegurl", contentType)  || 0 == strcmp("audio/mpegurl", contentType));
        XBMC->FreeString(contentType);

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
            //        LogDebug(">>> Write: %d", bytesRead);
        }while (bytesRead > 0 && !IsCanceled());
        
        XBMC->CloseFile(f);
        
        if(contentIsPlaylist)
            return FillSegmentFromPlaylist(segment, contentForPlaylist, IsCanceled);
        return !IsCanceled() && segment->BytesReady() > 0;
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
            //m_cache->ReloadPlaylist();
            while (/*!isEof && */ !IsStopped()) {
                
                bool cacheIsFull = false;
                MutableSegment* segment =  nullptr;
                {
                    CLockObject lock(m_syncAccess);
                    segment = m_cache->SegmentToFill();
                    if(nullptr != segment) {
                        m_loadingSegmentIndex = segment->info.index;
                        LogDebug("PlaylistBuffer: Start fill segment #%" PRIu64 ".", m_loadingSegmentIndex);
                    }
                    // to avoid double lock in following while() loop
                    cacheIsFull = !m_cache->HasSpaceForNewSegment();
                }
                bool isStopped = IsStopped();

                // No reason to download next segment when cache is full
                while(cacheIsFull && !isStopped){
                    {
                        CLockObject lock(m_syncAccess);
                        // NOTE: this method frees space in cache
                        // Must be called in any case
                        cacheIsFull = !m_cache->HasSpaceForNewSegment();
                    }
                    // When we waiting for room in cache (e.g stream is on pause)
                    // try to ping server to avoid connection lost
                    if(cacheIsFull) {
                        if(nullptr != segment) {
                            isStopped = IsStopped(segment->Duration());
                        } else  {
                            isStopped = IsStopped(1);
                        }
                        if(!isStopped) {
                            LogDebug("PlaylistBuffer: waiting for space in cache...");
                            if(nullptr != segment) {
                                struct __stat64 stat;
                                XBMC->StatFile(segment->info.url.c_str(), &stat);
                                LogDebug("Stat segment #%" PRIu64 ".", m_loadingSegmentIndex);
                            }
                        }
                    }
                };
                
                auto startLoadingAt = std::chrono::system_clock::now();
                if(nullptr != segment){
                    float sleepTime = 1; //Min sleep time 1 sec
                    if(!IsStopped()) {
                        // Load segment data
                        const uint64_t loadingSegmentIndex = segment->info.index;
                        const bool segmentReady = FillSegment(segment, [this, loadingSegmentIndex]() {
                            return IsStopped() || m_loadingSegmentIndex != loadingSegmentIndex;
                        });
                        if(segmentReady)
                            LogDebug("PlaylistBuffer: End fill segment #%" PRIu64 ".", m_loadingSegmentIndex);
                        else
                            LogDebug("PlaylistBuffer: FAILED (or canceled) to fill segment  #%" PRIu64 ". New segmenet to fill #%" PRIu64 ".", segment->info.index, m_loadingSegmentIndex);
                        

                        // Populate loaded segment
                        if(!IsStopped()){
                            CLockObject lock(m_syncAccess);
                            if(segmentReady) {
                                m_cache->SegmentReady(segment);
                                m_writeEvent.Signal();
                                auto endLoadingAt = std::chrono::system_clock::now();
                                std::chrono::duration<float> loatTime = endLoadingAt-startLoadingAt;
                                LogDebug("PlaylistBuffer: segment loaded in %0.2f sec. Duration %0.2f",loatTime.count(), segment->Duration());
                            } else {
                                m_cache->SegmentCanceled(segment);
                            }
                        }
                        sleepTime = 1.0;//std::max(duration / 2.0, 1.0);
                    } else {
                        ;//isEof = m_cache->IsEof(m_position);
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
        
        LogDebug("PlaylistBuffer: write thread is done.");

        return NULL;
    }
    
    ssize_t PlaylistBuffer::Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs)
    {
     
        if(IsStopped()) {
            LogError("PlaylistBuffer: write thread is not running.");
            return -1;
        }
        size_t totalBytesRead = 0;
        bool isEof = false;
        PlaylistCache::SegmentStatus segmentStatus;
        //    int32_t timeout = c_commonTimeoutMs;
        while (totalBytesRead < bufferSize)
        {
            int waitingCounter = 0;
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
                        if(waitingCounter++ == 0 && IsRunning()){
                            LogNotice("PlaylistBuffer: waiting for segment loading (max %d ms)...", timeoutMs);
                            // NOTE: timeout is set by Timeshift buffer
                            // Do not change it! May cause long waiting on stopping/exit.
                            //timeoutMs = 5*1000;
                            m_writeEvent.Wait(timeoutMs);
                        } else {
                            LogError("PlaylistBuffer: segment loading  timeout! %d sec.", timeoutMs * (waitingCounter -1) / 1000);
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

        return !isEof && IsRunning() ?  totalBytesRead : -1;
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
            m_loadingSegmentIndex = nextSegmentIndex;
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
