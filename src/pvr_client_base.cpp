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

#include <stdio.h>
#include <algorithm>
#include <list>
#include <map>
#include "kodi/libXBMC_addon.h"
#include "kodi/Filesystem.h"
#include "XMLTV_loader.hpp"

// Patch for Kodi buggy VFSDirEntry declaration
struct VFSDirEntry_Patch
{
    char* label;             //!< item label
    char* title;             //!< item title
    char* path;              //!< item path
    unsigned int num_props;  //!< Number of properties attached to item
    VFSProperty* properties; //!< Properties
    //    time_t date_time;        //!< file creation date & time
    bool folder;             //!< Item is a folder
    uint64_t size;           //!< Size of file represented by item
};

#include "p8-platform/util/util.h"
#include "p8-platform/util/timeutils.h"
#include "p8-platform/threads/mutex.h"
#include "p8-platform/util/StringUtils.h"


#include "timeshift_buffer.h"
#include "file_cache_buffer.hpp"
#include "memory_cache_buffer.hpp"
#include "plist_buffer.h"
#include "direct_buffer.h"
#include "simple_cyclic_buffer.hpp"
#include "helpers.h"
#include "pvr_client_base.h"
#include "globals.hpp"
#include "HttpEngine.hpp"
#include "client_core_base.hpp"
#include "ActionQueue.hpp"


using namespace std;
using namespace ADDON;
using namespace Buffers;
using namespace PvrClient;
using namespace Globals;
using namespace P8PLATFORM;
using namespace ActionQueue;

namespace CurlUtils
{
    extern void SetCurlTimeout(long timeout);
}
// NOTE: avoid '.' (dot) char in path. Causes to deadlock in Kodi code.
static const char* s_DefaultCacheDir = "special://temp/pvr-puzzle-tv";
static const char* s_DefaultRecordingsDir = "special://temp/pvr-puzzle-tv/recordings";
static std::string s_LocalRecPrefix = "Local";
static std::string s_RemoteRecPrefix = "On Server";
static const int c_InitialLastByteRead = 1;


const unsigned int RELOAD_EPG_MENU_HOOK = 1;
const unsigned int RELOAD_RECORDINGS_MENU_HOOK = 2;
const unsigned int PVRClientBase::s_lastCommonMenuHookId = RELOAD_RECORDINGS_MENU_HOOK;

ADDON_STATUS PVRClientBase::Init(PVR_PROPERTIES* pvrprops)
{
    m_clientCore = NULL;
    m_inputBuffer = NULL;
    m_recordBuffer.buffer = NULL;
    m_recordBuffer.duration = 0;
    m_localRecordBuffer = NULL;
    m_supportSeek = false;
    
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
    string recordingsPath;
    if (XBMC->GetSetting("recordings_path", &buffer))
        recordingsPath = buffer;
    
    int timeshiftBufferSize = 0;
    XBMC->GetSetting("timeshift_size", &timeshiftBufferSize);
    timeshiftBufferSize *= 1024*1024;
    
    int cacheSizeLimit = 0;
    XBMC->GetSetting("timeshift_off_cache_limit", &cacheSizeLimit);
    cacheSizeLimit *= 1024*1024;
    
    
    TimeshiftBufferType timeshiftBufferType = k_TimeshiftBufferMemory;
    XBMC->GetSetting("timeshift_type", &timeshiftBufferType);
    
    m_rpcPort = 8080;
    XBMC->GetSetting("rpc_local_port", &m_rpcPort);
    
    m_channelIndexOffset = 0;
    XBMC->GetSetting("channel_index_offset", &m_channelIndexOffset);
    
    m_addCurrentEpgToArchive = true;
    XBMC->GetSetting("archive_for_current_epg_item", &m_addCurrentEpgToArchive);

    m_addChannelGroupForArchive = false;
    XBMC->GetSetting("archive_use_channel_groups", &m_addChannelGroupForArchive);
    
    long waitForInetTimeout = 0;
    XBMC->GetSetting("wait_for_inet", &waitForInetTimeout);
    
    if(waitForInetTimeout > 0){
        XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32022));
        
        P8PLATFORM::CTimeout waitForInet(waitForInetTimeout * 1000);
        bool connected = false;
        long timeLeft = 0;
        do {
            timeLeft = waitForInet.TimeLeft();
            connected = HttpEngine::CheckInternetConnection(timeLeft/1000);
            if(!connected)
                P8PLATFORM::CEvent::Sleep(1000);
        }while(!connected && timeLeft > 0);
        if(!connected) {
            XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32023));
        }
    }
    
    CurlUtils::SetCurlTimeout(curlTimout);
    SetChannelReloadTimeout(channelTimeout);
    SetTimeshiftEnabled(isTimeshiftEnabled);
    SetTimeshiftPath(timeshiftPath);
    SetRecordingsPath(recordingsPath);
    SetTimeshiftBufferSize(timeshiftBufferSize);
    SetTimeshiftBufferType(timeshiftBufferType);
    SetCacheLimit(cacheSizeLimit);
    
    PVR_MENUHOOK hook = {RELOAD_EPG_MENU_HOOK, 32050, PVR_MENUHOOK_ALL};
    PVR->AddMenuHook(&hook);

    hook = {RELOAD_RECORDINGS_MENU_HOOK, 32051, PVR_MENUHOOK_ALL};
    PVR->AddMenuHook(&hook);

    // Local recordings path prefix
    char* localizedString  = XBMC->GetLocalizedString(32014);
    s_LocalRecPrefix = localizedString;
    XBMC->FreeString(localizedString);
    // Remote recordings path prefix
    localizedString  = XBMC->GetLocalizedString(32015);
    s_RemoteRecPrefix = localizedString;
    XBMC->FreeString(localizedString);
    
    m_liveChannelId =  m_localRecordChannelId = UnknownChannelId;
    m_lastBytesRead = c_InitialLastByteRead;
    m_lastRecordingsAmount = 0;
    
    m_destroyer = new CActionQueue(100, "Streams Destroyer");
    m_destroyer->CreateThread();
    
    return ADDON_STATUS_OK;
    
}

