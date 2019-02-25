/*
 *
 *   Copyright (C) 2019 Sergey Shramchenko
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

#ifndef __plist_buffer_delegate_h__
#define __plist_buffer_delegate_h__

#include <memory>

namespace Buffers
{
    class IPlaylistBufferDelegate
    {
    public:
        virtual int SegmentsAmountToCache() const= 0;
        virtual time_t Duration() const= 0;
        virtual std::string UrlForTimeshift(time_t timeshift, time_t* timeshiftAdjusted) const = 0;
        virtual ~IPlaylistBufferDelegate() {}
    };
    typedef  std::shared_ptr<IPlaylistBufferDelegate> PlaylistBufferDelegate;
}

#endif /* __plist_buffer_delegate_h__ */
