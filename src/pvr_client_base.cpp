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

#include <stdio.h>
#include <algorithm>
#include <chrono>
#include <list>
#include <map>
#include "kodi/Filesystem.h"
#include "kodi/General.h"
#include "XMLTV_loader.hpp"

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
using namespace Buffers;
using namespace PvrClient;
using namespace Globals;
using namespace P8PLATFORM;
using namespace ActionQueue;
using namespace Helpers;

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
    kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32027).c_str(), delayInSec);
    P8PLATFORM::CEvent::Sleep(delayInSec * 1000);
}

static int CheckForInetConnection(int waitForInetTimeout)
{
    int waitingTimeInSec = 0;
    if(waitForInetTimeout > 0){
        kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32022).c_str());
        
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
            kodi::QueueFormattedNotification(QUEUE_ERROR, kodi::GetLocalizedString(32023).c_str());
        }
        waitingTimeInSec = waitForInetTimeout - waitForInet.TimeLeft()/1000;
    }
    return waitingTimeInSec;
}

static void CleanupTimeshiftDirectory(const std::string& path){
    
    if(!kodi::vfs::DirectoryExists(path))
        if(!kodi::vfs::CreateDirectory(path))
            LogError( "Failed to create timeshift folder %s .", path.c_str());
    // Cleanup chache
    if(kodi::vfs::DirectoryExists(path))
    {
        std::vector<kodi::vfs::CDirEntry> files;
        if(kodi::vfs::GetDirectory(path, "*.bin", files)) {
            for (const auto& f : files) {
                if(!f.IsFolder()){
                    if(!kodi::vfs::DeleteFile(f.Path())){
                        LogError( "Failed to delete timeshift folder entry %s", f.Path().c_str());
                    }
                }
            }
        } else {
            LogError( "Failed obtain content of timeshift folder %s", path.c_str());
        }
    }
}

static void CheckRecordingsPath(const std::string& path){
    if(!kodi::vfs::DirectoryExists(path))
        if(!kodi::vfs::CreateDirectory(path))
            LogError( "Failed to create recordings folder %s .", path.c_str());
}

static bool IsHlsUrl(const std::string& url)
{
    const std::string m3uExt = ".m3u";
    const std::string m3u8Ext = ".m3u8";
    return url.find(m3u8Ext) != std::string::npos || url.find(m3uExt) != std::string::npos;
}
static bool IsMulticastUrl(const std::string& url)
{
    const std::string rdpPattern = "/rdp/";
    const std::string udpPattern = "/udp/";
    return url.find(rdpPattern) != std::string::npos || url.find(udpPattern) != std::string::npos;
}

PVRClientBase::PVRClientBase()
    : m_addonSettings(m_addonMutableSettings)
{
    
}

ADDON_STATUS PVRClientBase::Init(const std::string& clientPath, const std::string& userPath)
{
    m_clientCore = NULL;
    m_inputBuffer = NULL;
    m_recordBuffer.buffer = NULL;
    m_recordBuffer.duration = 0;
    m_recordBuffer.isLocal = false;
    m_recordBuffer.seekToSec = 0;
    m_localRecordBuffer = NULL;
    m_supportSeek = false;
    
    m_clientPath = clientPath;
    m_userPath = userPath;
    LogDebug( "User path: %s", m_userPath.c_str());
    LogDebug( "Client path: %s", m_clientPath.c_str());
    //auto g_strUserPath   = pvrprops->strUserPath;

    InitSettings();
    
    DelayStartup(StartupDelay() - CheckForInetConnection(WaitForInetTimeout()));
    
    RegisterCommonMenuHook(RELOAD_EPG_MENU_HOOK, 32050);
    RegisterCommonMenuHook(RELOAD_RECORDINGS_MENU_HOOK, 32051);

    // Local recordings path prefix
    s_LocalRecPrefix = kodi::GetLocalizedString(32014);
    // Remote recordings path prefix
    s_RemoteRecPrefix = kodi::GetLocalizedString(32015);
    
    m_liveChannelId =  m_localRecordChannelId = UnknownChannelId;
    m_lastBytesRead = c_InitialLastByteRead;
    m_lastRecordingsAmount = 0;
    
    m_destroyer = new CActionQueue(100, "Streams Destroyer");
    m_destroyer->CreateThread();
    
    return ADDON_STATUS_OK;
    
}

void PVRClientBase::OnCoreCreated() {
    IClientCore::RpcSettings rpc;
    rpc.port = RpcLocalPort();
    rpc.user = RpcUser();
    rpc.password = RpcPassword();
    rpc.is_secure = RpcEnableSsl();
    
    m_clientCore->SetRpcSettings(rpc);
    m_clientCore->SupportMuticastUrls(SuppotMulticastUrls(), UdpProxyHost(), UdpProxyPort());
    m_clientCore->CheckRpcConnection();

    // We may be here when core is re-creating
    // In this case Destroyer is running and may be busy
    // Validate that this criticlal Init action is started
    bool isAsyncInitStarted = false;
    m_destroyer->PerformAsync([&isAsyncInitStarted, this](){
        isAsyncInitStarted = true;
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
            while(!phase->Wait(100))
            {
                //timeout. Check for termination...
                if(m_destroyer->IsStopped())
                    return;
            }
            if(nullptr == m_clientCore) {
                // May happend on destruction...
                // TODO: Stop m_destroyer before destruction.
                return;
            }
            if(LoadArchiveAfterEpg()) {
                LogDebug("PVRClientBase: update recorderings.");
                m_clientCore->ReloadRecordings();
            }
            LogDebug("PVRClientBase: scheduling first recording update.");
            ScheduleRecordingsUpdate();
        }
    });
    while(!isAsyncInitStarted) {
        m_destroyerEvent.Broadcast();
        m_destroyerEvent.Wait(100);
    }
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
             LogDebug("PVRClientBase: scheduling next recording update.");
             ScheduleRecordingsUpdate();
         }else {
             LogDebug("PVRClientBase: scheduling of recording update canceled.");
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
    //DestroyCoreSafe();
}
void PVRClientBase::OnSystemWake()
{
    DelayStartup(StartupDelay() - CheckForInetConnection(WaitForInetTimeout()));
    //CreateCoreSafe(false);
}

