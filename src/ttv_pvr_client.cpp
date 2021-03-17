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
#include "ttv_pvr_client.h"
#include "helpers.h"
#include "ttv_player.h"
#include "plist_buffer.h"
#include "globals.hpp"

using namespace Globals;
using namespace std;
using namespace TtvEngine;
using namespace PvrClient;
using namespace Helpers;

static const char* c_epg_setting = "ttv_epg_url";
static const char* c_ttv_ace_server_uri = "ttv_ace_server_uri";
static const char* c_ttv_ace_server_port = "ttv_ace_server_port";
static const char* c_ttv_adult = "ttv_adult";
static const char* c_ttv_filter_by_alexelec = "ttv_filter_by_alexelec";

ADDON_STATUS TtvPVRClient::Init(const std::string& clientPath, const std::string& userPath)
{
    ADDON_STATUS retVal = PVRClientBase::Init(clientPath, userPath);
    if(ADDON_STATUS_OK != retVal)
        return retVal;

    m_epgUrl =  kodi::GetSettingString(c_epg_setting);
    m_enableAdultContent = kodi::GetSettingBoolean(c_ttv_adult, false);
    m_aceServerUri = kodi::GetSettingString(c_ttv_ace_server_uri, "127.0.0.1");
    m_aceServerPort = kodi::GetSettingInt(c_ttv_ace_server_port, 6878);
    m_filterByAlexElec = kodi::GetSettingBoolean(c_ttv_filter_by_alexelec, true);
    
    m_currentChannelStreamIdx = -1;


    retVal = CreateCoreSafe(false);
    
    return retVal;
    
}

void TtvPVRClient::PopulateSettings(AddonSettingsMutableDictionary& settings)
{
    
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
        OnCoreCreated();
    }
    catch (exception & ex)
    {
        LogError("Torrent TV: exception on core creation: %s", ex.what());
        kodi::QueueFormattedNotification(QUEUE_ERROR, "Torrent TV: exception on core creation.");
    }
    catch(...)
    {
        kodi::QueueFormattedNotification(QUEUE_ERROR, "Torrent TV: unhandeled exception on core creation.");
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
    
    TtvEngine::Core::CoreParams cp;
    cp.aceServerUri = m_aceServerUri;
    cp.aceServerPort = m_aceServerPort;
    cp.enableAdult = m_enableAdultContent;
    cp.filterByAlexElec = m_filterByAlexElec;
    cp.epgUrl = m_epgUrl;
    m_clientCore = m_core = new TtvEngine::Core(cp);
    m_core->IncludeCurrentEpgToArchive(HowToAddCurrentEpgToArchive());
    m_core->SetEpgCorrectionShift(EpgCorrectionShift());
    m_core->SetLocalLogosFolder(LocalLogosFolder());
    m_core->InitAsync(clearEpgCache, IsArchiveSupported());
}

ADDON_STATUS TtvPVRClient::SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;
    
    if(c_ttv_ace_server_uri == settingName && settingValue.GetString() != m_aceServerUri) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(c_epg_setting == settingName && settingValue.GetString() != m_epgUrl) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if (c_ttv_adult == settingName) {
        result = ADDON_STATUS_NEED_RESTART;
        kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32016).c_str());
//      Doesn't work because of modal dialog "NEED RESSTART" ...
//        m_clientCore->CallRpcAsync("{\"jsonrpc\": \"2.0\", \"method\": \"GUI.ActivateWindow\", \"params\": {\"window\": \"pvrsettings\"},\"id\": 1}",
//                                   [&] (rapidjson::Document& jsonRoot) {
//                                       kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32016).c_str());
//                                   },
//                                   [&](const ActionQueue::ActionResult& s) {});
    }
    else if (c_ttv_ace_server_port == settingName) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if (c_ttv_filter_by_alexelec == settingName) {
        result = ADDON_STATUS_NEED_RESTART;
        kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32016).c_str());
//      Doesn't work because of modal dialog "NEED RESSTART" ...
//        m_clientCore->CallRpcAsync("{\"jsonrpc\": \"2.0\", \"method\": \"GUI.ActivateWindow\", \"params\": {\"window\": \"pvrsettings\"},\"id\": 1}",
//                                   [&] (rapidjson::Document& jsonRoot) {
//                                       kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32016).c_str());
//                                   },
//                                   [&](const ActionQueue::ActionResult& s) {});
    }
    else {
        result = PVRClientBase::SetSetting(settingName, settingValue);
    }
    return result;
}

PVR_ERROR TtvPVRClient::GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities)
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

bool TtvPVRClient::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
    return false;
}


PVR_ERROR TtvPVRClient::SignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus)
{
    signalStatus.SetAdapterName("IPTV Torrent TV");
    signalStatus.SetAdapterStatus((m_core == NULL) ? "Not connected" :"OK");
    return this->PVRClientBase::SignalStatus(channelUid, signalStatus);
}




