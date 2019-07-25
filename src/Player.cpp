/*
 *
 *   Copyright (C) 2019 Sergey Shramchenko
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

#include "Player.hpp"
#include "globals.hpp"
#include "helpers.h"

#include "timeshift_buffer.h"
#include "file_cache_buffer.hpp"
#include "memory_cache_buffer.hpp"
#include "plist_buffer.h"
#include "direct_buffer.h"
#include "simple_cyclic_buffer.hpp"
#include "ActionQueue.hpp"

static const int c_InitialLastByteRead = 1;

namespace PvrClient
{
    using namespace std;
    Player::Player()
    : m_delegate(nullptr)
    , m_lastBytesRead(c_InitialLastByteRead)
    , m_inputBuffer(nullptr)
    {
        m_recordBuffer.buffer = nullptr;
        m_recordBuffer.duration = 0;
        m_localRecordBuffer = nullptr;

        m_liveChannelId =  m_localRecordChannelId = UnknownChannelId;
        m_destroyer = new CActionQueue(100, "Streams Destroyer");
        m_destroyer->CreateThread();
        
    }
    
    Player::~Player()
    {
        Cleanup();
        if(m_destroyer) {
            SAFE_DELETE(m_destroyer);
        }
    }
    
    void Player::Cleanup()
    {
        CloseLiveStream();
        CloseRecordedStream();
        if(m_localRecordBuffer)
            SAFE_DELETE(m_localRecordBuffer);
    }

    bool Player::OpenLiveStream(const PVR_CHANNEL& channel)
    {
        if(nullptr == m_delegate)
            return false;
        
        const ChannelId chId(channel.iUniqueId);
        string url = m_delegate->GetLiveStreamUrl(chId);
        if(url.empty())
            return false;

        m_lastBytesRead = c_InitialLastByteRead;
        bool succeeded = OpenLiveStream(chId, url);
        while(!succeeded) {
            string url = GetNextStreamUrl(chId);
            if(url.empty()) {// no more streams
                LogDebug("No alternative stream found.");
                XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32026));
                break;
            }
            XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32025));
            succeeded = OpenLiveStream(chId, url);
        }
        
        return succeeded;
        
    }
    
    bool Player::OpenLiveStream(ChannelId channelId, const std::string& url)
    {
        
        if(channelId == m_liveChannelId && IsLiveInRecording())
            return true; // Do not change url of local recording stream
        
        if(channelId == m_localRecordChannelId) {
            CLockObject lock(m_mutex);
            m_liveChannelId = m_localRecordChannelId;
            m_inputBuffer = m_localRecordBuffer;
            return true;
        }
        
        m_liveChannelId = UnknownChannelId;
        if (url.empty())
            return false;
        try
        {
            InputBuffer* buffer = BufferForUrl(url);
            CLockObject lock(m_mutex);
            m_inputBuffer = new Buffers::TimeshiftBuffer(buffer, CreateLiveCache());
            if(!m_inputBuffer->WaitForInput(m_channelReloadTimeout * 1000)) {
                throw InputBufferException("no data available diring reload timeout (bad ace link?)");
            }
        }
        catch (InputBufferException &ex)
        {
            LogError(  "PVRClientBase: input buffer error in OpenLiveStream: %s", ex.what());
            CloseLiveStream();
            OnOpenStremFailed(channelId, url);
            return false;
        }
        m_liveChannelId = channelId;
        return true;
    }

    int Player::ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize)
    {
        CLockObject lock(m_mutex);
        if(nullptr == m_inputBuffer){
            return -1;
        }
        
        ChannelId chId = GetLiveChannelId();
        int bytesRead = m_inputBuffer->Read(pBuffer, iBufferSize, m_channelReloadTimeout * 1000);
        // Assuming stream hanging.
        // Try to restart current channel only when previous read operation succeeded.
        if (bytesRead != iBufferSize &&  m_lastBytesRead >= 0 && !IsLiveInRecording()) {
            LogError("PVRClientBase:: trying to restart current channel.");
            string  url = m_inputBuffer->GetUrl();
            if(!url.empty()){
                XBMC->QueueNotification(QUEUE_INFO, XBMC_Message(32000));
                if(SwitchChannel(chId, url))
                    bytesRead = m_inputBuffer->Read(pBuffer, iBufferSize, m_channelReloadTimeout * 1000);
                else
                    bytesRead = -1;
            }
        }
        
        m_lastBytesRead = bytesRead;
        return bytesRead;
    }
    
    
    long long Player::SeekLiveStream(long long iPosition, int iWhence)
    {
        CLockObject lock(m_mutex);
        if(nullptr == m_inputBuffer){
            return -1;
        }
        return m_inputBuffer->Seek(iPosition, iWhence);
    }
    
    long long Player::PositionLiveStream()
    {
        CLockObject lock(m_mutex);
        if(nullptr == m_inputBuffer){
            return -1;
        }
        return m_inputBuffer->GetPosition();
    }
    
    long long Player::LengthLiveStream()
    {
        CLockObject lock(m_mutex);
        if(nullptr == m_inputBuffer){
            return -1;
        }
        return m_inputBuffer->GetLength();
    }
    

    void Player::CloseLiveStream()
    {
        CLockObject lock(m_mutex);
        m_liveChannelId = UnknownChannelId;
        if(m_inputBuffer && !IsLiveInRecording()) {
            LogNotice("PVRClientBase: closing input stream...");
            auto oldBuffer = m_inputBuffer;
            m_destroyer->PerformAsync([oldBuffer] (){
                LogDebug("PVRClientBase: destroying input stream...");
                delete oldBuffer;
                LogDebug("PVRClientBase: input stream been destroyed");
            }, [] (const ActionResult& result) {
                if(result.exception){
                    try {
                        std::rethrow_exception(result.exception);
                    } catch (std::exception ex) {
                        LogError("PVRClientBase: exception thrown during closing of input stream: %s.", ex.what());
                        
                    }
                } else {
                    LogNotice("PVRClientBase: input stream closed.");
                }
            });
        }
        
        m_inputBuffer = nullptr;
    }
#pragma mark - Recordings
    
    bool Player::OpenRecordedStream(const PVR_RECORDING &recording)
    {
        IPlayerDelegate::RecordingParams params;
        if(!m_delegate->GetRecordingParams(recording, params)) {
            LogError("Player: FAILED to open recording.")
            return false;
        }
        // If local recording
        if(params.flags & IPlayerDelegate::LocalRecording == IPlayerDelegate::LocalRecording) {
            try {
                InputBuffer* buffer = new DirectBuffer(new FileCacheBuffer(params.url));
                
                if(m_recordBuffer.buffer) {
                    SAFE_DELETE(m_recordBuffer.buffer);
                }
                m_recordBuffer.buffer = buffer;
                m_recordBuffer.duration = recording.iDuration;
            } catch (std::exception ex) {
                LogError("OpenRecordedStream (local) exception: %s", ex.what());
            }
            return true;

        }
    }

#pragma mark - Other
    

    bool Player::SwitchChannel(const PVR_CHANNEL& channel)
    {
        const ChannelId chId(channel.iUniqueId);
        string url = m_delegate->GetLiveStreamUrl(chId);
        if(url.empty())
            return false;
        
        CLockObject lock(m_mutex);
        CloseLiveStream();
        return OpenLiveStream(chId, url); // Split/join live and recording streams (when nesessry)
    }


    bool Player::IsRealTimeStream(void)
    {
        // Archive is not RTS
        if(m_recordBuffer.buffer)
            return false;
        // No timeshift means RTS
        if(!IsTimeshiftEnabled())
            return true;
        // Return true when timeshift buffer position close to end of buffer for < 10 sec
        // https://github.com/kodi-pvr/pvr.hts/issues/173
        CLockObject lock(m_mutex);
        if(nullptr == m_inputBuffer){
            return true;
        }
        double reliativePos = (double)(m_inputBuffer->GetLength() - m_inputBuffer->GetPosition()) / m_inputBuffer->GetLength();
        time_t timeToEnd = reliativePos * (m_inputBuffer->EndTime() - m_inputBuffer->StartTime());
        const bool isRTS = timeToEnd < 10;
        LogDebug("PVRClientBase: is RTS? %s. Reliative pos: %f. Time to end: %d", ((isRTS) ? "YES" : "NO"), reliativePos, timeToEnd );
        return isRTS;
    }

    
    bool Player::CanPauseStream()
    {
        return IsTimeshiftEnabled();
    }
    
    bool Player::CanSeekStream()
    {
        return IsTimeshiftEnabled();
    }
    
    PVR_ERROR Player::GetStreamTimes(PVR_STREAM_TIMES *times)
    {
        
        if (!times)
            return PVR_ERROR_INVALID_PARAMETERS;
        //    if(!IsTimeshiftEnabled())
        //        return PVR_ERROR_NOT_IMPLEMENTED;
        
        int64_t timeStart = 0;
        int64_t  timeEnd = 0;
        if (m_inputBuffer)
        {
            
            CLockObject lock(m_mutex);
            timeStart = m_inputBuffer->StartTime();
            timeEnd   = m_inputBuffer->EndTime();
        }
        else if (m_recordBuffer.buffer){
            {
                timeStart = 0;
                timeEnd   = m_recordBuffer.duration;
            }
        }
        else
            return PVR_ERROR_NOT_IMPLEMENTED;
        
        times->startTime = timeStart;
        times->ptsStart  = 0;
        times->ptsBegin  = 0;
        times->ptsEnd    = (timeEnd - timeStart) * DVD_TIME_BASE;
        return PVR_ERROR_NO_ERROR;
    }

    
    bool Player::StartRecordingFor(const PVR_TIMER &timer)
    {
        std::string url = m_delegate->InitLocalRecordingFor(timer);
        
        if(url.empty()) {
            return false;
        }
        
        m_localRecordChannelId = timer.iClientChannelUid;
        // When recording channel is same to live channel
        // merge live buffer with local recording
        if(m_liveChannelId == timer.iClientChannelUid){
            //        CLockObject lock(m_mutex);
            //        CloseLiveStream();
            m_inputBuffer->SwapCache( new Buffers::FileCacheBuffer(recordingDir, 255, false));
            m_localRecordBuffer = m_inputBuffer;
            m_liveChannelId = m_localRecordChannelId; // ???
            return true;
        }
        // otherwise just open new recording stream
        m_localRecordBuffer = new Buffers::TimeshiftBuffer(BufferForUrl(url), new Buffers::FileCacheBuffer(recordingDir, 255, false));
        
        return true;
    }
#pragma mark - Timers
    bool Player::StopRecordingFor(const PVR_TIMER &timer)
    {
        m_delegate->FinalizeLocalRecordingFor(timer);
        
        // When recording channel is same to live channel
        // merge live buffer with local recording
        if(m_liveChannelId == timer.iClientChannelUid){
            //CLockObject lock(m_mutex);
            m_inputBuffer->SwapCache(CreateLiveCache());
            m_localRecordBuffer = nullptr;
        } else {
            if(m_localRecordBuffer) {
                m_localRecordChannelId = UnknownChannelId;
                SAFE_DELETE(m_localRecordBuffer);
            }
        }
        
        // trigger Kodi recordings update
        PVR->TriggerRecordingUpdate();
        return true;
        
    }
    

    
    InputBuffer*  Player::BufferForUrl(const std::string& url )
    {
        InputBuffer* buffer = NULL;
        const std::string m3uExt = ".m3u";
        const std::string m3u8Ext = ".m3u8";
        if( url.find(m3u8Ext) != std::string::npos || url.find(m3uExt) != std::string::npos)
            buffer = new Buffers::PlaylistBuffer(url, nullptr, false); // No segments cache for live playlist
        else
            buffer = new DirectBuffer(url);
        return buffer;
    }
    
    Buffers::ICacheBuffer* Player::CreateLiveCache() const {
        if (IsTimeshiftEnabled()){
            if(IPlayerDelegate::k_TimeshiftBufferFile == m_delegate->GetTimeshiftBufferType()) {
                return new Buffers::FileCacheBuffer(m_cacheDir, m_timshiftBufferSize /  Buffers::FileCacheBuffer::CHUNK_FILE_SIZE_LIMIT);
            } else {
                return new Buffers::MemoryCacheBuffer(m_timshiftBufferSize /  Buffers::MemoryCacheBuffer::CHUNK_SIZE_LIMIT);
            }
        }
        else
            return new Buffers::SimpleCyclicBuffer(m_cacheSizeLimit / Buffers::SimpleCyclicBuffer::CHUNK_SIZE_LIMIT);
        
    }
    

}
