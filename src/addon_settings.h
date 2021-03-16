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

// for_each

template<std::size_t I = 0, typename FuncT, typename... Tp>
inline typename std::enable_if<I == sizeof...(Tp), void>::type
for_each(const std::tuple<Tp...> &, FuncT) // Unused arguments are given no names.
{ }

template<std::size_t I = 0, typename FuncT, typename... Tp>
inline typename std::enable_if<I < sizeof...(Tp), void>::type
for_each(std::tuple<Tp...>& t, FuncT f)
{
    auto& ref = std::get<I>(t);
    f(ref);
    for_each<I + 1, FuncT, Tp...>(t, f);
}

// first_if

template<std::size_t I = 0, typename PredT, typename FuncT, typename... Tp>
inline typename std::enable_if<I == sizeof...(Tp), bool>::type
first_if(const std::tuple<Tp...> &, PredT&&, FuncT&&) // Unused arguments are given no names.
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


class AddonSettingsDictionary {

protected:
    template<typename T>
    class AddonSetting {
    public:
        typedef std::function<void(const T&)> TPropagator;
        typedef std::function<void(std::function<const T&()>)> TOnValueSet;

        AddonSetting(const std::string& n, const T& d = T{}, ADDON_STATUS s = ADDON_STATUS_OK, TOnValueSet onSet = &DefaultOnValueSet, TPropagator f = &DefaultPropagator)
        : name(n), defaultValue(d), statusWhenChanged(s), onValueSet(onSet), propagator(f)
        {
            Init(defaultValue);
        }
        AddonSetting(const AddonSetting& as)
        : name(as.name), defaultValue(as.defaultValue), statusWhenChanged(as.statusWhenChanged)
        , onValueSet(as.onValueSet), propagator(as.propagator)
        {
            Init(as.Value());
        }
        const std::string name;
        inline const T& Value() const {return value;}
        inline void Init(const T& v)
        {
            if(v == T{})
                value = defaultValue;
            else
                value = v;
            propagator(value);
        }
        inline ADDON_STATUS SetValue(const T& v)
        {
            if(v == value)
                return ADDON_STATUS_OK;
            Init(v);
            onValueSet([v]{return v;});
            return statusWhenChanged;
        }
        static void DefaultPropagator (const T&) {}
        static void DefaultOnValueSet (std::function<const T&()>) {}
    private:

        T value;
        T defaultValue;
        const ADDON_STATUS statusWhenChanged;
        const TOnValueSet onValueSet;
        const TPropagator propagator;
    };
   
private:

    template<typename T>
    struct Types{
        typedef std::map<std::string, AddonSetting<T> > Map;
    };
    
    template<typename T>
    struct TypePredicate
    {
        template<typename U>
        bool operator()(const U& ) const { return false;}
        //template<>
        bool operator()(const typename Types<T>::Map&) const { return true;}
    };
    
    template<typename T>
    struct SettingMapAction
    {
        typedef std::function<void(typename Types<T>::Map&)> TFunc;
        
        SettingMapAction(TFunc f) : func(f) {}

        template<typename U>
        void operator()(U& t) const {
            std::string errorStr = "Incompatable container for setting type.";
            Globals::LogError(errorStr.c_str());
            throw std::logic_error(errorStr);
        }
        //template<>
        void operator()(typename Types<T>::Map& m) const
        {
            func(m);
        }
        TFunc func;
    };

    struct SetSettingsValue
    {
        struct Found {
            Found(const ADDON_STATUS& s) : status (s) {}
            const ADDON_STATUS status;
        };
        
        SetSettingsValue(const std::string& n, const kodi::CSettingValue& v)
        : name(n), value(v) {}
        
        template<typename T>
        void operator()( std::map<std::string, AddonSetting<T> >& m) const;
        
        void operator()( std::map<std::string, AddonSetting<int> >& m) const
        {
            if(m.count(name) == 0)
                return;
            throw Found(m.at(name).SetValue(value.GetInt()));
        }
        void operator()( std::map<std::string, AddonSetting<bool> >& m) const
        {
            if(m.count(name) == 0)
                return;
            throw Found(m.at(name).SetValue(value.GetBoolean()));
        }
        void operator()( std::map<std::string, AddonSetting<float> >& m) const
        {
            if(m.count(name) == 0)
                return;
            throw Found(m.at(name).SetValue(value.GetFloat()));
        }
        //template<>
        void operator()(std::map<std::string, AddonSetting<std::string> >& m) const
        {
            if(m.count(name) == 0)
                return;
            throw Found(m.at(name).SetValue(value.GetString()));
        }
        const std::string name;
        const kodi::CSettingValue& value;
    };

    template <typename TFunc>
    struct ForEachAction
    {
        ForEachAction(TFunc f = TFunc()) : func(f) {}
        template<typename T>
        void operator()(T& m) const
        {
            for(auto& as : m){
                func(as.second);
            }
        }

    protected:
        TFunc func;
    };

    struct PrintSetting
    {
        template<typename T>
        void operator()(const AddonSetting<T> & t) const
        {
            std::stringstream ss;
            const std::string& name = t.name;
            ss << "S[ " << name << " ] = ";
            if(name.find("password") == std::string::npos && name.find("login") == std::string::npos)
                ss << t.Value();
            else
                ss << "*****";
            Globals::LogDebug(ss.str().c_str());
        }
    };
    
