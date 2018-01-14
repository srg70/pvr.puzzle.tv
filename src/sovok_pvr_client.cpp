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
#include "p8-platform/util/util.h"
#include "kodi/xbmc_addon_cpp_dll.h"

#include "libXBMC_pvr.h"
#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "sovok_pvr_client.h"
#include "helpers.h"
#include "sovok_tv.h"

using namespace std;
using namespace ADDON;
using namespace PvrClient;

typedef std::map<string, SovokTV::CountryTemplate*> CountryFilterMap;
static CountryFilterMap& GetCountryFilterMap();
static SovokTV::CountryFilter& GetCountryFilter();


ADDON_STATUS SovokPVRClient::Init(CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                               PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    if(ADDON_STATUS_OK != (retVal = PVRClientBase::Init(addonHelper, pvrHelper, pvrprops)))
       return retVal;
    
    m_lastChannelRestartCount = 0;
    char buffer[1024];
    
    if (m_addonHelper->GetSetting("login", &buffer))
        m_login = buffer;
    if (m_addonHelper->GetSetting("password", &buffer))
        m_password = buffer;

    m_addonHelper->GetSetting("filter_by_country", &GetCountryFilter().IsOn);
    for(auto& f : GetCountryFilterMap())
        m_addonHelper->GetSetting(f.first.c_str(), &f.second->Hidden);
    
    m_addonHelper->GetSetting("enable_adult", &m_enableAdult);
    if(m_enableAdult && m_addonHelper->GetSetting("pin_code", &buffer))
        m_pinCode = buffer;

    m_addonHelper->GetSetting("archive_support", &m_supportArchive);
    std::string streamer;
    if (m_addonHelper->GetSetting("streamer", &buffer))
        m_strimmer = buffer;
    

    try
    {
        CreateCore();
    }
    catch (AuthFailedException &)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32007));
    }
    
    catch (MissingHttpsSupportException &)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Missing HTTPS support.");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    
    catch(MissingApiException & ex)
    {
        m_addonHelper->QueueNotification(QUEUE_WARNING, (std::string("Missing Sovok API: ") + ex.reason).c_str());
    }
    catch(...)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Sovok TV: unhandeled exception");
        retVal = ADDON_STATUS_PERMANENT_FAILURE;
    }
    //    PVR_MENUHOOK hook = {1, 30020, PVR_MENUHOOK_EPG};
    //    m_pvr->AddMenuHook(&hook);
    return retVal;
}


SovokPVRClient::~SovokPVRClient()
{
    // Probably is better to close streams before engine destruction
    CloseLiveStream();
    CloseRecordedStream();
    if(m_sovokTV != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_sovokTV);
    }

}

void SovokPVRClient::CreateCore()
{
    if(m_sovokTV != NULL) {
        m_clientCore = NULL;
        SAFE_DELETE(m_sovokTV);
    }
    
    m_clientCore = m_sovokTV = new SovokTV(m_addonHelper, m_pvrHelper, m_login, m_password);

    if(m_enableAdult)
        m_sovokTV->SetPinCode(m_pinCode);
    
    SetCountryFilter();
    
    auto streamersList = m_sovokTV->GetStreamersList();
    string strimmersPath = GetClientPath();
    strimmersPath.append("/").append("resources/").append("streamers/");
    if(!m_addonHelper->DirectoryExists(strimmersPath.c_str()))
        m_addonHelper->CreateDirectory(strimmersPath.c_str());
    
    std::for_each(streamersList.begin(), streamersList.end(), [&](StreamerNamesList::value_type &s)
                  {
                      
                      auto filename = strimmersPath+s;
                      if(!m_addonHelper->FileExists(filename.c_str(), true))
                      {
                          void* f = m_addonHelper->OpenFileForWrite(filename.c_str(), true);
                          if(f) m_addonHelper->CloseFile(f);
                      }
                  });

    auto current = streamersList[GetStreamerId()];
    if (current != m_strimmer)
    {
        char* message  = m_addonHelper->GetLocalizedString(32008);
        m_addonHelper->QueueNotification(QUEUE_WARNING, message);
        m_addonHelper->FreeString(message);
    }

    
}

