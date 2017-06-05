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

#include "libXBMC_pvr.h"
#include "timeshift_buffer.h"
#include "direct_buffer.h"
#include "sovok_pvr_client.h"
#include "helpers.h"


using namespace std;
using namespace ADDON;

// NOTE: avoid '.' (dot) char in path. Causes to deadlock in Kodi code.
const char* s_DefaultCacheDir = "special://temp/pvr-puzzle-tv";

SovokPVRClient::SovokPVRClient(CHelper_libXBMC_addon *addonHelper, CHelper_libXBMC_pvr *pvrHelper,
                               const std::string &sovokLogin, const std::string &sovokPassword) :
    m_addonHelper(addonHelper),
    m_pvrHelper(pvrHelper),
    m_sovokTV(addonHelper, sovokLogin, sovokPassword),
    m_inputBuffer(NULL),
    m_isTimeshiftEnabled(false),
    m_shouldAddFavoritesGroup(true)
{
    SetTimeshiftPath(s_DefaultCacheDir);
}

SovokPVRClient::~SovokPVRClient()
{
    CloseLiveStream();
}

void SovokPVRClient::SetTimeshiftPath(const std::string& path){
    const char* nonEmptyPath = (path.empty()) ? s_DefaultCacheDir : path.c_str();
    if(!m_addonHelper->DirectoryExists(nonEmptyPath))
        if(!m_addonHelper->CreateDirectory(nonEmptyPath))
            m_addonHelper->Log(LOG_ERROR, "Failed to create cache folder");
    m_CacheDir = nonEmptyPath;
}

PVR_ERROR SovokPVRClient::GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL& channel, time_t iStart, time_t iEnd)
{
    EpgEntryList epgEntries;
    m_sovokTV.GetEpg(channel.iUniqueId, iStart, iEnd, epgEntries);
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
//    m_sovokTV.FindEpg(item.data.iEpgUid, epgEntry);
    return PVR_ERROR_NOT_IMPLEMENTED;
    
}
int SovokPVRClient::GetChannelGroupsAmount()
{
    size_t numberOfGroups = m_sovokTV.GetGroupList().size();
    if (m_shouldAddFavoritesGroup)
        numberOfGroups++;
    return numberOfGroups;
}

PVR_ERROR SovokPVRClient::GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
    if (!bRadio)
    {
        PVR_CHANNEL_GROUP pvrGroup = { 0 };
        pvrGroup.bIsRadio = false;

        if (m_shouldAddFavoritesGroup)
        {
            strncpy(pvrGroup.strGroupName, "Избранное", sizeof(pvrGroup.strGroupName));
            m_pvrHelper->TransferChannelGroup(handle, &pvrGroup);
        }

        GroupList groups = m_sovokTV.GetGroupList();
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
    if (m_shouldAddFavoritesGroup && group.strGroupName == std::string("Избранное"))
    {
        FavoriteList favorites = m_sovokTV.GetFavorites();
        FavoriteList::const_iterator itFavorite = favorites.begin();
        for (; itFavorite != favorites.end(); ++itFavorite)
        {
            PVR_CHANNEL_GROUP_MEMBER pvrGroupMember = { 0 };
            strncpy(pvrGroupMember.strGroupName, "Избранное", sizeof(pvrGroupMember.strGroupName));
            pvrGroupMember.iChannelUniqueId = *itFavorite;
            m_pvrHelper->TransferChannelGroupMember(handle, &pvrGroupMember);
        }
    }

    const GroupList &groups = m_sovokTV.GetGroupList();
    GroupList::const_iterator itGroup = groups.find(group.strGroupName);
    if (itGroup != groups.end())
    {
        std::set<int>::const_iterator itChannel = itGroup->second.Channels.begin();
        for (; itChannel != itGroup->second.Channels.end(); ++itChannel)
        {
            if (group.strGroupName == itGroup->first)
            {
                PVR_CHANNEL_GROUP_MEMBER pvrGroupMember = { 0 };
                strncpy(pvrGroupMember.strGroupName, itGroup->first.c_str(), sizeof(pvrGroupMember.strGroupName));
                pvrGroupMember.iChannelUniqueId = *itChannel;
                m_pvrHelper->TransferChannelGroupMember(handle, &pvrGroupMember);
            }
        }
    }

    return PVR_ERROR_NO_ERROR;
}

