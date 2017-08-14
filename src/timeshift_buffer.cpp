/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
 *
 *  Copyright (C) 2013 Alex Deryskyba (alex@codesnake.com)
 *  https://bitbucket.org/codesnake/pvr.sovok.tv_xbmc_addon
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
#include "p8-platform/windows/os-types.h"
#else
#include "p8-platform/posix/os-types.h"
#endif

#include "timeshift_buffer.h"
#include "helpers.h"
#include <sstream>
#include <functional>

#include "libXBMC_addon.h"

namespace Buffers {
    
    using namespace std;
    using namespace ADDON;
    using namespace P8PLATFORM;
    
    TimeshiftBuffer::TimeshiftBuffer(CHelper_libXBMC_addon *addonHelper, InputBuffer* inputBuffer, ICacheBuffer* cache)
    : m_addonHelper(addonHelper)
    , m_inputBuffer(inputBuffer)
    , m_cache(cache)
    {
        if (!m_inputBuffer)
        throw InputBufferException("TimesiftBuffer: source stream buffer is NULL.");
        if (!m_cache)
        throw InputBufferException("TimesiftBuffer: cache buffer is NULL.");
        Init();
    }
    
    void TimeshiftBuffer::Init(const std::string& newUrl) {
        StopThread();
        
        if(!newUrl.empty())
        m_inputBuffer->SwitchStream(newUrl);
        
        m_writeEvent.Reset();
        m_cache->Init();
        CreateThread();
    }
    void TimeshiftBuffer::DebugLog(const std::string& message ) const
    {
        //    char* msg = m_addonHelper->UnknownToUTF8(message.c_str());
        m_addonHelper->Log(LOG_DEBUG, message.c_str());
        //    m_addonHelper->FreeString(msg);
    }
    
    TimeshiftBuffer::~TimeshiftBuffer()
    {
        StopThread();
        
        if(m_inputBuffer)
            delete m_inputBuffer;
        if(m_cache)
        delete m_cache;
    }
    
    bool TimeshiftBuffer::StopThread(int iWaitMs)
    {
        int stopCounter = 1;
        bool retVal = false;
        while(!(retVal = this->CThread::StopThread(iWaitMs))){
            if(stopCounter++ > 3)
            break;
            m_addonHelper->Log(LOG_NOTICE, "TimeshiftBuffer: can't stop thread in %d ms", iWaitMs);
        }
        if(!retVal)
        m_addonHelper->Log(LOG_ERROR, "TimeshiftBuffer: can't stop thread in %d ms", stopCounter*iWaitMs);
        
        return retVal;
    }
    void *TimeshiftBuffer::Process()
    {
        unsigned char* buffer = new unsigned char[m_cache->UnitSize()];
        bool isError = false;
        try {
            while (!isError && m_inputBuffer != NULL && !IsStopped()) {
                
                // Fill read buffer
                ssize_t bytesRead = 0;
                do {
                    bytesRead += m_inputBuffer->Read(buffer + bytesRead, sizeof(buffer) - bytesRead);
                    isError = bytesRead < 0;
                }while (bytesRead > 0 && bytesRead < sizeof(buffer) && !IsStopped());
                
                if(bytesRead > 0) {
                    // Write to local chunk
                    //DebugLog(std::string(">>> Write: ") + n_to_string(bytesRead));
                    ssize_t bytesWritten = m_cache->Write(buffer, bytesRead);
                    // Allow write errors. Cache may be full.
                    //isError |= bytesWritten != bytesRead;
                    if(bytesWritten != bytesRead) {
                        m_addonHelper->Log(LOG_ERROR, "TimeshiftBuffer: write cache error written (%d) != read (%d)", bytesWritten,bytesRead);
                    }
                    m_writeEvent.Signal();
                }
            }
            
        } catch (std::exception& ex ) {
            m_addonHelper->Log(LOG_ERROR, "Exception in timshift background thread: %s", ex.what());
        }

		if (NULL != buffer)
			delete[] buffer;
        return NULL;
    }
    
    ssize_t TimeshiftBuffer::Read(unsigned char *buffer, size_t bufferSize)
    {
        
        size_t totalBytesRead = 0;
        int32_t timeout = 5000;//c_commonTimeoutMs + 1000;
        
        while (totalBytesRead < bufferSize && IsRunning()) {
            ssize_t bytesRead = 0;
            size_t bytesToRead = bufferSize - totalBytesRead;
            bytesRead = m_cache->Read( buffer + totalBytesRead, bytesToRead);
            if(bytesRead == 0 && !m_writeEvent.Wait(timeout)){ //timeout
                m_addonHelper->Log(LOG_NOTICE, "TimeshiftBuffer: nothing to read within %d msec.", timeout);
                //StopThread();
                //break;
            }
            //DebugLog(std::string(">>> Read: ") + n_to_string(bytesRead));
            totalBytesRead += bytesRead;
        }
        return (IsStopped() || !IsRunning()) ? -1 :totalBytesRead;
    }
    
    int64_t TimeshiftBuffer::GetLength() const
    {
        //DebugLog(std::string(">>> GetLength(): ") + n_to_string(length));
        return m_cache->Length();
    }
    
    int64_t TimeshiftBuffer::GetPosition() const
    {
        //DebugLog(std::string(">>> GetPosition(): ") + n_to_string(pos));
        return m_cache->Position();
    }
    
    
    int64_t TimeshiftBuffer::Seek(int64_t iPosition, int iWhence)
    {
           return m_cache->Seek(iPosition,iWhence);
    }
    
    bool TimeshiftBuffer::SwitchStream(const string &newUrl)
    {
        bool succeeded = false;
        try {
            Init(newUrl);
            succeeded = true;
        } catch (const InputBufferException& ex) {
            m_addonHelper->Log(LOG_ERROR, "Failed to switch streams. Error: %s", ex.what());
        }
        
        return succeeded;
    }
    
    
}