PVRClientBase::~PVRClientBase()
{
    Cleanup();
    if(m_destroyer)
        SAFE_DELETE(m_destroyer);
}
void PVRClientBase::Cleanup()
{
    CloseLiveStream();
    CloseRecordedStream();
    if(m_localRecordBuffer)
        SAFE_DELETE(m_localRecordBuffer);
}

void PVRClientBase::OnSystemSleep()
{
    Cleanup();
    DestroyCoreSafe();
}
void PVRClientBase::OnSystemWake()
{
    CreateCoreSafe(false);
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
    else if (strcmp(settingName, "recordings_path") == 0)
    {
        SetRecordingsPath((const char *)(settingValue));
    }
    else if (strcmp(settingName, "timeshift_type") == 0)
    {
        SetTimeshiftBufferType(*(TimeshiftBufferType*)(settingValue));
    }
    else if (strcmp(settingName, "timeshift_size") == 0)
    {
        auto size = *(int *)(settingValue);
        size *= 1024*1024;
        SetTimeshiftBufferSize(size);
    }
    else if (strcmp(settingName, "timeshift_off_cache_limit") == 0)
    {
        auto size = *(int *)(settingValue);
        size *= 1024*1024;
        SetCacheLimit(size);
    }
    else if (strcmp(settingName, "curl_timeout") == 0)
    {
        CurlUtils::SetCurlTimeout(*(int *)(settingValue));
    }
    else if (strcmp(settingName, "rpc_local_port") == 0)
    {
        m_rpcPort = *(int *)(settingValue);
    }
    else if (strcmp(settingName, "archive_for_current_epg_item") == 0)
    {
        bool newValue = *(bool *)(settingValue);
        if(newValue != m_addCurrentEpgToArchive) {
            m_addCurrentEpgToArchive = newValue;
            return ADDON_STATUS_NEED_RESTART;
        }
    }
    else if (strcmp(settingName, "archive_use_channel_groups") == 0)
    {
        bool newValue = *(bool *)(settingValue);
        if(newValue != m_addChannelGroupForArchive) {
            m_addChannelGroupForArchive = newValue;
            return ADDON_STATUS_NEED_RESTART;
        }
    }
    else if (strcmp(settingName, "channel_index_offset") == 0)
    {
        int newValue = *(int *)(settingValue);
        if(newValue != m_channelIndexOffset) {
            m_channelIndexOffset = newValue;
            return ADDON_STATUS_NEED_RESTART;
        }
    }

    return ADDON_STATUS_OK;
}

PVR_ERROR PVRClientBase::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    //pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    //pCapabilities->bSupportsRadio = true;
    //pCapabilities->bSupportsChannelGroups = true;
    //pCapabilities->bHandlesInputStream = true;
    //pCapabilities->bSupportsRecordings = true;
    pCapabilities->bSupportsTimers = true;

//    pCapabilities->bSupportsChannelScan = false;
//    pCapabilities->bHandlesDemuxing = false;
//    pCapabilities->bSupportsRecordingPlayCount = false;
//    pCapabilities->bSupportsLastPlayedPosition = false;
//    pCapabilities->bSupportsRecordingEdl = false;

    // Kodi 18
    pCapabilities->bSupportsRecordingsRename = false;
    pCapabilities->bSupportsRecordingsLifetimeChange = false;
    pCapabilities->bSupportsDescrambleInfo = false;

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

