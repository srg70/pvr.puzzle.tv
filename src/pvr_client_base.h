/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
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

#ifndef pvr_client_base_h
#define pvr_client_base_h

#include <string>
#include "pvr_client_types.h"
#include "xbmc_pvr_types.h"
#include "p8-platform/threads/mutex.h"
#include "addon.h"
#include "addon_settings.h"
#include "globals.hpp"

namespace Buffers {
    class IPlaylistBufferDelegate;
    class InputBuffer;
    class TimeshiftBuffer;
    class ICacheBuffer;
}
namespace ActionQueue {
    class CActionQueue;
}

namespace PvrClient
{
    
    class PVRClientBase: public IPvrIptvDataSource
    {
    public:
        
        static const unsigned int s_lastCommonMenuHookId;
        typedef enum {
            k_TimeshiftBufferMemory = 0,
            k_TimeshiftBufferFile = 1
        }TimeshiftBufferType;
        
        PVRClientBase();
        ADDON_STATUS Init(PVR_PROPERTIES* pvrprops);
        virtual ~PVRClientBase();
        
        ADDON_STATUS SetSetting(const char *settingName, const void *settingValue);
        
        PVR_ERROR GetAddonCapabilities(PVR_ADDON_CAPABILITIES* pCapabilities) = 0;
        
        bool CanPauseStream();
        bool CanSeekStream();
        bool IsRealTimeStream(void);
        PVR_ERROR GetStreamTimes(PVR_STREAM_TIMES *times);

        int GetChannelsAmount();
        PVR_ERROR GetChannels(ADDON_HANDLE handle, bool bRadio);

        int GetChannelGroupsAmount();
        PVR_ERROR GetChannelGroups(ADDON_HANDLE handle, bool bRadio);
        PVR_ERROR GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP& group);
        
        PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd);
        ADDON_STATUS GetStatus();
        
        bool OpenLiveStream(const PVR_CHANNEL& channel);
        void CloseLiveStream();
        int ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize);
        long long SeekLiveStream(long long iPosition, int iWhence);
        long long PositionLiveStream();
        long long LengthLiveStream();
        bool SwitchChannel(const PVR_CHANNEL& channel);

        
        uint64_t TimeshiftBufferSize() const;
        TimeshiftBufferType TypeOfTimeshiftBuffer() const;
        const std::string& TimeshiftPath() const;
        const std::string& RecordingsPath() const;

        int GetRecordingsAmount(bool deleted);
        PVR_ERROR GetRecordings(ADDON_HANDLE handle, bool deleted);
        PVR_ERROR DeleteRecording(const PVR_RECORDING &recording);
        void CloseRecordedStream(void);
        PVR_ERROR GetStreamReadChunkSize(int* chunksize);
        int ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize);
        long long SeekRecordedStream(long long iPosition, int iWhence);
        long long PositionRecordedStream(void);
        long long LengthRecordedStream(void);
        PVR_ERROR IsEPGTagRecordable(const EPG_TAG* tag, bool* bIsRecordable);

        bool StartRecordingFor(const PVR_TIMER &timer);
        bool StopRecordingFor(const PVR_TIMER &timer);
        bool FindEpgFor(const PVR_TIMER &timer);

        
        PVR_ERROR CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item);
        
        void OnSystemSleep();
        void OnSystemWake();
        PVR_ERROR SignalStatus(PVR_SIGNAL_STATUS &signalStatus);

    protected:
        IClientCore* m_clientCore;
        AddonSettingsDictionary& m_addonSettings;

        AddCurrentEpgToArchive HowToAddCurrentEpgToArchive() const;
        bool UseChannelGroupsForArchive() const;
        
        virtual PVR_ERROR  MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item);
        virtual ADDON_STATUS OnReloadEpg();
        virtual ADDON_STATUS OnReloadRecordings();

        virtual std::string GetStreamUrl(ChannelId channelId);
        virtual std::string GetNextStreamUrl(ChannelId channelId) {return std::string();}
        virtual void OnOpenStremFailed(PvrClient::ChannelId channelId, const std::string& streamUrl) {}
        ChannelId GetLiveChannelId() const { return  m_liveChannelId;}
        std::string GetLiveUrl() const;
        bool IsLiveInRecording() const;
        bool SwitchChannel(ChannelId channelId, const std::string& url);

        enum RecordingStreamFlags{
            NoRecordingFlags = 0x0,
            SupportVodSeek = 0x0001,
            ForcePlaylist = 0x0002
        };
        bool OpenRecordedStream(const std::string& url, Buffers::IPlaylistBufferDelegate* delegate, RecordingStreamFlags flags);
        bool IsLocalRecording(const PVR_RECORDING &recording) const;
        // Implemented for local recordings. Should be defined by derived class
        virtual bool OpenRecordedStream(const PVR_RECORDING &recording) = 0;
        
        // Recordingd
        uint32_t StartRecordingPadding() const;
        uint32_t EndRecordingPadding() const;

        
        const std::string& GetClientPath() const { return m_clientPath;}
        const std::string& GetUserPath() const { return m_userPath;}
        
        virtual ADDON_STATUS CreateCoreSafe(bool clearEpgCache) = 0;
        virtual void DestroyCoreSafe() = 0;
        
        void OnCoreCreated();
        
        static bool CheckPlaylistUrl(const std::string& url);
        
        ChannelId ChannelIdForBrodcastId(KodiChannelId uId) const {return m_kodiToPluginLut.at(uId);};
        KodiChannelId BrodcastIdForChannelId(ChannelId chId) const {return m_pluginToKodiLut.at(chId);};
        
        bool IsSeekSupported() const { return m_supportSeek; }
        void SetSeekSupported(bool yesNo) { m_supportSeek = yesNo; }
        
        bool IsArchiveSupported() const;
        
        virtual void PopulateSettings(AddonSettingsMutableDictionary& settings) = 0;

    private:
        typedef std::map<KodiChannelId, ChannelId> TKodiToPluginChannelIdLut;
        typedef std::map<ChannelId, KodiChannelId> TPluginToKodiChannelIdLut;

        void InitSettings();
        const ChannelList& GetChannelListWhenLutsReady();
        void Cleanup();
        
        uint64_t CacheSizeLimit() const;
        int ChannelReloadTimeout() const;
        bool IsTimeshiftEnabled() const;
        int RpcLocalPort() const;
        int ChannelIndexOffset() const;
        int WaitForInetTimeout() const;
        int StartupDelay() const;
        bool LoadArchiveAfterEpg() const;
        uint32_t ArchiveRefreshInterval() const;
        int LivePlaybackDelayForHls() const;
        int LivePlaybackDelayForTs() const;
        int LivePlaybackDelayForMulticast() const;

        void FillRecording(const EpgEntryList::value_type& epgEntry, PVR_RECORDING& tag, const char* dirPrefix);
        std::string DirectoryForRecording(unsigned int epgId) const;
        std::string PathForRecordingInfo(unsigned int epgId) const;
        static Buffers::InputBuffer*  BufferForUrl(const std::string& url );
        bool OpenLiveStream(ChannelId channelId, const std::string& url );
        Buffers::ICacheBuffer* CreateLiveCache() const;

        void ScheduleRecordingsUpdate();
        
        ChannelId m_liveChannelId;
        Buffers::TimeshiftBuffer *m_inputBuffer;
        struct {
            Buffers::InputBuffer * buffer;
            time_t duration;
            bool isLocal;
        } m_recordBuffer;
        ChannelId m_localRecordChannelId;
        Buffers::TimeshiftBuffer *m_localRecordBuffer;
        std::string m_cacheDir;
        int m_lastRecordingsAmount;        
        std::string m_clientPath;
        std::string m_userPath;
        mutable P8PLATFORM::CMutex m_mutex;
        int m_lastBytesRead;

        ActionQueue::CActionQueue* m_destroyer;
        P8PLATFORM::CEvent m_destroyerEvent;
        TKodiToPluginChannelIdLut m_kodiToPluginLut;
        TPluginToKodiChannelIdLut m_pluginToKodiLut;
        
        bool m_supportSeek;
        
        AddonSettingsMutableDictionary m_addonMutableSettings;

    };
}
#endif //pvr_client_base_h
