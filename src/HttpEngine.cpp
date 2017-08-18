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

static const size_t c_MaxQueueSize = 100000;

HttpEngine::HttpEngine(ADDON::CHelper_libXBMC_addon * addonHelper)
    :  m_addonHelper(addonHelper),
    m_apiCalls(new CActionQueue(c_MaxQueueSize)),
    m_apiCallCompletions(new CActionQueue(c_MaxQueueSize))
{

    m_apiCalls->CreateThread();
    m_apiCallCompletions->CreateThread();
    
}

HttpEngine::~HttpEngine()
{
    if(m_apiCalls) {
        m_apiCalls->StopThread();
        SAFE_DELETE(m_apiCalls);
    }
    if(m_apiCallCompletions) {
        m_apiCallCompletions->StopThread();
        SAFE_DELETE(m_apiCallCompletions);
    }

}

size_t HttpEngine::CurlWriteData(void *buffer, size_t size, size_t nmemb, void *userp)
{
    std::string *response = (std::string *)userp;
    response->append((char *)buffer, size * nmemb);
    return size * nmemb;
}

