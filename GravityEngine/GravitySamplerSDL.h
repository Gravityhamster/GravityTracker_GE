#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <iostream>
#include <chrono>
#include <math.h>
#include <future>
#include <thread>
#include <vector>
#include <queue>
#include <string>
#include <unordered_map>
#include <random>
#include <map>
#include <climits>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdint>
#include "SPSC_Queue.h"
#include "GravityEngineTypes.h"

// For flushing denormal floats to zero (x86/x64 only)
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#define GRAVITY_HAS_SSE_CSR 1
#endif

#define PI 3.14159265358979323846f

// Set to 0 to silence the sampler's once-every-2-seconds timing diagnostics
#ifndef SAMPLER_DIAG
#define SAMPLER_DIAG 1
#endif

// TODO: Any effects related directly to instrument automation should be implemented directly into the sampler (i.e. vibrato, pitchsweep, fadein, fadeout, etc.)

// Chamerblain filter processing - COPILOT function implemented into a sequestored function
// NOTE: GenerateAudio no longer calls this (the coefficients are computed once per buffer and the
// three filter lines are inlined in the loop). It is kept here in case anything else uses it.
// float sample : Current decimal audio position
// float cutoff : 0 to 1 freq filter cutoff 
// float resonance : 0 to 1 resonance frequency
// float sample_rate_freq : Audio sample rate (e.g. 48000hz)
// float* lp : Pointer to the lowpass filter state variable
// float* bp : Pointer to the bandpass filter state variable
// float* hp : Pointer to the highpass filter state variable
inline void SamplerProcessChamberlainFilter(float sample, float cutoff, float resonance, float sample_rate_freq, float* lp, float* bp, float* hp)
{
    // Apply filter
    float warped = cutoff * cutoff * cutoff;
    float cutoff_hz = warped * (sample_rate_freq * 0.5f);
    float f = std::clamp(2.0f * sinf(PI * cutoff_hz / sample_rate_freq), 0.f, 0.999f);
    float q = std::clamp(1.0f - resonance, 0.05f, 1.f);

    // Calculate filter
    (*hp) = sample - (*lp) - q * (*bp);
    (*bp) = (*bp) + f * (*hp);
    (*lp) = (*lp) + f * (*bp);

    // Dampen output to prevent feedback looping
    (*hp) *= 0.999f;
    (*bp) *= 0.999f;
    (*lp) *= 0.999f;
}

// Template for sampler objects
class GravityEngine_Sampler
{

    // sampler parameters
public:

    std::vector<GravityEngine_Voice*> voices;
    int sample_frames;

    // How many buffers to keep queued in the stream. 1 = lowest latency, but only one buffer of
    // slack before an underrun. 2 = about 10 ms more latency (at 480 frames) but survives a late wake-up.
    static constexpr int kBuffersAhead = 2;

    // Row/tick gate. The tracker holds this while it writes ALL the voice changes for one tick
    // (stg_* values, live_changes pushes, will_start_playing flags), e.g. at the top of DoTick():
    //     std::lock_guard<std::recursive_mutex> lock(audiosampler->batch_mutex);
    // It is recursive so entry points can nest (DoTick -> step -> PlayStepPhrase).
    // The audio thread only ever try_locks it, so it never waits on the tracker.
    // NOTE: will_stop_playing is deliberately NOT gated. The tracker blocks until the audio thread
    // clears that flag, so the audio thread must be able to handle it while the tracker holds the gate.
    std::recursive_mutex batch_mutex;

    // Total frames pushed to the stream so far. The tracker can use this as an audio-driven clock
    // instead of wall-clock time, so ticks can't drift against the sample clock.
    std::atomic<int64_t> frames_rendered = 0;

    // time : Milliseconds to convert to bytes
    // audio_spec : Channel audio spec
    static size_t milliseconds_to_bytes(long time, SDL_AudioSpec audio_spec)
    {
        // Bytes in every sample
        double bytes_per_sample = SDL_AUDIO_BITSIZE(audio_spec.format) / 8.0 * audio_spec.channels;
        // Samples in ever second
        double samples_per_second = audio_spec.freq;
        // Bytes per second
        double bytes_per_second = bytes_per_sample * samples_per_second;
        // Bytes per millisecond
        return (size_t)floor(time * (bytes_per_second / 1000.0));
    }

