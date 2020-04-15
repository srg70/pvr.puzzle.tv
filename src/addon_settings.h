/*
 *
 *   Copyright (C) 2020 Sergey Shramchenko
 *   https://github.com/srg70/pvr.puzzle.tv
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

#ifndef addon_settings_h
#define addon_settings_h

#include <map>
#include <string>
#include <memory>
#include <tuple>
#include <functional>
#include <utility>
#include <sstream>
#include "globals.hpp"

namespace PvrClient {


template<typename T>
class AddonSetting {
public:
    typedef std::function<void(const T&)> TPropagator;
    AddonSetting(const std::string& n, const T& d = T{},  TPropagator f = &DefaultPropagator)
    : name(n), propagator(f) {SetValue(d);}
    const std::string name;
    const T& Value() const {return value;}
    void SetValue(const T& v) {value = v; propagator(v);}
private:
    static void DefaultPropagator (const T&) {}
    T value;
    const TPropagator propagator;
};


template<typename... Tp>
class AddonSettings {
public:
    typedef std::tuple<Tp...> container_type;
    
    AddonSettings(){}
    AddonSettings(const container_type& tuple) :settings(tuple) {}
    AddonSettings(container_type&& tuple) :settings(tuple) {}
    template <typename T>
    AddonSettings<Tp..., AddonSetting<T> > AddSetting(const AddonSetting<T>& s)
    {
        AddonSettings<Tp..., AddonSetting<T> > as (std::tuple_cat(settings, std::make_tuple(s)));
        return as;

    }
    template <typename T>
    AddonSettings<Tp..., AddonSetting<T> > AddSetting(const std::string& n, const T& defaultV)
    {
        return AddSetting(AddonSetting<T>(n, defaultV));
    }
    template <typename T>
    AddonSettings<Tp..., AddonSetting<T> > AddSetting(const std::string& n, const T& defaultV, typename AddonSetting<T>::TPropagator p)
    {
        return AddSetting(AddonSetting<T>(n, defaultV, p));
    }

//private:
    container_type settings;
};


// for_each

template<std::size_t I = 0, typename FuncT, typename... Tp>
inline typename std::enable_if<I == sizeof...(Tp), void>::type
for_each(std::tuple<Tp...> &, FuncT) // Unused arguments are given no names.
{ }

template<std::size_t I = 0, typename FuncT, typename... Tp>
inline typename std::enable_if<I < sizeof...(Tp), void>::type
for_each(std::tuple<Tp...>& t, FuncT f)
{
    f(std::get<I>(t));
    for_each<I + 1, FuncT, Tp...>(t, f);
}

// first_if

template<std::size_t I = 0, typename PredT, typename FuncT, typename... Tp>
inline typename std::enable_if<I == sizeof...(Tp), bool>::type
first_if(std::tuple<Tp...> &, PredT&&, FuncT&&) // Unused arguments are given no names.
{ return false;}

template<std::size_t I = 0, typename PredT, typename FuncT, typename... Tp>
inline typename std::enable_if<I < sizeof...(Tp), bool>::type
first_if(std::tuple<Tp...>& t, PredT&& p, FuncT&& f)
{
    auto& ref = std::get<I>(t);
    if(p(ref)) {
        f(ref);
        return true;
    }
    return first_if<I + 1, PredT, FuncT, Tp...>(t, std::forward<PredT>(p), std::forward<FuncT>(f));
}



struct SettingsAdaptor{
public:
    template<typename... Tp>
    SettingsAdaptor(const AddonSettings<Tp...>& ss)
    : init(std::bind(&SettingsAdaptor::Init<Tp...>, this))
    , print(std::bind(&SettingsAdaptor::Print<Tp...>, this))
    , set(std::bind(&SettingsAdaptor::Set<Tp...>, this,  std::placeholders::_1,  std::placeholders::_2))
    , get_int(std::bind(&SettingsAdaptor::Get<int, Tp...>, this,  std::placeholders::_1))
    , destroy(std::bind(&SettingsAdaptor::Destroy<Tp...>, this))
    {
        settings_holder = new typename AddonSettings<Tp...>::container_type(ss.settings);
    }
    ~SettingsAdaptor() { destroy();}
    
    const std::function<void()> init;
    const std::function<void()> print;
    const std::function<void(const char*, const void*)> set;
    const std::function<int(const char*)> get_int;
private:
    void* settings_holder;
    const std::function<void()> destroy;

    struct InitFromGlobals
    {
        
        template<typename T>
        void operator()(T& t) const;
        template<typename T>
        void operator()(AddonSetting<T> & t) const
        {
            T v;
            Globals::XBMC->GetSetting(t.name.c_str(), &v);
            t.SetValue(v);
        }
        template<>
        void operator()(AddonSetting<std::string> & t) const
        {
            char v[1024];
            Globals::XBMC->GetSetting(t.name.c_str(), &v);
            t.SetValue(v);
        }
    };

    struct SettingComparator
    {
        SettingComparator(const char* settingName) : name (settingName) {}
            
        template<typename T>
        bool operator()(const T& t) const;
        template<typename T>
        bool operator()(const AddonSetting<T> & t) const
        {
            return t.name == name;
        }
            
        const std:: string name;
            
    };

    struct SetSettingsValue
    {
        SetSettingsValue(const void* newSettingValue) : value(newSettingValue) {}
        template<typename T>
        void operator()(T& t) const;
        template<typename T>
        void operator()(AddonSetting<T> & t) const
        {
            t.SetValue(*reinterpret_cast<const T*>(value));
        }
        template<>
        void operator()(AddonSetting<std::string> & t) const
        {
            t.SetValue((reinterpret_cast<const char*>(value)));
        }
        const void * value;
    };

    template<typename T>
    struct GetSettingsValue
    {
        template<typename U>
        void operator()(U& t)
        {
            std::string errorStr("Wrong setting's value type.");
            Globals::LogError(errorStr.c_str());
            throw std::invalid_argument(errorStr);
        }
        template<>
        void operator()(AddonSetting<T> & t)
        {
            value = t.Value();
        }
        T value;
    };

    struct PrintSetting
    {
        
        template<typename T>
        void operator()(T& t) const;
        
        template<typename T>
        void operator()(AddonSetting<T> & t) const
        {
            std::stringstream ss;
            ss << "S[ " << t.name << " ] = " << t.Value();
            Globals::LogDebug(ss.str().c_str());
        }
    };
    template<typename... Tp>
    void Init(){
        typename AddonSettings<Tp...>::container_type* pSs = reinterpret_cast<typename AddonSettings<Tp...>::container_type*>(settings_holder);
        for_each(*pSs, InitFromGlobals());
    }
    template<typename... Tp>
    void Print(){
        typename AddonSettings<Tp...>::container_type* pSs = reinterpret_cast<typename AddonSettings<Tp...>::container_type*>(settings_holder);
        for_each(*pSs, PrintSetting());
    }
    template<typename... Tp>
    void Set(const char* name, const void* value){
        typename AddonSettings<Tp...>::container_type* pSs = reinterpret_cast<typename AddonSettings<Tp...>::container_type*>(settings_holder);
        
        if(!first_if(*pSs, SettingComparator(name), SetSettingsValue(value))){
            std::string errorStr = "Setting ";
            errorStr += name;
            errorStr += " not found.";
            Globals::LogError(errorStr.c_str());
            throw std::invalid_argument(errorStr);
        }
    }
    template<typename T, typename... Tp>
    T Get(const char* name){
        typename AddonSettings<Tp...>::container_type* pSs = reinterpret_cast<typename AddonSettings<Tp...>::container_type*>(settings_holder);
        
        GetSettingsValue<T> v;
        if(!first_if(*pSs, SettingComparator(name), v)){
            std::string errorStr = "Setting ";
            errorStr += name;
            errorStr += " not found.";
            Globals::LogError(errorStr.c_str());
            throw std::invalid_argument(errorStr);
        }
        return v.value;
    }
    template<typename... Tp>
    void Destroy(){
        typename AddonSettings<Tp...>::container_type* pSs = reinterpret_cast<typename AddonSettings<Tp...>::container_type*>(settings_holder);
        delete pSs;
        settings_holder = nullptr;
    }
};


}

#endif //addon_settings_h
