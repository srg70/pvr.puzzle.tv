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
#include <WinSock2.h>
#include <windows.h>
#ifdef GetObject
#undef GetObject
#endif
#endif

#include <algorithm>
#include <ctime>
#include "p8-platform/util/util.h"
#include <kodi/General.h>

#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "sovok_pvr_client.h"
#include "helpers.h"
#include "sovok_tv.h"
#include "globals.hpp"

using namespace std;
using namespace PvrClient;
using namespace Globals;
using namespace Helpers;

typedef std::map<string, SovokTV::CountryTemplate*> CountryFilterMap;
static CountryFilterMap& GetCountryFilterMap();
static SovokTV::CountryFilter& GetCountryFilter();


ADDON_STATUS SovokPVRClient::Init(const std::string& clientPath, const std::string& userPath)
{
    ADDON_STATUS retVal = PVRClientBase::Init(clientPath, userPath);
    if(ADDON_STATUS_OK != retVal)
       return retVal;
        
    m_login = kodi::GetSettingString("login");
    m_password = kodi::GetSettingString("password");

    GetCountryFilter().IsOn = kodi::GetSettingBoolean("filter_by_country", false);
    for(auto& f : GetCountryFilterMap())
        f.second->Hidden = kodi::GetSettingBoolean(f.first, false);
    
    m_enableAdult = kodi::GetSettingBoolean("enable_adult", false);
    if(m_enableAdult)
        m_pinCode = kodi::GetSettingString("pin_code");
    
    m_strimmer = kodi::GetSettingString("streamer");
    
    retVal = CreateCoreSafe(false);
    
    return retVal;
}

void SovokPVRClient::PopulateSettings(AddonSettingsMutableDictionary& settings)
{
    
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
        OnCoreCreated();
    }
    catch (AuthFailedException &) {
        kodi::QueueFormattedNotification(QUEUE_ERROR, kodi::GetLocalizedString(32007).c_str(), "Sovok TV");
    }
    catch (MissingHttpsSupportException &) {
        kodi::QueueFormattedNotification(QUEUE_ERROR, "Missing HTTPS support.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    catch(MissingApiException & ex) {
        kodi::QueueFormattedNotification(QUEUE_WARNING, (std::string("Missing Sovok API: ") + ex.reason).c_str());
    }
    catch(...) {
        kodi::QueueFormattedNotification(QUEUE_ERROR, "Sovok TV: unhandeled exception");
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
    m_sovokTV->IncludeCurrentEpgToArchive(HowToAddCurrentEpgToArchive());
    m_sovokTV->SetEpgCorrectionShift(EpgCorrectionShift());
    m_sovokTV->SetLocalLogosFolder(LocalLogosFolder());
    m_sovokTV->InitAsync(clearEpgCache, IsArchiveSupported());

    
    auto streamersList = m_sovokTV->GetStreamersList();
    string strimmersPath = GetClientPath();
    strimmersPath.append("/").append("resources/").append("streamers/");
    if(!kodi::vfs::DirectoryExists(strimmersPath))
        kodi::vfs::CreateDirectory(strimmersPath);
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
                      if(!kodi::vfs::FileExists(filename, true))
                      {
                          kodi::vfs::CFile f;
                          f.OpenFileForWrite(filename, true);
                      }
                  });

    auto current = streamersList[GetStreamerId()];
    if (current != m_strimmer)
    {
        kodi::QueueFormattedNotification(QUEUE_WARNING, kodi::GetLocalizedString(32008).c_str());
    }

    
}

