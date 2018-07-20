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

#include "p8-platform/os.h"
#if (defined(_WIN32) || defined(__WIN32__))
#include <windows.h>
#ifdef GetObject
#undef GetObject
#endif
#endif

#include "timeshift_buffer.h"
#include "helpers.h"
#include <sstream>
#include <functional>
#include "libXBMC_addon.h"
#include "globals.hpp"

namespace Buffers {
    
    using namespace std;
    using namespace ADDON;
    using namespace P8PLATFORM;
    using namespace Globals;
    
    TimeshiftBuffer::TimeshiftBuffer(InputBuffer* inputBuffer, ICacheBuffer* cache)
    : m_inputBuffer(inputBuffer)
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
        //    char* msg = XBMC->UnknownToUTF8(message.c_str());
        XBMC->Log(LOG_DEBUG, message.c_str());
        //    XBMC->FreeString(msg);
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
            XBMC->Log(LOG_NOTICE, "TimeshiftBuffer: can't stop thread in %d ms", iWaitMs);
        }
        if(!retVal)
            XBMC->Log(LOG_ERROR, "TimeshiftBuffer: can't stop thread in %d ms", stopCounter*iWaitMs);
        
        return retVal;
    }
    void *TimeshiftBuffer::Process()
    {
        const size_t bufferLenght = m_cache->UnitSize();
        unsigned char* buffer = new unsigned char[bufferLenght];
        bool isError = false;
        try {
            while (!isError && m_inputBuffer != NULL && !IsStopped()) {
                
                // Fill read buffer
                ssize_t bytesRead = 0;
                do {
                    ssize_t loacalBytesRad = m_inputBuffer->Read(buffer + bytesRead, bufferLenght - bytesRead, 1000);
                    bytesRead += loacalBytesRad;
                    isError = loacalBytesRad < 0;
                }while (!isError && bytesRead < bufferLenght && !IsStopped());
                
                if(bytesRead > 0) {
                    // Write to local chunk
                    //DebugLog(std::string(">>> Write: ") + n_to_string(bytesRead));
                    ssize_t bytesWritten = m_cache->Write(buffer, bytesRead);
                    // Allow write errors. Cache may be full.
                    //isError |= bytesWritten != bytesRead;
                    if(bytesWritten != bytesRead) {
                        XBMC->Log(LOG_ERROR, "TimeshiftBuffer: write cache error written (%d) != read (%d)", bytesWritten,bytesRead);
                    }
                    m_writeEvent.Signal();
                }
            }
            
        } catch (std::exception& ex ) {
            XBMC->Log(LOG_ERROR, "Exception in timshift background thread: %s", ex.what());
        }

		if (NULL != buffer)
			delete[] buffer;
        return NULL;
    }
    
    ssize_t TimeshiftBuffer::Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs)
    {
        size_t totalBytesRead = 0;

        while (totalBytesRead < bufferSize && IsRunning()) {
            ssize_t bytesRead = 0;
            size_t bytesToRead = bufferSize - totalBytesRead;
            bytesRead = m_cache->Read( buffer + totalBytesRead, bytesToRead);
            totalBytesRead += bytesRead;
            bool isTimeout = false;
            while(!isTimeout && bytesRead == 0 && (m_cache->Length() - m_cache->Position()) < (bufferSize - totalBytesRead)) {
                isTimeout = !m_writeEvent.Wait(timeoutMs); //timeout
            }
            if(isTimeout){
                XBMC->Log(LOG_NOTICE, "TimeshiftBuffer: nothing to read within %d msec.", timeoutMs);
                totalBytesRead = -1;
                break;
            }
            //DebugLog(std::string(">>> Read: ") + n_to_string(bytesRead));
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
            XBMC->Log(LOG_ERROR, "Failed to switch streams. Error: %s", ex.what());
        }
        
        return succeeded;
    }
    
    
}

