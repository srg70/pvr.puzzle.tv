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

#include <algorithm>
#include <ctime>
#include "p8-platform/util/util.h"

#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "sharatv_pvr_client.h"
#include "helpers.h"
#include "sharatv_player.h"
#include "plist_buffer.h"
#include "globals.hpp"
#include "httplib.h"

using namespace Globals;
using namespace std;
using namespace ADDON;
using namespace SharaTvEngine;
using namespace PvrClient;
using namespace Helpers;

ADDON_STATUS SharaTvPVRClient::Init(PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = PVRClientBase::Init(pvrprops);
    if(ADDON_STATUS_OK != retVal)
        return retVal;
     
    SetSeekSupported(true);
    return CreateCoreSafe(false);
}

SharaTvPVRClient::~SharaTvPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    DestroyCoreSafe();
}

void SharaTvPVRClient::NotifyAccessDenied() const
{
    const char* providerName = "Unknown provider";
    const PlistProviderType providerType = ProviderType();
    if(c_PlistProvider_SharaTv == providerType) {
        providerName = "Shara TV";
    }else if(c_PlistProvider_Ottg == providerType){
        providerName = "OTTG";
    }
     
    XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32007), providerName);
}

ADDON_STATUS SharaTvPVRClient::CreateCoreSafe(bool clearEpgCache)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    try
    {
        CreateCore(clearEpgCache);
        OnCoreCreated();
    }
    catch (ServerErrorException &)
    {
        NotifyAccessDenied();
    }
    catch (AuthFailedException &)
    {
        NotifyAccessDenied();
    }
    catch(...)
    {
        XBMC->QueueNotification(QUEUE_ERROR, "Shara TV: unhandeled exception on core creation.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    return retVal;
}

void SharaTvPVRClient::DestroyCoreSafe()
{
    if(m_core != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_core);
    }

}

void SharaTvPVRClient::CreateCore(bool clearEpgCache)
{
    DestroyCoreSafe();
    
    string playlistUrl, epgUrl;
    
    if(c_DataSourceType_Login == DataSource()) {
        const PlistProviderType providerType = ProviderType();
        if(c_PlistProvider_SharaTv == providerType) {
            if(SharaTvLogin().empty() || SharaTvPassword().empty())
                throw AuthFailedException();
            //http://tvfor.pro/g/xxx:yyy/1/playlist.m3u
            playlistUrl = string("http://tvfor.pro/g/") +  SharaTvLogin() + ":" + SharaTvPassword() + "/1/playlist.m3u";
        } else if(c_PlistProvider_Ottg == providerType){
            playlistUrl = string("http://pl.ottg.tv/get.php?username=")
                + OttgLogin() + "&password=" +  OttgPassword()
                + "&type=m3u&output=" + (PreferHls()?"hls": "ts");
            if(!EnableAdult())
                playlistUrl += "&censored=0";
            epgUrl = "http://ottg.tv/epg.xml.gz";
        } else {
            // Unsupported provider fro login/pass
            throw AuthFailedException();
        }
    } else {
        if(c_PathType_Local == PlaylistPathType()){
            playlistUrl = PlayListPath();
        } else {
            playlistUrl = PlayListUrl();
        }
        epgUrl = EpgUrl();
    }
    
    m_clientCore = m_core = new SharaTvEngine::Core(playlistUrl, epgUrl, EnableAdult());
    m_core->IncludeCurrentEpgToArchive(HowToAddCurrentEpgToArchive());
    m_core->SetEpgCorrectionShift(EpgCorrectionShift());
    m_core->SupportMuticastUrls(SuppotMulticastUrls(), UdpProxyHost(), UdpProxyPort());
    if(c_PlistProvider_SharaTv == ProviderType()) {
        // Shara TV supports archive length up to 2 hours
        // Split EPG items longer than 120 min to avoid this limitation
        m_core->SetMaxArchiveDuration(120 * 60);
    }
    m_core->InitAsync(clearEpgCache, IsArchiveSupported());
}

PVR_ERROR SharaTvPVRClient::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    pCapabilities->bSupportsRadio = false;
    pCapabilities->bSupportsChannelGroups = true;
    pCapabilities->bHandlesInputStream = true;
//    pCapabilities->bSupportsRecordings = true;
    
    pCapabilities->bSupportsTimers = false;
    pCapabilities->bSupportsChannelScan = false;
    pCapabilities->bHandlesDemuxing = false;
    pCapabilities->bSupportsRecordingPlayCount = false;
    pCapabilities->bSupportsLastPlayedPosition = false;
    pCapabilities->bSupportsRecordingEdl = false;
    
    return PVRClientBase::GetAddonCapabilities(pCapabilities);
}

