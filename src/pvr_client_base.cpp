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
#endif

#include "p8-platform/util/util.h"
#include "kodi/xbmc_addon_cpp_dll.h"

#include "libXBMC_pvr.h"
#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "helpers.h"
#include "pvr_client_base.h"

using namespace std;
using namespace ADDON;

// NOTE: avoid '.' (dot) char in path. Causes to deadlock in Kodi code.
const char* s_DefaultCacheDir = "special://temp/pvr-puzzle-tv";

ADDON_STATUS PVRClientBase::Init(CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                                  PVR_PROPERTIES* pvrprops)
{
    
    
    m_addonHelper = addonHelper;
    m_pvrHelper = pvrHelper;
    m_inputBuffer = NULL;
    SetTimeshiftPath(s_DefaultCacheDir);
    
    m_addonHelper->Log(LOG_DEBUG, "User path: %s", pvrprops->strUserPath);
    m_addonHelper->Log(LOG_DEBUG, "Client path: %s", pvrprops->strClientPath);
    //auto g_strUserPath   = pvrprops->strUserPath;
    m_clientPath = pvrprops->strClientPath;
    m_userPath = pvrprops->strUserPath;
    
    char buffer[1024];
    
    bool isTimeshiftEnabled;
    m_addonHelper->GetSetting("enable_timeshift", &isTimeshiftEnabled);
    string timeshiftPath;
    if (m_addonHelper->GetSetting("timeshift_path", &buffer))
        timeshiftPath = buffer;
    
    
    SetTimeshiftEnabled(isTimeshiftEnabled);
    SetTimeshiftPath(timeshiftPath);
    
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return ADDON_STATUS_OK;
    
}

PVRClientBase::~PVRClientBase()
{
    CloseLiveStream();
    CloseRecordedStream();
}

static ADDON_StructSetting ** g_sovokSettings = NULL;
static int g_sovokSettingsSize = 0;

int PVRClientBase::GetSettings(ADDON_StructSetting ***sSet)
{
    return 0;
    /*
     if(NULL == g_sovokSettings)
     {
     std::vector<DllSetting> sovokSettings;
     
     DllSetting timeshift(DllSetting::CHECK, "enable_timeshift", "Enable Timeshift" );
     timeshift.current =  (m_sovokTV) ? IsTimeshiftEnabled() : 0;
     sovokSettings.push_back(timeshift);
     
     DllSetting addFaorites(DllSetting::CHECK, "add_favorites_group", "Add Favorites Group" );
     addFaorites.current =  (m_sovokTV) ? ShouldAddFavoritesGroup() : 0;
     sovokSettings.push_back(addFaorites);
     
     DllSetting streamer(DllSetting::SPIN, "streamer", "Streamer" );
     streamer.current =  0;//GetStreamerId();
     if(m_sovokTV)
     {
     auto streamersList = GetStreamersList();
     auto current = streamersList[GetStreamerId()];
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

ADDON_STATUS PVRClientBase::SetSetting(const char *settingName, const void *settingValue)
{
    if (strcmp(settingName, "enable_timeshift") == 0)
    {
          SetTimeshiftEnabled(*(bool *)(settingValue));
    }
    else if (strcmp(settingName, "timeshift_path") == 0)
    {
          SetTimeshiftPath((const char *)(settingValue));
    }
    return ADDON_STATUS_NEED_RESTART;
}

void PVRClientBase::FreeSettings()
{
    if(g_sovokSettings && g_sovokSettingsSize)
        DllUtils::FreeStruct(g_sovokSettingsSize, &g_sovokSettings);
    g_sovokSettingsSize = 0;
    g_sovokSettings = NULL;
}

PVR_ERROR PVRClientBase::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    //pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    //pCapabilities->bSupportsRadio = true;
    //pCapabilities->bSupportsChannelGroups = true;
    pCapabilities->bHandlesInputStream = true;
    pCapabilities->bSupportsRecordings = true;
    
//    pCapabilities->bSupportsTimers = false;
//    pCapabilities->bSupportsChannelScan = false;
//    pCapabilities->bHandlesDemuxing = false;
//    pCapabilities->bSupportsRecordingPlayCount = false;
//    pCapabilities->bSupportsLastPlayedPosition = false;
//    pCapabilities->bSupportsRecordingEdl = false;
    
    return PVR_ERROR_NO_ERROR;
}

bool PVRClientBase::CanPauseStream()
{
    return IsTimeshiftEnabled();
}

bool PVRClientBase::CanSeekStream()
{
    return IsTimeshiftEnabled();
}

ADDON_STATUS PVRClientBase::GetStatus()
{
    return  /*(m_sovokTV == NULL)? ADDON_STATUS_LOST_CONNECTION : */ADDON_STATUS_OK;
}

