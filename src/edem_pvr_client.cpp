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
#include "p8-platform/util/util.h"
#include "kodi/xbmc_addon_cpp_dll.h"

#include "libXBMC_pvr.h"
#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "edem_pvr_client.h"
#include "helpers.h"
#include "edem_player.h"

using namespace std;
using namespace ADDON;
using namespace EdemEngine;

static const char* c_playlist_setting = "edem_playlist_url";
static const char* c_epg_setting = "edem_epg_url";

ADDON_STATUS EdemPVRClient::Init(CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                               PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    if(ADDON_STATUS_OK != (retVal = PVRClientBase::Init(addonHelper, pvrHelper, pvrprops)))
       return retVal;
    
    char buffer[1024];
    
    if (m_addonHelper->GetSetting(c_playlist_setting, &buffer))
        m_playlistUrl = buffer;
    if (m_addonHelper->GetSetting(c_epg_setting, &buffer))
        m_epgUrl = buffer;
    
    try
    {
        CreateCore();
    }
    catch (AuthFailedException &)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32003));
    }
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;

}

EdemPVRClient::~EdemPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    if(m_core != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_core);
    }

}

void EdemPVRClient::CreateCore()
{
    if(m_core != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_core);
    }
    m_clientCore = m_core = new EdemEngine::Core(m_addonHelper, m_pvrHelper, m_playlistUrl, m_epgUrl);
}

ADDON_STATUS EdemPVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;

    if (strcmp(settingName,  c_playlist_setting) == 0 && strcmp((const char*) settingValue, m_playlistUrl.c_str()) != 0) {
        m_playlistUrl= (const char*) settingValue;
        if (m_playlistUrl.empty() || m_playlistUrl.find("***") != string::npos) {
            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32010));
        } else {
            try {
                CreateCore();
                result = ADDON_STATUS_NEED_RESTART;
            }catch (AuthFailedException &) {
                m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32011));
            }
        }
    }
    else if(strcmp(settingName,  c_epg_setting) == 0 && strcmp((const char*) settingValue, m_epgUrl.c_str()) != 0) {
        m_epgUrl = (const char*) settingValue;
        try {
            CreateCore();
            result = ADDON_STATUS_NEED_RESTART;
        }catch (AuthFailedException &) {
            m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32011));
        }
    }
    else {
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
    pCapabilities->bSupportsRecordings = false;
    
    pCapabilities->bSupportsTimers = false;
    pCapabilities->bSupportsChannelScan = false;
    pCapabilities->bHandlesDemuxing = false;
    pCapabilities->bSupportsRecordingPlayCount = false;
    pCapabilities->bSupportsLastPlayedPosition = false;
    pCapabilities->bSupportsRecordingEdl = false;
    
    return PVRClientBase::GetAddonCapabilities(pCapabilities);
}


PVR_ERROR EdemPVRClient::GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd)
{
    if(NULL == m_core)
        return PVR_ERROR_SERVER_ERROR;
    
    EpgEntryList epgEntries;
    m_core->GetEpg(channel.iUniqueId, iStart, iEnd, epgEntries);
    EpgEntryList::const_iterator itEpgEntry = epgEntries.begin();
    for (int i = 0; itEpgEntry != epgEntries.end(); ++itEpgEntry, ++i)
    {
        EPG_TAG tag = { 0 };
        tag.iUniqueBroadcastId = itEpgEntry->first;
        tag.iChannelNumber = channel.iUniqueId;
        tag.strTitle = itEpgEntry->second.Title.c_str();
        tag.strPlot = itEpgEntry->second.Description.c_str();
        tag.startTime = itEpgEntry->second.StartTime;
        tag.endTime = itEpgEntry->second.EndTime;
        m_pvrHelper->TransferEpgEntry(handle, &tag);
    }
    return PVR_ERROR_NO_ERROR;
}
PVR_ERROR  EdemPVRClient::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
//    SovokEpgEntry epgEntry;
//    m_core->FindEpg(item.data.iEpgUid, epgEntry);
    return PVR_ERROR_NOT_IMPLEMENTED;
    
}

