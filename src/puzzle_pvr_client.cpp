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

#include "libXBMC_pvr.h"
#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "puzzle_pvr_client.h"
#include "helpers.h"
#include "puzzle_tv.h"

using namespace std;
using namespace ADDON;
using namespace PuzzleEngine;
using namespace PvrClient;

ADDON_STATUS PuzzlePVRClient::Init(CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                               PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    if(ADDON_STATUS_OK != (retVal = PVRClientBase::Init(addonHelper, pvrHelper, pvrprops)))
       return retVal;
    
    char buffer[1024];
    
    m_currentChannelStreamIdx = -1;
    int serverPort = 8089;
    m_addonHelper->GetSetting("puzzle_server_port", &serverPort);
    
    m_addonHelper->GetSetting("puzzle_server_uri", &buffer);
    
   
    
    try
    {
        CreateCore(false);
        m_puzzleTV->SetServerPort(serverPort);
        m_puzzleTV->SetServerUri(buffer);
    }
    catch (std::exception& ex)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR,  m_addonHelper->GetLocalizedString(32005));
        m_addonHelper->Log(LOG_ERROR, "PuzzlePVRClient:: Can't create Puzzle Server core. Exeption: [%s].", ex.what());
        retVal = ADDON_STATUS_LOST_CONNECTION;
    }
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;

}

PuzzlePVRClient::~PuzzlePVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    if(m_puzzleTV != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_puzzleTV);
    }

}

void PuzzlePVRClient::CreateCore(bool clearEpgCache)
{
    if(m_puzzleTV != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_puzzleTV);
    }
    m_clientCore = m_puzzleTV = new PuzzleTV(m_addonHelper, m_pvrHelper, clearEpgCache);
}

ADDON_STATUS PuzzlePVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    if (strcmp(settingName, "puzzle_server_port") == 0)
    {
        if(m_puzzleTV)
            m_puzzleTV->SetServerPort(*(int *)(settingValue));
    }
    else if (strcmp(settingName, "puzzle_server_uri") == 0 )//&& strcmp((const char*) settingValue, m_password.c_str()) != 0)
    {
        if(m_puzzleTV)
            m_puzzleTV->SetServerUri((const char *)(settingValue));
    }
    else {
        return PVRClientBase::SetSetting(settingName, settingValue);
    }
    return ADDON_STATUS_NEED_RESTART;
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
    return PVRClientBase::MenuHook(menuhook, item);
    
}

ADDON_STATUS PuzzlePVRClient::OnReloadEpg()
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    try
    {
        auto port =m_puzzleTV->GetServerPort();
        string uri = m_puzzleTV->GetServerUri();

        CreateCore(true);
        m_puzzleTV->SetServerPort(port);
        m_puzzleTV->SetServerUri(uri.c_str());
    }
    catch (std::exception& ex)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR,  m_addonHelper->GetLocalizedString(32005));
        m_addonHelper->Log(LOG_ERROR, "PuzzlePVRClient:: Can't create Puzzle Server core. Exeption: [%s].", ex.what());
        retVal = ADDON_STATUS_LOST_CONNECTION;
    }
    catch(...)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Puzzle Server: unhandeled exception on reload EPG.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    
    if(ADDON_STATUS_OK == retVal && nullptr != m_puzzleTV){
        std::time_t startTime = std::time(nullptr);
        startTime = std::mktime(std::gmtime(&startTime));
        // Request EPG for all channels from -7 to +1 days
        time_t endTime = startTime + 1 * 24 * 60 * 60;
        startTime -= 7 * 24 * 60 * 60;
        
        m_puzzleTV->UpdateEpgForAllChannels(startTime, endTime);
    }
    
    return retVal;
}


string PuzzlePVRClient::GetStreamUrl(const PVR_CHANNEL& channel)
{
    m_currentChannelId = channel.iUniqueId;
    string url = m_puzzleTV->GetUrl(m_currentChannelId);
    m_currentChannelStreamIdx = 0;
    return url;
}
bool PuzzlePVRClient::OpenLiveStream(const PVR_CHANNEL& channel)
{
    bool succeeded = PVRClientBase::OpenLiveStream(GetStreamUrl(channel));
    bool tryToRecover = !succeeded;
    while(tryToRecover) {
        m_addonHelper->Log(LOG_ERROR, "PuzzlePVRClient:: trying to move to next stream from [%d].", m_currentChannelStreamIdx);
        string url = m_puzzleTV->GetNextStream(m_currentChannelId,m_currentChannelStreamIdx);
        if(url.empty()) // nomore streams
            break;
        ++m_currentChannelStreamIdx;
        succeeded = PVRClientBase::OpenLiveStream(url);
        tryToRecover = !succeeded;
    }

    return succeeded;
}

