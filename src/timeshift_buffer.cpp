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

#include "p8-platform/posix/os-types.h"
#include "libXBMC_addon.h"
#include "timeshift_buffer.h"
#include "helpers.h"
#include <sstream>

using namespace std;
using namespace ADDON;
using namespace P8PLATFORM;

#ifdef DeleteFile
#undef DeleteFile
#endif

static const int STREAM_READ_BUFFER_SIZE = 8192;
static const  unsigned int CHUNK_FILE_SIZE_LIMIT = (unsigned int)(STREAM_READ_BUFFER_SIZE * 1024) * 256; // 2GB chunk (temporary, pending completion of chunck factory)

TimeshiftBuffer::TimeshiftBuffer(CHelper_libXBMC_addon *addonHelper, const string &streamUrl, const string &bufferCacheDir) :
    m_addonHelper(addonHelper),
    m_bufferDir(bufferCacheDir),
    m_FreeChunks(1000),
    m_PopulatedChunks(1000),
    m_CurrentReadChunk(NULL),
    m_length(0),
    m_position(0),
    m_streamUrl(streamUrl)
{
    m_streamHandle = m_addonHelper->OpenFile(m_streamUrl.c_str(), 0);
    if (!m_streamHandle)
        throw InputBufferException("Failed to open source stream.");

    if(!m_addonHelper->DirectoryExists(m_bufferDir.c_str()))
       if(!m_addonHelper->CreateDirectory(m_bufferDir.c_str()))
          throw InputBufferException("Failed to create cahche directory for timeshift buffer.");

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
    m_PopulatedChunks.Clear();
    m_FreeChunks.Clear();
    m_CurrentReadChunk = NULL;
//    m_addonHelper->CloseFile(m_bufferWriteHandle);
//    m_addonHelper->CloseFile(m_bufferReadHandle);

//    m_addonHelper->DeleteFile(m_bufferPath.c_str());
}

void *TimeshiftBuffer::Process()
{
    unsigned char buffer[STREAM_READ_BUFFER_SIZE];
    ssize_t chunkSize = 0;
    ChunkFilePtr currentChunkFile = NULL;
    bool isEof = false;
    
    while (!isEof && m_streamHandle != NULL && !IsStopped())
    {
        if(NULL == currentChunkFile)
        {
            currentChunkFile  = GetFreeChunk();
//            currentChunkFile->m_reader.Seek(0, SEEK_SET);
//            currentChunkFile->m_reader.Truncate(0);
//            currentChunkFile->m_writer.Seek(0, SEEK_SET);
//            currentChunkFile->m_writer.Truncate(0);
            m_PopulatedChunks.Push(currentChunkFile);
            chunkSize = 0;
            DebugLog(std::string(">>> New current chunk (for write): ") + currentChunkFile->Path());
        }
        ssize_t bytesRead = m_addonHelper->ReadFile(m_streamHandle, buffer, sizeof(buffer));
        isEof = bytesRead <= 0;
        DebugLog(std::string(">>> Write: ") + n_to_string(bytesRead));
        ssize_t bytesWritten = currentChunkFile->m_writer.Write(buffer, bytesRead);
        isEof |= bytesWritten != bytesRead;
        chunkSize += bytesWritten;
        if(chunkSize >= CHUNK_FILE_SIZE_LIMIT)
            currentChunkFile = NULL;
        m_writeEvent.Broadcast();
    }

    return NULL;
}

int64_t TimeshiftBuffer::GetLength() const
{
//    struct __stat64 statData;
//    m_addonHelper->StatFile(m_streamUrl.c_str(), &statData);
//    int64_t length = statData.st_size;
//    if(length) {
//        DebugLog(std::string(">>> GetLength(StatFile): ") + n_to_string(length));
//        return length;
//    }
    
    
    auto chunk = GetCurrentReadChunk(1);
    auto length = (NULL == chunk) ? 0 : m_CurrentReadChunk->m_reader.Length();
    DebugLog(std::string(">>> GetLength(): ") + n_to_string(length));
    return length;
}

