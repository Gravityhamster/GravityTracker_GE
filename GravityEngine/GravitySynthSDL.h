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
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdint>
#include "SPSC_Queue.h"
#include "GravityEngineTypes.h"
#include "GravityTempo.h"

// Get BPM from the tracker
extern std::atomic<int> bpm;

// For flushing denormal floats to zero (x86/x64 only)
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#define GRAVITY_HAS_SSE_CSR 1
#endif

#define PI 3.14159265358979323846f

// Set to 0 to silence the synth's once-every-2-seconds timing diagnostics
#ifndef SYNTH_DIAG
#define SYNTH_DIAG 1
#endif

// TODO: Any effects related directly to instrument automation should be implemented directly into the synth (i.e. vibrato, pitchsweep, fadein, fadeout, etc.)
// TODO: Acquire personal understanding of COPILOT marked code and rewrite it myself

// Chamerblain filter processing - COPILOT function implemented into a sequestored function
// float sample : Current decimal audio position
// float cutoff : 0 to 1 freq filter cutoff 
// float resonance : 0 to 1 resonance frequency
// float sample_rate_freq : Audio sample rate (e.g. 48000hz)
// float* lp : Pointer to the lowpass filter state variable
// float* bp : Pointer to the bandpass filter state variable
// float* hp : Pointer to the highpass filter state variable
inline void ProcessChamberlainFilter(float sample, float cutoff, float resonance, float sample_rate_freq, float* lp, float* bp, float* hp)
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

// Conversion map for waveforms
inline std::map<SynthWaveForm, std::string> waveform_to_string = {
    {SynthWaveForm::sine, "SINE"},
    {SynthWaveForm::square, "SQUARE"},
    {SynthWaveForm::pulse, "PULSE"},
    {SynthWaveForm::sawtooth, "SAWTOOTH"},
    {SynthWaveForm::triangle, "TRIANGLE"},
    {SynthWaveForm::noise, "NOISE"}
};

// Template for synth objects
class GravityEngine_Synth
{

    // Synth parameters
public:

    std::vector<GravityEngine_Voice*> voices;
    int sample_frames;

    // How many device buffers to keep queued in the stream (1 = lowest latency, 2 = more slack)
    static constexpr int kBuffersAhead = 2;

    // Row/tick gate. The tracker holds this while it writes ALL voice changes for one tick, e.g. at
    // the top of DoTick():
    //     std::scoped_lock lock(audiosampler->batch_mutex, audiosynth->batch_mutex);
    // Recursive so entry points can nest. The audio thread only try_locks it.
    // will_stop_playing is deliberately NOT gated: the tracker blocks until the audio thread clears it.
    std::recursive_mutex batch_mutex;

    // Total frames pushed to the stream so far (usable as an audio-driven clock)
    std::atomic<int64_t> frames_rendered = 0;

    // Conceptually this comes from a prompt I gave to Copilot, but then I rewrote it from scratch based on my understanding of the concepts.
    // It simply generates a waveform. Never call this indepentently please. Use BindSynthToChannel in the engine instead.
    // Now it's been further optimized by claude.
    // GravityEngine_Synth* synth : Synth object reference
    // SDL_AudioStream* stream : Audio stream that the synth audio plays on
    // SDL_AudioSpec* spec : Audio spec to format the audio with
    // SDL_AudioDeviceID dev : Device the audio will play on
    // ChannelStates* state : Current state of the channel
    // bool* synth_playing : Flag to indicate the thread has successfully finished
    static void GenerateAudio(GravityEngine_Synth* synth, SDL_AudioStream* stream, SDL_AudioSpec* spec, SDL_AudioDeviceID dev, std::atomic<ChannelStates>* state, std::atomic<bool>* synth_playing)
    {
#ifdef GRAVITY_HAS_SSE_CSR
        // Flush denormals to zero on this thread (decaying filters can otherwise get very slow)
        _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
        SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);

        // Get sample frames
        SDL_GetAudioDeviceFormat(dev, spec, &synth->sample_frames);
        if (spec->channels > 2)
            spec->channels = 2;

        const int channels = spec->channels;
        const int buffer_frames = synth->sample_frames;

        // Render in small blocks (a quarter of a device buffer). Tracker updates (note starts, queued
        // changes) are applied once per block, so onset jitter is ~2.5 ms instead of ~10 ms.
        const int block_frames = std::max(1, buffer_frames / 4);
        const int block_samples = block_frames * channels;
        const int block_bytes = block_samples * sizeof(float);
        float* buffer = (float*)SDL_malloc(block_bytes);
        bool first = true;

