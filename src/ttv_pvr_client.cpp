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
#include "ttv_pvr_client.h"
#include "helpers.h"
#include "ttv_player.h"
#include "plist_buffer.h"
#include "globals.hpp"

using namespace Globals;
using namespace std;
using namespace ADDON;
using namespace TtvEngine;
using namespace PvrClient;

static const char* c_playlist_setting = "ttv_playlist_url";
static const char* c_epg_setting = "ttv_epg_url";
static const char* c_seek_archives = "ttv_seek_archives";
static const char* c_ttv_mode = "ttv_mode";
static const char* c_ttv_user = "ttv_user";
static const char* c_ttv_password = "ttv_password";
static const char* c_ttv_use_acestream = "ttv_use_acestream";
static const char* c_ttv_ace_server_uri = "ttv_ace_server_uri";
static const char* c_ttv_ace_server_port = "ttv_ace_server_port";
static const char* c_ttv_adult = "ttv_adult";

ADDON_STATUS TtvPVRClient::Init(PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = PVRClientBase::Init(pvrprops);
    if(ADDON_STATUS_OK != retVal)
        return retVal;
    
    char buffer[1024];
    
    if (XBMC->GetSetting(c_playlist_setting, &buffer))
        m_playlistUrl = buffer;
    if (XBMC->GetSetting(c_epg_setting, &buffer))
        m_epgUrl = buffer;
    
    m_supportSeek = false;
    XBMC->GetSetting(c_seek_archives, &m_supportSeek);
    m_enableAdultContent = false;
    XBMC->GetSetting(c_ttv_adult, &m_enableAdultContent);

    m_ttvMode = TTVMode_api;
    XBMC->GetSetting(c_ttv_mode, &m_ttvMode);
    if (XBMC->GetSetting(c_ttv_user, &buffer))
        m_user = buffer;
    if (XBMC->GetSetting(c_ttv_password, &buffer))
        m_password = buffer;
    
    m_useAce = false;
    XBMC->GetSetting(c_ttv_use_acestream, &m_useAce);
    m_aceServerUri = "127.0.0.1";
    if (XBMC->GetSetting(c_ttv_ace_server_uri, &buffer))
        m_aceServerUri = buffer;
    m_aceServerPort = 6878;
    XBMC->GetSetting(c_ttv_ace_server_port, &m_aceServerPort);

    m_currentChannelStreamIdx = -1;

    
    retVal = CreateCoreSafe(false);
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;
    
}

TtvPVRClient::~TtvPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    DestroyCoreSafe();
}

ADDON_STATUS TtvPVRClient::CreateCoreSafe(bool clearEpgCache)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    try
    {
        CreateCore(clearEpgCache);
    }
    catch (AuthFailedException &)
    {
        char* message = XBMC->GetLocalizedString(32011);
        XBMC->QueueNotification(QUEUE_ERROR, message);
        XBMC->FreeString(message);
    }
    catch (exception & ex)
    {
        LogError("Torrent TV: exception on core creation: %s", ex.what());
        XBMC->QueueNotification(QUEUE_ERROR, "Torrent TV: exception on core creation.");
    }
    catch(...)
    {
        XBMC->QueueNotification(QUEUE_ERROR, "Torrent TV: unhandeled exception on core creation.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    return retVal;
}

void TtvPVRClient::DestroyCoreSafe()
{
    if(m_core != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_core);
    }

}

void TtvPVRClient::CreateCore(bool clearEpgCache)
{
    DestroyCoreSafe();
    
        switch(m_ttvMode)
        {
            case TTVMode_api:
            {
                TtvEngine::Core::CoreParams cp;
                cp.user = m_user;
                cp.password = m_password;
                cp.useAce = m_useAce;
                cp.aceServerUri = m_aceServerUri;
                cp.aceServerPort = m_aceServerPort;
                cp.enableAdult = m_enableAdultContent;
                m_core = new TtvEngine::Core(cp);
            }
                break;
            case TTVMode_playlist:
                if(!PVRClientBase::CheckPlaylistUrl(m_playlistUrl)) {
                    LogError("Invalid TTV playlist URL.");
                    return;
                }
                m_core = new TtvEngine::Core(m_playlistUrl, m_epgUrl);
                break;
            default:
                LogError("Unknown TTV mode %d", m_ttvMode);
                throw;
        }
        m_clientCore = m_core;
        m_core->IncludeCurrentEpgToArchive(m_addCurrentEpgToArchive);
        m_core->InitAsync(clearEpgCache);
}

ADDON_STATUS TtvPVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;
    
    if (strcmp(settingName,  c_playlist_setting) == 0 && strcmp((const char*) settingValue, m_playlistUrl.c_str()) != 0) {
//        m_playlistUrl= (const char*) settingValue;
//        if(!CheckPlaylistUrl()) {
//            return result;
//        } else {
//            result = CreateCoreSafe(false);
            result = ADDON_STATUS_NEED_RESTART;
//        }
    }
    else if ((strcmp(settingName,  c_ttv_user) == 0 && strcmp((const char*) settingValue, m_user.c_str()) != 0)
           || (strcmp(settingName,  c_ttv_password) == 0 && strcmp((const char*) settingValue, m_password.c_str()) != 0)) {
        if(nullptr != m_core){
            m_core->ClearSession();
        }
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(strcmp(settingName,  c_epg_setting) == 0 && strcmp((const char*) settingValue, m_epgUrl.c_str()) != 0) {
//        m_epgUrl = (const char*) settingValue;
//        result = CreateCoreSafe(false);
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(strcmp(settingName,  c_seek_archives) == 0) {
        m_supportSeek = *(const bool*) settingValue;
    }
    else if (strcmp(settingName,  c_ttv_adult) == 0) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(strcmp(settingName,  c_ttv_mode) == 0) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(strcmp(settingName,  c_ttv_use_acestream) == 0) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else {
        result = PVRClientBase::SetSetting(settingName, settingValue);
    }
    return result;
}

PVR_ERROR TtvPVRClient::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
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

PVR_ERROR  TtvPVRClient::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    return PVRClientBase::MenuHook(menuhook, item);
}

ADDON_STATUS TtvPVRClient::OnReloadEpg()
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


string TtvPVRClient::GetStreamUrl(ChannelId channelId)
{
    m_currentChannelStreamIdx = 0;
    return PVRClientBase::GetStreamUrl(channelId);
}

string TtvPVRClient::GetNextStreamUrl(ChannelId channelId)
{
    if(m_core == nullptr)
        return string();
    LogError("TtvPVRClient:: trying to move to next stream from [%d].", m_currentChannelStreamIdx);
    return m_core->GetNextStream(channelId, m_currentChannelStreamIdx++);
}


class TtvArchiveDelegate : public Buffers::IPlaylistBufferDelegate
{
public:
    TtvArchiveDelegate(TtvEngine::Core* core, const PVR_RECORDING &recording)
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
    TtvEngine::Core* _core;
};

bool TtvPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(NULL == m_core)
        return false;
    
    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
    auto delegate = new TtvArchiveDelegate(m_core, recording);
    string url = delegate->UrlForTimeshift(0);
    if(!m_supportSeek)
        SAFE_DELETE(delegate);
    return PVRClientBase::OpenRecordedStream(url, delegate);
}

PVR_ERROR TtvPVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Torrent TV");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_core == NULL) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}




