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

ADDON_STATUS SovokPVRClient::Init(CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                               PVR_PROPERTIES* pvrprops)
{
    ADDON_STATUS retVal = ADDON_STATUS_OK;
    if(ADDON_STATUS_OK != (retVal = PVRClientBase::Init(addonHelper, pvrHelper, pvrprops)))
       return retVal;
    
    m_shouldAddFavoritesGroup = true;
    char buffer[1024];
    
    if (m_addonHelper->GetSetting("login", &buffer))
        m_login = buffer;
    if (m_addonHelper->GetSetting("password", &buffer))
        m_password = buffer;
    bool shouldAddFavoritesGroup;
    m_addonHelper->GetSetting("add_favorites_group", &shouldAddFavoritesGroup);
   
    
    try
    {
        CreateCore();
        
        SetAddFavoritesGroup(shouldAddFavoritesGroup);
        
        std::string streamer;
        if (m_addonHelper->GetSetting("streamer", &buffer))
            streamer = buffer;
        
        auto streamersList = m_sovokTV->GetStreamersList();
        auto current = streamersList[GetStreamerId()];
        if (current != streamer)
        {
            m_addonHelper->QueueNotification(QUEUE_WARNING, "Streamer setting mismatch.");
        }
        m_addonHelper->GetSetting("enable_adult", &m_enableAdult);
        if(m_enableAdult && m_addonHelper->GetSetting("pin_code", &buffer))
            SetPinCode(buffer);
        
    }
    catch (AuthFailedException &)
    {
        m_addonHelper->QueueNotification(QUEUE_ERROR, "Login to Sovok.TV failed.");
    }
    
    catch(MissingApiException & ex)
    {
        m_addonHelper->QueueNotification(QUEUE_WARNING, (std::string("Missing Sovok API: ") + ex.reason).c_str());
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
    if(m_sovokTV != NULL)
        SAFE_DELETE(m_sovokTV);

}

void SovokPVRClient::CreateCore()
{
    if(m_sovokTV != NULL)
        SAFE_DELETE(m_sovokTV);
    m_sovokTV = new SovokTV(m_addonHelper, m_login, m_password);

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
    
}

ADDON_STATUS SovokPVRClient::SetSetting(const char *settingName, const void *settingValue)
{
    if (strcmp(settingName, "login") == 0 && strcmp((const char*) settingValue, m_login.c_str()) != 0)
    {
        m_login = (const char*) settingValue;
        if (!m_login.empty() && !m_password.empty() && m_sovokTV == NULL)
        {
            try {
                CreateCore();
            }catch (AuthFailedException &) {
                m_addonHelper->QueueNotification(QUEUE_ERROR, "Login to Sovok.TV failed.");
            }
        }
    }
    else if (strcmp(settingName, "password") == 0  && strcmp((const char*) settingValue, m_password.c_str()) != 0)
    {
        m_password = (const char*) settingValue;
        if (!m_login.empty() && !m_password.empty() && m_sovokTV == NULL)
        {
            try {
                CreateCore();
            }catch (AuthFailedException &) {
                m_addonHelper->QueueNotification(QUEUE_ERROR, "Login to Sovok.TV failed.");
            }
        }
    }
    else if (strcmp(settingName, "add_favorites_group") == 0)
    {
        if(m_sovokTV)
            SetAddFavoritesGroup(*(bool *)(settingValue));
    }
    else if (strcmp(settingName, "streamer") == 0)
    {
        m_strimmer = (const char*)settingValue;
        if(m_sovokTV != NULL)
        {
            auto streamersList = m_sovokTV->GetStreamersList();
            int currentId = 0;
            std::find_if(streamersList.begin(), streamersList.end(), [&](StreamerNamesList::value_type &s) {
                if(s == m_strimmer)
                    return true;
                ++currentId;
                return false;
            });
            if(currentId == streamersList.size() )
            {
                m_addonHelper->QueueNotification(QUEUE_WARNING, "Streamer setting mismatch.");
            }
            SetStreamerId(currentId);
        }
    }
    else if (strcmp(settingName, "enable_adult") == 0)
    {
        m_enableAdult = *(bool*)settingValue;
    }
    else if (strcmp(settingName, "pin_code") == 0)
    {
        if(m_enableAdult && m_sovokTV != NULL)
            SetPinCode((const char*) settingValue);
        
    }
    else {
        return PVRClientBase::SetSetting(settingName, settingValue);
    }
    return ADDON_STATUS_NEED_RESTART;
}

PVR_ERROR SovokPVRClient::GetAddonCapabilities(PVR_ADDON_CAPABILITIES *pCapabilities)
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
    
    return PVRClientBase::GetAddonCapabilities(pCapabilities);
}


