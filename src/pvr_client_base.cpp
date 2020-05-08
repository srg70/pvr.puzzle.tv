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
#include <chrono>
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
#include "addon_settings.h"

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
static const char* const s_DefaultCacheDir = "special://temp/pvr-puzzle-tv";
static const char* const s_DefaultRecordingsDir = "special://temp/pvr-puzzle-tv/recordings";
static std::string s_LocalRecPrefix = "Local";
static std::string s_RemoteRecPrefix = "On Server";
static const int c_InitialLastByteRead = 1;


const unsigned int RELOAD_EPG_MENU_HOOK = 1;
const unsigned int RELOAD_RECORDINGS_MENU_HOOK = 2;
const unsigned int PVRClientBase::s_lastCommonMenuHookId = RELOAD_RECORDINGS_MENU_HOOK;

static void DelayStartup(int delayInSec) {
    if(delayInSec <= 0){
        return;
    }
    XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32027), delayInSec);
    P8PLATFORM::CEvent::Sleep(delayInSec * 1000);
}


static int CheckForInetConnection(int waitForInetTimeout)
{
    int waitingTimeInSec = 0;
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
        waitingTimeInSec = waitForInetTimeout - waitForInet.TimeLeft()/1000;
    }
    return waitingTimeInSec;
}

static void CleanupTimeshiftDirectory(const std::string& path){
    const char* nonEmptyPath = path.c_str();
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
}

static void CheckRecordingsPath(const std::string& path){
    const char* nonEmptyPath = path.c_str();
    if(!XBMC->DirectoryExists(nonEmptyPath))
        if(!XBMC->CreateDirectory(nonEmptyPath))
            LogError( "Failed to create recordings folder.");
}


PVRClientBase::PVRClientBase()
    : m_addonSettings(m_addonMutableSettings)
{
    
}

const char* const c_curlTimeout = "curl_timeout";
const char* const c_channelReloadTimeout = "channel_reload_timeout";
const char* const c_numOfHlsThreads = "num_of_hls_threads";
const char* const c_enableTimeshift = "enable_timeshift";
const char* const c_timeshiftPath = "timeshift_path";
const char* const c_recordingPath = "recordings_path";
const char* const c_timeshiftSize = "timeshift_size";
const char* const c_cacheSizeLimit = "timeshift_off_cache_limit";
const char* const c_timeshiftType = "timeshift_type";
const char* const c_rpcLocalPort = "rpc_local_port";
const char* const c_channelIndexOffset = "channel_index_offset";
const char* const c_addCurrentEpgToArchive = "archive_for_current_epg_item";
const char* const c_useChannelGroupsForArchive = "archive_use_channel_groups";
const char* const c_waitForInternetTimeout = "wait_for_inet";
const char* const c_startupDelay = "startup_delay";
const char* const c_startRecordingPadding = "archive_start_padding";
const char* const c_endRecordingPadding = "archive_end_padding";
const char* const c_supportArchive = "archive_support";
const char* const c_loadArchiveAfterEpg = "archive_wait_for_epg";
const char* const c_archiveRefreshInterval = "archive_refresh_interval";

