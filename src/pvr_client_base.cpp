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
#include "p8-platform/util/StringUtils.h"

#include "kodi/xbmc_addon_cpp_dll.h"
#include "kodi/kodi_vfs_utils.hpp"

#include "timeshift_buffer.h"
#include "file_cache_buffer.hpp"
#include "memory_cache_buffer.hpp"
#include "plist_buffer.h"
#include "direct_buffer.h"
#include "helpers.h"
#include "pvr_client_base.h"
#include "globals.hpp"

using namespace std;
using namespace ADDON;
using namespace Buffers;
using namespace PvrClient;
using namespace Globals;

namespace CurlUtils
{
    extern void SetCurlTimeout(long timeout);
}
// NOTE: avoid '.' (dot) char in path. Causes to deadlock in Kodi code.
const char* s_DefaultCacheDir = "special://temp/pvr-puzzle-tv";

const int RELOAD_EPG_MENU_HOOK = 1;
const int RELOAD_RECORDINGS_MENU_HOOK = 2;

ADDON_STATUS PVRClientBase::Init(PVR_PROPERTIES* pvrprops)
{
    m_clientCore = NULL;
    m_inputBuffer = NULL;
    SetTimeshiftPath(s_DefaultCacheDir);
    
    LogDebug( "User path: %s", pvrprops->strUserPath);
    LogDebug( "Client path: %s", pvrprops->strClientPath);
    //auto g_strUserPath   = pvrprops->strUserPath;
    m_clientPath = pvrprops->strClientPath;
    m_userPath = pvrprops->strUserPath;
    
    char buffer[1024];
    
    int curlTimout = 15;
    XBMC->GetSetting("curl_timeout", &curlTimout);
    int channelTimeout = 5;
     XBMC->GetSetting("channel_reload_timeout", &channelTimeout);
   
    
    bool isTimeshiftEnabled;
    XBMC->GetSetting("enable_timeshift", &isTimeshiftEnabled);
    string timeshiftPath;
    if (XBMC->GetSetting("timeshift_path", &buffer))
        timeshiftPath = buffer;
    uint64_t timeshiftBufferSize = 0;
    XBMC->GetSetting("timeshift_size", &timeshiftBufferSize);
    timeshiftBufferSize *= 1024*1024;
    
    TimeshiftBufferType timeshiftBufferType = k_TimeshiftBufferMemory;
    XBMC->GetSetting("timeshift_type", &timeshiftBufferType);
    
    CurlUtils::SetCurlTimeout(curlTimout);
    SetChannelReloadTimeout(channelTimeout);
    SetTimeshiftEnabled(isTimeshiftEnabled);
    SetTimeshiftPath(timeshiftPath);
    SetTimeshiftBufferSize(timeshiftBufferSize);
    SetTimeshiftBufferType(timeshiftBufferType);
    
    PVR_MENUHOOK hook = {RELOAD_EPG_MENU_HOOK, 32050, PVR_MENUHOOK_EPG};
    PVR->AddMenuHook(&hook);

    hook = {RELOAD_RECORDINGS_MENU_HOOK, 32051, PVR_MENUHOOK_RECORDING};
    PVR->AddMenuHook(&hook);

    return ADDON_STATUS_OK;
    
}

PVRClientBase::~PVRClientBase()
{
    CloseLiveStream();
    CloseRecordedStream();
}

static ADDON_StructSetting ** g_sovokSettings = NULL;
static int g_sovokSettingsSize = 0;

int PVRClientBase::GetSettings(ADDON_StructSetting ***sSet)
{
    return 0;
}

ADDON_STATUS PVRClientBase::SetSetting(const char *settingName, const void *settingValue)
{
    if (strcmp(settingName, "enable_timeshift") == 0)
    {
          SetTimeshiftEnabled(*(bool *)(settingValue));
    }
    else if (strcmp(settingName, "channel_reload_timeout") == 0)
    {
        SetChannelReloadTimeout(*(int *)(settingValue));
    }
    else if (strcmp(settingName, "timeshift_path") == 0)
    {
          SetTimeshiftPath((const char *)(settingValue));
    }
    else if (strcmp(settingName, "timeshift_type") == 0)
    {
        SetTimeshiftBufferType(*(TimeshiftBufferType*)(settingValue));
    }
    else if (strcmp(settingName, "timeshift_size") == 0)
    {
        auto size = *(int32_t *)(settingValue);
        size *= 1024*1024;
        SetTimeshiftBufferSize(size);
    }
    else if (strcmp(settingName, "curl_timeout") == 0)
    {
        CurlUtils::SetCurlTimeout(*(int *)(settingValue));
    }

    return ADDON_STATUS_OK;
}

