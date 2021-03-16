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
#include "globals.hpp"

namespace Buffers {
    
    using namespace std;
    using namespace P8PLATFORM;
    using namespace Globals;
    
    TimeshiftBuffer::TimeshiftBuffer(InputBuffer* inputBuffer, ICacheBuffer* cache)
    : m_inputBuffer(inputBuffer)
    , m_cache(cache)
    , m_cacheToSwap(nullptr)
    , m_isWaitingForRead(false)
//    , m_downloadSpeed(33 * 1024 * 1024)
//    , m_playbackSpeed(33 * 1024 * 1024)
    {
        if (!m_inputBuffer)
            throw InputBufferException("TimesiftBuffer: source stream buffer is NULL.");
        if (!m_cache)
            throw InputBufferException("TimesiftBuffer: cache buffer is NULL.");
        Init();
    }
    
//  static bool test_read_started = false;
    void TimeshiftBuffer::Init(const std::string& newUrl) {
        
        if(!newUrl.empty()) {
            AbortRead();
            m_inputBuffer->SwitchStream(newUrl);            
        }
        
        m_writerWaitingForCacheSwap  = false;
        m_writeEvent.Reset();
        m_cache->Init();
        m_isInputBufferValid = false;
//        test_read_started = false;
//        m_downloadSpeed.Start();
        CreateThread();

    }
    
    TimeshiftBuffer::~TimeshiftBuffer()
    {
        AbortRead();
        
        if(m_inputBuffer)
            delete m_inputBuffer;
        if(m_cache)
             delete m_cache;
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
               
                while (!isError && (bytesRead < bufferLenght) && !IsStopped() && m_inputBuffer != NULL){
                    // Use some "common" timeout (30 sec) since it is background process
                    ssize_t loacalBytesRad = m_inputBuffer->Read(buffer + bytesRead, bufferLenght - bytesRead, 30*1000);
                    bytesRead += loacalBytesRad;
                    isError = loacalBytesRad < 0;
                }

                if(nullptr != buffer) {
                    m_cache->UnlockAfterWriten(buffer, bytesRead);
                    m_isInputBufferValid = true;
                    m_writeEvent.Signal();
                }
//                m_downloadSpeed.StepDone(bytesRead);
            }
        } catch (std::exception& ex ) {
            LogError("Exception in timshift background thread: %s", ex.what());
        }

        return NULL;
    }

//    float TimeshiftBuffer::GetSpeedRatio() const {
//        float d = m_downloadSpeed.KBytesPerSecond();
//        float r = m_playbackSpeed.KBytesPerSecond();
//        float diff = d-r;
//
//        return 0.5 + 0.5 * diff / (diff < 0 ? r : d);
//    }

    ssize_t TimeshiftBuffer::Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs)
    {
//        if(!test_read_started) {
//            test_read_started = true;
//            m_playbackSpeed.Start();
//        }
        size_t totalBytesRead = 0;

        m_isWaitingForRead = true;
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
        m_isWaitingForRead = false;
//        m_playbackSpeed.StepDone(totalBytesRead);
//        LogDebug("TimeshiftBuffer: download speed: %.2f - %.2f = %.2f KB/sec",
//                 m_downloadSpeed.KBytesPerSecond(),
//                 m_playbackSpeed.KBytesPerSecond(),
//                 m_downloadSpeed.KBytesPerSecond() - m_playbackSpeed.KBytesPerSecond());
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
    
    void TimeshiftBuffer::AbortRead()
    {
        StopThread(1);
        if(m_inputBuffer) {
            m_inputBuffer->AbortRead();
        }
        while(m_isWaitingForRead) {
             LogDebug("TimeshiftBuffer: waiting for readidng abort 100 ms...");
             P8PLATFORM::CEvent::Sleep(100);
             m_writeEvent.Signal();
         }
        while(IsRunning()) {
            LogNotice("TimeshiftBuffer: waiting 100 ms for thread stopping...");
            P8PLATFORM::CEvent::Sleep(100);
        }
    }
}