ADDON_STATUS PVRClientBase::SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue)
{
    try {
        return m_addonSettings.Set(settingName, settingValue);
    }
    catch (std::exception& ex) {
        LogInfo("Error on settings update: %s", ex.what());
    }
    
    return ADDON_STATUS_OK;
}

PVR_ERROR PVRClientBase::GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
    //pCapabilities->bSupportsEPG = true;
    capabilities.SetSupportsTV(true);
    //capabilitiesbSupportsRadio(true);
    //pCapabilities->bSupportsChannelGroups(true);
    //pCapabilities->bHandlesInputStream(true);
    capabilities.SetSupportsRecordings(true); //For local recordings
    capabilities.SetSupportsTimers(true);

//    pCapabilities->bSupportsChannelScan(false);
//    pCapabilities->bHandlesDemuxing)false);
//    pCapabilities->bSupportsRecordingPlayCount(false);
//    pCapabilities->bSupportsLastPlayedPosition(false);
//    pCapabilities->bSupportsRecordingEdl(false);

    // Kodi 18
    capabilities.SetSupportsRecordingsRename(false);
    capabilities.SetSupportsRecordingsLifetimeChange(false);
    capabilities.SetSupportsDescrambleInfo(false);

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

PVR_ERROR PVRClientBase::GetStreamTimes(kodi::addon::PVRStreamTimes& times)
{

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
    const int64_t DVD_TIME_BASE = 1000*1000; // to micro seconds factor
    times.SetStartTime(timeStart);
    times.SetPTSStart(0);
    times.SetPTSBegin(0);
    times.SetPTSEnd((timeEnd - timeStart) * DVD_TIME_BASE);
    return PVR_ERROR_NO_ERROR;
}

ADDON_STATUS PVRClientBase::GetStatus()
{
    return  /*(m_sovokTV == NULL)? ADDON_STATUS_LOST_CONNECTION : */ADDON_STATUS_OK;
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

PVR_ERROR PVRClientBase::SignalStatus(int /*channelUid*/, kodi::addon::PVRSignalStatus& signalStatus)
{
    if(nullptr != m_inputBuffer) {
        signalStatus.SetSignal(m_inputBuffer->FillingRatio() * 	0xFFFF);
//        signalStatus.iSNR = m_inputBuffer->GetSpeedRatio() * 0xFFFF;
    }
    return PVR_ERROR_NO_ERROR;
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

PVR_ERROR PVRClientBase::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
        
    const int channelIndexOffset = ChannelIndexOffset();
    for(auto& itChannel : GetChannelListWhenLutsReady())
    {
        auto & channel = itChannel.second;
        if (radio == channel.IsRadio)
        {
            kodi::addon::PVRChannel pvrChannel;
            pvrChannel.SetUniqueId (m_pluginToKodiLut.at(channel.UniqueId));
            pvrChannel.SetChannelNumber(channel.Number + channelIndexOffset);
            pvrChannel.SetIsRadio(channel.IsRadio);
            pvrChannel.SetChannelName(channel.Name);
            pvrChannel.SetIconPath(channel.IconPath);
            
            LogDebug("==> CH %-5d - %-40s", pvrChannel.GetChannelNumber(), channel.Name.c_str());
            
            results.Add(pvrChannel);
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

PVR_ERROR PVRClientBase::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& result)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    if (!radio)
    {
        kodi::addon::PVRChannelGroup pvrGroup;
        pvrGroup.SetIsRadio(false);
        for (auto& itGroup : m_clientCore->GetGroupList())
        {
            pvrGroup.SetPosition(itGroup.first);
            pvrGroup.SetGroupName(itGroup.second.Name);
            result.Add(pvrGroup);
            LogDebug("Group %d: %s", pvrGroup.GetPosition(), pvrGroup.GetGroupName().c_str());
        }
    }
    
    return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRClientBase::GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group, kodi::addon::PVRChannelGroupMembersResultSet& results)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    auto& groups = m_clientCore->GetGroupList();
    auto groupName = group.GetGroupName();
    auto itGroup =  std::find_if(groups.begin(), groups.end(), [&groupName](const GroupList::value_type& v ){
        return groupName == v.second.Name;
    });
    if (itGroup != groups.end())
    {
        for (auto it : itGroup->second.Channels)
        {
            kodi::addon::PVRChannelGroupMember pvrGroupMember;
            pvrGroupMember.SetGroupName(itGroup->second.Name);
            pvrGroupMember.SetChannelUniqueId(m_pluginToKodiLut.at(it.second));
            pvrGroupMember.SetChannelNumber(it.first);
            results.Add(pvrGroupMember);
        }
    }
   
    return PVR_ERROR_NO_ERROR;
}

#pragma mark - EPG

PVR_ERROR PVRClientBase::GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results)
{
    if(NULL == m_clientCore)
        return PVR_ERROR_SERVER_ERROR;
    
    m_clientCore->GetPhase(IClientCore::k_EpgCacheLoadingPhase)->Wait();
   
    if(m_kodiToPluginLut.count(channelUid) == 0){
        LogError("PVRClientBase: EPG request for unknown channel ID %d", channelUid);
        return PVR_ERROR_NO_ERROR;
    }
    
    ChannelId chUniqueId = m_kodiToPluginLut.at(channelUid);
    const auto& ch = GetChannelListWhenLutsReady().at(chUniqueId);
    
    IClientCore::EpgEntryAction onEpgEntry = [&channelUid, &results, ch](const EpgEntryList::value_type& epgEntry)
    {
        kodi::addon::PVREPGTag tag;
        tag.SetUniqueBroadcastId(epgEntry.first);
        epgEntry.second.FillEpgTag(tag);
        tag.SetUniqueChannelId(channelUid);//m_pluginToKodiLut.at(itEpgEntry->second.ChannelId);
        tag.SetStartTime(tag.GetStartTime() + ch.TvgShift);
        tag.SetEndTime(tag.GetEndTime() + ch.TvgShift);
        results.Add(tag);
        return true;// always continue enumeration...
    };
    
    m_clientCore->GetEpg(chUniqueId, start, end, onEpgEntry);
    
    return PVR_ERROR_NO_ERROR;
}

