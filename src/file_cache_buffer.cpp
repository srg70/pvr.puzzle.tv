//
//  file_cache_buffer.cpp
//  pvr.puzzle.tv
//
//  Created by Sergey Shramchenko on 04/08/2017.
//  Copyright © 2017 Home. All rights reserved.
//

#include "file_cache_buffer.hpp"
#include "libXBMC_addon.h"
#include "helpers.h"

static const int STREAM_READ_BUFFER_SIZE = 1024 * 32; // 32K input read buffer
static const  unsigned int CHUNK_FILE_SIZE_LIMIT = (unsigned int)(STREAM_READ_BUFFER_SIZE * 1024) * 32; // 1GB chunk (temporary, pending completion of chunck factory)


namespace Buffers
{
    using namespace P8PLATFORM;
    using namespace ADDON;
    
    std::string UniqueFilename(const std::string& dir, ADDON::CHelper_libXBMC_addon*  helper);
    
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
    
    
    
#pragma mark - CGenericFile
    ///////////////////////////////////////////
    //              CGenericFile
    //////////////////////////////////////////
    
    CGenericFile::CGenericFile(ADDON::CHelper_libXBMC_addon *addonHelper, void* handler)
    : m_handler(handler)
    , m_helper(addonHelper)
    {
        if(NULL == m_handler)
        throw CacheBufferException("Failed to open timeshift buffer chunk file.");
    }
    int64_t CGenericFile::Seek(int64_t iFilePosition, int iWhence)
    {
        auto s = m_helper->SeekFile(m_handler, iFilePosition, iWhence);
        //    std::stringstream ss;
        //    ss <<">>> SEEK to" << iFilePosition << ". Res=" << s ;
        //    m_helper->Log(LOG_DEBUG, ss.str().c_str());
        return s;
    }
    
    int64_t CGenericFile::Length()
    {
        auto l = m_helper->GetFileLength(m_handler);
        //    std::stringstream ss;
        //    ss <<">>> LENGHT=" << l;
        //    m_helper->Log(LOG_DEBUG, ss.str().c_str());
        return l;
    }
    int64_t CGenericFile::Position()
    {
        auto p = m_helper->GetFilePosition(m_handler);
        //    std::stringstream ss;
        //    ss <<">>> POSITION=" << p;
        //    m_helper->Log(LOG_DEBUG, ss.str().c_str());
        
        return p;
    }
    
    //int CGenericFile::Truncate(int64_t iSize)
    //{
    //    return m_helper->TruncateFile(m_handler, iSize);
    //}
    
    bool CGenericFile::IsOpened() const
    {
        return m_handler != NULL;
    }
    
    void CGenericFile::Close()
    {
        m_helper->CloseFile(m_handler);
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
    
    
    CFileForWrite::CFileForWrite(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &pathToFile)
    : CGenericFile(addonHelper, addonHelper->OpenFileForWrite(pathToFile.c_str(), true))
    {
    }
    ssize_t CFileForWrite::Write(const void* lpBuf, size_t uiBufSize)
    {
        return m_helper->WriteFile(m_handler, lpBuf, uiBufSize);
        
    }
    
#pragma mark - CFileForRead
    
    ///////////////////////////////////////////
    //              CFileForRead
    //////////////////////////////////////////
    
    
    CFileForRead::CFileForRead(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &pathToFile)
    : CGenericFile(addonHelper, addonHelper->OpenFile(pathToFile.c_str(), XFILE::READ_AUDIO_VIDEO | XFILE::READ_AFTER_WRITE))
    {
    }
    
    ssize_t CFileForRead::Read(void* lpBuf, size_t uiBufSize)
    {
        return m_helper->ReadFile(m_handler, lpBuf, uiBufSize);
    }
    
#pragma mark - CAddonFile
    ///////////////////////////////////////////
    //              CAddonFile
    //////////////////////////////////////////
    
    
    CAddonFile::CAddonFile(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &pathToFile)
    : m_path(pathToFile)
    , m_helper(addonHelper)
    , m_writer(addonHelper, pathToFile)
    , m_reader(addonHelper, pathToFile)
    {
    }
    const std::string& CAddonFile::Path() const
    {
        return m_path;
    }
    
    void CAddonFile::Reopen()
    {
        m_writer.~CFileForWrite();
        new (&m_writer) CFileForWrite(m_helper, m_path);
        m_reader.~CFileForRead();
        new (&m_reader) CFileForRead(m_helper, m_path);
    }
    
    
    CAddonFile::~CAddonFile()
    {
        m_reader.Close();
        m_writer.Close();
        m_helper->DeleteFile(m_path.c_str());
    }
    
    
    
    
    ///////////////////////////////////////////
    //              FileCacheBuffer
    //////////////////////////////////////////
    
    
    
    FileCacheBuffer::FileCacheBuffer(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string& bufferCacheDir)
    : m_addonHelper(addonHelper)
    , m_bufferDir(bufferCacheDir)
    , m_length(0)
    , m_position(0)
    {
        if(!m_addonHelper->DirectoryExists(m_bufferDir.c_str()))
        if(!m_addonHelper->CreateDirectory(m_bufferDir.c_str()))
        throw CacheBufferException("Failed to create cahche directory for timeshift buffer.");
        Init();
    }
    void FileCacheBuffer::Init() {
        m_length = 0;
        m_position = 0;
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
            iWhence = SEEK_SET;
            
            idx = GetChunkIndexFor(iPosition);
            if(idx >= m_ReadChunks.size()) {
                m_addonHelper->Log(LOG_ERROR, "TimeshiftBuffer: seek failed. Wrong chunk index %d", idx);
                return -1;
            }
            chunk = m_ReadChunks[idx];
        }
        auto inPos = GetPositionInChunkFor(iPosition);
        auto pos =  chunk->m_reader.Seek(inPos, iWhence);
        m_position = iPosition -  (inPos - pos);
        return iPosition;
        
    }
    
