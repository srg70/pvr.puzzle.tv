/*
 *
 *   Copyright (C) 2017 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
 *
 *   Copyright (C) 2013-2015 Anton Fedchin
 *   http://github.com/afedchin/xbmc-addon-iptvsimple/
 *
 *   Copyright (C) 2011 Pulse-Eight
 *   http://www.pulse-eight.com/
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

#include "p8-platform/util/StringUtils.h"
#include "p8-platform/util/timeutils.h"
#include "p8-platform/os.h"
#include "XMLTV_loader.hpp"
#include "zlib.h"
#include "rapidxml/rapidxml.hpp"
#include <ctime>
#include <functional>
#include <limits>
#include "globals.hpp"
#include "helpers.h"
#include "httplib.h"
#include "XmlSaxHandler.h"

using namespace std;
using namespace rapidxml;
using namespace Globals;
using namespace ADDON;
using namespace Helpers;

namespace XMLTV {
    
    static const std::string c_CacheFolder = "special://temp/pvr-puzzle-tv/XmlTvCache/";
    
    class Inflator{
    public:
        Inflator(DataWriter writer)
        {
            _writer = writer;
            /* allocate inflate state */
            _strm.zalloc = Z_NULL;
            _strm.zfree = Z_NULL;
            _strm.opaque = Z_NULL;
            _strm.avail_in = 0;
            _strm.next_in = Z_NULL;
            _state = inflateInit2(&_strm, (16+MAX_WBITS));//inflateInit(&strm);
            if (_state != Z_OK) {
                LogError("Inflator: failed to initialize ZIP library %d (inflateInit(...))", _state);
            }

        }
        bool Process(const char* buffer, unsigned int size)
        {
            if(_state == Z_STREAM_END)
                return true;
            if(_state != Z_OK && _state != Z_BUF_ERROR)
                return false;
            _strm.avail_in = size;
             if ((int)(_strm.avail_in) < 0) {
                 _state = Z_BUF_ERROR;
                 LogError("Inflator: failed to read from source.");
                 return false;
             }
             if (_strm.avail_in == 0)
                 return true;
            _strm.next_in = (unsigned char*)buffer;
            
            /* run inflate() on input until output buffer not full */
            const unsigned int CHUNK  = 16384;
            char out[CHUNK];
            int have;
            do {
                _strm.avail_out = CHUNK;
                _strm.next_out = (unsigned char*)out;
                _state = inflate(&_strm, Z_NO_FLUSH);
                assert(_state != Z_STREAM_ERROR);  /* state not clobbered */
                switch (_state) {
                case Z_NEED_DICT:
                    _state = Z_DATA_ERROR;     /* and fall through */
                case Z_DATA_ERROR:
                case Z_MEM_ERROR:
                    LogError("Inflator: failed to inflate compressed chunk %d (inflate(...))", _state);
                    return false;
                }
                have = CHUNK - _strm.avail_out;
                if (_writer(out, have) != have) {
                    LogError("Inflator: failed to write to destination.");
                    return Z_ERRNO;
                }
            } while (_strm.avail_out == 0);
            return true;
        }
        
        bool IsDone() const {return _state == Z_STREAM_END;}
        
        ~Inflator() {
            (void)inflateEnd(&_strm);
        }
    private:
        int _state;
        z_stream _strm;
        DataWriter _writer;

    };

