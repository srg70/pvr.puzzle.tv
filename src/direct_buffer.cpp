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


#include "libXBMC_addon.h"
#include "direct_buffer.h"

using namespace P8PLATFORM;

DirectBuffer::DirectBuffer(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &streamUrl) :
    m_addonHelper(addonHelper)
{
    m_streamHandle = m_addonHelper->OpenFile(streamUrl.c_str(), 0);
    if (!m_streamHandle)
        throw InputBufferException();
}

DirectBuffer::~DirectBuffer()
{
    m_addonHelper->CloseFile(m_streamHandle);
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

int64_t DirectBuffer::Seek(int64_t iPosition, int iWhence) const
{
    return -1;
}

bool DirectBuffer::SwitchStream(const std::string &newUrl)
{
    CLockObject lock(m_mutex);

    m_addonHelper->CloseFile(m_streamHandle);
    m_streamHandle = m_addonHelper->OpenFile(newUrl.c_str(), 0);

    return m_streamHandle != NULL;
}