#pragma mark - Streams

std::string PVRClientBase::GetLiveUrl() const {
    return (m_inputBuffer) ? m_inputBuffer->GetUrl() : std::string();
    
}

InputBuffer*  PVRClientBase::BufferForUrl(const std::string& url )
{
    InputBuffer* buffer = NULL;
    if(IsHlsUrl(url))
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

bool PVRClientBase::OpenLiveStream(const kodi::addon::PVRChannel& channel)
{
    auto phase =  m_clientCore->GetPhase(IClientCore::k_ChannelsLoadingPhase);
    if(phase) {
        phase->Wait();
    }

    const auto channelId =  channel.GetUniqueId();
    if(m_kodiToPluginLut.count(channelId) == 0){
        LogError("PVRClientBase: open stream request for unknown channel ID %d", channelId);
        return false;
    }
    
    m_lastBytesRead = c_InitialLastByteRead;
    const ChannelId chId = m_kodiToPluginLut.at(channelId);
    bool succeeded = OpenLiveStream(chId, GetStreamUrl(chId));
    bool tryToRecover = !succeeded;
    while(tryToRecover) {
        string url = GetNextStreamUrl(chId);
        if(url.empty()) {// no more streams
            LogDebug("No alternative stream found.");
            kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32026).c_str());
            break;
        }
        kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32025).c_str());
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
       
        Buffers::TimeshiftBuffer* inputBuffer = new Buffers::TimeshiftBuffer(buffer, CreateLiveCache());
        
        // Wait for first data from live stream
        auto startAt = std::chrono::system_clock::now();
        if(!inputBuffer->WaitForInput(ChannelReloadTimeout() * 1000)) {
            throw InputBufferException("no data available diring reload timeout (bad ace link?)");
        }
        auto endAt = std::chrono::system_clock::now();
        std::chrono::duration<float> validationDelay(endAt - startAt);
        
        // Wait preloading delay (from settings or playlist)
        const auto& ch = GetChannelListWhenLutsReady().at(channelId);

        int liveDelayValue = ch.PreloadingInterval;
        if(0 == liveDelayValue) {
            if(IsHlsUrl(url))
                liveDelayValue = LivePlaybackDelayForHls();
            else if(IsMulticastUrl(url))
                liveDelayValue = LivePlaybackDelayForMulticast();
            else
                liveDelayValue = LivePlaybackDelayForTs();
        }
        std::chrono::duration<float> livePreloadingDelay(liveDelayValue);
        auto resultDelay = livePreloadingDelay - validationDelay;
        if(resultDelay > std::chrono::seconds(0)) {
            int delaySeconds = (int)(resultDelay.count() + 0.5);
            while(delaySeconds-- && inputBuffer->FillingRatio() < 0.95) {
                LogDebug("Live preloading: left %d seconds. Buffer filling ratio %.3f", delaySeconds, inputBuffer->FillingRatio());
                CEvent::Sleep(1000);
            }
        }
        // Minimize lock time
        {
            CLockObject lock(m_mutex);
            m_inputBuffer = inputBuffer;
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
    // Try to restart current channel only when previous read operation succeeded
    // and current stream was NOT stopped
    if (bytesRead != iBufferSize &&  m_lastBytesRead >= 0 && !IsLiveInRecording() && !inputBuffer->IsStopped()) {
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
                kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32000).c_str());
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

int64_t PVRClientBase::SeekLiveStream(int64_t iPosition, int iWhence)
{
    CLockObject lock(m_mutex);
    if(nullptr == m_inputBuffer){
        return -1;
    }
    return m_inputBuffer->Seek(iPosition, iWhence);
}

int64_t PVRClientBase::PositionLiveStream()
{
    CLockObject lock(m_mutex);
    if(nullptr == m_inputBuffer){
        return -1;
    }
    return m_inputBuffer->GetPosition();
}

int64_t PVRClientBase::LengthLiveStream()
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

