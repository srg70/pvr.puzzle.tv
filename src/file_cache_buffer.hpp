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


#ifndef __file_cache_buffer_hpp__
#define __file_cache_buffer_hpp__

#include <memory>
#include <string>
#include <vector>
#include "cache_buffer.h"
#include "p8-platform/threads/mutex.h"


namespace ADDON
{
    class CHelper_libXBMC_addon;
}

namespace Buffers
{
    
    class CAddonFile;
    
    class FileCacheBuffer : public ICacheBuffer
    {
    public:
        FileCacheBuffer(ADDON::CHelper_libXBMC_addon *addonHelper,  const std::string &bufferCacheDir);
        
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
        virtual ssize_t Write(const void* lpBuf, size_t uiBufSize);
        
        ~FileCacheBuffer();
        
    private:
        typedef CAddonFile* ChunkFilePtr;
        typedef std::vector<ChunkFilePtr > FileChunks;
        typedef std::vector<std::unique_ptr<CAddonFile> > ChunkFileSwarm;

        ChunkFilePtr CreateChunk();
        static unsigned int GetChunkIndexFor(int64_t position);
        static int64_t GetPositionInChunkFor(int64_t position);

        
        mutable FileChunks m_ReadChunks;
        ChunkFileSwarm m_ChunkFileSwarm;
        mutable P8PLATFORM::CMutex m_SyncAccess;
        int64_t m_length;
        int64_t m_position;
        ADDON::CHelper_libXBMC_addon *m_addonHelper;
        std::string m_bufferDir;


    };
}
#endif // __file_cache_buffer_hpp__
