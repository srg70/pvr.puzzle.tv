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
    , m_cacheToSwap(nullptr)
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
        
        m_writerWaitingForCacheSwap  = false;
        m_writeEvent.Reset();
        m_cache->Init();
        CreateThread();

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
            LogNotice("TimeshiftBuffer: can't stop thread in %d ms", iWaitMs);
        }
        if(!retVal)
            LogError("TimeshiftBuffer: can't stop thread in %d ms", stopCounter*iWaitMs);
        
        return retVal;
    }
    
    void TimeshiftBuffer::CheckAndWaitForSwap() {
        if(nullptr == m_cacheToSwap)
            return;
        LogDebug("TimeshiftBuffer::CheckAndWaitForSwap(): waiting for cache swap...");
        m_writerWaitingForCacheSwap = true;
        m_cacheSwapEvent.Wait();
        m_writerWaitingForCacheSwap = false;
        LogDebug("TimeshiftBuffer::CheckAndWaitForSwap(): cache swap is done.");
    }
    
    void TimeshiftBuffer::CheckAndSwap() {
        // Can swap cache when we have a cache for swap and writer is waiting for us.
        if(nullptr != m_cacheToSwap &&  m_writerWaitingForCacheSwap){
            LogDebug("TimeshiftBuffer::CheckAndSwap(): starting cache swap.");
            
            m_cacheToSwap->Init();
            const size_t bufferLenght = m_cacheToSwap->UnitSize();
            uint8_t* buffer = nullptr;
            ssize_t bytesRead = 0;
            do {
                if(!m_cacheToSwap->LockUnitForWrite(&buffer)){
                    LogInfo("TimeshiftBuffer::CheckAndSwap(): new cache is too small.");
                    break;
                }
                bytesRead = m_cache->Read(buffer, bufferLenght);
                LogDebug("TimeshiftBuffer::CheckAndSwap(): swapping %d bytes.", bytesRead);
                if(bytesRead >=0 && nullptr != buffer) {
                    m_cacheToSwap->UnlockAfterWriten(buffer, bytesRead);
                }
            }while(bytesRead > 0);
            
            delete m_cache;
            m_cache = m_cacheToSwap;
            m_cacheToSwap = nullptr;
            m_writeEvent.Reset();
            m_cacheSwapEvent.Broadcast();
            LogDebug("TimeshiftBuffer::CheckAndSwap(): cache swap done.");
        }
    }

    
    void *TimeshiftBuffer::Process()
    {
        bool isError = false;
        try {
            while (!isError && m_inputBuffer != NULL && !IsStopped()) {
                
                CheckAndWaitForSwap() ;
                // Fill read buffer
                const size_t bufferLenght = m_cache->UnitSize();
                uint8_t* buffer = nullptr;
                while(!IsStopped() && !m_cache->LockUnitForWrite(&buffer)) {
                    LogError("TimeshiftBuffer: no free cache unit available. Cache is full? ");
                    Sleep(1000);
                }
                ssize_t bytesRead = 0;
                while (!isError && bytesRead < bufferLenght && !IsStopped()){
                    ssize_t loacalBytesRad = m_inputBuffer->Read(buffer + bytesRead, bufferLenght - bytesRead, 1000);
                    bytesRead += loacalBytesRad;
                    isError = loacalBytesRad < 0;
                }
                if(nullptr != buffer) {
                    m_cache->UnlockAfterWriten(buffer, bytesRead);
                    m_writeEvent.Signal();
                }
//                if(bytesRead > 0) {
//                    // Write to local chunk
//                    ssize_t bytesWritten = m_cache->Write(buffer, bytesRead);
//                    // Allow write errors. Cache may be full.
//                    //isError |= bytesWritten != bytesRead;
//                    if(bytesWritten != bytesRead) {
//                        LogError("TimeshiftBuffer: write cache error written (%d) != read (%d)", bytesWritten,bytesRead);
//                    }
//                    m_writeEvent.Signal();
//                }
            }
            
        } catch (std::exception& ex ) {
            LogError("Exception in timshift background thread: %s", ex.what());
        }

        return NULL;
    }
    
    ssize_t TimeshiftBuffer::Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs)
    {
        size_t totalBytesRead = 0;

        CheckAndSwap();
        
        while (totalBytesRead < bufferSize && IsRunning()) {
            ssize_t bytesRead = 0;
            size_t bytesToRead = bufferSize - totalBytesRead;
            bytesRead = m_cache->Read( buffer + totalBytesRead, bytesToRead);
            bool isTimeout = false;
            while(!isTimeout && bytesRead == 0 && (m_cache->Length() - m_cache->Position()) < (bufferSize - totalBytesRead)) {
                if(!(isTimeout = !m_writeEvent.Wait(timeoutMs)))
                   bytesRead = m_cache->Read( buffer + totalBytesRead, bytesToRead);
            }
            totalBytesRead += bytesRead;
            if(isTimeout){
                LogNotice("TimeshiftBuffer: nothing to read within %d msec.", timeoutMs);
                totalBytesRead = -1;
                break;
            }
        }
        return (IsStopped() || !IsRunning()) ? -1 :totalBytesRead;
    }
    
    int64_t TimeshiftBuffer::GetLength() const
    {
        return m_cache->Length();
    }
    
    int64_t TimeshiftBuffer::GetPosition() const
    {
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
            LogError("Failed to switch streams. Error: %s", ex.what());
        }
        
        return succeeded;
    }
    
    
}

