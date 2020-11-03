//
//  XmlSaxHandler.h
//  pvr.puzzle.tv
//
//  Created by Sergey Shramchenko on 31/10/2020.
//  Copyright © 2020 Home. All rights reserved.
//

#ifndef XmlSaxHandler_h
#define XmlSaxHandler_h

#include "expat.h"
#include <type_traits>

namespace XMLTV {

template<typename Derived = void>
class XmlEventHandler
{
public:
    typedef typename std::conditional<std::is_same<Derived, void>::value, XmlEventHandler, Derived>::type Override;

    XmlEventHandler()
    {
        p = XML_ParserCreate(NULL);
    }
    ~XmlEventHandler()
    {
        XML_ParserFree(p);
    }
    
    bool Default() { return true; }
    bool Element(const XML_Char *name, const XML_Char **attributes) { return static_cast<Override&>(*this).Default(); }
    bool ElementData(const XML_Char *data, int length) { return static_cast<Override&>(*this).Default(); }
    
private:
    XML_Parser p;
};

}


#endif /* XmlSaxHandler_h */
