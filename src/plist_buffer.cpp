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

using namespace P8PLATFORM;
using namespace ADDON;

namespace Buffers {
    
    static CTimeout s_SeekTimeout;

    static std::string ToAbsoluteUrl(const std::string& url, const std::string& baseUrl){
        const char* c_HTTP = "http://";
        
        // If Reliative URL
        if(std::string::npos == url.find(c_HTTP)) {
            // Append site prefix...
            auto urlPos = baseUrl.find(c_HTTP);
            if(std::string::npos == urlPos)
                throw PlistBufferException((std::string("Missing http:// in base URL: ") + baseUrl).c_str());
            urlPos = baseUrl.rfind('/');
            if(std::string::npos == urlPos)
                throw PlistBufferException((std::string("No '/' in base URL: ") + baseUrl).c_str());
            return baseUrl.substr(0, urlPos + 1) + url;
        }
        return url;
        
    }
    PlaylistBuffer::PlaylistBuffer(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &playListUrl,  PlaylistBufferDelegate delegate)
    : m_addonHelper(addonHelper)
    , m_totalLength(0)
    , m_totalDuration(0.0)
    , m_delegate(delegate)
    {
        Init(playListUrl);
    }
    
    PlaylistBuffer::~PlaylistBuffer()
    {
        StopThread();
    }
    
    void PlaylistBuffer::Init(const std::string &playListUrl, int64_t position, time_t timeshift)
    {
        StopThread(20000);
        {
            CLockObject lock(m_syncAccess);

            m_writeEvent.Reset();
            m_segmentUrls.clear();
            //m_segments.clear();
            m_position = position;
            m_writeTimshift = m_readTimshift =  timeshift;
            s_SeekTimeout.Init(10*1000);
        }
        SetBestPlaylist(playListUrl);
        CreateThread();
    }
    
    static uint64_t ParseXstreamInfTag(const std::string& data, std::string& url)
    {
        const char* c_BAND = "BANDWIDTH=";
        
        auto pos = data.find(c_BAND);
        if(std::string::npos == pos)
            throw PlistBufferException("Invalid playlist format: missing BANDWIDTH in #EXT-X-STREAM-INF tag.");
        pos += strlen(c_BAND);
        uint64_t bandwidth = std::stoull(data.substr(pos), &pos);
        pos = data.find('\n', pos);
        if(std::string::npos == pos)
            throw PlistBufferException("Invalid playlist format: missing NEW LINE in #EXT-X-STREAM-INF tag.");
        url = data.substr(++pos);
        trim(url);
        return bandwidth;
    }
    
    void PlaylistBuffer::SetBestPlaylist(const std::string& playlistUrl)
    {
        const char* c_XINF = "#EXT-X-STREAM-INF:";
        
        m_lastSegment = -1;
        m_playListUrl = playlistUrl;
        std::string data;
        LoadPlaylist(data);
        auto pos = data.find(c_XINF);
        // Simple playlist. No special parsing needed.
        if(std::string::npos == pos) {
            ParsePlaylist(data);
            return;
        }
        //    m_addonHelper->Log(LOG_DEBUG, "Variant playlist URL: \n %s", playlistUrl.c_str() );
        //    m_addonHelper->Log(LOG_DEBUG, "Variant playlist: \n %s", data.c_str() );
        
        uint64_t bestRate = 0;
        while(std::string::npos != pos)
        {
            pos += strlen(c_XINF);
            auto endTag = data.find(c_XINF,pos);
            std::string::size_type tagLen = endTag - pos;
            if(std::string::npos == endTag)
                tagLen = std::string::npos;
            
            std::string tagBody = data.substr(pos, tagLen);
            pos = endTag;
            std::string url;
            uint64_t rate = ParseXstreamInfTag(tagBody, url);
            if(rate > bestRate) {
                m_playListUrl = ToAbsoluteUrl(url, playlistUrl);
                bestRate = rate;
            }
        }
        //    m_addonHelper->Log(LOG_DEBUG, "Best URL (%d): \n %s", bestRate, m_playListUrl.c_str() );
        
    }
    
