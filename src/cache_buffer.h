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

#ifndef IChunckedBuffer_h
#define IChunckedBuffer_h


#include "p8-platform/os.h"
#include <stdint.h>

namespace Buffers
{
    
    // Interface for synchronized data stream
    // New data always apeended to the end of stream
    // Real data capacity may be limited.
    // In case of limited capacity stream will provide partial amount of data inside chache window (latest, i.e. from the tail),
    // although the total length will always correspond to amount of written data.
    
    class ICacheBuffer {
    public:
        ICacheBuffer() {};

        // Initialize empty buffer (aka constructor)
        virtual  void Init() = 0;
        virtual  uint32_t UnitSize() = 0;
        
        // Read interface
        // Seak read position within cache window
        virtual int64_t Seek(int64_t iFilePosition, int iWhence) = 0;
        // Virtual steream lenght.
        virtual int64_t Length() = 0;
        // Current read position
        virtual int64_t Position() = 0;
        // Reads data from Position(),
        virtual ssize_t Read(void* lpBuf, size_t uiBufSize) = 0;
        
        // Write interface
        virtual ssize_t Write(const void* lpBuf, size_t uiBufSize) = 0;
        
        virtual ~ICacheBuffer() {};
        
    private:
        ICacheBuffer(const ICacheBuffer&) = delete ;                    //disable copy-constructor
        ICacheBuffer& operator=(const ICacheBuffer&) = delete;  //disable copy-assignment
        
        
    };
    
    
    class CacheBufferException : public std::exception
    {
    public:
        CacheBufferException(const char* reason = "") : r(reason){}
        virtual const char* what() const noexcept {return r;}
        
    private:
        const char* r;
    };

}

#endif /* IChunckedBuffer_h */