        // Cheap noise generator (xorshift32) instead of mt19937 + distribution
        uint32_t noise_state = std::random_device{}() | 1u;

        // Diagnostics (printed every ~2 s when SYNTH_DIAG is on)
        uint64_t diag_blocks = 0, diag_gate_skips = 0, diag_empty_pops = 0, diag_underruns = 0;
        double diag_max_ms = 0.0;
        auto diag_last = std::chrono::steady_clock::now();

        SDL_SetAudioStreamGain(stream, 1.0f);

        // Keep supplying data
        while ((*state) == playing || (*state) == paused) {

            // Stops are handled every iteration, outside the gate: the tracker busy-waits on this flag
            for (auto v : synth->voices)
            {
                if (v->will_stop_playing)
                {
                    while (v->live_changes.pop()) {}
                    v->start_playing = false;
                    v->will_stop_playing = false;
                }
            }

            // If the synth is paused, do not play the synth
            if ((*state) == paused)
            {
                std::this_thread::yield();
                continue;
            }

            // Get the available stream in frames
            int available_frames = SDL_GetAudioStreamAvailable(stream) / (sizeof(float) * channels);

            // Stream completely drained after playback began = the device ran dry (audible gap)
            if (!first && available_frames == 0)
                ++diag_underruns;

            // Check available data
            if (available_frames < buffer_frames * kBuffersAhead)
            {
                // -= Timing: how long does it take to render one block? =-
                auto t0 = std::chrono::high_resolution_clock::now();

                SDL_memset(buffer, 0, block_bytes);

                // -= Phase 1: apply tracker updates (starts, queued changes) =-
                // The tracker holds batch_mutex while it writes a whole tick for all voices.
                // try_lock never blocks the audio thread: if the tracker is mid-write we apply nothing
                // this block and pick the whole tick up together on the next one, so a row can never
                // be split across two blocks.
                {
                    std::unique_lock<std::recursive_mutex> batch_lock(synth->batch_mutex, std::try_to_lock);
                    const bool can_apply = batch_lock.owns_lock();
                    if (!can_apply) ++diag_gate_skips;

                    for (auto v : synth->voices)
                    {
                        // Voice was just triggered: initialise its state
                        if (can_apply && v->will_start_playing)
                        {
                            v->freq = v->stg_base_freq.load();
                            v->volume = v->stg_volume.load();
                            v->cutoff = v->stg_cutoff.load();
                            v->resonance = v->stg_resonance.load();
                            v->panning = std::clamp<float>(v->stg_panning.load(), 0.f, 1.f);
                            v->pulse_width = v->stg_pulse_width.load();

                            v->hp = v->bp = v->lp = 0.0f;

                            // The tracker primes frame_counter to one full tick before starting a note
                            // so the first queued change is applied straight away. Keep that.
                            v->tick_acc = (double)v->frame_counter.load();

                            v->will_start_playing = false;
                            v->start_playing = true;
                        }

                        if (!v->start_playing)
                            continue;

                        // Has BPM changed?
                        auto global_bpm = bpm.load();
                        if (global_bpm != v->my_bpm)
                        {
                            v->my_bpm = global_bpm;

                            const double old_fpt = v->frames_per_tick.load();
                            const double new_fpt = GravityFramesPerTick(spec->freq, global_bpm, tps);

                            // Keep the same fractional progress through the current tick
                            if (old_fpt > 0.0)
                                v->tick_acc = (v->tick_acc / old_fpt) * new_fpt;

                            v->frames_per_tick = (float)new_fpt;
                        }

                        // -= Apply queued parameter changes (one per tick) =-
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
                                    // The tracker hasn't delivered this tick's change yet. Keep the tick
                                    // pending so it's applied the moment it arrives, instead of leaving
                                    // this voice one tick behind for the rest of the note.
                                    ++diag_empty_pops;
                                    v->tick_acc = std::min(v->tick_acc, tick_len * 2.0);
                                    break;
                                }
                                v->tick_acc -= tick_len;

                                if (change.freq != -9999) v->freq = change.freq;
                                if (change.pan != -2)     v->panning = change.pan;
                                if (change.pw != -1)      v->pulse_width = change.pw;
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
                                {
                                    v->will_stop_timer--;
                                }
                            }
                        }
                    }
                }

                // -= Phase 2: render every playing voice =-
                for (auto v : synth->voices)
                {
                    if (!v->start_playing)
                        continue;

                    // Parameters are constant across the block, so compute everything once
                    const SynthWaveForm waveform = v->waveform;
                    float phase = v->phase;

                    const float vol = v->volume.load();
                    const float pan = std::clamp<float>(v->panning.load(), 0.f, 1.f);
                    // Clamp to [0, Nyquist]: an unbounded pitch sweep reaches Inf, which turned the phase into NaN
                    // and wrote NaN to the device. std::max/min also map a NaN freq to 0.
                    const float step = std::min(0.5f, std::max(0.0f, v->freq.load() / (float)spec->freq));

                    float gain_l, gain_r;
                    if (channels == 2)
                    {
                        // Equal-power pan
                        const float angle = pan * (PI / 2.0f);
                        gain_l = std::cos(angle) * vol;
                        gain_r = std::sin(angle) * vol;
                    }
                    else
                    {
                        gain_l = (1.0f - std::abs(0.5f - pan)) * vol;
                        gain_r = gain_l;
                    }

                    // Pulse: sin(2*pi*p) > pw  <=>  p0 < p < 0.5 - p0
                    float pulse_lo = 0.f, pulse_hi = 0.5f;
                    if (waveform == SynthWaveForm::pulse)
                    {
                        const float pw = std::clamp<float>(v->pulse_width.load(), 0.f, 0.999f);
                        pulse_lo = std::asin(pw) / (2.0f * PI);
                        pulse_hi = 0.5f - pulse_lo;
                    }

                    // Filter (runs once on the mono signal, then pan/volume are applied)
                    const bool use_filter = (v->filter != FilterType::none) &&
                        (v->algorithm == FilterAlgorithm::chamberlain);
                    const FilterType filter_type = v->filter;
                    float lp = v->lp, bp = v->bp, hp = v->hp;
                    float f_coef = 0.f, q_coef = 0.f;
                    if (use_filter)
                    {
                        const float warped = v->cutoff * v->cutoff * v->cutoff;
                        const float cutoff_hz = warped * ((float)spec->freq * 0.5f);
                        f_coef = std::clamp(2.0f * sinf(PI * cutoff_hz / (float)spec->freq), 0.f, 0.999f);
                        q_coef = std::clamp(1.0f - v->resonance, 0.05f, 1.f);
                    }

                    float* out = buffer;

                    for (int i = 0; i < block_frames; i++)
                    {
                        // -= Raw waveform from the phase (0..1) =-
                        float s;
                        switch (waveform)
                        {
                        case SynthWaveForm::sine:
                            s = sinf(phase * 2.0f * PI);
                            break;
                        case SynthWaveForm::square:
                            s = (phase < 0.5f) ? 1.f : -1.f;
                            break;
                        case SynthWaveForm::pulse:
                            s = (phase > pulse_lo && phase < pulse_hi) ? 1.f : -1.f;
                            break;
                        case SynthWaveForm::sawtooth:
                            s = phase * 2.f - 1.f;
                            break;
                        case SynthWaveForm::triangle:
                            // Closed form of the old acos(cos()) triangle
                            s = (phase < 0.25f) ? 4.f * phase
                                : (phase < 0.75f) ? 2.f - 4.f * phase
                                : 4.f * phase - 4.f;
                            break;
                        default: // noise
                            noise_state ^= noise_state << 13;
                            noise_state ^= noise_state >> 17;
                            noise_state ^= noise_state << 5;
                            s = (float)(int32_t)noise_state * (1.0f / 2147483648.0f);
                            break;
                        }

                        if (use_filter)
                        {
                            hp = s - lp - q_coef * bp;
                            bp = bp + f_coef * hp;
                            lp = lp + f_coef * bp;

                            // Dampen output to prevent feedback looping
                            hp *= 0.999f;
                            bp *= 0.999f;
                            lp *= 0.999f;

                            if (filter_type == FilterType::lowpass)       s = lp;
                            else if (filter_type == FilterType::highpass) s = hp;
                            else                                          s = bp;
                        }

                        if (channels == 1)
                            out[i] += s * gain_l;
                        else
                        {
                            out[i * 2 + 0] += s * gain_l;
                            out[i * 2 + 1] += s * gain_r;
                        }

                        // Step and wrap the phase
                        phase += step;
                        if (phase >= 1.f)
                        {
                            phase -= 1.f;
                            if (phase >= 1.f) phase -= std::floor(phase); // very high freq safety
                        }
                    }

                    v->phase = phase;
                    v->lp = lp; v->bp = bp; v->hp = hp;
                }

                // Clamp the buffer to -1.0 to 1.0 to prevent clipping and distortion
                for (int i = 0; i < block_samples; i++)
                {
                    const float x = buffer[i];
                    buffer[i] = (x != x) ? 0.0f : std::clamp(x, -1.0f, 1.0f);
                }

                // -= Timing output (prints only when a block takes more than half its budget) =-
                auto t1 = std::chrono::high_resolution_clock::now();
                double render_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                double budget_ms = 1000.0 * block_frames / spec->freq;
                if (render_ms > budget_ms * 0.5)
                    printf("synth render %.2f ms of %.2f ms budget\n", render_ms, budget_ms);

                ++diag_blocks;
                diag_max_ms = std::max(diag_max_ms, render_ms);
