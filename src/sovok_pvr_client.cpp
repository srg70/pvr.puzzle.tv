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
#ifdef GetObject
#undef GetObject
#endif
#endif

#include <algorithm>
#include <ctime>
#include "p8-platform/util/util.h"
#include "kodi/xbmc_addon_cpp_dll.h"
#include "kodi/kodi_vfs_utils.hpp"

#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "sovok_pvr_client.h"
#include "helpers.h"
#include "sovok_tv.h"
#include "globals.hpp"

using namespace std;
using namespace ADDON;
using namespace PvrClient;
using namespace Globals;

typedef std::map<string, SovokTV::CountryTemplate*> CountryFilterMap;
static CountryFilterMap& GetCountryFilterMap();
static SovokTV::CountryFilter& GetCountryFilter();


ADDON_STATUS SovokPVRClient::Init(PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = PVRClientBase::Init(pvrprops);
    if(ADDON_STATUS_OK != retVal)
       return retVal;
    
    char buffer[1024];
    
    if (XBMC->GetSetting("login", &buffer))
        m_login = buffer;
    if (XBMC->GetSetting("password", &buffer))
        m_password = buffer;

    XBMC->GetSetting("filter_by_country", &GetCountryFilter().IsOn);
    for(auto& f : GetCountryFilterMap())
        XBMC->GetSetting(f.first.c_str(), &f.second->Hidden);
    
    XBMC->GetSetting("enable_adult", &m_enableAdult);
    if(m_enableAdult && XBMC->GetSetting("pin_code", &buffer))
        m_pinCode = buffer;

    XBMC->GetSetting("archive_support", &m_supportArchive);
    std::string streamer;
    if (XBMC->GetSetting("streamer", &buffer))
        m_strimmer = buffer;
    
    retVal = CreateCoreSafe(false);
    
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;
}


SovokPVRClient::~SovokPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    DestroyCoreSafe();

}