    struct InitFromGlobals
    {
        template<typename T>
        void operator()(AddonSetting<T> & t) const;

        void operator()(AddonSetting<std::string> & t) const
        {
            std::string v = kodi::GetSettingString(t.name);
            t.Init(v);
        }
        void operator()(AddonSetting<int> & t) const
        {
            int v = kodi::GetSettingInt(t.name);
            t.Init(v);
        }
        void operator()(AddonSetting<bool> & t) const
        {
            bool v = kodi::GetSettingBoolean(t.name);
            t.Init(v);
        }
        void operator()(AddonSetting<float> & t) const
        {
            float v = kodi::GetSettingFloat(t.name);
            t.Init(v);
        }
    };

    template <typename T>
    void PerformTypedAction(typename SettingMapAction<T>::TFunc func) const
    {
        if(!first_if(settings, TypePredicate<T>(), SettingMapAction<T>(func))){
            std::string errorStr = "Setting of unsuported type.";
            Globals::LogError(errorStr.c_str());
            throw std::invalid_argument(errorStr);
        }

    }
    
    template <typename T>
    const T& Get(const std::string& n) const
    {
        const T* p = nullptr;
        PerformTypedAction<T>([n, &p](typename Types<T>::Map& m){
            p = &m.at(n).Value();
        });
        return *p;
    }

protected:
    template <typename T>
    void AddSetting(const AddonSetting<T>& s)
    {
        PerformTypedAction<T>([s](typename Types<T>::Map& m){
            m.emplace(s.name, s);
        });
    }

public:
    void Print() const
    {
        for_each(settings, ForEachAction<PrintSetting>());
    }
    
    void Init()
    {
        for_each(settings, ForEachAction<InitFromGlobals>());
    }

//    template <typename T>
//    void Set(const std::string& n, const T& v)
//    {
//        PerformTypedAction<T>([n, v](typename Types<T>::Map& m){
//            m.at(n).SetValue(v);
//        });
//    }
    
    ADDON_STATUS Set(const std::string& n, const kodi::CSettingValue& v)
    {
        try{
            for_each(settings, SetSettingsValue(n, v));
            std::string errorStr = " Unknown setting ";
            errorStr += n;
            errorStr += " can't be set.";
            Globals::LogError(errorStr.c_str());
            throw std::invalid_argument(errorStr);
        }
        catch(SetSettingsValue::Found& found){
            return found.status;
        }
    }

    const std::string& GetString(const std::string& n) const { return Get<std::string>(n);}
    int GetInt(const std::string& n) const { return Get<int>(n);}
    bool GetBool(const std::string& n) const { return Get<bool>(n);}
    float GetFloat(const std::string& n) const { return Get<float>(n);}
private:
    mutable std::tuple <
    typename Types<std::string>::Map,
    typename Types<int>::Map,
    typename Types<bool>::Map,
    typename Types<float>::Map
    > settings;
};

class AddonSettingsMutableDictionary : public AddonSettingsDictionary
{
public:
    template <typename T>
    AddonSettingsMutableDictionary& Add(const std::string& n, const T& defaultV, ADDON_STATUS onChanged = ADDON_STATUS_OK)
    {
        AddSetting(AddonSetting<T>(n, defaultV, onChanged));
        return *this;
    }
    template <typename T>
    AddonSettingsMutableDictionary& Add(const std::string& n, const T& defaultV,
                                        typename AddonSetting<T>::TOnValueSet onSet,
                                        ADDON_STATUS onChanged = ADDON_STATUS_OK)
    {
        AddSetting(AddonSetting<T>(n, defaultV, onChanged, onSet));
        return *this;
    }
    template <typename T>
    AddonSettingsMutableDictionary& Add(const std::string& n, const T& defaultV,
                                        typename AddonSetting<T>::TPropagator p,
                                        ADDON_STATUS onChanged = ADDON_STATUS_OK)
    {
        AddSetting(AddonSetting<T>(n, defaultV, onChanged, &AddonSetting<T>::DefaultOnValueSet, p));
        return *this;
    }

    AddonSettingsMutableDictionary& Add(const std::string& n, const char* defaultV, ADDON_STATUS onChanged = ADDON_STATUS_OK)
    {
        AddSetting(AddonSetting<std::string>(n, defaultV, onChanged));
        return *this;
    }
    AddonSettingsMutableDictionary& Add(const std::string& n, const char* defaultV,
                                        typename AddonSetting<std::string>::TOnValueSet onSet,
                                        ADDON_STATUS onChanged = ADDON_STATUS_OK)
    {
        AddSetting(AddonSetting<std::string>(n, defaultV, onChanged, onSet));
        return *this;
    }
    AddonSettingsMutableDictionary& Add(const std::string& n, const char* defaultV,
                                        typename AddonSetting<std::string>::TPropagator p,
                                        ADDON_STATUS onChanged = ADDON_STATUS_OK)
    {
        AddSetting(AddonSetting<std::string>(n, defaultV, onChanged, &AddonSetting<std::string>::DefaultOnValueSet, p));
        return *this;
    }

};

}

#endif //addon_settings_h