ADDON_STATUS SovokPVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    ADDON_STATUS result = ADDON_STATUS_OK ;
    
    if (strcmp(settingName, "login") == 0) {
        if(strcmp((const char*) settingValue, m_login.c_str()) != 0) {
            m_login = (const char*) settingValue;
            if (!m_login.empty() && !m_password.empty()) {
                if(!HasCore()) {
                    m_addonHelper->Log(LOG_ERROR, " Failed to create core aftrer login changes");
                }
            }
            result = ADDON_STATUS_NEED_RESTART;
        }
    }
    else if (strcmp(settingName, "password") == 0) {
        if(strcmp((const char*) settingValue, m_password.c_str()) != 0){
            m_password = (const char*) settingValue;
            if (!m_login.empty() && !m_password.empty()) {
                if(!HasCore()) {
                    m_addonHelper->Log(LOG_ERROR, " Failed to create core aftrer password changes");
                }
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
                    m_addonHelper->QueueNotification(QUEUE_WARNING, m_addonHelper->GetLocalizedString(32008));
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
            m_pvrHelper->TriggerChannelUpdate();

            result = ADDON_STATUS_OK;
        }
    }
    else if (strcmp(settingName, "pin_code") == 0)
    {
        const char* newValue = m_enableAdult? (const char*) settingValue : "";
        if(m_pinCode != newValue) {
            m_pinCode = newValue;
            SetPinCode(m_pinCode);
            
            m_pvrHelper->TriggerChannelGroupsUpdate();
            m_pvrHelper->TriggerChannelUpdate();
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

            m_pvrHelper->TriggerChannelGroupsUpdate();
            m_pvrHelper->TriggerChannelUpdate();
            result = ADDON_STATUS_OK;
        }
    }
    else {
        auto& filters = GetCountryFilterMap();
        
        auto it = filters.find(settingName);
        if (it != filters.end())
        {
            it->second->Hidden = *(bool*)settingValue;
            SetCountryFilter();
            
            m_pvrHelper->TriggerChannelGroupsUpdate();
            m_pvrHelper->TriggerChannelUpdate();
            result = ADDON_STATUS_OK;
        }
        
        else {
            return PVRClientBase::SetSetting(settingName, settingValue);
        }
    }
    return result;
}

bool SovokPVRClient::HasCore()
{
    bool result = m_sovokTV != NULL;
    
    try {
        if(!result)
            CreateCore();
        result = m_sovokTV != NULL;
    }catch (AuthFailedException &) {
        m_addonHelper->QueueNotification(QUEUE_ERROR, m_addonHelper->GetLocalizedString(32007));
    }
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


PVR_ERROR SovokPVRClient::GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd)
{
    if(!HasCore())
        return PVR_ERROR_SERVER_ERROR;

    EpgEntryList epgEntries;
    m_sovokTV->GetEpg(channel.iUniqueId, iStart, iEnd, epgEntries);
    EpgEntryList::const_iterator itEpgEntry = epgEntries.begin();
    for (int i = 0; itEpgEntry != epgEntries.end(); ++itEpgEntry, ++i)
    {
        EPG_TAG tag = { 0 };
        tag.iUniqueBroadcastId = itEpgEntry->first;
        tag.iChannelNumber = channel.iUniqueId;
        tag.strTitle = itEpgEntry->second.Title.c_str();
        tag.strPlot = itEpgEntry->second.Description.c_str();
        tag.startTime = itEpgEntry->second.StartTime;
        tag.endTime = itEpgEntry->second.EndTime;
        m_pvrHelper->TransferEpgEntry(handle, &tag);
    }
    return PVR_ERROR_NO_ERROR;
}

PVR_ERROR  SovokPVRClient::MenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    return PVR_ERROR_NOT_IMPLEMENTED;
    
}

static ChannelId s_lastChannelId = 0;
bool SovokPVRClient::OpenLiveStream(const PVR_CHANNEL& channel)
{
    if(!HasCore())
        return false;
    s_lastChannelId = channel.iUniqueId;
    m_lastChannelRestartCount = 0;
    
    string url = m_sovokTV->GetUrl(channel.iUniqueId);
    return PVRClientBase::OpenLiveStream(url);
}