PVR_ERROR  SharaTvPVRClient::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    return PVRClientBase::MenuHook(menuhook, item);
}

ADDON_STATUS SharaTvPVRClient::OnReloadEpg()
{
    ADDON_STATUS retVal = CreateCoreSafe(true);
    
    if(ADDON_STATUS_OK == retVal && nullptr != m_core){
        std::time_t startTime = std::time(nullptr);
        startTime = std::mktime(std::gmtime(&startTime));
        // Request EPG for all channels from -7 to +1 days
        time_t endTime = startTime + 7 * 24 * 60 * 60;
        startTime -= 7 * 24 * 60 * 60;
        
        m_core->UpdateEpgForAllChannels(startTime, endTime, [](){return false;});
    }
    
    return retVal;
}


class SharaTvArchiveDelegate : public Buffers::IPlaylistBufferDelegate
{
public:
    SharaTvArchiveDelegate(SharaTvEngine::Core* core, const PVR_RECORDING &recording, uint32_t startPadding, uint32_t endPadding)
    : _duration(recording.iDuration + startPadding + endPadding)
    , _recordingTime(recording.recordingTime - startPadding)
    , _core(core)
    {
        _channelId = 1;
        
        // NOTE: Kodi does NOT provide recording.iChannelUid for unknown reason
        // Worrkaround: use EPG entry
        EpgEntry epgTag;
        int recId = stoi(recording.strRecordingId);
        if(!_core->GetEpgEntry(recId, epgTag)){
            LogError("Failed to obtain EPG tag for record ID %d. First channel ID will be used", recId);
            return;
        }
        
        _channelId =  epgTag.UniqueChannelId;
    }
    virtual int SegmentsAmountToCache() const {
        // 10 segments cache
        return 10;
    }

    virtual time_t Duration() const
    {
        time_t fromNow = time(nullptr) - _recordingTime;
        return (difftime(_duration, fromNow) < 0 ? _duration : fromNow);
    }
    virtual std::string UrlForTimeshift(time_t timeshiftReqested, time_t* timeshiftAdjusted = nullptr) const
    {
        //auto startTime = std::min(_recordingTime + timeshiftReqested, _recordingTime + Duration());
        time_t startTime = _recordingTime + timeshiftReqested;
        if(startTime < _recordingTime)
            startTime = _recordingTime;
        if(timeshiftAdjusted)
            *timeshiftAdjusted = startTime - _recordingTime;
        return  _core->GetArchiveUrl(_channelId, startTime, Duration());
    }
    
private:
    const time_t _duration;
    const time_t _recordingTime;
    PvrClient::ChannelId _channelId;
    SharaTvEngine::Core* _core;
};

bool SharaTvPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(NULL == m_core)
        return false;
    
    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
    auto delegate = new SharaTvArchiveDelegate(m_core, recording, StartRecordingPadding(), EndRecordingPadding());
    string url = delegate->UrlForTimeshift(0);
    if(!IsSeekSupported())
        SAFE_DELETE(delegate);
    RecordingStreamFlags flags = (RecordingStreamFlags)
    (
        (ProviderType() == c_PlistProvider_SharaTv ? ForcePlaylist : NoRecordingFlags) |
        (IsSeekSupported() ? SupportVodSeek : NoRecordingFlags)
    );
    return PVRClientBase::OpenRecordedStream(url, delegate, flags);
}

PVR_ERROR SharaTvPVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Playlist");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_core == NULL) ? "Not connected" :"OK");
    //const static int providerNames[] = {32023, 70008,70009,70011};
    const char* const providerNames[] = {"Other", "Shara TV", "OTTG"};
    int provideType = ProviderType();
    if(provideType > sizeof(providerNames) / sizeof(providerNames[0]))
        provideType = sizeof(providerNames) / sizeof(providerNames[0]) - 1;
    
    //const char* test = XBMC_Message(providerNames[provideType]);
    snprintf(signalStatus.strProviderName, sizeof(signalStatus.strProviderName), "%s",  providerNames[provideType]);
    
    return this->PVRClientBase::SignalStatus(signalStatus);
}

#pragma mark - Settings

static const std::string c_adult_setting("sharatv_adult");
static const std::string c_prefer_hls("playlist_prefer_hls");
static const std::string c_data_source_type("sharatv_data_source");
static const std::string c_epg_path("sharatv_epg_path");
static const std::string c_playlist_path_type("sharatv_playlist_path_type");
static const std::string c_playlist_url("sharatv_playlist_path"); // preserve old setting name :(
static const std::string c_playlist_path("sharatv_playlist_path_local");

