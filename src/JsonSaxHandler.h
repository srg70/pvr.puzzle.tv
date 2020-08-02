//
//  JsonSaxHandler.h
//  pvr.puzzle.tv
//
//  Created by Sergey Shramchenko on 31/07/2020.
//  Copyright © 2020 Home. All rights reserved.
//

#ifndef JsonSaxHandler_h
#define JsonSaxHandler_h

#include "rapidjson/reader.h"
#include "rapidjson/error/en.h"
#include <iostream>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>
 
 
namespace Helpers {
namespace Json {

using namespace std;
using namespace rapidjson;


template<class TDerived>
struct ParserForBase : public BaseReaderHandler<UTF8<>, TDerived>  {
private:
    typedef BaseReaderHandler<UTF8<>, TDerived>  TBase;

public:
    inline bool HasError() const { return m_isError; }
    inline const string& GetParseError() const {return m_error; }

    inline bool Null() { return this->TBase::Null();}
    inline bool Bool(bool b) { return this->TBase::Bool(b);}
    inline bool Int(int i) { return this->TBase::Int(i);}
    inline bool Uint(unsigned i) { return this->TBase::Uint(i);}
    inline bool Int64(int64_t i) { return this->TBase::Int64(i);}
    inline bool Uint64(uint64_t i) { return this->TBase::Uint64(i);}
    inline bool Double(double d) { return this->TBase::Double(d);}
    inline bool RawNumber(const typename TBase::Ch* str, SizeType length, bool copy){ return this->TBase::RawNumber(str, length, copy);}
    inline bool StartArray() { return this->TBase::StartArray();}
    inline bool EndArray(SizeType elementCount) { return this->TBase::EndArray(elementCount);}

protected:
    inline bool error(const string& reason)
    {
        m_isError = true;
        m_error = reason;
        return false;
    }
private:
    bool m_isError;
    string m_error;

};

template<class T>
struct ParserForObject : public ParserForBase<ParserForObject<T> > {

private :
    typedef ParserForBase<ParserForObject<T> > TBase;

public:
    typedef function <bool(const T&)> TObjectDelegate;
    typedef string T::*TStringField;
    
    inline ParserForObject<T>& WithField(const string& name, TStringField field)
    {
        m_stringFields.emplace(name, field);
        return *this;
    }
    
    bool StartObject() {
        switch (m_state) {
            case kExpectObjectStart:
            {
                m_object = T();
                m_jsonFields.clear();
                m_state = kExpectNameOrObjectEnd;
                return true;
            }
            default:
                return this->error(string("StartObject: unexpected state") + to_string(m_state));
        }
    }
    
    bool Key(const char* str, SizeType length, bool copy) {
         switch (m_state) {
             case kExpectNameOrObjectEnd:
             {
                 string name = string(str, length);
                 m_jsonFields.push_back(move(name));
                 m_state = kExpectValue;
                 return true;
             }
             default:
                 return this->error(string("Key: unexpected state") + to_string(m_state));
         }
         return String(str, length, copy);
     }

    bool String(const char* str, SizeType length, bool) {
        switch (m_state) {
            case kExpectValue:
            {
                auto pName = prev(m_jsonFields.end());
                m_state = kExpectNameOrObjectEnd;
                if(m_stringFields.count(*pName) == 0) {
                    m_jsonFields.erase(pName); // OK. The field was not registerd for parsing
                } else {
                    m_object.*m_stringFields[*pName] = string(str, length);
                }
                return true;
            }
            default:
                return this->error(string("String: unexpected state") + to_string(m_state));
        }
    }
    
    bool EndObject(SizeType) {
        switch (m_state) {
            case kExpectNameOrObjectEnd: 
                if(m_jsonFields.size() != m_stringFields.size()) {
                    string e = string("EndObject: missing value(s) for field(s):");
                    auto l = m_jsonFields.end();
                    for (const auto& f : m_stringFields) {
                        const auto& key = f.first;
                        if(find(m_jsonFields.begin(), l, key) == l) { // Not found
                            e += " '";
                            e += key + "'";
                        }
                    }
                    return TBase::error(e); // Not all fields set.
                }
                m_delegate(m_object);
                m_state = kExpectObjectStart;
                return true;
            default:
                return this->error(string("EndObject: unexpected state") + to_string(m_state));
        }
    }

    inline bool Null() { return this->TBase::Null();}
    inline bool Bool(bool b) { return this->TBase::Bool(b);}
    inline bool Int(int i) { return this->TBase::Int(i);}
    inline bool Uint(unsigned i) { return this->TBase::Uint(i);}
    inline bool Int64(int64_t i) { return this->TBase::Int64(i);}
    inline bool Uint64(uint64_t i) { return this->TBase::Uint64(i);}
    inline bool Double(double d) { return this->TBase::Double(d);}
    inline bool RawNumber(const typename TBase::Ch* str, SizeType length, bool copy){ return this->TBase::RawNumber(str, length, copy);}
    inline bool StartArray(){ return this->TBase::StartArray();}
    inline bool EndArray(SizeType elementCount){ return this->TBase::EndArray(elementCount);}

    bool Default() {
        switch (m_state) {
            case kExpectValue:
            {
                // Other than string types are allowed during kExpectValue state
                // Treated as unregistered field.
                m_state = kExpectNameOrObjectEnd;
                m_jsonFields.pop_back();
                return true;
            }
            default:
                return this->error(string("Default: unexpected state") + to_string(m_state));
        }
    }
    