    bool PlaylistBuffer::ParsePlaylist(const std::string& data)
    {
        const char* c_M3U = "#EXTM3U";
        const char* c_INF = "#EXTINF:";
        const char* c_SEQ = "#EXT-X-MEDIA-SEQUENCE:";
        const char* c_TYPE = "#EXT-X-PLAYLIST-TYPE:";
        
        try {
            auto pos = data.find(c_M3U);
            if(std::string::npos == pos)
                throw PlistBufferException("Invalid playlist format: missing #EXTM3U tag.");
            pos += strlen(c_M3U);
            
            int64_t mediaIndex = 0;
            std::string  body;
            pos = data.find(c_SEQ);
            // If we have media-sequence tag - use it
            if(std::string::npos != pos) {
                pos += strlen(c_SEQ);
                body = data.substr(pos);
                mediaIndex = std::stoull(body, &pos);
                m_isVod = false;
            } else {
                // ... otherwise check plist type. VOD list may ommit sequence ID
                pos = data.find(c_TYPE);
                if(std::string::npos == pos)
                    throw PlistBufferException("Invalid playlist format: missing #EXT-X-MEDIA-SEQUENCE and #EXT-X-PLAYLIST-TYPE tag.");
                pos+= strlen(c_TYPE);
                body = data.substr(pos);
                std::string vod("VOD");
                if(body.substr(0,vod.size()) != vod)
                    throw PlistBufferException("Invalid playlist format: VOD playlist expected.");
                m_isVod = true;
                mediaIndex = 0;
                body=body.substr(0, body.find("#EXT-X-ENDLIST"));
            }
            
            pos = body.find(c_INF, pos);
            bool hasContent = false;
            while(std::string::npos != pos) {
                pos += strlen(c_INF);
                body = body.substr(pos);
                float duration = std::stof (body, &pos);
                if(',' != body[pos++])
                    throw PlistBufferException("Invalid playlist format: no coma after INF tag.");
                
                std::string::size_type urlPos = body.find('\n',pos) + 1;
                pos = body.find(c_INF, urlPos);
                hasContent = true;
                // Check whether we have a segment already
                if(m_lastSegment >= mediaIndex) {
                    ++mediaIndex;
                    continue;
                }
                m_lastSegment = mediaIndex;
                std::string::size_type urlLen = pos - urlPos;
                if(std::string::npos == pos)
                    urlLen = std::string::npos;
                auto url = body.substr(urlPos, urlLen);
                trim(url);
                url = ToAbsoluteUrl(url, m_playListUrl);
                //            m_addonHelper->Log(LOG_NOTICE, "IDX: %u Duration: %f. URL: %s", mediaIndex, duration, url.c_str());
                m_segmentUrls[mediaIndex++]  = TSegmentUrls::mapped_type(duration, url);
            }
            m_addonHelper->Log(LOG_DEBUG, "m_segmentUrls.size = %d, %s", m_segmentUrls.size(), hasContent ? "Not empty." : "Empty."  );
            return hasContent;
        } catch (std::exception& ex) {
            m_addonHelper->Log(LOG_ERROR, "Bad M3U : parser error %s", ex.what() );
            m_addonHelper->Log(LOG_ERROR, "M3U data : \n %s", data.c_str() );
            throw;
        } catch (...) {
            m_addonHelper->Log(LOG_ERROR, "Bad M3U : \n %s", data.c_str() );
            throw;
        }
    }
    
    void PlaylistBuffer::LoadPlaylist(std::string& data)
    {
        char buffer[1024];

        m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: (re)loading playlist %s.", m_playListUrl.c_str());

        auto f = m_addonHelper->OpenFile(m_playListUrl.c_str(), XFILE::READ_NO_CACHE);
        if (!f)
            throw PlistBufferException("Failed to obtain playlist from server.");
        bool isEof = false;
        do{
            buffer[0]= 0;
            auto bytesRead = m_addonHelper->ReadFile(f, buffer, sizeof(buffer));
            isEof = bytesRead <= 0;
            data.append(&buffer[0], bytesRead);
        }while(!isEof);
        m_addonHelper->CloseFile(f);

        m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: (re)loading done. Content: \n%s", data.size()> 16000 ? "[More that 16K]" : data.c_str());

    }
    