bool PVRClientBase::IsRealTimeStream(void)
{
    // Archive is not RTS
    if(m_recordBuffer.buffer)
        return false;
    // No timeshift means RTS
    if(!IsTimeshiftEnabled())
        return true;
    // Return true when timeshift buffer position close to end of buffer for < 10 sec
    // https://github.com/kodi-pvr/pvr.hts/issues/173
    CLockObject lock(m_mutex);
    if(nullptr == m_inputBuffer){
        return true;
    }
    double reliativePos = (double)(m_inputBuffer->GetLength() - m_inputBuffer->GetPosition()) / m_inputBuffer->GetLength();
    time_t timeToEnd = reliativePos * (m_inputBuffer->EndTime() - m_inputBuffer->StartTime());
    const bool isRTS = timeToEnd < 10;
    LogDebug("PVRClientBase: is RTS? %s. Reliative pos: %f. Time to end: %d", ((isRTS) ? "YES" : "NO"), reliativePos, timeToEnd );
    return isRTS;
}

PVR_ERROR PVRClientBase::GetStreamTimes(PVR_STREAM_TIMES *times)
{

    if (!times)
        return PVR_ERROR_INVALID_PARAMETERS;
//    if(!IsTimeshiftEnabled())
//        return PVR_ERROR_NOT_IMPLEMENTED;
    
    int64_t timeStart = 0;
    int64_t  timeEnd = 0;
    if (m_inputBuffer)
    {

        CLockObject lock(m_mutex);
        timeStart = m_inputBuffer->StartTime();
        timeEnd   = m_inputBuffer->EndTime();
    }
    else if (m_recordBuffer.buffer){
        {
            timeStart = 0;
            timeEnd   = m_recordBuffer.duration;
        }
    }
    else
        return PVR_ERROR_NOT_IMPLEMENTED;
    
    times->startTime = timeStart;
    times->ptsStart  = 0;
    times->ptsBegin  = 0;
    times->ptsEnd    = (timeEnd - timeStart) * DVD_TIME_BASE;
    return PVR_ERROR_NO_ERROR;
}

ADDON_STATUS PVRClientBase::GetStatus()
{
    return  /*(m_sovokTV == NULL)? ADDON_STATUS_LOST_CONNECTION : */ADDON_STATUS_OK;
}

void PVRClientBase::SetRecordingsPath(const std::string& path){
    const char* nonEmptyPath = (path.empty()) ? s_DefaultRecordingsDir : path.c_str();
    if(!XBMC->DirectoryExists(nonEmptyPath))
        if(!XBMC->CreateDirectory(nonEmptyPath))
            LogError( "Failed to create recordings folder.");
    m_recordingsDir = nonEmptyPath;
}

void PVRClientBase::SetTimeshiftPath(const std::string& path){
    const char* nonEmptyPath = (path.empty()) ? s_DefaultCacheDir : path.c_str();
    if(!XBMC->DirectoryExists(nonEmptyPath))
        if(!XBMC->CreateDirectory(nonEmptyPath))
            LogError( "Failed to create cache folder");
    // Cleanup chache
    if(XBMC->DirectoryExists(nonEmptyPath))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(nonEmptyPath, "*.bin", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(!f.folder)
                    if(!XBMC->DeleteFile(f.path))
                        LogError( "Failed to delete timeshift folder entry %s", f.path);
            }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of timeshift folder %s", nonEmptyPath);
        }
    }

    m_cacheDir = nonEmptyPath;
}

