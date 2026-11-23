#pragma once


// Enum to define the current playback state of a sound channel
enum ChannelStates
{
    stopped,
    init,
    playing,
    paused,
    uninit
};

// Enum to define the type of filter applied to audio channel
enum class FilterType
{
    lowpass,
    highpass,
    bandpass,
    none,
    min = lowpass,
    max = none
};

// Enum to define filter algorithm
enum class FilterAlgorithm
{
    chamberlain
};

// Struct for submitting changes to the sampler parameters from the main thread to the audio thread
struct live_change
{
    double freq = -9999.0;
    double pan = -2.0;
    double vol = -1.0;
    double pw = -1.0;
    double ctf = -1.0;
    double res = -1.0;
};

// Enum to define the wave forms on a synth
enum class SynthWaveForm
{
    sine,
    square,
    pulse,
    sawtooth,
    triangle,
    noise,
    min = sine,
    max = noise
};

// Gravity Engine sound class
class GravityEngine_Sound
{
private:
    // Copilot help on this one
    // Convert the audio in the audio buffer to a different audio spec
    // Uint8* audio_buf : The buffer for the audio data
    // Uint32 audio_len : The length of the audio in the audio buffer
    // SDL_AudioSpec wav_audio_spec : Audio specifications of the audio to be converted
    // SDL_AudioSpec audio_spec : Audio specifications to convert the audio to
    std::vector<Uint8> ConvertAudio(Uint8* audio_buf, Uint32 audio_len, SDL_AudioSpec wav_audio_spec, SDL_AudioSpec audio_spec)
    {
        std::vector<Uint8> converted_data;

        SDL_AudioStream* conv = SDL_CreateAudioStream(&wav_audio_spec, &audio_spec);
        if (!conv)
        {
            SDL_Log("Sound conversion failed: %s", SDL_GetError());
            return converted_data;
        }

        if (SDL_PutAudioStreamData(conv, audio_buf, (int)audio_len) && SDL_FlushAudioStream(conv))
        {
            // Pull everything out in one go instead of growing a vector 4 KB at a time
            const int available = SDL_GetAudioStreamAvailable(conv);
            if (available > 0)
            {
                converted_data.resize((size_t)available);
                const int got = SDL_GetAudioStreamData(conv, converted_data.data(), available);
                converted_data.resize(got > 0 ? (size_t)got : 0);
            }
        }
        SDL_DestroyAudioStream(conv);
        return converted_data;
    }

public:
    // -= Attributes =-
    std::vector<Uint8> converted_audio; // Buffer for the final converted audio data

    // -= Methods =-

    // Construct audio
    // const char* path : File path of the audio
    // SDL_AudioSpec audio_spec : Audio specification to convert the audio to (this should be the global audio spec in the engine)
    GravityEngine_Sound(const char* path, SDL_AudioSpec audio_spec)
    {
        Uint8* audio_buf = nullptr;
        Uint32 audio_len = 0;
        SDL_AudioSpec wav_audio_spec;

        // Load the wav file (the result used to be ignored: a bad path left these uninitialised)
        if (!SDL_LoadWAV(path, &wav_audio_spec, &audio_buf, &audio_len))
        {
            SDL_Log("Could not load '%s': %s", path, SDL_GetError());
            return; // converted_audio stays empty, which the sampler plays as silence
        }

        // Convert the audio
        converted_audio = ConvertAudio(audio_buf, audio_len, wav_audio_spec, audio_spec);

        // SDL_LoadWAV allocates the original buffer. It was never freed: one leak per loaded sample.
        SDL_free(audio_buf);
    };

    // Destruct audio
    ~GravityEngine_Sound() {};
};

// Voice structure
class GravityEngine_Voice
{

public:
    SpscQueue<live_change, 1024> live_changes;
    std::atomic<int> my_bpm = 0;
    std::atomic<int> frame_counter = 0;
    std::atomic<float> frames_per_tick = 0;

    // Actual audio values that are used in the audio thread
    std::atomic<float> freq = 50.0;
    std::atomic<float> volume = 1;
    std::atomic<float> panning = 0.5;
    std::atomic<float> pulse_width = 0.5;

    // Staging values that are only used in tracking position in incremental changes to the sampler parameters. 
    // These are used to all movement to be smoother. So basically, the tracker thread will make a change to the
    // audio values, then write the change to the queue and then write the change to these staging values so that
    // it can continue from the last queue write value, even if the sampler has not yet applied its changes.
    std::atomic<float> stg_base_freq = 50.0;
    std::atomic<float> stg_step_freq = 50.0;
    std::atomic<float> stg_swpd_freq = 0.0;
    std::atomic<float> stg_volume = 1;
    std::atomic<float> stg_cutoff = 1;
    std::atomic<float> stg_resonance = 1;
    std::atomic<float> stg_panning = 0.5;
    std::atomic<float> stg_pulse_width = 0.5;

    std::atomic<float> pitch_freq = 0;
    std::atomic<float> volume_freq = 0;
    std::atomic<float> cutoff_freq = 0;
    std::atomic<float> resonance_freq = 0;
    std::atomic<float> pan_freq = 0.0;
    std::atomic<float> pulse_width_freq = 0.0;

    // float vibrato_freq = 0; -- Not yet implemented
    // float vibrato_amp = 0; -- Not yet implemented

    // Filter
    float cutoff = 0.5f; // 0.0 - 1.0
    float resonance = 0.5f; // 0.0 - 1.0

    // Sample filter state
    float lp_l = 0.0f;
    float bp_l = 0.0f;
    float hp_l = 0.0f;
    float lp_r = 0.0f;
    float bp_r = 0.0f;
    float hp_r = 0.0f;
    // Synth filter
    float lp = 0.0f;
    float bp = 0.0f;
    float hp = 0.0f;

    SynthWaveForm waveform = SynthWaveForm::sine;
    FilterType filter = FilterType::none;
    FilterAlgorithm algorithm = FilterAlgorithm::chamberlain;

    // Other state variables
    float pan_phase = 0.f;
    float pw_phase = 0.f;

    // Audio-thread-only tick clock. Fractional, so a frames_per_tick like 441.6 doesn't get truncated
    double tick_acc = 0.0;

    std::atomic<bool> will_start_playing = false;
    std::atomic<bool> will_stop_playing = false;
    std::atomic<int> will_stop_timer = -1;
    std::atomic<bool> start_playing = false;

    long start_time_ms = 0;
    long mid_time_ms = 0;
    long end_time_ms = -1;

    float phase = 0.;
    float last_freq = 0;

    GravityEngine_Sound* sound = nullptr;
    bool looping = false;

    // Frequency of the original sampled audio (0 = don't pitch shift, play at original speed)
    std::atomic<float> base_freq = 0.f;

    // Replaces `long read_ptr`: playback position in FRAMES, fractional
    double read_pos = 0.0;
    // Playback rate at the end of the previous buffer (used for smooth ramping)
    double cur_rate = 1.0;
};
