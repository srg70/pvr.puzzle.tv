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
#include "kodi/General.h"
#include "kodi/gui/dialogs/Select.h"

#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "puzzle_pvr_client.h"
#include "helpers.h"
#include "puzzle_tv.h"
#include "globals.hpp"

using namespace Globals;
using namespace std;
using namespace PuzzleEngine;
using namespace PvrClient;
using namespace Helpers;

static const char* c_server_url_setting = "puzzle_server_uri";
static const char* c_server_port_setting = "puzzle_server_port";
static const char* c_server_retries_setting = "puzzle_server_retries";
static const char* c_epg_provider_setting = "puzzle_server_epg_provider_type";
static const char* c_epg_url_setting = "puzzle_server_epg_url";
static const char* c_epg_port_setting = "puzzle_server_epg_port";
static const char* c_server_version_setting = "puzzle_server_version";
static const char* c_seek_archives = "puzzle_seek_archives";
static const char* c_block_dead_streams = "puzzle_block_dead_streams";

const unsigned int UPDATE_CHANNEL_STREAMS_MENU_HOOK = PVRClientBase::s_lastCommonMenuHookId + 1;
const unsigned int UPDATE_CHANNELS_MENU_HOOK = UPDATE_CHANNEL_STREAMS_MENU_HOOK + 1;


ADDON_STATUS PuzzlePVRClient::Init(const std::string& clientPath, const std::string& userPath)
{
    ADDON_STATUS retVal = PVRClientBase::Init(clientPath, userPath);
    if(ADDON_STATUS_OK != retVal)
       return retVal;
    
    m_currentChannelStreamIdx = -1;
    
    m_serverPort = kodi::GetSettingInt(c_server_port_setting, 8089);
    m_serverUri = kodi::GetSettingString(c_server_url_setting);
    m_maxServerRetries = kodi::GetSettingInt(c_server_retries_setting, 4);

    m_epgUrl = kodi::GetSettingString(c_epg_url_setting);
    m_epgType = kodi::GetSettingEnum<EpgType>(c_epg_provider_setting, c_EpgType_File);
    
    if(m_epgType != c_EpgType_Server){
        m_epgType = c_EpgType_File;
    }
    
    m_epgPort = kodi::GetSettingInt(c_epg_port_setting, 8085);
    m_serverVersion = kodi::GetSettingEnum<ServerVersion>(c_server_version_setting, c_PuzzleServer3);
    
    bool supportSeek = kodi::GetSettingBoolean(c_seek_archives,false);
    SetSeekSupported(supportSeek);
    
    m_blockDeadStreams = kodi::GetSettingBoolean(c_block_dead_streams, true);
    
    PVR->Addon_AddMenuHook(kodi::addon::PVRMenuhook(UPDATE_CHANNEL_STREAMS_MENU_HOOK, 32052, PVR_MENUHOOK_CHANNEL));
    PVR->Addon_AddMenuHook(kodi::addon::PVRMenuhook (UPDATE_CHANNELS_MENU_HOOK, 32053, PVR_MENUHOOK_CHANNEL));

    retVal = CreateCoreSafe(false);
    
    return retVal;

}

void PuzzlePVRClient::PopulateSettings(AddonSettingsMutableDictionary& settings)
{
    
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
        OnCoreCreated();
    }
    catch (std::exception& ex)
    {
        kodi::QueueFormattedNotification(QUEUE_ERROR,  kodi::GetLocalizedString(32005).c_str());
        LogError("PuzzlePVRClient:: Can't create Puzzle Server core. Exeption: [%s].", ex.what());
        retVal = ADDON_STATUS_LOST_CONNECTION;
    }
    catch(...)
    {
        kodi::QueueFormattedNotification(QUEUE_ERROR, "Puzzle Server: unhandeled exception on reload EPG.");
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
    m_puzzleTV->IncludeCurrentEpgToArchive(HowToAddCurrentEpgToArchive());
    m_puzzleTV->SetEpgCorrectionShift(EpgCorrectionShift());
    m_puzzleTV->SetLocalLogosFolder(LocalLogosFolder());
    m_puzzleTV->InitAsync(clearEpgCache, IsArchiveSupported());
}