PVR_ERROR  PVRClientBase::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    
    if(menuhook.iHookId == RELOAD_EPG_MENU_HOOK ) {
        XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32012));
        OnReloadEpg();
        m_clientCore->CallRpcAsync("{\"jsonrpc\": \"2.0\", \"method\": \"GUI.ActivateWindow\", \"params\": {\"window\": \"pvrsettings\"},\"id\": 1}",
                     [&] (rapidjson::Document& jsonRoot) {
                         XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32016));
                     },
                     [&](const ActionQueue::ActionResult& s) {});

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
    
    if(!bRadio) {
        m_kodiToPluginLut.clear();
        m_pluginToKodiLut.clear();
    }
    
    for(auto& itChannel : m_clientCore->GetChannelList())
    {
        auto & channel = itChannel.second;
        if (bRadio == channel.IsRadio)
        {
            KodiChannelId uniqueId = XMLTV::ChannelIdForChannelName(channel.Name);
            m_kodiToPluginLut[uniqueId] = channel.Id;
            m_pluginToKodiLut[channel.Id] = uniqueId;
            
            PVR_CHANNEL pvrChannel = { 0 };
            pvrChannel.iUniqueId = uniqueId;
            pvrChannel.iChannelNumber = channel.Number + m_channelIndexOffset;
            pvrChannel.bIsRadio = channel.IsRadio;
            strncpy(pvrChannel.strChannelName, channel.Name.c_str(), sizeof(pvrChannel.strChannelName));
            strncpy(pvrChannel.strIconPath, channel.IconPath.c_str(), sizeof(pvrChannel.strIconPath));
            
            LogDebug("==> CH %-5d - %-40s", pvrChannel.iChannelNumber, channel.Name.c_str());
            
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
            pvrGroupMember.iChannelUniqueId = m_pluginToKodiLut.at(it.second);
            pvrGroupMember.iChannelNumber = it.first;
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
   
    if(m_kodiToPluginLut.count(channel.iUniqueId) == 0){
        LogError("PVRClientBase: EPG request for unknown channel ID %d", channel.iUniqueId);
        return PVR_ERROR_NO_ERROR;
    }
    
    EpgEntryList epgEntries;
    m_clientCore->GetEpg(m_kodiToPluginLut.at(channel.iUniqueId), iStart, iEnd, epgEntries);
    EpgEntryList::const_iterator itEpgEntry = epgEntries.begin();
    for (int i = 0; itEpgEntry != epgEntries.end(); ++itEpgEntry, ++i)
    {
        EPG_TAG tag = { 0 };
        tag.iUniqueBroadcastId = itEpgEntry->first;
        itEpgEntry->second.FillEpgTag(tag);
        tag.iUniqueChannelId = m_pluginToKodiLut.at(itEpgEntry->second.ChannelId);
        PVR->TransferEpgEntry(handle, &tag);
    }
    return PVR_ERROR_NO_ERROR;
}

#pragma mark - Streams

std::string PVRClientBase::GetLiveUrl() const {
    return (m_inputBuffer) ? m_inputBuffer->GetUrl() : std::string();
    
}

InputBuffer*  PVRClientBase::BufferForUrl(const std::string& url )
{
    InputBuffer* buffer = NULL;
    const std::string m3uExt = ".m3u";
    const std::string m3u8Ext = ".m3u8";
    if( url.find(m3u8Ext) != std::string::npos || url.find(m3uExt) != std::string::npos)
        buffer = new Buffers::PlaylistBuffer(url, nullptr, false); // No segments cache for live playlist
    else
        buffer = new DirectBuffer(url);
    return buffer;
}

std::string PVRClientBase::GetStreamUrl(ChannelId channel)
{
    if(NULL == m_clientCore)
        return string();
    return  m_clientCore->GetUrl(channel);
}

bool PVRClientBase::OpenLiveStream(const PVR_CHANNEL& channel)
{
    if(m_kodiToPluginLut.count(channel.iUniqueId) == 0){
        LogError("PVRClientBase: open stream request for unknown channel ID %d", channel.iUniqueId);
        return false;
    }
    
    m_lastBytesRead = c_InitialLastByteRead;
    const ChannelId chId = m_kodiToPluginLut.at(channel.iUniqueId);
    bool succeeded = OpenLiveStream(chId, GetStreamUrl(chId));
    bool tryToRecover = !succeeded;
    while(tryToRecover) {
        string url = GetNextStreamUrl(chId);
        if(url.empty()) {// nomore streams
            LogDebug("No alternative stream found.");
            XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32026));
            break;
        }
        XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32025));
        succeeded = OpenLiveStream(chId, url);
        tryToRecover = !succeeded;
    }
    
    return succeeded;

}

Buffers::ICacheBuffer* PVRClientBase::CreateLiveCache() const {
    if (m_isTimeshiftEnabled){
        if(k_TimeshiftBufferFile == m_timeshiftBufferType) {
            return new Buffers::FileCacheBuffer(m_cacheDir, m_timshiftBufferSize /  Buffers::FileCacheBuffer::CHUNK_FILE_SIZE_LIMIT);
        } else {
            return new Buffers::MemoryCacheBuffer(m_timshiftBufferSize /  Buffers::MemoryCacheBuffer::CHUNK_SIZE_LIMIT);
        }
    }
    else
        return new Buffers::SimpleCyclicBuffer(m_cacheSizeLimit / Buffers::SimpleCyclicBuffer::CHUNK_SIZE_LIMIT);

}

bool PVRClientBase::OpenLiveStream(ChannelId channelId, const std::string& url )
{
    
    if(channelId == m_liveChannelId && IsLiveInRecording())
        return true; // Do not change url of local recording stream

    if(channelId == m_localRecordChannelId) {
        CLockObject lock(m_mutex);
        m_liveChannelId = m_localRecordChannelId;
        m_inputBuffer = m_localRecordBuffer;
        return true;
    }

    m_liveChannelId = UnknownChannelId;
    if (url.empty())
        return false;
    try
    {
        InputBuffer* buffer = BufferForUrl(url);
        CLockObject lock(m_mutex);
        m_inputBuffer = new Buffers::TimeshiftBuffer(buffer, CreateLiveCache());
        if(!m_inputBuffer->WaitForInput(m_channelReloadTimeout * 1000)) {
            throw InputBufferException("no data available diring reload timeout (bad ace link?)");
        }
    }
    catch (InputBufferException &ex)
    {
        LogError(  "PVRClientBase: input buffer error in OpenLiveStream: %s", ex.what());
        CloseLiveStream();
        OnOpenStremFailed(channelId, url);
        return false;
    }
    m_liveChannelId = channelId;
    return true;
}

