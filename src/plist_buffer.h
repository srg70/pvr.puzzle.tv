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

#ifndef plist_buffer_h
#define plist_buffer_h


#include <string>
#include <memory>
#include <map>
#include <vector>
#include <list>
#include "p8-platform/threads/threads.h"
#include "p8-platform/util/buffer.h"
#include "input_buffer.h"

namespace Buffers
{
    class IPlaylistBufferDelegate
    {
    public:
        virtual time_t Duration() const= 0;
        virtual std::string UrlForTimeshift(time_t timeshift, time_t* timeshiftAdjusted) const = 0;
    };
    typedef  std::shared_ptr<IPlaylistBufferDelegate> PlaylistBufferDelegate;
    
    class PlaylistBuffer :  public InputBuffer, public P8PLATFORM::CThread
    {
    public:
        PlaylistBuffer(const std::string &streamUrl,  PlaylistBufferDelegate delegate);
        ~PlaylistBuffer();
        
        int64_t GetLength() const;
        int64_t GetPosition() const;
        int64_t Seek(int64_t iPosition, int iWhence);
        ssize_t Read(unsigned char *buffer, size_t bufferSize, uint32_t timeoutMs);
        bool SwitchStream(const std::string &newUrl);
        
        /*!
         * @brief Stop the thread
         * @param iWaitMs negative = don't wait, 0 = infinite, or the amount of ms to wait
         */
        virtual bool StopThread(int iWaitMs = 5000);
        
    private:
        class Segment
        {
        public:
            Segment(float duration);
            Segment(const uint8_t* buffer, size_t size, float duration);
            void Push(const uint8_t* buffer, size_t size);
            const uint8_t* Pop(size_t requesred, size_t*  actual);
            size_t Read(uint8_t* buffer, size_t size);
            size_t Seek(size_t position);
            size_t Position() const  {return _begin - &_data[0];}
            size_t BytesReady() const {return  _size - Position();}
            float Bitrate() const { return  _duration == 0.0 ? 0.0 : _size/_duration;}
            float Duration() const {return _duration;}
            size_t Length() const {return _size;}
            
            ~Segment();
        private:
            uint8_t* _data;
            size_t _size;
            const uint8_t* _begin;
            const float _duration;
        };
        typedef std::map<uint64_t, std::pair<float, std::string> > TSegmentUrls;
        typedef std::map<time_t, std::shared_ptr<Segment> >  TSegments;
        
        TSegmentUrls m_segmentUrls;
        TSegments m_segments;
        int64_t m_lastSegment;
        std::string  m_playListUrl;
        mutable P8PLATFORM::CMutex m_syncAccess;
        P8PLATFORM::CEvent m_writeEvent;
        bool m_isVod;
        int64_t m_totalLength;
        float m_totalDuration;
        PlaylistBufferDelegate m_delegate;
        int64_t m_position;
        time_t m_writeTimshift;
        time_t m_readTimshift;
        
        void *Process();
        void Init(const std::string &playlistUrl);
        void Init(const std::string &playlistUrl, bool cleanContent, int64_t position, time_t timeshift);
        bool ParsePlaylist(const std::string& data);
        void SetBestPlaylist(const std::string& playlistUrl);
        void LoadPlaylist(std::string& data);
        bool FillSegment(const TSegmentUrls::mapped_type& segment);
        bool IsStopped(uint32_t timeoutInSec = 0);
        float Bitrate() const {
            return m_totalLength / (m_totalDuration + 0.01);
        }
    };
    
    class PlistBufferException : public InputBufferException
    {
    public:
        PlistBufferException(const char* reason = "")
        : m_reason(reason)
        , InputBufferException(NULL)
        {}
        virtual const char* what() const noexcept {return m_reason.c_str();}
        
    private:
        std::string m_reason;
    };
    
}
#endif //plist_buffer_h
