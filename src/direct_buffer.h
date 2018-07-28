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

#ifndef direct_buffer_h
#define direct_buffer_h

#include <string>
#include "p8-platform/threads/mutex.h"
#include "input_buffer.h"

namespace Buffers {
    
    class ICacheBuffer;
    class DirectBuffer : public InputBuffer
    {
    public:
        DirectBuffer(const std::string &streamUrl);
        DirectBuffer(ICacheBuffer* cacheBuffer);
        ~DirectBuffer();
        
        int64_t GetLength() const;
        int64_t GetPosition() const;
        int64_t Seek(int64_t iPosition, int iWhence);
        ssize_t Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs);
        bool SwitchStream(const std::string &newUrl);
        
    protected:
        void *m_streamHandle;
        mutable P8PLATFORM::CMutex m_mutex;
    private:
        static void* Open(const std::string &path);
        ICacheBuffer* m_cacheBuffer;
    };
    
    class ArchiveBuffer : public DirectBuffer
    {
    public:
        ArchiveBuffer(const std::string &streamUrl);
        ~ArchiveBuffer();
        
        int64_t GetLength() const;
        int64_t GetPosition() const;
        int64_t Seek(int64_t iPosition, int iWhence);
    };
}
#endif //direct_buffer_h