void PVRClientBase::SetTimeshiftPath(const std::string& path){
    const char* nonEmptyPath = (path.empty()) ? s_DefaultCacheDir : path.c_str();
    if(!m_addonHelper->DirectoryExists(nonEmptyPath))
        if(!m_addonHelper->CreateDirectory(nonEmptyPath))
            m_addonHelper->Log(LOG_ERROR, "Failed to create cache folder");
    m_CacheDir = nonEmptyPath;
}

PVR_ERROR  PVRClientBase::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    //    SovokEpgEntry epgEntry;
    //    m_sovokTV->FindEpg(item.data.iEpgUid, epgEntry);
    return PVR_ERROR_NOT_IMPLEMENTED;
    
}

bool PVRClientBase::OpenLiveStream(const std::string& url )
{
    if (url.empty())
        return false;
    try
    {
        if (m_isTimeshiftEnabled)
        {
            m_inputBuffer = new TimeshiftBuffer(m_addonHelper, url, m_CacheDir);
        }
        else
            m_inputBuffer = new DirectBuffer(m_addonHelper, url);
    }
    catch (InputBufferException &)
    {
        return false;
    }
    
    return true;
}

void PVRClientBase::CloseLiveStream()
{
    if(m_inputBuffer) {
        m_addonHelper->Log(LOG_NOTICE, "PVRClientBase: closing input sream...");
        SAFE_DELETE(m_inputBuffer);
        m_addonHelper->Log(LOG_NOTICE, "PVRClientBase: input sream closed.");
    }
}

int PVRClientBase::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    return m_inputBuffer->Read(pBuffer, iBufferSize);
}

long long PVRClientBase::SeekLiveStream(long long iPosition, int iWhence)
{
    return m_inputBuffer->Seek(iPosition, iWhence);
}

long long PVRClientBase::PositionLiveStream()
{
    return m_inputBuffer->GetPosition();
}

long long PVRClientBase::LengthLiveStream()
{
    return m_inputBuffer->GetLength();
}

bool PVRClientBase::SwitchChannel(const std::string& url)
{
    return m_inputBuffer->SwitchStream(url);
}

void PVRClientBase::SetTimeshiftEnabled(bool enable)
{
    m_isTimeshiftEnabled = enable;
}

bool PVRClientBase::OpenRecordedStream(const std::string& url)
{
     if (url.empty())
        return false;
    
    try
    {
        //        if (m_isTimeshiftEnabled)
        //        {
        //            m_recordBuffer = new TimeshiftBuffer(m_addonHelper, url, m_CacheDir);
        //        }
        //        else
        m_recordBuffer = new ArchiveBuffer(m_addonHelper, url);
    }
    catch (InputBufferException & ex)
    {
        m_addonHelper->Log(LOG_ERROR, "%s: failed. Can't open recording stream.", __FUNCTION__);
        return false;
    }
    
    return true;
    
}
void PVRClientBase::CloseRecordedStream(void)
{
    if(m_recordBuffer) {
        m_addonHelper->Log(LOG_NOTICE, "PVRClientBase: closing recorded sream...");
        SAFE_DELETE(m_recordBuffer);
        m_addonHelper->Log(LOG_NOTICE, "PVRClientBase: input recorded closed.");
    }
    
}
int PVRClientBase::ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->Read(pBuffer, iBufferSize);
}

long long PVRClientBase::SeekRecordedStream(long long iPosition, int iWhence)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->Seek(iPosition, iWhence);
}

long long PVRClientBase::PositionRecordedStream(void)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->GetPosition();
}
long long PVRClientBase::LengthRecordedStream(void)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->GetLength();
}

PVR_ERROR PVRClientBase::CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    m_addonHelper->Log(LOG_DEBUG, " >>>> !!!! Menu hook !!!! <<<<<");
    return MenuHook(menuhook, item);
}




