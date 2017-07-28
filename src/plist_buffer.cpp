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
#endif

#include "plist_buffer.h"
#include "libXBMC_addon.h"

using namespace P8PLATFORM;
using namespace ADDON;


// trim from start (in place)
static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// trim from both ends (in place)
static inline void trim(std::string &s) {
    ltrim(s);
    rtrim(s);
}

static std::string ToAbsoluteUrl(const std::string& url, const std::string& baseUrl){
    const char* c_HTTP = "http://";

    // If Reliative URL
    if(std::string::npos == url.find(c_HTTP)) {
        // Append site prefix...
        auto urlPos = baseUrl.find(c_HTTP);
        if(std::string::npos == urlPos)
            throw InputBufferException((std::string("Missing http:// in base URL: ") + baseUrl).c_str());
        urlPos = baseUrl.rfind('/');
        if(std::string::npos == urlPos)
            throw InputBufferException((std::string("No '/' in base URL: ") + baseUrl).c_str());
        return baseUrl.substr(0, urlPos + 1) + url;
    }
    return url;

}
PlaylistBuffer::PlaylistBuffer(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &playListUrl) :
    m_addonHelper(addonHelper)
{
    Init(playListUrl);
}

PlaylistBuffer::~PlaylistBuffer()
{
    StopThread();
}

void PlaylistBuffer::Init(const std::string &playListUrl)
{
    StopThread();
    m_writeEvent.Reset();
    m_segmentUrls.clear();
    m_segments.clear();
    SetBestPlaylist(playListUrl);
    CreateThread();
}

static uint64_t ParseXstreamInfTag(const std::string& data, std::string& url)
{
    const char* c_BAND = "BANDWIDTH=";

    auto pos = data.find(c_BAND);
    if(std::string::npos == pos)
        throw InputBufferException("Invalid playlist format: missing BANDWIDTH in #EXT-X-STREAM-INF tag.");
    pos += strlen(c_BAND);
    uint64_t bandwidth = std::stoull(data.substr(pos), &pos);
    pos = data.find('\n', pos);
    if(std::string::npos == pos)
        throw InputBufferException("Invalid playlist format: missing NEW LINE in #EXT-X-STREAM-INF tag.");
    url = data.substr(++pos);
    rtrim(url);
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

    try {
        auto pos = data.find(c_M3U);
        if(std::string::npos == pos)
            throw InputBufferException("Invalid playlist format: missing #EXTM3U tag.");
        pos += strlen(c_M3U);
        
        pos = data.find(c_SEQ);
        if(std::string::npos == pos)
            throw InputBufferException("Invalid playlist format: missing #EXT-X-MEDIA-SEQUENCE tag.");
        pos += strlen(c_SEQ);
        std::string  body = data.substr(pos);
        auto mediaIndex = std::stoull(body, &pos);
        
        pos = body.find(c_INF, pos);
        while(std::string::npos != pos) {
            pos += strlen(c_INF);
            body = body.substr(pos);
            float duration = std::stof (body, &pos);
            if(',' != body[pos++])
                throw InputBufferException("Invalid playlist format: no coma after INT tag.");

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
            url = ToAbsoluteUrl(url, m_playListUrl);
//            m_addonHelper->Log(LOG_NOTICE, "IDX: %u Duration: %f. URL: %s", mediaIndex, duration, url.c_str());
            m_segmentUrls[mediaIndex++]  = TSegmentUrls::mapped_type(duration, url);
        }
    } catch (...) {
        m_addonHelper->Log(LOG_ERROR, "Bad M3U : \n %s", data.c_str() );
        throw;
    }
}

void PlaylistBuffer::LoadPlaylist(std::string& data)
{
    char buffer[1024];
    auto f = m_addonHelper->OpenFile(m_playListUrl.c_str(), 0);
    if (!f)
        throw InputBufferException("Failed to obtain playlist from server.");
    bool isEof = false;
    do{
        buffer[0]= 0;
        auto bytesRead = m_addonHelper->ReadFile(f, buffer, sizeof(buffer));
        isEof = bytesRead <= 0;
        data += &buffer[0];
    }while(!isEof);
    m_addonHelper->CloseFile(f);
}