void PVRClientBase::CloseLiveStream()
{
    CLockObject lock(m_mutex);
    m_liveChannelId = UnknownChannelId;
    if(m_inputBuffer && !IsLiveInRecording()) {
        LogNotice("PVRClientBase: closing input stream...");
        auto oldBuffer = m_inputBuffer;
        m_destroyer->PerformAsync([oldBuffer] (){
            LogDebug("PVRClientBase: destroying input stream...");
            delete oldBuffer;
            LogDebug("PVRClientBase: input stream been destroyed");
        }, [] (const ActionResult& result) {
            if(result.exception){
                try {
                    std::rethrow_exception(result.exception);
                } catch (std::exception ex) {
                    LogError("PVRClientBase: exception thrown during closing of input stream: %s.", ex.what());

                }
            } else {
                LogNotice("PVRClientBase: input stream closed.");
            }
        });
    }
    
    m_inputBuffer = nullptr;
}

int PVRClientBase::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    CLockObject lock(m_mutex);
    if(nullptr == m_inputBuffer){
        return -1;
    }

    ChannelId chId = GetLiveChannelId();
    int bytesRead = m_inputBuffer->Read(pBuffer, iBufferSize, m_channelReloadTimeout * 1000);
    // Assuming stream hanging.
    // Try to restart current channel only when previous read operation succeeded.
    if (bytesRead != iBufferSize &&  m_lastBytesRead >= 0 && !IsLiveInRecording()) {
        LogError("PVRClientBase:: trying to restart current channel.");
        string  url = m_inputBuffer->GetUrl();
        if(!url.empty()){
            XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32000));
            if(SwitchChannel(chId, url))
                bytesRead = m_inputBuffer->Read(pBuffer, iBufferSize, m_channelReloadTimeout * 1000);
            else
                bytesRead = -1;
        }
    }
    
    m_lastBytesRead = bytesRead;
    return bytesRead;
}

long long PVRClientBase::SeekLiveStream(long long iPosition, int iWhence)
{
    CLockObject lock(m_mutex);
    if(nullptr == m_inputBuffer){
        return -1;
    }
    return m_inputBuffer->Seek(iPosition, iWhence);
}

long long PVRClientBase::PositionLiveStream()
{
    CLockObject lock(m_mutex);
    if(nullptr == m_inputBuffer){
        return -1;
    }
    return m_inputBuffer->GetPosition();
}

long long PVRClientBase::LengthLiveStream()
{
    CLockObject lock(m_mutex);
    if(nullptr == m_inputBuffer){
        return -1;
    }
    return m_inputBuffer->GetLength();
}

bool PVRClientBase::SwitchChannel(const PVR_CHANNEL& channel)
{
    const ChannelId chId = m_kodiToPluginLut.at(channel.iUniqueId);
    return SwitchChannel(chId, GetStreamUrl(chId));
}

bool PVRClientBase::SwitchChannel(ChannelId channelId, const std::string& url)
{
    if(url.empty())
        return false;

    CLockObject lock(m_mutex);
    CloseLiveStream();
    return OpenLiveStream(channelId, url); // Split/join live and recording streams (when nesessry)
}

void PVRClientBase::SetTimeshiftEnabled(bool enable)
{
    m_isTimeshiftEnabled = enable;
}

void PVRClientBase::SetChannelReloadTimeout(int timeout)
{
    m_channelReloadTimeout = timeout;
}

void PVRClientBase::SetCacheLimit(uint64_t size)
{
    m_cacheSizeLimit = size;
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
        return true;
    };
    m_clientCore->ForEachEpg(action);
//    if(size == 0){
//        m_clientCore->ReloadRecordings();
//        m_clientCore->ForEachEpg(action);
//    }
    
    // Add local recordings
    if(XBMC->DirectoryExists(m_recordingsDir.c_str()))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(m_recordingsDir.c_str(), "", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(f.folder)
                    ++size;

            }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of local recordings folder (amount) %s", m_recordingsDir.c_str());
        }

    }

    if(m_lastRecordingsAmount  != size)
        ;//PVR->TriggerRecordingUpdate();
    LogDebug("PVRClientBase: found %d recordings. Was %d", size, m_lastRecordingsAmount);
    m_lastRecordingsAmount = size;
    return size;
    
}

