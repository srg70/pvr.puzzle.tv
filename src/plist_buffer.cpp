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
    
    void PlaylistBuffer::Init(const std::string &playListUrl)
    {
        StopThread(20000);
        {
            CLockObject lock(m_syncAccess);

            m_writeEvent.Reset();
            m_segmentUrls.clear();
            m_segments.clear();
            m_position = 0;
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
        
        m_lastSegment = 0;
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
    
    void PlaylistBuffer::ParsePlaylist(const std::string& data)
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
            
            auto mediaIndex = 0;
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
            while(std::string::npos != pos) {
                pos += strlen(c_INF);
                body = body.substr(pos);
                float duration = std::stof (body, &pos);
                if(',' != body[pos++])
                    throw PlistBufferException("Invalid playlist format: no coma after INT tag.");
                
                std::string::size_type urlPos = body.find('\n',pos) + 1;
                pos = body.find(c_INF, urlPos);
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
            m_addonHelper->Log(LOG_DEBUG, "m_segmentUrls.size = %d", m_segmentUrls.size() );
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
    }
    
    bool PlaylistBuffer::FillSegment(const PlaylistBuffer::TSegmentUrls::mapped_type& segment, float* _)
    {
        unsigned char buffer[8196];
        void* f = m_addonHelper->OpenFile(segment.second.c_str(), XFILE::READ_NO_CACHE | XFILE::READ_CHUNKED); //XFILE::READ_AUDIO_VIDEO);
        if(!f)
            throw PlistBufferException("Failed to download playlist media segment.");
        
        auto segmentData = new TSegments::value_type::element_type(segment.first);
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
                m_segments.push_back(TSegments::value_type(segmentData));
                m_totalDuration += segmentData->Duration();
                m_totalLength += segmentData->Length();
                bitrate = Bitrate();
                segmantsSize = m_segments.size();
            }
        }
        m_addonHelper->Log(LOG_DEBUG, ">>> Segment added (%d). Bitrate %f", segmantsSize, segmentData->Bitrate());
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
                    std::string data;
                    LoadPlaylist(data);
                    ParsePlaylist(data);
                }
                
                //isEof = m_segmentUrls.size() == 0;
                auto it = m_segmentUrls.begin();
                auto end = m_segmentUrls.end();
                float sleepTime = 1; //Min sleep time 1 sec
//                float totalBitrate = 0.0;
                while (it != end && !isEof  && !IsStopped()) {
//                    float bitrate = 0.0;
                    isEof = FillSegment(it->second, nullptr);
//                    m_addonHelper->Log(LOG_DEBUG, ">>> Average bitrate: %f", m_bitrate);

//                    totalBitrate += bitrate;
                    auto duration = it->second.first;
                    sleepTime = duration;
                    m_addonHelper->Log(LOG_DEBUG, ">>> Segment duration: %f", duration);
                    //                sleepInterval += duration;
                    ++it;
                    if(!IsStopped())
                        m_writeEvent.Signal();
                }
//                if(m_bitrate == 0.0 && totalBitrate != 0.0){
//                    m_bitrate =  totalBitrate / m_segmentUrls.size();
//                    m_addonHelper->Log(LOG_DEBUG, ">>> Average Bitrate: %f", m_bitrate);
//                }

                if(!IsStopped())
                    m_segmentUrls.clear();

                if(m_isVod) {
                    m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: write is done");
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
            TSegments::value_type segment = NULL;
            {
                CLockObject lock(m_syncAccess);
                if(!m_segments.empty())
                    segment = m_segments.front();
            }
            // Retry 1 time after write operation
            if(NULL == segment && m_writeEvent.Wait(1000))
            {
                CLockObject lock(m_syncAccess);
                if(!m_segments.empty())
                    segment = m_segments.front();
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


            // Release empty segment
            if(segment->BytesReady() <= 0){
                CLockObject lock(m_syncAccess);
                if(!m_segments.empty()) {
                    m_segments.pop_front();
                    m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: Segment removed (%d)", m_segments.size());
                }
            }
            
            //        m_position += bytesRead;
            
        }
        m_position += totalBytesRead;
//        m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer::Read(): totalBytesRead %d, position %lld", totalBytesRead, m_position);

        bool hasMoreData = false;
        {
            CLockObject lock(m_syncAccess);
            hasMoreData = m_segments.size() > 0  ||  (!IsStopped() && IsRunning());
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
        if(m_isVod || m_delegate == nullptr) {
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
        if(m_isVod || m_delegate == nullptr) {
            m_addonHelper->Log(LOG_DEBUG, "Plist archive position =1");
            return -1;
        }
        m_addonHelper->Log(LOG_DEBUG, "Plist archive position %lld", m_position);
        return m_position;
    }
    
    int64_t PlaylistBuffer::Seek(int64_t iPosition, int iWhence)
    {
        if(m_isVod || m_delegate == nullptr)
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
        
        // If we seek forward for less than 5M, just read, Avoid reinitialization of playlist.
        if(seekDelta > 0 && seekDelta < 5 * 1024 * 1024) {
            unsigned char* dummy = new unsigned char[seekDelta];
            if(dummy) {
                Read(&dummy[0], seekDelta, 0);
                delete [] dummy;
                dummy = nullptr;
            }
            return m_position;
        }
        
        try {
            
            float bitrate = 0.0;
            {
                CLockObject lock(m_syncAccess);
                bitrate = Bitrate();
            }

            time_t requestedTimeshit = iPosition / (bitrate + 0.001);
            time_t adjustedTimeshift = requestedTimeshit;
            Init(m_delegate->UrlForTimeshift(requestedTimeshit, &adjustedTimeshift));
            // If we can't read from strean in 60 sec - report error
            if(!m_writeEvent.Wait(60000)) {
                m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer::Seek. failed to read after seek in 60 sec.", m_position);
                return -1;
            }

            if(requestedTimeshit == adjustedTimeshift )
                m_position =  iPosition;
            else
                m_position =  adjustedTimeshift * bitrate;
        } catch (std::exception&  ex) {
            m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer::Seek. Exception thrown. Reason: %s. Reset position!", ex.what());
            m_position = 0;
            return -1;

        }catch (...) {
            m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer::Seek. Unknown exception. Reset position!");
            m_position = 0;
            return -1;
        }
        m_addonHelper->Log(LOG_DEBUG, "PlaylistBuffer::Seek. pos after seek %lld", m_position);
        return m_position;
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
