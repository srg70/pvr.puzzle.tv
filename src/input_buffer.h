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

#ifndef input_buffer_h
#define input_buffer_h

#include "p8-platform/os.h"
#include <stdint.h>
#include <exception>

namespace Buffers {
    
    class InputBufferException : public std::exception
    {
    public:
        InputBufferException(const char* reason = "") : r(reason){}
        virtual const char* what() const noexcept {return r;}
        
    private:
        const char* r;
    };
    
    class InputBuffer
    {
    public:
        virtual ~InputBuffer() {}
        
        virtual int64_t GetLength() const = 0;
        virtual int64_t GetPosition() const = 0;
        virtual ssize_t Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs) = 0;
        virtual int64_t Seek(int64_t iPosition, int iWhence) = 0;
        virtual bool SwitchStream(const std::string &newUrl) = 0;
    protected:
        const int c_commonTimeoutMs = 10000; // 10 sec
    };
    
}

#endif //input_buffer_h
