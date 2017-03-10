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


#include "addon.h"
#include "sovok_data_source.h"
#include "xbmc_pvr_dll.h"
#include "p8-platform/util/util.h"


#ifdef TARGET_WINDOWS
#define snprintf _snprintf
#endif

static IPvrIptvDataSource* m_DataSource = NULL;


extern "C" {

ADDON_STATUS ADDON_Create(void* hdl, void* props)
{
    //m_DataSource = new IptvSimple();
    m_DataSource = new SovokTvDataSource();
    return m_DataSource->Init(hdl, props);
}

ADDON_STATUS ADDON_GetStatus()
{
    return m_DataSource->GetStatus();
}

void ADDON_Destroy()
{
    m_DataSource->Destroy();
    SAFE_DELETE(m_DataSource);
}

bool ADDON_HasSettings()
{
  return true;
}

unsigned int ADDON_GetSettings(ADDON_StructSetting ***sSet)
{
    return m_DataSource->GetSettings(sSet);
}

ADDON_STATUS ADDON_SetSetting(const char *settingName, const void *settingValue)
{
    return m_DataSource->SetSetting(settingName, settingValue);
}


void ADDON_FreeSettings()
{
    m_DataSource->FreeSettings();
}
    
void ADDON_Stop()
{
}

/***********************************************************
 * PVR Client AddOn specific public library functions
 ***********************************************************/

void OnSystemSleep()
{
}

void OnSystemWake()
{
}

void OnPowerSavingActivated()
{
}

void OnPowerSavingDeactivated()
{
}

const char* GetPVRAPIVersion(void)
{
  static const char *strApiVersion = XBMC_PVR_API_VERSION;
  return strApiVersion;
}

const char* GetMininumPVRAPIVersion(void)
{
  static const char *strMinApiVersion = XBMC_PVR_MIN_API_VERSION;
  return strMinApiVersion;
}

const char* GetGUIAPIVersion(void)
{
  return ""; // GUI API not used
}

const char* GetMininumGUIAPIVersion(void)
{
  return ""; // GUI API not used
}

PVR_ERROR GetAddonCapabilities(PVR_ADDON_CAPABILITIES* pCapabilities)
{
    return m_DataSource->GetAddonCapabilities(pCapabilities);
}

const char *GetBackendName(void)
{
  static const char *strBackendName = "Puzzle TV PVR Add-on";
  return strBackendName;
}

const char *GetBackendVersion(void)
{
  static std::string strBackendVersion = XBMC_PVR_API_VERSION;
  return strBackendVersion.c_str();
}

const char *GetConnectionString(void)
{
  static std::string strConnectionString = "connected";
  return strConnectionString.c_str();
}

const char *GetBackendHostname(void)
{
  return "";
}

PVR_ERROR GetDriveSpace(long long *iTotal, long long *iUsed)
{
  *iTotal = 0;
  *iUsed  = 0;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL &channel, time_t iStart, time_t iEnd)
{
    return m_DataSource->GetEPGForChannel(handle, channel, iStart, iEnd);
}

int GetChannelsAmount(void)
{
    return m_DataSource->GetChannelsAmount();
}

PVR_ERROR GetChannels(ADDON_HANDLE handle, bool bRadio)
{
    return m_DataSource->GetChannels(handle, bRadio);
}

bool OpenLiveStream(const PVR_CHANNEL &channel)
{
    return m_DataSource->OpenLiveStream(channel);
}

void CloseLiveStream(void)
{
    m_DataSource->CloseLiveStream();
}

bool SwitchChannel(const PVR_CHANNEL &channel)
{
    return m_DataSource->SwitchChannel(channel);
}

PVR_ERROR GetStreamProperties(PVR_STREAM_PROPERTIES* pProperties)
{
  return PVR_ERROR_NOT_IMPLEMENTED;
}

int GetChannelGroupsAmount(void)
{
    return m_DataSource->GetChannelGroupsAmount();
}

PVR_ERROR GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
    return m_DataSource->GetChannelGroups(handle, bRadio);
}

PVR_ERROR GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP &group)
{
    return m_DataSource->GetChannelGroupMembers(handle, group);
}

PVR_ERROR SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    return m_DataSource->SignalStatus(signalStatus);
}