void PVRClientBase::FreeSettings()
{
    if(g_sovokSettings && g_sovokSettingsSize)
        DllUtils::FreeStruct(g_sovokSettingsSize, &g_sovokSettings);
    g_sovokSettingsSize = 0;
    g_sovokSettings = NULL;
}

PVR_ERROR PVRClientBase::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    //pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    //pCapabilities->bSupportsRadio = true;
    //pCapabilities->bSupportsChannelGroups = true;
    //pCapabilities->bHandlesInputStream = true;
    //pCapabilities->bSupportsRecordings = true;
    
//    pCapabilities->bSupportsTimers = false;
//    pCapabilities->bSupportsChannelScan = false;
//    pCapabilities->bHandlesDemuxing = false;
//    pCapabilities->bSupportsRecordingPlayCount = false;
//    pCapabilities->bSupportsLastPlayedPosition = false;
//    pCapabilities->bSupportsRecordingEdl = false;
    
    return PVR_ERROR_NO_ERROR;
}

bool PVRClientBase::CanPauseStream()
{
    return IsTimeshiftEnabled();
}

bool PVRClientBase::CanSeekStream()
{
    return IsTimeshiftEnabled();
}

ADDON_STATUS PVRClientBase::GetStatus()
{
    return  /*(m_sovokTV == NULL)? ADDON_STATUS_LOST_CONNECTION : */ADDON_STATUS_OK;
}

void PVRClientBase::SetTimeshiftPath(const std::string& path){
    const char* nonEmptyPath = (path.empty()) ? s_DefaultCacheDir : path.c_str();
    if(!XBMC->DirectoryExists(nonEmptyPath))
        if(!XBMC->CreateDirectory(nonEmptyPath))
            LogError( "Failed to create cache folder");
    // Cleanup chache
    if(XBMC->DirectoryExists(nonEmptyPath))
    {
        std::vector<CVFSDirEntry> files;
        VFSUtils::GetDirectory(XBMC, nonEmptyPath, "*.bin", files);
        for (auto& f : files) {
            if(!f.IsFolder())
                if(!XBMC->DeleteFile(f.Path().c_str()))
                    LogError( "Failed to delete timeshift folder entry %s", f.Path().c_str());
        }
    }

    m_CacheDir = nonEmptyPath;
}

PVR_ERROR  PVRClientBase::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    
    if(menuhook.iHookId == RELOAD_EPG_MENU_HOOK ) {
        char* message = XBMC->GetLocalizedString(32012);
        XBMC->QueueNotification(QUEUE_INFO, message);
        XBMC->FreeString(message);
        OnReloadEpg();
    } else if(RELOAD_RECORDINGS_MENU_HOOK == menuhook.iHookId) {
//        char* message = XBMC->GetLocalizedString(32012);
//        XBMC->QueueNotification(QUEUE_INFO, message);
//        XBMC->FreeString(message);
        OnReloadRecordings();
    }
    return PVR_ERROR_NO_ERROR;
    
}

ADDON_STATUS PVRClientBase::OnReloadEpg()
{
    return ADDON_STATUS_OK;
}

ADDON_STATUS PVRClientBase::OnReloadRecordings()
{
    if(NULL == m_clientCore)
        return ADDON_STATUS_LOST_CONNECTION;
    
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    m_clientCore->ReloadRecordings();
    return retVal;
}

#pragma mark - Channels

PVR_ERROR PVRClientBase::GetChannels(ADDON_HANDLE handle, bool bRadio)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    for(auto& itChannel : m_clientCore->GetChannelList())
    {
        auto & channel = itChannel.second;
        if (bRadio == channel.IsRadio)
        {
            PVR_CHANNEL pvrChannel = { 0 };
            pvrChannel.iUniqueId = channel.Id;
            pvrChannel.iChannelNumber = channel.Number;
            pvrChannel.bIsRadio = channel.IsRadio;
            strncpy(pvrChannel.strChannelName, channel.Name.c_str(), sizeof(pvrChannel.strChannelName));
            strncpy(pvrChannel.strIconPath, channel.IconPath.c_str(), sizeof(pvrChannel.strIconPath));
            
            PVR->TransferChannelEntry(handle, &pvrChannel);
        }
    }
    
    return PVR_ERROR_NO_ERROR;
}

int PVRClientBase::GetChannelsAmount()
{
    if(NULL == m_clientCore)
        return -1;
    
    return m_clientCore->GetChannelList().size();
}
#pragma mark - Groups

int PVRClientBase::GetChannelGroupsAmount()
{
    if(NULL == m_clientCore)
        return -1;
    
    size_t numberOfGroups = m_clientCore->GetGroupList().size();
    return numberOfGroups;
}

