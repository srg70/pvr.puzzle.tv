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
#include "kodi/xbmc_addon_cpp_dll.h"

#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "edem_pvr_client.h"
#include "helpers.h"
#include "edem_player.h"
#include "plist_buffer.h"
#include "globals.hpp"

using namespace Globals;
using namespace std;
using namespace ADDON;
using namespace EdemEngine;
using namespace PvrClient;

static const char* c_playlist_setting = "edem_playlist_url";
static const char* c_epg_setting = "edem_epg_url";
static const char* c_seek_archives = "edem_seek_archives";
static const char* c_edem_adult = "edem_adult";

ADDON_STATUS EdemPVRClient::Init(PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = PVRClientBase::Init(pvrprops);
    if(ADDON_STATUS_OK != retVal)
        return retVal;
    
    char buffer[1024];
    
    if (XBMC->GetSetting(c_playlist_setting, &buffer))
        m_playlistUrl = buffer;
    if (XBMC->GetSetting(c_epg_setting, &buffer))
        m_epgUrl = buffer;
    
    bool supportSeek = false;
    XBMC->GetSetting(c_seek_archives, &supportSeek);
    SetSeekSupported(supportSeek);
    
    m_enableAdult = false;
    XBMC->GetSetting(c_edem_adult, &m_enableAdult);
    
    retVal = CreateCoreSafe(false);
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;
    
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
    }
    catch (AuthFailedException &)
    {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32011));
    }
    catch(...)
    {
        XBMC->QueueNotification(QUEUE_ERROR, "Edem TV: unhandeled exception on core creation.");
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

void EdemPVRClient::CreateCore(bool clearEpgCache)
{
    DestroyCoreSafe();
    
    if(PVRClientBase::CheckPlaylistUrl(m_playlistUrl)) {
        m_clientCore = m_core = new EdemEngine::Core(m_playlistUrl, m_epgUrl, m_enableAdult);
        m_core->IncludeCurrentEpgToArchive(m_addCurrentEpgToArchive);
        m_core->InitAsync(clearEpgCache);
    }
}

ADDON_STATUS EdemPVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;
    
    if (strcmp(settingName,  c_playlist_setting) == 0 && strcmp((const char*) settingValue, m_playlistUrl.c_str()) != 0) {
        m_playlistUrl= (const char*) settingValue;
        if(!PVRClientBase::CheckPlaylistUrl(m_playlistUrl)) {
            return result;
        } else {
            result = CreateCoreSafe(false);
        }
    }
    else if(strcmp(settingName,  c_epg_setting) == 0 && strcmp((const char*) settingValue, m_epgUrl.c_str()) != 0) {
        m_epgUrl = (const char*) settingValue;
        result = CreateCoreSafe(false);
    }
    else if(strcmp(settingName,  c_seek_archives) == 0) {
        SetSeekSupported(*(const bool*) settingValue);
    }
    else if(strcmp(settingName,  c_edem_adult) == 0) {
        bool newValue = *(const bool*) settingValue;
        if(newValue != m_enableAdult) {
            m_enableAdult = newValue;
            result = CreateCoreSafe(false);
            m_clientCore->CallRpcAsync("{\"jsonrpc\": \"2.0\", \"method\": \"GUI.ActivateWindow\", \"params\": {\"window\": \"pvrsettings\"},\"id\": 1}",
                                       [&] (rapidjson::Document& jsonRoot) {
                                           XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32016));
                                       },
                                       [&](const ActionQueue::ActionResult& s) {});
        }

    } else {
        result = PVRClientBase::SetSetting(settingName, settingValue);
    }
    return result;
}

PVR_ERROR EdemPVRClient::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    pCapabilities->bSupportsRadio = false;
    pCapabilities->bSupportsChannelGroups = true;
    pCapabilities->bHandlesInputStream = true;
    pCapabilities->bSupportsRecordings = true;
    
    pCapabilities->bSupportsTimers = false;
    pCapabilities->bSupportsChannelScan = false;
    pCapabilities->bHandlesDemuxing = false;
    pCapabilities->bSupportsRecordingPlayCount = false;
    pCapabilities->bSupportsLastPlayedPosition = false;
    pCapabilities->bSupportsRecordingEdl = false;
    
    return PVRClientBase::GetAddonCapabilities(pCapabilities);
}

PVR_ERROR  EdemPVRClient::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    return PVRClientBase::MenuHook(menuhook, item);
}

ADDON_STATUS EdemPVRClient::OnReloadEpg()
{
    return CreateCoreSafe(true);
}


class EdemArchiveDelegate : public Buffers::IPlaylistBufferDelegate
{
public:
    EdemArchiveDelegate(EdemEngine::Core* core, const PVR_RECORDING &recording)
    : _duration(recording.iDuration)
    , _recordingTime(recording.recordingTime)
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
        
        _channelId =  epgTag.ChannelId;
    }
    virtual int SegmentsAmountToCache() const {
        // 20 segments cache
        return 20;
    }
    
    virtual time_t Duration() const
    {
        time_t fromNow = time(nullptr) - _recordingTime;
        return std::min(_duration, fromNow) ;
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

bool EdemPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(NULL == m_core)
        return false;
    
    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
    auto delegate = new EdemArchiveDelegate(m_core, recording);
    string url = delegate->UrlForTimeshift(0);
    if(!IsSeekSupported())
        SAFE_DELETE(delegate);
    return PVRClientBase::OpenRecordedStream(url, delegate, IsSeekSupported() ? SupportVodSeek : NoRecordingFlags);
}

PVR_ERROR EdemPVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Edem TV");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_core == NULL) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}