    // Virtual steream lenght.
    int64_t FileCacheBuffer::Length() {
        int64_t length = -1;
        {
            CLockObject lock(m_SyncAccess);
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
        int32_t timeout = 5000;//c_commonTimeoutMs + 1000;
        
        while (totalBytesRead < bufferSize) {
            unsigned int idx = GetChunkIndexFor(m_position);
            ChunkFilePtr chunk = NULL;
            {
                CLockObject lock(m_SyncAccess);
                chunk = (idx >= m_ReadChunks.size()) ? NULL : m_ReadChunks[idx];
            }
            
            if(NULL == chunk)  {
                m_addonHelper->Log(LOG_ERROR, "FileCacheBuffer: failed to obtain chunk for read.");
                break;
            }
            
            size_t bytesToRead = bufferSize - totalBytesRead;
            ssize_t bytesRead = chunk->m_reader.Read( ((char*)buffer) + totalBytesRead, bytesToRead);
            if(bytesRead == 0 ) {
                m_addonHelper->Log(LOG_NOTICE, "FileCacheBuffer: nothing to read.");
                break;
            }
            //DebugLog(std::string(">>> Read: ") + n_to_string(bytesRead));
            totalBytesRead += bytesRead;
            m_position += bytesRead;
            if(chunk->m_reader.Length() >= CHUNK_FILE_SIZE_LIMIT && chunk->m_reader.Position() == chunk->m_reader.Length()) {
                chunk = NULL;
            }
        }
        
        return totalBytesRead;
        
    }
    
    // Write interface
    ssize_t FileCacheBuffer::Write(const void* buf, size_t bufferSize) {
        
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
                    CLockObject lock(m_SyncAccess);
                    m_length += bytesWritten;
                }
                totalWritten += bytesWritten;
                if(bytesWritten != bytesToWrite) {
                    m_addonHelper->Log(LOG_ERROR, "FileCachetBuffer: write cache error, written (%d) != read (%d)", bytesWritten,bytesToWrite);
                    break;// ???
                }
                available -= bytesWritten;
                buffer += bytesWritten;
                bufferSize -= bytesWritten;
                if(available <= 0) {
                    chunk = NULL;
                }
            }
            
        } catch (std::exception&  ) {
            m_addonHelper->Log(LOG_ERROR, "Failed to create timeshift chunkfile in directory %s", m_bufferDir.c_str());
        }
        
        return totalWritten;
        
    }
    
    FileCacheBuffer::ChunkFilePtr FileCacheBuffer::CreateChunk()
    {
        ChunkFilePtr newChunk = new CAddonFile(m_addonHelper, UniqueFilename(m_bufferDir, m_addonHelper).c_str());
        m_ChunkFileSwarm.push_back(ChunkFileSwarm::value_type(newChunk));
        m_addonHelper->Log(LOG_DEBUG, ">>> TimeshiftBuffer: new current chunk (for write):  %s", + newChunk->Path().c_str());
        return newChunk;
    }
    
    unsigned int FileCacheBuffer::GetChunkIndexFor(int64_t pos) {
        return pos / CHUNK_FILE_SIZE_LIMIT;
    }
    int64_t FileCacheBuffer::GetPositionInChunkFor(int64_t pos) {
        return pos % CHUNK_FILE_SIZE_LIMIT;
    }
    
    
    FileCacheBuffer::~FileCacheBuffer(){
        m_ReadChunks.clear();
        
    }
    std::string UniqueFilename(const std::string& dir, ADDON::CHelper_libXBMC_addon*  helper)
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
        }while(helper->FileExists(candidate.c_str(), false));
        return candidate;
    }
    
} // namespace