    // Claude - Milliseconds to (fractional) frames
    static double milliseconds_to_frames(long time, const SDL_AudioSpec& audio_spec)
    {
        return time * (audio_spec.freq / 1000.0);
    }

    // Claude - Playback speed needed to turn a sample recorded at base_freq into freq
    static double PlaybackRate(float freq, float base_freq)
    {
        if (base_freq <= 0.f)
            return 1.0; // no root pitch set, play as recorded
        return std::clamp((double)freq / (double)base_freq, 0.0, 16.0);
    }

    // Mostly Claude reworked
    // Reads a sample from the sound at a fractional frame position, using linear interpolation.
    // GravityEngine_sampler* sampler : sampler object reference
    // SDL_AudioStream* stream : Audio stream that the sampler audio plays on
    // SDL_AudioSpec* spec : Audio spec to format the audio with
    // SDL_AudioDeviceID dev : Device the audio will play on
    // ChannelStates* state : Current state of the channel
    // bool* sampler_playing : Flag to indicate the thread has successfully finished
    static void GenerateAudio(GravityEngine_Sampler* sampler, SDL_AudioStream* stream, SDL_AudioSpec* spec, SDL_AudioDeviceID dev, std::atomic<ChannelStates>* state, std::atomic<bool>* sampler_playing)
    {
#ifdef GRAVITY_HAS_SSE_CSR
        // Flush denormals to zero on this thread (decaying filters can otherwise get very slow)
        _mm_setcsr(_mm_getcsr() | 0x8040);
#endif

        // Ask the OS to schedule this thread promptly. A late wake-up of even a few ms is audible.
        SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);

        bool first = true;

        // Get sample frames
        SDL_GetAudioDeviceFormat(dev, spec, &sampler->sample_frames);
        if (spec->channels > 2)
            spec->channels = 2;

        const int channels = spec->channels;
        const int buffer_frames = sampler->sample_frames;
        const int bytes_per_sample = SDL_AUDIO_BITSIZE(spec->format) / 8;
        // Render in small blocks (a quarter of a device buffer). Tracker updates (note starts, queued
        // changes) are applied once per block, so onset jitter is ~2.5 ms instead of ~10 ms.
        // The amount of audio kept queued in the stream is unchanged (kBuffersAhead device buffers).
        const int block_frames = std::max(1, buffer_frames / 4);
        const int buffer_size = block_frames * channels * bytes_per_sample; // bytes per BLOCK
        const size_t frame_bytes = sizeof(float) * channels;
        std::vector<Uint8> buffer(buffer_size);

        // Diagnostics (printed every ~2 s when SAMPLER_DIAG is on)
        uint64_t diag_blocks = 0, diag_gate_skips = 0, diag_empty_pops = 0, diag_underruns = 0;
        double diag_max_ms = 0.0;
        auto diag_last = std::chrono::steady_clock::now();

        SDL_SetAudioStreamGain(stream, 1.0f);