int PuzzlePVRClient::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    int readBytes = PVRClientBase::ReadLiveStream(pBuffer,iBufferSize);
    bool tryToRecover = readBytes < 0;
    while(tryToRecover) {
        m_addonHelper->Log(LOG_ERROR, "PuzzlePVRClient:: trying to move to next stream from [%d].", m_currentChannelStreamIdx);
        string url = m_puzzleTV->GetNextStream(m_currentChannelId,m_currentChannelStreamIdx);
        if(url.empty()) // nomore streams
            break;
        ++m_currentChannelStreamIdx;
        PVRClientBase::SwitchChannel(url);
        readBytes = PVRClientBase::ReadLiveStream(pBuffer,iBufferSize);
        tryToRecover = readBytes < 0;
    }

    return readBytes;
}

bool PuzzlePVRClient::SwitchChannel(const PVR_CHANNEL& channel)
{
    return PVRClientBase::SwitchChannel(GetStreamUrl(channel));
}

//void PuzzlePVRClient::SetAddFavoritesGroup(bool shouldAddFavoritesGroup)
//{
//    if (shouldAddFavoritesGroup != m_shouldAddFavoritesGroup)
//    {
//        m_shouldAddFavoritesGroup = shouldAddFavoritesGroup;
//        m_pvrHelper->TriggerChannelGroupsUpdate();
//    }
//}


int PuzzlePVRClient::GetRecordingsAmount(bool deleted)
{
    return -1;
//    if(deleted)
//        return -1;
//    
//    int size = 0;
//    std::function<void(const ArchiveList&)> f = [&size](const ArchiveList& list){size = list.size();};
//    m_sovokTV->Apply(f);
//    if(size == 0)
//    {
//        std::function<void(void)> action = [=](){
//            m_pvrHelper->TriggerRecordingUpdate();
//        };
//        m_sovokTV->StartArchivePollingWithCompletion(action);
////            m_sovokTV->Apply(f);
////            if(size != 0)
////                action();
    
//    }
//    return size;
    
}
PVR_ERROR PuzzlePVRClient::GetRecordings(ADDON_HANDLE handle, bool deleted)
{
    return PVR_ERROR_NOT_IMPLEMENTED;
//    if(deleted)
//        return PVR_ERROR_NOT_IMPLEMENTED;
//    
//    PVR_ERROR result = PVR_ERROR_NO_ERROR;
//    SovokTV& sTV(*m_sovokTV);
//    CHelper_libXBMC_pvr * pvrHelper = m_pvrHelper;
//    ADDON::CHelper_libXBMC_addon * addonHelper = m_addonHelper;
//    std::function<void(const ArchiveList&)> f = [&sTV, &handle, pvrHelper, addonHelper ,&result](const ArchiveList& list){
//        for(const auto &  i :  list) {
//            try {
//                const SovokEpgEntry& epgTag = sTV.GetEpgList().at(i);
//
//                PVR_RECORDING tag = { 0 };
//    //            memset(&tag, 0, sizeof(PVR_RECORDING));
//                sprintf(tag.strRecordingId, "%d",  i);
//                strncpy(tag.strTitle, epgTag.Title.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
//                strncpy(tag.strPlot, epgTag.Description.c_str(), PVR_ADDON_DESC_STRING_LENGTH - 1);
//                strncpy(tag.strChannelName, sTV.GetChannelList().at(epgTag.ChannelId).Name.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
//                tag.recordingTime = epgTag.StartTime;
//                tag.iLifetime = 0; /* not implemented */
//
//                tag.iDuration = epgTag.EndTime - epgTag.StartTime;
//                tag.iEpgEventId = i;
//                tag.iChannelUid = epgTag.ChannelId;
//
//                string dirName = tag.strChannelName;
//                char buff[20];
//                strftime(buff, sizeof(buff), "/%d-%m-%y", localtime(&epgTag.StartTime));
//                dirName += buff;
//                strncpy(tag.strDirectory, dirName.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
//
//                pvrHelper->TransferRecordingEntry(handle, &tag);
//                
//            }
//            catch (...)  {
//                addonHelper->Log(LOG_ERROR, "%s: failed.", __FUNCTION__);
//                result = PVR_ERROR_FAILED;
//            }
//
//        }
//    };
//    m_sovokTV->Apply(f);
//    return result;
}

bool PuzzlePVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    return false;
}

PVR_ERROR PuzzlePVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Puzzle Server");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_puzzleTV == NULL) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}
       



