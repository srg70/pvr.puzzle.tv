//
//  XmlSaxHandler.h
//  pvr.puzzle.tv
//
//  Created by Sergey Shramchenko on 31/10/2020.
//  Copyright © 2020 Home. All rights reserved.
//

#ifndef XmlSaxHandler_h
#define XmlSaxHandler_h

#include <type_traits>
#include "expat.h"
#include "globals.hpp"


#ifdef XML_LARGE_SIZE
#  define XML_FMT_INT_MOD "ll"
#else
#  define XML_FMT_INT_MOD "l"
#endif

#ifdef XML_UNICODE_WCHAR_T
#  include <wchar.h>
#  define XML_FMT_STR "ls"
#else
#  define XML_FMT_STR "s"
#endif


namespace XMLTV {

template<typename Derived = void>
class XmlEventHandler
{
public:
    typedef typename std::conditional<std::is_same<Derived, void>::value, XmlEventHandler, Derived>::type Override;

    XmlEventHandler()
    {
        _parser = XML_ParserCreate(NULL);
        XML_SetElementHandler(_parser, StartElement, EndElement);
        XML_SetUserData(_parser, this);
        XML_SetCharacterDataHandler(_parser, CharacterDataHandler);
    }
    ~XmlEventHandler()
    {
        XML_ParserFree(_parser);
    }
    
    bool Default() { return true; }
    bool Element(const XML_Char *name, const XML_Char **attributes) { return static_cast<Override&>(*this).Default(); }
    bool ElementData(const XML_Char *data, int length) { return static_cast<Override&>(*this).Default(); }
    bool ElementEnd(const XML_Char *name) { return static_cast<Override&>(*this).Default(); }

    
    bool Parse(const char *buffer, int size, bool isFinal) {
        if (XML_STATUS_ERROR == XML_Parse(_parser, buffer, size, isFinal)) {
             Globals::LogError("%" XML_FMT_STR " at line %" XML_FMT_INT_MOD "u\n",
                               XML_ErrorString(XML_GetErrorCode(_parser)),
                               XML_GetCurrentLineNumber(_parser));
            return false;
        }
        return true;
    }
    
private:
    void OnError()
    {
        XML_StopParser(_parser, false);
    }
    
    static void XMLCALL StartElement(void *userData, const XML_Char *name, const XML_Char **atts)
    {
        Override* pThis = (Override*)userData;
        if(!pThis->Element(name, atts))
            pThis->OnError();
    }
    static void XMLCALL EndElement(void *userData, const XML_Char *name)
    {
        Override* pThis = (Override*)userData;
        if(!pThis->ElementEnd(name))
            pThis->OnError();
    }
    static void XMLCALL CharacterDataHandler(void *userData, const XML_Char *buffer, int size)
    {
        Override* pThis = (Override*)userData;
        if(!pThis->ElementData(buffer, size))
            pThis->OnError();
    }
    
    XML_Parser _parser;
};

}


#endif /* XmlSaxHandler_h */
