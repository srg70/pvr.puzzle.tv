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
#endif

#include "direct_buffer.h"
#include "libXBMC_addon.h"

using namespace P8PLATFORM;

DirectBuffer::DirectBuffer(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &streamUrl) :
    m_addonHelper(addonHelper)
{
    Open(streamUrl.c_str());
    if (!m_streamHandle)
        throw InputBufferException();
}

DirectBuffer::~DirectBuffer()
{
    m_addonHelper->CloseFile(m_streamHandle);
}

void DirectBuffer::Open(const char* path)
{
    m_streamHandle = m_addonHelper->OpenFile(path, XFILE::READ_AUDIO_VIDEO | XFILE::READ_AFTER_WRITE);
}


int64_t DirectBuffer::GetLength() const
{
    return -1;
}

int64_t DirectBuffer::GetPosition() const
{
    return -1;
}

ssize_t DirectBuffer::Read(unsigned char *buffer, size_t bufferSize)
{
    CLockObject lock(m_mutex);

    return m_addonHelper->ReadFile(m_streamHandle, buffer, bufferSize);
}

int64_t DirectBuffer::Seek(int64_t iPosition, int iWhence)
{
    return -1;
}

bool DirectBuffer::SwitchStream(const std::string &newUrl)
{
    CLockObject lock(m_mutex);

    m_addonHelper->CloseFile(m_streamHandle);
    Open(newUrl.c_str());

    return m_streamHandle != NULL;
}


ArchiveBuffer::ArchiveBuffer(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &streamUrl)
    :DirectBuffer(addonHelper, streamUrl)
{}
ArchiveBuffer::~ArchiveBuffer()
{}

int64_t ArchiveBuffer::GetLength() const
{
    CLockObject lock(m_mutex);
    auto retVal =  m_addonHelper->GetFileLength(m_streamHandle);
    m_addonHelper->Log(ADDON::LOG_DEBUG, "ArchiveBuffer: length = %d", retVal);
    return retVal;
}
int64_t ArchiveBuffer::GetPosition() const
{
    CLockObject lock(m_mutex);
    auto retVal =  m_addonHelper->GetFilePosition(m_streamHandle);
    m_addonHelper->Log(ADDON::LOG_DEBUG, "ArchiveBuffer: position = %d", retVal);
    return retVal;
}
int64_t ArchiveBuffer::Seek(int64_t iPosition, int iWhence)
{
    CLockObject lock(m_mutex);
    auto retVal =  m_addonHelper->SeekFile(m_streamHandle, iPosition, iWhence);
    m_addonHelper->Log(ADDON::LOG_DEBUG, "ArchiveBuffer: Seek = %d(requested %d from %d)", retVal, iPosition, iWhence);
    return retVal;
    
}