bool PlaylistBuffer::FillSegment(const PlaylistBuffer::TSegmentUrls::mapped_type& segment)
{
    unsigned char buffer[8196];
    void* f = m_addonHelper->OpenFile(segment.second.c_str(), XFILE::READ_NO_CACHE | XFILE::READ_CHUNKED); //XFILE::READ_AUDIO_VIDEO);
    if(!f)
        throw InputBufferException("Failed to download playlist media segment.");

    auto segmentData = new TSegments::value_type::element_type();
//    segmentData->m_addonHelper = m_addonHelper;
    ssize_t  bytesRead;
    do {
        bytesRead = m_addonHelper->ReadFile(f, buffer, sizeof(buffer));
        segmentData->Push(buffer, bytesRead);
//        m_addonHelper->Log(LOG_DEBUG, ">>> Write: %d", bytesRead);
    }while (bytesRead > 0 && !IsStopped());
    
    m_addonHelper->CloseFile(f);
    
    CLockObject lock(m_syncAccess);
    m_segments.push_back(TSegments::value_type(segmentData));
//    m_addonHelper->Log(LOG_DEBUG, ">>> Segment added (%d)", m_segments.size());

    return bytesRead < 0; // 0 (i.e. EOF) means no error, caller may continue with next chunk
}

void *PlaylistBuffer::Process()
{
    bool isEof = false;
    try {
        while (!isEof  && !IsStopped()) {
            if(m_segmentUrls.empty()) {
                std::string data;
                LoadPlaylist(data);
                ParsePlaylist(data);
            }

            //isEof = m_segmentUrls.size() == 0;
            auto it = m_segmentUrls.begin();
            auto end = m_segmentUrls.end();
//            float sleepInterval = 0;
            float sleepTime = 1000; //Min sleep time 1 sec
            while (it != end && !isEof  && !IsStopped()) {
                isEof = FillSegment(it->second);
                auto duration = it->second.first;
                sleepTime = duration * 1000;
                m_addonHelper->Log(LOG_DEBUG, ">>> Segment duration: %f", duration);
//                sleepInterval += duration;
                ++it;
                m_writeEvent.Signal();
            }
            m_segmentUrls.clear();
            
//            sleepInterval = 10;
//            while(sleepInterval > 0 && !isEof && !IsStopped()) {
//                m_addonHelper->Log(LOG_DEBUG, ">>> Write threads sleep: %f ", sleepInterval);
//                sleepInterval -= 1,0;
//                P8PLATFORM::CEvent::Sleep(1000);
//            }
            Sleep(sleepTime);
        }
        
    } catch (InputBufferException& ex ) {
        m_addonHelper->Log(LOG_ERROR, "Playlist download thread failed with error: %s", ex.what());
    }
    
    return NULL;
}

ssize_t PlaylistBuffer::Read(unsigned char *buffer, size_t bufferSize)
{
    
    size_t totalBytesRead = 0;
//    int32_t timeout = c_commonTimeoutMs;
    
    while (totalBytesRead < bufferSize && IsRunning())
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
//            m_addonHelper->Log(LOG_DEBUG, ">>> Read: %d", bytesRead);
            totalBytesRead += bytesRead;
        }while(bytesToRead > 0 && bytesRead > 0);
        
        // Release empty segment
        if(segment->BytesReady() <= 0 ){
            CLockObject lock(m_syncAccess);
            if(!m_segments.empty()) {
                m_segments.pop_front();
                m_addonHelper->Log(LOG_DEBUG, ">>> PlaylistBuffer: Segment removed (%d)", m_segments.size());
            }
        }
        
//        m_position += bytesRead;

    }
    
    return (IsStopped() || !IsRunning()) ? -1 :totalBytesRead;
}

int64_t PlaylistBuffer::Seek(int64_t iPosition, int iWhence)
{
    return -1;
}

bool PlaylistBuffer::SwitchStream(const std::string &newUrl)
{
    bool succeeded = false;
    try {
        Init(newUrl);
        succeeded = true;
    } catch (const InputBufferException& ex) {
        m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer: Failed to switch streams. Error: %s", ex.what());
    }
    
    return succeeded;
}

int64_t PlaylistBuffer::GetLength() const
{
    return -1;
}

int64_t PlaylistBuffer::GetPosition() const
{
    return -1;
}

bool PlaylistBuffer::StopThread(int iWaitMs)
{
    int stopCounter = 1;
    bool retVal = false;
    while(!(retVal = this->CThread::StopThread(iWaitMs))){
        if(stopCounter++ > 3)
            break;
        m_addonHelper->Log(LOG_NOTICE, "PlaylistBuffer: can't stop thread in %d ms", iWaitMs);
    }
    if(!retVal)
        m_addonHelper->Log(LOG_ERROR, "PlaylistBuffer: can't stop thread in %d ms", stopCounter*iWaitMs);
    
    return retVal;
}


PlaylistBuffer::Segment::Segment()
    :_data(NULL), _size(0), _begin(_data)
{}

PlaylistBuffer::Segment::Segment(const uint8_t* buffer, size_t size)
{
    Push(buffer, size);
}

void PlaylistBuffer::Segment::Push(const uint8_t* buffer, size_t size)
{
    if(NULL == buffer || 0 == size)
        return;
    
    void * ptr = realloc(_data, _size + size);
    if(NULL == ptr)
        throw InputBufferException("Faied to allocate segmwnt.");
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
               
