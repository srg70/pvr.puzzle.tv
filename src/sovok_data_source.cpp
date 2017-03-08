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

#include <stdio.h>
#include "kodi/xbmc_addon_cpp_dll.h"
#include "kodi/libXBMC_addon.h"
#include "kodi/libXBMC_pvr.h"
#include "p8-platform/util/util.h"
//#include <xbmc_pvr_dll.h>
//#include <libXBMC_pvr.h>
//#include <libXBMC_gui.h>
//#include "config.h"
#include "sovok_pvr_client.h"
#include "sovok_data_source.h"


#include "p8-platform/util/util.h"

using namespace ADDON;
using namespace std;


SovokTvDataSource::SovokTvDataSource()
: m_client(NULL), m_xbmc(NULL), m_pvr(NULL)
{
}
ADDON_STATUS SovokTvDataSource::Init(void *callbacks, void* props)
{
    m_xbmc = new CHelper_libXBMC_addon();
    if (!m_xbmc->RegisterMe(callbacks))
    {
        SAFE_DELETE(m_xbmc);
        return ADDON_STATUS_PERMANENT_FAILURE;
    }

    m_pvr = new CHelper_libXBMC_pvr();
    if (!m_pvr->RegisterMe(callbacks))
    {
        SAFE_DELETE(m_pvr);
        SAFE_DELETE(m_xbmc);
        return ADDON_STATUS_PERMANENT_FAILURE;
    }

   PVR_PROPERTIES* pvrprops = (PVR_PROPERTIES*)props;
    m_xbmc->Log(LOG_DEBUG, "User path: %s", pvrprops->strUserPath);
    m_xbmc->Log(LOG_DEBUG, "Client path: %s", pvrprops->strClientPath);
    //auto g_strUserPath   = pvrprops->strUserPath;
    m_clientPath = pvrprops->strClientPath;
    m_userPath = pvrprops->strUserPath;

    char buffer[1024];

    if (m_xbmc->GetSetting("login", &buffer))
        m_login = buffer;
    if (m_xbmc->GetSetting("password", &buffer))
        m_password = buffer;
    m_xbmc->GetSetting("enable_timeshift", &m_isTimeshiftEnabled);
    m_xbmc->GetSetting("add_favorites_group", &m_shouldAddFavoritesGroup);

    try
    {
        CreateCore();
 
        m_client->SetTimeshiftEnabled(m_isTimeshiftEnabled);
        m_client->SetAddFavoritesGroup(m_shouldAddFavoritesGroup);

        std::string streamer;
        if (m_xbmc->GetSetting("streamer", &buffer))
            streamer = buffer;
        
        auto streamersList = m_client->GetStreamersList();
        auto current = streamersList[m_client->GetStreamerId()];
        if (current != streamer)
        {
            m_xbmc->QueueNotification(QUEUE_WARNING, "Streamer setting mismatch.");
            return ADDON_STATUS_OK;
        }
 
    }
    catch (AuthFailedException &)
    {
        m_xbmc->QueueNotification(QUEUE_ERROR, "Login to Sovok.TV failed.");
    }

    catch(MissingApiException & ex)
    {
        m_xbmc->QueueNotification(QUEUE_WARNING, (std::string("Missing Sovok API: ") + ex.reason).c_str());
    }
    
//    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
//    m_pvr->AddMenuHook(&hook);
    return ADDON_STATUS_OK;
}

void SovokTvDataSource::CreateCore()
{
    if(m_client != NULL)
        SAFE_DELETE(m_client);
    m_client = new SovokPVRClient(m_xbmc, m_pvr, m_login, m_password);
    
    auto streamersList = m_client->GetStreamersList();
    auto strimmersPath = m_clientPath;
    strimmersPath.append("/").append("resources/").append("streamers/");
    if(!m_xbmc->DirectoryExists(strimmersPath.c_str()))
        m_xbmc->CreateDirectory(strimmersPath.c_str());
    std::for_each(streamersList.begin(), streamersList.end(), [&](StreamerNamesList::value_type &s)
                  {
                      
                      auto filename = strimmersPath+s;
                      if(!m_xbmc->FileExists(filename.c_str(), true))
                      {
                          void* f = m_xbmc->OpenFileForWrite(filename.c_str(), true);
                          if(f) m_xbmc->CloseFile(f);
                      }
                  });
    
}

ADDON_STATUS SovokTvDataSource::GetStatus()
{
    return  ADDON_STATUS_OK;
}

void SovokTvDataSource::Destroy()
{
    if(m_client)
        SAFE_DELETE(m_client);
    if(m_xbmc)
        SAFE_DELETE(m_xbmc);
    if(m_pvr)
        SAFE_DELETE(m_pvr);
}


