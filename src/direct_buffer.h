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

namespace ADDON
{
    class CHelper_libXBMC_addon;
}

class DirectBuffer : public InputBuffer
{
public:
    DirectBuffer(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &streamUrl);
    ~DirectBuffer();

    int64_t GetLength() const;
    int64_t GetPosition() const;
    int64_t Seek(int64_t iPosition, int iWhence) const;
    ssize_t Read(unsigned char *buffer, size_t bufferSize);
    bool SwitchStream(const std::string &newUrl);

private:
    ADDON::CHelper_libXBMC_addon *m_addonHelper;
    void *m_streamHandle;
    P8PLATFORM::CMutex m_mutex;
};

#endif //direct_buffer_h
