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

#if (defined(_WIN32) || defined(__WIN32__))
#include <windows.h>
#include "p8-platform/windows/os-types.h"
#else
#include "p8-platform/posix/os-types.h"
#endif

#include "timeshift_buffer.h"
#include "helpers.h"
#include <sstream>

#include "libXBMC_addon.h"

using namespace std;
using namespace ADDON;
using namespace P8PLATFORM;

#ifdef DeleteFile
#undef DeleteFile
#endif

static const int STREAM_READ_BUFFER_SIZE = 1024 * 32; // 32K input read buffer
static const  unsigned int CHUNK_FILE_SIZE_LIMIT = (unsigned int)(STREAM_READ_BUFFER_SIZE * 1024) * 32; // 1GB chunk (temporary, pending completion of chunck factory)

TimeshiftBuffer::TimeshiftBuffer(CHelper_libXBMC_addon *addonHelper, const string &streamUrl, const string &bufferCacheDir) :
    m_addonHelper(addonHelper),
    m_bufferDir(bufferCacheDir),
m_streamHandle(NULL),
    m_length(0),
    m_position(0)
{
    if(!m_addonHelper->DirectoryExists(m_bufferDir.c_str()))
       if(!m_addonHelper->CreateDirectory(m_bufferDir.c_str()))
          throw InputBufferException("Failed to create cahche directory for timeshift buffer.");
    Init(streamUrl);
}

void TimeshiftBuffer::Init(const string &streamUrl) {
    StopThread();
    
    if(m_streamHandle) {
        m_addonHelper->CloseFile(m_streamHandle);
        m_streamHandle = NULL;
    }
    m_length = 0;
    m_position = 0;
    m_ReadChunks.clear();
    m_ChunkFileSwarm.clear();
    m_streamHandle = m_addonHelper->OpenFile(streamUrl.c_str(), XFILE::READ_AUDIO_VIDEO | XFILE::READ_AFTER_WRITE);
    if (!m_streamHandle)
        throw InputBufferException("Failed to open source stream.");
    CreateThread();
}
void TimeshiftBuffer::DebugLog(const std::string& message ) const
{
    char* msg = m_addonHelper->UnknownToUTF8(message.c_str());
    m_addonHelper->Log(LOG_DEBUG, msg);
    m_addonHelper->FreeString(msg);
}

TimeshiftBuffer::~TimeshiftBuffer()
{
    StopThread();

    m_addonHelper->CloseFile(m_streamHandle);
    m_ReadChunks.clear();
}

void *TimeshiftBuffer::Process()
{
    unsigned char buffer[STREAM_READ_BUFFER_SIZE];
    ssize_t chunkSize = 0;
    ChunkFilePtr chunk = NULL;
    bool isEof = false;
    try {
        while (!isEof && m_streamHandle != NULL && !IsStopped()) {
            // Create new chunk if nesessary
            if(NULL == chunk)
            {
                chunk  = CreateChunk();
                {
                    CLockObject lock(m_SyncAccess);
                    m_ReadChunks.push_back(chunk);
                }
                chunkSize = 0;
                DebugLog(std::string(">>> New current chunk (for write): ") + chunk->Path());
            }
            // Fill read buffer
            ssize_t bytesRead = 0;
            do{
                bytesRead += m_addonHelper->ReadFile(m_streamHandle, buffer + bytesRead, sizeof(buffer) - bytesRead);
                isEof = bytesRead <= 0;
            }while (!isEof && bytesRead < sizeof(buffer));
            // Write to local chunk
            DebugLog(std::string(">>> Write: ") + n_to_string(bytesRead));
            ssize_t bytesWritten = chunk->m_writer.Write(buffer, bytesRead);
            {
                CLockObject lock(m_SyncAccess);
                m_length += bytesWritten;
            }
            isEof |= bytesWritten != bytesRead;
            chunkSize += bytesWritten;
            if(chunkSize >= CHUNK_FILE_SIZE_LIMIT)
                chunk = NULL;
            m_writeEvent.Broadcast();
        }

    } catch (InputBufferException& ex ) {
        m_addonHelper->Log(LOG_ERROR, "Failed to create timeshift chunkfile in directory %s", m_bufferDir.c_str());
    }

    return NULL;
}

