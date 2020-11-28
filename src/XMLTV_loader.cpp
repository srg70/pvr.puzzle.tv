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
        LogDebug("XMLTV: open file %s." , url.c_str());

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
                LogError("XMLTV: file reading callback failed.");
            } else {
                LogDebug("XMLTV: file reading done.");
            }

        }
        else
        {
            LogDebug("XMLTV: failed  to open file.");
            return false;
        }
        
        return true;
    }
    
    static bool ShouldreloadCahcedFile(const std::string &filePath, const std::string& strCachedPath)
    {
        bool bNeedReload = false;
        
        
        LogDebug("XMLTV: open cached file %s." , filePath.c_str());
        
        // check cached file is exists
        if (XBMC->FileExists(strCachedPath.c_str(), false))
        {
            struct __stat64 statCached;
            
            XBMC->StatFile(strCachedPath.c_str(), &statCached);
            
            // Modification time is not provided by some servers.
            // It should be safe to compare file sizes.
            // Patch: Puzzle server does not provide file attributes. If we have cached file less than 12 hours old - use it
            using namespace P8PLATFORM;
            struct timeval cur_time = {0};
            if(0 != gettimeofday(&cur_time, nullptr)){
                cur_time.tv_sec = statCached.st_mtime;//st_mtimespec.tv_sec;
            }
            bNeedReload = (cur_time.tv_sec - statCached.st_mtime) > 12 * 60 * 60;
            if(bNeedReload) {
                // Check remote file stat only when time check failed
                // because it may take a time
                struct __stat64 statOrig;
                XBMC->StatFile(httplib::detail::encode_get_url(filePath).c_str(), &statOrig);
                bNeedReload = statOrig.st_size == 0 ||  statOrig.st_size != statCached.st_size;
            }
            LogDebug("XMLTV: cached file exists. Reload?  %s." , bNeedReload ? "Yes" : "No");
            
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
                auto written = XBMC->WriteFile(fileHandle, buffer, size);
                writer(buffer, size);
                // NOTE: when caching - ignore pareser errors.
                // We  should write full cache file in any case
                return written;
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


    static time_t ParseDateTime(const char* strDate, bool iDateFormat = true)
    {
        static  long offset = LocalTimeOffset();

        struct tm timeinfo;
        memset(&timeinfo, 0, sizeof(tm));
        char sign = '+';
        int hours = 0;
        int minutes = 0;
        
        if (iDateFormat)
            sscanf(strDate, "%04d%02d%02d%02d%02d%02d %c%02d%02d", &timeinfo.tm_year, &timeinfo.tm_mon, &timeinfo.tm_mday, &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec, &sign, &hours, &minutes);
        else
            sscanf(strDate, "%02d.%02d.%04d%02d:%02d:%02d", &timeinfo.tm_mday, &timeinfo.tm_mon, &timeinfo.tm_year, &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec);
        
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
    
    PvrClient::KodiChannelId ChannelIdForChannelName(const std::string& channelName)
    {
        int id = std::hash<std::string>{}(channelName);
        // Although Kodi defines unique broadcast ID as unsigned int for addons
        // internal DB serialisation accepts only signed positive IDs
        return id < 0 ? -id : id;
    }
    
    PvrClient::KodiChannelId EpgChannelIdForXmlEpgId(const char* strId)
    {
        if(*strId == '\0')
            return PvrClient::UnknownChannelId;
        string strToHash(strId);
        StringUtils::ToLower(strToHash);
        return std::hash<std::string>{}(strToHash);
    }

    template <class T>
    class ChannelHandler : public XmlEventHandler<ChannelHandler<T> >
    {
    public:
        typedef std::function<void(const T& newChannel)> TCallback;
        ChannelHandler(TCallback onChannelReady)
        : _isTvTag(false)
        , _isChannelTag(false)
        , _isDisplayNameTag(false)
        , _onChannelReady(onChannelReady)
        {}
        
        bool Element(const XML_Char *name, const XML_Char **attributes) {
            if(strcmp(name, "tv") == 0) {
                _isTvTag = true;
            } else if(strcmp(name, "channel") == 0) {
                if(!_isTvTag) {
                    LogError("XMLTV: no <tv> tag found");
                    return false;
                }
                Cleanup();
                new (&_currentChannel) T();
                for (int i = 0; attributes[i]; i += 2) {
                    if(strcmp(attributes[i], "id") == 0) {
                        _currentChannel.id = EpgChannelIdForXmlEpgId(attributes[i + 1]);
                        break;
                    }
                }
                if(PvrClient::UnknownChannelId == _currentChannel.id) {
                    // Skip this channel, i.e. retrun true when _isChannelTag is false.
                    LogDebug("XMLTV: no channel ID found.");
                    return true;
                }
                _isChannelTag = true;
            }else if(_isChannelTag && strcmp(name, "display-name") == 0) {
                _isDisplayNameTag = true;
            } else if(_isChannelTag && strcmp(name, "icon") == 0) {
                for (int i = 0; attributes[i]; i += 2) {
                    if(strcmp(attributes[i], "src") == 0) {
                        _currentChannel.strIcon = attributes[i + 1];
                        break;
                    }
                }
            } else if(strcmp(name, "programme") == 0){
                LogDebug("XMLTV: found <programme> tag, i.e. channels processing is done.");
                return false;
            }
                
            return true;
        }
        bool ElementEnd(const XML_Char *name) {
            if(strcmp(name, "tv") == 0) {
                _isTvTag = false;
            } else if(strcmp(name, "channel") == 0) {
                if(_isChannelTag) {
                    _onChannelReady(_currentChannel);
                    // NOTE: cleanup will done on element start
                    // because final destructor will destruct _currentChannel (twice)
                    //Cleanup();
                 }
            } else if(strcmp(name, "display-name") == 0) {
                _isDisplayNameTag = false;
            }
            return true;
        }
        
        bool ElementData(const XML_Char *data, int length) {
            if(_isChannelTag && _isDisplayNameTag){
                std::string name(data, length);
                _currentChannel.displayNames.push_back(name);
            }
            return true;
        }

    private:
        void Cleanup() {
            _currentChannel.~T();
            _isChannelTag = false;
        }
        bool _isTvTag;
        bool _isChannelTag;
        bool _isDisplayNameTag;
        TCallback _onChannelReady;
        T _currentChannel;

    };

    bool ParseChannels(const std::string& url,  const ChannelCallback& onChannelFound)
    {
        ChannelHandler<EpgChannel> handler (onChannelFound);
        
        if (!GetCachedFileContents(url, [&handler](const char* buf, unsigned int size) {
            /// NOTE: add parsing error prepegation
            if(handler.Parse(buf, size, false))
               return (int)size;
            return -1;
        }))
        {
            LogError("XMLTV: unable to load EPG file '%s'.", url.c_str());
            return false;
        }
        char dummy;
        handler.Parse(&dummy, 0, true);
        LogNotice( "XMLTV: channels loaded.");
        return true;
    }

    template <class T>
    class ProgrammeHandler : public XmlEventHandler<ProgrammeHandler<T> >
    {
    public:
        typedef std::function<bool(const T& newProgramme)> TCallback;
        ProgrammeHandler(TCallback onProgrammeReady)
        : _isTvTag(false)
        , _isProgrammeTag(false)
        , _isTitleTag(false)
        , _isDescTag(false)
        , _onProgrammeReady(onProgrammeReady)
        , _validElementCounter(0)
        {
            _fileStartAt = time(NULL) + 60*60*24*7; // A week after now
            _fileEndAt = 0;
        }
        
        bool Element(const XML_Char *name, const XML_Char **attributes) {
            if(strcmp(name, "tv") == 0) {
                _isTvTag = true;
            } else if(strcmp(name, "programme") == 0) {
                if(!_isTvTag) {
                    LogError("XMLTV: no <tv> tag found");
                    return false;
                }
                Cleanup();
                new (&_currentProgramme) T();
                _isProgrammeTag = ProcessPorgarmmeAttributes(attributes);
            } else if(_isProgrammeTag && strcmp(name, "title") == 0) {
                _isTitleTag = true;
            } else if(_isProgrammeTag && strcmp(name, "desc") == 0) {
                _isDescTag = true;
            } else if(_isProgrammeTag && strcmp(name, "icon") == 0) {
                for (int i = 0; attributes[i]; i += 2) {
                    if(strcmp(attributes[i], "src") == 0) {
                        _currentProgramme.iconPath = attributes[i + 1];
                        break;
                    }
                }
            }
                
            return true;
        }
        bool ElementEnd(const XML_Char *name) {
            if(strcmp(name, "tv") == 0) {
                _isTvTag = false;
            } else if(strcmp(name, "programme") == 0) {
                if(_isProgrammeTag) {
                    bool interrupted = !_onProgrammeReady(_currentProgramme);
                    // NOTE: cleanup will done on element start
                    // because final destructor will destruct _currentProgramme (twice)
                    //Cleanup();
                    if(interrupted)
                        LogNotice( "XMLTV: EPG is NOT fully loaded (cancelled ?).");
                    else
                        ++_validElementCounter;
                    return !interrupted;
                 }
            } else if(strcmp(name, "title") == 0) {
                _isTitleTag = false;
            } else if(strcmp(name, "desc") == 0) {
                _isDescTag = false;
            }
            return true;
        }
        bool ElementData(const XML_Char *data, int length) {
            if(_isProgrammeTag && _isTitleTag){
                _currentProgramme.strTitle.assign(data, length);
            } else if(_isProgrammeTag && _isDescTag) {
                _currentProgramme.strPlot.assign(data, length);
            }
            return true;
        }
        time_t StartAt() const { return _fileStartAt;}
        time_t EndAt() const { return _fileEndAt;}
        uint32_t Count() const {return _validElementCounter;}
    private:
        bool ProcessPorgarmmeAttributes(const XML_Char ** attributes) {
            const XML_Char* strId = nullptr;
            const XML_Char* strStart = nullptr;
            const XML_Char* strStop = nullptr;
            for (int i = 0; attributes[i]; i += 2) {
                if(strcmp(attributes[i], "channel") == 0) {
                    strId = attributes[i + 1];
                } else if (strcmp(attributes[i], "start") == 0){
                    strStart = attributes[i + 1];
                } else if (strcmp(attributes[i], "stop") == 0){
                    strStop = attributes[i + 1];
                }
            }
            
            if(nullptr == strId)  {
                LogDebug("XMLTV: no channel ID found (programme).");
                return false;
            }
            if(nullptr == strStart || nullptr == strStop){
                LogDebug("XMLTV: no programme start/stop found.");
                return false;
            }
            
            time_t iTmpStart = ParseDateTime(strStart);
            if(iTmpStart > 0 &&  difftime(_fileStartAt, iTmpStart) > 0)
                _fileStartAt = iTmpStart;
            time_t iTmpEnd = ParseDateTime(strStop);
            if(difftime(_fileEndAt, iTmpEnd) < 0)
                _fileEndAt = iTmpEnd;
            
            _currentProgramme.EpgId = EpgChannelIdForXmlEpgId(strId);
            _currentProgramme.startTime = iTmpStart;
            _currentProgramme.endTime = iTmpEnd;
            return true;
        }
        void Cleanup() {
            _currentProgramme.~T();
            _isProgrammeTag = false;
        }
        
        bool _isTvTag;
        bool _isProgrammeTag;
        bool _isTitleTag;
        bool _isDescTag;
        TCallback _onProgrammeReady;
        T _currentProgramme;
        time_t _fileStartAt;
        time_t _fileEndAt;
        uint32_t _validElementCounter;

    };

    bool ParseEpg(const std::string& url,  const EpgEntryCallback& onEpgEntryFound)
    {
        ProgrammeHandler<EpgEntry> handler (onEpgEntryFound);
        
        if (!GetCachedFileContents(url, [&handler](const char* buf, unsigned int size) {
            /// NOTE: add parsing error propagation
            if(handler.Parse(buf, size, false))
               return (int)size;
            return -1;
        }))
        {
            LogError("XMLTV: unable to load EPG file '%s'.", url.c_str());
            return false;
        }
        char dummy;
        handler.Parse(&dummy, 0, true);
        LogDebug("XMLTV: found %d valid EPG elements.", handler.Count());
        if(handler.EndAt() > 0) {
            LogNotice("XMLTV: EPG loaded from %s to  %s", time_t_to_string(handler.StartAt()).c_str(), time_t_to_string(handler.EndAt()).c_str());
        } else {
            LogNotice( "XMLTV: EPG is empty.");
        }
        return true;
    }
    
}