PVR_ERROR PVRClientBase::GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    if (!bRadio)
    {
        PVR_CHANNEL_GROUP pvrGroup = { 0 };
        pvrGroup.bIsRadio = false;
        for (auto& itGroup : m_clientCore->GetGroupList())
        {
            strncpy(pvrGroup.strGroupName, itGroup.second.Name.c_str(), sizeof(pvrGroup.strGroupName));
            PVR->TransferChannelGroup(handle, &pvrGroup);
        }
    }
    
    return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRClientBase::GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP& group)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    auto& groups = m_clientCore->GetGroupList();
    auto itGroup =  std::find_if(groups.begin(), groups.end(), [&](const GroupList::value_type& v ){
        return strcmp(group.strGroupName, v.second.Name.c_str()) == 0;
    });
    if (itGroup != groups.end())
    {
        for (auto it : itGroup->second.Channels)
        {
            PVR_CHANNEL_GROUP_MEMBER pvrGroupMember = { 0 };
            strncpy(pvrGroupMember.strGroupName, itGroup->second.Name.c_str(), sizeof(pvrGroupMember.strGroupName));
            pvrGroupMember.iChannelUniqueId = it;
            PVR->TransferChannelGroupMember(handle, &pvrGroupMember);
        }
    }
   
    return PVR_ERROR_NO_ERROR;
}

#pragma mark - EPG

PVR_ERROR PVRClientBase::GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    m_clientCore->GetPhase(IClientCore::k_InitPhase)->Wait();
   
    EpgEntryList epgEntries;
    m_clientCore->GetEpg(channel.iUniqueId, iStart, iEnd, epgEntries);
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
        PVR->TransferEpgEntry(handle, &tag);
    }
    return PVR_ERROR_NO_ERROR;
}

#pragma mark - Streams

bool PVRClientBase::OpenLiveStream(const std::string& url )
{
    if (url.empty())
        return false;
    try
    {
        InputBuffer* buffer = NULL;
        
        const std::string m3uExt = ".m3u";
        const std::string m3u8Ext = ".m3u8";
        if( url.find(m3u8Ext) != std::string::npos || url.find(m3uExt) != std::string::npos)
            buffer = new Buffers::PlaylistBuffer(url, NULL, 0); // No segments cache for live playlist
        else
            buffer = new DirectBuffer(url);
        
        if (m_isTimeshiftEnabled){
            if(k_TimeshiftBufferFile == m_timeshiftBufferType) {
                m_inputBuffer = new Buffers::TimeshiftBuffer(buffer, new Buffers::FileCacheBuffer(m_CacheDir, m_timshiftBufferSize /  Buffers::FileCacheBuffer::CHUNK_FILE_SIZE_LIMIT));
            } else {
                m_inputBuffer = new Buffers::TimeshiftBuffer(buffer, new Buffers::MemoryCacheBuffer(m_timshiftBufferSize /  Buffers::MemoryCacheBuffer::CHUNK_SIZE_LIMIT));
            }
        }
        else
            m_inputBuffer = buffer;

    }
    catch (InputBufferException &ex)
    {
        LogError(  "PVRClientBase: input buffer error in OpenLiveStream: %s", ex.what());
        return false;
    }
    
    return true;
}

void PVRClientBase::CloseLiveStream()
{
    if(m_inputBuffer) {
        LogNotice("PVRClientBase: closing input sream...");
        SAFE_DELETE(m_inputBuffer);
        LogNotice("PVRClientBase: input sream closed.");
    }
}

int PVRClientBase::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    return m_inputBuffer->Read(pBuffer, iBufferSize, m_channelReloadTimeout * 1000);
}

long long PVRClientBase::SeekLiveStream(long long iPosition, int iWhence)
{
    return m_inputBuffer->Seek(iPosition, iWhence);
}

long long PVRClientBase::PositionLiveStream()
{
    return m_inputBuffer->GetPosition();
}

long long PVRClientBase::LengthLiveStream()
{
    return m_inputBuffer->GetLength();
}

bool PVRClientBase::SwitchChannel(const std::string& url)
{
    return m_inputBuffer->SwitchStream(url);
}

void PVRClientBase::SetTimeshiftEnabled(bool enable)
{
    m_isTimeshiftEnabled = enable;
}

void PVRClientBase::SetChannelReloadTimeout(int timeout)
{
    m_channelReloadTimeout = timeout;
}

void PVRClientBase::SetTimeshiftBufferSize(uint64_t size)
{
    m_timshiftBufferSize = size;
}

void PVRClientBase::SetTimeshiftBufferType(PVRClientBase::TimeshiftBufferType type)
{
    m_timeshiftBufferType = type;
}

#pragma mark - Recordings