PVR_ERROR PVRClientBase::GetRecordingsAmount(bool deleted, int& amount)
{
    if(NULL == m_clientCore){
        amount = -1;
        return PVR_ERROR_SERVER_ERROR;
    }
    
    if(deleted) {
        amount = -1;
        return PVR_ERROR_NOT_IMPLEMENTED;
    }
    
    int size = 0;
    
    // When archivee loading is delayed
    // check whether EPG is loded already
    if(LoadArchiveAfterEpg()) {
        auto phase =  m_clientCore->GetPhase(IClientCore::k_EpgLoadingPhase);
         if(!phase) {
             amount = 0;
             return PVR_ERROR_NO_ERROR;
         }
        
        if(!phase->IsDone()){
            amount = 0;
            return PVR_ERROR_NO_ERROR;
        }
    }
    
    // Add remote archive (if enabled)
    if(IsArchiveSupported()) {
        auto phase =  m_clientCore->GetPhase(IClientCore::k_EpgCacheLoadingPhase);
        if(!phase) {
            amount = 0;
            return PVR_ERROR_NO_ERROR;
        }
        phase->Wait();
        size = m_clientCore->UpdateArchiveInfoAndCount();
    }
    // Add local recordings
    if(kodi::vfs::DirectoryExists(RecordingsPath()))
    {
        std::vector<kodi::vfs::CDirEntry> files;
        if(kodi::vfs::GetDirectory(RecordingsPath().c_str(), "", files)) {
            for (const auto& f : files) {
                if(f.IsFolder())
                    ++size;
            }
        } else {
            LogError( "Failed obtain content of local recordings folder (amount) %s", RecordingsPath().c_str());
        }

    }

    LogDebug("PVRClientBase: found %d recordings. Was %d", size, m_lastRecordingsAmount);

    amount = size;
    return PVR_ERROR_NO_ERROR;
}

void PVRClientBase::FillRecording(const EpgEntryList::value_type& epgEntry, kodi::addon::PVRRecording& tag, const char* dirPrefix)
{
    const auto& epgTag = epgEntry.second;
    
    const Channel& ch = GetChannelListWhenLutsReady().at(epgTag.UniqueChannelId);

    tag.SetRecordingId(to_string(epgEntry.first));
    tag.SetTitle(epgTag.Title);
    tag.SetPlot(epgTag.Description);
    tag.SetChannelName(ch.Name);
    tag.SetRecordingTime(epgTag.StartTime);
    tag.SetLifetime(0); /* not implemented */
    
    tag.SetDuration(epgTag.EndTime - epgTag.StartTime);
    tag.SetEPGEventId(epgEntry.first);
    tag.SetChannelUid(m_pluginToKodiLut.at(ch.UniqueId));
    tag.SetChannelType(PVR_RECORDING_CHANNEL_TYPE_TV);
    if(!epgTag.IconPath.empty())
        tag.SetIconPath(epgTag.IconPath);
    
    string dirName(dirPrefix);
    dirName += '/';
    if(UseChannelGroupsForArchive()) {
        GroupId groupId = m_clientCore->GroupForChannel(ch.UniqueId);
        dirName += (-1 == groupId) ? "---" : m_clientCore->GetGroupList().at(groupId).Name.c_str();
        dirName += '/';
    }
    dirName += tag.GetChannelName();
    char buff[20];
    time_t startTime = epgTag.StartTime;
    strftime(buff, sizeof(buff), "/%d-%m-%y", localtime(&startTime));
    dirName += buff;
    tag.SetDirectory(dirName);

}

