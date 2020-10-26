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

public:
    inline bool HasError() const { return m_isError; }
    inline const string& GetParseError() const {return m_error; }

protected:
    typedef BaseReaderHandler<UTF8<>, TDerived>  TBase;

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
struct ObjectDeliverer {
    ObjectDeliverer() {
        m_delegate = [](const T&){return false;};
    }
    typedef function <bool(const T&)> TObjectDelegate;
    inline bool SendObject(const T& obj) { return m_delegate(obj);}
    void SetDelegate(const TObjectDelegate& delegate) {m_delegate = delegate;}
private:
    TObjectDelegate m_delegate;
};



template<class T>
struct ParserForObject : public ParserForBase<ParserForObject<T> > {

private :
    typedef typename ParserForBase<ParserForObject<T> >::TBase TBase;

public:
    typedef typename ObjectDeliverer<T>::TObjectDelegate TObjectDelegate;
    typedef string T::*TStringField;
    typedef vector<string> T::*TStringArrayField;

    inline ParserForObject<T>& WithField(const string& name, TStringField field, bool isMandatory = true)
    {
        m_stringFields.emplace(name, field);
        if(isMandatory)
            m_mandatoryTags.push_back(name);
        return *this;
    }
    
    inline ParserForObject<T>& WithField(const string& name, TStringArrayField arrayField, bool isMandatory = true)
    {
        m_stringArrayFields.emplace(name, arrayField);
        if(isMandatory)
            m_mandatoryTags.push_back(name);
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
            case kExpectArrayValue:
            {
                auto pName = prev(m_jsonFields.end());
               
                if(m_stringArrayFields.count(*pName) == 0){
                    m_jsonFields.erase(pName); // OK. The field was not registerd for parsing
                } else {
                    (m_object.*m_stringArrayFields[*pName]).push_back(string(str, length));
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
            {
                auto l = m_jsonFields.end();
                for (const auto& tagName : m_mandatoryTags) {
                    if(find(m_jsonFields.begin(), l, tagName) == l) { // Not found
                        string e = string("EndObject: missing value(s) for field(s):");
                        e += " '";
                        e += tagName + "'";
                        return this->error(e); // Not all fields set.
                    }
                }
                m_state = kExpectObjectStart;
                return m_deliverer.SendObject(m_object);
            }
            default:
                return this->error(string("EndObject: unexpected state") + to_string(m_state));
        }
    }

    inline bool Null() { return TBase::Null();}
    inline bool Bool(bool b) { return TBase::Bool(b);}
    inline bool Int(int i) { return TBase::Int(i);}
    inline bool Uint(unsigned i) { return TBase::Uint(i);}
    inline bool Int64(int64_t i) { return TBase::Int64(i);}
    inline bool Uint64(uint64_t i) { return TBase::Uint64(i);}
    inline bool Double(double d) { return TBase::Double(d);}
    inline bool RawNumber(const typename TBase::Ch* str, SizeType length, bool copy){ return TBase::RawNumber(str, length, copy);}
    inline bool StartArray() {
        // if we expect value, probably it is array field
        if(kExpectValue == m_state){
            m_state = kExpectArrayValue;
            return true;
        }
        // else - check start of array of objects
        else {
            switch (m_arrayState) {
                case kExpectStartArray:
                {
                    if(kExpectObjectStart != m_state)
                        return this->error(string("StartArray: unexpected object state") + to_string(m_state));
                    m_arrayState = kExpectEndArray;
                    return true;
                }
                default:
                    return this->error(string("StartArray: unexpected array state") + to_string(m_arrayState));
            }
        }
    }
    inline bool EndArray(SizeType elementCount){
        if(kExpectArrayValue == m_state){
            m_state = kExpectNameOrObjectEnd;
            return true;
        }
        switch (m_arrayState) {
            case kExpectEndArray:
            {
                if(kExpectObjectStart != m_state)
                    return this->error(string("EndArray: unexpected object state") + to_string(m_state));
                m_arrayState = kExpectStartArray;
                return true;
            }
            default:
                return this->error(string("EndArray: unexpected array state") + to_string(m_arrayState));
        }
    }

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
    
    ParserForObject()
    : m_state(kExpectObjectStart)
    , m_arrayState(kExpectStartArray)
    {}

protected:
    typedef map<string, TStringField> StringFieldsMap;
    typedef map<string, TStringArrayField> StringArrayFieldsMap;
    typedef vector<string> FieldNames;
    void SetDeliverer(const ObjectDeliverer<T>& deliverer) {m_deliverer = deliverer;}
    
private:
    template<class U>
    friend bool ParseJsonStream(const char* json,
                                   ParserForObject<U>& handler,
                                   typename ParserForObject<U>::TObjectDelegate onObjectReady,
                                   string* errorMessage);
//    template<class U>
//    friend bool ParseJsonArray(const char* json, ParserForObject<U>& handler,
//                               typename ParserForObject<U>::TObjectDelegate onObjectReady,
//                               string* errorMessage);
    
    StringFieldsMap m_stringFields;
    StringArrayFieldsMap m_stringArrayFields;

    enum State {
        kExpectObjectStart,
        kExpectNameOrObjectEnd,
        kExpectValue,
        kExpectArrayValue
    }m_state;
    
    enum ArrayState {
        kExpectStartArray,
        kExpectEndArray
    }m_arrayState;
    
    T m_object;
    FieldNames m_jsonFields;
    FieldNames m_mandatoryTags;
    ObjectDeliverer<T> m_deliverer;
};

template<class TArray, class TElement>
struct ParserForArrayObject : public ParserForObject<TElement> {
private:
    typedef ParserForObject<TElement> TBase;
public:
    typedef function<bool(TArray&, const TElement&)> TObjectDelegate;
    ParserForArrayObject(TObjectDelegate delegate)
    : TBase()
    , m_delegate (delegate)
    {}
private:
    TObjectDelegate m_delegate;
};

template<class T>
inline bool ParseJsonStream(const char* json, ParserForObject<T>& handler,
                            typename ParserForObject<T>::TObjectDelegate onObjectReady,
                            string* errorMessage) {
    ObjectDeliverer<T> deliverer;
    deliverer.SetDelegate(onObjectReady);
    handler.SetDeliverer(deliverer);
    
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

} // Json
} // Helpers
 
#endif /* JsonSaxHandler_h */
