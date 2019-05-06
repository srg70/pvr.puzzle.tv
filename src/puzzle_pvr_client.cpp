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

#include <ctime>
#include "p8-platform/util/util.h"
#include "kodi/xbmc_addon_cpp_dll.h"

#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "puzzle_pvr_client.h"
#include "helpers.h"
#include "puzzle_tv.h"
#include "globals.hpp"

using namespace Globals;
using namespace std;
using namespace ADDON;
using namespace PuzzleEngine;
using namespace PvrClient;

static const char* c_server_url_setting = "puzzle_server_uri";
static const char* c_server_port_setting = "puzzle_server_port";
static const char* c_server_retries_setting = "puzzle_server_retries";
static const char* c_epg_provider_setting = "puzzle_server_epg_provider_type";
static const char* c_epg_url_setting = "puzzle_server_epg_url";
static const char* c_epg_port_setting = "puzzle_server_epg_port";
static const char* c_server_version_setting = "puzzle_server_version";

const unsigned int UPDATE_CHANNEL_STREAMS_MENU_HOOK = PVRClientBase::s_lastCommonMenuHookId + 1;
const unsigned int UPDATE_CHANNELS_MENU_HOOK = UPDATE_CHANNEL_STREAMS_MENU_HOOK + 1;


ADDON_STATUS PuzzlePVRClient::Init(PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = PVRClientBase::Init(pvrprops);
    if(ADDON_STATUS_OK != retVal)
       return retVal;
    
    char buffer[1024];
    m_currentChannelStreamIdx = -1;
    int serverPort = 8089;
    m_maxServerRetries = 4;

    XBMC->GetSetting(c_server_port_setting, &m_serverPort);
    if(XBMC->GetSetting(c_server_url_setting, &buffer))
        m_serverUri =buffer;
    XBMC->GetSetting(c_server_retries_setting, &m_maxServerRetries);
    if(m_maxServerRetries < 4)
        m_maxServerRetries = 4;
    if (XBMC->GetSetting(c_epg_url_setting, &buffer))
        m_epgUrl = buffer;
    XBMC->GetSetting(c_epg_provider_setting, &m_epgType);
    if(m_epgType != c_EpgType_Server){
        m_epgType = c_EpgType_File;
    }
    
    if(!XBMC->GetSetting(c_epg_port_setting, &m_epgPort)){
        m_epgPort = 8085;
    }
    XBMC->GetSetting(c_server_version_setting, &m_serverVersion);
    if(m_serverVersion != c_PuzzleServer3){
        m_serverVersion = c_PuzzleServer2;
    }
    
    PVR_MENUHOOK hook = {UPDATE_CHANNEL_STREAMS_MENU_HOOK, 32052, PVR_MENUHOOK_CHANNEL};
    PVR->AddMenuHook(&hook);

    hook = {UPDATE_CHANNELS_MENU_HOOK, 32053, PVR_MENUHOOK_CHANNEL};
    PVR->AddMenuHook(&hook);

    retVal = CreateCoreSafe(false);
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;

}

PuzzlePVRClient::~PuzzlePVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    DestroyCoreSafe();

}
ADDON_STATUS PuzzlePVRClient::CreateCoreSafe(bool clearEpgCache)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    try
    {
        CreateCore(clearEpgCache);
    }
    catch (std::exception& ex)
    {
        char* message  = XBMC->GetLocalizedString(32005);
        XBMC->QueueNotification(QUEUE_ERROR,  message);
        XBMC->FreeString(message);
        
        LogError("PuzzlePVRClient:: Can't create Puzzle Server core. Exeption: [%s].", ex.what());
        retVal = ADDON_STATUS_LOST_CONNECTION;
    }
    catch(...)
    {
        XBMC->QueueNotification(QUEUE_ERROR, "Puzzle Server: unhandeled exception on reload EPG.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    return retVal;
}
void PuzzlePVRClient::DestroyCoreSafe()
{
    if(m_puzzleTV != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_puzzleTV);
    }
}

