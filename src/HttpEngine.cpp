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
        m_DebugRequestId(1)
{

    m_apiCalls->CreateThread();
    m_apiCallCompletions->CreateThread();
    
}


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

std::string HttpEngine::Escape(const std::string& str)
{
    std::string result;
    CURL *curl = curl_easy_init();
    if(curl) {
        char *output = curl_easy_escape(curl, str.c_str(), str.size());
        if(output) {
            result = output;
            curl_free(output);
        }
        curl_easy_cleanup(curl);
    }
    return result;
}

