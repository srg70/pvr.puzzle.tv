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
#include "kodi/General.h"
#include "p8-platform/util/util.h"

#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "ott_pvr_client.h"
#include "helpers.h"
#include "ott_player.h"
#include "plist_buffer.h"
#include "globals.hpp"

using namespace Globals;
using namespace std;
using namespace PvrClient;
using namespace Helpers;

static const char* c_playlist_setting = "ott_playlist_url";
static const char* c_key_setting = "ott_key";

ADDON_STATUS OttPVRClient::Init(const std::string& clientPath, const std::string& userPath)
{
    ADDON_STATUS retVal = PVRClientBase::Init(clientPath, userPath);
    if(ADDON_STATUS_OK != retVal)
       return retVal;
       
    m_playlistUrl = kodi::GetSettingString(c_playlist_setting);
    m_key = kodi::GetSettingString(c_key_setting);

    SetSeekSupported(true);
    retVal = CreateCoreSafe(false);

    return retVal;

}

void OttPVRClient::PopulateSettings(AddonSettingsMutableDictionary& settings)
{
    
}

OttPVRClient::~OttPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    
    DestroyCoreSafe();
}

ADDON_STATUS OttPVRClient::CreateCoreSafe(bool clearEpgCache)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    try
    {
        CreateCore(clearEpgCache);
        OnCoreCreated();
    }
    catch (OttEngine::AuthFailedException &)
    {
        kodi::QueueFormattedNotification(QUEUE_ERROR, kodi::GetLocalizedString(32007).c_str(), "OTT");
    }
    catch(...)
    {
        kodi::QueueFormattedNotification(QUEUE_ERROR, "OTT club: unhandeled exception on reload EPG.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    return retVal;
}
void OttPVRClient::DestroyCoreSafe()
{
    if(m_core != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_core);
    }
}

void OttPVRClient::CreateCore(bool clearEpgCache)
{
    DestroyCoreSafe();
    
    m_clientCore = m_core = new OttEngine::Core(m_playlistUrl, m_key);
    m_core->IncludeCurrentEpgToArchive(HowToAddCurrentEpgToArchive());
    m_core->SetEpgCorrectionShift(EpgCorrectionShift());
    m_core->SetLocalLogosFolder(LocalLogosFolder());
    m_core->InitAsync(clearEpgCache, IsArchiveSupported());
    OttEngine::Core::TToPublicChannelId f = [this](ChannelId chId) {
        return this->BrodcastIdForChannelId(chId);
    };
    m_core->SetChannelIdConverter(f);

}

ADDON_STATUS OttPVRClient::SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue)
{
    if (c_playlist_setting == settingName && settingValue.GetString() != m_playlistUrl)
    {
        m_playlistUrl = settingValue.GetString();
        if (!m_playlistUrl.empty() && !m_key.empty() && m_core == NULL){
            CreateCoreSafe(false);
        }
    }
    else if (c_key_setting == settingName  && settingValue.GetString() != m_key)
    {
        m_key = settingValue.GetString();
        if (!m_playlistUrl.empty() && !m_key.empty() && m_core == NULL){
            CreateCoreSafe(false);
        }
    }
    else {
        return PVRClientBase::SetSetting(settingName, settingValue);
    }
    return ADDON_STATUS_NEED_RESTART;
}

PVR_ERROR OttPVRClient::GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
    capabilities.SetSupportsEPG(true);
    capabilities.SetSupportsTV(true);
    capabilities.SetSupportsRadio(false);
    capabilities.SetSupportsChannelGroups(true);
    capabilities.SetHandlesInputStream(true);
//    capabilities.SetSupportsRecordings(true)(true);
    
    capabilities.SetSupportsTimers(false);
    capabilities.SetSupportsChannelScan(false);
    capabilities.SetHandlesDemuxing(false);
    capabilities.SetSupportsRecordingPlayCount(false);
    capabilities.SetSupportsLastPlayedPosition(false);
    capabilities.SetSupportsRecordingEdl(false);
    
    return PVRClientBase::GetAddonCapabilities(capabilities);
}

ADDON_STATUS OttPVRClient::OnReloadEpg()
{
    ADDON_STATUS retVal = CreateCoreSafe(true);
    
//    if(ADDON_STATUS_OK == retVal && nullptr != m_core){
//        std::time_t startTime = std::time(nullptr);
//        startTime = std::mktime(std::gmtime(&startTime));
//        // Request EPG for all channels from -7 to +1 days
//        time_t endTime = startTime + 1 * 24 * 60 * 60;
//        startTime -= 7 * 24 * 60 * 60;
//
//        m_core->UpdateEpgForAllChannels(startTime, endTime);
//    }
    
    return retVal;
}

class OttArchiveDelegate : public Buffers::IPlaylistBufferDelegate
{
public:
    OttArchiveDelegate(OttEngine::Core* core, const kodi::addon::PVRRecording& recording, uint32_t startPadding, uint32_t endPadding)
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
        return (difftime(_duration, fromNow) < 0 ? _duration : fromNow);
    }
    virtual std::string UrlForTimeshift(time_t timeshiftReqested, time_t* timeshiftAdjusted = nullptr) const
    {
        const time_t requested = _recordingTime + timeshiftReqested;
        const time_t duration = _recordingTime + Duration();
        auto startTime = difftime(requested, duration) < 0 ? requested : duration;
        if(startTime < _recordingTime)
            startTime = _recordingTime;
        if(timeshiftAdjusted)
            *timeshiftAdjusted = startTime - _recordingTime;
        return _core->GetArchiveUrl(_channelId, startTime, Duration()  -  (startTime - _recordingTime));
    }
    
private:
    const time_t _duration;
    const time_t _recordingTime;
    PvrClient::ChannelId _channelId;
    OttEngine::Core* _core;
};


bool OttPVRClient::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
    if(NULL == m_core)
        return false;
    
    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
    auto delegate = new OttArchiveDelegate(m_core, recording, StartRecordingPadding(), EndRecordingPadding());
    string url = delegate->UrlForTimeshift(0);
    if(!IsSeekSupported())
        SAFE_DELETE(delegate);
    return PVRClientBase::OpenRecordedStream(url, delegate, IsSeekSupported() ? SupportVodSeek : NoRecordingFlags);
}

PVR_ERROR OttPVRClient::SignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus)
{
    signalStatus.SetAdapterName("IPTV OTT Club");
    signalStatus.SetAdapterStatus((m_core == NULL) ? "Not connected" :"OK");
    return this->PVRClientBase::SignalStatus(channelUid, signalStatus);
}