PVR_ERROR SovokPVRClient::GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd)
{
    if(m_sovokTV == NULL)
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
//    SovokEpgEntry epgEntry;
//    m_sovokTV->FindEpg(item.data.iEpgUid, epgEntry);
    return PVR_ERROR_NOT_IMPLEMENTED;
    
}
int SovokPVRClient::GetChannelGroupsAmount()
{
    if(m_sovokTV == NULL)
        return -1;
    
    size_t numberOfGroups = m_sovokTV->GetGroupList().size();
    if (m_shouldAddFavoritesGroup)
        numberOfGroups++;
    return numberOfGroups;
}

PVR_ERROR SovokPVRClient::GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
    if(m_sovokTV == NULL)
        return PVR_ERROR_SERVER_ERROR;

    if (!bRadio)
    {
        PVR_CHANNEL_GROUP pvrGroup = { 0 };
        pvrGroup.bIsRadio = false;

        if (m_shouldAddFavoritesGroup)
        {
            strncpy(pvrGroup.strGroupName, "Избранное", sizeof(pvrGroup.strGroupName));
            m_pvrHelper->TransferChannelGroup(handle, &pvrGroup);
        }

        GroupList groups = m_sovokTV->GetGroupList();
        GroupList::const_iterator itGroup = groups.begin();
        for (; itGroup != groups.end(); ++itGroup)
        {
            strncpy(pvrGroup.strGroupName, itGroup->first.c_str(), sizeof(pvrGroup.strGroupName));
            m_pvrHelper->TransferChannelGroup(handle, &pvrGroup);
        }
    }

    return PVR_ERROR_NO_ERROR;
}

PVR_ERROR SovokPVRClient::GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP& group)
{
    if(m_sovokTV == NULL)
        return PVR_ERROR_SERVER_ERROR;

    const std::string favoriteGroupName("Избранное");
    if (m_shouldAddFavoritesGroup && group.strGroupName == favoriteGroupName)
    {
        const char* c_GroupName = favoriteGroupName.c_str();
        auto favorites = m_sovokTV->GetFavorites();
        for (auto it= favorites.begin(),  end = favorites.end() ; it != end; ++it)
        {
            PVR_CHANNEL_GROUP_MEMBER pvrGroupMember = { 0 };
            strncpy(pvrGroupMember.strGroupName, c_GroupName, sizeof(pvrGroupMember.strGroupName));
            pvrGroupMember.iChannelUniqueId = *it;
            m_pvrHelper->TransferChannelGroupMember(handle, &pvrGroupMember);
        }
    }

    auto& groups = m_sovokTV->GetGroupList();
    GroupList::const_iterator itGroup = groups.find(group.strGroupName);
    if (itGroup != groups.end())
    {
        for (auto it= itGroup->second.Channels.begin(), end = itGroup->second.Channels.end(); it !=end;  ++it)
        {
//            if (group.strGroupName == itGroup->first)
            {
                PVR_CHANNEL_GROUP_MEMBER pvrGroupMember = { 0 };
                strncpy(pvrGroupMember.strGroupName, itGroup->first.c_str(), sizeof(pvrGroupMember.strGroupName));
                pvrGroupMember.iChannelUniqueId = *it;
                m_pvrHelper->TransferChannelGroupMember(handle, &pvrGroupMember);
            }
        }
    }

    return PVR_ERROR_NO_ERROR;
}

int SovokPVRClient::GetChannelsAmount()
{
    if(m_sovokTV == NULL)
        return -1;
    return m_sovokTV->GetChannelList().size();
}