int SovokPVRClient::GetChannelsAmount()
{
    return m_sovokTV.GetChannelList().size();
}

PVR_ERROR SovokPVRClient::GetChannels(ADDON_HANDLE handle, bool bRadio)
{
    const ChannelList &channels = m_sovokTV.GetChannelList();
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
    string url = m_sovokTV.GetUrl(channel.iUniqueId);
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

void SovokPVRClient::CloseLiveStream()
{
    m_addonHelper->Log(LOG_NOTICE, "SovokPVRClient: closing input sream...");
    delete m_inputBuffer;
    m_inputBuffer = NULL;
    m_addonHelper->Log(LOG_NOTICE, "SovokPVRClient: input sream closed.");
}

int SovokPVRClient::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
{
    return m_inputBuffer->Read(pBuffer, iBufferSize);
}

long long SovokPVRClient::SeekLiveStream(long long iPosition, int iWhence)
{
    return m_inputBuffer->Seek(iPosition, iWhence);
}

long long SovokPVRClient::PositionLiveStream()
{
    return m_inputBuffer->GetPosition();
}

long long SovokPVRClient::LengthLiveStream()
{
    return m_inputBuffer->GetLength();
}

bool SovokPVRClient::SwitchChannel(const PVR_CHANNEL& channel)
{
    string url = m_sovokTV.GetUrl(channel.iUniqueId);
    return m_inputBuffer->SwitchStream(url);
}

void SovokPVRClient::SetTimeshiftEnabled(bool enable)
{
    m_isTimeshiftEnabled = enable;
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
    if(deleted)
        return -1;
    
    int size = 0;
    std::function<void(const ArchiveList&)> f = [&size](const ArchiveList& list){size = list.size();};
    m_sovokTV.Apply(f);
    if(size == 0)
    {
        std::function<void(void)> action = [=](){
            m_pvrHelper->TriggerRecordingUpdate();
        };
        m_sovokTV.StartArchivePollingWithCompletion(action);
//        m_sovokTV.Apply(f);
//        if(size != 0)
//            action();
    
    }
    return size;
    
}
PVR_ERROR SovokPVRClient::GetRecordings(ADDON_HANDLE handle, bool deleted)
{
    if(deleted)
        return PVR_ERROR_NOT_IMPLEMENTED;
    
    PVR_ERROR result = PVR_ERROR_NO_ERROR;
    SovokTV& sTV(m_sovokTV);
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
    m_sovokTV.Apply(f);
    return result;
}

bool SovokPVRClient::OpenRecordedStream(const PVR_RECORDING &recording)
{
    SovokEpgEntry epgTag;
    
    unsigned int epgId = recording.iEpgEventId;
    if( epgId == 0 )
        epgId = strtoi(recording.strRecordingId);
    if(!m_sovokTV.FindEpg(epgId, epgTag))
        return false;
    
    string url = m_sovokTV.GetArchiveForEpg(epgTag);
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
void SovokPVRClient::CloseRecordedStream(void)
{
    delete m_recordBuffer;
    m_recordBuffer = NULL;

}
int SovokPVRClient::ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->Read(pBuffer, iBufferSize);
}

long long SovokPVRClient::SeekRecordedStream(long long iPosition, int iWhence)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->Seek(iPosition, iWhence);
}

long long SovokPVRClient::PositionRecordedStream(void)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->GetPosition();
}
long long SovokPVRClient::LengthRecordedStream(void)
{
    return (m_recordBuffer == NULL) ? -1 : m_recordBuffer->GetLength();
}