void PVRClientBase::FillRecording(const EpgEntryList::value_type& epgEntry, PVR_RECORDING& tag, const char* dirPrefix)
{
    const auto& epgTag = epgEntry.second;
    
    const Channel& ch = m_clientCore->GetChannelList().at(epgTag.ChannelId);
    sprintf(tag.strRecordingId, "%d",  epgEntry.first);
    strncpy(tag.strTitle, epgTag.Title.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
    strncpy(tag.strPlot, epgTag.Description.c_str(), PVR_ADDON_DESC_STRING_LENGTH - 1);
    strncpy(tag.strChannelName, ch.Name.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
    tag.recordingTime = epgTag.StartTime;
    tag.iLifetime = 0; /* not implemented */
    
    tag.iDuration = epgTag.EndTime - epgTag.StartTime;
    tag.iEpgEventId = epgEntry.first;
    tag.iChannelUid = m_pluginToKodiLut.at(epgTag.ChannelId);
    tag.channelType = PVR_RECORDING_CHANNEL_TYPE_TV;
    if(!epgTag.IconPath.empty())
        strncpy(tag.strIconPath, epgTag.IconPath.c_str(), sizeof(tag.strIconPath) -1);
    
    string dirName(dirPrefix);
    dirName += '/';
    if(m_addChannelGroupForArchive) {
        GroupId groupId = m_clientCore->GroupForChannel(ch.Id);
        dirName += (-1 == groupId) ? "---" : m_clientCore->GetGroupList().at(groupId).Name.c_str();
        dirName += '/';
    }
    dirName += tag.strChannelName;
    char buff[20];
    strftime(buff, sizeof(buff), "/%d-%m-%y", localtime(&epgTag.StartTime));
    dirName += buff;
    strncpy(tag.strDirectory, dirName.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);

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
    
    std::list<PVR_RECORDING> recs;
    IClientCore::EpgEntryAction action = [pThis ,&result, &recs](const EpgEntryList::value_type& epgEntry)
    {
        try {
            if(!epgEntry.second.HasArchive)
                return true;

            PVR_RECORDING tag = { 0 };
            pThis->FillRecording(epgEntry, tag, s_RemoteRecPrefix.c_str());
            recs.push_back(tag);
            return true;
        }
        catch (...)  {
            LogError( "%s: failed.", __FUNCTION__);
            result = PVR_ERROR_FAILED;
        }
        // Should not be here..
        return false;
    };
    m_clientCore->ForEachEpg(action);
    
    // Populate server recordings
    for (auto& r : recs) {
        PVR->TransferRecordingEntry(handle, &r);
    }
    // Add local recordings
    if(XBMC->DirectoryExists(m_recordingsDir.c_str()))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(m_recordingsDir.c_str(), "", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(!f.folder)
                    continue;
                std::string infoPath = f.path;
                infoPath += PATH_SEPARATOR_CHAR;
                infoPath += "recording.inf";
                void* infoFile = XBMC->OpenFile(infoPath.c_str(), 0);
                if(nullptr == infoFile)
                    continue;
                PVR_RECORDING tag = { 0 };
                bool isValid = XBMC->ReadFile(infoFile, &tag, sizeof(tag)) == sizeof(tag);
                XBMC->CloseFile(infoFile);
                if(!isValid)
                    continue;
                
                PVR->TransferRecordingEntry(handle, &tag);
           }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of local recordings folder %s", m_recordingsDir.c_str());
        }
        
    }

    return result;
}

PVR_ERROR PVRClientBase::DeleteRecording(const PVR_RECORDING &recording)
{
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    // Is recording local?
    if(!IsLocalRecording(recording))
        return PVR_ERROR_REJECTED;
    std::string dir = DirectoryForRecording(stoul(recording.strRecordingId));
    if(!XBMC->DirectoryExists(dir.c_str()))
        return PVR_ERROR_INVALID_PARAMETERS;

    
    if(XBMC->DirectoryExists(m_recordingsDir.c_str()))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(dir.c_str(), "", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(f.folder)
                    continue;
                if(!XBMC->DeleteFile(f.path))
                    return PVR_ERROR_FAILED;
            }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of local recordings folder (delete) %s", m_recordingsDir.c_str());
        }
        
    }

    XBMC->RemoveDirectory(dir.c_str());
    PVR->TriggerRecordingUpdate();
    
    return PVR_ERROR_NO_ERROR;
}

bool PVRClientBase::IsLiveInRecording() const
{
    return m_inputBuffer == m_localRecordBuffer;
}


bool PVRClientBase::IsLocalRecording(const PVR_RECORDING &recording) const
{
    return StringUtils::StartsWith(recording.strDirectory, s_LocalRecPrefix.c_str());
}