ADDON_STATUS SovokPVRClient::CreateCoreSafe(bool clearEpgCache)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    
    try
    {
        CreateCore(clearEpgCache);
    }
    catch (AuthFailedException &) {
        XBMC->QueueNotification(QUEUE_ERROR, XBMC_Message(32007));
    }
    catch (MissingHttpsSupportException &) {
        XBMC->QueueNotification(QUEUE_ERROR, "Missing HTTPS support.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    catch(MissingApiException & ex) {
        XBMC->QueueNotification(QUEUE_WARNING, (std::string("Missing Sovok API: ") + ex.reason).c_str());
    }
    catch(...) {
        XBMC->QueueNotification(QUEUE_ERROR, "Sovok TV: unhandeled exception");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    return retVal;
}

void SovokPVRClient::DestroyCoreSafe()
{
    if(m_sovokTV != NULL) {
        m_clientCore = NULL;
        auto ptr = m_sovokTV;
        m_sovokTV = NULL;
        SAFE_DELETE(ptr);
    }
}

void SovokPVRClient::CreateCore(bool clearEpgCache)
{
    DestroyCoreSafe();
    
    m_clientCore = m_sovokTV = new SovokTV(m_login, m_password);
    if(m_enableAdult)
        m_sovokTV->SetPinCode(m_pinCode);
    SetCountryFilter();
    m_sovokTV->IncludeCurrentEpgToArchive(m_addCurrentEpgToArchive);
    m_sovokTV->InitAsync(clearEpgCache);

    
    auto streamersList = m_sovokTV->GetStreamersList();
    string strimmersPath = GetClientPath();
    strimmersPath.append("/").append("resources/").append("streamers/");
    if(!XBMC->DirectoryExists(strimmersPath.c_str()))
        XBMC->CreateDirectory(strimmersPath.c_str());
    // Cleanup streamers list
//    if(XBMC->DirectoryExists(strimmersPath.c_str()))
//    {
//        std::vector<CVFSDirEntry> files;
//        VFSUtils::GetDirectory(XBMC, strimmersPath, "*.*", files);
//        for (auto& f : files) {
//            if(!f.IsFolder())
//                if(!XBMC->DeleteFile(f.Path().c_str()))
//                    LogError( "Failed to delete streamer from addon settings. %s", f.Path().c_str());
//        }
//    }
    std::for_each(streamersList.begin(), streamersList.end(), [&](StreamerNamesList::value_type &s)
                  {
                      
                      auto filename = strimmersPath+s;
                      if(!XBMC->FileExists(filename.c_str(), true))
                      {
                          void* f = XBMC->OpenFileForWrite(filename.c_str(), true);
                          if(f) XBMC->CloseFile(f);
                      }
                  });

    auto current = streamersList[GetStreamerId()];
    if (current != m_strimmer)
    {
        XBMC->QueueNotification(QUEUE_WARNING, XBMC_Message(32008));
    }

    
}

ADDON_STATUS SovokPVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;
    
    if (strcmp(settingName, "login") == 0) {
        if(strcmp((const char*) settingValue, m_login.c_str()) != 0) {
            m_login = (const char*) settingValue;
            if (!m_login.empty() && !m_password.empty()) {
//                if(!HasCore()) {
//                    XBMC->Log(LOG_ERROR, " Failed to create core aftrer login changes");
//                }
            }
            result = ADDON_STATUS_NEED_RESTART;
        }
    }
    else if (strcmp(settingName, "password") == 0) {
        if(strcmp((const char*) settingValue, m_password.c_str()) != 0){
            m_password = (const char*) settingValue;
            if (!m_login.empty() && !m_password.empty()) {
//                if(!HasCore()) {
//                    XBMC->Log(LOG_ERROR, " Failed to create core aftrer password changes");
//                }
            }
            result = ADDON_STATUS_NEED_RESTART;
        }
    }
    else if (strcmp(settingName, "streamer") == 0)
    {
        m_strimmer = (const char*)settingValue;
        if(m_sovokTV != NULL) {
            auto streamersList = m_sovokTV->GetStreamersList();
            int currentId = 0;
            std::find_if(streamersList.begin(), streamersList.end(), [&](StreamerNamesList::value_type &s) {
                if(s == m_strimmer)
                    return true;
                ++currentId;
                return false;
            });
            if(currentId != GetStreamerId()) {
                if(currentId == streamersList.size() ) {
                    XBMC->QueueNotification(QUEUE_WARNING, XBMC_Message(32008));
                }
                SetStreamerId(currentId);
                result = ADDON_STATUS_NEED_RESTART;
            }
        }
    }
    else if (strcmp(settingName, "enable_adult") == 0)
    {
        bool newValue = *(bool*)settingValue;
        if(newValue != m_enableAdult) {
            m_enableAdult = newValue;
            SetPinCode(m_enableAdult? m_pinCode : "");
            PVR->TriggerChannelUpdate();

            result = ADDON_STATUS_OK;
        }
    }
    else if (strcmp(settingName, "pin_code") == 0)
    {
        const char* newValue = m_enableAdult? (const char*) settingValue : "";
        if(m_pinCode != newValue) {
            m_pinCode = newValue;
            SetPinCode(m_pinCode);
            
            PVR->TriggerChannelGroupsUpdate();
            PVR->TriggerChannelUpdate();
            result = ADDON_STATUS_OK;
        }
    }
    else if (strcmp(settingName, "archive_support") == 0)
    {
        bool newValue = *(bool*)settingValue;
        result = (m_supportArchive != newValue) ? ADDON_STATUS_NEED_RESTART : ADDON_STATUS_OK;
        
    }
    else if (strcmp(settingName, "filter_by_country") == 0)
    {
        bool newValue = *(bool*)settingValue;
        if(newValue != GetCountryFilter().IsOn) {
            GetCountryFilter().IsOn = (*(bool *)(settingValue));
            SetCountryFilter();
            m_sovokTV->RebuildChannelAndGroupList();
            
            PVR->TriggerChannelGroupsUpdate();
            PVR->TriggerChannelUpdate();
            result = ADDON_STATUS_OK;
        }
    }
    else {
        auto& filters = GetCountryFilterMap();
        
        auto it = filters.find(settingName);
        if (it != filters.end())
        {
            bool newValue = *(bool*)settingValue;
            if(it->second->Hidden == newValue) {
                result = ADDON_STATUS_OK;
            }
            else {
                it->second->Hidden = newValue;
                SetCountryFilter();
                m_sovokTV->RebuildChannelAndGroupList();
                PVR->TriggerChannelGroupsUpdate();
                PVR->TriggerChannelUpdate();
                result = ADDON_STATUS_OK;
            }
        } else {
            return PVRClientBase::SetSetting(settingName, settingValue);
        }
    }
    return result;
}

bool SovokPVRClient::HasCore()
{
    bool result = m_sovokTV != NULL;
    
//    try {
//        if(!result)
//            CreateCore(false);
//        result = m_sovokTV != NULL;
//    }catch (AuthFailedException &) {
//        XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(32007));
//    }
    return result;
}

PVR_ERROR SovokPVRClient::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
{
    pCapabilities->bSupportsEPG = true;
    pCapabilities->bSupportsTV = true;
    pCapabilities->bSupportsRadio = true;
    pCapabilities->bSupportsChannelGroups = true;
    pCapabilities->bHandlesInputStream = true;
    pCapabilities->bSupportsRecordings = m_supportArchive;
    
    pCapabilities->bSupportsTimers = false;
    pCapabilities->bSupportsChannelScan = false;
    pCapabilities->bHandlesDemuxing = false;
    pCapabilities->bSupportsRecordingPlayCount = false;
    pCapabilities->bSupportsLastPlayedPosition = false;
    pCapabilities->bSupportsRecordingEdl = false;
    
    return PVRClientBase::GetAddonCapabilities(pCapabilities);
}

