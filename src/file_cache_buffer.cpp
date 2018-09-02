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
#include <windows.h>
#ifdef GetObject
#undef GetObject
#endif
#endif

#include <algorithm>
#include "kodi/kodi_vfs_utils.hpp"
#include "file_cache_buffer.hpp"
#include "libXBMC_addon.h"
#include "helpers.h"
#include "globals.hpp"
#include "neutral_sorting.h"

namespace Buffers
{
    using namespace P8PLATFORM;
    using namespace ADDON;
    using namespace Globals;
    
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
        CGenericFile(void* m_handler);
        void* m_handler;
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
    
    CGenericFile::CGenericFile(void* handler)
    : m_handler(handler)
    {
        if(NULL == m_handler)
        throw CacheBufferException("Failed to open timeshift buffer chunk file.");
    }
    int64_t CGenericFile::Seek(int64_t iFilePosition, int iWhence)
    {
        auto s = XBMC->SeekFile(m_handler, iFilePosition, iWhence);
        //    std::stringstream ss;
        //    ss <<">>> SEEK to" << iFilePosition << ". Res=" << s ;
        //    m_helper->Log(LOG_DEBUG, ss.str().c_str());
        return s;
    }
    
    int64_t CGenericFile::Length()
    {
        auto l = XBMC->GetFileLength(m_handler);
        //    std::stringstream ss;
        //    ss <<">>> LENGHT=" << l;
        //    XBMC->Log(LOG_DEBUG, ss.str().c_str());
        return l;
    }
    int64_t CGenericFile::Position()
    {
        auto p = XBMC->GetFilePosition(m_handler);
        //    std::stringstream ss;
        //    ss <<">>> POSITION=" << p;
        //    XBMC->Log(LOG_DEBUG, ss.str().c_str());
        
        return p;
    }
    
    //int CGenericFile::Truncate(int64_t iSize)
    //{
    //    return XBMC->TruncateFile(m_handler, iSize);
    //}
    
    bool CGenericFile::IsOpened() const
    {
        return m_handler != NULL;
    }
    
    void CGenericFile::Close()
    {
        XBMC->CloseFile(m_handler);
        m_handler = NULL;
    }
    CGenericFile::~CGenericFile()
    {
        if(m_handler) Close();
    }
    
    
    
#pragma mark - CFileForWrite
    
    ///////////////////////////////////////////
    //              CFileForWrite
    //////////////////////////////////////////
    
    
    CFileForWrite::CFileForWrite(const std::string &pathToFile)
    : CGenericFile(XBMC->OpenFileForWrite(pathToFile.c_str(), false))
    {
    }
    ssize_t CFileForWrite::Write(const void* lpBuf, size_t uiBufSize)
    {
        return XBMC->WriteFile(m_handler, lpBuf, uiBufSize);
        
    }
    
#pragma mark - CFileForRead
    
    ///////////////////////////////////////////
    //              CFileForRead
    //////////////////////////////////////////
    
    
    CFileForRead::CFileForRead(const std::string &pathToFile)
    : CGenericFile(XBMC->OpenFile(pathToFile.c_str(), XFILE::READ_AUDIO_VIDEO | XFILE::READ_AFTER_WRITE))
    {
    }
    