#if SYNTH_DIAG
                if (t1 - diag_last > std::chrono::seconds(2))
                {
                    if (diag_gate_skips || diag_empty_pops || diag_underruns)
                        printf("[synth] blocks %llu | gate skips %llu | empty pops %llu | underruns %llu | max render %.2f ms\n",
                            (unsigned long long)diag_blocks, (unsigned long long)diag_gate_skips,
                            (unsigned long long)diag_empty_pops, (unsigned long long)diag_underruns, diag_max_ms);
                    diag_blocks = diag_gate_skips = diag_empty_pops = diag_underruns = 0;
                    diag_max_ms = 0.0;
                    diag_last = t1;
                }
#endif

                // Push block to stream
                SDL_PutAudioStreamData(stream, buffer, block_bytes);
                synth->frames_rendered += block_frames;
            }
            else
            {
                // Stream is full. Yield (don't sleep): the tracker busy-waits on will_stop_playing,
                // so a sleeping audio thread would stall it.
                std::this_thread::yield();
            }

            // Start the synth playback but only if this is the first time starting
            if (first == true)
            {
                // Attach the audio stream to the channel's audio device
                SDL_BindAudioStream(dev, stream);
                // Start playback
                SDL_ResumeAudioDevice(dev);
                // Audio is bound, don't do this again.
                first = false;
            }
        }
        // End sequence
        SDL_free(buffer);
        (*synth_playing) = false;
    }

    // Automate the synth modulation variables (Effected by call rate)
    void SynthAutomation(double* new_freq, double* new_pan, double* new_pw, double* new_vol, double* new_cutoff, double* new_resonance, int voice_index)
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
        // Step pulse width
        if (voices[voice_index]->pulse_width_freq != 0)
        {
            voices[voice_index]->pw_phase += voices[voice_index]->pulse_width_freq / 100;
            (*new_pw) = (sin(voices[voice_index]->pw_phase * 2. * PI) / 2) * 0.99 + 0.5;
            if (voices[voice_index]->pw_phase > 1.)
                voices[voice_index]->pw_phase -= 1.;
            if (voices[voice_index]->pw_phase < 0.)
                voices[voice_index]->pw_phase += 1.;
            voices[voice_index]->stg_pulse_width = (*new_pw);
        }
        // Step note
        if (voices[voice_index]->pitch_freq != 0)
        {
            if (voices[voice_index]->pitch_freq > 0)
                voices[voice_index]->stg_swpd_freq = voices[voice_index]->stg_swpd_freq * (voices[voice_index]->pitch_freq + 1); // Stage the new freq change
            if (voices[voice_index]->pitch_freq < 0)
                voices[voice_index]->stg_swpd_freq = voices[voice_index]->stg_swpd_freq / (abs(voices[voice_index]->pitch_freq) + 1); // Stage the new freq change
            auto freq_swp_ofst = voices[voice_index]->stg_swpd_freq - voices[voice_index]->stg_base_freq;
            (*new_freq) += freq_swp_ofst;
        }
        // Step volume
        if (voices[voice_index]->volume_freq != 0)
        {
            (*new_vol) = voices[voice_index]->stg_volume + (voices[voice_index]->volume_freq * 16) / 200;
            if ((*new_vol) < 0)
                (*new_vol) = 0;
            voices[voice_index]->stg_volume = (*new_vol);
        }
        // Step cutoff
        if (voices[voice_index]->cutoff_freq != 0)
        {
            (*new_cutoff) = voices[voice_index]->stg_cutoff + (voices[voice_index]->cutoff_freq * 16) / 200;
            if ((*new_cutoff) < 0)
                (*new_cutoff) = 0;
            if ((*new_cutoff) > 1)
                (*new_cutoff) = 1;
            voices[voice_index]->stg_cutoff = (*new_cutoff);
        }
        // Step resonance
        if (voices[voice_index]->resonance_freq != 0)
        {
            (*new_resonance) = voices[voice_index]->stg_resonance + (voices[voice_index]->resonance_freq * 16) / 200;
            if ((*new_resonance) < 0)
                (*new_resonance) = 0;
            if ((*new_resonance) > 1)
                (*new_resonance) = 1;
            voices[voice_index]->stg_resonance = (*new_resonance);
        }
    }
};