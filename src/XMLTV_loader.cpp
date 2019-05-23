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
#include "globals.hpp"

using namespace std;
using namespace XMLTV;
using namespace rapidxml;
using namespace Globals;
using namespace ADDON;

namespace XMLTV {
    
    static const std::string c_CacheFolder = "special://temp/pvr-puzzle-tv/XmlTvCache/";
    
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
    
    static int GetFileContents(const string& url, string &strContent)
    {
        strContent.clear();
        
        XBMC->Log(LOG_DEBUG, "XMLTV Loader: open file %s." , url.c_str());

        void* fileHandle = XBMC->OpenFile(url.c_str(), 0);
        if (fileHandle)
        {
            char buffer[1024];
            while (int bytesRead = XBMC->ReadFile(fileHandle, buffer, 1024))
                strContent.append(buffer, bytesRead);
            XBMC->CloseFile(fileHandle);
            XBMC->Log(LOG_DEBUG, "XMLTV Loader: file reading done.");

        }
        else
        {
            XBMC->Log(LOG_DEBUG, "XMLTV Loader: failed  to open file.");
        }
        
        return strContent.length();
    }
    
    static std::string GetCachedPathFor(const std::string& original)
    {
        return c_CacheFolder + std::to_string(std::hash<std::string>{}(original));
    }
    
    static bool ShouldreloadCahcedFile(const std::string &filePath, const std::string& strCachedPath)
    {
        bool bNeedReload = false;
        
        
        XBMC->Log(LOG_DEBUG, "XMLTV Loader: open cached file %s." , filePath.c_str());
        
        // check cached file is exists
        if (XBMC->FileExists(strCachedPath.c_str(), false))
        {
            struct __stat64 statCached;
            struct __stat64 statOrig;
            
            XBMC->StatFile(strCachedPath.c_str(), &statCached);
            XBMC->StatFile(filePath.c_str(), &statOrig);
            
            //bNeedReload = statCached.st_mtime < statOrig.st_mtime || statOrig.st_mtime == 0;
            // Modification time is not provided by some servers.
            // It should be safe to compare file sizes.
            // Patch: Puzzle server does not provide file attributes. If we have cached file less than 5 min old - use it
            using namespace P8PLATFORM;
            struct timeval cur_time = {0};
            if(0 != gettimeofday(&cur_time, nullptr)){
                cur_time.tv_sec = statCached.st_mtime;//st_mtimespec.tv_sec;
            }
            bNeedReload = (cur_time.tv_sec - statCached.st_mtime) > 5 * 60  && (statOrig.st_size == 0 ||  statOrig.st_size != statCached.st_size);
            XBMC->Log(LOG_DEBUG, "XMLTV Loader: cached file exists. Reload?  %s." , bNeedReload ? "Yes" : "No");
            
        }
        else
            bNeedReload = true;
        return bNeedReload;
    }
    
    static bool ReloadCachedFile(const std::string &filePath, const std::string& strCachedPath, std::string &strContents)
    {
        bool succeeded = false;
        
        GetFileContents(filePath, strContents);
        
        // write to cache
        if (strContents.length() > 0)
        {
            void* fileHandle = XBMC->OpenFileForWrite(strCachedPath.c_str(), true);
            if(!fileHandle) {
                XBMC->CreateDirectory(c_CacheFolder.c_str());
                fileHandle = XBMC->OpenFileForWrite(strCachedPath.c_str(), true);
            }
            if (fileHandle)
            {
                auto bytesToWrite = strContents.length();
                succeeded = bytesToWrite == XBMC->WriteFile(fileHandle, strContents.c_str(), bytesToWrite);
                XBMC->CloseFile(fileHandle);
            }
        }
        return succeeded;
    }
    
    int GetCachedFileContents(const std::string &filePath, std::string &strContents)
    {
        std::string strCachedPath =  GetCachedPathFor(filePath);
        bool bNeedReload =  ShouldreloadCahcedFile(filePath, strCachedPath);
        
        if (bNeedReload)
        {
            ReloadCachedFile(filePath, strCachedPath, strContents);
            return strContents.length();
        }
        
        return GetFileContents(strCachedPath, strContents);
    }

    std::string GetCachedFilePath(const std::string &filePath)
    {
        std::string strCachedPath =  GetCachedPathFor(filePath);
        bool bNeedReload =  ShouldreloadCahcedFile(filePath, strCachedPath);
        
        if (!bNeedReload)
            return strCachedPath;

        std::string strContents;
        if(ReloadCachedFile(filePath, strCachedPath, strContents))
            return strCachedPath;
        
        return std::string();
    }
    
    static int ParseDateTime(std::string& strDate, bool iDateFormat = true)
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
#endif // TARGET_WINDOWS

