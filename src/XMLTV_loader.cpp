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
#include "p8-platform/os.h"
#include "XMLTV_loader.hpp"
#include "zlib.h"
#include "rapidxml/rapidxml.hpp"
#include <ctime>
#include <functional>

using namespace std;
using namespace XMLTV;
using namespace ADDON;
using namespace rapidxml;

namespace XMLTV {
    
    static const std::string c_CacheFolder = "special://temp/pvr-puzzle-tv/XmlTvCache/";
    
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
    
    static int GetFileContents(const string& url, string &strContent, CHelper_libXBMC_addon * XBMC)
    {
        strContent.clear();
        
        void* fileHandle = XBMC->OpenFile(url.c_str(), 0);
        if (fileHandle)
        {
            char buffer[1024];
            while (int bytesRead = XBMC->ReadFile(fileHandle, buffer, 1024))
                strContent.append(buffer, bytesRead);
            XBMC->CloseFile(fileHandle);
        }
        
        return strContent.length();
    }
    
    static std::string GetCachedPathFor(const std::string& original)
    {
        return c_CacheFolder + std::to_string(std::hash<std::string>{}(original));
    }
    
    static int GetCachedFileContents(const std::string &filePath, std::string &strContents, CHelper_libXBMC_addon * XBMC)
    {
        bool bNeedReload = false;
        std::string strCachedPath = GetCachedPathFor(filePath);
        
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
            bNeedReload = statOrig.st_size == 0 ||  statOrig.st_size != statCached.st_size;
        }
        else
            bNeedReload = true;
        
        if (bNeedReload)
        {
            GetFileContents(filePath, strContents, XBMC);
            
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
                    XBMC->WriteFile(fileHandle, strContents.c_str(), strContents.length());
                    XBMC->CloseFile(fileHandle);
                }
            }
            return strContents.length();
        }
        
        return GetFileContents(strCachedPath, strContents, XBMC);
    }

    
    static int ParseDateTime(std::string& strDate, bool iDateFormat = true)
    {
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
        
        std::time_t current_time;
        std::time(&current_time);
        long offset = 0;
#ifndef TARGET_WINDOWS
        offset = -std::localtime(&current_time)->tm_gmtoff;
#else
        _get_timezone(&offset);
#endif // TARGET_WINDOWS
        
        long offset_of_date = (hours * 60 * 60) + (minutes * 60);
        if (sign == '-')
        {
            offset_of_date = -offset_of_date;
        }
        
        return mktime(&timeinfo) - offset_of_date - offset;
    }
    
    
    /*
     * This method uses zlib to decompress a gzipped file in memory.
     * Author: Andrew Lim Chong Liang
     * http://windrealm.org
     */
    static bool GzipInflate( const string& compressedBytes, string& uncompressedBytes ) {
        
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
    
   bool CreateDocument(const std::string& url,  xml_document<>& xmlDoc, ADDON::CHelper_libXBMC_addon * XBMC)
    {
        if (url.empty())
        {
            XBMC->Log(LOG_NOTICE, "EPG file path is not configured. EPG not loaded.");
            return false;
        }
        
        string data;
        string decompressed;
        
        if (GetCachedFileContents(url, data, XBMC) == 0)
        {
            XBMC->Log(LOG_ERROR, "Unable to load EPG file '%s':  file is missing or empty.", url.c_str());
            return false;
        }
        
        char * buffer;
        // gzip packed
        if (data[0] == '\x1F' && data[1] == '\x8B' && data[2] == '\x08')
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
    
    bool ParseChannels(const std::string& url,  const ChannelCallback& onChannelFound, ADDON::CHelper_libXBMC_addon * XBMC)
    {
        xml_document<> xmlDoc;
        
        if(!CreateDocument(url, xmlDoc, XBMC))
           return false;
        
        xml_node<> *pRootElement = xmlDoc.first_node("tv");
        if (!pRootElement)
        {
            XBMC->Log(LOG_ERROR, "Invalid EPG XML: no <tv> tag found");
            return false;
        }
        
        xml_node<> *pChannelNode = NULL;
        for(pChannelNode = pRootElement->first_node("channel"); pChannelNode; pChannelNode = pChannelNode->next_sibling("channel"))
        {
            EpgChannel channel;
            if(!GetAttributeValue(pChannelNode, "id", channel.strId))
                continue;
            
            if(!GetNodeValue(pChannelNode, "display-name", channel.strName))
                continue;
            
            xml_node<> *pIconNode = pChannelNode->first_node("icon");
            if (pIconNode == NULL || !GetAttributeValue(pIconNode, "src", channel.strIcon))
                channel.strIcon = "";
                
                onChannelFound(channel);
                }
        
        xmlDoc.clear();
        
        XBMC->Log(LOG_NOTICE, "XMLTV: channels Loaded.");
        
        return true;
    }

    
    bool ParseEpg(const std::string& url,  const EpgEntryCallback& onEpgEntryFound ,ADDON::CHelper_libXBMC_addon * XBMC)
    {
        xml_document<> xmlDoc;
        
        if(!CreateDocument(url, xmlDoc, XBMC))
           return false;
           
        xml_node<> *pRootElement = xmlDoc.first_node("tv");
        if (!pRootElement)
        {
            XBMC->Log(LOG_ERROR, "Invalid EPG XML: no <tv> tag found");
            return false;
        }
        
        xml_node<> *pChannelNode = NULL;
        for(pChannelNode = pRootElement->first_node("programme"); pChannelNode; pChannelNode = pChannelNode->next_sibling("programme"))
        {
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
            entry.iChannelId = stoul(strId.c_str());
            entry.startTime = iTmpStart;
            entry.endTime = iTmpEnd;
            
            GetNodeValue(pChannelNode, "title", entry.strTitle);
            GetNodeValue(pChannelNode, "desc", entry.strPlot);
            
            onEpgEntryFound(entry);
        }
        
        xmlDoc.clear();
        
        XBMC->Log(LOG_NOTICE, "XMLTV: EPG loaded.");
        
        return true;
    }
    
}
