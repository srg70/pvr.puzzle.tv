/*
 *
 *   Copyright (C) 2018 Sergey Shramchenko
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

#ifndef __simple_cyclic_buffer_hpp__
#define __simple_cyclic_buffer_hpp__

#include "cache_buffer.h"
#include "globals.hpp"
#include "p8-platform/util/buffer.h"
#include <memory>
#include <vector>

namespace Buffers
{
    class SimpleCyclicBuffer : public ICacheBuffer
    {
    public:
        static const uint32_t CHUNK_SIZE_LIMIT = 1024 * 32; // 32K input read buffer

    private:
        struct Unit {
            static const uint32_t size = CHUNK_SIZE_LIMIT;
            Unit() : pos(0) {
                buf = new unsigned char[size];
            }
            ~Unit() {
                delete [] buf;
            }
            unsigned char* buf;
            int64_t pos;
        };
        typedef P8PLATFORM::SyncedBuffer <Unit*> Units;

        std::vector<std::unique_ptr<Unit> > m_unitsSwamp;
        Units m_freeUnits;
        Units m_fullUnits;
        Unit* m_currentUnit;
        Unit* m_lockedChunk;
        const uint64_t m_unitsLimit;
    public:
        SimpleCyclicBuffer(uint64_t maxSize = 1500)
        : m_unitsLimit (maxSize)
        , m_freeUnits(maxSize)
        , m_fullUnits(maxSize)
        {
           // Init();
        }
        
        virtual  void Init() {
            m_fullUnits.Clear();
            m_freeUnits.Clear();
            m_unitsSwamp.clear();
            m_currentUnit = m_lockedChunk = nullptr;
            int cnt = m_unitsLimit;
            while(cnt--){
                std::unique_ptr<Unit> p(new Unit());
                m_freeUnits.Push(p.get());
                m_unitsSwamp.push_back(std::move(p));
            }
        }
        virtual  uint32_t UnitSize() { return Unit::size;}
        
        
        // Read interface
        // Seak read position within cache window
        virtual int64_t Seek(int64_t iFilePosition, int iWhence) {return -1;}
        // Virtual steream lenght.
        virtual int64_t Length() {return -1;}
        // Current read position
        virtual int64_t Position() {return -1;}
        
        // Reads data from Position(),
        virtual ssize_t Read(void* lpBuf, size_t uiBufSize) {
            size_t totalRead = 0;
            while(totalRead < uiBufSize){
                if(nullptr == m_currentUnit) {
                    if(!m_fullUnits.Pop(m_currentUnit)){
                        Globals::LogDebug("SimpleCyclicBuffer::Read(): nothing to read!");
                        break;
                    }
                }
                
                int64_t bytesToRead = uiBufSize - totalRead;
                ssize_t readBytes = std::min(bytesToRead, Unit::size - m_currentUnit->pos);
                if(readBytes > 0) {
                    memcpy(((uint8_t*)lpBuf) + totalRead, m_currentUnit->buf + m_currentUnit->pos, readBytes);
                    m_currentUnit->pos += readBytes;
                    totalRead += readBytes;
                }
                if(m_currentUnit->pos == Unit::size){// Unit empty
                    m_currentUnit->pos = 0;
                    m_freeUnits.Push(m_currentUnit);
//                    Globals::LogDebug("SimpleCyclicBuffer::Read(): free unit.");
                    m_currentUnit = nullptr;
                }
            }
//            Globals::LogDebug("SimpleCyclicBuffer::Read() read %d bytes", totalRead);
            return totalRead;
        }
        
        // Write interface
        virtual bool LockUnitForWrite(uint8_t** pBuf) {
            if(pBuf == nullptr) {
                Globals::LogError("Error: SimpleCyclicBuffer::LockUnitForWrite() null pointer for buffer. ");
                return false;
            }
            *pBuf = nullptr;
            if(m_lockedChunk != nullptr) {
                Globals::LogError("Error: SimpleCyclicBuffer::LockUnitForWrite() overlocking of chunk for write. ");
                return false;
            }

            if(!m_freeUnits.Pop(m_lockedChunk)) {
                Globals::LogDebug("SimpleCyclicBuffer::LockUnitForWrite() no chunk available for write.");
                return false;
            }
            *pBuf =  m_lockedChunk->buf;
            return true;
        }
        
        virtual void UnlockAfterWriten(uint8_t* pBuf, ssize_t writtenBytes = -1){
            if(m_lockedChunk == nullptr){
                Globals::LogError("Error: SimpleCyclicBuffer::UnlockAfterWriten() no locked chunk.");
                return;
            }
            if(m_lockedChunk->buf != pBuf) {
                Globals::LogError("Error: SimpleCyclicBuffer::UnlockAfterWriten() wrong buffer to unlock.");
                return;
            }
            // nothing to write. Make chunke free.
            if(writtenBytes == 0) {
                m_freeUnits.Push(m_lockedChunk);
                m_lockedChunk = nullptr;
                return;
            }
            if(writtenBytes > 0 && writtenBytes != UnitSize()) {
                Globals::LogInfo("Warning: SimpleCyclicBuffer::UnlockAfterWriten() buffer not full.");
                ssize_t freeSpace = UnitSize() - writtenBytes;
                if(freeSpace > 0){
                    memset(m_lockedChunk->buf + writtenBytes, 0, freeSpace);
                } else {
                    Globals::LogInfo("Warning: SimpleCyclicBuffer::UnlockAfterWriten() written more bytes than buffer size.");
                }
            }
            m_fullUnits.Push(m_lockedChunk);
            m_lockedChunk = nullptr;
//            Globals::LogDebug("SimpleCyclicBuffer::UnlockAfterWriten(): written %d bytes", writtenBytes >= 0 ? writtenBytes : Unit::size);

        }

        ~SimpleCyclicBuffer(){}
    };
}
#endif /* __simple_double_buffer_hpp__ */
