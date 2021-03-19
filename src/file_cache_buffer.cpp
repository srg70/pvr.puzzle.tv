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
#if (defined(_WIN32) || defined(__WIN32__))
#include <WinSock2.h>
#include <windows.h>
#ifdef GetObject
#undef GetObject
#endif
#endif

#include <algorithm>
#include "kodi/Filesystem.h"
#include "globals.hpp"

#include "file_cache_buffer.hpp"
#include "helpers.h"
#include "globals.hpp"
#include "neutral_sorting.h"

namespace Buffers
{
    using namespace P8PLATFORM;
    using namespace Globals;
    using namespace Helpers;

    std::string UniqueFilename(const std::string& dir);
    
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
        CGenericFile();
        kodi::vfs::CFile m_handler;
        void Close();
    private:
        CGenericFile(const CGenericFile&) = delete ;                    //disable copy-constructor
        CGenericFile& operator=(const CGenericFile&) = delete;  //disable copy-assignment
        friend class CAddonFile;
    };
    
    class CFileForWrite : public CGenericFile
    {
    public:
        CFileForWrite(const std::string &pathToFile);
        ssize_t Write(const void* lpBuf, size_t uiBufSize);
    };
    
    class CFileForRead : public CGenericFile
    {
    public:
        CFileForRead(const std::string &pathToFile);
        ssize_t Read(void* lpBuf, size_t uiBufSize);
    };
    class CAddonFile
    {
    public:
        CFileForWrite m_writer;
        CFileForRead m_reader;
        
        CAddonFile(const std::string &pathToFile, bool autoDelete);
        
        const std::string& Path() const;
        void Reopen();
        ~CAddonFile();
        
        
    private:
        CAddonFile(const CAddonFile&) = delete ;                    //disable copy-constructor
        CAddonFile& operator=(const CAddonFile&) = delete;  //disable copy-assignment
        std::string m_path;
        const bool m_autoDelete;

    };
    
    
    
#pragma mark - CGenericFile
    ///////////////////////////////////////////
    //              CGenericFile
    //////////////////////////////////////////
    
    CGenericFile::CGenericFile()
    {
    }
    int64_t CGenericFile::Seek(int64_t iFilePosition, int iWhence)
    {
        auto s = m_handler.Seek(iFilePosition, iWhence);
//        LogDebug("SEEK to %lld. Result %lld" , iFilePosition, s);
        return s;
    }
    
    int64_t CGenericFile::Length()
    {
        auto l = m_handler.GetLength();
        //    LogDebug(">>> LENGHT= %lld", l);
        return l;
    }
    int64_t CGenericFile::Position()
    {
        auto p = m_handler.GetPosition();
          //    LogDebug(">>> POSITION= %lld", p);
        
        return p;
    }
    
    //int CGenericFile::Truncate(int64_t iSize)
    //{
    //    return XBMC->TruncateFile(m_handler, iSize);
    //}
    
    bool CGenericFile::IsOpened() const
    {
        return m_handler.IsOpen();
    }
    
    void CGenericFile::Close()
    {
        m_handler.Close();
    }
    CGenericFile::~CGenericFile()
    {
        if(m_handler.IsOpen())
            Close();
    }
    
    
    
#pragma mark - CFileForWrite
    
    ///////////////////////////////////////////
    //              CFileForWrite
    //////////////////////////////////////////
    
    
    CFileForWrite::CFileForWrite(const std::string &pathToFile)
    : CGenericFile()
    {
        m_handler.OpenFileForWrite(pathToFile, false);
        if(!m_handler.IsOpen())
            throw CacheBufferException("Failed to open timeshift buffer chunk file.");

    }
    ssize_t CFileForWrite::Write(const void* lpBuf, size_t uiBufSize)
    {
        return m_handler.Write(lpBuf, uiBufSize);
        
    }
    
#pragma mark - CFileForRead
    
    ///////////////////////////////////////////
    //              CFileForRead
    //////////////////////////////////////////
    
    
    CFileForRead::CFileForRead(const std::string &pathToFile)
    {
        m_handler.OpenFile(pathToFile, ADDON_READ_AUDIO_VIDEO | ADDON_READ_AFTER_WRITE);
        if(!m_handler.IsOpen())
            throw CacheBufferException("Failed to open timeshift buffer chunk file.");

    }
    
    ssize_t CFileForRead::Read(void* lpBuf, size_t uiBufSize)
    {
        return m_handler.Read(lpBuf, uiBufSize);
    }
    