PVR_ERROR PVRClientBase::GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results)
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
        IClientCore::EpgEntryAction action = [pThis ,&result, &results, &size, &predicate, isFastEpgLoopAvailable, &fillTotal, &transferTotal]
        (const EpgEntryList::value_type& epgEntry) {
            try {
                // Optimisation: for first time we'll call ForEachEpgLocked()
                //  Check predicate in this case
                if(isFastEpgLoopAvailable) {
                    if(!predicate(epgEntry))
                        return true;
                }
                
                kodi::addon::PVRRecording tag;
                
//                auto startAt = std::chrono::system_clock::now();
                
                pThis->FillRecording(epgEntry, tag, s_RemoteRecPrefix.c_str());
                
//                auto endAt = std::chrono::system_clock::now();
//                fillTotal += endAt-startAt;
//                startAt = endAt;

                results.Add(tag);

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
    if(kodi::vfs::DirectoryExists(RecordingsPath().c_str()))
    {
        std::vector<kodi::vfs::CDirEntry> files;
        if(kodi::vfs::GetDirectory(RecordingsPath(), "*.bin", files)) {
            for (const auto& f : files) {
                if(!f.IsFolder()){
                    continue;
                }
                std::string infoPath = f.Path();
                if(infoPath[infoPath.length() - 1] != PATH_SEPARATOR_CHAR) {
                    infoPath += PATH_SEPARATOR_CHAR;
                }
                infoPath += "recording.inf";
                kodi::vfs::CFile* infoFile = XBMC_OpenFile(infoPath);
                if(nullptr == infoFile)
                    continue;
                PVR_RECORDING tag = { 0 };
                bool isValid = infoFile->Read( &tag, sizeof(tag)) == sizeof(tag);
                infoFile->Close();
                delete infoFile;
                if(!isValid)
                    continue;
                kodi::addon::PVRRecording data;
                *(PVR_RECORDING*)data = tag;
                results.Add(data);
                ++size;
           }
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

PVR_ERROR PVRClientBase::DeleteRecording(const kodi::addon::PVRRecording& recording)
{
    
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    // Is recording local?
    if(!IsLocalRecording(recording))
        return PVR_ERROR_REJECTED;
    std::string dir = DirectoryForRecording(stoul(recording.GetRecordingId()));
    if(!kodi::vfs::DirectoryExists(dir))
        return PVR_ERROR_INVALID_PARAMETERS;

    if(kodi::vfs::DirectoryExists(RecordingsPath()))
    {
        std::vector<kodi::vfs::CDirEntry> files;
        if(kodi::vfs::GetDirectory(dir, "", files)) {
            for (const auto& f : files) {
                if(f.IsFolder()){
                    continue;
                }
                if(!kodi::vfs::DeleteFile(f.Path()))
                    return PVR_ERROR_FAILED;
            }
        } else {
            LogError( "Failed obtain content of local recordings folder (delete) %s", RecordingsPath().c_str());
        }
        
    }

    kodi::vfs::RemoveDirectory(dir);
    PVR->Addon_TriggerRecordingUpdate();
    
    return PVR_ERROR_NO_ERROR;
}

bool PVRClientBase::IsLiveInRecording() const
{
    return m_inputBuffer == m_localRecordBuffer;
}


bool PVRClientBase::IsLocalRecording(const kodi::addon::PVRRecording& recording) const
{
    return StringUtils::StartsWith(recording.GetDirectory(), s_LocalRecPrefix);
}

bool PVRClientBase::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
    if(!IsLocalRecording(recording))
        return false;
    try {
        InputBuffer* buffer = new DirectBuffer(new FileCacheBuffer(DirectoryForRecording(stoul(recording.GetRecordingId().c_str()))));
        
        if(m_recordBuffer.buffer)
            SAFE_DELETE(m_recordBuffer.buffer);
        m_recordBuffer.buffer = buffer;
        m_recordBuffer.duration = recording.GetDuration();
        m_recordBuffer.isLocal = true;
        m_recordBuffer.seekToSec = 0;
    } catch (std::exception ex) {
        LogError("OpenRecordedStream (local) exception: %s", ex.what());
    }
    
    return true;
}
    
bool PVRClientBase::OpenRecordedStream(const std::string& url, Buffers::IPlaylistBufferDelegate* delegate, RecordingStreamFlags flags)
{
     if (url.empty())
        return false;
    
    try
    {
        InputBuffer* buffer = NULL;
        
        const bool enforcePlaylist = (flags & ForcePlaylist) == ForcePlaylist;
        const bool isM3u = enforcePlaylist || IsHlsUrl(url);
        const bool seekForVod = (flags & SupportVodSeek) == SupportVodSeek;
        Buffers::PlaylistBufferDelegate plistDelegate(delegate);
        if(isM3u)
            buffer = new Buffers::PlaylistBuffer(url, plistDelegate, seekForVod);
        else
            buffer = new ArchiveBuffer(url);

        m_recordBuffer.buffer = buffer;
        m_recordBuffer.duration = (delegate) ? delegate->Duration() : 0;
        m_recordBuffer.isLocal = false;
        m_recordBuffer.seekToSec = (SeekArchivePadding() && (!isM3u || seekForVod) ) ?  StartRecordingPadding() : 0;
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

PVR_ERROR PVRClientBase::GetStreamReadChunkSize(int& chunksize) {
    // TODO: obtain from buffer...
    chunksize = 32 * 1024; // 32K chunk.
    return PVR_ERROR_NO_ERROR;
}

static bool IsPlayerItemSameTo(rapidjson::Document& jsonRoot, const std::string& recordingName)
{
    LogDebug("PVRClientBase: JSON Player.GetItem commend response:");
    dump_json(jsonRoot);

    if(!jsonRoot.IsObject()) {
        LogError("PVRClientBase: wrong JSON format of Player.GetItem response.");
        return false;
    }
    if(!jsonRoot.HasMember("result")) {
        LogError("PVRClientBase: missing 'result' in Player.GetItem response.");
        return false;
    }
    auto& r = jsonRoot["result"];
    if(!r.IsObject() || !r.HasMember("item")){
        LogError("PVRClientBase: missing 'item' in Player.GetItem response.");
        return false;
    }
    auto& i = r["item"];
    if(!i.IsObject() || !i.HasMember("label")){
        LogError("PVRClientBase: missing 'item.label' in Player.GetItem response.");
        return false;
    }

    if(recordingName != i["label"].GetString() ) {
        LogDebug("PVRClientBase: waiting for Kodi's player becomes playing %s ...", recordingName.c_str());
        return false;
    }
    return true;

}
static float GetPlayedSeconds(rapidjson::Document& jsonRoot, float& total)
{
    if(!jsonRoot.IsObject()) {
        LogError("PVRClientBase: wrong JSON format of Player.GetProperties.Time response.");
        throw RpcCallException("Wrong JSON-RPC response format.");
    }
    if(!jsonRoot.HasMember("result")) {
        LogError("PVRClientBase: missing 'result' in Player.GetProperties.Time response.");
        throw RpcCallException("Wrong JSON-RPC response format.");
    }
    auto& r = jsonRoot["result"];
    if(!r.IsObject() || !r.HasMember("time") || !r.HasMember("totaltime")){
        LogError("PVRClientBase: missing 'time' in Player.GetProperties.Time response.");
        throw RpcCallException("Wrong JSON-RPC response format.");
    }
    auto& tt = r["totaltime"];
    total = tt["milliseconds"].GetInt()/1000.0f + tt["seconds"].GetInt() + tt["minutes"].GetInt() * 60 + tt["hours"].GetInt() * 60  * 60;
    auto& t = r["time"];
    return t["milliseconds"].GetInt()/1000.0f + t["seconds"].GetInt() + t["minutes"].GetInt() * 60 + t["hours"].GetInt() * 60  * 60;
    
}

void PVRClientBase::SeekKodiPlayerAsyncToOffset(int offsetInSeconds, std::function<void(bool done)> result)
{
    
    auto pThis = this;
    // m_clientCore->CallRpcAsync(R"({"jsonrpc": "2.0", "method": "Player.GetItem", "params": {"playerid":1},"id": 1})",
    m_clientCore->CallRpcAsync(R"({"jsonrpc": "2.0", "method": "Player.GetProperties", "params": {"playerid":1, "properties":["totaltime", "time"]},"id": 1})",
                               [offsetInSeconds, pThis, result] (rapidjson::Document& jsonRoot) {
        dump_json(jsonRoot);
        
        float totalTime = 0.0;
        float playedSeconds  = GetPlayedSeconds(jsonRoot, totalTime);
        
        if(totalTime < 0.01 || playedSeconds < 0.01) {
            LogDebug("PVRClientBase: waiting for Kodi's player becomes started...");
            throw RpcCallException("Waiting for Kodi's player becomes started.");
        }
        if(offsetInSeconds <= playedSeconds) {
            LogDebug("PVRClientBase: Kodi's player position (%d) is after the offset (%d).", playedSeconds, offsetInSeconds);
            return;
        }
        //        {"jsonrpc":"2.0", "method":"Player.Seek", "params": { "playerid":1, "value":{ "seconds": 30 } }, "id":1}
        std::string rpcCommand(R"({"jsonrpc": "2.0", "method": "Player.Seek", "params": {"playerid":1, "value":{ "time": {)");
        rpcCommand+= R"("hours":)";
        rpcCommand+= std::to_string(offsetInSeconds / 3600);
        rpcCommand+= R"(, "minutes":)";
        rpcCommand+= std::to_string((offsetInSeconds % 3600) / 60);
        rpcCommand+= R"(, "seconds":)";
        rpcCommand+= std::to_string((offsetInSeconds % 3600) %	 60);
        rpcCommand += R"(, "milliseconds":0 }}},"id": 1})";
        pThis->m_clientCore->CallRpcAsync(rpcCommand, [result] (rapidjson::Document& jsonRoot) {
            LogDebug("PVRClientBase: JSON seek commend response:");
            dump_json(jsonRoot);
        }, [result](const ActionQueue::ActionResult& s) {
            //result(true);
            if(s.status == kActionFailed) {
                LogError("PVRClientBase: JSON seek command failed");
            } else {
                LogDebug("PVRClientBase: JSON seek command succeeded.");
            }
        });
        LogDebug("PVRClientBase: sent JSON commad to seek to %d sec offset. Played seconds %f", offsetInSeconds, playedSeconds);
    },[result](const ActionQueue::ActionResult& s) {
        if(s.status == kActionFailed) {
            result(false);
        } else {
            result(true);
        }
    });
}

int PVRClientBase::ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
    //uint32_t timeoutMs = 5000;
    if(m_recordBuffer.seekToSec > 0){
        auto offset = m_recordBuffer.seekToSec;
        auto pThis = this;
        m_recordBuffer.seekToSec = 0;
        SeekKodiPlayerAsyncToOffset(offset, [offset, pThis] (bool done) {
            if(!done)
                pThis->m_recordBuffer.seekToSec = offset;
        } );
    }
    return (m_recordBuffer.buffer == NULL) ? -1 : m_recordBuffer.buffer->Read(pBuffer, iBufferSize, (m_recordBuffer.isLocal)? 0 : ChannelReloadTimeout() * 1000);
}

int64_t PVRClientBase::SeekRecordedStream(int64_t iPosition, int iWhence)
{
    return (m_recordBuffer.buffer == NULL) ? -1 : m_recordBuffer.buffer->Seek(iPosition, iWhence);
}

int64_t PVRClientBase::PositionRecordedStream(void)
{
    return (m_recordBuffer.buffer == NULL) ? -1 : m_recordBuffer.buffer->GetPosition();
}
int64_t PVRClientBase::LengthRecordedStream(void)
{
    return (m_recordBuffer.buffer == NULL) ? -1 : m_recordBuffer.buffer->GetLength();
}

PVR_ERROR PVRClientBase::IsEPGTagRecordable(const kodi::addon::PVREPGTag& tag, bool& isRecordable)
{
    // Seems we can record all tags
    isRecordable = true;
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

bool PVRClientBase::StartRecordingFor(kodi::addon::PVRTimer &timer)
{
    if(NULL == m_clientCore)
        return false;

    bool hasEpg = false;
    auto pThis = this;
    kodi::addon::PVRRecording tag;
    IClientCore::EpgEntryAction action = [pThis ,&tag, timer, &hasEpg](const EpgEntryList::value_type& epgEntry)
    {
        try {
            if(epgEntry.first != timer.GetEPGUid())
                return true;
           
            pThis->FillRecording(epgEntry, tag, s_LocalRecPrefix.c_str());
            tag.SetRecordingTime(time(nullptr));
            tag.SetDuration(timer.GetEndTime() - tag.GetRecordingTime());
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
    
    std::string recordingDir = DirectoryForRecording(timer.GetEPGUid());
    
    if(!kodi::vfs::CreateDirectory(recordingDir)) {
        LogError("StartRecordingFor(): failed to create recording directory %s ", recordingDir.c_str());
        return false;
    }

    std::string infoPath = PathForRecordingInfo(timer.GetEPGUid());
    kodi::vfs::CFile infoFile;
    
    if(!infoFile.OpenFileForWrite(infoPath, true)){
        LogError("StartRecordingFor(): failed to create recording info file %s ", infoPath.c_str());
        return false;
    }
    const PVR_RECORDING* data = tag.GetCStructure();
    if(infoFile.Write(data, sizeof(*data))  != sizeof(*data)){
        LogError("StartRecordingFor(): failed to write recording info file %s ", infoPath.c_str());
        infoFile.Close();
        return false;
    }
    infoFile.Close();
    
    KodiChannelId kodiChannelId = timer.GetClientChannelUid();
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

bool PVRClientBase::StopRecordingFor(kodi::addon::PVRTimer &timer)
{
    void* infoFile = nullptr;
    // Update recording duration
    do {
        std::string infoPath = PathForRecordingInfo(timer.GetEPGUid());
        kodi::vfs::CFile infoFile;
        if(!infoFile.OpenFileForWrite(infoPath, false)){
            LogError("StopRecordingFor(): failed to open recording info file %s ", infoPath.c_str());
            break;
        }
        PVR_RECORDING tag = {0};
        infoFile.Seek(0, SEEK_SET);
        if(infoFile.Read(&tag, sizeof(tag))  != sizeof(tag)){
            LogError("StopRecordingFor(): failed to read from recording info file %s ", infoPath.c_str());
            break;
        }
        tag.iDuration = time(nullptr) - tag.recordingTime;
        infoFile.Seek(0, SEEK_SET);
        if(infoFile.Write(&tag, sizeof(tag))  != sizeof(tag)){
            LogError("StopRecordingFor(): failed to write recording info file %s ", infoPath.c_str());
            infoFile.Close();
            break;
        }
    } while(false);
    
    KodiChannelId kodiChannelId = timer.GetClientChannelUid();
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
    PVR->Addon_TriggerRecordingUpdate();
    return true;
    
}
bool PVRClientBase::FindEpgFor(kodi::addon::PVRTimer &timer)
{
    return true;
}



#pragma mark - Menus

void PVRClientBase::RegisterCommonMenuHook(unsigned int hookId, unsigned int localizedStringId)
{
    kodi::addon::PVRMenuhook hook(hookId,  localizedStringId, PVR_MENUHOOK_CHANNEL);
    PVR->Addon_AddMenuHook(hook);
    hook.SetCategory(PVR_MENUHOOK_EPG);
    PVR->Addon_AddMenuHook(hook);
    hook.SetCategory(PVR_MENUHOOK_RECORDING);
    PVR->Addon_AddMenuHook(hook);
    hook.SetCategory(PVR_MENUHOOK_SETTING);
    PVR->Addon_AddMenuHook(hook);
}

PVR_ERROR  PVRClientBase::HandleCommonMenuHook(const kodi::addon::PVRMenuhook &menuhook)
{
    
    if(RELOAD_EPG_MENU_HOOK == menuhook.GetHookId()) {
        kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32012).c_str());
        OnReloadEpg();
        m_clientCore->CallRpcAsync("{\"jsonrpc\": \"2.0\", \"method\": \"GUI.ActivateWindow\", \"params\": {\"window\": \"pvrsettings\"},\"id\": 1}",
                     [&] (rapidjson::Document& jsonRoot) {
                        kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32016).c_str());
                     },
                     [&](const ActionQueue::ActionResult& s) {});

    } else if(RELOAD_RECORDINGS_MENU_HOOK == menuhook.GetHookId()) {
//        char* message = XBMC->GetLocalizedString(32012);
//        kodi::QueueFormattedNotification(QUEUE_INFO, message);
//        XBMC->FreeString(message);
        OnReloadRecordings();
    }
    return PVR_ERROR_NO_ERROR;
    
}

PVR_ERROR PVRClientBase::CallSettingsMenuHook(const kodi::addon::PVRMenuhook& menuhook)
{
    return HandleCommonMenuHook(menuhook);
}
PVR_ERROR PVRClientBase::CallChannelMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRChannel&)
{
    return HandleCommonMenuHook(menuhook);
}

PVR_ERROR PVRClientBase::CallEPGMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVREPGTag&)
{
    return HandleCommonMenuHook(menuhook);
}

PVR_ERROR PVRClientBase::CallRecordingMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRRecording&)
{
    return HandleCommonMenuHook(menuhook);
}

PVR_ERROR PVRClientBase::CallTimerMenuHook(const kodi::addon::PVRMenuhook&, const kodi::addon::PVRTimer&)
{
    return PVR_ERROR_NO_ERROR;
}


#pragma mark - Playlist Utils
bool PVRClientBase::CheckPlaylistUrl(const std::string& url)
{
    auto f  = XBMC_OpenFile(url);
    
    if (nullptr == f) {
        kodi::QueueFormattedNotification(QUEUE_ERROR, kodi::GetLocalizedString(32010).c_str());
        return false;
    }
    
    f->Close();
    delete f;
    return true;
}

void PvrClient::EpgEntry::FillEpgTag(kodi::addon::PVREPGTag& tag) const{
    // NOTE: internal channel ID is not valid for Kodi's EPG
    // This field should be filled by caller
    //tag.iUniqueChannelId = ChannelId;
    tag.SetTitle(Title);
    tag.SetPlot(Description);
    tag.SetStartTime(StartTime);
    tag.SetEndTime(EndTime);
    tag.SetIconPath(IconPath);
    if(!Category.empty()) {
        tag.SetGenreType(EPG_GENRE_USE_STRING);
        tag.SetGenreDescription(Category);
    }
}

#pragma mark - Settings

template<typename T>
static void NotifyClearPvrData(std::function<const T&()>) {
    kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32016).c_str());
}

