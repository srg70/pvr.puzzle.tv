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

#include <map>
#include <string>
#include <exception>
#include "ActionQueue.hpp"
#include "globals.hpp"

class QueueNotRunningException : public std::exception
{
public:
    const char* what() const noexcept {return reason.c_str();}
    const std::string reason;
    QueueNotRunningException(const char* r = "") : reason(r) {}
    
};

class CurlErrorException : public std::exception
{
public:
    const char* what() const noexcept {return reason.c_str();}
    const std::string reason;
    CurlErrorException(const char* r = "") : reason(r) {}
    
};


class HttpEngine
{
public:
    
    enum RequestPriority{
        RequestPriority_Hi = 0,
        RequestPriority_Low
    };
    
    typedef std::map<std::string, std::string> TCoocies;
    
    HttpEngine ();
    ~HttpEngine();
    
    template <typename TParser, typename TCompletion>
    void CallApiAsync(const std::string& request, TParser parser, TCompletion completion, RequestPriority priority = RequestPriority_Low)
    {
        if(!m_apiCalls->IsRunning())
            throw QueueNotRunningException("API request queue in not running.");
        auto pThis = this;
        ActionQueue::TAction action = [pThis, request,  parser, completion, priority](){
            pThis->SendHttpRequest(request, pThis->m_sessionCookie, parser,completion, priority);
        };
        ActionQueue::TCompletion comp = [completion](const ActionQueue::ActionResult& s) {
            if(s.status != ActionQueue::kActionCompleted){
                completion(s);
            }
        };
        if(priority == RequestPriority_Hi)
            m_apiCalls->PerformHiPriority(action, comp);
        else
            m_apiCalls->PerformAsync(action, comp);
    }
    
    //template <typename TParser, typename TCompletion>
    void RunOnCompletionQueueAsync(ActionQueue::TAction action, ActionQueue::TCompletion completion)
    {
        m_apiCallCompletions->PerformAsync(action,  completion);
    }
    void CancelAllRequests();
    static void SetCurlTimeout(long timeout);
    static std::string Escape(const std::string& str);
    
    TCoocies m_sessionCookie;

    static bool CheckInternetConnection(long timeout);
    
    static void DoCurl(const std::string &url, const TCoocies &cookie, std::string* response, unsigned long long requestId = 0)
    {
        char errorMessage[CURL_ERROR_SIZE];
        CURL *curl = curl_easy_init();
        if (nullptr == curl)
            throw CurlErrorException("CURL initialisation failed.");
        
        auto start = P8PLATFORM::GetTimeMs();
        Globals::LogInfo("Sending request: %s. ID=%llu", url.c_str(), requestId);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorMessage);
        
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, c_CurlTimeout);
        
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
        CURLcode curlCode;
        
        while (retries-- > 0)
        {
            curlCode = curl_easy_perform(curl);
            
            if (curlCode == CURLE_OPERATION_TIMEDOUT)
            {
                Globals::LogError("CURL operation timeout! (%d sec). ID=%llu", c_CurlTimeout, requestId);
                break;
            }
            else if (curlCode != CURLE_OK)
            {
                Globals::LogError("CURL error %d. Message: %s. ID=%llu", curlCode, errorMessage, requestId);
                break;
            }
            
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            
            if (httpCode != 503) // temporarily unavailable
                break;
            
            Globals::LogInfo("%s: %s. ID=%llu", __FUNCTION__, "HTTP error 503 (temporarily unavailable)", requestId);
            
            P8PLATFORM::CEvent::Sleep(1000);
        }
        Globals::LogInfo("Got HTTP response (%d) in %d ms. ID=%llu", httpCode,  P8PLATFORM::GetTimeMs() - start, requestId);
        
        if (httpCode != 200)
            *response = "";
        
        curl_easy_cleanup(curl);
        if(curlCode != CURLE_OK){
            delete response;
            throw CurlErrorException(&errorMessage[0]);
        }
    }
    
private:
    static size_t CurlWriteData(void *buffer, size_t size, size_t nmemb, void *userp);
    static  long c_CurlTimeout;
    mutable unsigned long long m_DebugRequestId;

    template <typename TResultCallback, typename TCompletion>
    void SendHttpRequest(const std::string &url, const TCoocies &cookie, TResultCallback result, TCompletion completion, RequestPriority priority) const
    {
        std::string* response = new std::string();
        const unsigned long long requestId = m_DebugRequestId++;
        
        DoCurl(url, cookie, response, requestId);
        
        ActionQueue::TAction action = [result, response, requestId]() {
            Globals::LogDebug("Processing response. ID=%llu", requestId);
            result(*response);
        };
        ActionQueue::TCompletion comp =[completion, response, requestId](const ActionQueue::ActionResult& s) {
            delete response;
            Globals::LogDebug("Complete response. ID=%llu", requestId);
            completion(s);
        };
        if(priority == RequestPriority_Hi) {
            if(!m_apiHiPriorityCallCompletions->IsRunning()){
                delete response;
                throw QueueNotRunningException("API call completion queue in not running.");
            }
            m_apiHiPriorityCallCompletions->PerformAsync(action, comp);
        } else {
            if(!m_apiCallCompletions->IsRunning()){
                delete response;
                throw QueueNotRunningException("API call completion queue in not running.");
            }
            m_apiCallCompletions->PerformAsync(action, comp);
        }
    }
    
    ActionQueue::CActionQueue* m_apiCalls;
    ActionQueue::CActionQueue* m_apiCallCompletions;
    ActionQueue::CActionQueue* m_apiHiPriorityCallCompletions;

};



#endif /* HttpEngine_hpp */
