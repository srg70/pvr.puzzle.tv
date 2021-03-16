/*
 *      Copyright (C) 2017 Sergey Shramchenko
 *      https://github.com/srg70/pvr.puzzle.tv
 *
 *      Copyright (C) 2013-2015 Anton Fedchin
 *      http://github.com/afedchin/xbmc-addon-iptvsimple/
 *
 *      Copyright (C) 2011 Pulse-Eight
 *      http://www.pulse-eight.com/
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

#include <kodi/General.h>
#include "addon.h"
#include "globals.hpp"
#include "sovok_pvr_client.h"
#include "puzzle_pvr_client.h"
#include "ott_pvr_client.h"
#include "edem_pvr_client.h"
#include "ttv_pvr_client.h"
#include "sharatv_pvr_client.h"
#include "p8-platform/util/util.h"
#include "TimersEngine.hpp"

#ifdef TARGET_WINDOWS
#define snprintf _snprintf
#endif

static ITimersEngine* m_timersEngine = NULL;
static IPvrIptvDataSource* m_DataSource = NULL;
static int m_clientType = 1;

namespace Globals {
    void CreateWithHandle(IAddonDelegate* pvr);
    void Cleanup();
}

//extern "C" {
    
static IPvrIptvDataSource* CreateDataSourceWithType(int type)
{
    IPvrIptvDataSource* dataSource = NULL;
    switch (type) {
        case 0:
            dataSource = new PuzzlePVRClient();
            break;
        case 1:
            dataSource = new SovokPVRClient();
            break;
        case 2:
            dataSource = new OttPVRClient();
            break;
        case 3:
            dataSource = new EdemPVRClient();
            break;
        case 4:
            dataSource = new TtvPVRClient();
            break;
        case 5:
            dataSource = new SharaTvPVRClient();
            break;

        default:
            dataSource = NULL;
            break;
    }
    return dataSource;
}
    
class ATTRIBUTE_HIDDEN PVRPuzzleTv
  : public kodi::addon::CAddonBase,
    public kodi::addon::CInstancePVRClient,
    public IAddonDelegate
{
public:
    
    void Addon_TriggerRecordingUpdate() override {TriggerRecordingUpdate();}
    void Addon_AddMenuHook(const kodi::addon::PVRMenuhook& hook) override {AddMenuHook(hook);}
    void Addon_TriggerChannelUpdate() override {TriggerChannelUpdate();}
    void Addon_TriggerChannelGroupsUpdate() override {TriggerChannelGroupsUpdate();}
    void Addon_TriggerEpgUpdate(unsigned int channelUid) override {TriggerEpgUpdate(channelUid);}
    void Addon_TriggerTimerUpdate() override {TriggerTimerUpdate();}
    
    ADDON_STATUS Create() override
    {
        
        Globals::CreateWithHandle(this);
        m_clientType = kodi::GetSettingInt("provider_type");
    
        m_DataSource = CreateDataSourceWithType(m_clientType);
        if (NULL == m_DataSource) {
            kodi::QueueFormattedNotification(QUEUE_ERROR, kodi::GetLocalizedString(32001).c_str());
            return ADDON_STATUS_NEED_SETTINGS;
        }
        
        ADDON_STATUS result = m_DataSource->Init(kodi::GetBaseUserPath(), kodi::GetAddonPath());
        m_timersEngine = new Engines::TimersEngine(m_DataSource);
        return result;
    }
    
    ADDON_STATUS ADDON_GetStatus()
    {
        return m_DataSource->GetStatus();
    }
    
    void ADDON_Destroy()
    {
        if(m_timersEngine)
            SAFE_DELETE(m_timersEngine);
        if(m_DataSource)
            SAFE_DELETE(m_DataSource);

        Globals::Cleanup();
    }
    
    ADDON_STATUS SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue) override
    {
        if (settingName == "provider_type") {
            
            int newValue = settingValue.GetInt();
            if(m_clientType != newValue) {
                m_clientType = newValue;
                return ADDON_STATUS_NEED_RESTART;
            }
            return ADDON_STATUS_OK;
        }
        
        return m_DataSource->SetSetting(settingName, settingValue);
    }
    
    
    /***********************************************************
     * PVR Client AddOn specific public library functions
     ***********************************************************/
    
    PVR_ERROR OnSystemSleep() override
    {
        Globals::LogDebug("Energy: OnSystemSleep() called.");
        if(m_timersEngine){
            SAFE_DELETE(m_timersEngine);
        }
        if(m_DataSource){
            m_DataSource->OnSystemSleep();
        }
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR OnSystemWake() override
    {
        Globals::LogDebug("Energy: OnSystemWake() called.");
        if(m_DataSource) {
            m_DataSource->OnSystemWake();
            m_timersEngine = new Engines::TimersEngine(m_DataSource);
        }
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR OnPowerSavingActivated() override {
        return PVR_ERROR_NO_ERROR;
    }
    PVR_ERROR OnPowerSavingDeactivated() override {
        return PVR_ERROR_NO_ERROR;
    }

    PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override
    {
        return m_DataSource->GetAddonCapabilities(capabilities);
    }
    
    PVR_ERROR GetBackendName(std::string& name) override
    {
        name = "Puzzle TV PVR Add-on";
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR GetBackendVersion(std::string& version) override
    {
        version = STR(IPTV_VERSION);
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR GetConnectionString(std::string& connection) override
    {
        connection = "connected";
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR GetBackendHostname(std::string& hostname) override
    {
        hostname = "";
        return PVR_ERROR_NO_ERROR;
    }
    
//    PVR_ERROR GetDriveSpace(long long *iTotal, long long *iUsed)
//    {
//        *iTotal = 0;
//        *iUsed  = 0;
//        return PVR_ERROR_NO_ERROR;
//    }
#pragma mark - EPG
    PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results) override
    {
        return m_DataSource->GetEPGForChannel(channelUid, start, end, results);
    }
    
//    PVR_ERROR GetEPGTagStreamProperties(const kodi::addon::PVREPGTag& tag, std::vector<kodi::addon::PVRStreamProperty>& properties)  override
//    {
//
//    }
    
    
//    PVR_ERROR IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag, bool& bIsPlayable) override
//    {
//    }
    PVR_ERROR SetEPGMaxPastDays(int epgMaxPastDays) override
    {
        return PVR_ERROR_NO_ERROR;
    }
    PVR_ERROR SetEPGMaxFutureDays(int epgMaxFutureDays) override
    {
        return PVR_ERROR_NO_ERROR;
    }

#pragma mark - Channels and Groups
    PVR_ERROR GetChannelsAmount(int& amount) override
    {
        amount = m_DataSource->GetChannelsAmount();
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override
    {
        return m_DataSource->GetChannels(radio, results);
    }
    
//    PVR_ERROR GetChannelStreamProperties(const kodi::addon::PVRChannel& channel, std::vector<kodi::addon::PVRStreamProperty>& properties) override
//    {
//
//    }

    PVR_ERROR GetChannelGroupsAmount(int& amount) override
    {
        amount = m_DataSource->GetChannelGroupsAmount();
        return PVR_ERROR_NO_ERROR;
    }
    
    PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override
    {
        return m_DataSource->GetChannelGroups(radio, results);
    }
    
    PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group, kodi::addon::PVRChannelGroupMembersResultSet& results) override
    {
        return m_DataSource->GetChannelGroupMembers(group, results);
    }
    
    PVR_ERROR GetSignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus) override
    {
        return m_DataSource->SignalStatus(channelUid, signalStatus);
    }
    
#pragma mark - Live Stream
    // ******* MENU ******/
    bool OpenLiveStream(const kodi::addon::PVRChannel& channel) override
    {
        return m_DataSource->OpenLiveStream(channel);
    }
    
    void CloseLiveStream(void) override
    {
        m_DataSource->CloseLiveStream();
    }
    
    bool CanPauseStream(void) override
    {
        return m_DataSource->CanPauseStream();
    }
    
    bool CanSeekStream(void) override
    {
        return m_DataSource->CanSeekStream();
    }
    
    int64_t SeekLiveStream(int64_t position, int whence) override
    {
        return m_DataSource->SeekLiveStream(position, whence);
    }
        
    int64_t LengthLiveStream(void) override
    {
        return m_DataSource->LengthLiveStream();
    }
    
    int ReadLiveStream(unsigned char* buffer, unsigned int size) override
    {
        return m_DataSource->ReadLiveStream(buffer, size);
    }
#pragma mark - Menu
    // ******* MENU ******/
    PVR_ERROR CallSettingsMenuHook(const kodi::addon::PVRMenuhook& menuhook) override
    {
      return m_DataSource->CallSettingsMenuHook(menuhook);
    }
    PVR_ERROR CallChannelMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRChannel& item) override
    {
        return m_DataSource->CallChannelMenuHook(menuhook, item);
    }
    PVR_ERROR CallEPGMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVREPGTag& tag) override
    {
        return m_DataSource->CallEPGMenuHook(menuhook, tag);
    }
    PVR_ERROR CallRecordingMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRRecording& item) override
    {
        return m_DataSource->CallRecordingMenuHook(menuhook, item);
    }
    PVR_ERROR CallTimerMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRTimer& item) override
    {
        return m_DataSource->CallTimerMenuHook(menuhook, item);
    }
    