PVR_ERROR  SovokPVRClient::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    return PVRClientBase::MenuHook(menuhook, item);
    
}
ADDON_STATUS SovokPVRClient::OnReloadEpg()
{
   return CreateCoreSafe(true);
}

bool SovokPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(!HasCore())
        return false;
    
    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
  // NOTE: Kodi does NOT provide recording.iChannelUid for unknown reason
    // Worrkaround: use EPG entry
    
    EpgEntry epgTag;
    if(!m_sovokTV->GetEpgEntry(stoi(recording.strRecordingId), epgTag))
        return false;
    
    string url = m_sovokTV->GetArchiveUrl(epgTag.ChannelId, recording.recordingTime);
    return PVRClientBase::OpenRecordedStream(url, nullptr);
}

PVR_ERROR SovokPVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Sovok TV");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (!HasCore()) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}

void SovokPVRClient::SetStreamerId(int streamerIdx)
{
    if(!HasCore())
        return;
    m_sovokTV->SetStreamerId(streamerIdx);
}
int SovokPVRClient::GetStreamerId()
{
    if(!HasCore())
        return 0;
    
    return m_sovokTV->GetSreamerId();
}
void SovokPVRClient::SetPinCode(const std::string& code)
{
    if(!HasCore())
        return;
    m_sovokTV->SetPinCode(code);
}

enum CountryCodes{
    CountryCode_LT = 0,
    CountryCode_US,
    CountryCode_DE,
    CountryCode_IL,
    CountryCode_LV,
    CountryCode_EE,
    CountryCode_IT,
    CountryCode_FR,
    CountryCode_Total
};

static CountryFilterMap& GetCountryFilterMap()
{
    static CountryFilterMap countryFilterMap;
    if(countryFilterMap.size() == 0) {
        auto & filter = GetCountryFilter();
        countryFilterMap["hide_LT_channels"] = &filter.Filters[CountryCode_LT];
        countryFilterMap["hide_US_channels"] = &filter.Filters[CountryCode_US];
        countryFilterMap["hide_DE_channels"] = &filter.Filters[CountryCode_DE];
        countryFilterMap["hide_IL_channels"] = &filter.Filters[CountryCode_IL];
        countryFilterMap["hide_LV_channels"] = &filter.Filters[CountryCode_LV];
        countryFilterMap["hide_EE_channels"] = &filter.Filters[CountryCode_EE];
        countryFilterMap["hide_IT_channels"] = &filter.Filters[CountryCode_IT];
        countryFilterMap["hide_FR_channels"] = &filter.Filters[CountryCode_FR];
    }
    return countryFilterMap;
}
static SovokTV::CountryFilter& GetCountryFilter()
{
    static SovokTV::CountryFilter countryFilter;
    
    
    if(countryFilter.Filters.size() == 0) {
        countryFilter.Filters.resize(CountryCode_Total);
        SovokTV::CountryTemplate*
        countryTemplate = &countryFilter.Filters[CountryCode_LT];
        countryTemplate->FilterPattern = "[LT]";
        countryTemplate->GroupName = "Литовские";
        countryTemplate = &countryFilter.Filters[CountryCode_US];
        countryTemplate->FilterPattern = "[US]";
        countryTemplate->GroupName = "Американские";
        countryTemplate = &countryFilter.Filters[CountryCode_DE];
        countryTemplate->FilterPattern = "[DE]";
        countryTemplate->GroupName = "Немецкие";
        countryTemplate = &countryFilter.Filters[CountryCode_IL];
        countryTemplate->FilterPattern = "[IL]";
        countryTemplate->GroupName = "Израильские";
        countryTemplate = &countryFilter.Filters[CountryCode_LV];
        countryTemplate->FilterPattern = "[LV]";
        countryTemplate->GroupName = "Латвийские";
        countryTemplate = &countryFilter.Filters[CountryCode_EE];
        countryTemplate->FilterPattern = "[EE]";
        countryTemplate->GroupName = "Эстонские";
        countryTemplate = &countryFilter.Filters[CountryCode_IT];
        countryTemplate->FilterPattern = "[IT]";
        countryTemplate->GroupName = "Итальянские";
        countryTemplate = &countryFilter.Filters[CountryCode_FR];
        countryTemplate->FilterPattern = "[FR]";
        countryTemplate->GroupName = "Французские";
    }
    return countryFilter;
}

void SovokPVRClient::SetCountryFilter()
{
    
    if(!HasCore())
        return;

    auto & filter = GetCountryFilter();
    m_sovokTV->SetCountryFilter(filter);
}



