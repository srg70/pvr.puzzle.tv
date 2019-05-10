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

#ifndef timeshift_buffer_h
#define timeshift_buffer_h


#include <string>
#include "p8-platform/threads/threads.h"
#include "p8-platform/util/buffer.h"
#include "input_buffer.h"
#include "cache_buffer.h"

namespace Buffers {
    
    class TimeshiftBuffer : public InputBuffer, public P8PLATFORM::CThread
    {
    public:
        TimeshiftBuffer(InputBuffer* inputBuffer, ICacheBuffer* cache);
        ~TimeshiftBuffer();
        
        const std::string& GetUrl() const { return m_inputBuffer->GetUrl(); };
        int64_t GetLength() const;
        int64_t GetPosition() const;
        ssize_t Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs);
        int64_t Seek(int64_t iPosition, int iWhence);
        bool SwitchStream(const std::string &newUrl);
        
        void SwapCache(ICacheBuffer* cache){
            m_cacheToSwap = cache;
//            m_cacheSwapEvent.Wait();
        }
        
        /*!
         * @brief Stop the thread
         * @param iWaitMs negative = don't wait, 0 = infinite, or the amount of ms to wait
         */
        virtual bool StopThread(int iWaitMs = 5000);
        
        inline time_t StartTime() { return m_cache->StartTime(); }
        inline time_t EndTime() { return m_cache->EndTime(); }

    private:
        void *Process();
        
        void Init(const std::string &newUrl = std::string());
        void CheckAndWaitForSwap();
        void CheckAndSwap();
        
        P8PLATFORM::CEvent m_writeEvent;
        P8PLATFORM::CEvent m_cacheSwapEvent;
        bool m_writerWaitingForCacheSwap;
        InputBuffer* m_inputBuffer;
        ICacheBuffer* m_cache;
        ICacheBuffer* m_cacheToSwap;
        
    };
}

#endif //timeshift_buffer_h