void PuzzlePVRClient::CreateCore(bool clearEpgCache)
{
    DestroyCoreSafe();
    
    m_clientCore = m_puzzleTV = new PuzzleTV((ServerVersion) m_serverVersion, m_serverUri.c_str(), m_serverPort);
    m_puzzleTV->SetMaxServerRetries(m_maxServerRetries);
    m_puzzleTV->SetEpgParams(EpgType(m_epgType), m_epgUrl, m_epgPort);
    m_puzzleTV->IncludeCurrentEpgToArchive(m_addCurrentEpgToArchive);
    m_puzzleTV->InitAsync(clearEpgCache);
}

ADDON_STATUS PuzzlePVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;

    if (strcmp(settingName, c_server_port_setting) == 0)
    {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if (strcmp(settingName, c_server_url_setting) == 0 )
    {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if (strcmp(settingName, c_server_retries_setting) == 0 )
    {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(strcmp(settingName,  c_epg_url_setting) == 0) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(strcmp(settingName,  c_epg_provider_setting) == 0) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(strcmp(settingName,  c_server_version_setting) == 0) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(strcmp(settingName,  c_epg_port_setting) == 0) {
        result = ADDON_STATUS_NEED_RESTART;
    }
   else {
        result = PVRClientBase::SetSetting(settingName, settingValue);
    }
    return result;
}

PVR_ERROR PuzzlePVRClient::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    pCapabilities->bSupportsRadio = true;
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

PVR_ERROR  PuzzlePVRClient::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    if(menuhook.iHookId <= PVRClientBase::s_lastCommonMenuHookId)
        return PVRClientBase::MenuHook(menuhook, item);
    if(m_puzzleTV == nullptr)
        return PVR_ERROR_SERVER_ERROR;
    if(UPDATE_CHANNEL_STREAMS_MENU_HOOK == menuhook.iHookId) {
//        const char* items[] = {"Item 1", "Item 2"};
//        int selected = GUI->Dialog_Select("TEST", &items[0], sizeof(items)/sizeof(items[0]));
        m_puzzleTV->UpdateChannelStreams(item.data.channel.iUniqueId);
    } else if (UPDATE_CHANNELS_MENU_HOOK == menuhook.iHookId) {
        CreateCoreSafe(false);
        PVR->TriggerChannelUpdate();
    }
    return PVR_ERROR_NO_ERROR;
}

ADDON_STATUS PuzzlePVRClient::OnReloadEpg()
{
    ADDON_STATUS retVal = CreateCoreSafe(true);
    
//    if(ADDON_STATUS_OK == retVal && nullptr != m_puzzleTV){
//        std::time_t startTime = std::time(nullptr);
//        startTime = std::mktime(std::gmtime(&startTime));
//        // Request EPG for all channels from -7 to +1 days
//        time_t endTime = startTime + 1 * 24 * 60 * 60;
//        startTime -= 7 * 24 * 60 * 60;
//        
//        m_puzzleTV->UpdateEpgForAllChannels(startTime, endTime);
//    }
    
    return retVal;
}


string PuzzlePVRClient::GetStreamUrl(ChannelId channelId)
{
    m_currentChannelStreamIdx = 0;
    return PVRClientBase::GetStreamUrl(channelId);
}

string PuzzlePVRClient::GetNextStreamUrl(ChannelId channelId)
{
    if(m_puzzleTV == nullptr)
        return string();
    LogError("PuzzlePVRClient:: trying to move to next stream from [%d].", m_currentChannelStreamIdx);
   return m_puzzleTV->GetNextStream(channelId, m_currentChannelStreamIdx++);
}

//int PuzzlePVRClient::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
//{
//    int readBytes = PVRClientBase::ReadLiveStream(pBuffer,iBufferSize);
//    bool tryToRecover = readBytes < 0;
//    while(tryToRecover) {
//        string url = GetNextStreamUrl(GetLiveChannelId());
//        if(url.empty()) // nomore streams
//            break;
//        SwitchChannel(GetLiveChannelId(), url);
//        readBytes = PVRClientBase::ReadLiveStream(pBuffer,iBufferSize);
//        tryToRecover = readBytes < 0;
//    }
//
//    return readBytes;
//}

bool PuzzlePVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
   return false;
}

PVR_ERROR PuzzlePVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Puzzle Server");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_puzzleTV == NULL) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}
       