        return offset;
    }
    
    /*
     * This method uses zlib to decompress a gzipped file in memory.
     * Author: Andrew Lim Chong Liang
     * http://windrealm.org
     */
    bool GzipInflate( const string& compressedBytes, string& uncompressedBytes ) {
        
#define HANDLE_CALL_ZLIB(status) {   \
if(status != Z_OK) {        \
free(uncomp);             \
return false;             \
}                           \
}
        
        if ( compressedBytes.size() == 0 )
        {
            uncompressedBytes = compressedBytes ;
            return true ;
        }
        
        uncompressedBytes.clear() ;
        
        unsigned full_length = compressedBytes.size() ;
        unsigned half_length = compressedBytes.size() / 2;
        
        unsigned uncompLength = full_length ;
        char* uncomp = (char*) calloc( sizeof(char), uncompLength );
        
        z_stream strm;
        strm.next_in = (Bytef *) compressedBytes.c_str();
        strm.avail_in = compressedBytes.size() ;
        strm.total_out = 0;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        
        bool done = false ;
        
        HANDLE_CALL_ZLIB(inflateInit2(&strm, (16+MAX_WBITS)));
        
        while (!done)
        {
            // If our output buffer is too small
            if (strm.total_out >= uncompLength )
            {
                // Increase size of output buffer
                uncomp = (char *) realloc(uncomp, uncompLength + half_length);
                if (uncomp == NULL)
                    return false;
                uncompLength += half_length ;
            }
            
            strm.next_out = (Bytef *) (uncomp + strm.total_out);
            strm.avail_out = uncompLength - strm.total_out;
            
            // Inflate another chunk.
            int err = inflate (&strm, Z_SYNC_FLUSH);
            if (err == Z_STREAM_END)
                done = true;
            else if (err != Z_OK)
            {
                break;
            }
        }
        
        HANDLE_CALL_ZLIB(inflateEnd (&strm));
        
        for ( size_t i=0; i<strm.total_out; ++i )
        {
            uncompressedBytes += uncomp[ i ];
        }
        
        free( uncomp );
        return true ;
    }
   
    bool IsDataCompressed(const std::string& data)
    {
        return (data[0] == '\x1F' && data[1] == '\x8B' && data[2] == '\x08');
    }
    
   bool CreateDocument(const std::string& url,  XmlDocumentAndData& xmlDoc)
    {
        if (url.empty())
        {
            XBMC->Log(LOG_NOTICE, "EPG file path is not configured. EPG not loaded.");
            return false;
        }
        
        string data;
        string decompressed;
        
        if (GetCachedFileContents(url, data) == 0)
        {
            XBMC->Log(LOG_ERROR, "Unable to load EPG file '%s':  file is missing or empty.", url.c_str());
            return false;
        }
        
        char * buffer;
        // gzip packed
        if (IsDataCompressed(data))
        {
            if (!GzipInflate(data, decompressed))
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
    
    bool ParseChannels(const std::string& url,  const ChannelCallback& onChannelFound)
    {

        XmlDocumentAndData xmlDoc;
        
        XBMC->Log(LOG_DEBUG, "XMLTV Loader: open document from %s." , url.c_str());

        
        if(!CreateDocument(url, xmlDoc))
           return false;
        
        XBMC->Log(LOG_DEBUG, "XMLTV Loader: start document parsing.");

        xml_node<> *pRootElement = xmlDoc.doc.first_node("tv");
        if (!pRootElement)
        {
            XBMC->Log(LOG_ERROR, "Invalid EPG XML: no <tv> tag found");
            return false;
        }
        
        xml_node<> *pChannelNode = NULL;
        for(pChannelNode = pRootElement->first_node("channel"); pChannelNode; pChannelNode = pChannelNode->next_sibling("channel"))
        {
            //XBMC->Log(LOG_DEBUG, "XMLTV Loader: found channel node.");

            EpgChannel channel;
            std::string strId;
            if(!GetAttributeValue(pChannelNode, "id", strId)){
                XBMC->Log(LOG_DEBUG, "XMLTV Loader: no channel ID found.");
                continue;
            }
            
            channel.id = ChannelIdForChannelName(strId);
            
            if(!GetNodeValue(pChannelNode, "display-name", channel.strName)){
                XBMC->Log(LOG_DEBUG, "XMLTV Loader: no channel display name found.");
                continue;
            }
            
            //XBMC->Log(LOG_DEBUG, "XMLTV Loader: found channel %s.", channel.strName.c_str());

            xml_node<> *pIconNode = pChannelNode->first_node("icon");
            if (pIconNode == NULL)
                channel.strIcon = "";
            else if (!GetAttributeValue(pIconNode, "src", channel.strIcon))
                channel.strIcon = "";
            
            //XBMC->Log(LOG_DEBUG, "XMLTV Loader: populating channel");

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
        
        xml_node<> *pChannelNode = NULL;
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
                
                int iTmpStart = ParseDateTime(strStart);
                int iTmpEnd = ParseDateTime(strStop);
                
                EpgEntry entry;
                entry.iChannelId = ChannelIdForChannelName(strId);
                entry.startTime = iTmpStart;
                entry.endTime = iTmpEnd;
                
                GetNodeValue(pChannelNode, "title", entry.strTitle);
                GetNodeValue(pChannelNode, "desc", entry.strPlot);
                
                onEpgEntryFound(entry);

            } catch (...) {
                LogError("Bad XML EPG entry.");
            }
        }
        
        xmlDoc.doc.clear();
        
        XBMC->Log(LOG_NOTICE, "XMLTV: EPG loaded.");
        
        return true;
    }
    
}
