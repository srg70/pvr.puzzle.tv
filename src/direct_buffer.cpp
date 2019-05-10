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
#ifdef GetObject
#undef GetObject
#endif
#endif

#include "direct_buffer.h"
#include "libXBMC_addon.h"
#include "globals.hpp"
#include "cache_buffer.h"

namespace Buffers {
    
    using namespace Globals;

    void* DirectBuffer::Open(const std::string & path){
        return XBMC->OpenFile(path.c_str(), XFILE::READ_AUDIO_VIDEO | XFILE::READ_AFTER_WRITE);
    }

    
    DirectBuffer::DirectBuffer(const std::string &streamUrl)
    : m_streamHandle(Open(streamUrl))
    , m_cacheBuffer(nullptr)
    , m_url(streamUrl)
    {
        if (!m_streamHandle)
            throw InputBufferException();
    }
    DirectBuffer::DirectBuffer(ICacheBuffer* cacheBuffer)
    : m_streamHandle(nullptr)
    , m_cacheBuffer(cacheBuffer)
    {
        if (!m_cacheBuffer)
            throw InputBufferException();
    }

    
    DirectBuffer::~DirectBuffer()
    {
        if(m_streamHandle)
            XBMC->CloseFile(m_streamHandle);
        if(m_cacheBuffer)
            delete m_cacheBuffer;
    }
    
    int64_t DirectBuffer::GetLength() const
    {
        return m_cacheBuffer ? m_cacheBuffer->Length() : -1;
    }
    
    int64_t DirectBuffer::GetPosition() const
    {
        return m_cacheBuffer ? m_cacheBuffer->Position() :-1;
    }
    
    ssize_t DirectBuffer::Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs)
    {
        if(m_cacheBuffer)
            return m_cacheBuffer->Read(buffer, bufferSize);

        return XBMC->ReadFile(m_streamHandle, buffer, bufferSize);
    }
    
    int64_t DirectBuffer::Seek(int64_t iPosition, int iWhence)
    {
        return m_cacheBuffer ? m_cacheBuffer->Seek(iPosition, iWhence) : -1;
    }
    
    bool DirectBuffer::SwitchStream(const std::string &newUrl)
    {
        if(m_cacheBuffer)
            return false;
        
        XBMC->CloseFile(m_streamHandle);
        m_streamHandle = Open(newUrl);
        return m_streamHandle != NULL;
    }
    
    
    ArchiveBuffer::ArchiveBuffer(const std::string &streamUrl)
    :DirectBuffer(streamUrl)
    {}
    ArchiveBuffer::~ArchiveBuffer()
    {}
    
    int64_t ArchiveBuffer::GetLength() const
    {
        auto retVal =  XBMC->GetFileLength(m_streamHandle);
        //XBMC->Log(ADDON::LOG_DEBUG, "ArchiveBuffer: length = %d", retVal);
        return retVal;
    }
    int64_t ArchiveBuffer::GetPosition() const
    {
        auto retVal =  XBMC->GetFilePosition(m_streamHandle);
        //XBMC->Log(ADDON::LOG_DEBUG, "ArchiveBuffer: position = %d", retVal);
        return retVal;
    }
    int64_t ArchiveBuffer::Seek(int64_t iPosition, int iWhence)
    {
        auto retVal =  XBMC->SeekFile(m_streamHandle, iPosition, iWhence);
        //XBMC->Log(ADDON::LOG_DEBUG, "ArchiveBuffer: Seek = %d(requested %d from %d)", retVal, iPosition, iWhence);
        return retVal;
        
    }
}