#pragma mark - File Cache
    static bool GetFileContents(const string& url, DataWriter writer)
    {
        LogDebug("XMLTV Loader: open file %s." , url.c_str());

        void* fileHandle = XBMC_OpenFile(url);
        if (fileHandle)
        {
            char buffer[1024];
            bool isError = false;
            while (int bytesRead = XBMC->ReadFile(fileHandle, buffer, 1024)) {
                if(bytesRead != writer(buffer, bytesRead)) {
                    isError = true;
                    break;
                }
            }
            XBMC->CloseFile(fileHandle);
            if(isError) {
                LogError("XMLTV Loader: file reading callback failed.");
            } else {
                LogDebug("XMLTV Loader: file reading done.");
            }

        }
        else
        {
            LogDebug("XMLTV Loader: failed  to open file.");
            return false;
        }
        
        return true;
    }
    
    static bool ShouldreloadCahcedFile(const std::string &filePath, const std::string& strCachedPath)
    {
        bool bNeedReload = false;
        
        
        LogDebug("XMLTV Loader: open cached file %s." , filePath.c_str());
        
        // check cached file is exists
        if (XBMC->FileExists(strCachedPath.c_str(), false))
        {
            struct __stat64 statCached;
            struct __stat64 statOrig;
            
            XBMC->StatFile(strCachedPath.c_str(), &statCached);
            XBMC->StatFile(httplib::detail::encode_get_url(filePath).c_str(), &statOrig);
            
            // Modification time is not provided by some servers.
            // It should be safe to compare file sizes.
            // Patch: Puzzle server does not provide file attributes. If we have cached file less than 5 min old - use it
            using namespace P8PLATFORM;
            struct timeval cur_time = {0};
            if(0 != gettimeofday(&cur_time, nullptr)){
                cur_time.tv_sec = statCached.st_mtime;//st_mtimespec.tv_sec;
            }
            bNeedReload = (cur_time.tv_sec - statCached.st_mtime) > 5 * 60  && (statOrig.st_size == 0 ||  statOrig.st_size != statCached.st_size);
            LogDebug("XMLTV Loader: cached file exists. Reload?  %s." , bNeedReload ? "Yes" : "No");
            
        }
        else
            bNeedReload = true;
        return bNeedReload;
    }
    
    static bool IsDataCompressed(const char* data, unsigned int size)
    {
        return size > 2 && (data[0] == '\x1F' && data[1] == '\x8B' && data[2] == '\x08');
    }

    static bool ReloadCachedFile(const std::string &filePath, const std::string& strCachedPath, DataWriter writer)
    {

        void* fileHandle = XBMC->OpenFileForWrite(strCachedPath.c_str(), true);
        if(!fileHandle) {
            XBMC->CreateDirectory(c_CacheFolder.c_str());
            fileHandle = XBMC->OpenFileForWrite(strCachedPath.c_str(), true);
        }
        DataWriter cacheWriter = writer;
        if (fileHandle)
        {
            cacheWriter = [&fileHandle, &writer](const char* buffer, unsigned int size){
                XBMC->WriteFile(fileHandle, buffer, size);
                return writer(buffer, size);
            };
        }

        bool succeeded = false;
        bool isContentZipped = false;
        bool checkCompression = true;
        Inflator inflator(cacheWriter);
        DataWriter decompressor = [&cacheWriter, &inflator, &isContentZipped, &checkCompression](const char* buffer, unsigned int size){
            if(checkCompression){
                checkCompression = false;
                isContentZipped = IsDataCompressed(buffer, size);
            }
            if(isContentZipped)
                return inflator.Process(buffer, size) ? (int)size : -1;
            return cacheWriter(buffer, size);
        };;

        succeeded = GetFileContents(filePath, decompressor);
        if (fileHandle)
        {
           XBMC->CloseFile(fileHandle);
        }
        return succeeded;
    }
    
    bool GetCachedFileContents(const std::string &filePath, DataWriter writer)
    {
        std::string strCachedPath =  GetCachedPathFor(filePath);
        bool bNeedReload =  ShouldreloadCahcedFile(filePath, strCachedPath);
        
        if (bNeedReload)
        {
            return ReloadCachedFile(filePath, strCachedPath, writer);
        }
        
        return GetFileContents(strCachedPath, writer);
    }

    std::string GetCachedPathFor(const std::string& original)
    {
        return c_CacheFolder + std::to_string(std::hash<std::string>{}(original));
    }

    /*
     * This method uses zlib to decompress a gzipped file in memory.
     * Author: Andrew Lim Chong Liang
     * http://windrealm.org
     */
    // Modified by srg70 to reduce memory footprint based on examples from https://zlib.net/zlib_how.html
    static bool GzipInflate(DataReder reader, DataWriter writer) {
        const unsigned int CHUNK  = 16384;
        char in[CHUNK];
        Inflator inflator(writer);
        do {
            int size = reader(in, CHUNK);
            if(!inflator.Process(in, size))
                break;
        } while(!inflator.IsDone());
        return inflator.IsDone();
//        const unsigned int CHUNK  = 16384;
//        int ret;
//        unsigned have;
//        z_stream strm;
//        char in[CHUNK];
//        char out[CHUNK];
//        /* allocate inflate state */
//        strm.zalloc = Z_NULL;
//        strm.zfree = Z_NULL;
//        strm.opaque = Z_NULL;
//        strm.avail_in = 0;
//        strm.next_in = Z_NULL;
//        ret = inflateInit2(&strm, (16+MAX_WBITS));//inflateInit(&strm);
//        if (ret != Z_OK) {
//            LogError("XMLTV: failed to initialize ZIP library %d (inflateInit(...))", ret);
//            return false;
//        }
//
//        /* decompress until deflate stream ends or end of file */
//        do {
//            strm.avail_in = reader(in,CHUNK);
//             if ((int)(strm.avail_in) < 0) {
//                 (void)inflateEnd(&strm);
//                 LogError("XMLTV: failed to read from source.");
//                 return false;
//             }
//             if (strm.avail_in == 0)
//                 break;
//             strm.next_in = (unsigned char*)in;
//
//            /* run inflate() on input until output buffer not full */
//            do {
//                strm.avail_out = CHUNK;
//                 strm.next_out = (unsigned char*)out;
//                ret = inflate(&strm, Z_NO_FLUSH);
//                assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
//                switch (ret) {
//                case Z_NEED_DICT:
//                    ret = Z_DATA_ERROR;     /* and fall through */
//                case Z_DATA_ERROR:
//                case Z_MEM_ERROR:
//                    LogError("XMLTV: failed to inflate compressed chunk %d (inflate(...))", ret);
//                    (void)inflateEnd(&strm);
//                    return false;
//                }
//                have = CHUNK - strm.avail_out;
//                if (writer(out, have) != have) {
//                    LogError("XMLTV: failed to write to destination.");
//                    (void)inflateEnd(&strm);
//                    return Z_ERRNO;
//                }
//            } while (strm.avail_out == 0);
//
//            /* done when inflate() says it's done */
//        } while (ret != Z_STREAM_END);
//
//        /* clean up and return */
//        (void)inflateEnd(&strm);
//        return ret == Z_STREAM_END;// ? Z_OK : Z_DATA_ERROR;

    }