int PVRClientBase::GetRecordingsAmount(bool deleted)
{
    if(NULL == m_clientCore)
        return -1;
    
    if(deleted)
        return -1;
    
    if(!m_clientCore->GetPhase(IClientCore::k_EpgLoadingPhase)->IsDone())
        return 0;
    
    int size = 0;
    IClientCore::EpgEntryAction action = [&size](const EpgEntryList::value_type& p)
    {
        if(p.second.HasArchive)
            ++size;
    };
    m_clientCore->ForEachEpg(action);
//    if(size == 0){
//        m_clientCore->ReloadRecordings();
//        m_clientCore->ForEachEpg(action);
//    }
    
    LogDebug("PVRClientBase: found %d recordings.", size);
    return size;
    
}
PVR_ERROR PVRClientBase::GetRecordings(ADDON_HANDLE handle, bool deleted)
{

    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;

    if(deleted)
        return PVR_ERROR_NOT_IMPLEMENTED;
    
    if(!m_clientCore->GetPhase(IClientCore::k_EpgLoadingPhase)->IsDone())
        return PVR_ERROR_NO_ERROR;

    
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    auto pThis = this;
    
    auto Pvr = PVR;
    IClientCore::EpgEntryAction action = [&handle, pThis ,&result, Pvr](const EpgEntryList::value_type& epgEntry)
    {
        try {
            const auto& epgTag = epgEntry.second;
            
            if(!epgTag.HasArchive)
                return;
            
            PVR_RECORDING tag = { 0 };
            //            memset(&tag, 0, sizeof(PVR_RECORDING));
            sprintf(tag.strRecordingId, "%d",  epgEntry.first);
            strncpy(tag.strTitle, epgTag.Title.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
            strncpy(tag.strPlot, epgTag.Description.c_str(), PVR_ADDON_DESC_STRING_LENGTH - 1);
            strncpy(tag.strChannelName, pThis->m_clientCore->GetChannelList().at(epgTag.ChannelId).Name.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
            tag.recordingTime = epgTag.StartTime;
            tag.iLifetime = 0; /* not implemented */
            
            tag.iDuration = epgTag.EndTime - epgTag.StartTime;
            tag.iEpgEventId = epgEntry.first;
            tag.iChannelUid = epgTag.ChannelId;
            tag.channelType = PVR_RECORDING_CHANNEL_TYPE_TV;
            
            string dirName = tag.strChannelName;
            char buff[20];
            strftime(buff, sizeof(buff), "/%d-%m-%y", localtime(&epgTag.StartTime));
            dirName += buff;
            strncpy(tag.strDirectory, dirName.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
            
            Pvr->TransferRecordingEntry(handle, &tag);
            
        }
        catch (...)  {
            LogError( "%s: failed.", __FUNCTION__);
            result = PVR_ERROR_FAILED;
        }
    };
    m_clientCore->ForEachEpg(action);
    return result;
}



bool PVRClientBase::OpenRecordedStream(const std::string& url,  Buffers::IPlaylistBufferDelegate* delegate)
{
     if (url.empty())
        return false;
    
    try
    {
        InputBuffer* buffer = NULL;
        
        const std::string m3uExt = ".m3u";
        const std::string m3u8Ext = ".m3u8";
        const bool isM3u = url.find(m3u8Ext) != std::string::npos || url.find(m3uExt) != std::string::npos;
        Buffers::PlaylistBufferDelegate plistDelegate(delegate);
        if(isM3u)
            buffer = new Buffers::PlaylistBuffer(url, plistDelegate, 20); // Cache 20 segments for archive seek optimization
        else
            buffer = new ArchiveBuffer(url);

        m_recordBuffer = buffer;
    }
    catch (InputBufferException & ex)
    {
        LogError(  "%s: failed. Can't open recording stream.", __FUNCTION__);
        return false;
    }
    
    return true;
    
}
void PVRClientBase::CloseRecordedStream(void)
{
    if(m_recordBuffer) {
        LogNotice("PVRClientBase: closing recorded sream...");
        SAFE_DELETE(m_recordBuffer);
        LogNotice("PVRClientBase: input recorded closed.");
    }
    
}
int PVRClientBase::ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
    uint32_t timeoutMs = 5000;
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->Read(pBuffer, iBufferSize, timeoutMs);
}

long long PVRClientBase::SeekRecordedStream(long long iPosition, int iWhence)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->Seek(iPosition, iWhence);
}

long long PVRClientBase::PositionRecordedStream(void)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->GetPosition();
}
long long PVRClientBase::LengthRecordedStream(void)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->GetLength();
}

PVR_ERROR PVRClientBase::CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    LogDebug( " >>>> !!!! Menu hook !!!! <<<<<");
    return MenuHook(menuhook, item);
}