#pragma mark - CAddonFile
    ///////////////////////////////////////////
    //              CAddonFile
    //////////////////////////////////////////
    
    
    CAddonFile::CAddonFile(const std::string &pathToFile, bool autoDelete)
    : m_path(pathToFile)
    , m_writer(pathToFile)
    , m_reader(pathToFile)
    , m_autoDelete(autoDelete)
    {
    }
    const std::string& CAddonFile::Path() const
    {
        return m_path;
    }
    
    void CAddonFile::Reopen()
    {
        m_writer.~CFileForWrite();
        new (&m_writer) CFileForWrite(m_path);
        m_reader.~CFileForRead();
        new (&m_reader) CFileForRead(m_path);
    }
    
    
    CAddonFile::~CAddonFile()
    {
        m_reader.Close();
        m_writer.Close();
        if(m_autoDelete)
            kodi::vfs::DeleteFile(m_path.c_str());
    }
    
    
    
    
    ///////////////////////////////////////////
    //              FileCacheBuffer
    //////////////////////////////////////////
    
    
#pragma mark - FileCacheBuffer
    
    FileCacheBuffer::FileCacheBuffer(const std::string& bufferCacheDir, uint8_t  sizeFactor,  bool autoDelete)
    : m_bufferDir(bufferCacheDir)
    , m_maxSize(std::max(uint8_t(3), sizeFactor) * CHUNK_FILE_SIZE_LIMIT)
    , m_autoDelete(autoDelete)
    , m_isReadOnly(false)
    , m_startTime(0)
    , m_endTime(0)
    , m_chunkForLock(new uint8_t[STREAM_READ_BUFFER_SIZE])
    {
        if(!kodi::vfs::DirectoryExists(m_bufferDir)) {
            if(!kodi::vfs::CreateDirectory(m_bufferDir)) {
                throw CacheBufferException("Failed to create cahche  directory for timeshift buffer.");
            }
        }
        //Init();
    }

    static int64_t CalculateDataSize(const std::string& bufferCacheDir)
    {
        int64_t result = 0;
        std::vector<kodi::vfs::CDirEntry> files;

        if(kodi::vfs::GetDirectory(bufferCacheDir, "*.bin", files)) {
            for (const auto& f : files) {
                if(!f.IsFolder())
                    result += f.Size();
            }
        } else {
            LogError( "Failed obtain content of FileCacheBuffer folder %s", bufferCacheDir.c_str());
        }
        return result;
    }
    
    FileCacheBuffer::FileCacheBuffer(const std::string& bufferCacheDir)
    : m_bufferDir(bufferCacheDir)
    , m_maxSize(CalculateDataSize(bufferCacheDir))
    , m_autoDelete(false)
    , m_isReadOnly(true)
    {
        if(!kodi::vfs::DirectoryExists(m_bufferDir)) {
            throw CacheBufferException("Directory for timeshift buffer (read mode) does not exist.");
        }
        Init();
        // Load *.bin files
        std::vector<kodi::vfs::CDirEntry> binFiles;
        std::vector<kodi::vfs::CDirEntry> files;
        if(kodi::vfs::GetDirectory(bufferCacheDir, "*.bin", files)) {
            for (const auto& f : files) {
                if(!f.IsFolder())
                    binFiles.push_back(f);
            }
            // run "neutral sorting" on files list
            struct cvf_alphanum_less : public std::binary_function<const VFSDirEntry*, const VFSDirEntry*, bool>
            {
                bool operator()(const kodi::vfs::CDirEntry& left, const kodi::vfs::CDirEntry& right) const
                {
                    return doj::alphanum_comp(left.Path(), right.Path()) < 0;
                }
            } neutral_sorter;
            std::sort(binFiles.begin(), binFiles.end(), neutral_sorter);
            for (const auto& f : binFiles) {
                ChunkFilePtr newChunk = new CAddonFile(f.Path(), m_autoDelete);
                m_length += f.Size();
                m_ChunkFileSwarm.push_back(ChunkFileSwarm::value_type(newChunk));
                m_ReadChunks.push_back(newChunk);
            }
            
        } else {
            LogError( "Failed obtain content of FileCacheBuffer folder %s", bufferCacheDir.c_str());
        }


    }

    void FileCacheBuffer::Init() {
        m_length = 0;
        m_position = 0;
        m_begin = 0;
        m_ReadChunks.clear();
        m_ChunkFileSwarm.clear();
    }
    
    uint32_t FileCacheBuffer::UnitSize() {
        return STREAM_READ_BUFFER_SIZE;
    }
    
    // Seak read position within cache window
    int64_t FileCacheBuffer::Seek(int64_t iPosition, int iWhence) {
        unsigned int idx = -1;
        ChunkFilePtr chunk = NULL;
        {
//            LogDebug("TimeshiftBuffer::Seek. >>> Requested pos %d", iPosition);

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
            LogDebug("TimeshiftBuffer::Seek. Calculated pos %lld", iPosition);
            LogDebug("TimeshiftBuffer::Seek. Begin %lld Length %lld", m_begin, m_length);

            idx = GetChunkIndexFor(iPosition);
            if(idx >= m_ReadChunks.size()) {
                LogError("TimeshiftBuffer: seek failed. Wrong chunk index %d", idx);
                return -1;
            }
            chunk = m_ReadChunks[idx];
        }
        auto inPos = GetPositionInChunkFor(iPosition);
        auto pos =  chunk->m_reader.Seek(inPos, iWhence);
        m_position = iPosition -  (inPos - pos);
        LogDebug("TimeshiftBuffer::Seek: chunk idx %lld, pos in chunk %lld, actual pos %lld", idx, inPos, pos);
        LogDebug("TimeshiftBuffer::Seek: result pos %lld", m_position);
        return iPosition;
        
    }
    
    // Virtual steream lenght.
    int64_t FileCacheBuffer::Length() {
        int64_t length = -1;
        {
//            CLockObject lock(m_SyncAccess);
            length = m_length;
        }
        return length;
    }
    
    // Current read position
    int64_t  FileCacheBuffer::Position() {
        
        int64_t pos = m_position;
        //        {
        //            CLockObject lock(m_SyncAccess);
        //            pos = m_position;
        //        }
        return pos;
    }
    
    // Reads data from Position(),
    ssize_t FileCacheBuffer::Read(void* buffer, size_t bufferSize) {
        
        size_t totalBytesRead = 0;
        
        ChunkFilePtr chunk = nullptr;
        while (totalBytesRead < bufferSize) {
            unsigned int idx = GetChunkIndexFor(m_position);
            {
                chunk = nullptr;
                CLockObject lock(m_SyncAccess);
                if(idx < m_ReadChunks.size()) {
                    chunk = m_ReadChunks[idx];
                    chunk->m_reader.Seek(GetPositionInChunkFor(m_position), SEEK_SET);
                }
            }
            
            if(nullptr == chunk)  {
                LogError("FileCacheBuffer: failed to obtain chunk for read. Buffer pos=%lld, length=%lld", m_position, m_length);
                break;
            }
            
            size_t bytesToRead = bufferSize - totalBytesRead;
            ssize_t bytesRead = chunk->m_reader.Read( ((char*)buffer) + totalBytesRead, bytesToRead);
            //LogDebug("FileCacheBuffer: >>> Read: %d" , bytesRead);

            totalBytesRead += bytesRead;
            m_position += bytesRead;
            // Did we done with chunk?
            if(chunk->m_reader.Length() >= CHUNK_FILE_SIZE_LIMIT && chunk->m_reader.Position() == chunk->m_reader.Length()) {
                chunk = nullptr;
            } else if(bytesRead == 0 ) {
                // Chunk is NOT full, but has no more data.
                // Break to let the player to request another time
                // or let the user to stop playing.
                    LogDebug("FileCacheBuffer: nothing to read from chunk. Chunk pos=%lld, lenght=%lld", chunk->m_reader.Position(), chunk->m_reader.Length());
                    break;
            }
        }
        if(nullptr == chunk && !m_isReadOnly) {
            CLockObject lock(m_SyncAccess);
            while(m_length - m_begin >=  m_maxSize)
            {
                m_startTime += CHUNK_FILE_SIZE_LIMIT * (m_endTime - m_startTime)/(m_length - m_begin);
                m_begin  +=  CHUNK_FILE_SIZE_LIMIT;
                m_ReadChunks.pop_front();
                m_ChunkFileSwarm.pop_front();
            }
        }
        return totalBytesRead;
        
    }
    
    // Write interface
    bool FileCacheBuffer::LockUnitForWrite(uint8_t** pBuf) {
        if(!m_ReadChunks.size()) {
            m_startTime = time(NULL);
        }
        *pBuf = m_chunkForLock.get();
        return true;
    }
    void FileCacheBuffer::UnlockAfterWriten(uint8_t* pBuf, ssize_t writtenBytes) {
        if(m_chunkForLock.get() != pBuf) {
            LogError("FileCacheBuffer: FileCacheBuffer::UnlockUnit() wrong buffer to unlock.");
            return;
        }
        Write(pBuf, writtenBytes < 0 ? UnitSize() : writtenBytes);
    }

    ssize_t FileCacheBuffer::Write(const void* buf, size_t bufferSize) {
        if(m_isReadOnly)
            return 0;
        
        const uint8_t* buffer = (const uint8_t*)  buf;
        ssize_t totalWritten = 0;
        try {
            
            ChunkFilePtr chunk = NULL;
            while (bufferSize) {
                // Create new chunk if nesessary
                if(NULL == chunk)  {
                    CLockObject lock(m_SyncAccess);
                    
                    m_endTime = time(NULL);
                    if(m_ReadChunks.size()) {
                        chunk = m_ReadChunks.back();
                        if(chunk->m_writer.Length() >= CHUNK_FILE_SIZE_LIMIT) {
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
                
                size_t available = CHUNK_FILE_SIZE_LIMIT - chunk->m_writer.Length();
                const size_t bytesToWrite = std::min(available, bufferSize);
                // Write bytes
                const ssize_t bytesWritten = chunk->m_writer.Write(buffer, bytesToWrite);
                {
                    //CLockObject lock(m_SyncAccess);
                    m_length += bytesWritten;
                }
                totalWritten += bytesWritten;
                if(bytesWritten != bytesToWrite) {
                    LogError("FileCachetBuffer: write cache error, written (%d) != read (%d)", bytesWritten,bytesToWrite);
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
            
        } catch (std::exception&  ) {
            LogError("FileCacheBuffer: failed to create timeshift chunkfile in directory %s", m_bufferDir.c_str());
        }
        
        return totalWritten;
        
    }
    
    FileCacheBuffer::ChunkFilePtr FileCacheBuffer::CreateChunk()
    {
        // No room for new data
        if(m_length - m_begin >=  m_maxSize ) {
            return NULL;
        }
        ChunkFilePtr newChunk = new CAddonFile(UniqueFilename(m_bufferDir).c_str(), m_autoDelete);
        m_ChunkFileSwarm.push_back(ChunkFileSwarm::value_type(newChunk));
        LogDebug(">>> TimeshiftBuffer: new current chunk (for write):  %s", + newChunk->Path().c_str());
        return newChunk;
    }
    
    unsigned int FileCacheBuffer::GetChunkIndexFor(int64_t pos) {
        pos -= m_begin;
        return pos / CHUNK_FILE_SIZE_LIMIT;
    }
    int64_t FileCacheBuffer::GetPositionInChunkFor(int64_t pos) {
        pos -= m_begin;
        return pos % CHUNK_FILE_SIZE_LIMIT;
    }
    
    
    FileCacheBuffer::~FileCacheBuffer(){
        m_ReadChunks.clear();
        
    }
    std::string UniqueFilename(const std::string& dir)
    {
        int cnt = 0;
        std::string candidate;
        do
        {
            candidate = dir;
            candidate += PATH_SEPARATOR_CHAR;
            candidate +="TimeshiftBuffer-";
            candidate +=n_to_string(cnt++);
            candidate += ".bin";
        }while(kodi::vfs::FileExists(candidate, false));
        return candidate;
    }
    
} // namespace
