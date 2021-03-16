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

#include "HttpEngine.hpp"
#include "p8-platform/util/util.h"
#include "p8-platform/threads/mutex.h"
#include "base64.h"
#include "httplib.h"

namespace CurlUtils
{
    void SetCurlTimeout(long timeout) {
        HttpEngine::SetCurlTimeout(timeout);
    }
}

using namespace ActionQueue;

static const size_t c_MaxQueueSize = 100000;
long HttpEngine::c_CurlTimeout = 15; // in sec

HttpEngine::HttpEngine()
    :   m_apiCalls(new CActionQueue(c_MaxQueueSize, "API Calls")),
        m_apiCallCompletions(new CActionQueue(c_MaxQueueSize, "API Complition")),
        m_apiHiPriorityCallCompletions(new CActionQueue(c_MaxQueueSize, "API Hi Priority Comp")),
        m_DebugRequestId(1)
{

    m_apiCalls->CreateThread();
    m_apiCallCompletions->CreateThread();
    m_apiHiPriorityCallCompletions->CreateThread();
}


#ifdef  ADDON_GLOBAL_VERSION_MAIN // I.e. Kodi 18.* and later
    //Use Kodi's CURL library
    bool HttpEngine::CheckInternetConnection(long timeout)
    {
        using namespace Globals;
        kodi::vfs::CFile curl;
        try {
            const std::string url("https://www.google.com");
                    
            if (!curl.CURLCreate(url)) {
                Globals::LogError("CheckInternetConnection() failed. Can't instansiate CURL.");
                return false;
            }
            Globals::LogInfo("Sending request: %s", url.c_str());
            if(!curl.CURLOpen(ADDON_READ_NO_CACHE | ADDON_READ_TRUNCATED | ADDON_READ_CHUNKED)){
                Globals::LogError("Failed to open URL = %s.", url.c_str());
                curl.Close();
                return false;
            }
            
            ssize_t bytesRead = 0;
            do {
                char buffer[256];
                bytesRead = curl.Read(&buffer[0], sizeof(buffer));
            } while(bytesRead > 0);
            curl.Close();

            return bytesRead == 0;

        } catch (...) {
            if (curl.IsOpen())
                curl.Close();
            throw;
        }
    }


    void HttpEngine::DoCurl(const Request &request, const TCookies &cookie, std::string* response, unsigned long long requestId, std::string* effectiveUrl)
    {
        using namespace Globals;
        kodi::vfs::CFile curl;
        
        try {
            
            if (!curl.CURLCreate(httplib::detail::encode_get_url(request.Url)))
                throw CurlErrorException("CURL initialisation failed.");
            
            // Post data
            if(request.IsPost()) {
                curl.CURLAddOption(ADDON_CURL_OPTION_HEADER, "postdata",
                                    base64_encode(reinterpret_cast<const unsigned char*>(request.PostData.c_str()), request.PostData.size()));
            }
            // Custom headers
            if(!request.Headers.empty()) {
                for (const auto& header : request.Headers) {
                    auto pos =  header.find(':');
                    if(std::string::npos == pos)
                        continue;
                    curl.CURLAddOption(ADDON_CURL_OPTION_HEADER, header.substr(0, pos), header.substr(pos + 1));
                }
            }
            // Custom cookies
            std::string cookieStr;
            auto itCookie = cookie.begin();
            for(; itCookie != cookie.end(); ++itCookie)
            {
                if (itCookie != cookie.begin())
                    cookieStr += "; ";
                cookieStr += itCookie->first + "=" + itCookie->second;
            }
            curl.CURLAddOption(ADDON_CURL_OPTION_HEADER, "cookie", cookieStr);
            
            auto start = P8PLATFORM::GetTimeMs();
            Globals::LogInfo("Sending request: %s. ID=%llu", request.Url.c_str(), requestId);
            
            if(!curl.CURLOpen(ADDON_READ_NO_CACHE | ADDON_READ_TRUNCATED | ADDON_READ_CHUNKED)){
                            throw CurlErrorException(std::string("Failed to open URL = " + request.Url).c_str());
            }

            ssize_t bytesRead = 0;
            do {
                char buffer[32*1024]; //32K buffer
                bytesRead = curl.Read(&buffer[0], sizeof(buffer));
                response->append(&buffer[0], bytesRead);
            } while(bytesRead > 0);

            if (bytesRead < 0)
                *response = "";
            
            else if(nullptr != effectiveUrl){
                auto url = curl.GetPropertyValue(ADDON_FILE_PROPERTY_EFFECTIVE_URL, "");
                if(!url.empty()) {
                    *effectiveUrl = url;
                }
            }
            if(bytesRead < 0){
                throw CurlErrorException(std::string("CURL (by Kodi) failed to read from " + request.Url + ". See log for details.").c_str());
            }
            
            curl.Close();
        } catch (...) {
            if (curl.IsOpen())
                curl.Close();
            throw;
        }
    }