static const std::string c_curlTimeout = "curl_timeout";
static const std::string c_channelReloadTimeout = "channel_reload_timeout";
static const std::string c_numOfHlsThreads = "num_of_hls_threads";
static const std::string c_enableTimeshift = "enable_timeshift";
static const std::string c_timeshiftPath = "timeshift_path";
static const std::string c_recordingPath = "recordings_path";
static const std::string c_timeshiftSize = "timeshift_size";
static const std::string c_cacheSizeLimit = "timeshift_off_cache_limit";
static const std::string c_timeshiftType = "timeshift_type";
static const std::string c_rpcLocalPort = "rpc_local_port";
static const std::string c_rpcUser = "rpc_user";
static const std::string c_rpcPassword = "rpc_password";
static const std::string c_rpcEnableSsl = "rpc_enable_ssl";
static const std::string c_channelIndexOffset = "channel_index_offset";
static const std::string c_addCurrentEpgToArchive = "archive_for_current_epg_item";
static const std::string c_useChannelGroupsForArchive = "archive_use_channel_groups";
static const std::string c_waitForInternetTimeout = "wait_for_inet";
static const std::string c_startupDelay = "startup_delay";
static const std::string c_startRecordingPadding = "archive_start_padding";
static const std::string c_endRecordingPadding = "archive_end_padding";
static const std::string c_supportArchive = "archive_support";
static const std::string c_loadArchiveAfterEpg = "archive_wait_for_epg";
static const std::string c_archiveRefreshInterval = "archive_refresh_interval";
static const std::string c_livePlaybackDelayHls = "live_playback_delay_hls";
static const std::string c_livePlaybackDelayTs = "live_playback_delay_ts";
static const std::string c_livePlaybackDelayUdp = "live_playback_delay_udp";
static const std::string c_seekArchivePadding = "archive_seek_padding_on_start";
static const std::string c_epgCorrectionShift = "epg_correction_shift";
static const std::string c_logosFolderPath = "channel_logos_folder";
static const std::string c_suppotMulticastUrls("playlist_support_multicast_urls");
static const std::string c_udpProxyHost("playlist_udp_proxy_host");
static const std::string c_udpProxyPort("playlist_udp_proxy_port");


