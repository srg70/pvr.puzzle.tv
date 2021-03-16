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
    ADDON_STATUS Init(const std::string& clientPath, const std::string& userPath) override;
    ~SharaTvPVRClient();

    PVR_ERROR GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities) override;
    PVR_ERROR SignalStatus(int /*channelUid*/, kodi::addon::PVRSignalStatus& signalStatus) override;
    bool OpenRecordedStream(const kodi::addon::PVRRecording& recording) override;

protected:
    ADDON_STATUS OnReloadEpg() override;

    ADDON_STATUS CreateCoreSafe(bool clearEpgCache) override;
    void DestroyCoreSafe() override;
    void PopulateSettings(PvrClient::AddonSettingsMutableDictionary& settings) override;

private:
    
    enum DataSourceType{
        c_DataSourceType_Login = 0,
        c_DataSourceType_Playlist = 1
    };
    
    enum PathType{
        c_PathType_Url = 0,
        c_PathType_Local = 1
    };
    
    enum PlistProviderType{
        c_PlistProvider_Other = 0,
        c_PlistProvider_SharaTv = 1,
        c_PlistProvider_Ottg = 2
    };

    enum StreamType{
        c_StreamType_Hls = 0,
        c_StreamType_Ts = 1
    };
    
    bool PreferHls() const;
    PlistProviderType ProviderType() const;
    const std::string& EpgUrl() const;
    DataSourceType DataSource() const;
    bool EnableAdult() const;
    PathType PlaylistPathType() const;
    const std::string& PlayListUrl() const;
    const std::string& PlayListPath() const;
    const std::string& SharaTvLogin() const;
    const std::string& SharaTvPassword() const;
    const std::string& OttgLogin() const;
    const std::string& OttgPassword() const;
    bool SuppotMulticastUrls() const;
    uint32_t UdpProxyPort() const;
    const std::string& UdpProxyHost() const;

    void NotifyAccessDenied() const;
    void CreateCore(bool clearEpgCache);
    SharaTvEngine::Core* m_core;
};

#endif //__sahra_tv_pvr_client_h__