static ADDON_StructSetting ** g_sovokSettings = NULL;
static int g_sovokSettingsSize = 0;

int SovokTvDataSource::GetSettings(ADDON_StructSetting ***sSet)
{
    return 0;
    /*
    if(NULL == g_sovokSettings)
    {
        std::vector<DllSetting> sovokSettings;
        
        DllSetting timeshift(DllSetting::CHECK, "enable_timeshift", "Enable Timeshift" );
        timeshift.current =  (m_client) ? m_client->IsTimeshiftEnabled() : 0;
        sovokSettings.push_back(timeshift);
        
        DllSetting addFaorites(DllSetting::CHECK, "add_favorites_group", "Add Favorites Group" );
        addFaorites.current =  (m_client) ? m_client->ShouldAddFavoritesGroup() : 0;
        sovokSettings.push_back(addFaorites);

        DllSetting streamer(DllSetting::SPIN, "streamer", "Streamer" );
        streamer.current =  0;//m_client->GetStreamerId();
        if(m_client)
        {
            auto streamersList = m_client->GetStreamersList();
            auto current = streamersList[m_client->GetStreamerId()];
            int counter = 0;
            std::for_each(streamersList.begin(), streamersList.end(), [&](StreamerNamesList::value_type &s)
            {
                if (s == current)
                    streamer.current = counter;
                ++counter;

                streamer.AddEntry(s.c_str());
            });
        }
        sovokSettings.push_back(streamer);
        
        g_sovokSettingsSize = DllUtils::VecToStruct(sovokSettings, &g_sovokSettings);
    }
    *sSet = g_sovokSettings;
    return g_sovokSettingsSize;
     */
}

ADDON_STATUS SovokTvDataSource::SetSetting(const char *settingName, const void *settingValue)
{
    if (strcmp(settingName, "login") == 0 && strcmp((const char*) settingValue, m_login.c_str()) != 0)
    {
        m_login = (const char*) settingValue;
       if (!m_login.empty() && !m_password.empty() && m_client == NULL)
        {
            try {
                CreateCore();
            }catch (AuthFailedException &) {
                m_xbmc->QueueNotification(QUEUE_ERROR, "Login to Sovok.TV failed.");
            }
        }
    }
    else if (strcmp(settingName, "password") == 0  && strcmp((const char*) settingValue, m_password.c_str()) != 0)
    {
        m_password = (const char*) settingValue;
        if (!m_login.empty() && !m_password.empty() && m_client == NULL)
        {
            try {
                CreateCore();
            }catch (AuthFailedException &) {
                m_xbmc->QueueNotification(QUEUE_ERROR, "Login to Sovok.TV failed.");
            }
        }
    }
    else if (strcmp(settingName, "enable_timeshift") == 0)
    {
        m_isTimeshiftEnabled = *((const bool*)settingValue);
        if(m_client)
            m_client->SetTimeshiftEnabled(*(bool *)(settingValue));
    }
 
    else if (strcmp(settingName, "add_favorites_group") == 0)
    {
        m_shouldAddFavoritesGroup = *((bool*)settingValue);
        if(m_client)
            m_client->SetAddFavoritesGroup(*(bool *)(settingValue));
    }

    else if (strcmp(settingName, "streamer") == 0)
    {
        m_strimmer = (const char*)settingValue;
        if(m_client != NULL)
        {
            auto streamersList = m_client->GetStreamersList();
            int currentId = 0;
            std::find_if(streamersList.begin(), streamersList.end(), [&](StreamerNamesList::value_type &s) {
                if(s == m_strimmer)
                    return true;
                ++currentId;
                return false;
            });
            if(currentId == streamersList.size() )
            {
                m_xbmc->QueueNotification(QUEUE_WARNING, "Streamer setting mismatch.");
            }
            m_client->SetStreamerId(currentId);
        }
    }

    return ADDON_STATUS_NEED_RESTART;
}

void SovokTvDataSource::FreeSettings()
{
    if(g_sovokSettings && g_sovokSettingsSize)
        DllUtils::FreeStruct(g_sovokSettingsSize, &g_sovokSettings);
    g_sovokSettingsSize = 0;
    g_sovokSettings = NULL;
}

PVR_ERROR SovokTvDataSource::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    pCapabilities->bSupportsRadio = true;
    pCapabilities->bSupportsChannelGroups = true;
    pCapabilities->bHandlesInputStream = true;
    pCapabilities->bSupportsRecordings = true;

    pCapabilities->bSupportsTimers = false;
    pCapabilities->bSupportsChannelScan = false;
    pCapabilities->bHandlesDemuxing = false;
    pCapabilities->bSupportsRecordingPlayCount = false;
    pCapabilities->bSupportsLastPlayedPosition = false;
    pCapabilities->bSupportsRecordingEdl = false;

    return PVR_ERROR_NO_ERROR;
}

