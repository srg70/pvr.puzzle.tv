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

#define NOMINMAX
#include <algorithm>
#include "memory_cache_buffer.hpp"
#include "libXBMC_addon.h"
#include "helpers.h"
#include "globals.hpp"

namespace Buffers
{
    using namespace P8PLATFORM;
    using namespace ADDON;
    using namespace Globals;
    
    class CMemoryBlock
    {
    public:
        CMemoryBlock()
        : m_readPos (0)
        , m_writePos(0)
        {
            m_buffer.ptr = new uint8_t[MemoryCacheBuffer::CHUNK_SIZE_LIMIT];
            m_buffer.size = MemoryCacheBuffer::CHUNK_SIZE_LIMIT;
        }
        ~CMemoryBlock()
        {
            if(NULL != m_buffer.ptr) {
                delete[] m_buffer.ptr;
                m_buffer.ptr = NULL;
            }
        }
        int64_t Seek(int64_t iPosition) {
            m_readPos = iPosition;
            m_readPos = std::min(std::max(int64_t(0), m_readPos), m_writePos);
            return m_readPos;
        }
        ssize_t Read(uint8_t* buffer,  size_t bytesToRead) {
            ssize_t readBytes = std::min(int64_t(bytesToRead), m_writePos - m_readPos);
            if(readBytes <= 0) {
                return 0; // even for EOF.
            }
            memcpy(buffer, m_buffer.ptr + m_readPos, readBytes);
            m_readPos += readBytes;
            return readBytes;
        }
        ssize_t Write(const uint8_t* buffer,  size_t bytesToWrite) {
            ssize_t writtenBytes = std::min(int64_t(bytesToWrite), Available());
            if(writtenBytes <= 0) {
                return 0; // even for EOF.
            }
            memcpy(m_buffer.ptr + m_writePos, buffer, writtenBytes);
            m_writePos += writtenBytes;
            return writtenBytes;
        }
        
        uint8_t* LockForWrite() {
             return m_buffer.ptr + m_writePos;
        }
        void UnlockAfterWriten(size_t writtenBytes){
            int64_t bytesToWrite = std::min(int64_t(writtenBytes), Available());
            if(bytesToWrite != writtenBytes)
                LogError("Error: CMemoryBlock::UnlockAfterWriten() chunk overflow on write! Data will be truncated.");
            m_writePos +=  bytesToWrite;
        }

        inline int64_t ReadPos() const {return m_readPos;}
        inline int64_t WritePos() const {return m_writePos;}
        inline int64_t Capacity() const {return m_buffer.size;}
        inline int64_t Available() const {return Capacity() - WritePos();}
        bool IsMyBuffer (const uint8_t* ptr) const {return ptr == m_buffer.ptr + m_writePos;}
    private:
        struct {
            uint8_t* ptr;
            int64_t  size;
        } m_buffer;
        int64_t m_readPos;
        int64_t m_writePos;
    };
    
    ///////////////////////////////////////////
    //              MemoryCacheBuffer
    //////////////////////////////////////////
    
    
    
    MemoryCacheBuffer::MemoryCacheBuffer(uint32_t  sizeFactor)
    : m_maxSize(std::max(uint32_t(3), sizeFactor) * CHUNK_SIZE_LIMIT)
    {
        //Init();
    }
    void MemoryCacheBuffer::Init() {
        m_length = 0;
        m_position = 0;
        m_begin = 0;
        m_ReadChunks.clear();
        m_ChunkSwarm.clear();
        m_lockedChunk = nullptr;
    }
    
    uint32_t MemoryCacheBuffer::UnitSize() {
        return STREAM_READ_BUFFER_SIZE;
    }
    
