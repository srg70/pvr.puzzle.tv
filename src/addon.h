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

#include "kodi/AddonBase.h"
#include "xbmc_addon_types.h"
#include "xbmc_pvr_types.h"


class ITimersEngineDelegate{
public:
    virtual bool StartRecordingFor(const PVR_TIMER &timer) = 0;
    virtual bool StopRecordingFor(const PVR_TIMER &timer) = 0;
protected:
    virtual ~ITimersEngineDelegate() {}
};

namespace Buffers {
    class IPlaylistBufferDelegate;
}

namespace PvrClient
{

    class IChannelsSource
    {
    public:
        virtual int GetChannelsAmount(void) = 0;
        virtual PVR_ERROR GetChannels(ADDON_HANDLE handle, bool bRadio) = 0;
        
        virtual int GetChannelGroupsAmount(void) = 0;
        virtual PVR_ERROR GetChannelGroups(ADDON_HANDLE handle, bool bRadio) = 0;
        virtual PVR_ERROR GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP &group) = 0;
        
    protected:
        virtual ~IChannelsSource() {}
        
        
    };
    
    class IEpgSource
    {
    public:
        virtual PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL &channel, time_t iStart, time_t iEnd) = 0;
        virtual PVR_ERROR IsEPGTagRecordable(const EPG_TAG* tag, bool* bIsRecordable) = 0;
    protected:
        virtual ~IEpgSource() {}
    };
    
    class IRecordingsSource{
    public:
        virtual int GetRecordingsAmount(bool deleted) = 0;
        virtual PVR_ERROR GetRecordings(ADDON_HANDLE handle, bool deleted) = 0;
        virtual PVR_ERROR DeleteRecording(const PVR_RECORDING &recording) = 0;
        
    protected:
        virtual ~IRecordingsSource() {}
    };
    
    class ILivePlayer {
    public:
        virtual bool OpenLiveStream(const PVR_CHANNEL &channel) = 0;
        virtual void CloseLiveStream(void) = 0;
        virtual bool SwitchChannel(const PVR_CHANNEL &channel) = 0;
        virtual bool CanPauseStream(void) = 0;
        virtual bool CanSeekStream(void) = 0;
        virtual bool IsRealTimeStream(void) = 0;
        virtual PVR_ERROR GetStreamTimes(PVR_STREAM_TIMES *times) = 0;
        virtual long long SeekLiveStream(long long iPosition, int iWhence) = 0;
        virtual long long PositionLiveStream(void) = 0;
        virtual long long LengthLiveStream(void)  = 0;
        virtual int ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize) = 0;
        
        
    protected:
        virtual ~ILivePlayer() {}
    };
    
    class IRecordingPlayer {
    public:
        virtual bool OpenRecordedStream(const PVR_RECORDING &recording) = 0;
        virtual void CloseRecordedStream(void) = 0;
        virtual PVR_ERROR GetStreamReadChunkSize(int* chunksize) = 0;
        virtual int ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize) = 0;
        virtual long long SeekRecordedStream(long long iPosition, int iWhence /* = SEEK_SET */) = 0;
        virtual long long PositionRecordedStream(void) = 0;
        virtual long long LengthRecordedStream(void) = 0;
        
    protected:
        virtual ~IRecordingPlayer() {}
    };
    
    class IPlayerDelegate {
    public:
        // Although Kodi defines unique channel ID as unsigned integer
        // some Kodi modules require signed int internaly and reject negative values.
        typedef int KodiChannelId;

        typedef enum {
            k_TimeshiftBufferNone = -1,
            k_TimeshiftBufferMemory = 0,
            k_TimeshiftBufferFile = 1
        } TimeshiftBufferType;

        typedef enum {
            NoRecordingFlags = 0x0,
            SupportVodSeek = 0x0001,
            ForcePlaylist = 0x0002,
            LocalRecording = 0x004
        } RecordingStreamFlags;

        typedef struct _RecordingParams{
            std::string url;
            Buffers::IPlaylistBufferDelegate* delegate;
            RecordingStreamFlags flags;
        } RecordingParams;
        virtual std::string GetLiveStreamUrl(const KodiChannelId& channel) = 0;
        virtual TimeshiftBufferType GetTimeshiftBufferType() const = 0;
        virtual std::string InitLocalRecordingFor(const PVR_TIMER &timer) = 0;
        virtual void FinalizeLocalRecordingFor(const PVR_TIMER &timer) = 0;
        virtual bool GetRecordingParams(const PVR_RECORDING &recording, RecordingParams& params) = 0;

    protected:
        virtual ~IPlayerDelegate() {}
    };
    
    class IPlayer : public ILivePlayer, public IRecordingPlayer, public ITimersEngineDelegate {
    public:
        virtual void SetLiveDelegate(IPlayerDelegate* delegate) = 0;
    protected:
        virtual ~IPlayer() {}
    };
    
    
    
    class IPvrIptvDataSource : public IChannelsSource, public IEpgSource,  public IRecordingsSource, public IPlayerDelegate
    {
    public:
        virtual ADDON_STATUS Init(PVR_PROPERTIES* pvrprops) = 0;
        virtual ADDON_STATUS GetStatus() = 0;
        
        virtual ADDON_STATUS SetSetting(const char *settingName, const void *settingValue) = 0;
        
        virtual PVR_ERROR GetAddonCapabilities(PVR_ADDON_CAPABILITIES* pCapabilities) = 0;
        virtual PVR_ERROR SignalStatus(PVR_SIGNAL_STATUS &signalStatus) = 0;
        
        virtual PVR_ERROR CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item) = 0;
        
        virtual  void OnSystemSleep() = 0;
        virtual void OnSystemWake() = 0;
        
        virtual ~IPvrIptvDataSource(){}
    };
}

class ITimersEngine
{
public:
    virtual int GetTimersAmount(void) = 0;
    virtual PVR_ERROR AddTimer(const PVR_TIMER &timer) = 0;
    virtual PVR_ERROR GetTimers(ADDON_HANDLE handle) = 0;
    virtual PVR_ERROR DeleteTimer(const PVR_TIMER &timer, bool bForceDelete) = 0;
    virtual PVR_ERROR UpdateTimer(const PVR_TIMER &timer) = 0;
    virtual ~ITimersEngine() {}

};

//class PuzzleTvAddon : public ::kodi::addon::CAddonBase
//{
//public:
//    virtual ~PuzzleTvAddon() override;
//    
//    virtual ADDON_STATUS Create() override;
//    
//    virtual ADDON_STATUS GetStatus() override;
//    
//    virtual ADDON_STATUS SetSetting(const std::string& settingName, const ::kodi::CSettingValue& settingValue) override;
//
//    
//};

#endif /* __Iptv_Pvr_Addoin_h__ */