ssize_t TimeshiftBuffer::Read(unsigned char *buffer, size_t bufferSize)
{

    size_t totalBytesRead = 0;
    int32_t timeout = 5000; //5 sec

    while (totalBytesRead < bufferSize && !IsStopped())
    {
        unsigned int idx = GetChunkIndexFor(m_position);
        ChunkFilePtr chunk = NULL;
        {
            CLockObject lock(m_SyncAccess);
            chunk = (idx >= m_ReadChunks.size()) ? NULL : m_ReadChunks[idx];
        }
        // Retry 1 time after write operation
        if(NULL == chunk && m_writeEvent.Wait(timeout))
        {
            idx = GetChunkIndexFor(m_position);
            CLockObject lock(m_SyncAccess);
            chunk = (idx >= m_ReadChunks.size()) ? NULL : m_ReadChunks[idx];
        }
        if(NULL == chunk)
        {
            StopThread();
            DebugLog("TimeshiftBuffer: failed to obtain chunk for read.");
            break;
        }

        ssize_t bytesRead = 0;
        do
        {
            size_t bytesToRead = bufferSize - totalBytesRead;
            bytesRead = chunk->m_reader.Read( buffer + totalBytesRead, bytesToRead);
            if(bytesRead == 0 && !m_writeEvent.Wait(timeout)) //timeout
            {
                StopThread();
                break;
            }
            DebugLog(std::string(">>> Read: ") + n_to_string(bytesRead));
            totalBytesRead += bytesRead;
            m_position += bytesRead;
            if(chunk->m_reader.Length() >= CHUNK_FILE_SIZE_LIMIT && chunk->m_reader.Position() == chunk->m_reader.Length())
                chunk = NULL;
        }while(bytesRead == 0 && NULL != chunk && !IsStopped());
    }

    return IsStopped() ? -1 :totalBytesRead;
}

TimeshiftBuffer::ChunkFilePtr TimeshiftBuffer::CreateChunk()
{
    ChunkFilePtr newChunk = new CAddonFile(m_addonHelper, UniqueFilename(m_bufferDir, m_addonHelper).c_str());
    m_ChunkFileSwarm.push_back(ChunkFileSwarm::value_type(newChunk));
    return newChunk;
}

unsigned int TimeshiftBuffer::GetChunkIndexFor(int64_t pos) {
    return pos / CHUNK_FILE_SIZE_LIMIT;
}
int64_t TimeshiftBuffer::GetPositionInChunkFor(int64_t pos) {
    return pos % CHUNK_FILE_SIZE_LIMIT;
}

int64_t TimeshiftBuffer::GetLength() const
{
    int64_t length = -1;
    {
        CLockObject lock(m_SyncAccess);
        length = m_length;
    }
    DebugLog(std::string(">>> GetLength(): ") + n_to_string(length));
    return length;
}

int64_t TimeshiftBuffer::GetPosition() const
{
    int64_t pos = m_position;
    DebugLog(std::string(">>> GetPosition(): ") + n_to_string(pos));
    return pos;
}


