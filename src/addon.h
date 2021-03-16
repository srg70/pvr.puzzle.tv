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

#ifndef __Iptv_Pvr_Addoin_h__
#define __Iptv_Pvr_Addoin_h__

#include <kodi/addon-instance/PVR.h>

class ITimersEngineDelegate{
public:
    virtual bool StartRecordingFor(kodi::addon::PVRTimer &timer) = 0;
    virtual bool StopRecordingFor(kodi::addon::PVRTimer &timer) = 0;
    virtual bool FindEpgFor(kodi::addon::PVRTimer &timer) = 0;
protected:
    virtual ~ITimersEngineDelegate() {}
};

class IAddonDelegate{
public:
    virtual void Addon_TriggerRecordingUpdate() = 0;
    virtual void Addon_AddMenuHook(const kodi::addon::PVRMenuhook& hook) = 0;
    virtual void Addon_TriggerChannelUpdate() = 0;
    virtual void Addon_TriggerChannelGroupsUpdate() = 0;
    virtual void Addon_TriggerEpgUpdate(unsigned int channelUid) = 0;
    virtual void Addon_TriggerTimerUpdate() = 0;
};

class IPvrIptvDataSource : public ITimersEngineDelegate
{
public:
    virtual ADDON_STATUS Init(const std::string& clientPath, const std::string& userPath) = 0;
    virtual ADDON_STATUS GetStatus() = 0;

    virtual ADDON_STATUS SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue) = 0;

//    virtual const char *GetBackendName() = 0;
    virtual PVR_ERROR GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities) = 0;
    virtual PVR_ERROR SignalStatus(int channelUid, kodi::addon::PVRSignalStatus& signalStatus) = 0;
    
    virtual PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results) = 0;
    virtual int GetChannelsAmount() = 0;
    virtual PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) = 0;
    virtual bool OpenLiveStream(const kodi::addon::PVRChannel& channel) = 0;
    virtual void CloseLiveStream() = 0;
    virtual bool SwitchChannel(const PVR_CHANNEL &channel) = 0;
    virtual int GetChannelGroupsAmount() = 0;
    virtual PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) = 0;
    virtual PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group, kodi::addon::PVRChannelGroupMembersResultSet& results) = 0;

    virtual bool CanPauseStream() = 0;
    virtual bool CanSeekStream() = 0;
    virtual bool IsRealTimeStream() = 0;
    virtual PVR_ERROR GetStreamTimes(kodi::addon::PVRStreamTimes& times) = 0;
    virtual int64_t SeekLiveStream(int64_t position, int whence) = 0;
    virtual int64_t PositionLiveStream() = 0;
    virtual int64_t LengthLiveStream()  = 0;
    virtual int ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize) = 0;
    
    
    virtual PVR_ERROR GetRecordingsAmount(bool deleted, int& amount) = 0;
    virtual PVR_ERROR GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results) = 0;
    virtual PVR_ERROR DeleteRecording(const kodi::addon::PVRRecording& recording) = 0;
    virtual bool OpenRecordedStream(const kodi::addon::PVRRecording& recording) = 0;
    virtual void CloseRecordedStream() = 0;
    virtual PVR_ERROR GetStreamReadChunkSize(int& chunksize) = 0;
    virtual int ReadRecordedStream(unsigned char* buffer, unsigned int size) = 0;
    virtual int64_t SeekRecordedStream(int64_t position, int whence) = 0;
    virtual int64_t LengthRecordedStream() = 0;
    virtual PVR_ERROR IsEPGTagRecordable(const kodi::addon::PVREPGTag& tag, bool& isRecordable) = 0;

    virtual PVR_ERROR CallSettingsMenuHook(const kodi::addon::PVRMenuhook& menuhook) = 0;
    virtual PVR_ERROR CallChannelMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRChannel& item) = 0;
    virtual PVR_ERROR CallEPGMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVREPGTag& tag) = 0;
    virtual PVR_ERROR CallRecordingMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRRecording& item) = 0;
    virtual PVR_ERROR CallTimerMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRTimer& item) = 0;

    virtual  void OnSystemSleep() = 0;
    virtual void OnSystemWake() = 0;
    
    virtual ~IPvrIptvDataSource(){}
};

class ITimersEngine
{
public:
    virtual int GetTimersAmount() = 0;
    virtual PVR_ERROR AddTimer(const kodi::addon::PVRTimer& timer) = 0;
    virtual PVR_ERROR GetTimers(kodi::addon::PVRTimersResultSet& results) = 0;
    virtual PVR_ERROR DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete) = 0;
    virtual PVR_ERROR UpdateTimer(const kodi::addon::PVRTimer& timer) = 0;
    virtual ~ITimersEngine() {}

};

#endif /* __Iptv_Pvr_Addoin_h__ */