int SovokPVRClient::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    int readBytes = PVRClientBase::ReadLiveStream(pBuffer,iBufferSize);
    // Assuming stream hanging.
    // Try to restart current channel one time.
    if (readBytes != iBufferSize ) {
        m_addonHelper->Log(LOG_ERROR, "SovokPVRClient:: trying to restart current channel.");
        string url = m_sovokTV->GetUrl(s_lastChannelId);
        if(!url.empty()){
            char* message = m_addonHelper->GetLocalizedString(32000);
            m_addonHelper->QueueNotification(QUEUE_INFO, message);
            m_addonHelper->FreeString(message);
            PVRClientBase::SwitchChannel(url);
            readBytes = PVRClientBase::ReadLiveStream(pBuffer,iBufferSize);
        }
    }
    
    return readBytes;
}


bool SovokPVRClient::SwitchChannel(const PVR_CHANNEL& channel)
{
    if(!HasCore())
        return false;

    s_lastChannelId = channel.iUniqueId;
    string url = m_sovokTV->GetUrl(channel.iUniqueId);
    return PVRClientBase::SwitchChannel(url);
}



int SovokPVRClient::GetRecordingsAmount(bool deleted)
{
    if(!HasCore())
        return -1;

    if(deleted)
        return -1;
    
    int size = 0;
    std::function<void(const SovokArchiveEntry&)> f = [&size](const SovokArchiveEntry&){++size;};
    m_sovokTV->ForEach(f);
    if(size == 0)
    {
        std::function<void(void)> action = [=](){
            m_pvrHelper->TriggerRecordingUpdate();
        };
        m_sovokTV->StartArchivePollingWithCompletion(action);
    }
    return size;
    
}
PVR_ERROR SovokPVRClient::GetRecordings(ADDON_HANDLE handle, bool deleted)
{
    if(!HasCore())
        return PVR_ERROR_SERVER_ERROR;

    if(deleted)
        return PVR_ERROR_NOT_IMPLEMENTED;
    
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    SovokTV& sTV(*m_sovokTV);
    CHelper_libXBMC_pvr * pvrHelper = m_pvrHelper;
    ADDON::CHelper_libXBMC_addon * addonHelper = m_addonHelper;
    std::function<void(const SovokArchiveEntry&)> f = [&sTV, &handle, pvrHelper, addonHelper ,&result](const SovokArchiveEntry& i)
    {
        try {
            const SovokEpgEntry& epgTag = sTV.GetEpgList().at(i);
            
            PVR_RECORDING tag = { 0 };
            //            memset(&tag, 0, sizeof(PVR_RECORDING));
            sprintf(tag.strRecordingId, "%d",  i);
            strncpy(tag.strTitle, epgTag.Title.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
            strncpy(tag.strPlot, epgTag.Description.c_str(), PVR_ADDON_DESC_STRING_LENGTH - 1);
            strncpy(tag.strChannelName, sTV.GetChannelList().at(epgTag.ChannelId).Name.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
            tag.recordingTime = epgTag.StartTime;
            tag.iLifetime = 0; /* not implemented */
            
            tag.iDuration = epgTag.EndTime - epgTag.StartTime;
            tag.iEpgEventId = i;
            tag.iChannelUid = epgTag.ChannelId;
            
            string dirName = tag.strChannelName;
            char buff[20];
            strftime(buff, sizeof(buff), "/%d-%m-%y", localtime(&epgTag.StartTime));
            dirName += buff;
            strncpy(tag.strDirectory, dirName.c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
            
            pvrHelper->TransferRecordingEntry(handle, &tag);
            
        }
        catch (...)  {
            addonHelper->Log(LOG_ERROR, "%s: failed.", __FUNCTION__);
            result = PVR_ERROR_FAILED;
        }
    };
    m_sovokTV->ForEach(f);
    return result;
}

bool SovokPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(!HasCore())
        return false;

    string url = m_sovokTV->GetArchiveUrl(recording.iChannelUid, recording.recordingTime);
    return PVRClientBase::OpenRecordedStream(url);
}

PVR_ERROR SovokPVRClient::CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
    m_addonHelper->Log(LOG_DEBUG, " >>>> !!!! Menu hook !!!! <<<<<");
    return MenuHook(menuhook, item);
}

PVR_ERROR SovokPVRClient::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
    snprintf(signalStatus.strAdapterName, sizeof(signalStatus.strAdapterName), "IPTV Sovok TV Adapter 1");
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (!HasCore()) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}

ADDON_STATUS SovokPVRClient::GetStatus(){ return  /*(!HasCore())? ADDON_STATUS_LOST_CONNECTION : */ADDON_STATUS_OK;}
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