    // Seak read position within cache window
    int64_t MemoryCacheBuffer::Seek(int64_t iPosition, int iWhence) {
        unsigned int idx = -1;
        ChunkPtr chunk = NULL;
        {
            LogDebug("MemoryCacheBuffer::Seek. >>> Requested pos %lld", iPosition);
            
            CLockObject lock(m_SyncAccess);
            
            // Translate position to offset from start of buffer.
            if(iWhence == SEEK_CUR) {
                iPosition = m_position + iPosition;
            } else if(iWhence == SEEK_END) {
                iPosition = m_length + iPosition;
            }
            if(iPosition > m_length) {
                iPosition = m_length;
            }
            if(iPosition < m_begin) {
                iPosition = m_begin;
            }
            iWhence = SEEK_SET;
            LogDebug("MemoryCacheBuffer::Seek. Calculated pos %lld", iPosition);
            LogDebug("MemoryCacheBuffer::Seek. Begin %lld Length %lld", m_begin, m_length);
            
            idx = GetChunkIndexFor(iPosition);
            if(idx >= m_ReadChunks.size()) {
                LogError("MemoryCacheBuffer: seek failed. Wrong chunk index %d", idx);
                return m_position;
            }
            chunk = m_ReadChunks[idx];
            
            auto inPos = GetPositionInChunkFor(iPosition);
            auto pos =  chunk->Seek(inPos);
            m_position = iPosition -  (inPos - pos);
            LogDebug("MemoryCacheBuffer::Seek. Chunk idx %d, pos in chunk %lld, actual pos %lld", idx, inPos, pos);
        }
        LogDebug("MemoryCacheBuffer::Seek. <<< Result pos %lld", m_position);
        return m_position;
        
    }
    
    // Virtual steream lenght.
    int64_t MemoryCacheBuffer::Length() {
        int64_t length = -1;
        {
            //            CLockObject lock(m_SyncAccess);
            length = m_length;
        }
        return length;
    }
    
    // Current read position
    int64_t  MemoryCacheBuffer::Position() {
        
        int64_t pos = m_position;
        //        {
        //            CLockObject lock(m_SyncAccess);
        //            pos = m_position;
        //        }
        return pos;
    }
    
    // Reads data from Position(),
    ssize_t MemoryCacheBuffer::Read(void* buffer, size_t bufferSize) {
        
        size_t totalBytesRead = 0;
        size_t bytesRead = 0;
        
        ChunkPtr chunk = NULL;
        while (totalBytesRead < bufferSize) {
            {
                CLockObject lock(m_SyncAccess);
                unsigned int idx = GetChunkIndexFor(m_position);
                chunk = (idx >= m_ReadChunks.size()) ? NULL : m_ReadChunks[idx];
                if(NULL == chunk)  {
                    LogError("MemoryCacheBuffer: failed to obtain chunk for read. %d of %d", idx, m_ReadChunks.size());
                    break;
                }
                chunk->Seek(GetPositionInChunkFor(m_position));
                size_t bytesToRead = bufferSize - totalBytesRead;
                bytesRead = chunk->Read( ((uint8_t*)buffer) + totalBytesRead, bytesToRead);
                m_position += bytesRead;
            }
            if(bytesRead == 0 ) {
                //LogDebug("MemoryCacheBuffer: nothing to read.");
                break;
            }
            //DebugLog(std::string(">>> Read: ") + n_to_string(bytesRead));
            totalBytesRead += bytesRead;
            if(chunk->ReadPos() == chunk->Capacity()) {
                chunk = NULL;
            }
        }
        // Do we have memory before read position to free?
        if(GetChunkIndexFor(m_position) > 0) {
            CLockObject lock(m_SyncAccess);
            // Free oldest chunks at one MByte before max size
            // NOTE: write will not wait, just will drop current unit.
            while(m_length - m_begin >=  m_maxSize - 1024*1024 && GetChunkIndexFor(m_position) > 0)
            {
                m_begin  +=m_ReadChunks.front()->Capacity();
                m_ReadChunks.pop_front();
                m_ChunkSwarm.pop_front();
            }
        }
        return totalBytesRead;
        
    }
    
