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

#include "pvr_client_types.h"
#include "xbmc_pvr_types.h"
#include <string>
#include "addon.h"
#include "globals.hpp"

namespace Buffers {
    class IPlaylistBufferDelegate;
    class InputBuffer;
}

namespace PvrClient
{
    
    class PVRClientBase: public IPvrIptvDataSource
    {
    public:
        
        typedef enum {
            k_TimeshiftBufferMemory = 0,
            k_TimeshiftBufferFile = 1
        }TimeshiftBufferType;
        
        ADDON_STATUS Init(PVR_PROPERTIES* pvrprops);
        virtual ~PVRClientBase();
        
        int GetSettings(ADDON_StructSetting ***sSet);
        ADDON_STATUS SetSetting(const char *settingName, const void *settingValue);
        void FreeSettings();
        
        PVR_ERROR GetAddonCapabilities(PVR_ADDON_CAPABILITIES* pCapabilities);
        bool CanPauseStream();
        bool CanSeekStream();
        
        int GetChannelsAmount();
        PVR_ERROR GetChannels(ADDON_HANDLE handle, bool bRadio);

        int GetChannelGroupsAmount();
        PVR_ERROR GetChannelGroups(ADDON_HANDLE handle, bool bRadio);
        PVR_ERROR GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP& group);
        
        PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd);
        ADDON_STATUS GetStatus();
        
        void CloseLiveStream();
        int ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize);
        long long SeekLiveStream(long long iPosition, int iWhence);
        long long PositionLiveStream();
        long long LengthLiveStream();
        
        
        void SetTimeshiftEnabled(bool enable);
        void SetTimeshiftBufferSize(uint64_t size);
        void SetTimeshiftBufferType(TimeshiftBufferType type);
        bool IsTimeshiftEnabled() { return m_isTimeshiftEnabled; }
        void SetTimeshiftPath(const std::string& path);
        
        int GetRecordingsAmount(bool deleted);
        PVR_ERROR GetRecordings(ADDON_HANDLE handle, bool deleted);
        void CloseRecordedStream(void);
        int ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize);
        long long SeekRecordedStream(long long iPosition, int iWhence);
        long long PositionRecordedStream(void);
        long long LengthRecordedStream(void);
        
        PVR_ERROR AddTimer(const PVR_TIMER &timer) { return PVR_ERROR_NOT_IMPLEMENTED; }
        int GetTimersAmount(void) { return -1; }
        PVR_ERROR GetTimers(ADDON_HANDLE handle) { return PVR_ERROR_NOT_IMPLEMENTED; }
        PVR_ERROR DeleteTimer(const PVR_TIMER &timer, bool bForceDelete) { return PVR_ERROR_NOT_IMPLEMENTED; }
        PVR_ERROR UpdateTimer(const PVR_TIMER &timer) { return PVR_ERROR_NOT_IMPLEMENTED; }

        
        PVR_ERROR CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item);
        
    protected:
        IClientCore* m_clientCore;
        
        virtual PVR_ERROR  MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item);
        virtual ADDON_STATUS OnReloadEpg();
        virtual ADDON_STATUS OnReloadRecordings();

        bool OpenLiveStream(const std::string& url );
        bool OpenRecordedStream(const std::string& url, Buffers::IPlaylistBufferDelegate* delegate);
        bool SwitchChannel(const std::string& url);
        const std::string& GetClientPath() const { return m_clientPath;}
        const std::string& GetUserPath() const { return m_userPath;}
        
    private:
        
        void SetChannelReloadTimeout(int timeout);
        
        Buffers::InputBuffer *m_inputBuffer;
        Buffers::InputBuffer *m_recordBuffer;
        bool m_isTimeshiftEnabled;
        uint64_t m_timshiftBufferSize;
        TimeshiftBufferType m_timeshiftBufferType;
        std::string m_CacheDir;
        std::string m_clientPath;
        std::string m_userPath;
        int m_channelReloadTimeout;
    };
}
#endif //pvr_client_base_h
