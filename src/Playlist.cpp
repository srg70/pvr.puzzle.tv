//
//  Playlist.cpp
//  comple.test
//
//  Created by Sergey Shramchenko on 30/10/2018.
//  Copyright © 2018 Home. All rights reserved.
//

#include "Playlist.hpp"
#include "globals.hpp"
#include "helpers.h"

namespace Buffers {
    using namespace Globals;
    
    static std::string ToAbsoluteUrl(const std::string& url, const std::string& baseUrl){
        const char* c_HTTP = "http://";
        
        // If Reliative URL
        if(std::string::npos == url.find(c_HTTP)) {
            // Append site prefix...
            auto urlPos = baseUrl.find(c_HTTP);
            if(std::string::npos == urlPos)
                throw PlaylistException((std::string("Missing http:// in base URL: ") + baseUrl).c_str());
            urlPos = baseUrl.rfind('/');
            if(std::string::npos == urlPos)
                throw PlaylistException((std::string("No '/' in base URL: ") + baseUrl).c_str());
            return baseUrl.substr(0, urlPos + 1) + url;
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
    
    Playlist::Playlist(const std::string &url){
        SetBestPlaylist(url);
    }
    
    void Playlist::SetBestPlaylist(const std::string& playlistUrl)
    {
        const char* c_XINF = "#EXT-X-STREAM-INF:";
        
        m_loadIterator = 0;
        m_playListUrl = playlistUrl;
        std::string data;
        LoadPlaylist(data);
        auto pos = data.find(c_XINF);
        // Do we have bitstream info to choose best strea?
        if(std::string::npos != pos) {
            //    LogDebug("Variant playlist URL: \n %s", playlistUrl.c_str() );
            //    LogDebug("Variant playlist: \n %s", data.c_str() );
            
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
            //    LogDebug("Best URL (%d): \n %s", bestRate, m_playListUrl.c_str() );
            
            LoadPlaylist(data);
        }
        
        ParsePlaylist(data);
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
        
        try {
            auto pos = data.find(c_M3U);
            if(std::string::npos == pos)
                throw PlaylistException("Invalid playlist format: missing #EXTM3U tag.");
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
                    throw PlaylistException("Invalid playlist format: missing #EXT-X-MEDIA-SEQUENCE and #EXT-X-PLAYLIST-TYPE tag.");
                pos+= strlen(c_TYPE);
                body = data.substr(pos);
                std::string vod("VOD");
                if(body.substr(0,vod.size()) != vod)
                    throw PlaylistException("Invalid playlist format: VOD playlist expected.");
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
                    throw PlaylistException("Invalid playlist format: missing coma after INF tag.");
                
                std::string::size_type urlPos = body.find('\n',pos) + 1;
                pos = body.find(c_INF, urlPos);
                hasContent = true;
                // Check whether we have a segment already
                if(m_segmentUrls.count(mediaIndex) != 0) {
                    ++mediaIndex;
                    continue;
                }
                std::string::size_type urlLen = pos - urlPos;
                if(std::string::npos == pos)
                    urlLen = std::string::npos;
                auto url = body.substr(urlPos, urlLen);
                trim(url);
                url = ToAbsoluteUrl(url, m_playListUrl);
                //            LogNotice("IDX: %u Duration: %f. URL: %s", mediaIndex, duration, url.c_str());
                m_segmentUrls[mediaIndex++] = TSegmentUrls::mapped_type(duration, url);
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
        
        LogDebug(">>> PlaylistBuffer: (re)loading playlist %s.", m_playListUrl.c_str());
        
        auto f = XBMC->OpenFile(m_playListUrl.c_str(), XFILE::READ_NO_CACHE);
        if (!f)
            throw PlaylistException("Failed to obtain playlist from server.");
        bool isEof = false;
        do{
            buffer[0]= 0;
            auto bytesRead = XBMC->ReadFile(f, buffer, sizeof(buffer));
            isEof = bytesRead <= 0;
            data.append(&buffer[0], bytesRead);
        }while(!isEof);
        XBMC->CloseFile(f);
        
        LogDebug(">>> PlaylistBuffer: (re)loading done. Content: \n%s", data.size()> 16000 ? "[More that 16K]" : data.c_str());
        
    }
    
    bool Playlist::NextSegment(const SegmentInfo** ppInfo, bool& hasMoreSegments) {
        hasMoreSegments = false;
        if(m_segmentUrls.count(m_loadIterator) != 0) {
            *ppInfo = &m_segmentUrls[m_loadIterator++];
            hasMoreSegments = m_segmentUrls.count(m_loadIterator) > 0;
            return true;
        }
        *ppInfo = nullptr;
        // For VOD plist NO refresh is available
        // So return EOF right now.
        if(m_isVod)
            return false;
        std::string data;
        LoadPlaylist(data);
        // Empty playlist treat as EOF.
        if(!ParsePlaylist(data))
            return false;
        if(m_segmentUrls.count(m_loadIterator) != 0) {
            *ppInfo = &m_segmentUrls[m_loadIterator++];
            hasMoreSegments = m_segmentUrls.count(m_loadIterator) > 0;
        }
        return true;
    }
    
}