void PVRClientBase::InitSettings()
{
    // NOTE: where default type value (i.e. T{}) is a valid value
    // do not set default setting value other then T{} (use setting XML instead)
    // OTherwise T{} will be replaced with default vaue on AddonSettings.Init()
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
    .Add(c_startupDelay, 0)
    .Add(c_startRecordingPadding, 0)
    .Add(c_endRecordingPadding, 0)
    .Add(c_supportArchive, false, ADDON_STATUS_NEED_RESTART)
    .Add(c_loadArchiveAfterEpg, false, ADDON_STATUS_NEED_RESTART)
    .Add(c_archiveRefreshInterval, 0) // default 3 (see notes abouve!)
    .Add(c_livePlaybackDelayHls, 0)
    .Add(c_livePlaybackDelayTs, 0)
    .Add(c_livePlaybackDelayUdp, 0)
    .Add(c_seekArchivePadding, false)
    .Add(c_rpcUser,"kodi")
    .Add(c_rpcPassword, "")
    .Add(c_rpcEnableSsl, false)
    .Add(c_epgCorrectionShift, 0.0f, ADDON_STATUS_NEED_RESTART)
    .Add(c_logosFolderPath, "", ADDON_STATUS_NEED_RESTART)
    .Add(c_suppotMulticastUrls, false, NotifyClearPvrData<bool>, ADDON_STATUS_NEED_RESTART)
    .Add(c_udpProxyHost, "", NotifyClearPvrData<std::string>, ADDON_STATUS_NEED_RESTART)
    .Add(c_udpProxyPort, 0, NotifyClearPvrData<int>, ADDON_STATUS_NEED_RESTART)
    ;
    
    PopulateSettings(m_addonMutableSettings);
    
    m_addonMutableSettings.Init();
    m_addonMutableSettings.Print();
}