ADDON_STATUS PuzzlePVRClient::SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;

    if (c_server_port_setting == settingName)
    {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if (c_server_url_setting == settingName)
    {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if (c_server_retries_setting == settingName)
    {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(c_epg_url_setting == settingName) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(c_epg_provider_setting == settingName) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(c_server_version_setting == settingName) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(c_epg_port_setting == settingName) {
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(c_seek_archives == settingName) {
        SetSeekSupported(settingValue.GetBoolean());
        result = ADDON_STATUS_NEED_RESTART;
    }
    else if(c_block_dead_streams == settingName) {
        m_blockDeadStreams = settingValue.GetBoolean();
    }
    else {
        result = PVRClientBase::SetSetting(settingName, settingValue);
    }
    return result;
}

PVR_ERROR PuzzlePVRClient::GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
    capabilities.SetSupportsEPG(true);
    capabilities.SetSupportsTV(true);
    capabilities.SetSupportsRadio(true);
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

PVR_ERROR PuzzlePVRClient::CallChannelMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRChannel& item)
{
    if(m_puzzleTV == nullptr)
        return PVR_ERROR_SERVER_ERROR;
    
    if(UPDATE_CHANNEL_STREAMS_MENU_HOOK == menuhook.GetHookId()) {
        HandleStreamsMenuHook(ChannelIdForBrodcastId(item.GetUniqueId()));
        return PVR_ERROR_NO_ERROR;
    } else if (UPDATE_CHANNELS_MENU_HOOK == menuhook.GetHookId()) {
        CreateCoreSafe(false);
        PVR->Addon_TriggerChannelUpdate();
        m_clientCore->CallRpcAsync("{\"jsonrpc\": \"2.0\", \"method\": \"GUI.ActivateWindow\", \"params\": {\"window\": \"pvrsettings\"},\"id\": 1}",
                                   [&] (rapidjson::Document& jsonRoot) {
                                        kodi::QueueFormattedNotification(QUEUE_INFO, kodi::GetLocalizedString(32016).c_str());
                                   },
                                   [&](const ActionQueue::ActionResult& s) {});
        return PVR_ERROR_NO_ERROR;
    }
    return PVRClientBase::CallChannelMenuHook(menuhook, item);
}

struct StreamMenuItem{
    StreamMenuItem(const string& title, bool isEnabled = false)
    {
        IsEnabled = isEnabled;
        Title = title;
    }
    std::string Title;
    bool IsEnabled;
};

static int ShowStreamsMenu(const string& title, std::vector<StreamMenuItem>& items )
{
 
    std::vector<string> menu;
    std::vector<int> lut;
    for (int i = 0; i <  items.size(); ++i)
    {
        if(items[i].IsEnabled) {
            menu.push_back(items[i].Title);
            lut.push_back(i);
        }
    }
    int selected = kodi::gui::dialogs::Select::Show(title, menu, -1);
    if(selected < 0)
        return selected;
    return lut[selected];
}

static void FillStreamTitle(const PuzzleTV::PuzzleSource& stream,  std::string& title)
{
    char buf[100];
    sprintf(buf, "%s", stream.Server.c_str());
    title = buf;
}

void PuzzlePVRClient::HandleStreamsMenuHook(ChannelId channelId)
{
    int selected;
    std::string enableStreamLable(kodi::GetLocalizedString(32054));
    std::string disableStreamLable(kodi::GetLocalizedString(32055));
    std::string emptyStreamLable(kodi::GetLocalizedString(32060));
    std::string updateStreamsLable(kodi::GetLocalizedString(32056));
    do {
        PuzzleTV::TPrioritizedSources sources = m_puzzleTV->GetSourcesForChannel(channelId);
        
        StreamMenuItem disableItem(disableStreamLable, false);
        std::vector<StreamMenuItem> disableMenu;
        StreamMenuItem enableItem(enableStreamLable, false);
        std::vector<StreamMenuItem> enableMenu;
        StreamMenuItem emptyItem(emptyStreamLable, false);
        std::vector<StreamMenuItem> emptyMenu;
        
        std::vector<PuzzleTV::TCacheUrl> cacheUrls;
        while(!sources.empty()) {
            const auto source = sources.top();
            sources.pop();
            cacheUrls.push_back(source->first);
            disableItem.IsEnabled |= source->second.IsOn() && !source->second.IsEmpty();
            {
                StreamMenuItem item("", source->second.IsOn() && !source->second.IsEmpty());
                FillStreamTitle(source->second, item.Title);
                disableMenu.push_back(item);
            }
            enableItem.IsEnabled |= source->second.CanBeOn();
            {
                StreamMenuItem item("", source->second.CanBeOn());
                FillStreamTitle(source->second, item.Title);
                enableMenu.push_back(item);
            }
            emptyItem.IsEnabled |= source->second.IsOn() && source->second.IsEmpty();
            {
                StreamMenuItem item("", source->second.IsOn() && source->second.IsEmpty());
                FillStreamTitle(source->second, item.Title);
                emptyMenu.push_back(item);
            }
        }
        
        std::vector<StreamMenuItem> rootItems;
        rootItems.push_back(disableItem);
        rootItems.push_back(enableItem);
        rootItems.push_back(emptyItem);
        rootItems.push_back(StreamMenuItem(updateStreamsLable, true));

        selected = ShowStreamsMenu(kodi::GetLocalizedString(32057), rootItems);
        switch (selected)
        {
            // Disable stream
            case 0:
            {
                int selectedSource = ShowStreamsMenu(kodi::GetLocalizedString(32058), disableMenu);
                if(selectedSource >= 0) {
                    m_puzzleTV->DisableSource(channelId, cacheUrls[selectedSource]);
                }
                break;
            }
            // Enable stream
            case 1:
            {
                int selectedSource = ShowStreamsMenu(kodi::GetLocalizedString(32059), enableMenu);
                if(selectedSource >= 0){
                    m_puzzleTV->EnableSource(channelId, cacheUrls[selectedSource]);
                }
                break;
            }
            // Disable empty stream
            case 2:
            {
                int selectedSource = ShowStreamsMenu(kodi::GetLocalizedString(32058), emptyMenu);
                if(selectedSource >= 0){
                    m_puzzleTV->DisableSource(channelId, cacheUrls[selectedSource]);
                }
                break;
            }
            // Update stream list
            case 3:
                m_puzzleTV->UpdateChannelSources(channelId);
                break;
            default:
                break;
        }
    } while(selected >= 0);
    
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

void PuzzlePVRClient::OnOpenStremFailed(ChannelId channelId, const std::string& streamUrl)
{
    if(m_puzzleTV == nullptr)
        return;
    if(!m_blockDeadStreams)
        return;
    
    // Mark stream as bad and reset "good" stream offset.
    m_puzzleTV->OnOpenStremFailed(channelId, streamUrl);
    m_currentChannelStreamIdx = 0;
    // Expe
}

bool PuzzlePVRClient::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
    if(NULL == m_puzzleTV)
        return false;

    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
    // NOTE: Kodi does NOT provide recording.iChannelUid for unknown reason
    // Worrkaround: use EPG entry
    
    EpgEntry epgTag;
    if(!m_puzzleTV->GetEpgEntry(stoi(recording.GetRecordingId().c_str()), epgTag))
        return false;
    
    string url = m_puzzleTV->GetArchiveUrl(epgTag.UniqueChannelId, recording.GetRecordingTime());

    return PVRClientBase::OpenRecordedStream(url, nullptr, IsSeekSupported() ? SupportVodSeek : NoRecordingFlags);
}

PVR_ERROR PuzzlePVRClient::SignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus)
{
    signalStatus.SetAdapterName("IPTV Puzzle Server");
    signalStatus.SetAdapterStatus((m_puzzleTV == NULL) ? "Not connected" :"OK");
    string liveUrl = GetLiveUrl();
    string serviceName;
    if(!liveUrl.empty()) {
        PuzzleTV::TPrioritizedSources sources = m_puzzleTV->GetSourcesForChannel(GetLiveChannelId());
        string currentSource;
        while(!sources.empty()) {
            const auto source = sources.top();
            sources.pop();
            for (const auto& stream  : source->second.Streams) {
                if(stream.first == liveUrl) {
                    serviceName = source->second.Server;
                    break;
                }
            }
            if(!serviceName.empty()) {
                break;
            }
        }
        signalStatus.SetProviderName(serviceName);
        
    }
    return this->PVRClientBase::SignalStatus(channelUid, signalStatus);
}
       