        while ((*state) == playing || (*state) == paused) {

            // Stops are handled every iteration, outside the gate: the tracker busy-waits on this flag
            for (auto v : sampler->voices)
            {
                if (v->will_stop_playing)
                {
                    while (v->live_changes.pop()) {}
                    v->start_playing = false;
                    v->will_stop_playing = false;
                }
            }

            if ((*state) == paused)
            {
                std::this_thread::yield();
                continue;
            }

            int threshold_frames = sampler->sample_frames * kBuffersAhead;
            int available_frames = SDL_GetAudioStreamAvailable(stream) / (sizeof(float) * channels);

            // Stream completely drained after playback began = the device ran dry (audible gap)
            if (!first && available_frames == 0)
                ++diag_underruns;

            if (available_frames < threshold_frames)
            {
                // -= Timing: how long does it take to render one buffer? =-
                auto t0 = std::chrono::high_resolution_clock::now();

                SDL_memset(buffer.data(), 0, buffer_size);

                float* out = (float*)buffer.data();

                // -= Phase 1: apply tracker updates (stops, starts, queued changes) =-
                // The tracker holds batch_mutex while it writes a whole row/tick for all voices.
                // try_lock never blocks the audio thread: if the tracker is mid-write we apply nothing
                // this buffer and pick the whole row up together on the next one. A row can therefore
                // never be split across two buffers (which sounded like a flam / doubled note).
                {
                    std::unique_lock<std::recursive_mutex> batch_lock(sampler->batch_mutex, std::try_to_lock);
                    const bool can_apply = batch_lock.owns_lock();
                    if (!can_apply) ++diag_gate_skips;

                    for (auto v : sampler->voices)
                    {
                        if (can_apply)
                        {
                            // Voice was just triggered: initialise its state
                            if (v->will_start_playing)
                            {
                                v->freq = v->stg_step_freq.load();
                                v->volume = v->stg_volume.load();
                                v->cutoff = v->stg_cutoff.load();
                                v->resonance = v->stg_resonance.load();
                                v->panning = v->stg_panning.load();

                                v->panning = std::clamp<float>(v->panning, 0.f, 1.f);

                                v->hp_l = v->bp_l = v->lp_l = 0.0f;
                                v->hp_r = v->bp_r = v->lp_r = 0.0f;

                                v->read_pos = v->start_time_ms != -1
                                    ? milliseconds_to_frames(v->start_time_ms, *spec)
                                    : 0.0;

                                // Start at the right speed so there's no pitch glide into the note
                                v->cur_rate = PlaybackRate(v->freq.load(), v->base_freq.load());

                                // The tracker primes frame_counter to one full tick before starting a
                                // note so the first queued change is applied straight away. Keep that.
                                v->tick_acc = (double)v->frame_counter.load();

                                v->will_start_playing = false;
                                v->start_playing = true;
                            }
                        }

                        if (!v->start_playing || !v->sound)
                            continue;

                        // -= Apply queued parameter changes =-
                        // Runs once per buffer. The accumulator is fractional, so ticks stay on the
                        // tracker's exact rate. If we couldn't take the gate this buffer, the clock
                        // still advances and the missed ticks are applied together next buffer.
                        const double fpt = v->frames_per_tick.load();
                        if (fpt > 0.0)
                        {
                            const double tick_len = std::max(fpt, 1.0);
                            v->tick_acc += block_frames;

                            while (can_apply && v->tick_acc >= tick_len)
                            {
                                live_change change;
                                if (!v->live_changes.pop(change))
                                {
                                    // The tracker hasn't delivered this tick's change yet (its wall-clock
                                    // tick ran a little late). Keep the tick pending so the change is
                                    // applied the moment it arrives. Consuming the tick here would leave
                                    // this voice's automation one tick behind for the rest of the note.
                                    ++diag_empty_pops;
                                    v->tick_acc = std::min(v->tick_acc, tick_len * 2.0);
                                    break;
                                }
                                v->tick_acc -= tick_len;

                                if (change.freq != -9999) v->freq = change.freq;
                                if (change.pan != -2)     v->panning = change.pan;
                                if (change.vol != -1)     v->volume = change.vol;
                                if (change.ctf != -1)     v->cutoff = change.ctf;
                                if (change.res != -1)     v->resonance = change.res;

                                // Dec will_stop_timer
                                if (v->will_stop_timer == 0)
                                {
                                    v->will_stop_playing = true;
                                    v->will_start_playing = false;
                                    v->start_playing = false;
                                }
                                if (v->will_stop_timer > -1)
                                    v->will_stop_timer--;
                            }
                        }
                    }
                }

                // -= Phase 2: render every playing voice =-
                for (auto v : sampler->voices)
                {
                    if (!v->start_playing || !v->sound)
                        continue;

                    // -= Volume and panning gains for this buffer =-
                    const float vol = v->volume.load();
                    const float pan = std::clamp<float>(v->panning.load(), 0.f, 1.f);

                    float gains[2];
                    if (channels == 2)
                    {
                        const float angle = pan * (PI / 2.0f);
                        gains[0] = std::cos(angle) * vol;
                        gains[1] = std::sin(angle) * vol;
                    }
                    else
                    {
                        gains[0] = (1.0f - std::abs(0.5f - pan)) * vol;
                        gains[1] = gains[0];
                    }

                    // -= Region of the sample that can be played (in frames) =-
                    const auto& audio = v->sound->converted_audio;
                    const float* src = (const float*)audio.data();
                    const int64_t total_frames = (int64_t)(audio.size() / frame_bytes);

                    int64_t end_frame = total_frames;
                    if (v->end_time_ms != -1)
                        end_frame = std::min<int64_t>(total_frames, (int64_t)milliseconds_to_frames(v->end_time_ms, *spec));

                    const int64_t loop_start = (int64_t)milliseconds_to_frames(v->mid_time_ms, *spec);
                    // Only loop if the loop point is before the end
                    const bool can_loop = v->looping && loop_start < end_frame;

                    // -= Playback speed, ramped across the buffer to avoid pitch stepping =-
                    const double rate_start = v->cur_rate;
                    const double rate_end = PlaybackRate(v->freq.load(), v->base_freq.load());

                    // Rate ramps linearly across the buffer: add a step instead of dividing every frame
                    const double rate_step = (rate_end - rate_start) / block_frames;
                    double rate = rate_start;

                    double pos = v->read_pos;

                    // -= Filter setup =-
                    // Coefficients only depend on cutoff/resonance, so compute them once per buffer
                    // instead of once per sample (this was two sinf calls per frame before).
                    const FilterType filter_type = v->filter;
                    const bool use_filter = (filter_type != FilterType::none) &&
                        (v->algorithm == FilterAlgorithm::chamberlain);
                    float f_coef = 0.f, q_coef = 0.f;
                    if (use_filter)
                    {
                        const float warped = v->cutoff * v->cutoff * v->cutoff;
                        const float cutoff_hz = warped * ((float)spec->freq * 0.5f);
                        f_coef = std::clamp(2.0f * sinf(PI * cutoff_hz / (float)spec->freq), 0.f, 0.999f);
                        q_coef = std::clamp(1.0f - v->resonance, 0.05f, 1.f);
                    }

                    // Filter state is copied into locals for the loop and written back afterwards,
                    // so it carries over between buffers.
                    float lp[2] = { v->lp_l, v->lp_r };
                    float bp[2] = { v->bp_l, v->bp_r };
                    float hp[2] = { v->hp_l, v->hp_r };

                    for (int i = 0; i < block_frames; i++)
                    {
                        // Past the end of the playable region?
                        if (pos >= (double)end_frame)
                        {
                            if (can_loop)
                            {
                                // fmod keeps the fractional part, so looping doesn't drift in pitch
                                pos = loop_start + std::fmod(pos - loop_start, (double)(end_frame - loop_start));
                            }
                            else
                            {
                                v->start_playing = false;
                                pos = 0.0;
                                break;
                            }
                        }

                        // Linear interpolation between the two neighbouring frames
                        const int64_t i0 = (int64_t)pos;
                        int64_t i1 = i0 + 1;
                        if (i1 >= end_frame)
                            i1 = can_loop ? loop_start : i0;
                        const float frac = (float)(pos - (double)i0);

                        for (int c = 0; c < channels; c++)
                        {
                            const float a = src[i0 * channels + c];
                            const float b = src[i1 * channels + c];

                            // Pitched sample with volume and pan applied (same as the synth's left_sample / right_sample)
                            float s = (a + (b - a) * frac) * gains[c];

                            // Filter (same maths as SamplerProcessChamberlainFilter)
                            if (use_filter)
                            {
                                hp[c] = s - lp[c] - q_coef * bp[c];
                                bp[c] = bp[c] + f_coef * hp[c];
                                lp[c] = lp[c] + f_coef * bp[c];

                                // Dampen output to prevent feedback looping
                                hp[c] *= 0.999f;
                                bp[c] *= 0.999f;
                                lp[c] *= 0.999f;

                                if (filter_type == FilterType::lowpass)       s = lp[c];
                                else if (filter_type == FilterType::highpass) s = hp[c];
                                else                                          s = bp[c];
                            }

                            out[i * channels + c] += s;
                        }

                        // Advance by the current rate
                        pos += rate;
                        rate += rate_step;
                    }

                    // Save the filter state for the next buffer
                    v->lp_l = lp[0]; v->bp_l = bp[0]; v->hp_l = hp[0];
                    v->lp_r = lp[1]; v->bp_r = bp[1]; v->hp_r = hp[1];

                    v->read_pos = pos;
                    v->cur_rate = rate_end;
                }

                // Clamp so overlapping voices can't exceed full scale
                const int total_samples = buffer_size / (int)sizeof(float);
                for (int i = 0; i < total_samples; i++)
                {
                    // A runaway parameter (NaN/Inf) would otherwise be sent straight to the device
                    const float x = out[i];
                    out[i] = (x != x) ? 0.0f : std::clamp(x, -1.0f, 1.0f);
                }

                // -= Timing output (prints only when a buffer takes more than half its budget) =-
                // Change the condition to `true` to see every buffer.
                auto t1 = std::chrono::high_resolution_clock::now();
                double render_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                double budget_ms = 1000.0 * block_frames / spec->freq;
                if (render_ms > budget_ms * 0.5)
                    printf("sampler render %.2f ms of %.2f ms budget\n", render_ms, budget_ms);

                ++diag_blocks;
                diag_max_ms = std::max(diag_max_ms, render_ms);
#if SAMPLER_DIAG
                if (t1 - diag_last > std::chrono::seconds(2))
                {
                    if (diag_gate_skips || diag_empty_pops || diag_underruns)
                        printf("[sampler] blocks %llu | gate skips %llu | empty pops %llu | underruns %llu | max render %.2f ms\n",
                            (unsigned long long)diag_blocks, (unsigned long long)diag_gate_skips,
                            (unsigned long long)diag_empty_pops, (unsigned long long)diag_underruns, diag_max_ms);
                    diag_blocks = diag_gate_skips = diag_empty_pops = diag_underruns = 0;
                    diag_max_ms = 0.0;
                    diag_last = t1;
                }
#endif

                SDL_PutAudioStreamData(stream, buffer.data(), buffer_size);
                sampler->frames_rendered += block_frames;
            }
            else
            {
                // Stream is full. Yield (don't sleep): the tracker busy-waits on will_stop_playing,
                // so a sleeping audio thread would stall it for up to the sleep length per voice.
                std::this_thread::yield();
            }

            if (first == true)
            {
                SDL_BindAudioStream(dev, stream);
                SDL_ResumeAudioDevice(dev);
                first = false;
            }
        }
        (*sampler_playing) = false;
    }

    // Automate the sampler modulation variables (Effected by call rate)
    void SampleAutomation(double* new_freq, double* new_pan, double* new_vol, double* new_cutoff, double* new_resonance, int voice_index)
    {
        // Step panning
        if (voices[voice_index]->pan_freq != 0)
        {
            voices[voice_index]->pan_phase += voices[voice_index]->pan_freq / 100;
            (*new_pan) = (sin(voices[voice_index]->pan_phase * 2. * PI) / 2) + 0.5;
            if (voices[voice_index]->pan_phase > 1.)
                voices[voice_index]->pan_phase -= 1.;
            if (voices[voice_index]->pan_phase < 0.)
                voices[voice_index]->pan_phase += 1.;
            voices[voice_index]->stg_panning = (*new_pan);
        }
        // Step note
        if (voices[voice_index]->pitch_freq != 0)
        {
            if (voices[voice_index]->pitch_freq > 0)
                voices[voice_index]->stg_swpd_freq = voices[voice_index]->stg_swpd_freq * (voices[voice_index]->pitch_freq + 1); // Stage the new freq change
            if (voices[voice_index]->pitch_freq < 0)
                voices[voice_index]->stg_swpd_freq = voices[voice_index]->stg_swpd_freq / (abs(voices[voice_index]->pitch_freq) + 1); // Stage the new freq change
            auto freq_swp_ofst = voices[voice_index]->stg_swpd_freq - voices[voice_index]->stg_step_freq;
            (*new_freq) += freq_swp_ofst;
        }
        // Step volume
        if (voices[voice_index]->volume_freq != 0)
        {
            (*new_vol) = voices[voice_index]->stg_volume + voices[voice_index]->volume_freq / 200;
            if ((*new_vol) < 0)
                (*new_vol) = 0;
            voices[voice_index]->stg_volume = (*new_vol);
        }
        // Step cutoff
        if (voices[voice_index]->cutoff_freq != 0)
        {
            (*new_cutoff) = voices[voice_index]->stg_cutoff + voices[voice_index]->cutoff_freq / 200;
            if ((*new_cutoff) < 0)
                (*new_cutoff) = 0;
            voices[voice_index]->stg_cutoff = (*new_cutoff);
        }
        // Step resonance
        if (voices[voice_index]->resonance_freq != 0)
        {
            (*new_resonance) = voices[voice_index]->stg_resonance + voices[voice_index]->resonance_freq / 200;
            if ((*new_resonance) < 0)
                (*new_resonance) = 0;
            voices[voice_index]->stg_resonance = (*new_resonance);
        }
    }
};