    bool PlaylistBuffer::FillSegment(const PlaylistBuffer::TSegmentUrls::mapped_type& segmentUrl)
    {
        unsigned char buffer[8196];
        void* f = m_addonHelper->OpenFile(segmentUrl.second.c_str(), XFILE::READ_NO_CACHE | XFILE::READ_CHUNKED); //XFILE::READ_AUDIO_VIDEO);
        if(!f)
            throw PlistBufferException("Failed to download playlist media segment.");
        
        auto segmentData = TSegments::mapped_type(new TSegments::mapped_type::element_type(segmentUrl.first));
        //    segmentData->m_addonHelper = m_addonHelper;
        ssize_t  bytesRead;
        do {
            bytesRead = m_addonHelper->ReadFile(f, buffer, sizeof(buffer));
            segmentData->Push(buffer, bytesRead);
            //        m_addonHelper->Log(LOG_DEBUG, ">>> Write: %d", bytesRead);
        }while (bytesRead > 0 && !IsStopped());
        
        m_addonHelper->CloseFile(f);
        
        float bitrate = 0.0;
        size_t segmantsSize = 0;
        {
            CLockObject lock(m_syncAccess);
            if(!IsStopped()) {
                m_segments[m_writeTimshift] = segmentData;
                m_writeTimshift += segmentData->Duration();
                m_totalDuration += segmentData->Duration();
                m_totalLength += segmentData->Length();
                bitrate = Bitrate();
                segmantsSize = m_segments.size();
            }
        }
        m_addonHelper->Log(LOG_DEBUG, ">>> Segment added at %d. Total %d segs. Bitrate %f", m_writeTimshift, segmantsSize, segmentData->Bitrate());
        m_addonHelper->Log(LOG_DEBUG, ">>> Average bitrate: %f", bitrate);

        return bytesRead < 0; // 0 (i.e. EOF) means no error, caller may continue with next chunk
    }
    
    bool PlaylistBuffer::IsStopped(uint32_t timeoutInSec) {
        P8PLATFORM::CTimeout timeout(timeoutInSec * 1000);
        bool isStoppedOrTimeout = P8PLATFORM::CThread::IsStopped() || timeout.TimeLeft() == 0;
        while(!isStoppedOrTimeout) {
            isStoppedOrTimeout = P8PLATFORM::CThread::IsStopped() || timeout.TimeLeft() == 0;
            Sleep(1000);//1sec
        }
        return P8PLATFORM::CThread::IsStopped();
    }
    
    void *PlaylistBuffer::Process()
    {
        bool isEof = false;
        try {
            while (!isEof && !IsStopped()) {
                if(m_segmentUrls.empty()) {
                    // Update archive URL with actual timeshift
//                    if(m_delegate != nullptr) {
//                        time_t adjustedTimeshift = m_writeTimshift;
//                        std::string newPlistUrl = m_delegate->UrlForTimeshift(m_writeTimshift, &adjustedTimeshift);
//                        if(m_writeTimshift == adjustedTimeshift) {
//                            m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: update url from %s to %s.", m_playListUrl.c_str(), newPlistUrl.c_str());
//                            m_playListUrl = newPlistUrl;
//                        } else {
//                            m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: received all archive stream. Write is done.");
//                            break;
//                        }
//
//                    }
                    std::string data;
                    LoadPlaylist(data);
                    // Empty playlist treat as EOF.
                    isEof = !ParsePlaylist(data);
                }
                
                auto it = m_segmentUrls.begin();
                auto end = m_segmentUrls.end();
                float sleepTime = 1; //Min sleep time 1 sec
                while (it != end && !isEof  && !IsStopped()) {
                    const auto& segmentUrl = it->second;
                    isEof = FillSegment(segmentUrl);

                    auto duration = segmentUrl.first;
                    sleepTime = duration;
                    m_addonHelper->Log(LOG_DEBUG, ">>> Segment duration: %f", duration);
                    ++it;
                    if(!IsStopped())
                        m_writeEvent.Signal();
                }
                if(!IsStopped())
                    m_segmentUrls.clear();

                if(m_isVod) {
                    m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: received all VOD stream. Write is done.");
                    break;
                }
                IsStopped(sleepTime);
            }
            
        } catch (InputBufferException& ex ) {
            m_addonHelper->Log(LOG_ERROR, "Playlist download thread failed with error: %s", ex.what());
        }
        
        return NULL;
    }
    