bool EdemPVRClient::OpenLiveStream(const PVR_CHANNEL& channel)
{
    if(NULL == m_core)
        return false;

    string url = m_core->GetUrl(channel.iUniqueId);
    return PVRClientBase::OpenLiveStream(url);
}

bool EdemPVRClient::SwitchChannel(const PVR_CHANNEL& channel)
{
    if(NULL == m_core)
        return false;

    string url = m_core->GetUrl(channel.iUniqueId);
    return PVRClientBase::SwitchChannel(url);
}



int EdemPVRClient::GetRecordingsAmount(bool deleted)
{
    return -1;
    
    if(NULL == m_core)
        return -1;

    if(deleted)
        return -1;
    
    int size = 0;
    std::function<void(const ArchiveList&)> f = [&size](const ArchiveList& list){size = list.size();};
    m_core->Apply(f);
    if(size == 0)
    {
        std::function<void(void)> action = [=](){
            m_pvrHelper->TriggerRecordingUpdate();
        };
        m_core->StartArchivePollingWithCompletion(action);
//        m_core->Apply(f);
//        if(size != 0)
//            action();
    
    }
    return size;
    
    
    
}
PVR_ERROR EdemPVRClient::GetRecordings(ADDON_HANDLE handle, bool deleted)
{
    return PVR_ERROR_NOT_IMPLEMENTED;

    if(deleted)
        return PVR_ERROR_NOT_IMPLEMENTED;
    
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    Core& sTV(*m_core);
    CHelper_libXBMC_pvr * pvrHelper = m_pvrHelper;
    ADDON::CHelper_libXBMC_addon * addonHelper = m_addonHelper;
    std::function<void(const ArchiveList&)> f = [&sTV, &handle, pvrHelper, addonHelper ,&result](const ArchiveList& list)
    {
        for(const auto &  i :  list) {
            try {
                const EpgEntry& epgTag = sTV.GetEpgList().at(i);

                PVR_RECORDING tag = { 0 };
    //            memset(&tag, 0, sizeof(PVR_RECORDING));
                sprintf(tag.strRecordingId, "%d",  i);
                strncpy(tag.strTitle, epgTag.Title.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
                strncpy(tag.strPlot, epgTag.Description.c_str(), PVR_ADDON_DESC_STRING_LENGTH - 1);
                strncpy(tag.strChannelName, sTV.GetChannelList().at(epgTag.ChannelId).Name.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
                tag.recordingTime = epgTag.StartTime;
                tag.iLifetime = 0; /* not implemented */

                tag.iDuration = epgTag.EndTime - epgTag.StartTime;
                tag.iEpgEventId = i;
                tag.iChannelUid = epgTag.ChannelId;

                string dirName = tag.strChannelName;
                char buff[20];
                strftime(buff, sizeof(buff), "/%d-%m-%y", localtime(&epgTag.StartTime));
                dirName += buff;
                strncpy(tag.strDirectory, dirName.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);

                pvrHelper->TransferRecordingEntry(handle, &tag);
                
            }
            catch (...)  {
                addonHelper->Log(LOG_ERROR, "%s: failed.", __FUNCTION__);
                result = PVR_ERROR_FAILED;
            }

        }
    };
    m_core->Apply(f);
    return result;
}

bool EdemPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    return false;
    
    if(NULL == m_core)
        return PVR_ERROR_SERVER_ERROR;

    EpgEntry epgTag;
    
    unsigned int epgId = recording.iEpgEventId;
    if( epgId == 0 )
        epgId = stoul(recording.strRecordingId);
    if(!m_core->FindEpg(epgId, epgTag))
        return false;
    
    string url = m_core->GetArchiveForEpg(epgTag);
    return PVRClientBase::OpenRecordedStream(url);
}

PVR_ERROR EdemPVRClient::CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    m_addonHelper->Log(LOG_DEBUG, " >>>> !!!! Menu hook !!!! <<<<<");
    return MenuHook(menuhook, item);
}

PVR_ERROR EdemPVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Sovok TV Adapter 1");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_core == NULL) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}