bool CanPauseStream(void)
{
    return m_DataSource->CanPauseStream();
}

bool CanSeekStream(void)
{
    return m_DataSource->CanSeekStream();
}

long long SeekLiveStream(long long iPosition, int iWhence /* = SEEK_SET */)
{
    return m_DataSource->SeekLiveStream(iPosition, iWhence);
}

long long PositionLiveStream(void)
{
    return m_DataSource->PositionLiveStream();
}
    
long long LengthLiveStream(void)
{
    return m_DataSource->LengthLiveStream();
}
   
int ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    return m_DataSource->ReadLiveStream(pBuffer, iBufferSize);
}
PVR_ERROR CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    return m_DataSource->CallMenuHook(menuhook, item);
}

    // ******* RECORDING ******/
    int GetRecordingsAmount(bool deleted)
    {
        return m_DataSource->GetRecordingsAmount(deleted);
    }

    PVR_ERROR GetRecordings(ADDON_HANDLE handle, bool deleted)
    {
        return m_DataSource->GetRecordings(handle,  deleted);
    }
    
    
    bool OpenRecordedStream(const PVR_RECORDING &recording)
    {
        return m_DataSource->OpenRecordedStream(recording);
    }
    
    void CloseRecordedStream(void)
    {
        return m_DataSource->CloseRecordedStream();
    }
    int ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
    {
        return m_DataSource->ReadRecordedStream(pBuffer, iBufferSize);
    }
    
    long long SeekRecordedStream(long long iPosition, int iWhence)
    {
        return m_DataSource->SeekRecordedStream(iPosition, iWhence);
    }
    long long PositionRecordedStream(void)
    {
        return m_DataSource->PositionRecordedStream();
    }

    long long LengthRecordedStream(void)
    {
        return m_DataSource->LengthRecordedStream();
    }


    
/** UNUSED API FUNCTIONS */
const char * GetLiveStreamURL(const PVR_CHANNEL &channel)  { return ""; }
PVR_ERROR OpenDialogChannelScan(void) { return PVR_ERROR_NOT_IMPLEMENTED; }

PVR_ERROR DeleteChannel(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR RenameChannel(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR MoveChannel(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR OpenDialogChannelSettings(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR OpenDialogChannelAdd(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
void DemuxReset(void) {}
void DemuxFlush(void) {}
PVR_ERROR DeleteRecording(const PVR_RECORDING &recording) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR RenameRecording(const PVR_RECORDING &recording) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR SetRecordingPlayCount(const PVR_RECORDING &recording, int count) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR SetRecordingLastPlayedPosition(const PVR_RECORDING &recording, int lastplayedposition) { return PVR_ERROR_NOT_IMPLEMENTED; }
int GetRecordingLastPlayedPosition(const PVR_RECORDING &recording) { return -1; }
PVR_ERROR GetRecordingEdl(const PVR_RECORDING&, PVR_EDL_ENTRY[], int*) { return PVR_ERROR_NOT_IMPLEMENTED; };
PVR_ERROR GetTimerTypes(PVR_TIMER_TYPE types[], int *size) { return PVR_ERROR_NOT_IMPLEMENTED; }
int GetTimersAmount(void) { return -1; }
PVR_ERROR GetTimers(ADDON_HANDLE handle) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR AddTimer(const PVR_TIMER &timer) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR DeleteTimer(const PVR_TIMER &timer, bool bForceDelete) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR UpdateTimer(const PVR_TIMER &timer) { return PVR_ERROR_NOT_IMPLEMENTED; }
void DemuxAbort(void) {}
DemuxPacket* DemuxRead(void) { return NULL; }
unsigned int GetChannelSwitchDelay(void) { return 0; }
bool IsTimeshifting(void) { return false; }
bool IsRealTimeStream(void) { return true; }
void PauseStream(bool bPaused) {}
bool SeekTime(double,bool,double*) { return false; }
void SetSpeed(int) {};
time_t GetPlayingTime() { return 0; }
time_t GetBufferTimeStart() { return 0; }
time_t GetBufferTimeEnd() { return 0; }
PVR_ERROR UndeleteRecording(const PVR_RECORDING& recording) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR DeleteAllRecordingsFromTrash() { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR SetEPGTimeFrame(int) { return PVR_ERROR_NOT_IMPLEMENTED; }
}