    ssize_t PlaylistBuffer::Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs)
    {
        
        size_t totalBytesRead = 0;
        //    int32_t timeout = c_commonTimeoutMs;
        while (totalBytesRead < bufferSize)
        {
            TSegments::mapped_type segment = NULL;
            {
                CLockObject lock(m_syncAccess);
                if(m_segments.count(m_readTimshift) > 0)
                    segment = m_segments.at(m_readTimshift);
            }
            // Retry 1 time after write operation
            if(NULL == segment && m_writeEvent.Wait(timeoutMs))
            {
                CLockObject lock(m_syncAccess);
                if(m_segments.count(m_readTimshift) > 0)
                    segment = m_segments.at(m_readTimshift);
            }
            if(NULL == segment)
            {
                //            StopThread();
                m_addonHelper->Log(LOG_NOTICE, "PlaylistBuffer: no segment for read.");
                break;
            }
            size_t bytesToRead;
            size_t bytesRead;
            do
            {
                bytesToRead = bufferSize - totalBytesRead;
                bytesRead = segment->Read( buffer + totalBytesRead, bytesToRead);
                totalBytesRead += bytesRead;
            }while(bytesToRead > 0 && bytesRead > 0);


            if(segment->BytesReady() <= 0){
                m_readTimshift += segment->Duration();
                m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: Segment been read. Read/write timeshift (%d/%d)", m_readTimshift, m_writeTimshift);
                // Reset segment position for next usage.
                segment->Seek(0);

                CLockObject lock(m_syncAccess);
                // Check cache size ...
                while(m_segments.size() > 20) {
                    // Release farest (from current position) segment
                    time_t maxTimeDelta = 0;
                    time_t segToRemove;
                    for (auto& seg: m_segments) {
                        time_t timeDelta = labs(m_readTimshift - seg.first);
                        if(timeDelta <= maxTimeDelta)
                            continue;
                        maxTimeDelta = timeDelta;
                        segToRemove = seg.first;
                    }
                    m_segments.erase(segToRemove);
                    m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: Segment removed at %d sec. Total %d segs.", segToRemove, m_segments.size());
                }
            }
        }
        m_position += totalBytesRead;
//        m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer::Read(): totalBytesRead %d, position %lld", totalBytesRead, m_position);

        bool hasMoreData = false;
        {
            CLockObject lock(m_syncAccess);
            hasMoreData = m_segments.count(m_readTimshift) > 0  ||  (!IsStopped() && IsRunning());
        }
        if(!hasMoreData)
            m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: read is done. No more data.");
        
        if (hasMoreData)
            return totalBytesRead;
        return -1;
    }
    
    bool PlaylistBuffer::SwitchStream(const std::string &newUrl)
    {
        bool succeeded = false;
        try {
            Init(newUrl);
            succeeded = true;
        } catch (const InputBufferException& ex) {
            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32004), ex.what());
            m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer: Failed to switch streams to %s.\n Error: %s", newUrl.c_str(), ex.what());
        }
        
        return succeeded;
    }
    
    int64_t PlaylistBuffer::GetLength() const
    {
        if(m_delegate == nullptr) {
            m_addonHelper->Log(LOG_DEBUG, "Plist archive lenght -1");
            return -1;
        }
        float bitrate = 0.0;
        {
            CLockObject lock(m_syncAccess);
            bitrate = Bitrate();
         }
        int64_t retVal = m_delegate->Duration() * bitrate;
       m_addonHelper->Log(LOG_DEBUG, "Plist archive lenght %lld (bitrate %f)", retVal, bitrate);
        return retVal;
    }
    
    int64_t PlaylistBuffer::GetPosition() const
    {
        if(m_delegate == nullptr) {
            m_addonHelper->Log(LOG_DEBUG, "Plist archive position =1");
            return -1;
        }
        m_addonHelper->Log(LOG_DEBUG, "Plist archive position %lld", m_position);
        return m_position;
    }
    
    int64_t PlaylistBuffer::Seek(int64_t iPosition, int iWhence)
    {
        if(m_delegate == nullptr)
            return -1;
        
        m_addonHelper->Log(LOG_DEBUG, "PlaylistBuffer::Seek. >>> Requested pos %lld, from %d", iPosition, iWhence);
        
        // Translate position to offset from start of buffer.
        int64_t length = GetLength();
        int64_t begin = 0;
        
        if(iWhence == SEEK_CUR) {
            iPosition = m_position + iPosition;
        } else if(iWhence == SEEK_END) {
            iPosition = length + iPosition;
        }
        if(iPosition < 0 )
            m_addonHelper->Log(LOG_DEBUG, "PlaylistBuffer::Seek. Can't be pos %lld", iPosition);

        if(iPosition > length) {
            iPosition = length;
        }
        if(iPosition < begin) {
            iPosition = begin;
        }
        iWhence = SEEK_SET;
        m_addonHelper->Log(LOG_DEBUG, "PlaylistBuffer::Seek. Calculated pos %lld", iPosition);
        
        int64_t seekDelta =  iPosition - m_position;
        if(seekDelta == 0)
            return m_position;
        
        // Do we have the data in cache already?
        float bitrate = 0.0;
        time_t requestedTimeshit, curTimeshift;
        time_t writeTimeshift = 0, readTimshift;
        bool haveCachedSeg = false, shouldReinit = false;
        {
            CLockObject lock(m_syncAccess);
            bitrate = Bitrate();
            requestedTimeshit = iPosition / (bitrate + 0.001);
            curTimeshift = m_position / (bitrate + 0.001);
            
            for (auto& seg: m_segments) {
                if(seg.first > requestedTimeshit || seg.first + seg.second->Duration()  < requestedTimeshit)
                    continue;
                // We have a segment
                haveCachedSeg = true;
                readTimshift = seg.first;
                m_addonHelper->Log(LOG_DEBUG, "PlaylistBuffer::Seek. Found cached segment. timeshift %d", readTimshift);
                size_t posInSegment = requestedTimeshit - seg.first * bitrate;
                seg.second->Seek(posInSegment);
                
                // continue write after continuesly last cahed segment after the found one
                writeTimeshift = readTimshift;
                TSegments::mapped_type segData;
                do{
                    segData = m_segments.at(writeTimeshift);
                    writeTimeshift += segData->Duration();
                } while(m_segments.count(writeTimeshift) > 0);
                shouldReinit = haveCachedSeg && m_writeTimshift != writeTimeshift;
                break;
            }
        }
        if(shouldReinit) {
            time_t adjustedTimeshift = writeTimeshift;
            std::string newUrl = m_delegate->UrlForTimeshift(writeTimeshift, &adjustedTimeshift);
            if(writeTimeshift != adjustedTimeshift ) {
                writeTimeshift = adjustedTimeshift;
            }

            m_addonHelper->Log(LOG_DEBUG, "PlaylistBuffer::Seek. Restart write from %lld sec. Read at %lld sec", writeTimeshift, readTimshift);
            
            Init(newUrl,  iPosition, writeTimeshift);
        }
        if(haveCachedSeg) {
            m_readTimshift = readTimshift;
            return m_position;
        }
        
        // No data in cache - reinit writer
        try {
//            if(s_SeekTimeout.TimeLeft() > 0)
//            {
//                m_addonHelper->Log(LOG_DEBUG, "PlaylistBuffer::Seek. >>> Can't seek now, wait %d ms", s_SeekTimeout.TimeLeft());
//                CEvent::Sleep(s_SeekTimeout.TimeLeft());
//            }

            time_t adjustedTimeshift = requestedTimeshit;
            std::string newUrl = m_delegate->UrlForTimeshift(requestedTimeshit, &adjustedTimeshift);
            if(requestedTimeshit != adjustedTimeshift ) {
                iPosition = adjustedTimeshift * bitrate;
                requestedTimeshit = adjustedTimeshift;
            }
            m_addonHelper->Log(LOG_DEBUG, "PlaylistBuffer::Seek. Delta %f Mb, %lld(%lld) sec", seekDelta/1024.0/1024.0, (requestedTimeshit - curTimeshift), requestedTimeshit);

            // Flour to segment duration (10 sec)
            time_t segTimeshift = requestedTimeshit / 10 * 10;
            Init(newUrl,  iPosition, segTimeshift);

            // If we can't read from strean in 60 sec - report error
            if(!m_writeEvent.Wait(60000)) {
                m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer::Seek. failed to read after seek in 60 sec.");
                return -1;
            }
            {
                CLockObject lock(m_syncAccess);
                auto& seg = m_segments.at(segTimeshift);
                size_t posInSegment = iPosition - segTimeshift * bitrate;
                seg->Seek(posInSegment);
                m_position = iPosition;
            }
            m_addonHelper->Log(LOG_DEBUG, "PlaylistBuffer::Seek. pos after seek %lld", m_position);
            return m_position;

        } catch (std::exception&  ex) {
            m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer::Seek. Exception thrown. Reason: %s. Reset position!", ex.what());
            m_position = 0;
            return -1;

        }catch (...) {
            m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer::Seek. Unknown exception. Reset position!");
            m_position = 0;
            return -1;
        }
    }
    
    bool PlaylistBuffer::StopThread(int iWaitMs)
    {
        int stopCounter = 0;
        bool retVal = false;
        while(!(retVal = this->CThread::StopThread(iWaitMs))){
            if(++stopCounter > 3)
                break;
            m_addonHelper->Log(LOG_NOTICE, "PlaylistBuffer: can't stop thread in %d ms (%d)", iWaitMs, stopCounter);
        }
        if(!retVal)
            m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer: failed to stop thread in %d ms", stopCounter*iWaitMs);
        
        return retVal;
    }
    
