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

#ifndef timeshift_buffer_h
#define timeshift_buffer_h


#include <string>
#include <memory>
#include <vector>
#include "p8-platform/threads/threads.h"
#include "p8-platform/util/buffer.h"
#include "input_buffer.h"

namespace ADDON
{
    class CHelper_libXBMC_addon;
}

class TimeshiftBuffer : public InputBuffer, public P8PLATFORM::CThread
{
public:
    TimeshiftBuffer(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &streamUrl, const std::string &bufferCacheDir);
    ~TimeshiftBuffer();

    int64_t GetLength() const;
    int64_t GetPosition() const;
    ssize_t Read(unsigned char *buffer, size_t bufferSize);
    int64_t Seek(long long iPosition, int iWhence) const;
    bool SwitchStream(const std::string &newUrl);

private:
    void *Process();
    
    class CAddonFile;
    class CGenericFile
    {
    public:
        int64_t Seek(int64_t iFilePosition, int iWhence);
        int64_t Length();
        int64_t Position();
       // int Truncate(int64_t iSize);
        bool IsOpened() const;
        ~CGenericFile();
    protected:
        CGenericFile(ADDON::CHelper_libXBMC_addon *addonHelper, void* m_handler);
        void* m_handler;
        ADDON::CHelper_libXBMC_addon * m_helper;
        void Close();
    private:
        CGenericFile(const CGenericFile&) = delete ;                    //disable copy-constructor
        CGenericFile& operator=(const CGenericFile&) = delete;  //disable copy-assignment
        friend class CAddonFile;
    };
    
    class CFileForWrite : public CGenericFile
    {
    public:
        CFileForWrite(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &pathToFile);
        ssize_t Write(const void* lpBuf, size_t uiBufSize);
    };

    class CFileForRead : public CGenericFile
    {
    public:
        CFileForRead(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &pathToFile);
        ssize_t Read(void* lpBuf, size_t uiBufSize);
    };
    class CAddonFile
    {
    public:
        CFileForWrite m_writer;
        CFileForRead m_reader;

        CAddonFile(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &pathToFile);
        
        const std::string& Path() const;
        void Reopen();
        ~CAddonFile();
        
        
    private:
        CAddonFile(const CAddonFile&) = delete ;                    //disable copy-constructor
        CAddonFile& operator=(const CAddonFile&) = delete;  //disable copy-assignment
        std::string m_path;
        ADDON::CHelper_libXBMC_addon * m_helper;

    };
    
    P8PLATFORM::CEvent m_writeEvent;
    typedef CAddonFile* ChunkFilePtr;

    ChunkFilePtr GetFreeChunk();
    std::string UniqueFilename(const std::string& dir);

    typedef std::vector<ChunkFilePtr> ReadCache;
    ADDON::CHelper_libXBMC_addon *m_addonHelper;
    std::string m_bufferDir;
    void *m_streamHandle;
    //void *m_bufferReadHandle;
    //ReadCache m_ReadCache;
    mutable ChunkFilePtr m_CurrentReadChunk;
    ChunkFilePtr GetCurrentReadChunk(int32_t timeout) const;
    void FreeCurrentChunk() const;
    //P8PLATFORM::CMutex m_mutex;
    
    typedef P8PLATFORM::SyncedBuffer <ChunkFilePtr> ChunkFileBuffer;
    mutable ChunkFileBuffer m_FreeChunks;
    mutable ChunkFileBuffer m_PopulatedChunks;
    
    typedef std::vector<std::unique_ptr<CAddonFile> > ChunkFileSwarm;
    ChunkFileSwarm m_ChunkFileSwarm;
    
    void DebugLog(const std::string& message) const;
    
    long long m_length;
    long long m_position;
};

#endif //timeshift_buffer_h