bool PVRClientBase::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(!IsLocalRecording(recording))
        return false;
    try {
        InputBuffer* buffer = new DirectBuffer(new FileCacheBuffer(DirectoryForRecording(stoul(recording.strRecordingId))));
        
        if(m_recordBuffer.buffer)
            SAFE_DELETE(m_recordBuffer.buffer);
        m_recordBuffer.buffer = buffer;
        m_recordBuffer.duration = recording.iDuration;
    } catch (std::exception ex) {
        LogError("OpenRecordedStream (local) exception: %s", ex.what());
    }
    
    return true;
}

bool PVRClientBase::OpenRecordedStream(const std::string& url,  Buffers::IPlaylistBufferDelegate* delegate, RecordingStreamFlags flags)
{
     if (url.empty())
        return false;
    
    try
    {
        InputBuffer* buffer = NULL;
        
        const bool enforcePlaylist = (flags & ForcePlaylist) == ForcePlaylist;
        const std::string m3uExt = ".m3u";
        const std::string m3u8Ext = ".m3u8";
        const bool isM3u = enforcePlaylist || url.find(m3u8Ext) != std::string::npos || url.find(m3uExt) != std::string::npos;
        const bool seekForVod = (flags & SupportVodSeek) == SupportVodSeek;
        Buffers::PlaylistBufferDelegate plistDelegate(delegate);
        if(isM3u)
            buffer = new Buffers::PlaylistBuffer(url, plistDelegate, seekForVod);
        else
            buffer = new ArchiveBuffer(url);

        m_recordBuffer.buffer = buffer;
        m_recordBuffer.duration = (delegate) ? delegate->Duration() : 0;
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
    if(m_recordBuffer.buffer) {
        LogNotice("PVRClientBase: closing recorded sream...");
        SAFE_DELETE(m_recordBuffer.buffer);
        LogNotice("PVRClientBase: input recorded closed.");
    }
    
}

PVR_ERROR PVRClientBase::GetStreamReadChunkSize(int* chunksize) {
    // TODO: obtain from buffer...
    *chunksize = 32 * 1024; // 32K chunk.
    return PVR_ERROR_NO_ERROR;
}

int PVRClientBase::ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
    //uint32_t timeoutMs = 5000;
    return (m_recordBuffer.buffer == NULL) ? -1 : m_recordBuffer.buffer->Read(pBuffer, iBufferSize, m_channelReloadTimeout * 1000);
}

long long PVRClientBase::SeekRecordedStream(long long iPosition, int iWhence)
{
    return (m_recordBuffer.buffer == NULL) ? -1 : m_recordBuffer.buffer->Seek(iPosition, iWhence);
}

long long PVRClientBase::PositionRecordedStream(void)
{
    return (m_recordBuffer.buffer == NULL) ? -1 : m_recordBuffer.buffer->GetPosition();
}
long long PVRClientBase::LengthRecordedStream(void)
{
    return (m_recordBuffer.buffer == NULL) ? -1 : m_recordBuffer.buffer->GetLength();
}

PVR_ERROR PVRClientBase::IsEPGTagRecordable(const EPG_TAG*, bool* bIsRecordable)
{
    // Seems we can record all tags
    *bIsRecordable = true;
    return PVR_ERROR_NO_ERROR;
}

#pragma mark - Timer  delegate

std::string PVRClientBase::DirectoryForRecording(unsigned int epgId) const
{
    std::string recordingDir = m_recordingsDir;
    if(recordingDir[recordingDir.length() -1] != PATH_SEPARATOR_CHAR)
        recordingDir += PATH_SEPARATOR_CHAR;
    recordingDir += n_to_string(epgId);
    return recordingDir;
}

std::string PVRClientBase::PathForRecordingInfo(unsigned int epgId) const
{
    std::string infoPath = DirectoryForRecording(epgId);
    infoPath += PATH_SEPARATOR_CHAR;
    infoPath += "recording.inf";
    return infoPath;
}