#pragma mark - Segment
    
    PlaylistBuffer::Segment::Segment(float duration)
    :_data(NULL)
    , _size(0)
    , _begin(NULL)
    , _duration(duration)
    {
    }
    
    PlaylistBuffer::Segment::Segment(const uint8_t* buffer, size_t size, float duration)
    : _duration(duration)
    , _begin(NULL)
    {
        Push(buffer, size);
    }
    
    void PlaylistBuffer::Segment::Push(const uint8_t* buffer, size_t size)
    {
        if(NULL == buffer || 0 == size)
            return;
        
        void * ptr = realloc(_data, _size + size);
        if(NULL == ptr)
            throw PlistBufferException("Faied to allocate segmwnt.");
        _data = (uint8_t*) ptr;
        memcpy(&_data[_size], buffer, size);
        _size += size;
        //    m_addonHelper->Log(LOG_DEBUG, ">>> Size: %d", _size);
    }
    
    const uint8_t* PlaylistBuffer::Segment::Pop(size_t requesred, size_t*  actual)
    {
        if(_begin == NULL)
            _begin = &_data[0];
        
        size_t available = _size - (_begin - &_data[0]);
        *actual = std::min(requesred, available);
        const uint8_t* retVal = _begin;
        _begin += *actual;
        return retVal;
    }
    
    size_t PlaylistBuffer::Segment::Seek(size_t position)
    {
        _begin = &_data[0] + std::min(position, _size);
        return Position();
    }


    size_t PlaylistBuffer::Segment::Read(uint8_t* buffer, size_t size)
    {
        if(_begin == NULL)
            _begin = &_data[0];
        size_t actual = std::min(size, BytesReady());
        //    m_addonHelper->Log(LOG_DEBUG, ">>> Available: %d  Actual: %d", available, actual);
        memcpy(buffer, _begin, actual);
        _begin += actual;
        return actual;
    }
    
    
    PlaylistBuffer::Segment::~Segment()
    {
        if(_data != NULL)
            delete _data;
    }
    
}