#pragma mark - Recordings
    // ******* RECORDING ******/
    
    PVR_ERROR GetStreamReadChunkSize(int& chunksize) override
    {
        return m_DataSource->GetStreamReadChunkSize(chunksize);
    }
    PVR_ERROR GetRecordingsAmount(bool deleted, int& amount) override
    {
        return m_DataSource->GetRecordingsAmount(deleted, amount);
    }
    
    PVR_ERROR GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results) override
    {
        return m_DataSource->GetRecordings(deleted,  results);
    }
    
    PVR_ERROR DeleteRecording(const kodi::addon::PVRRecording& recording) override
    {
        return m_DataSource->DeleteRecording(recording);
    }

    bool OpenRecordedStream(const kodi::addon::PVRRecording& recording) override
    {
        return m_DataSource->OpenRecordedStream(recording);
    }
    
    void CloseRecordedStream(void) override
    {
        return m_DataSource->CloseRecordedStream();
    }
    int ReadRecordedStream(unsigned char* buffer, unsigned int size) override
    {
        return m_DataSource->ReadRecordedStream(buffer, size);
    }
    
    int64_t SeekRecordedStream(int64_t position, int whence) override
    {
        return m_DataSource->SeekRecordedStream(position, whence);
    }
   
    int64_t LengthRecordedStream() override
    {
        return m_DataSource->LengthRecordedStream();
    }
    
    PVR_ERROR IsEPGTagRecordable(const kodi::addon::PVREPGTag& tag, bool& isRecordable) override
    {
        return m_DataSource->IsEPGTagRecordable(tag, isRecordable);
    }