uint32_t PVRClientBase::UdpProxyPort() const
{
    int port = m_addonSettings.GetInt(c_udpProxyPort);
    if(port < 0)
        port = 0;
    return port;
}

const std::string& PVRClientBase::UdpProxyHost() const
{
    return m_addonSettings.GetString(c_udpProxyHost);
}

bool PVRClientBase::SuppotMulticastUrls() const
{
    return m_addonSettings.GetBool(c_suppotMulticastUrls);
}

bool PVRClientBase::RpcEnableSsl() const
{
    return m_addonSettings.GetBool(c_rpcEnableSsl);
}

const std::string& PVRClientBase::RpcUser() const
{
    return m_addonSettings.GetString(c_rpcUser);
}
const std::string& PVRClientBase::RpcPassword() const
{
    return m_addonSettings.GetString(c_rpcPassword);
}

bool PVRClientBase::SeekArchivePadding() const
{
    return m_addonSettings.GetBool(c_seekArchivePadding);
}

int PVRClientBase::LivePlaybackDelayForHls() const
{
    return m_addonSettings.GetInt(c_livePlaybackDelayHls);
}

int PVRClientBase::LivePlaybackDelayForTs() const
{
    return m_addonSettings.GetInt(c_livePlaybackDelayTs);
}

int PVRClientBase::LivePlaybackDelayForMulticast() const
{
    return m_addonSettings.GetInt(c_livePlaybackDelayUdp);
}

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

uint32_t PVRClientBase::ArchiveRefreshInterval() const
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

int PVRClientBase::EpgCorrectionShift() const
{
    return m_addonSettings.GetFloat(c_epgCorrectionShift) * 60 * 60 + 0.5;
}

const std::string& PVRClientBase::LocalLogosFolder() const
{
    return m_addonSettings.GetString(c_logosFolderPath);
}