ADDON_STATUS SovokPVRClient::SetSetting(const std::string& settingName, const kodi::CSettingValue& settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;
    
    if ("login" == settingName) {
        auto v = settingValue.GetString();
        if(v != m_login) {
            m_login = v;
            if (!m_login.empty() && !m_password.empty()) {
//                if(!HasCore()) {
//                    XBMC->Log(LOG_ERROR, " Failed to create core aftrer login changes");
//                }
            }
            result = ADDON_STATUS_NEED_RESTART;
        }
    }
    else if ("password" == settingName) {
        auto v = settingValue.GetString();
        if(v != m_password){
            m_password = v;
            if (!m_login.empty() && !m_password.empty()) {
//                if(!HasCore()) {
//                    XBMC->Log(LOG_ERROR, " Failed to create core aftrer password changes");
//                }
            }
            result = ADDON_STATUS_NEED_RESTART;
        }
    }
    else if ("streamer" == settingName)
    {
        m_strimmer = settingValue.GetString();
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
                    kodi::QueueFormattedNotification(QUEUE_WARNING, kodi::GetLocalizedString(32008).c_str());
                }
                SetStreamerId(currentId);
                result = ADDON_STATUS_NEED_RESTART;
            }
        }
    }
    else if ("enable_adult" == settingName)
    {
        bool newValue = settingValue.GetBoolean();
        if(newValue != m_enableAdult) {
            m_enableAdult = newValue;
            SetPinCode(m_enableAdult? m_pinCode : "");
            PVR->Addon_TriggerChannelUpdate();

            result = ADDON_STATUS_OK;
        }
    }
    else if ("pin_code" == settingName)
    {
        auto newValue = m_enableAdult ? settingValue.GetString() : "";
        if(m_pinCode != newValue) {
            m_pinCode = newValue;
            SetPinCode(m_pinCode);
            
            PVR->Addon_TriggerChannelGroupsUpdate();
            PVR->Addon_TriggerChannelUpdate();
            result = ADDON_STATUS_OK;
        }
    }
    else if ("filter_by_country" == settingName)
    {
        bool newValue = settingValue.GetBoolean();
        if(newValue != GetCountryFilter().IsOn) {
            GetCountryFilter().IsOn = newValue;
            SetCountryFilter();
            m_sovokTV->RebuildChannelAndGroupList();
            
            PVR->Addon_TriggerChannelGroupsUpdate();
            PVR->Addon_TriggerChannelUpdate();
            result = ADDON_STATUS_OK;
        }
    }
    else {
        auto& filters = GetCountryFilterMap();
        
        auto it = filters.find(settingName);
        if (it != filters.end())
        {
            bool newValue = settingValue.GetBoolean();
            if(it->second->Hidden == newValue) {
                result = ADDON_STATUS_OK;
            }
            else {
                it->second->Hidden = newValue;
                SetCountryFilter();
                m_sovokTV->RebuildChannelAndGroupList();
                PVR->Addon_TriggerChannelGroupsUpdate();
                PVR->Addon_TriggerChannelUpdate();
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

PVR_ERROR SovokPVRClient::GetAddonCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
    capabilities.SetSupportsEPG(true);
    capabilities.SetSupportsTV(true);
    capabilities.SetSupportsRadio(true);
    capabilities.SetSupportsChannelGroups(true);
    capabilities.SetHandlesInputStream(true);
//    capabilities.SetSupportsRecordings(true);
    
    capabilities.SetSupportsTimers(false);
    capabilities.SetSupportsChannelScan(false);
    capabilities.SetHandlesDemuxing(false);
    capabilities.SetSupportsRecordingPlayCount(false);
    capabilities.SetSupportsLastPlayedPosition(false);
    capabilities.SetSupportsRecordingEdl(false);
    
    return PVRClientBase::GetAddonCapabilities(capabilities);
}

ADDON_STATUS SovokPVRClient::OnReloadEpg()
{
   return CreateCoreSafe(true);
}

bool SovokPVRClient::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
    if(!HasCore())
        return false;
    
    if(IsLocalRecording(recording))
        return PVRClientBase::OpenRecordedStream(recording);
    
  // NOTE: Kodi does NOT provide recording.iChannelUid for unknown reason
    // Worrkaround: use EPG entry
    
    EpgEntry epgTag;
    if(!m_sovokTV->GetEpgEntry(stoi(recording.GetRecordingId().c_str()), epgTag))
        return false;
    
    string url = m_sovokTV->GetArchiveUrl(epgTag.UniqueChannelId, recording.GetRecordingTime());
    return PVRClientBase::OpenRecordedStream(url, nullptr, IsSeekSupported() ? SupportVodSeek : NoRecordingFlags);
}

PVR_ERROR SovokPVRClient::SignalStatus(int /*channelUid*/, kodi::addon::PVRSignalStatus& signalStatus)
{
    signalStatus.SetAdapterName("IPTV Sovok TV");
    signalStatus.SetAdapterStatus((!HasCore()) ? "Not connected" :"OK");
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



