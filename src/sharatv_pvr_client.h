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

#ifndef  __sahra_tv_pvr_client_h__
#define __sahra_tv_pvr_client_h__

#include "xbmc_pvr_types.h"
#include <string>
#include "addon.h"
#include "pvr_client_base.h"

class InputBuffer;
namespace SharaTvEngine {
    class Core;
}

class SharaTvPVRClient: public PvrClient::PVRClientBase
{
public:
    ADDON_STATUS Init(PVR_PROPERTIES* pvrprops);
    ~SharaTvPVRClient();

    ADDON_STATUS SetSetting(const char *settingName, const void *settingValue);
    PVR_ERROR GetAddonCapabilities(PVR_ADDON_CAPABILITIES* pCapabilities);
    PVR_ERROR SignalStatus(PVR_SIGNAL_STATUS &signalStatus);
    bool OpenRecordedStream(const PVR_RECORDING &recording);

protected:
    PVR_ERROR  MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item);
    ADDON_STATUS OnReloadEpg();

    ADDON_STATUS CreateCoreSafe(bool clearEpgCache);
    void DestroyCoreSafe();

private:
    void CreateCore(bool clearEpgCache);

    SharaTvEngine::Core* m_core;
    std::string m_login;
    std::string m_password;
    //std::string m_epgUrl;
    bool m_enableAdult;
};

#endif //__sahra_tv_pvr_client_h__
