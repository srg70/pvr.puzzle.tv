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


#ifndef __memory_cache_buffer_hpp__
#define __memory_cache_buffer_hpp__

#include <memory>
#include <string>
#include <vector>
#include <deque>
#include "cache_buffer.h"
#include "p8-platform/threads/mutex.h"

namespace Buffers
{
    
    class CMemoryBlock;
    
    class MemoryCacheBuffer : public ICacheBuffer
    {
    public:
        static const uint32_t STREAM_READ_BUFFER_SIZE = 1024 * 32; // 32K input read buffer
        static const  uint32_t CHUNK_SIZE_LIMIT = STREAM_READ_BUFFER_SIZE; //(STREAM_READ_BUFFER_SIZE * 1024) * 4; // 128MB chunk

        
        MemoryCacheBuffer(uint32_t  sizeFactor);
        
        virtual  void Init();
        virtual  uint32_t UnitSize();

        
        // Read interface
        // Seak read position within cache window
        virtual int64_t Seek(int64_t iFilePosition, int iWhence) ;
        // Virtual steream lenght.
        virtual int64_t Length();
        // Current read position
        virtual int64_t Position();
        // Reads data from Position(),
        virtual ssize_t Read(void* lpBuf, size_t uiBufSize);
        
        // Write interface
        virtual bool LockUnitForWrite(uint8_t** pBuf);
        virtual void UnlockAfterWriten(uint8_t* pBuf, ssize_t writtenBytes = -1);

        ~MemoryCacheBuffer();
        
    private:
        typedef CMemoryBlock* ChunkPtr;
        typedef std::deque<ChunkPtr> Chunks;
        typedef std::deque<std::unique_ptr<CMemoryBlock> > ChunkSwarm;

        ChunkPtr CreateChunk();
        unsigned int GetChunkIndexFor(int64_t position);
        int64_t GetPositionInChunkFor(int64_t position);

        
        mutable Chunks m_ReadChunks;
        ChunkSwarm m_ChunkSwarm;
        mutable P8PLATFORM::CMutex m_SyncAccess;
        int64_t m_length;
        int64_t m_position;
        int64_t m_begin;// virtual start of cache
        const int64_t m_maxSize;
        ChunkPtr m_lockedChunk;
    };
}
#endif // __memory_cache_buffer_hpp__
