//
//  file_cache_buffer.cpp
//  pvr.puzzle.tv
//
//  Created by Sergey Shramchenko on 04/08/2017.
//  Copyright © 2017 Home. All rights reserved.
//

#define NOMINMAX
#include <algorithm>
#include "memory_cache_buffer.hpp"
#include "libXBMC_addon.h"
#include "helpers.h"


namespace Buffers
{
    using namespace P8PLATFORM;
    using namespace ADDON;
  
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
            ssize_t writtenBytes = std::min(int64_t(bytesToWrite), m_buffer.size - m_writePos);
            if(writtenBytes <= 0) {
                return 0; // even for EOF.
            }
            memcpy(m_buffer.ptr + m_writePos, buffer, writtenBytes);
            m_writePos += writtenBytes;
            return writtenBytes;
        }

        int64_t ReadPos() const {return m_readPos;}
        int64_t WritePos() const {return m_writePos;}
        int64_t Capacity() const {return m_buffer.size;}
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
    
    
    
    MemoryCacheBuffer::MemoryCacheBuffer(ADDON::CHelper_libXBMC_addon *addonHelper, uint8_t  sizeFactor)
    : m_addonHelper(addonHelper)
    , m_maxSize(std::max(uint8_t(3), sizeFactor) * CHUNK_SIZE_LIMIT)
    {
        Init();
    }
    void MemoryCacheBuffer::Init() {
        m_length = 0;
        m_position = 0;
        m_begin = 0;
        m_ReadChunks.clear();
        m_ChunkSwarm.clear();
    }
    
    uint32_t MemoryCacheBuffer::UnitSize() {
        return STREAM_READ_BUFFER_SIZE;
    }
    
    // Seak read position within cache window
    int64_t MemoryCacheBuffer::Seek(int64_t iPosition, int iWhence) {
        unsigned int idx = -1;
        ChunkPtr chunk = NULL;
        {
            m_addonHelper->Log(LOG_DEBUG, "TimeshiftBuffer::Seek. >>> Requested pos %d", iPosition);

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
            m_addonHelper->Log(LOG_DEBUG, "TimeshiftBuffer::Seek. Calculated pos %d", iPosition);
            m_addonHelper->Log(LOG_DEBUG, "TimeshiftBuffer::Seek. Begin %d Length %d", m_begin, m_length);

            idx = GetChunkIndexFor(iPosition);
            if(idx >= m_ReadChunks.size()) {
                m_addonHelper->Log(LOG_ERROR, "TimeshiftBuffer: seek failed. Wrong chunk index %d", idx);
                return -1;
            }
            chunk = m_ReadChunks[idx];
        }
        auto inPos = GetPositionInChunkFor(iPosition);
        auto pos =  chunk->Seek(inPos);
        m_position = iPosition -  (inPos - pos);
        m_addonHelper->Log(LOG_DEBUG, "TimeshiftBuffer::Seek. Chunk idx %d, pos in chunk %d, actual pos %d", idx, inPos, pos);
        m_addonHelper->Log(LOG_DEBUG, "TimeshiftBuffer::Seek. <<< Result pos %d", m_position);
        return iPosition;
        
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
        
        ChunkPtr chunk = NULL;
        while (totalBytesRead < bufferSize) {
            unsigned int idx = GetChunkIndexFor(m_position);
            {
                CLockObject lock(m_SyncAccess);
                chunk = (idx >= m_ReadChunks.size()) ? NULL : m_ReadChunks[idx];
            }
            
            if(NULL == chunk)  {
                m_addonHelper->Log(LOG_ERROR, "TimeshiftBuffer: failed to obtain chunk for read.");
                break;
            }
            
            size_t bytesToRead = bufferSize - totalBytesRead;
            ssize_t bytesRead = chunk->Read( ((uint8_t*)buffer) + totalBytesRead, bytesToRead);
            if(bytesRead == 0 ) {
                m_addonHelper->Log(LOG_NOTICE, "TimeshiftBuffer: nothing to read.");
                break;
            }
            //DebugLog(std::string(">>> Read: ") + n_to_string(bytesRead));
            totalBytesRead += bytesRead;
            m_position += bytesRead;
            if(chunk->ReadPos() == chunk->WritePos() && chunk->WritePos() == chunk->Capacity()) {
                chunk = NULL;
            }
        }
        if(NULL == chunk) {
            CLockObject lock(m_SyncAccess);
            // Free oldest chunk one unit before max size
            // NOTE: write will not wait, just will drop current unit.
            while(m_length - m_begin >=  m_maxSize - UnitSize())
            {
                m_begin  +=CHUNK_SIZE_LIMIT;
                m_ReadChunks.pop_front();
                m_ChunkSwarm.pop_front();
            }
        }
        return totalBytesRead;
        
    }
    
    // Write interface
    ssize_t MemoryCacheBuffer::Write(const void* buf, size_t bufferSize) {
        
        const uint8_t* buffer = (const uint8_t*)  buf;
        ssize_t totalWritten = 0;
        try {
            
            ChunkPtr chunk = NULL;
            while (bufferSize) {
                // Create new chunk if nesessary
                if(NULL == chunk)  {
                    CLockObject lock(m_SyncAccess);
                    
                    if(m_ReadChunks.size()) {
                        chunk = m_ReadChunks.back();
                        if(chunk->WritePos() == chunk->Capacity()) {
                            chunk = CreateChunk();
                            // No room for new data
                            if(NULL == chunk)
                                return  totalWritten;
                            m_ReadChunks.push_back(chunk);
                        }
                    }
                    else {
                        chunk = CreateChunk();
                        m_ReadChunks.push_back(chunk);
                    }
                }
                
                size_t available = chunk->Capacity() - chunk->WritePos();
                const size_t bytesToWrite = std::min(available, bufferSize);
                // Write bytes
                const ssize_t bytesWritten = chunk->Write(buffer, bytesToWrite);
                {
                    //CLockObject lock(m_SyncAccess);
                    m_length += bytesWritten;
                }
                totalWritten += bytesWritten;
                if(bytesWritten != bytesToWrite) {
                    m_addonHelper->Log(LOG_ERROR, "TimeshiftBuffer: write cache error, written (%d) != read (%d)", bytesWritten,bytesToWrite);
                    //break;// ???
                }
                available -= bytesWritten;
                buffer += bytesWritten;
                bufferSize -= bytesWritten;
                if(available <= 0) {
//                    chunk->m_writer.Close();
                    chunk = NULL;
                }
            }
            
        } catch (std::exception&  ex) {
            m_addonHelper->Log(LOG_ERROR, "TimeshiftBuffer: generic exception %s", ex.what());
        }
        
        return totalWritten;
        
    }
    
    MemoryCacheBuffer::ChunkPtr MemoryCacheBuffer::CreateChunk()
    {
        // No room for new data
        if(m_length - m_begin >=  m_maxSize ) {
            return NULL;
        }
        try {
            ChunkPtr newChunk = new CMemoryBlock();
            m_ChunkSwarm.push_back(ChunkSwarm::value_type(newChunk));
            m_addonHelper->Log(LOG_DEBUG, ">>> TimeshiftBuffer: new current chunk (for write). Total %d", m_ChunkSwarm.size());
            return newChunk;
        } catch (std::exception& ex) {
            m_addonHelper->Log(LOG_DEBUG, ">>> TimeshiftBuffer: allocation of new chunck failed. Exception: %s", ex.what());
        }
        return NULL;
    }
    
    unsigned int MemoryCacheBuffer::GetChunkIndexFor(int64_t pos) {
        pos -= m_begin;
        return pos / CHUNK_SIZE_LIMIT;
    }
    int64_t MemoryCacheBuffer::GetPositionInChunkFor(int64_t pos) {
        pos -= m_begin;
        return pos % CHUNK_SIZE_LIMIT;
    }
    
    
    MemoryCacheBuffer::~MemoryCacheBuffer(){
        m_ReadChunks.clear();
    }
    
} // namespace
