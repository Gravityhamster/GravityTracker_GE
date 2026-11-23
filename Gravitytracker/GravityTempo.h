#pragma once
#include <algorithm>
#include <atomic>

extern std::atomic<int> bpm;
extern int tps;

// Ticks per second: bpm beats/min * 4 steps/beat * tps ticks/step / 60
inline double GravityTicksPerSecond(int b, int ticks_per_step)
{
    return std::max(1.0, ((double)b * ticks_per_step * 4.0) / 60.0);
}

// Audio frames per tracker tick
inline double GravityFramesPerTick(int sample_rate, int b, int ticks_per_step)
{
    return (double)sample_rate / GravityTicksPerSecond(b, ticks_per_step);
}