#pragma mark - XML

    struct XmlDocumentAndData {
        xml_document<> doc;
        template<int Flags>
        void parse(const char *text){
            data = text;
            doc.parse<Flags>(&data[0]);
        }
    private:
        string data;
    };

    template<class Ch>
    inline bool GetNodeValue(const xml_node<Ch> * pRootNode, const char* strTag, string& strStringValue)
    {
        xml_node<Ch> *pChildNode = pRootNode->first_node(strTag);
        if (pChildNode == NULL)
        {
            return false;
        }
        strStringValue = pChildNode->value();
        return true;
    }

    template<class Ch>
    inline bool GetAllNodesValue(const xml_node<Ch> * pRootNode, const char* strTag, list<string>& strStringValues)
    {
        xml_node<Ch> *pChildNode = pRootNode->first_node(strTag);
        if (pChildNode == NULL)
        {
            return false;
        }
        do{
            strStringValues.push_back(pChildNode->value());
            pChildNode = pChildNode->next_sibling(strTag);
        } while(pChildNode);
        
        return true;
    }

    template<class Ch>
    inline bool GetAttributeValue(const xml_node<Ch> * pNode, const char* strAttributeName, string& strStringValue)
    {
        xml_attribute<Ch> *pAttribute = pNode->first_attribute(strAttributeName);
        if (pAttribute == NULL)
        {
            return false;
        }
        strStringValue = pAttribute->value();
        return true;
    }


    static time_t ParseDateTime(std::string& strDate, bool iDateFormat = true)
    {
        static  long offset = LocalTimeOffset();

        struct tm timeinfo;
        memset(&timeinfo, 0, sizeof(tm));
        char sign = '+';
        int hours = 0;
        int minutes = 0;
        
        if (iDateFormat)
            sscanf(strDate.c_str(), "%04d%02d%02d%02d%02d%02d %c%02d%02d", &timeinfo.tm_year, &timeinfo.tm_mon, &timeinfo.tm_mday, &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec, &sign, &hours, &minutes);
        else
            sscanf(strDate.c_str(), "%02d.%02d.%04d%02d:%02d:%02d", &timeinfo.tm_mday, &timeinfo.tm_mon, &timeinfo.tm_year, &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec);
        
        timeinfo.tm_mon  -= 1;
        timeinfo.tm_year -= 1900;
        timeinfo.tm_isdst = -1;
        
        long offset_of_date = (hours * 60 * 60) + (minutes * 60);
        if (sign == '-')
        {
            offset_of_date = -offset_of_date;
        }
        
        return mktime(&timeinfo) - offset_of_date - offset;
    }
    
    long LocalTimeOffset()
    {
        std::time_t current_time;
        std::time(&current_time);
        long offset = 0;
#ifndef TARGET_WINDOWS
        offset = -std::localtime(&current_time)->tm_gmtoff;
#else
        _get_timezone(&offset);
        int daylightHours = 0;
        _get_daylight(&daylightHours);
        LogDebug("Timezone offset: %d sec, daylight offset %d h", offset, daylightHours);
        offset -= daylightHours * 60 * 60;
#endif // TARGET_WINDOWS

        LogDebug("Total timezone offset: %d sec", offset);
        return offset;
    }
    
    bool CreateDocument(const std::string& url,  XmlDocumentAndData& xmlDoc)
    {
        if (url.empty())
        {
            XBMC->Log(LOG_NOTICE, "EPG file path is not configured. EPG not loaded.");
            return false;
        }
        
        string data;
        if (!GetCachedFileContents(url, [&data](const char* buf, unsigned int size) {
            data.append(buf, size);
            return size;
        }))
        {
            XBMC->Log(LOG_ERROR, "Unable to load EPG file '%s':  file is missing or empty.", url.c_str());
            return false;
        }
        
        char * buffer;
        string decompressed;
        // gzip packed
        if (IsDataCompressed(data.c_str(), data.length()))
        {
            unsigned int dataIndex = 0;
            if (!GzipInflate([&data, &dataIndex](char* buf, unsigned int size){
                const unsigned int toBeCopied = std::min(data.size() - dataIndex, (size_t)size);
                memcpy(buf, &data[dataIndex], toBeCopied);
                dataIndex += toBeCopied;
                return toBeCopied;
            }, [&decompressed](const char* buf, unsigned int size) {
                decompressed.append(buf, size);
                return size;
            }))
            {
                XBMC->Log(LOG_ERROR, "Invalid EPG file '%s': unable to decompress file.", url.c_str());
                return false;
            }
            buffer = &(decompressed[0]);
        }
        else
            buffer = &(data[0]);
        
        // xml should starts with '<?xml'
        if (buffer[0] != '\x3C' || buffer[1] != '\x3F' || buffer[2] != '\x78' ||
            buffer[3] != '\x6D' || buffer[4] != '\x6C')
        {
            // check for BOM
            if (buffer[0] != '\xEF' || buffer[1] != '\xBB' || buffer[2] != '\xBF')
            {
                // check for tar archive
                if (strcmp(buffer + 0x101, "ustar") || strcmp(buffer + 0x101, "GNUtar"))
                    buffer += 0x200; // RECORDSIZE = 512
                else
                {
                    XBMC->Log(LOG_ERROR, "Invalid EPG file '%s': unable to parse file.", url.c_str());
                    return false;
                }
            }
        }
        
        try
        {
            xmlDoc.parse<0>(buffer);
        }
        catch(parse_error p)
        {
            XBMC->Log(LOG_ERROR, "Unable parse EPG XML: %s", p.what());
            return false;
        }
        return true;

    }
    
    PvrClient::KodiChannelId ChannelIdForChannelName(const std::string& channelName)
    {
        int id = std::hash<std::string>{}(channelName);
        // Although Kodi defines unique broadcast ID as unsigned int for addons
        // internal DB serialisation accepts only signed positive IDs
        return id < 0 ? -id : id;
    }
    
    PvrClient::KodiChannelId EpgChannelIdForXmlEpgId(const std::string& strId)
    {
        if(strId.empty())
            return PvrClient::UnknownChannelId;
        string strToHash(strId);
        StringUtils::ToLower(strToHash);
        return std::hash<std::string>{}(strToHash);
    }

    bool ParseChannels(const std::string& url,  const ChannelCallback& onChannelFound)
    {
        class ChannelHandler : public XmlEventHandler<ChannelHandler>
        {
            
        } handler;
        
        XmlDocumentAndData xmlDoc;
        
        LogDebug("XMLTV Loader: open document from %s." , url.c_str());

        
        if(!CreateDocument(url, xmlDoc))
           return false;
        
        LogDebug("XMLTV Loader: start document parsing.");

        xml_node<> *pRootElement = xmlDoc.doc.first_node("tv");
        if (!pRootElement)
        {
            XBMC->Log(LOG_ERROR, "Invalid EPG XML: no <tv> tag found");
            return false;
        }
        
        xml_node<> *pChannelNode = NULL;
        for(pChannelNode = pRootElement->first_node("channel"); pChannelNode; pChannelNode = pChannelNode->next_sibling("channel"))
        {
            //LogDebug("XMLTV Loader: found channel node.");

            EpgChannel channel;
            std::string strId;
            if(!GetAttributeValue(pChannelNode, "id", strId)){
                LogDebug("XMLTV Loader: no channel ID found.");
                continue;
            }
            
            channel.id = EpgChannelIdForXmlEpgId(strId);
            
            if(!GetAllNodesValue(pChannelNode, "display-name", channel.displayNames)){
                LogDebug("XMLTV Loader: no channel display name found.");
                continue;
            }
            
            //LogDebug("XMLTV Loader: found channel %s.", channel.strName.c_str());

            xml_node<> *pIconNode = pChannelNode->first_node("icon");
            if (pIconNode == NULL)
                channel.strIcon = "";
            else if (!GetAttributeValue(pIconNode, "src", channel.strIcon))
                channel.strIcon = "";
            
            //LogDebug("XMLTV Loader: populating channel");

            onChannelFound(channel);
        }
        
        xmlDoc.doc.clear();
        
        XBMC->Log(LOG_NOTICE, "XMLTV: channels Loaded.");
        
        return true;
    }

    
    bool ParseEpg(const std::string& url,  const EpgEntryCallback& onEpgEntryFound)
    {
        XmlDocumentAndData xmlDoc;
        
        if(!CreateDocument(url, xmlDoc))
           return false;
           
        xml_node<> *pRootElement = xmlDoc.doc.first_node("tv");
        if (!pRootElement)
        {
            XBMC->Log(LOG_ERROR, "Invalid EPG XML: no <tv> tag found");
            return false;
        }
        time_t fileStartAt = time(NULL) + 60*60*24*7; // A week after now
        time_t fileEndAt = 0;
        xml_node<> *pChannelNode = NULL;
        
        bool interrupted = false;
        for(pChannelNode = pRootElement->first_node("programme"); pChannelNode; pChannelNode = pChannelNode->next_sibling("programme"))
        {
            try {
                string strId;
                if (!GetAttributeValue(pChannelNode, "channel", strId))
                    continue;
                
                string strStart, strStop;
                if ( !GetAttributeValue(pChannelNode, "start", strStart)
                    || !GetAttributeValue(pChannelNode, "stop", strStop))
                    continue;
                
                time_t iTmpStart = ParseDateTime(strStart);
                if(fileStartAt > iTmpStart)
                    fileStartAt = iTmpStart;
                time_t iTmpEnd = ParseDateTime(strStop);
                if(fileEndAt < iTmpEnd)
                    fileEndAt = iTmpEnd;

                
                EpgEntry entry;
                xml_node<> * iconAttribute = pChannelNode->first_node("icon");
                if (iconAttribute != NULL)
                {
                    GetAttributeValue(iconAttribute, "src", entry.iconPath);
                }
                
                entry.EpgId = EpgChannelIdForXmlEpgId(strId);
               // LogDebug("Program: TVG id %s => EPG id %d", strId.c_str(), entry.EpgId);
                entry.startTime = iTmpStart;
                entry.endTime = iTmpEnd;
                
                GetNodeValue(pChannelNode, "title", entry.strTitle);
                GetNodeValue(pChannelNode, "desc", entry.strPlot);
                
                interrupted = !onEpgEntryFound(entry);
                if(interrupted)
                    break;

            } catch (...) {
                LogError("Bad XML EPG entry.");
            }
        }
        
        xmlDoc.doc.clear();
        
        if(interrupted) {
            XBMC->Log(LOG_NOTICE, "XMLTV: EPG is NOT fully loaded (cancelled ?).");
        }else if(fileEndAt > 0) {
            XBMC->Log(LOG_NOTICE, "XMLTV: EPG loaded from %s to  %s", time_t_to_string(fileStartAt).c_str(), time_t_to_string(fileEndAt).c_str());

        } else {
            XBMC->Log(LOG_NOTICE, "XMLTV: EPG is empty.");
        }
        
        return true;
    }
    
}
