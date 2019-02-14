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

#ifndef plist_buffer_h
#define plist_buffer_h


#include <string>
#include <vector>
#include <list>
#include "p8-platform/threads/threads.h"
#include "p8-platform/util/buffer.h"
#include "input_buffer.h"
#include "plist_buffer_delegate.h"

namespace Buffers
{
    class MutableSegment;
    class PlaylistCache;
    
    class PlaylistBuffer :  public InputBuffer, public P8PLATFORM::CThread
    {
    public:
        PlaylistBuffer(const std::string &streamUrl,  PlaylistBufferDelegate delegate, int segmentsCacheSize);
        ~PlaylistBuffer();
        
        int64_t GetLength() const;
        int64_t GetPosition() const;
        int64_t Seek(int64_t iPosition, int iWhence);
        ssize_t Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs);
        bool SwitchStream(const std::string &newUrl);
        
        /*!
         * @brief Stop the thread
         * @param iWaitMs negative = don't wait, 0 = infinite, or the amount of ms to wait
         */
        virtual bool StopThread(int iWaitMs = 5000);
        
    private:
        mutable P8PLATFORM::CMutex m_syncAccess;
        P8PLATFORM::CEvent m_writeEvent;
        PlaylistBufferDelegate m_delegate;
        int64_t m_position;
        const int m_segmentsCacheSize;
        PlaylistCache* m_cache;
        
        void *Process();
        void Init(const std::string &playlistUrl);
        void Init(const std::string &playlistUrl, bool cleanContent, int64_t position,  time_t readTimshift, time_t writeTimshift);
        bool FillSegment(MutableSegment* segment);
        bool IsStopped(uint32_t timeoutInSec = 0);
    };
    
    class PlistBufferException : public InputBufferException
    {
    public:
        PlistBufferException(const char* reason = "")
        : m_reason(reason)
        , InputBufferException(NULL)
        {}
        virtual const char* what() const noexcept {return m_reason.c_str();}
        
    private:
        std::string m_reason;
    };
    
}
#endif //plist_buffer_h
