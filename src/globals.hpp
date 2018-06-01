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

#ifndef __globals_hpp__
#define __globals_hpp__

#include "kodi/libXBMC_pvr.h"
#include "kodi/libXBMC_addon.h"
#include "addon.h"

namespace Globals {

    extern CHelper_libXBMC_pvr* const& PVR;
    extern ADDON::CHelper_libXBMC_addon* const& XBMC;
    
    extern ITimersEngine* const TIMERS;
    
    void LogError(const char *format, ... );
    void LogInfo(const char *format, ... );
    void LogNotice(const char *format, ... );
    void LogDebug(const char *format, ... );

}
#endif /* __globals_hpp__ */