    ParserForObject( TObjectDelegate delegate)
    : m_state(kExpectObjectStart)
    , m_delegate(delegate)
    {}

protected:
    typedef map<string, TStringField> StringFieldsMap;
    typedef vector<string> FieldNames;
private:
    
    StringFieldsMap m_stringFields;
    enum State {
        kExpectObjectStart,
        kExpectNameOrObjectEnd,
        kExpectValue,
    }m_state;
    
    T m_object;
    FieldNames m_jsonFields;
    TObjectDelegate m_delegate;
};

template<class T>
inline bool ParseJsonObject(const char* json, ParserForObject<T>& handler, string* errorMessage = nullptr) {
    Reader reader;
    StringStream ss(json);
    if (!reader.Parse(ss, handler)){
        if(nullptr  != errorMessage) {
            ParseErrorCode e = reader.GetParseErrorCode();
            size_t o = reader.GetErrorOffset();
            *errorMessage += "Parser error: '";
            *errorMessage += GetParseError_En(e);
            *errorMessage += "' at offset ";
            *errorMessage += to_string(o);
            string tail = string(&json[o]);
            if(tail.size() > 0){
               *errorMessage += " near '" + tail.substr(10) +  "...'\n";
            } else  {
                *errorMessage += " (last symbol) \n";
            }
            if(handler.HasError()) {
                *errorMessage += "Object error: '";
                *errorMessage += handler.GetParseError() + "'";
            }
        }
        return false;
    }
    return true;
}



template<class TObjectParser>
struct ParserForObjectArray : public ParserForBase<ParserForObjectArray<TObjectParser> > {
    
private :
    typedef ParserForBase<ParserForObjectArray<TObjectParser> > TBase;

public:
    ParserForObjectArray( TObjectParser objParser)
    : m_state(kExpectStartArray)
    , m_objectParser(objParser)
    {}

    bool StartArray() {
        switch (m_state) {
            case kExpectStartArray:
            {
                m_state = kExpectEndArray;
                return true;
            }
            default:
                return this->error(string("StartArray: unexpected state") + to_string(m_state));
        }
    }
    bool EndArray(SizeType elementCount){
        switch (m_state) {
            case kExpectEndArray:
            {
                m_state = kExpectStartArray;
                return true;
            }
            default:
                return this->error(string("EndArray: unexpected state") + to_string(m_state));
        }
    }

    bool StartObject() { return CallObjectParser(&TObjectParser::StartObject); }
    
    bool String(const char* str, SizeType length, bool copy) {
        return CallObjectParser(&TObjectParser::String, str, length, copy);
    }
    bool Key(const char* str, SizeType length, bool copy) {
        return CallObjectParser(&TObjectParser::Key, str, length, copy);
    }

    bool EndObject(SizeType size){return CallObjectParser(&TObjectParser::EndObject, size);}
    bool Null(){ return CallObjectParser(&TObjectParser::Null); }
    bool Bool(bool b){ return CallObjectParser(&TObjectParser::Bool, b); }
    bool Int(int i){ return CallObjectParser(&TObjectParser::Int, i); }
    bool Uint(unsigned i){ return CallObjectParser(&TObjectParser::Uint, i); }
    bool Int64(int64_t i){ return CallObjectParser(&TObjectParser::Int64, i); }
    bool Uint64(uint64_t i){ return CallObjectParser(&TObjectParser::Uint64, i); }
    bool Double(double d){ return CallObjectParser(&TObjectParser::Double, d); }
    bool RawNumber(const typename TBase::Ch* str, SizeType length, bool copy){
        return CallObjectParser(&TObjectParser::RawNumber, str, length, copy);
    }

    bool Default() {
        return this->error("Default: unknown event for array parser");
    }
private:
    template<class ... Types>
    inline bool CallObjectParser(bool (TObjectParser::*f)(Types...), Types ... args)
    {
        switch (m_state) {
            case kExpectEndArray:
                return (m_objectParser.*f)(args...);
            default:
                return this->error(string("CallObjectParser: unexpected state") + to_string(m_state));
        }
    }
 
    enum State {
        kExpectStartArray,
        kExpectEndArray
    }m_state;
    
    TObjectParser m_objectParser;
};


template<class T>
inline bool ParseJsonArray(const char* json, ParserForObject<T>& handler, string* errorMessage = nullptr) {
    Reader reader;
    StringStream ss(json);
    ParserForObjectArray<ParserForObject<T> > arrayHandler(handler);
    
    if (!reader.Parse(ss, arrayHandler)){
        if(nullptr  != errorMessage) {
            ParseErrorCode e = reader.GetParseErrorCode();
            size_t o = reader.GetErrorOffset();
            *errorMessage += "Parser error: '";
            *errorMessage += GetParseError_En(e);
            *errorMessage += "' at offset ";
            *errorMessage += to_string(o);
            string tail = string(&json[o]);
            if(tail.size() > 0){
               *errorMessage += " near '" + tail.substr(10) +  "...'\n";
            } else  {
                *errorMessage += " (last symbol) \n";
            }
            if(arrayHandler.HasError()) {
                *errorMessage += "Array error: '";
                *errorMessage += arrayHandler.GetParseError() + "'\n";
            }
            if(handler.HasError()) {
                *errorMessage += "Object error: '";
                *errorMessage += handler.GetParseError() + "'";
            }
        }
        return false;
    }
    return true;
}
} // Json
} // Helpers
 
#endif /* JsonSaxHandler_h */
