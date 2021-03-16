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
#include "kodi/General.h"

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
using namespace SharaTvEngine;
using namespace PvrClient;
using namespace Helpers;

ADDON_STATUS SharaTvPVRClient::Init(const std::string& clientPath, const std::string& userPath)
{
    ADDON_STATUS retVal = PVRClientBase::Init(clientPath, userPath);
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
     
    kodi::QueueFormattedNotification(QUEUE_ERROR, kodi::GetLocalizedString(32007).c_str(), providerName);
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
        kodi::QueueFormattedNotification(QUEUE_ERROR, "Shara TV: unhandeled exception on core creation.");
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
    m_core->SetLocalLogosFolder(LocalLogosFolder());
    if(c_PlistProvider_SharaTv == ProviderType()) {
        // Shara TV supports archive length up to 2 hours
        // Split EPG items longer than 120 min to avoid this limitation
        m_core->SetMaxArchiveDuration(120 * 60);
    }
    m_core->InitAsync(clearEpgCache, IsArchiveSupported());
}

PVR_ERROR SharaTvPVRClient::GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
    capabilities.SetSupportsEPG(true);
    capabilities.SetSupportsTV(true);
    capabilities.SetSupportsRadio(false);
    capabilities.SetSupportsChannelGroups(true);
    capabilities.SetHandlesInputStream(true);
//    capabilities.SetSupportsRecordings(true);
    
    capabilities.SetSupportsTimers(false);
    capabilities.SetSupportsChannelScan(false);
    capabilities.SetHandlesDemuxing(false);
    capabilities.SetSupportsRecordingPlayCount(false);
    capabilities.SetSupportsLastPlayedPosition(false);
    capabilities.SetSupportsRecordingEdl(false);
    
    return PVRClientBase::GetAddonCapabilities(capabilities);
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
    SharaTvArchiveDelegate(SharaTvEngine::Core* core, const kodi::addon::PVRRecording& recording, uint32_t startPadding, uint32_t endPadding)
    : _duration(recording.GetDuration() + startPadding + endPadding)
    , _recordingTime(recording.GetRecordingTime() - startPadding)
    , _core(core)
    {
        _channelId = 1;
        
        // NOTE: Kodi does NOT provide recording.iChannelUid for unknown reason
        // Worrkaround: use EPG entry
        EpgEntry epgTag;
        int recId = stoi(recording.GetRecordingId().c_str());
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

bool SharaTvPVRClient::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
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

PVR_ERROR SharaTvPVRClient::SignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus)
{
    signalStatus.SetAdapterName("IPTV Playlist");
    signalStatus.SetAdapterStatus((m_core == NULL) ? "Not connected" :"OK");
    //const static int providerNames[] = {32023, 70008,70009,70011};
    const char* const providerNames[] = {"Other", "Shara TV", "OTTG"};
    int provideType = ProviderType();
    if(provideType > sizeof(providerNames) / sizeof(providerNames[0]))
        provideType = sizeof(providerNames) / sizeof(providerNames[0]) - 1;
    
    //const char* test = kodi::GetLocalizedString(providerNames[provideType]);
    signalStatus.SetProviderName(providerNames[provideType]);
    
    return this->PVRClientBase::SignalStatus(channelUid, signalStatus);
}

#pragma mark - Settings

template<typename T>
static void NotifyClearPvrData(std::function<const T&()>) {
    kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32016).c_str());
}

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

void SharaTvPVRClient::PopulateSettings(AddonSettingsMutableDictionary& settings)
{
    // NOTE: where default type value (i.e. T{}) is a valid value
    // do not set default setting value other then T{} (use setting XML instead)
    // OTherwise T{} will be replaced with default vaue on AddonSettings.Init()
    settings
    .Add(c_epg_path, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_data_source_type, (int)c_DataSourceType_Login,  NotifyClearPvrData<int>, ADDON_STATUS_NEED_RESTART)
    .Add(c_adult_setting, false, NotifyClearPvrData<bool>, ADDON_STATUS_NEED_RESTART)
    .Add(c_playlist_path_type, (int)c_PathType_Url, NotifyClearPvrData<int>, ADDON_STATUS_NEED_RESTART)
    .Add(c_playlist_url, "", NotifyClearPvrData<std::string>, ADDON_STATUS_NEED_RESTART)
    .Add(c_playlist_path, "", NotifyClearPvrData<std::string>, ADDON_STATUS_NEED_RESTART)
    .Add(c_plist_provider, (int)c_PlistProvider_Other, NotifyClearPvrData<int>, ADDON_STATUS_NEED_RESTART)
    .Add(c_sharatv_login, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_sharatv_password, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_ottg_login, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_ottg_password, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_prefer_hls, false, NotifyClearPvrData<bool>, ADDON_STATUS_NEED_RESTART) // default should be true, but see note above
    ;
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



