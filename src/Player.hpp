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

#ifndef __Player_hpp__
#define __Player_hpp__

#include "addon.h"
#include "p8-platform/threads/mutex.h"


namespace Buffers {
    class InputBuffer;
    class TimeshiftBuffer;
    class ICacheBuffer;
}

namespace ActionQueue {
    class CActionQueue;
}

namespace PvrClient
{
    class Player : public IPlayer {
    public:
        Player();
        virtual void SetLiveDelegate(IPlayerDelegate* delegate);
        // ITimersEngineDelegate
        virtual bool StartRecordingFor(const PVR_TIMER &timer);
        virtual bool StopRecordingFor(const PVR_TIMER &timer);

        // ILivePlayer
        virtual bool OpenLiveStream(const PVR_CHANNEL &channel);
        virtual void CloseLiveStream(void);
        virtual bool SwitchChannel(const PVR_CHANNEL &channel);
        virtual bool CanPauseStream(void);
        virtual bool CanSeekStream(void);
        virtual bool IsRealTimeStream(void);
        virtual PVR_ERROR GetStreamTimes(PVR_STREAM_TIMES *times);
        virtual long long SeekLiveStream(long long iPosition, int iWhence);
        virtual long long PositionLiveStream(void);
        virtual long long LengthLiveStream(void) ;
        virtual int ReadLiveStream(unsigned char* pBuffer, unsigned int iBufferSize);
        // IRecordingPlayer
        virtual bool OpenRecordedStream(const PVR_RECORDING &recording);
        virtual void CloseRecordedStream(void);
        virtual PVR_ERROR GetStreamReadChunkSize(int* chunksize);
        virtual int ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize);
        virtual long long SeekRecordedStream(long long iPosition, int iWhence /* = SEEK_SET */);
        virtual long long PositionRecordedStream(void);
        virtual long long LengthRecordedStream(void);

    protected:
        virtual ~Player();
    private:
        IPlayerDelegate* m_delegate;
        
        static Buffers::InputBuffer*  BufferForUrl(const std::string& url );

        // Live player
        bool OpenLiveStream(ChannelId channelId, const std::string& url);
        inline bool IsLiveInRecording() const { return m_inputBuffer == m_localRecordBuffer; }
        inline bool IsTimeshiftEnabled() const { return nullptr == m_delegate ? false : m_delegate->GetTimeshiftBufferType() != IPlayerDelegate::k_TimeshiftBufferNone; }

        Buffers::ICacheBuffer* CreateLiveCache() const;
        void Cleanup();
        
        int m_lastBytesRead;
        ChannelId m_liveChannelId;
        mutable P8PLATFORM::CMutex m_mutex;
        Buffers::TimeshiftBuffer *m_inputBuffer;
        ActionQueue::CActionQueue* m_destroyer;

        struct {
            Buffers::InputBuffer * buffer;
            time_t duration;
        } m_recordBuffer;

        ChannelId m_localRecordChannelId;
        Buffers::TimeshiftBuffer *m_localRecordBuffer;


    };
}
#endif /* __Player_hpp__ */
