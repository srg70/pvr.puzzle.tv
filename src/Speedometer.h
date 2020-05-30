//
//  Speedometer.h
//  comple.test
//
//  Created by Sergey Shramchenko on 24/05/2020.
//  Copyright © 2020 Home. All rights reserved.
//

#ifndef Speedometer_h
#define Speedometer_h

#include <stdint.h>
#include <chrono>
#include <queue>

namespace Helpers {

class Speedometer
{
public:
    Speedometer(std::uint32_t dataSizeToMesure)
    : m_dataLimit (dataSizeToMesure)
    {
        Start();
    }
    void Start() {
        while(!m_steps.empty()) m_steps.pop();
        m_totalBytes = 0;
        m_totalSec = 0.0;
        m_stepStart = std::chrono::system_clock::now();
    }
    
    void StartStep()
    {
        m_stepStart = std::chrono::system_clock::now();
        
    }
    void StepDone(ssize_t chunkSize)
    {
        m_steps.emplace(m_stepStart, std::chrono::system_clock::now(), chunkSize);
        const auto& last = m_steps.back();
        m_totalBytes += last.howMany;
        m_totalSec += last.Seconds();
        m_stepStart = last.to;
        while(m_totalBytes > m_dataLimit) {
            const auto& first = m_steps.front();
            m_totalBytes -= first.howMany;
            m_totalSec -= first.Seconds();
            m_steps.pop();
        }
    }
    
    uint32_t BytesPerSecond() const { return m_totalBytes / m_totalSec; }
    float KBytesPerSecond() const { return m_totalBytes / m_totalSec / 1024; }
    float MBytesPerSecond() const { return m_totalBytes / m_totalSec / 1024 / 1024 ; }
private:
    typedef std::chrono::time_point<std::chrono::system_clock> time_point;
    struct StepInfo
    {
        StepInfo(const time_point& f, const time_point& t, ssize_t d)
        : from(f), to(t), howMany(d)
        {}
        StepInfo(StepInfo && other)
        : from(other.from), to(other.to), howMany(other.howMany)
        {}
        inline float Seconds() const
        {
            std::chrono::duration<float> diff = to - from;
            return diff.count();
        }
        const time_point from;
        const time_point to;
        const ssize_t howMany;
    };
    std::queue<StepInfo> m_steps;
    const std::uint32_t m_dataLimit;
    uint32_t m_totalBytes;
    float m_totalSec;
    time_point m_stepStart;
    
};

} //Helpers

#endif /* Speedometer_h */