PVR_ERROR SovokPVRClient::GetChannels(ADDON_HANDLE handle, bool bRadio)
{
    if(m_sovokTV == NULL)
        return PVR_ERROR_SERVER_ERROR;
    
    const ChannelList &channels = m_sovokTV->GetChannelList();
    ChannelList::const_iterator itChannel = channels.begin();
    for(; itChannel != channels.end(); ++itChannel)
    {
        const SovokChannel &sovokChannel = itChannel->second;
        if (bRadio == sovokChannel.IsRadio)
        {
            PVR_CHANNEL pvrChannel = { 0 };
            pvrChannel.iUniqueId = sovokChannel.Id;
            pvrChannel.iChannelNumber = sovokChannel.Id;
            pvrChannel.bIsRadio = sovokChannel.IsRadio;
            strncpy(pvrChannel.strChannelName, sovokChannel.Name.c_str(), sizeof(pvrChannel.strChannelName));

            string iconUrl = "http://sovok.tv" + sovokChannel.IconPath;
            strncpy(pvrChannel.strIconPath, iconUrl.c_str(), sizeof(pvrChannel.strIconPath));;

            m_pvrHelper->TransferChannelEntry(handle, &pvrChannel);
        }
    }

    return PVR_ERROR_NO_ERROR;
}

bool SovokPVRClient::OpenLiveStream(const PVR_CHANNEL& channel)
{
    if(m_sovokTV == NULL)
        return false;

    string url = m_sovokTV->GetUrl(channel.iUniqueId);
    return PVRClientBase::OpenLiveStream(url);
}

bool SovokPVRClient::SwitchChannel(const PVR_CHANNEL& channel)
{
    if(m_sovokTV == NULL)
        return false;

    string url = m_sovokTV->GetUrl(channel.iUniqueId);
    return PVRClientBase::SwitchChannel(url);
}

void SovokPVRClient::SetAddFavoritesGroup(bool shouldAddFavoritesGroup)
{
    if (shouldAddFavoritesGroup != m_shouldAddFavoritesGroup)
    {
        m_shouldAddFavoritesGroup = shouldAddFavoritesGroup;
        m_pvrHelper->TriggerChannelGroupsUpdate();
    }
}


int SovokPVRClient::GetRecordingsAmount(bool deleted)
{
    if(m_sovokTV == NULL)
        return -1;

    if(deleted)
        return -1;
    
    int size = 0;
    std::function<void(const ArchiveList&)> f = [&size](const ArchiveList& list){size = list.size();};
    m_sovokTV->Apply(f);
    if(size == 0)
    {
        std::function<void(void)> action = [=](){
            m_pvrHelper->TriggerRecordingUpdate();
        };
        m_sovokTV->StartArchivePollingWithCompletion(action);
//        m_sovokTV->Apply(f);
//        if(size != 0)
//            action();
    
    }
    return size;
    
}
PVR_ERROR SovokPVRClient::GetRecordings(ADDON_HANDLE handle, bool deleted)
{
    if(m_sovokTV == NULL)
        return PVR_ERROR_SERVER_ERROR;

    if(deleted)
        return PVR_ERROR_NOT_IMPLEMENTED;
    
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    SovokTV& sTV(*m_sovokTV);
    CHelper_libXBMC_pvr * pvrHelper = m_pvrHelper;
    ADDON::CHelper_libXBMC_addon * addonHelper = m_addonHelper;
    std::function<void(const ArchiveList&)> f = [&sTV, &handle, pvrHelper, addonHelper ,&result](const ArchiveList& list){
        for(const auto &  i :  list) {
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

        }
    };
    m_sovokTV->Apply(f);
    return result;
}

bool SovokPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    if(m_sovokTV == NULL)
        return false;

    SovokEpgEntry epgTag;
    
    unsigned int epgId = recording.iEpgEventId;
    if( epgId == 0 )
        epgId = stoul(recording.strRecordingId);
    if(!m_sovokTV->FindEpg(epgId, epgTag))
        return false;
    
    string url = m_sovokTV->GetArchiveForEpg(epgTag);
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
    snprintf(signalStatus.strAdapterStatus, sizeof(signalStatus.strAdapterStatus), (m_sovokTV == NULL) ? "Not connected" :"OK");
    return PVR_ERROR_NO_ERROR;
}

ADDON_STATUS SovokPVRClient::GetStatus(){ return  /*(m_sovokTV == NULL)? ADDON_STATUS_LOST_CONNECTION : */ADDON_STATUS_OK;}
void SovokPVRClient::SetStreamerId(int streamerIdx)
{
    if(m_sovokTV == NULL)
        return;
    m_sovokTV->SetStreamerId(streamerIdx);
}
int SovokPVRClient::GetStreamerId()
{
    if(m_sovokTV == NULL)
        return 0;
    
    return m_sovokTV->GetSreamerId();
}
void SovokPVRClient::SetPinCode(const std::string& code)
{
    if(m_sovokTV == NULL)
        return;
    m_sovokTV->SetPinCode(code);
}
       