ADDON_STATUS PVRClientBase::Init(PVR_PROPERTIES* pvrprops)
{
    m_clientCore = NULL;
    m_inputBuffer = NULL;
    m_recordBuffer.buffer = NULL;
    m_recordBuffer.duration = 0;
    m_recordBuffer.isLocal = false;
    m_localRecordBuffer = NULL;
    m_supportSeek = false;
    
    LogDebug( "User path: %s", pvrprops->strUserPath);
    LogDebug( "Client path: %s", pvrprops->strClientPath);
    //auto g_strUserPath   = pvrprops->strUserPath;
    m_clientPath = pvrprops->strClientPath;
    m_userPath = pvrprops->strUserPath;
            
    m_addonMutableSettings
    .Add(c_curlTimeout, 15, CurlUtils::SetCurlTimeout)
    .Add(c_channelReloadTimeout, 5)
    .Add(c_numOfHlsThreads, 1, Buffers::PlaylistBuffer::SetNumberOfHlsTreads)
    .Add(c_enableTimeshift, false)
    .Add(c_timeshiftPath, s_DefaultCacheDir, CleanupTimeshiftDirectory)
    .Add(c_recordingPath, s_DefaultRecordingsDir, CheckRecordingsPath)
    .Add(c_timeshiftSize, 0)
    .Add(c_cacheSizeLimit, 0)
    .Add(c_timeshiftType, (int)k_TimeshiftBufferMemory)
    .Add(c_rpcLocalPort, 8080, ADDON_STATUS_NEED_RESTART)
    .Add(c_channelIndexOffset, 0, ADDON_STATUS_NEED_RESTART)
    .Add(c_addCurrentEpgToArchive, (int)k_AddCurrentEpgToArchive_No, ADDON_STATUS_NEED_RESTART)
    .Add(c_useChannelGroupsForArchive, false, ADDON_STATUS_NEED_RESTART)
    .Add(c_waitForInternetTimeout, 0)
    .Add(c_startupDelay,0)
    .Add(c_startRecordingPadding, 0)
    .Add(c_endRecordingPadding, 0)
    .Add(c_supportArchive, false, ADDON_STATUS_NEED_RESTART)
    .Add(c_loadArchiveAfterEpg, false, ADDON_STATUS_NEED_RESTART)
    .Add(c_archiveRefreshInterval, 3);
    
    PopulateSettings(m_addonMutableSettings);
    
    m_addonMutableSettings.Init();
    m_addonMutableSettings.Print();
       
    DelayStartup(StartupDelay() - CheckForInetConnection(WaitForInetTimeout()));
    
    
    PVR_MENUHOOK hook = {RELOAD_EPG_MENU_HOOK, 32050, PVR_MENUHOOK_ALL};
    PVR->AddMenuHook(&hook);

    hook = {RELOAD_RECORDINGS_MENU_HOOK, 32051, PVR_MENUHOOK_ALL};
    PVR->AddMenuHook(&hook);

    // Local recordings path prefix
    s_LocalRecPrefix = XBMC_Message(32014);
    // Remote recordings path prefix
    s_RemoteRecPrefix = XBMC_Message(32015);
    
    m_liveChannelId =  m_localRecordChannelId = UnknownChannelId;
    m_lastBytesRead = c_InitialLastByteRead;
    m_lastRecordingsAmount = 0;
    
    m_destroyer = new CActionQueue(100, "Streams Destroyer");
    m_destroyer->CreateThread();
    
    return ADDON_STATUS_OK;
    
}

void PVRClientBase::OnCoreCreated() {
    m_clientCore->SetRpcPort(RpcLocalPort());
    m_destroyer->PerformAsync([this](){
//        while(m_clientCore == nullptr) {
//            P8PLATFORM::CEvent::Sleep(100);
//        }
        if(nullptr == m_clientCore)
            throw std::logic_error("Client core must be initialized alraedy!");
        auto phase =  m_clientCore->GetPhase(IClientCore::k_ChannelsLoadingPhase);
        phase->Wait();

        m_kodiToPluginLut.clear();
        m_pluginToKodiLut.clear();
        for (const auto& channel : m_clientCore->GetChannelList()) {
            KodiChannelId uniqueId = XMLTV::ChannelIdForChannelName(channel.second.Name);
            m_kodiToPluginLut[uniqueId] = channel.second.UniqueId;
            m_pluginToKodiLut[channel.second.UniqueId] = uniqueId;
        }
        phase =  m_clientCore->GetPhase(IClientCore::k_ChannelsIdCreatingPhase);
        if(nullptr != phase) {
            phase->Broadcast();
        }
    }, [this](const ActionResult& res){
        if(ActionQueue::kActionCompleted != res.status) {
            LogError("PVRClientBase: async creating of LUTs failed! Critical error.");
        } else {
            auto phase =  m_clientCore->GetPhase(IClientCore::k_EpgLoadingPhase);
            phase->Wait();
            if(LoadArchiveAfterEpg()) {
                LogDebug("PVRClientBase: update recorderings.");
                m_clientCore->ReloadRecordings();
            }
            LogDebug("PVRClientBase: sheduling first recording update.");
            ScheduleRecordingsUpdate();
        }
    });
}