int64_t TimeshiftBuffer::Seek(int64_t iPosition, int iWhence)
{
    unsigned int idx = -1;
    ChunkFilePtr chunk = NULL;
    {
        CLockObject lock(m_SyncAccess);
        if(iWhence == SEEK_CUR)
            iPosition = m_position + iPosition;
        else if(iWhence == SEEK_END)
            iPosition = m_length + iPosition;

        if(iPosition > m_length)
            iPosition = m_length;
        
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

bool TimeshiftBuffer::SwitchStream(const string &newUrl)
{
    bool succeeded = false;
    try {
        Init(newUrl);
        succeeded = true;
    } catch (const InputBufferException& ex) {
        m_addonHelper->Log(LOG_ERROR, "Failed to switch streams. Error: %s", ex.what());
    }

    return succeeded;
}

std::string TimeshiftBuffer::UniqueFilename(const std::string& dir, ADDON::CHelper_libXBMC_addon*  helper)
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

#pragma mark - CGenericFile

TimeshiftBuffer::CGenericFile::CGenericFile(ADDON::CHelper_libXBMC_addon *addonHelper, void* handler)
    : m_handler(handler)
    , m_helper(addonHelper)
{
    if(NULL == m_handler)
        throw InputBufferException("Failed to open timeshift buffer chunk file.");
}
int64_t TimeshiftBuffer::CGenericFile::Seek(int64_t iFilePosition, int iWhence)
{
    auto s = m_helper->SeekFile(m_handler, iFilePosition, iWhence);
    std::stringstream ss;
    ss <<">>> SEEK to" << iFilePosition << ". Res=" << s ;
    m_helper->Log(LOG_DEBUG, ss.str().c_str());
    return s;
}

int64_t TimeshiftBuffer::CGenericFile::Length()
{
    auto l = m_helper->GetFileLength(m_handler);
    std::stringstream ss;
    ss <<">>> LENGHT=" << l;
    m_helper->Log(LOG_DEBUG, ss.str().c_str());
    return l;
}
int64_t TimeshiftBuffer::CGenericFile::Position()
{
    auto p = m_helper->GetFilePosition(m_handler);
    std::stringstream ss;
    ss <<">>> POSITION=" << p;
    m_helper->Log(LOG_DEBUG, ss.str().c_str());

    return p;
}

//int TimeshiftBuffer::CGenericFile::Truncate(int64_t iSize)
//{
//    return m_helper->TruncateFile(m_handler, iSize);
//}

bool TimeshiftBuffer::CGenericFile::IsOpened() const
{
    return m_handler != NULL;
}

void TimeshiftBuffer::CGenericFile::Close()
{
    m_helper->CloseFile(m_handler);
    m_handler = NULL;
}
TimeshiftBuffer::CGenericFile::~CGenericFile()
{
    if(m_handler) Close();
}



#pragma mark - CFileForWrite


TimeshiftBuffer::CFileForWrite::CFileForWrite(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &pathToFile)
    : CGenericFile(addonHelper, addonHelper->OpenFileForWrite(pathToFile.c_str(), true))
{
}
ssize_t TimeshiftBuffer::CFileForWrite::Write(const void* lpBuf, size_t uiBufSize)
{
    return m_helper->WriteFile(m_handler, lpBuf, uiBufSize);

}

#pragma mark - CFileForRead


TimeshiftBuffer::CFileForRead::CFileForRead(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &pathToFile)
: CGenericFile(addonHelper, addonHelper->OpenFile(pathToFile.c_str(), 0))
{
}

ssize_t TimeshiftBuffer::CFileForRead::Read(void* lpBuf, size_t uiBufSize)
{
    return m_helper->ReadFile(m_handler, lpBuf, uiBufSize);
}

#pragma mark - CAddonFile

TimeshiftBuffer::CAddonFile::CAddonFile(ADDON::CHelper_libXBMC_addon *addonHelper, const std::string &pathToFile)
    : m_path(pathToFile)
    , m_helper(addonHelper)
    , m_writer(addonHelper, pathToFile)
    , m_reader(addonHelper, pathToFile)
{
}
const std::string& TimeshiftBuffer::CAddonFile::Path() const
{
    return m_path;
}

void TimeshiftBuffer::CAddonFile::Reopen()
{
    m_writer.~CFileForWrite();
    new (&m_writer) CFileForWrite(m_helper, m_path);
    m_reader.~CFileForRead();
    new (&m_reader) CFileForRead(m_helper, m_path);
}
          
          
TimeshiftBuffer::CAddonFile::~CAddonFile()
{
    m_reader.Close();
    m_writer.Close();
    m_helper->DeleteFile(m_path.c_str());
}


