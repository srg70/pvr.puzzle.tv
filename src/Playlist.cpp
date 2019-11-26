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
#include <inttypes.h>
#include "HttpEngine.hpp"
#include "Playlist.hpp"
#include "globals.hpp"
#include "helpers.h"
#include "p8-platform/util/StringUtils.h"

namespace Buffers {
using namespace Globals;

static std::string ToAbsoluteUrl(const std::string& url, const std::string& baseUrl){
    const char* c_HTTP = "http://";
    const char* c_HTTPS = "https://";
    
    // If Reliative URL
    if(std::string::npos == url.find(c_HTTP)) {
        std::string absUrl;
        // Append site prefix...
        // Find schema.
        auto urlPos = baseUrl.find(c_HTTP);
        if(std::string::npos != urlPos){
            urlPos += strlen(c_HTTP);
        } else {
            urlPos = baseUrl.find(c_HTTPS);
            if(std::string::npos == urlPos)
                throw PlaylistException((std::string("Missing http:// or https:// in base URL: ") + baseUrl).c_str());
            urlPos += strlen(c_HTTPS);
        }
        // Find site
        urlPos = baseUrl.find('/', urlPos);
        if(std::string::npos == urlPos) {
            throw PlaylistException((std::string("Missing site address in base URL: ") + baseUrl).c_str());
        }
        absUrl = baseUrl.substr(0, urlPos + 1); // +1 for '/'
        std::string urlPath = baseUrl.substr(urlPos + 1);
        // Find path
        urlPos = urlPath.rfind('/');
        if(std::string::npos != urlPos) {
            //throw PlaylistException((std::string("No '/' in base URL: ") + baseUrl).c_str());
            urlPath = urlPath.substr(0, urlPos + 1); // +1 for '/'
        }
        urlPath += url;
        absUrl += urlPath;//HttpEngine::Escape(urlPath);
        //trim(absUrl);
        return absUrl;
        
    }
    return url;
    
}

static uint64_t ParseXstreamInfTag(const std::string& data, std::string& url)
{
    const char* c_BAND = "BANDWIDTH=";
    
    auto pos = data.find(c_BAND);
    if(std::string::npos == pos)
        throw PlaylistException("Invalid playlist format: missing BANDWIDTH in #EXT-X-STREAM-INF tag.");
    pos += strlen(c_BAND);
    uint64_t bandwidth = std::stoull(data.substr(pos), &pos);
    pos = data.find('\n', pos);
    if(std::string::npos == pos)
        throw PlaylistException("Invalid playlist format: missing NEW LINE in #EXT-X-STREAM-INF tag.");
    url = data.substr(++pos);
    trim(url);
    return bandwidth;
}

static bool IsPlaylistContent(const std::string& content) {
    const char* c_M3U = "#EXTM3U";
    return  content.find(c_M3U) != std::string::npos;
}

Playlist::Playlist(const std::string &urlOrContent, uint64_t indexOffset)
: m_indexOffset(indexOffset)
, m_targetDuration(0)
, m_initialInternalIndex(-1)
{
    const std::string* pData (&urlOrContent);
    std::string data;
    if(!IsPlaylistContent(urlOrContent)){
        m_playListUrl = urlOrContent;
        
        // Kodi's HTTP headers schema.
        LogDebug("Playlist::Playlist(): original URL %s", m_playListUrl.c_str());
        auto headersPos = m_playListUrl.find("|");
        if(headersPos != std::string::npos){
            m_httplHeaders = m_playListUrl.substr(headersPos);
            m_playListUrl = m_playListUrl.substr(0,headersPos);
            LogDebug("Playlist::Playlist(): HTTP headers %s", m_httplHeaders.c_str());
        }
        
        LoadPlaylist(data);
        pData = &data;
    }
    SetBestPlaylist(*pData);
    
}

void Playlist::SetBestPlaylist(const std::string& data)
{
    const char* c_XINF = "#EXT-X-STREAM-INF:";
    m_loadIterator = 0;
    auto pos = data.find(c_XINF);
    // Do we have bitstream info to choose best strea?
    if(std::string::npos != pos) {
        //    LogDebug("Variant playlist URL: \n %s", playlistUrl.c_str() );
        //    LogDebug("Variant playlist: \n %s", data.c_str() );
        
        uint64_t bestRate = 0;
        while(std::string::npos != pos)
        {
            pos += strlen(c_XINF);
            auto endTag = data.find("#",pos);
            std::string::size_type tagLen = endTag - pos;
            if(std::string::npos == endTag)
                tagLen = std::string::npos;
            
            std::string tagBody = data.substr(pos, tagLen);
            pos = data.find(c_XINF,pos);
            
            std::string url;
            uint64_t rate = ParseXstreamInfTag(tagBody, url);
            if(rate > bestRate) {
                m_playListUrl = ToAbsoluteUrl(url, m_effectivePlayListUrl);
                bestRate = rate;
            }
        }
        //    LogDebug("Best URL (%d): \n %s", bestRate, m_playListUrl.c_str() );
        std::string newData;
        LoadPlaylist(newData);
        ParsePlaylist(newData);
    } else {
        ParsePlaylist(data);
    }
    
    // Init load iterator with media index of first segment
    const auto first = m_segmentUrls.begin();
    if( first != m_segmentUrls.end()){
        m_loadIterator = first->first;
    }
}

bool Playlist::ParsePlaylist(const std::string& data)
{
    const char* c_M3U = "#EXTM3U";
    const char* c_INF = "#EXTINF:";
    const char* c_SEQ = "#EXT-X-MEDIA-SEQUENCE:";
    const char* c_TYPE = "#EXT-X-PLAYLIST-TYPE:";
    const char* c_TARGET = "#EXT-X-TARGETDURATION:";
    const char* c_CACHE = "#EXT-X-ALLOW-CACHE:"; // removed in v7 but in use by TTV :(
    const char* c_END = "#EXT-X-ENDLIST";
    
    try {
        auto pos = data.find(c_M3U);
        if(std::string::npos == pos)
            throw PlaylistException("Invalid playlist format: missing #EXTM3U tag.");
        pos += strlen(c_M3U);
        
        // Fill target duration value (mandatory tag)
        pos = data.find(c_TARGET);
        if(std::string::npos == pos)
            throw PlaylistException("Invalid playlist format: missing #EXT-X-TARGETDURATION tag.");
        
        pos += strlen(c_TARGET);
        m_targetDuration = std::stoi(data.substr(pos), &pos);
        
        // Playlist may be sub-sequence of some bigger stream (e.g. archive at Edem)
        // Initial index offset helps to right possitionig of segments range
        int64_t mediaIndex = m_indexOffset;
        // use playlist indexing
        std::string  body;
        pos = data.find(c_SEQ);
        // If we have media-sequence tag - use it
        if(std::string::npos != pos) {
            pos += strlen(c_SEQ);
            body = data.substr(pos);
            auto internaIndex = std::stoull(body, &pos);
            // Initialize internal index of playlist on first loading
            if(-1 == m_initialInternalIndex) {
                m_initialInternalIndex = internaIndex;
            }
            mediaIndex +=  internaIndex - m_initialInternalIndex;
        }
        
        // VOD should contain END tag
        pos = data.find(c_END);
        m_isVod = pos != std::string::npos;
        body=data.substr(0, pos);
        
        pos = body.find(c_INF);
        bool hasContent = false;
        // Load playlist segments
        while(std::string::npos != pos) {
            pos += strlen(c_INF);
            body = body.substr(pos);
            float duration = std::stof (body, &pos);
            if(',' != body[pos++])
                throw PlaylistException("Invalid playlist format: missing coma after INF tag.");
            
            const std::string::size_type urlPos = body.find('\n',pos) + 1;
            pos = body.find('\n', urlPos);
            hasContent = true;
            // Check whether we have a segment already
            if(m_segmentUrls.count(mediaIndex) == 0) {
                std::string::size_type urlLen = pos - urlPos;
                if(std::string::npos == pos)
                    urlLen = std::string::npos;
                auto url = body.substr(urlPos, urlLen);
                trim(url);
                url = ToAbsoluteUrl(url, m_effectivePlayListUrl) + m_httplHeaders;
                LogDebug("Plist::ParsePlist(): new segment URL IDX: #%" PRIu64 " Duration: %f. URL: %s", mediaIndex, duration, url.c_str());
                TimeOffset relativeSeegmetStart = GetTimeOffset();
                // Set reliative segment start time to either previous segment end time or 0.0 for first one
                if(m_segmentUrls.count(mediaIndex - 1)){
                    const auto& prevSegment = m_segmentUrls.at(mediaIndex - 1);
                    relativeSeegmetStart = prevSegment.startTime + prevSegment.duration;
                }
                m_segmentUrls[mediaIndex] = TSegmentUrls::mapped_type(relativeSeegmetStart, duration, url, mediaIndex);
            }
            ++mediaIndex;
            pos = body.find(c_INF, urlPos);
        }
        LogDebug("m_segmentUrls.size = %d, %s", m_segmentUrls.size(), hasContent ? "Not empty." : "Empty."  );
        return hasContent;
    } catch (std::exception& ex) {
        LogError("Bad M3U : parser error %s", ex.what() );
        LogError("M3U data : \n %s", data.c_str() );
        throw;
    } catch (...) {
        LogError("Bad M3U : \n %s", data.c_str() );
        throw;
    }
}


void Playlist::LoadPlaylist(std::string& data) const
{
    char buffer[1024];
    
    LogDebug(">>> PlaylistBuffer: (re)loading playlist %s", m_playListUrl.c_str());
    
    bool succeeded = false;
    try{
        m_effectivePlayListUrl.clear();
        
        std::vector<std::string> headers;
        if(!m_httplHeaders.empty()) {
            auto headerStrings = StringUtils::Split(m_httplHeaders.substr(1), "=");
            auto headerStringsSize = headerStrings.size();
            int i = 0;
            while(i < headerStringsSize) {
                const auto& headerName = headerStrings[i++];
                if(i >= headerStringsSize)
                    continue;
                const auto& headerValue = headerStrings[i++];
                headers.push_back(headerName + ": " + headerValue);
            }
        }
        
        HttpEngine::Request request(m_playListUrl, "", headers);
        HttpEngine::DoCurl(request, HttpEngine::TCoocies(), &data, 9999999, &m_effectivePlayListUrl);
        succeeded = true;
        
    }catch (std::exception& ex) {
        LogError("Playlist::LoadPlaylist(): STD exception: %s",  ex.what());
    }catch (...) {
        LogError("Playlist::LoadPlaylist(): unknown exception.");
    }
    
    if (!succeeded)
        throw PlaylistException("Failed to obtain playlist from server.");
    
    LogDebug(">>> PlaylistBuffer: playlist effective URL %s", m_effectivePlayListUrl.c_str());
    LogDebug(">>> PlaylistBuffer: (re)loading done. Content: \n%s", data.substr(0, 16000).c_str());
    
}

bool Playlist::Reload() {
    // For VOD plist we can't reload/refresh.
    if(m_isVod)
        return true;
    try {
        std::string data;
        LoadPlaylist(data);
        // Empty playlist treat as EOF.
        return ParsePlaylist(data);
    } catch (std::exception& ex) {
        LogError("Playlist: FAILED to reload playlist. Error: %s", ex.what());
    }
    return false;
}

bool Playlist::NextSegment(SegmentInfo& info, bool& hasMoreSegments) {
    hasMoreSegments = false;
    //        LogDebug("Playlist: searching for segment info #%" PRIu64 "...", m_loadIterator);
    if(m_segmentUrls.count(m_loadIterator) != 0) {
        info = m_segmentUrls[m_loadIterator++];
        hasMoreSegments = m_segmentUrls.count(m_loadIterator) > 0;
        //            LogDebug("Playlist: segment info is found. Has more? %s", hasMoreSegments ? "YES" : "NO");
        return true;
    }
    //        LogDebug("Playlist: segment info is missing");
    return false;
}

bool Playlist::SetNextSegmentIndex(uint64_t idx) {
    if(m_segmentUrls.count(idx) == 0) {
        LogDebug("Playlist: failed to set next segment to #%" PRIu64 ". m_segmentUrls contains serments [%" PRIu64 ", %" PRIu64 "].", idx, m_segmentUrls.begin()->first, (--m_segmentUrls.end())->first);
        return false;
    }
    m_loadIterator = idx;
    LogDebug("Playlist: next segment index been set to #%" PRIu64 ".", m_loadIterator);
    return true;
}
}
