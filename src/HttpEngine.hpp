/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
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

#ifndef HttpEngine_hpp
#define HttpEngine_hpp

#if (defined(_WIN32) || defined(__WIN32__))
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define CURL_STATICLIB 1
#ifdef GetObject
#undef GetObject
#endif
#endif

#include <curl/curl.h>

#include "libXBMC_pvr.h"
#include <map>
#include <exception>
#include "ActionQueue.hpp"

class QueueNotRunningException : public std::exception
{
public:
    const char* what() const noexcept {return reason.c_str();}
    const std::string reason;
    QueueNotRunningException(const char* r = "") : reason(r) {}
    
};

class HttpEngine
{
public:
    typedef std::map<std::string, std::string> TCoocies;
    
    HttpEngine (ADDON::CHelper_libXBMC_addon * addonHelper);
    ~HttpEngine();
    
    template <typename TParser, typename TCompletion>
    void CallApiAsync(const std::string& request, TParser parser, TCompletion completion)
    {
        if(!m_apiCalls->IsRunning())
            throw QueueNotRunningException("API request queue in not running.");
        
        m_apiCalls->PerformAsync([=](){
            SendHttpRequest(request, m_sessionCookie, parser,
                            [=](const CActionQueue::ActionResult& s) { completion(s);});
        },[=](const CActionQueue::ActionResult& s) {
            if(s.status != CActionQueue::kActionCompleted)
                completion(s);
        });
    }
    
    template <typename TParser, typename TCompletion>
    void RunOnCompletionQueueAsync(TParser parser, TCompletion completion)
    {
        m_apiCallCompletions->PerformAsync(parser,  completion);
    }
   
     TCoocies m_sessionCookie;

private:
    static size_t CurlWriteData(void *buffer, size_t size, size_t nmemb, void *userp);

    template <typename TResultCallback, typename TCompletion>
    void SendHttpRequest(const std::string &url,const TCoocies &cookie, TResultCallback result, TCompletion completion) const
    {
        char *errorMessage[CURL_ERROR_SIZE];
        std::string* response = new std::string();
        CURL *curl = curl_easy_init();
        if (curl)
        {
            auto start = P8PLATFORM::GetTimeMs();
            m_addonHelper->Log(ADDON::LOG_INFO, "Sending request: %s", url.c_str());
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorMessage);
            
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteData);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
            
            std::string cookieStr;
            auto itCookie = cookie.begin();
            for(; itCookie != cookie.end(); ++itCookie)
            {
                if (itCookie != cookie.begin())
                    cookieStr += "; ";
                cookieStr += itCookie->first + "=" + itCookie->second;
            }
            curl_easy_setopt(curl, CURLOPT_COOKIE, cookieStr.c_str());
            
            long httpCode = 0;
            int retries = 5;
            while (retries-- > 0)
            {
                CURLcode curlCode = curl_easy_perform(curl);
                if (curlCode != CURLE_OK)
                {
                    m_addonHelper->Log(ADDON::LOG_ERROR, "%s: %s", __FUNCTION__, errorMessage);
                    break;
                }
                
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                
                if (httpCode != 503) // temporarily unavailable
                    break;
                
                m_addonHelper->Log(ADDON::LOG_INFO, "%s: %s", __FUNCTION__, "HTTP error 503 (temporarily unavailable)");
                
                P8PLATFORM::CEvent::Sleep(1000);
            }
            m_addonHelper->Log(ADDON::LOG_INFO, "Got HTTP response (%d) in %d ms", httpCode,  P8PLATFORM::GetTimeMs() - start);
            
            if (httpCode != 200)
                *response = "";
            
            curl_easy_cleanup(curl);
            if(!m_apiCallCompletions->IsRunning())
                throw QueueNotRunningException("API call completion queue in not running.");
            
            m_apiCallCompletions->PerformAsync([=]() {
                result(*response);
            }, [=](const CActionQueue::ActionResult& s) {
                delete response;
                completion(s);
            });
        }
    }
    
    ADDON::CHelper_libXBMC_addon *m_addonHelper;
    CActionQueue* m_apiCalls;
    CActionQueue* m_apiCallCompletions;

};



#endif /* HttpEngine_hpp */
