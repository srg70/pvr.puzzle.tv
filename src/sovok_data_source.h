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

#ifndef sovok_client_h
#define sovok_client_h


#include "addon.h"
#include <string>

class SovokPVRClient;
class CHelper_libXBMC_pvr;
namespace ADDON
{
class CHelper_libXBMC_addon;
}

class SovokTvDataSource : public IPvrIptvDataSource
{
public:
    SovokTvDataSource();
    virtual ADDON_STATUS Init(void* hdl, void* props);
    virtual ADDON_STATUS GetStatus();
    virtual void Destroy();
    
    virtual int GetSettings(ADDON_StructSetting ***sSet);
    virtual ADDON_STATUS SetSetting(const char *settingName, const void *settingValue);
    virtual void FreeSettings();

//    virtual const char *GetBackendName(void);
    virtual PVR_ERROR GetAddonCapabilities(PVR_ADDON_CAPABILITIES* pCapabilities);
    virtual PVR_ERROR SignalStatus(PVR_SIGNAL_STATUS &signalStatus);

    virtual PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL &channel, time_t iStart, time_t iEnd);
    virtual int GetChannelsAmount(void);
    virtual PVR_ERROR GetChannels(ADDON_HANDLE handle, bool bRadio);
    virtual bool OpenLiveStream(const PVR_CHANNEL &channel);
    virtual void CloseLiveStream(void);
    virtual bool SwitchChannel(const PVR_CHANNEL &channel);
    virtual int GetChannelGroupsAmount(void);
    virtual PVR_ERROR GetChannelGroups(ADDON_HANDLE handle, bool bRadio);
    virtual PVR_ERROR GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP &group);
    virtual bool CanPauseStream(void);
    virtual bool CanSeekStream(void);
    virtual long long SeekLiveStream(long long iPosition, int iWhence);
    virtual long long PositionLiveStream(void);
    virtual long long LengthLiveStream(void);
    virtual int ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize);
    
    virtual int GetRecordingsAmount(bool deleted);
    virtual PVR_ERROR GetRecordings(ADDON_HANDLE handle, bool deleted);
    virtual bool OpenRecordedStream(const PVR_RECORDING &recording);
    virtual void CloseRecordedStream(void);
    virtual int ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize);
    virtual long long SeekRecordedStream(long long iPosition, int iWhence);
    virtual long long PositionRecordedStream(void);
    virtual long long LengthRecordedStream(void);

    
    virtual PVR_ERROR CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item);
private:
    void CreateCore();
    
    std::string m_login;
    std::string m_password;
    std::string m_strimmer;
    bool m_isTimeshiftEnabled;
    bool m_shouldAddFavoritesGroup;
    std::string m_clientPath;
    std::string m_userPath;
    SovokPVRClient *m_client = NULL;

    ADDON::CHelper_libXBMC_addon *m_xbmc;
    CHelper_libXBMC_pvr *m_pvr;

};


#endif /* sovok_client_h */
