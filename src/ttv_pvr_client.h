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

#ifndef  __ttv_pvr_client_h__
#define __ttv_pvr_client_h__

#include <string>
#include "addon.h"
#include "pvr_client_base.h"

class InputBuffer;
namespace TtvEngine {
    class Core;
}

class TtvPVRClient: public PvrClient::PVRClientBase
{
public:
    ADDON_STATUS Init(const std::string& clientPath, const std::string& userPath) override;
    ~TtvPVRClient();

    ADDON_STATUS SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue) override;
    PVR_ERROR GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities) override;
    PVR_ERROR SignalStatus(int /*channelUid*/, kodi::addon::PVRSignalStatus& signalStatus) override;

protected:
    std::string GetStreamUrl(PvrClient::ChannelId channelId) override;
    std::string GetNextStreamUrl(PvrClient::ChannelId channelId) override;
    ADDON_STATUS OnReloadEpg() override;
    
    bool OpenRecordedStream(const kodi::addon::PVRRecording& recording) override;
    
    ADDON_STATUS CreateCoreSafe(bool clearEpgCache) override;
    void DestroyCoreSafe() override;
    void PopulateSettings(PvrClient::AddonSettingsMutableDictionary& settings) override;

private:
    void CreateCore(bool clearEpgCache);

    TtvEngine::Core* m_core;
    std::string m_epgUrl;
    int m_currentChannelStreamIdx;

    bool m_enableAdultContent;
    std::string m_aceServerUri;
    int m_aceServerPort;
    bool m_filterByAlexElec;
};

#endif //__ttv_pvr_client_h__
