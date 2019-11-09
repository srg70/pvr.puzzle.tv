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

using namespace Globals;
using namespace std;
using namespace ADDON;
using namespace SharaTvEngine;
using namespace PvrClient;

static const char* c_login_setting = "sharatv_login";
static const char* c_password_setting = "sharatv_password";
static const char* c_adult_setting = "sharatv_adult";
static const char* c_data_source_type = "sharatv_data_source";
static const char* c_playlist_path = "sharatv_playlist_path";
//static const char* c_seek_archives = "ttv_seek_archives";




ADDON_STATUS SharaTvPVRClient::Init(PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = PVRClientBase::Init(pvrprops);
    if(ADDON_STATUS_OK != retVal)
        return retVal;
    
    char buffer[1024];
    
    XBMC->GetSetting(c_data_source_type, &m_dataSourceType);
    
    if (XBMC->GetSetting(c_login_setting, &buffer))
        m_login = buffer;
    if (XBMC->GetSetting(c_password_setting, &buffer))
        m_password = buffer;

    if (XBMC->GetSetting(c_playlist_path, &buffer))
        m_playListUrl = buffer;

    
    SetSeekSupported(true);
//    XBMC->GetSetting(c_seek_archives, &m_supportSeek);
    
    m_enableAdult = false;
    XBMC->GetSetting(c_adult_setting, &m_enableAdult);
    
    retVal = CreateCoreSafe(false);
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;
    
}

SharaTvPVRClient::~SharaTvPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    DestroyCoreSafe();
}

ADDON_STATUS SharaTvPVRClient::CreateCoreSafe(bool clearEpgCache)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    try
    {
        CreateCore(clearEpgCache);
    }
    catch (AuthFailedException &)
    {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32007), "Shara TV");
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
    
    string playlistUrl;
    
    if(c_DataSourceType_Login == m_dataSourceType) {
        if(m_login.empty() || m_password.empty())
            throw AuthFailedException();
        //http://tvfor.pro/g/xxx:yyy/1/playlist.m3u
        playlistUrl = string("http://tvfor.pro/g/") +  m_login + ":" + m_password + "/1/playlist.m3u";
    } else {
        playlistUrl = m_playListUrl;
    }
    
    m_clientCore = m_core = new SharaTvEngine::Core(playlistUrl, m_enableAdult);
    m_core->IncludeCurrentEpgToArchive(m_addCurrentEpgToArchive);
    m_core->InitAsync(clearEpgCache);
}


ADDON_STATUS SharaTvPVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;
    
    if (strcmp(settingName, c_login_setting) == 0) {
        if(strcmp((const char*) settingValue, m_login.c_str()) != 0) {
            m_login = (const char*) settingValue;
            result = ADDON_STATUS_NEED_RESTART;
        }
    } else if (strcmp(settingName, c_password_setting) == 0) {
        if(strcmp((const char*) settingValue, m_password.c_str()) != 0){
            m_password = (const char*) settingValue;
            result = ADDON_STATUS_NEED_RESTART;
        }
    } else if(strcmp(settingName,  c_adult_setting) == 0) {
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
        
    } else if(strcmp(settingName,  c_data_source_type) == 0) {
        XBMC->GetSetting(c_data_source_type, &m_dataSourceType);
        result = ADDON_STATUS_NEED_RESTART;
    } else if(strcmp(settingName,  c_playlist_path) == 0) {
        if(strcmp((const char*) settingValue, m_playListUrl.c_str()) != 0){
            m_playListUrl = (const char*) settingValue;
            result = ADDON_STATUS_NEED_RESTART;
        }
    } else {
        result = PVRClientBase::SetSetting(settingName, settingValue);
    }
    return result;
}

PVR_ERROR SharaTvPVRClient::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
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
        
        m_core->UpdateEpgForAllChannels(startTime, endTime);
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
        return  _core->GetArchiveUrl(_channelId, startTime, _duration);
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
    
    auto delegate = new SharaTvArchiveDelegate(m_core, recording, GetStartRecordingPadding(), GetEndRecordingPadding());
    string url = delegate->UrlForTimeshift(0);
    if(!IsSeekSupported())
        SAFE_DELETE(delegate);
    RecordingStreamFlags flags = (RecordingStreamFlags)(ForcePlaylist | (IsSeekSupported() ? SupportVodSeek : NoRecordingFlags));
    return PVRClientBase::OpenRecordedStream(url, delegate, flags);
}

PVR_ERROR SharaTvPVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Shara TV");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_core == NULL) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}