static const std::string c_plist_provider("plist_provider_type");
static const std::string c_sharatv_login("sharatv_login");
static const std::string c_sharatv_password("sharatv_password");
static const std::string c_ottg_login("ottg_login");
static const std::string c_ottg_password("ottg_password");

static const std::string c_suppotMulticastUrls("playlist_support_multicast_urls");
static const std::string c_udpProxyHost("playlist_udp_proxy_host");
static const std::string c_udpProxyPort("playlist_udp_proxy_port");

template<typename T>
static void NotifyClearPvrData(std::function<const T&()>) {
    XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32016));
}

void SharaTvPVRClient::PopulateSettings(AddonSettingsMutableDictionary& settings)
{
    settings
    .Add(c_epg_path, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_data_source_type, (int)c_DataSourceType_Login,  NotifyClearPvrData<int>, ADDON_STATUS_NEED_RESTART)
    .Add(c_adult_setting, false, NotifyClearPvrData<bool>, ADDON_STATUS_NEED_RESTART)
    .Add(c_playlist_path_type, (int)c_PathType_Url, NotifyClearPvrData<int>, ADDON_STATUS_NEED_RESTART)
    .Add(c_playlist_url, "", NotifyClearPvrData<std::string>, ADDON_STATUS_NEED_RESTART)
    .Add(c_playlist_path, "", NotifyClearPvrData<std::string>, ADDON_STATUS_NEED_RESTART)
    .Add(c_plist_provider, (int)c_PlistProvider_SharaTv, NotifyClearPvrData<int>, ADDON_STATUS_NEED_RESTART)
    .Add(c_sharatv_login, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_sharatv_password, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_ottg_login, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_ottg_password, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_prefer_hls, true, NotifyClearPvrData<bool>, ADDON_STATUS_NEED_RESTART)
    .Add(c_suppotMulticastUrls, false, NotifyClearPvrData<bool>, ADDON_STATUS_NEED_RESTART)
    .Add(c_udpProxyHost, "", NotifyClearPvrData<std::string>, ADDON_STATUS_NEED_RESTART)
    .Add(c_udpProxyPort, 0, NotifyClearPvrData<int>, ADDON_STATUS_NEED_RESTART);
}

uint32_t SharaTvPVRClient::UdpProxyPort() const
{
    int port = m_addonSettings.GetInt(c_udpProxyPort);
    if(port < 0)
        port = 0;
    return port;
}

const std::string& SharaTvPVRClient::UdpProxyHost() const
{
    return m_addonSettings.GetString(c_udpProxyHost);
}

bool SharaTvPVRClient::SuppotMulticastUrls() const
{
    return m_addonSettings.GetBool(c_suppotMulticastUrls);
}

bool SharaTvPVRClient::PreferHls() const
{
    return m_addonSettings.GetBool(c_prefer_hls);
}

const std::string& SharaTvPVRClient::OttgLogin() const
{
    return m_addonSettings.GetString(c_ottg_login);
}

const std::string& SharaTvPVRClient::OttgPassword() const
{
    return m_addonSettings.GetString(c_ottg_password);
}

SharaTvPVRClient::PlistProviderType SharaTvPVRClient::ProviderType() const
{
    return (PlistProviderType)m_addonSettings.GetInt(c_plist_provider);
}

SharaTvPVRClient::PathType SharaTvPVRClient::PlaylistPathType() const
{
    return (PathType)m_addonSettings.GetInt(c_playlist_path_type);
}

const std::string& SharaTvPVRClient::PlayListPath() const
{
    return m_addonSettings.GetString(c_playlist_path);
}

const std::string& SharaTvPVRClient::PlayListUrl() const
{
    return m_addonSettings.GetString(c_playlist_url);
}

const std::string& SharaTvPVRClient::SharaTvLogin() const
{
    return m_addonSettings.GetString(c_sharatv_login);
}

const std::string& SharaTvPVRClient::SharaTvPassword() const
{
    return m_addonSettings.GetString(c_sharatv_password);
}

const std::string& SharaTvPVRClient::EpgUrl() const
{
    return m_addonSettings.GetString(c_epg_path);
}

SharaTvPVRClient::DataSourceType SharaTvPVRClient::DataSource() const
{
    return (DataSourceType)m_addonSettings.GetInt(c_data_source_type);
}

bool SharaTvPVRClient::EnableAdult() const
{
    return m_addonSettings.GetBool(c_adult_setting);
}



