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
        ADDON_STATUS Init(const std::string& clientPath, const std::string& userPath) override;
        virtual ~PVRClientBase();
        
        ADDON_STATUS SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue) override;
        
        PVR_ERROR GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities) override = 0;
        
        bool CanPauseStream() override;
        bool CanSeekStream() override;
        bool IsRealTimeStream(void) override;
        PVR_ERROR GetStreamTimes(kodi::addon::PVRStreamTimes& times) override;

        int GetChannelsAmount() override;
        PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override;

        int GetChannelGroupsAmount() override;
        PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& result) override;
        PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group, kodi::addon::PVRChannelGroupMembersResultSet& results) override;
        
        PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results) override;
        ADDON_STATUS GetStatus() override;
        
        bool OpenLiveStream(const kodi::addon::PVRChannel& channel) override;
        void CloseLiveStream() override;
        int ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize) override;
        long long SeekLiveStream(long long iPosition, int iWhence) override;
        long long PositionLiveStream() override;
        long long LengthLiveStream() override;
        bool SwitchChannel(const PVR_CHANNEL& channel) override;

        
        uint64_t TimeshiftBufferSize() const;
        TimeshiftBufferType TypeOfTimeshiftBuffer() const;
        const std::string& TimeshiftPath() const;
        const std::string& RecordingsPath() const;

        PVR_ERROR GetRecordingsAmount(bool deleted, int& amount) override;
        PVR_ERROR GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results) override;
        PVR_ERROR DeleteRecording(const kodi::addon::PVRRecording& recording) override;
        void CloseRecordedStream(void) override;
        PVR_ERROR GetStreamReadChunkSize(int& chunksize) override;
        int ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize) override;
        long long SeekRecordedStream(long long iPosition, int iWhence) override;
        long long PositionRecordedStream(void);
        long long LengthRecordedStream(void) override;
        PVR_ERROR IsEPGTagRecordable(const kodi::addon::PVREPGTag& tag, bool& isRecordable) override;

        bool StartRecordingFor(kodi::addon::PVRTimer &timer) override;
        bool StopRecordingFor(kodi::addon::PVRTimer &timer) override;
        bool FindEpgFor(kodi::addon::PVRTimer &timer) override;

        
        PVR_ERROR CallSettingsMenuHook(const kodi::addon::PVRMenuhook& menuhook) override;
        PVR_ERROR CallChannelMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRChannel& item) override;
        PVR_ERROR CallEPGMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVREPGTag& tag) override;
        PVR_ERROR CallRecordingMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRRecording& item) override;
        PVR_ERROR CallTimerMenuHook(const kodi::addon::PVRMenuhook& menuhook, const kodi::addon::PVRTimer& item) override;

        void OnSystemSleep() override;
        void OnSystemWake() override;
        PVR_ERROR SignalStatus(int /*channelUid*/, kodi::addon::PVRSignalStatus& signalStatus) override;
        
    protected:
        IClientCore* m_clientCore;
        AddonSettingsDictionary& m_addonSettings;

        AddCurrentEpgToArchive HowToAddCurrentEpgToArchive() const;
        int EpgCorrectionShift() const;
        const std::string& LocalLogosFolder() const;
        bool UseChannelGroupsForArchive() const;
        
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
        bool IsLocalRecording(const kodi::addon::PVRRecording& recording) const;
        // Implemented for local recordings. Should be defined by derived class
        virtual bool OpenRecordedStream(const kodi::addon::PVRRecording& recording) override = 0;
        
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
        
        PVR_ERROR HandleCommonMenuHook(const kodi::addon::PVRMenuhook &menuhook);
        void RegisterCommonMenuHook(unsigned int hookId, unsigned int localizedStringId);

        uint64_t CacheSizeLimit() const;
        int ChannelReloadTimeout() const;
        bool IsTimeshiftEnabled() const;
        int RpcLocalPort() const;
        const std::string& RpcUser() const;
        const std::string& RpcPassword() const;
        bool RpcEnableSsl() const;
        int ChannelIndexOffset() const;
        int WaitForInetTimeout() const;
        int StartupDelay() const;
        bool LoadArchiveAfterEpg() const;
        uint32_t ArchiveRefreshInterval() const;
        int LivePlaybackDelayForHls() const;
        int LivePlaybackDelayForTs() const;
        int LivePlaybackDelayForMulticast() const;
        bool SeekArchivePadding() const;
        bool SuppotMulticastUrls() const;
        const std::string& UdpProxyHost() const;
        uint32_t UdpProxyPort() const;
        
        void FillRecording(const EpgEntryList::value_type& epgEntry, kodi::addon::PVRRecording& tag, const char* dirPrefix);
        std::string DirectoryForRecording(unsigned int epgId) const;
        std::string PathForRecordingInfo(unsigned int epgId) const;
        static Buffers::InputBuffer*  BufferForUrl(const std::string& url );
        bool OpenLiveStream(ChannelId channelId, const std::string& url );
        Buffers::ICacheBuffer* CreateLiveCache() const;

        void ScheduleRecordingsUpdate();
        void SeekKodiPlayerAsyncToOffset(int offsetInSeconds, std::function<void(bool done)> result);

        ChannelId m_liveChannelId;
        Buffers::TimeshiftBuffer *m_inputBuffer;
        struct {
            Buffers::InputBuffer * buffer;
            time_t duration;
            bool isLocal;
            unsigned int seekToSec;
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
