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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define CURL_STATICLIB 1
#ifdef GetObject
#undef GetObject
#endif
#endif

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
    
    typedef std::map<std::string, std::string> TCookies;
    
    HttpEngine ();
    ~HttpEngine();
    
    struct Request {
        Request(const std::string& url) : Url(url){}
        Request(const std::string& url, const std::string& postData) : Url(url), PostData(postData){}
        Request(const std::string& url, const std::string& postData, const std::vector<std::string>& headers) : Url(url), PostData(postData), Headers(headers){}
        bool IsPost() const {return !PostData.empty();}
        const std::string Url;
        const std::string PostData;
        const std::vector<std::string> Headers;
    };
    
    template <typename TParser, typename TCompletion>
    void CallApiAsync(const Request& request, TParser parser, TCompletion completion, RequestPriority priority = RequestPriority_Low)
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
//    static std::string Escape(const std::string& str);
    
    TCookies m_sessionCookie;

    static bool CheckInternetConnection(long timeout);
    
    static void DoCurl(const Request &request, const TCookies &cookie, std::string* response, uint64_t requestId = 0, std::string* effectiveUrl = nullptr);
    
private:
    static size_t CurlWriteData(void *buffer, size_t size, size_t nmemb, void *userp);
    static  long c_CurlTimeout;
    mutable uint64_t m_DebugRequestId;

    template <typename TResultCallback, typename TCompletion>
    void SendHttpRequest(const Request &request, const TCookies &cookie, TResultCallback result, TCompletion completion, RequestPriority priority) const
    {
        std::string* response = new std::string();
        const uint64_t requestId = m_DebugRequestId++;
        
        DoCurl(request, cookie, response, requestId);
        
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