    ssize_t CFileForRead::Read(void* lpBuf, size_t uiBufSize)
    {
        return XBMC->ReadFile(m_handler, lpBuf, uiBufSize);
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
            XBMC->DeleteFile(m_path.c_str());
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
    , m_chunkForLock(new uint8_t[STREAM_READ_BUFFER_SIZE])
    {
        if(!XBMC->DirectoryExists(m_bufferDir.c_str())) {
            if(!XBMC->CreateDirectory(m_bufferDir.c_str())) {
                throw CacheBufferException("Failed to create cahche  directory for timeshift buffer.");
            }
        }
        //Init();
    }

    static int64_t CalculateDataSize(const std::string& bufferCacheDir)
    {
        int64_t result = 0;
        std::vector<CVFSDirEntry> files;
        VFSUtils::GetDirectory(XBMC, bufferCacheDir, "*.bin", files);
        for (auto& f : files) {
            if(!f.IsFolder())
                result += f.Size();
        }
        return result;
    }
    
    FileCacheBuffer::FileCacheBuffer(const std::string& bufferCacheDir)
    : m_bufferDir(bufferCacheDir)
    , m_maxSize(CalculateDataSize(bufferCacheDir))
    , m_autoDelete(false)
    , m_isReadOnly(true)
    {
        if(!XBMC->DirectoryExists(m_bufferDir.c_str())) {
            throw CacheBufferException("Directory for timeshift buffer (read mode) does not exist.");
        }
        Init();
        // Load *.bin files
        std::vector<CVFSDirEntry> files;
        VFSUtils::GetDirectory(XBMC, m_bufferDir, "*.bin", files);
        // run "neutral sorting" on files list
        struct cvf_alphanum_less : public std::binary_function<CVFSDirEntry, CVFSDirEntry, bool>
        {
            bool operator()(const CVFSDirEntry& left, const CVFSDirEntry& right) const
            {
                return doj::alphanum_comp(left.Path(), right.Path()) < 0;
            }
        } neutral_sorter;
        std::sort(files.begin(), files.end(), neutral_sorter);
        for (auto& f : files) {
            if(!f.IsFolder()) {
                ChunkFilePtr newChunk = new CAddonFile(f.Path().c_str(), m_autoDelete);
                m_length += f.Size();
                m_ChunkFileSwarm.push_back(ChunkFileSwarm::value_type(newChunk));
                m_ReadChunks.push_back(newChunk);
            }
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
//            XBMC->Log(LOG_DEBUG, "TimeshiftBuffer::Sseek. >>> Requested pos %d", iPosition);

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
//            XBMC->Log(LOG_DEBUG, "TimeshiftBuffer::Sseek. Calculated pos %d", iPosition);
//            XBMC->Log(LOG_DEBUG, "TimeshiftBuffer::Sseek. Begin %d Length %d", m_begin, m_length);

            idx = GetChunkIndexFor(iPosition);
            if(idx >= m_ReadChunks.size()) {
                XBMC->Log(LOG_ERROR, "TimeshiftBuffer: seek failed. Wrong chunk index %d", idx);
                return -1;
            }
            chunk = m_ReadChunks[idx];
        }
        auto inPos = GetPositionInChunkFor(iPosition);
        auto pos =  chunk->m_reader.Seek(inPos, iWhence);
        m_position = iPosition -  (inPos - pos);
//        XBMC->Log(LOG_DEBUG, "TimeshiftBuffer::Sseek. Chunk idx %d, pos in chunk %d, actual pos %d", idx, inPos, pos);
//        XBMC->Log(LOG_DEBUG, "TimeshiftBuffer::Sseek. <<< Result pos %d", m_position);
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
                XBMC->Log(LOG_ERROR, "FileCacheBuffer: failed to obtain chunk for read.");
                break;
            }
            
            size_t bytesToRead = bufferSize - totalBytesRead;
            ssize_t bytesRead = chunk->m_reader.Read( ((char*)buffer) + totalBytesRead, bytesToRead);
            if(bytesRead == 0 ) {
                //XBMC->Log(LOG_NOTICE, "FileCacheBuffer: nothing to read.");
                break;
            }
            //DebugLog(std::string(">>> Read: ") + n_to_string(bytesRead));
            totalBytesRead += bytesRead;
            m_position += bytesRead;
            if(chunk->m_reader.Length() >= CHUNK_FILE_SIZE_LIMIT && chunk->m_reader.Position() == chunk->m_reader.Length()) {
                chunk = nullptr;
            }
        }
        if(nullptr == chunk && !m_isReadOnly) {
            CLockObject lock(m_SyncAccess);
            while(m_length - m_begin >=  m_maxSize)
            {
                m_begin  +=CHUNK_FILE_SIZE_LIMIT;
                m_ReadChunks.pop_front();
                m_ChunkFileSwarm.pop_front();
            }
        }
        return totalBytesRead;
        
    }
    
    // Write interface
    bool FileCacheBuffer::LockUnitForWrite(uint8_t** pBuf) {
        *pBuf = m_chunkForLock.get();
        return true;
    }
    void FileCacheBuffer::UnlockAfterWriten(uint8_t* pBuf, ssize_t writtenBytes) {
        if(m_chunkForLock.get() != pBuf) {
            LogError("Error: FileCacheBuffer::UnlockUnit() wrong buffer to unlock.");
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
                    XBMC->Log(LOG_ERROR, "FileCachetBuffer: write cache error, written (%d) != read (%d)", bytesWritten,bytesToWrite);
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
            XBMC->Log(LOG_ERROR, "Failed to create timeshift chunkfile in directory %s", m_bufferDir.c_str());
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
        XBMC->Log(LOG_DEBUG, ">>> TimeshiftBuffer: new current chunk (for write):  %s", + newChunk->Path().c_str());
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
        }while(XBMC->FileExists(candidate.c_str(), false));
        return candidate;
    }
    
} // namespace