int64_t TimeshiftBuffer::GetPosition() const
{
    auto chunk = GetCurrentReadChunk(1);
    auto pos =  (NULL == chunk) ? 0 : m_CurrentReadChunk->m_reader.Position();
    DebugLog(std::string(">>> GetPosition(): ") + n_to_string(pos));
    return pos;
}

TimeshiftBuffer::ChunkFilePtr TimeshiftBuffer::GetCurrentReadChunk(int32_t timeout) const
{
    if(NULL == m_CurrentReadChunk)
    {
        m_PopulatedChunks.Pop(m_CurrentReadChunk, timeout);
        DebugLog(std::string(">>> New current chunk: ") + ((m_CurrentReadChunk) ? m_CurrentReadChunk->Path() : std::string("NULL !!!")));
    }
    return m_CurrentReadChunk;
}

void TimeshiftBuffer::FreeCurrentChunk() const
{

    if(NULL == m_CurrentReadChunk)
        return;
    DebugLog(std::string(">>> Free chunk: ") + m_CurrentReadChunk->Path());
    m_FreeChunks.Push(m_CurrentReadChunk);
    m_CurrentReadChunk = NULL;
}

ssize_t TimeshiftBuffer::Read(unsigned char *buffer, size_t bufferSize)
{

    size_t totalBytesRead = 0;
    int32_t timeout = 10000; //10 sec

    while (totalBytesRead < bufferSize && !IsStopped())
    {
        auto chunk = GetCurrentReadChunk(timeout);
        if(NULL == chunk)
        {
                StopThread();
                break;
        }

        ssize_t bytesRead = 0;
        do
        {
            bytesRead = chunk->m_reader.Read( buffer, bufferSize - totalBytesRead);
            if(bytesRead == 0 && !m_writeEvent.Wait(timeout)) //timeout
            {
                StopThread();
                break;
            }
            DebugLog(std::string(">>> Read: ") + n_to_string(bytesRead));
            totalBytesRead += bytesRead;
            buffer += bytesRead;
            if(chunk->m_reader.Length() >= CHUNK_FILE_SIZE_LIMIT && chunk->m_reader.Position() == chunk->m_reader.Length())
                FreeCurrentChunk();
        }while(0 == bytesRead && !IsStopped());
    }

    return IsStopped() ? -1 :totalBytesRead;
}

long long TimeshiftBuffer::Seek(long long iPosition, int iWhence) const
{
    auto chunk = GetCurrentReadChunk(1);
    auto pos =  (NULL == chunk) ? -1 : m_CurrentReadChunk->m_reader.Seek(iPosition, iWhence);
    DebugLog(std::string(">>> SEEK: ") + n_to_string(pos));
    return pos;
}

bool TimeshiftBuffer::SwitchStream(const string &newUrl)
{

    StopThread();
    FreeCurrentChunk();
    ChunkFilePtr chunk;
    while(m_PopulatedChunks.Pop(chunk))
        m_FreeChunks.Push(chunk);
    m_addonHelper->CloseFile(m_streamHandle);

    m_streamHandle = m_addonHelper->OpenFile(newUrl.c_str(), 0);
    CreateThread();

    return m_streamHandle != NULL;
}

std::string TimeshiftBuffer::UniqueFilename(const std::string& dir)
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
    }while(m_addonHelper->FileExists(candidate.c_str(), false));
    return candidate;
}

TimeshiftBuffer::ChunkFilePtr TimeshiftBuffer::GetFreeChunk()
{
    ChunkFilePtr newChunk;
    if(m_FreeChunks.Pop(newChunk))
    {
        newChunk->Reopen();
        return newChunk;
    }
    newChunk = new CAddonFile(m_addonHelper, UniqueFilename(m_bufferDir).c_str());
    m_ChunkFileSwarm.push_back(ChunkFileSwarm::value_type(newChunk));
    return newChunk;
}


#pragma mark - CGenericFile

TimeshiftBuffer::CGenericFile::CGenericFile(ADDON::CHelper_libXBMC_addon *addonHelper, void* handler)
    : m_handler(handler)
    , m_helper(addonHelper)
{
    if(NULL == m_handler)
        throw InputBufferException("Failed to open tmishift buffer chunk file.");
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