PVR_ERROR SovokTvDataSource::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Sovok TV Adapter 1");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), "OK");
    
    return PVR_ERROR_NO_ERROR;

}


PVR_ERROR SovokTvDataSource::GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd)
{
    return (m_client == NULL)? PVR_ERROR_SERVER_ERROR : m_client->GetEPGForChannel(handle, channel, iStart, iEnd);
}

int SovokTvDataSource::GetChannelGroupsAmount()
{
    return (m_client == NULL)? -1 : m_client->GetChannelGroupsAmount();
}

PVR_ERROR SovokTvDataSource::GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
    return (m_client == NULL)? PVR_ERROR_SERVER_ERROR : m_client->GetChannelGroups(handle, bRadio);
}

PVR_ERROR SovokTvDataSource::GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP& group)
{
    return (m_client == NULL)? PVR_ERROR_SERVER_ERROR : m_client->GetChannelGroupMembers(handle, group);
}

int SovokTvDataSource::GetChannelsAmount()
{
    return (m_client == NULL)? -1 : m_client->GetChannelsAmount();
}

PVR_ERROR SovokTvDataSource::GetChannels(ADDON_HANDLE handle, bool bRadio)
{
    return (m_client == NULL)? PVR_ERROR_SERVER_ERROR : m_client->GetChannels(handle, bRadio);
}

bool SovokTvDataSource::OpenLiveStream(const PVR_CHANNEL& channel)
{
    return (m_client == NULL)? false : m_client->OpenLiveStream(channel);
}

void SovokTvDataSource::CloseLiveStream()
{
    m_client->CloseLiveStream();
}

int SovokTvDataSource::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    return (m_client == NULL)? -1 : m_client->ReadLiveStream(pBuffer, iBufferSize);
}

bool SovokTvDataSource::SwitchChannel(const PVR_CHANNEL& channel)
{
    return (m_client == NULL)? false : m_client->SwitchChannel(channel);
}

bool SovokTvDataSource::CanPauseStream()
{
    return (m_client == NULL)? false : m_client->IsTimeshiftEnabled();
}

bool SovokTvDataSource::CanSeekStream()
{
    return (m_client == NULL)? false : m_client->IsTimeshiftEnabled();
}

long long SovokTvDataSource::SeekLiveStream(long long iPosition, int iWhence)
{
    return (m_client == NULL)? -1 : m_client->SeekLiveStream(iPosition, iWhence);
}

long long SovokTvDataSource::PositionLiveStream()
{
    return (m_client == NULL)? -1 : m_client->PositionLiveStream();
}

long long SovokTvDataSource::LengthLiveStream()
{
    return (m_client == NULL)? -1 : m_client->LengthLiveStream();
}


int SovokTvDataSource::GetRecordingsAmount(bool deleted)
{
    return (m_client == NULL)? -1 : m_client->GetRecordingsAmount(deleted);
}
PVR_ERROR SovokTvDataSource::GetRecordings(ADDON_HANDLE handle, bool deleted)
{
    return (m_client == NULL)? PVR_ERROR_SERVER_ERROR : m_client->GetRecordings(handle, deleted);

}
bool SovokTvDataSource::OpenRecordedStream(const PVR_RECORDING &recording)
{
    return (m_client == NULL)? false : m_client->OpenRecordedStream(recording);

}
void SovokTvDataSource::CloseRecordedStream(void)
{
    m_client->CloseRecordedStream();

}
int SovokTvDataSource::ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
    return (m_client == NULL)? -1 : m_client->ReadRecordedStream(pBuffer, iBufferSize);
    
}

long long SovokTvDataSource::SeekRecordedStream(long long iPosition, int iWhence)
{
    return (m_client == NULL)? -1 : m_client->SeekRecordedStream(iPosition, iWhence);

}
long long SovokTvDataSource::PositionRecordedStream(void)
{
    return (m_client == NULL)? -1 : m_client->PositionRecordedStream();

}
long long SovokTvDataSource::LengthRecordedStream(void)
{
    return (m_client == NULL)? -1 : m_client->LengthRecordedStream();

}



PVR_ERROR SovokTvDataSource::CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    m_xbmc->Log(LOG_DEBUG, " >>>> !!!! Menu hook !!!! <<<<<");
    return (m_client == NULL)? PVR_ERROR_SERVER_ERROR : m_client->MenuHook(menuhook, item);
}