void PVRClientBase::ScheduleRecordingsUpdate() {
     m_destroyer->PerformAsync([this]() {
         if(!m_destroyerEvent.Wait(ArchiveRefreshInterval() * 60 * 1000) && nullptr != m_clientCore && !m_destroyer->IsStopped()) {
            LogDebug("PVRClientBase: call reload recorderings.");
            m_clientCore->ReloadRecordings();
         } else {
             LogDebug("PVRClientBase: bypass reload recorderings.");
         }
         
     }, [this](const ActionResult& res) {
         if(ActionQueue::kActionFailed == res.status) {
             LogError("PVRClientBase: async recordings update failed!");
         } else if(ActionQueue::kActionCompleted == res.status) {
             LogDebug("PVRClientBase: sheduling next recording update.");
             ScheduleRecordingsUpdate();
         }else {
             LogDebug("PVRClientBase: sheduling of recording update canceled.");
         }
     });
}


PVRClientBase::~PVRClientBase()
{
    Cleanup();
    if(m_destroyer) {
        m_destroyer->StopThread(1);
        while(m_destroyer->IsRunning()) {
            m_destroyerEvent.Broadcast();
        }
        SAFE_DELETE(m_destroyer);
    }
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
    DelayStartup(StartupDelay() - CheckForInetConnection(WaitForInetTimeout()));
    CreateCoreSafe(false);
}