    // Write interface
    bool MemoryCacheBuffer::LockUnitForWrite(uint8_t** pBuf) {
        if(pBuf == nullptr) {
            LogError("Error: MemoryCacheBuffer::LockUnitForWrite() null pointer for buffer. ");
            return false;
        }
        if(m_lockedChunk != nullptr) {
            LogError("Error: MemoryCacheBuffer::LockUnitForWrite() uinit already locked.");
            return false;
        }
        ChunkPtr chunk = nullptr;
        *pBuf = nullptr;
        CLockObject lock(m_SyncAccess);
        if(m_ReadChunks.size()) {
            chunk = m_ReadChunks.back();
            // If chunck is full?
            if(chunk->Available() < UnitSize()) {
                chunk = CreateChunk();
                // No room for new data
                if(NULL == chunk)
                    return false;
                m_ReadChunks.push_back(chunk);
            }
        }
        else {
            chunk = CreateChunk();
            m_ReadChunks.push_back(chunk);
        }
        m_lockedChunk = chunk;
        *pBuf = chunk->LockForWrite();
        return true;
    }
    void MemoryCacheBuffer::UnlockAfterWriten(uint8_t* pBuf, ssize_t writtenBytes) {
        if(m_lockedChunk == nullptr){
            LogError("Error: MemoryCacheBuffer::UnlockAfterWriten() no locked chunk.");
            return;
        }
        if(!m_lockedChunk->IsMyBuffer(pBuf)) {
            LogError("Error: MemoryCacheBuffer::UnlockAfterWriten() wrong buffer to unlock.");
        } else {
            size_t byteToUnlock = writtenBytes < 0  ? UnitSize() : writtenBytes;
            m_lockedChunk->UnlockAfterWriten(byteToUnlock);
            m_length += byteToUnlock;
        }
        m_lockedChunk = nullptr;
    }

//    ssize_t MemoryCacheBuffer::Write(const void* buf, size_t bufferSize) {
//
//        const uint8_t* buffer = (const uint8_t*)  buf;
//        ssize_t totalWritten = 0;
//        try {
//
//            ChunkPtr chunk = NULL;
//            while (bufferSize) {
//                // Create new chunk if nesessary
//                if(NULL == chunk)  {
//                    CLockObject lock(m_SyncAccess);
//                    if(m_ReadChunks.size()) {
//                        chunk = m_ReadChunks.back();
//                        // If chunck is full?
//                        if(chunk->WritePos() >= chunk->Capacity()) {
//                            chunk = CreateChunk();
//                            // No room for new data
//                            if(NULL == chunk)
//                                return  totalWritten;
//                            m_ReadChunks.push_back(chunk);
//                        }
//                    }
//                    else {
//                        chunk = CreateChunk();
//                        m_ReadChunks.push_back(chunk);
//                    }
//                }
//
//                size_t available = std::max(int64_t(0), chunk->Available());
//                const size_t bytesToWrite = std::min(available, bufferSize);
//                // Write bytes
//                ssize_t bytesWritten = 0;
//                {
//                    CLockObject lock(m_SyncAccess);
//                    bytesWritten = chunk->Write(buffer, bytesToWrite);
//                    m_length += bytesWritten;
//                }
//                totalWritten += bytesWritten;
//                if(bytesWritten != bytesToWrite) {
//                    XBMC->Log(LOG_INFO, "MemoryCacheBuffer: chunk is full, written (%d) != to write (%d)", bytesWritten,bytesToWrite);
//                    //break;// ???
//                }
//                available -= bytesWritten;
//                buffer += bytesWritten;
//                bufferSize -= bytesWritten;
//                if(available <= 0) {
//                    chunk = NULL;
//                }
//            }
//
//        } catch (std::exception&  ex) {
//            LogError("MemoryCacheBuffer: generic exception %s", ex.what());
//        }
//
//        return totalWritten;
//
//    }
//
    MemoryCacheBuffer::ChunkPtr MemoryCacheBuffer::CreateChunk()
    {
        // No room for new data
        if(m_length - m_begin >=  m_maxSize ) {
            return NULL;
        }
        try {
            ChunkPtr newChunk = new CMemoryBlock();
            m_ChunkSwarm.push_back(ChunkSwarm::value_type(newChunk));
            LogDebug(">>> MemoryCacheBuffer: new current chunk (for write). Total %d", m_ChunkSwarm.size());
            return newChunk;
        } catch (std::exception& ex) {
            LogDebug(">>> MemoryCacheBuffer: allocation of new chunck failed. Exception: %s", ex.what());
        }
        return NULL;
    }
    
    unsigned int MemoryCacheBuffer::GetChunkIndexFor(int64_t pos) {
        pos -= m_begin;
        if(pos < 0)
            return 0;
        return  pos / CHUNK_SIZE_LIMIT;
    }
    int64_t MemoryCacheBuffer::GetPositionInChunkFor(int64_t pos) {
        pos -= m_begin;
        if(pos < 0)
            return 0;
        return pos % CHUNK_SIZE_LIMIT;
    }
    
    
    MemoryCacheBuffer::~MemoryCacheBuffer(){
        m_ReadChunks.clear();
    }
    
} // namespace
