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
#include <WinSock2.h>
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
#include "edem_pvr_client.h"
#include "helpers.h"
#include "edem_player.h"
#include "plist_buffer.h"
#include "globals.hpp"

using namespace Globals;
using namespace std;
using namespace EdemEngine;
using namespace PvrClient;
using namespace Helpers;

static const char* c_playlist_setting = "edem_playlist_url";
static const char* c_epg_setting = "edem_epg_url";
static const char* c_seek_archives = "edem_seek_archives";
static const char* c_edem_adult = "edem_adult";

ADDON_STATUS EdemPVRClient::Init(const std::string& clientPath, const std::string& userPath)
{
    ADDON_STATUS retVal = PVRClientBase::Init(clientPath, userPath);
    if(ADDON_STATUS_OK != retVal)
        return retVal;
    
    m_playlistUrl = kodi::GetSettingString(c_playlist_setting);
    m_epgUrl = kodi::GetSettingString(c_epg_setting);
    
    SetSeekSupported(kodi::GetSettingBoolean(c_seek_archives, false));
    
    m_enableAdult = kodi::GetSettingBoolean(c_edem_adult, false);
    
    retVal = CreateCoreSafe(false);
    
    return retVal;
    
}

void EdemPVRClient::PopulateSettings(AddonSettingsMutableDictionary& settings)
{
    
}

EdemPVRClient::~EdemPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    DestroyCoreSafe();
}

ADDON_STATUS EdemPVRClient::CreateCoreSafe(bool clearEpgCache)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    try
    {
        CreateCore(clearEpgCache);
        OnCoreCreated();
    }
    catch (AuthFailedException &)
    {
        kodi::QueueFormattedNotification(QUEUE_ERROR, kodi::GetLocalizedString(32011).c_str());
    }
    catch(...)
    {
        kodi::QueueFormattedNotification(QUEUE_ERROR, "Edem TV: unhandeled exception on core creation.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    return retVal;
}

void EdemPVRClient::DestroyCoreSafe()
{
    if(m_core != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_core);
    }

}

bool EdemPVRClient::CheckEdemPlaylistUrl()
{
    if(std::string::npos != m_playlistUrl.find("***"))
        return false;
    return PVRClientBase::CheckPlaylistUrl(m_playlistUrl);
}

void EdemPVRClient::CreateCore(bool clearEpgCache)
{
    DestroyCoreSafe();
    
    if(!CheckEdemPlaylistUrl())
        throw AuthFailedException();
    m_clientCore = m_core = new EdemEngine::Core(m_playlistUrl, m_epgUrl, m_enableAdult);
    m_core->IncludeCurrentEpgToArchive(HowToAddCurrentEpgToArchive());
    m_core->SetEpgCorrectionShift(EpgCorrectionShift());
    m_core->SetLocalLogosFolder(LocalLogosFolder());
    m_core->InitAsync(clearEpgCache, IsArchiveSupported());

}

ADDON_STATUS EdemPVRClient::SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;
    
    if (c_playlist_setting == settingName && settingValue.GetString() != m_playlistUrl) {
        m_playlistUrl= settingValue.GetString();
        if(!CheckEdemPlaylistUrl()) {
            return result;
        } else {
            result = CreateCoreSafe(false);
        }
    }
    else if(c_epg_setting == settingName && settingValue.GetString() != m_epgUrl) {
        m_epgUrl = settingValue.GetString();
        result = CreateCoreSafe(false);
    }
    else if(c_seek_archives == settingName) {
        SetSeekSupported(settingValue.GetBoolean());
    }
    else if(c_edem_adult == settingName) {
        bool newValue = settingValue.GetBoolean();
        if(newValue != m_enableAdult) {
            m_enableAdult = newValue;
            result = CreateCoreSafe(false);
            m_clientCore->CallRpcAsync("{\"jsonrpc\": \"2.0\", \"method\": \"GUI.ActivateWindow\", \"params\": {\"window\": \"pvrsettings\"},\"id\": 1}",
                                       [&] (rapidjson::Document& jsonRoot) {
                                            kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32016).c_str());
                                       },
                                       [&](const ActionQueue::ActionResult& s) {});
        }

    } else {
        result = PVRClientBase::SetSetting(settingName, settingValue);
    }
    return result;
}

PVR_ERROR EdemPVRClient::GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities)
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

ADDON_STATUS EdemPVRClient::OnReloadEpg()
{
    return CreateCoreSafe(true);
}


class EdemArchiveDelegate : public Buffers::IPlaylistBufferDelegate
{
public:
    EdemArchiveDelegate(EdemEngine::Core* core, const kodi::addon::PVRRecording& recording, uint32_t startPadding, uint32_t endPadding)
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
        // 20 segments cache
        return 20;
    }
    
    virtual time_t Duration() const
    {
        time_t fromNow = time(nullptr) - _recordingTime;
        return (difftime(_duration, fromNow) < 0 ? _duration : fromNow) ;
    }
    virtual std::string UrlForTimeshift(time_t timeshiftReqested, time_t* timeshiftAdjusted = nullptr) const
    {
        //auto startTime = std::min(_recordingTime + timeshiftReqested, _recordingTime + Duration());
        auto startTime = _recordingTime + timeshiftReqested;
        if(startTime < _recordingTime)
            startTime = _recordingTime;
        if(timeshiftAdjusted)
            *timeshiftAdjusted = startTime - _recordingTime;
        return  _core->GetArchiveUrl(_channelId, startTime);
    }
    
private:
    const time_t _duration;
    const time_t _recordingTime;
    PvrClient::ChannelId _channelId;
    EdemEngine::Core* _core;
};

bool EdemPVRClient::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
    if(NULL == m_core)
        return false;
    
    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
    auto delegate = new EdemArchiveDelegate(m_core, recording, StartRecordingPadding(), EndRecordingPadding());
    string url = delegate->UrlForTimeshift(0);
    if(!IsSeekSupported())
        SAFE_DELETE(delegate);
    return PVRClientBase::OpenRecordedStream(url, delegate, IsSeekSupported() ? SupportVodSeek : NoRecordingFlags);
}

PVR_ERROR EdemPVRClient::SignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus)
{
    signalStatus.SetAdapterName("IPTV Edem TV");
    signalStatus.SetAdapterStatus((m_core == NULL) ? "Not connected" :"OK");
    return this->PVRClientBase::SignalStatus(channelUid, signalStatus);
}