#pragma mark - Timers
    /********************************************************************************/
    /********************************* T I M E R S **********************************/
    /********************************************************************************/
    
    PVR_ERROR AddTimer(const kodi::addon::PVRTimer& timer) override
    {
        return m_timersEngine ? m_timersEngine->AddTimer (timer) : PVR_ERROR_FAILED;
    }
    
    PVR_ERROR DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete) override
    {
        return m_timersEngine ? m_timersEngine->DeleteTimer (timer,forceDelete) : PVR_ERROR_FAILED;
    }
    
    PVR_ERROR UpdateTimer(const kodi::addon::PVRTimer& timer) override
    {
        return m_timersEngine ? m_timersEngine->UpdateTimer (timer) : PVR_ERROR_FAILED;
    }
    
    PVR_ERROR GetTimers(kodi::addon::PVRTimersResultSet& results) override
    {
        return m_timersEngine ? m_timersEngine->GetTimers(results) : PVR_ERROR_FAILED;
    }
    
    PVR_ERROR GetTimersAmount(int& amount) override
    {
        amount =  m_timersEngine ? m_timersEngine->GetTimersAmount() : -1;
        return  -1 == amount ? PVR_ERROR_FAILED : PVR_ERROR_NO_ERROR;
    }

//    PVR_ERROR GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types) override {
//        return PVR_ERROR_NOT_IMPLEMENTED;
//    }

#pragma mark - Timeshift
    /********************************************************************************/
    /**************************** TIMESHIFT API FUNCTIONS ***************************/
    /********************************************************************************/
    
    PVR_ERROR GetStreamTimes(kodi::addon::PVRStreamTimes& times) override
    {
        return m_DataSource->GetStreamTimes(times);
    }
       
    bool IsRealTimeStream() override
    {
        return m_DataSource->IsRealTimeStream();        
    }
};

ADDONCREATOR(PVRPuzzleTv)

//}