#else
    // Use external CURL library
    #include <curl/curl.h>
    bool HttpEngine::CheckInternetConnection(long timeout)
    {
         char errorMessage[CURL_ERROR_SIZE];
         std::string* response = new std::string();
         
         CURL *curl = curl_easy_init();
         if (nullptr == curl || nullptr == response) {
             Globals::LogError("CheckInternetConnection() failed. Can't instansiate CURL.");
             return false;
         }
         std::string url("https://www.google.com");
         Globals::LogInfo("Sending request: %s", url.c_str());
         curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
         curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
         curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorMessage);
         
         curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteData);
         curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
         curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
         CURLcode curlCode  = curl_easy_perform(curl);
         delete response;
         if (curlCode != CURLE_OK)
         {
             Globals::LogError("CURL error %d. Message: %s.", curlCode, errorMessage);
             return false;
         }
         
         long httpCode = 0;
         curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
         Globals::LogInfo("Got HTTP response (%d)", httpCode);
         return httpCode == 200;
    }


    void HttpEngine::DoCurl(const Request &request, const TCookies &cookie, std::string* response, unsigned long long requestId, std::string* effectiveUrl)
    {
        char errorMessage[CURL_ERROR_SIZE];
        CURL *curl = curl_easy_init();
        if (nullptr == curl)
            throw CurlErrorException("CURL initialisation failed.");
        
        auto start = P8PLATFORM::GetTimeMs();
        Globals::LogInfo("Sending request: %s. ID=%llu", request.Url.c_str(), requestId);
        curl_easy_setopt(curl, CURLOPT_URL, httplib::detail::encode_get_url(request.Url).c_str());
        if(request.IsPost()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.PostData.c_str());
        }
        if(!request.Headers.empty()) {
            struct curl_slist *headers = NULL;
            for (const auto& header : request.Headers) {
                headers = curl_slist_append(headers, header.c_str());
            }
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_slist_free_all(headers);
        }
        
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorMessage);
        
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, c_CurlTimeout);
        //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
        
        
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
        
        if(curlCode == CURLE_OK && nullptr != effectiveUrl){
            char *url = NULL;
            CURLcode redirectResult = curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
            if(url)
                *effectiveUrl = url;
        }
        
        curl_easy_cleanup(curl);
        if(curlCode != CURLE_OK){
            //delete response;
            throw CurlErrorException(&errorMessage[0]);
        }
    }

#endif
void HttpEngine::CancelAllRequests()
{
    Globals::LogInfo("Cancelling API requests...");

    P8PLATFORM::CEvent event;
    std::exception_ptr ex = nullptr;
    m_apiCalls->CancellAllBefore([=]{},
                             [&](const ActionResult& s) {event.Signal();});
    event.Wait();
    Globals::LogNotice("All API requests canceled.");
}

HttpEngine::~HttpEngine()
{
    if(m_apiCalls) {
        Globals::LogInfo("Destroying API calls queue...");
        m_apiCalls->StopThread();
        SAFE_DELETE(m_apiCalls);
        Globals::LogInfo("API calls queue deleted.");
    }
    if(m_apiCallCompletions) {
        Globals::LogInfo("Destroying API completion queue...");
        m_apiCallCompletions->StopThread();
        SAFE_DELETE(m_apiCallCompletions);
        Globals::LogInfo("API completion queue deleted.");
    }
    if(m_apiHiPriorityCallCompletions) {
        Globals::LogInfo("Destroying API hi-priority completion queue...");
        m_apiHiPriorityCallCompletions->StopThread();
        SAFE_DELETE(m_apiHiPriorityCallCompletions);
        Globals::LogInfo("API hi-priority completion queue deleted.");
    }

}

size_t HttpEngine::CurlWriteData(void *buffer, size_t size, size_t nmemb, void *userp)
{
    std::string *response = (std::string *)userp;
    response->append((char *)buffer, size * nmemb);
    return size * nmemb;
}

void HttpEngine::SetCurlTimeout(long timeout)
{
    HttpEngine::c_CurlTimeout = timeout;
}

//std::string HttpEngine::Escape(const std::string& str)
//{
//    std::string result;
//    CURL *curl = curl_easy_init();
//    if(curl) {
//        char *output = curl_easy_escape(curl, str.c_str(), str.size());
//        if(output) {
//            result = output;
//            curl_free(output);
//        }
//        curl_easy_cleanup(curl);
//    }
//    return result;
//}