bool PVRClientBase::StartRecordingFor(const PVR_TIMER &timer)
{
    if(NULL == m_clientCore)
        return false;

    bool hasEpg = false;
    auto pThis = this;
    PVR_RECORDING tag = { 0 };
    IClientCore::EpgEntryAction action = [pThis ,&tag, timer, &hasEpg](const EpgEntryList::value_type& epgEntry)
    {
        try {
            if(epgEntry.first != timer.iEpgUid)
                return true;
           
            pThis->FillRecording(epgEntry, tag, s_LocalRecPrefix.c_str());
            tag.recordingTime = time(nullptr);
            tag.iDuration = timer.endTime - tag.recordingTime;
            hasEpg = true;
            return false;
        }
        catch (...)  {
            LogError( "%s: failed.", __FUNCTION__);
            hasEpg = false;
        }
        // Should not be here...
        return false;
    };
    m_clientCore->ForEachEpg(action);
    
    if(!hasEpg) {
        LogError("StartRecordingFor(): timers without EPG are not supported.");
        return false;
    }
    
    std::string recordingDir = DirectoryForRecording(timer.iEpgUid);
    
    if(!XBMC->CreateDirectory(recordingDir.c_str())) {
        LogError("StartRecordingFor(): failed to create recording directory %s ", recordingDir.c_str());
        return false;
    }

    std::string infoPath = PathForRecordingInfo(timer.iEpgUid);
    void* infoFile = XBMC->OpenFileForWrite(infoPath.c_str() , true);
    if(nullptr == infoFile){
        LogError("StartRecordingFor(): failed to create recording info file %s ", infoPath.c_str());
        return false;
    }
    if(XBMC->WriteFile(infoFile, &tag, sizeof(tag))  != sizeof(tag)){
        LogError("StartRecordingFor(): failed to write recording info file %s ", infoPath.c_str());
        XBMC->CloseFile(infoFile);
        return false;
    }
    XBMC->CloseFile(infoFile);
    
    std::string url = m_clientCore ->GetUrl(timer.iClientChannelUid);
    m_localRecordChannelId = timer.iClientChannelUid;
    // When recording channel is same to live channel
    // merge live buffer with local recording
    if(m_liveChannelId == timer.iClientChannelUid){
//        CLockObject lock(m_mutex);
//        CloseLiveStream();
        m_inputBuffer->SwapCache( new Buffers::FileCacheBuffer(recordingDir, 255, false));
        m_localRecordBuffer = m_inputBuffer;
        m_liveChannelId = m_localRecordChannelId; // ???
        return true;
    }
    // otherwise just open new recording stream
    m_localRecordBuffer = new Buffers::TimeshiftBuffer(BufferForUrl(url), new Buffers::FileCacheBuffer(recordingDir, 255, false));

    return true;
}

bool PVRClientBase::StopRecordingFor(const PVR_TIMER &timer)
{
    void* infoFile = nullptr;
    // Update recording duration
    do {
        std::string infoPath = PathForRecordingInfo(timer.iEpgUid);
        void* infoFile = XBMC->OpenFileForWrite(infoPath.c_str() , false);
        if(nullptr == infoFile){
            LogError("StopRecordingFor(): failed to open recording info file %s ", infoPath.c_str());
            break;
        }
        PVR_RECORDING tag = {0};
        XBMC->SeekFile(infoFile, 0, SEEK_SET);
        if(XBMC->ReadFile(infoFile, &tag, sizeof(tag))  != sizeof(tag)){
            LogError("StopRecordingFor(): failed to read from recording info file %s ", infoPath.c_str());
            break;
        }
        tag.iDuration = time(nullptr) - tag.recordingTime;
        XBMC->SeekFile(infoFile, 0, SEEK_SET);
        if(XBMC->WriteFile(infoFile, &tag, sizeof(tag))  != sizeof(tag)){
            LogError("StopRecordingFor(): failed to write recording info file %s ", infoPath.c_str());
            XBMC->CloseFile(infoFile);
            break;
        }
    } while(false);
    if(nullptr != infoFile)
        XBMC->CloseFile(infoFile);
    
    // When recording channel is same to live channel
    // merge live buffer with local recording
    if(m_liveChannelId == timer.iClientChannelUid){
        //CLockObject lock(m_mutex);
        m_inputBuffer->SwapCache(CreateLiveCache());
        m_localRecordBuffer = nullptr;
    } else {
        if(m_localRecordBuffer) {
            m_localRecordChannelId = UnknownChannelId;
            SAFE_DELETE(m_localRecordBuffer);
        }
    }
    
    // trigger Kodi recordings update
    PVR->TriggerRecordingUpdate();
    return true;
    
}
bool PVRClientBase::FindEpgFor(const PVR_TIMER &timer)
{
    return true;
}



#pragma mark - Menus

PVR_ERROR PVRClientBase::CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    LogDebug( " >>>> !!!! Menu hook !!!! <<<<<");
    return MenuHook(menuhook, item);
}

#pragma mark - Playlist Utils
bool PVRClientBase::CheckPlaylistUrl(const std::string& url)
{
    auto f  = XBMC->OpenFile(url.c_str(), 0);
    
    if (nullptr == f) {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32010));
        return false;
    }
    
    XBMC->CloseFile(f);
    return true;
}

void PvrClient::EpgEntry::FillEpgTag(EPG_TAG& tag) const{
    // NOTE: internal channel ID is not valid for Kodi's EPG
    // This field should be filled by caller
    //tag.iUniqueChannelId = ChannelId;
    tag.strTitle = Title.c_str();
    tag.strPlot = Description.c_str();
    tag.startTime = StartTime;
    tag.endTime = EndTime;
    tag.strIconPath = IconPath.c_str();
    if(!Category.empty()) {
        tag.iGenreType = EPG_GENRE_USE_STRING;
        tag.strGenreDescription = Category.c_str();
    }
}