ADDON_STATUS PVRClientBase::SetSetting(const char *settingName, const void *settingValue)
{
    try {
        return m_addonSettings.Set(settingName, settingValue);
    }
    catch (std::exception& ex) {
        LogInfo("Error on settings update: %s", ex.what());
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
    pCapabilities->bSupportsRecordings = true; //For local recordings
;
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
    //LogDebug("PVRClientBase: is RTS? %s. Reliative pos: %f. Time to end: %d", ((isRTS) ? "YES" : "NO"), reliativePos, timeToEnd );
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

    {
        CLockObject lock(m_mutex);
        if (m_inputBuffer)
        {
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
    }
    
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

const ChannelList& PVRClientBase::GetChannelListWhenLutsReady()
{
    auto phase =  m_clientCore->GetPhase(IClientCore::k_ChannelsIdCreatingPhase);
    if(phase) {
        phase->Wait();
    }
    return m_clientCore->GetChannelList();
}

PVR_ERROR PVRClientBase::GetChannels(ADDON_HANDLE handle, bool bRadio)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
        
    const int channelIndexOffset = ChannelIndexOffset();
    for(auto& itChannel : GetChannelListWhenLutsReady())
    {
        auto & channel = itChannel.second;
        if (bRadio == channel.IsRadio)
        {
            PVR_CHANNEL pvrChannel = { 0 };
            pvrChannel.iUniqueId = m_pluginToKodiLut.at(channel.UniqueId) ;
            pvrChannel.iChannelNumber = channel.Number + channelIndexOffset;
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
    
    return GetChannelListWhenLutsReady().size();
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
            pvrGroup.iPosition = itGroup.first;
            strncpy(pvrGroup.strGroupName, itGroup.second.Name.c_str(), sizeof(pvrGroup.strGroupName));
            PVR->TransferChannelGroup(handle, &pvrGroup);
            LogDebug("Group %d: %s", pvrGroup.iPosition, pvrGroup.strGroupName);
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
    
    m_clientCore->GetPhase(IClientCore::k_EpgCacheLoadingPhase)->Wait();
   
    if(m_kodiToPluginLut.count(channel.iUniqueId) == 0){
        LogError("PVRClientBase: EPG request for unknown channel ID %d", channel.iUniqueId);
        return PVR_ERROR_NO_ERROR;
    }
    
    ChannelId chUniqueId = m_kodiToPluginLut.at(channel.iUniqueId);
    const auto& ch = GetChannelListWhenLutsReady().at(chUniqueId);
    
    IClientCore::EpgEntryAction onEpgEntry = [&channel, &handle, ch](const EpgEntryList::value_type& epgEntry)
    {
        EPG_TAG tag = { 0 };
        tag.iUniqueBroadcastId = epgEntry.first;
        epgEntry.second.FillEpgTag(tag);
        tag.iUniqueChannelId = channel.iUniqueId;//m_pluginToKodiLut.at(itEpgEntry->second.ChannelId);
        tag.startTime += ch.TvgShift;
        tag.endTime += ch.TvgShift;
        PVR->TransferEpgEntry(handle, &tag);
        return true;// always continue enumeration...
    };
    
    m_clientCore->GetEpg(chUniqueId, iStart, iEnd, onEpgEntry);
    
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
    auto phase =  m_clientCore->GetPhase(IClientCore::k_ChannelsLoadingPhase);
    if(phase) {
        phase->Wait();
    }

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
        if(url.empty()) {// no more streams
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
    if (IsTimeshiftEnabled()){
        if(k_TimeshiftBufferFile == TypeOfTimeshiftBuffer()) {
            return new Buffers::FileCacheBuffer(m_cacheDir, TimeshiftBufferSize() /  Buffers::FileCacheBuffer::CHUNK_FILE_SIZE_LIMIT);
        } else {
            return new Buffers::MemoryCacheBuffer(TimeshiftBufferSize() /  Buffers::MemoryCacheBuffer::CHUNK_SIZE_LIMIT);
        }
    }
    else
        return new Buffers::SimpleCyclicBuffer(CacheSizeLimit() / Buffers::SimpleCyclicBuffer::CHUNK_SIZE_LIMIT);

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
        if(!m_inputBuffer->WaitForInput(ChannelReloadTimeout() * 1000)) {
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
        m_destroyerEvent.Broadcast();
    }
    
    m_inputBuffer = nullptr;
}

int PVRClientBase::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    Buffers::TimeshiftBuffer * inputBuffer = nullptr;
    // Do NOT lock stream access for read time (may be long)
    // to enable stream stoping command (hopefully in different Kodi's thread)
    {
        CLockObject lock(m_mutex);
        if(nullptr == m_inputBuffer){
            return -1;
        }
        inputBuffer = m_inputBuffer;
    }

    ChannelId chId = GetLiveChannelId();
    int bytesRead = inputBuffer->Read(pBuffer, iBufferSize, ChannelReloadTimeout() * 1000);
    // Assuming stream hanging.
    // Try to restart current channel only when previous read operation succeeded.
    if (bytesRead != iBufferSize &&  m_lastBytesRead >= 0 && !IsLiveInRecording()) {
        LogError("PVRClientBase:: trying to restart current channel.");
        // Re-accure input stream ptr (may be closed already)
        {
            CLockObject lock(m_mutex);
            if(nullptr == m_inputBuffer || inputBuffer != m_inputBuffer){
                LogInfo("PVRClientBase:: input stream ether changed or closed. Aborting the read operation.");
                return -1;
            }
            inputBuffer = nullptr;
            string  url = m_inputBuffer->GetUrl();
            if(!url.empty()){
                XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32000));
                if(SwitchChannel(chId, url))
                    inputBuffer = m_inputBuffer;
            }
        }

        if(inputBuffer){
            bytesRead = inputBuffer->Read(pBuffer, iBufferSize, ChannelReloadTimeout() * 1000);
        } else {
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

#pragma mark - Recordings

int PVRClientBase::GetRecordingsAmount(bool deleted)
{
    if(NULL == m_clientCore)
        return -1;
    
    if(deleted)
        return -1;
    
    int size = 0;
    
    // When archivee loading is delayed
    // check whether EPG is loded already
    if(LoadArchiveAfterEpg()) {
        auto phase =  m_clientCore->GetPhase(IClientCore::k_EpgLoadingPhase);
         if(!phase) {
             return 0;
         }
        if(!phase->IsDone())
            return 0;
    }
    
    // Add remote archive (if enabled)
    if(IsArchiveSupported()) {
        auto phase =  m_clientCore->GetPhase(IClientCore::k_EpgCacheLoadingPhase);
        if(!phase) {
            return 0;
        }
        phase->Wait();
        size = m_clientCore->UpdateArchiveInfoAndCount();
    }
    // Add local recordings
    if(XBMC->DirectoryExists(RecordingsPath().c_str()))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(RecordingsPath().c_str(), "", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(f.folder)
                    ++size;

            }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of local recordings folder (amount) %s", RecordingsPath().c_str());
        }

    }

    LogDebug("PVRClientBase: found %d recordings. Was %d", size, m_lastRecordingsAmount);

    return size;
    
}

void PVRClientBase::FillRecording(const EpgEntryList::value_type& epgEntry, PVR_RECORDING& tag, const char* dirPrefix)
{
    const auto& epgTag = epgEntry.second;
    
    const Channel& ch = GetChannelListWhenLutsReady().at(epgTag.UniqueChannelId);

    sprintf(tag.strRecordingId, "%d",  epgEntry.first);
    strncpy(tag.strTitle, epgTag.Title.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
    strncpy(tag.strPlot, epgTag.Description.c_str(), PVR_ADDON_DESC_STRING_LENGTH - 1);
    strncpy(tag.strChannelName, ch.Name.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
    tag.recordingTime = epgTag.StartTime;
    tag.iLifetime = 0; /* not implemented */
    
    tag.iDuration = epgTag.EndTime - epgTag.StartTime;
    tag.iEpgEventId = epgEntry.first;
    tag.iChannelUid = m_pluginToKodiLut.at(ch.UniqueId);
    tag.channelType = PVR_RECORDING_CHANNEL_TYPE_TV;
    if(!epgTag.IconPath.empty())
        strncpy(tag.strIconPath, epgTag.IconPath.c_str(), sizeof(tag.strIconPath) -1);
    
    string dirName(dirPrefix);
    dirName += '/';
    if(UseChannelGroupsForArchive()) {
        GroupId groupId = m_clientCore->GroupForChannel(ch.UniqueId);
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
    
    // When archivee loading is delayed
    // check whether EPG is loded already
    if(LoadArchiveAfterEpg()) {
        auto phase =  m_clientCore->GetPhase(IClientCore::k_EpgLoadingPhase);
         if(!phase) {
             return PVR_ERROR_NO_ERROR;
         }
        if(!phase->IsDone())
            return PVR_ERROR_NO_ERROR;
    }
    

    
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    int size = 0;

    std::shared_ptr<IClientCore::IPhase> phase = nullptr;
    LogDebug("PVRClientBase: start recorings transfering...");
    // Add remote archive (if enabled)
    if(IsArchiveSupported()) {

        phase =  m_clientCore->GetPhase(IClientCore::k_EpgCacheLoadingPhase);
        if(!phase) {
            return PVR_ERROR_NO_ERROR;
        }
        phase->Wait();
        
        phase =  m_clientCore->GetPhase(IClientCore::k_EpgLoadingPhase);
        const bool  isFastEpgLoopAvailable = !phase->IsDone();

        // Populate server recordings
        IClientCore::EpgEntryAction predicate = [] (const EpgEntryList::value_type& epgEntry) {
            return epgEntry.second.HasArchive;
        };
        std::chrono::duration<float> fillTotal(0.0f);
        std::chrono::duration<float> transferTotal(0.0f);
        auto pThis = this;
        IClientCore::EpgEntryAction action = [pThis ,&result, &handle, &size, &predicate, isFastEpgLoopAvailable, &fillTotal, &transferTotal]
        (const EpgEntryList::value_type& epgEntry) {
            try {
                // Optimisation: for first time we'll call ForEachEpgLocked()
                //  Check predicate in this case
                if(isFastEpgLoopAvailable) {
                    if(!predicate(epgEntry))
                        return true;
                }
                
                PVR_RECORDING tag = { 0 };
                
//                auto startAt = std::chrono::system_clock::now();
                
                pThis->FillRecording(epgEntry, tag, s_RemoteRecPrefix.c_str());
                
//                auto endAt = std::chrono::system_clock::now();
//                fillTotal += endAt-startAt;
//                startAt = endAt;

                PVR->TransferRecordingEntry(handle, &tag);

//                endAt = std::chrono::system_clock::now();
//                transferTotal += endAt-startAt;

                ++size;
                return true;
            }
            catch (std::exception& ex)  {
                LogError( "GetRecordings::action() failed. Exception: %s.", ex.what());
                //result = PVR_ERROR_FAILED;
            }
            catch (...)  {
                LogError( "GetRecordings::action() failed. Unknown exception.");
                //result = PVR_ERROR_FAILED;
            }
            // Looks like we can lost problematic recored
            // and continue EPG enumeration.
            return true;
        };
        if(isFastEpgLoopAvailable){
            m_clientCore->ForEachEpgLocked(action);
        } else {
            m_clientCore->ForEachEpgUnlocked(predicate, action);
        }
//        float preRec = 1000*fillTotal.count()/size;
//        LogDebug("PVRClientBase: FillRecording = %0.4f (%0.6f per kRec)", fillTotal.count(), preRec);
//        preRec = 1000*transferTotal.count()/size;
//        LogDebug("PVRClientBase: TransferRecordingEntry = %0.4f (%0.6f per kRec)", transferTotal.count(), preRec );
//        LogDebug("PVRClientBase: All Recording = %0.4f", transferTotal.count() + fillTotal.count() );
    }
    // Add local recordings
    if(XBMC->DirectoryExists(RecordingsPath().c_str()))
    {
        VFSDirEntry* files;
        unsigned int num_files;
        if(XBMC->GetDirectory(RecordingsPath().c_str(), "", &files, &num_files)) {
            VFSDirEntry_Patch* patched_files = (VFSDirEntry_Patch*) files;
            for (int i = 0; i < num_files; ++i) {
                const VFSDirEntry_Patch& f = patched_files[i];
                if(!f.folder)
                    continue;
                std::string infoPath = f.path;
                if(infoPath[infoPath.length() - 1] != PATH_SEPARATOR_CHAR) {
                    infoPath += PATH_SEPARATOR_CHAR;
                }
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
                ++size;
           }
            XBMC->FreeDirectory(files, num_files);
        } else {
            LogError( "Failed obtain content of local recordings folder %s", RecordingsPath().c_str());
        }
        
    }
    LogDebug("PVRClientBase: done transfering of %d recorings.", size);
    m_lastRecordingsAmount = size;
    
    phase = m_clientCore->GetPhase(IClientCore::k_RecordingsInitialLoadingPhase);
    if(nullptr == phase) {
        return PVR_ERROR_FAILED;
    }
    if(!phase->IsDone()) {
        phase->Broadcast();
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

    
    if(XBMC->DirectoryExists(RecordingsPath().c_str()))
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
            LogError( "Failed obtain content of local recordings folder (delete) %s", RecordingsPath().c_str());
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
        m_recordBuffer.isLocal = true;
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
        m_recordBuffer.isLocal = false;
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
    return (m_recordBuffer.buffer == NULL) ? -1 : m_recordBuffer.buffer->Read(pBuffer, iBufferSize, (m_recordBuffer.isLocal)? 0 : ChannelReloadTimeout() * 1000);
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
    std::string recordingDir(RecordingsPath());
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
    m_clientCore->ForEachEpgLocked(action);
    
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
    
    KodiChannelId kodiChannelId = timer.iClientChannelUid;
    if(m_kodiToPluginLut.count(kodiChannelId) == 0){
        LogError("StartRecordingFor(): recording request for unknown channel ID %d", kodiChannelId);
        return false;
    }
    
    ChannelId channelId = m_kodiToPluginLut.at(kodiChannelId);
    
    std::string url = m_clientCore ->GetUrl(channelId);
    m_localRecordChannelId = channelId;
    // When recording channel is same to live channel
    // merge live buffer with local recording
    if(m_liveChannelId == channelId){
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
    
    KodiChannelId kodiChannelId = timer.iClientChannelUid;
    ChannelId channelId = UnknownChannelId;
    if(m_kodiToPluginLut.count(kodiChannelId) != 0){
       channelId = m_kodiToPluginLut.at(kodiChannelId);
    } else {
        LogError("StopRecordingFor(): recording request for unknown channel ID %d", kodiChannelId);
    }
    

    // When recording channel is same to live channel
    // merge live buffer with local recording
    if(m_liveChannelId == channelId){
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

#pragma mark - Settings
const std::string& PVRClientBase::TimeshiftPath() const
{
    return m_addonSettings.GetString(c_timeshiftPath);
}

const std::string& PVRClientBase::RecordingsPath() const
{
    return m_addonSettings.GetString(c_recordingPath);
}

int PVRClientBase::RpcLocalPort() const
{
    return m_addonSettings.GetInt(c_rpcLocalPort);
}

int PVRClientBase::ChannelIndexOffset() const
{
    return m_addonSettings.GetInt(c_channelIndexOffset);
}

AddCurrentEpgToArchive PVRClientBase::HowToAddCurrentEpgToArchive() const
{
    return (AddCurrentEpgToArchive) m_addonSettings.GetInt(c_addCurrentEpgToArchive);
}

bool PVRClientBase::UseChannelGroupsForArchive() const
{
    return m_addonSettings.GetBool(c_useChannelGroupsForArchive);
}

int PVRClientBase::WaitForInetTimeout() const
{
    return m_addonSettings.GetInt(c_waitForInternetTimeout);
}

int PVRClientBase::StartupDelay() const
{
    return m_addonSettings.GetInt(c_startupDelay);
}

uint32_t PVRClientBase::StartRecordingPadding() const
{
    int padding = m_addonSettings.GetInt(c_startRecordingPadding);
    if(padding < 0)
        padding = 0;
    return padding *= 60;
}

uint32_t PVRClientBase::EndRecordingPadding() const
{
    int padding = m_addonSettings.GetInt(c_endRecordingPadding);
    if(padding < 0)
        padding = 0;
    return padding *= 60;
}

bool PVRClientBase::IsArchiveSupported() const
{
    return m_addonSettings.GetBool(c_supportArchive);
}

bool PVRClientBase::LoadArchiveAfterEpg() const
{
    return m_addonSettings.GetBool(c_loadArchiveAfterEpg);
}

uint PVRClientBase::ArchiveRefreshInterval() const
{
    int interval = m_addonSettings.GetInt(c_archiveRefreshInterval);
    if(interval < 0)
        interval = 3;
    return interval;
}

bool PVRClientBase::IsTimeshiftEnabled() const
{
    return m_addonSettings.GetBool(c_enableTimeshift);
}

int PVRClientBase::ChannelReloadTimeout() const
{
    return m_addonSettings.GetInt(c_channelReloadTimeout);
}

uint64_t PVRClientBase::CacheSizeLimit() const
{
    return m_addonSettings.GetInt(c_cacheSizeLimit) * 1024 * 1204;
}

uint64_t PVRClientBase::TimeshiftBufferSize() const
{
    return m_addonSettings.GetInt(c_timeshiftSize) * 1024 * 1204;
}

PVRClientBase::TimeshiftBufferType PVRClientBase::TypeOfTimeshiftBuffer() const
{
    return  (TimeshiftBufferType) m_addonSettings.GetInt(c_timeshiftType);
    
}
