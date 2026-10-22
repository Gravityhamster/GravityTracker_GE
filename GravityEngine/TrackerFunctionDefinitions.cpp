#include "TrackerTypesAndVariables.h"
#include "TrackerFunctionSignatures.h"
#include <thread>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include "GravityTempo.h"

// Find string f in s
bool str_contains(const std::string& s, const std::string& f) { return s.find(f) != std::string::npos; }

// Remove char from string
std::string str_remove(std::string s, char c)
{
    // https://www.geeksforgeeks.org/dsa/remove-all-occurrences-of-a-character-in-a-string/
    int j = 0; // Size variable
    for (int i = 0; i < s.size(); i++)
    {
        // Move chars to keep down to the front
        if (s[i] != c)
            s[j++] = s[i];
    }
    // Truncate to size
    s.resize(j);
    return s;
}

// Get note freq
double NoteFreq(double n)
{
    // https://superglobalcalculator.com/calculators/music/piano-key-frequency/
    return 440.0 * pow(2.0, ((n + 1) - 49.0) / 12.0);
}

// Get freq note
double FreqNote(double f)
{
    return 12 * std::log2(f / 440) + 48;
}

// Get sample ratio
// int base_pitch : Base pitch of the sample tuned to a piano. For example, C4 == 39
// int new_pitch : New pitch of the sample tuned to a piano. For example, C#4 == 40
// int detune : Cents to detune the pitch by
double GetSampleRatioChange(int base_pitch, int new_pitch, double detune = 0)
{
    return std::pow(2, (FreqNote(NoteFreq(new_pitch) + detune) - base_pitch) / 12.f);
}

// Insert at arbitrary location
// Takes ownership of ptr. An invalid index used to run vector::insert at begin() - 1 (undefined behaviour)
// and leak the object; now the object is freed instead.
template <typename T> void
InsertAt(std::vector<T*>* vec, int index, T* ptr)
{
    if (index < 0 || index > 0xFFFF)
    {
        delete ptr;
        return;
    }

    // Fill with nulls up to and including index
    if (vec->size() <= (size_t)index)
        vec->resize((size_t)index + 1, nullptr);

    // NOTE: an existing non-null entry is overwritten, not deleted. Every call site checks for null
    // first, so nothing is lost today, but deleting here could dangle pointers held elsewhere.
    (*vec)[index] = ptr;
}

// Get at arbitrary location
template <typename T> T*
GetAt(std::vector<T*>* vec, int index)
{
    if (index < 0 || (size_t)index >= vec->size())
        return nullptr;
    return (*vec)[index];
}

// Get next empty index
template <typename T> int
GetNextEmpty(std::vector<T*>* vec)
{
    int index = 0;
    // Iterate through, trying to find the next null or the end of the vector
    while (index < vec->size())
    {
        // If this is an empty cell, return this index
        if ((*vec)[index] == nullptr)
            return index;
        // Inc index
        index++;
    }
    // Return the last index
    return index;
}

// Audio frames per tracker tick at the current tempo
static double FramesPerTick()
{
    return GravityFramesPerTick(geptr->global_audio_spec.freq, bpm, tps);
}

// Wrap a note number into [min_note, max_note] in O(1).
// Same result as the old while-loops, which never terminated if min_note > max_note.
static void WrapNote(int* f)
{
    const int range = max_note - min_note + 1;
    if (range <= 0)
        return;
    int off = ((*f) - min_note) % range;
    if (off < 0)
        off += range;
    (*f) = min_note + off;
}

// Ask the audio thread to stop a voice and wait for it to confirm.
// The wait is bounded: if the audio thread isn't running (shutdown, device error) the old unbounded
// spin hung the tracker thread forever.
static void StopVoiceAndWait(std::atomic<bool>& will_stop)
{
    will_stop = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (will_stop)
    {
        if (std::chrono::steady_clock::now() > deadline)
            return; // give up; the flag is consumed whenever the audio thread resumes
        std::this_thread::yield();
    }
}

// Free a loaded sound safely. The old code freed it while the audio thread could still be reading it
// (use-after-free), and left voices pointing at the freed object.
static void ReleaseSound(int sound_index)
{
    auto* snd = geptr->GetSound(sound_index);
    {
        std::scoped_lock lock(audiosampler->batch_mutex, audiosynth->batch_mutex);
        for (auto* v : audiosampler->voices)
        {
            if (snd != nullptr && v->sound == snd)
            {
                StopVoiceAndWait(v->will_stop_playing); // audio thread confirms it's done with the voice
                v->sound = nullptr;
            }
        }
    }
    geptr->DeleteSound(sound_index);
}

// Preview sample audio
// channelnumber : The particular channel to play the sample on
// sample_index : Index of the sample to play
void PreviewSample(int channel_index, int sample_index)
{
    std::scoped_lock lock(audiosampler->batch_mutex, audiosynth->batch_mutex);

    channellist[channel_index].type = ChannelType::file;

    // The sample (and its loaded sound) must exist
    auto* preview_sample = GetAt(&samplelist, sample_index);
    auto* preview_sound = preview_sample != nullptr ? geptr->GetSound(preview_sample->sound_index) : nullptr;
    if (preview_sound == nullptr)
        return;

    // Reset sampler state and action queue
    auto v = audiosampler->voices[channel_index];
    StopVoiceAndWait(v->will_stop_playing);

    geptr->SetChannelVolume(1, 1);
    geptr->SetChannelPitchRatio(1, 1);

    // Init voice
    v->stg_step_freq = NoteFreq(39);
    v->stg_swpd_freq = v->stg_step_freq.load();
    v->base_freq = NoteFreq(39);
    v->stg_volume = 1.f;
    v->stg_cutoff = 1.f;
    v->stg_resonance = 1.f;
    v->volume_freq = 0.f;
    v->cutoff_freq = 0.f;
    v->resonance_freq = 0.f;
    v->stg_panning = 0.5f;
    v->pan_phase = 0.0f;
    v->pan_freq = 0.0f;
    v->pitch_freq = 0.0f;
    v->cutoff = 0.0f;
    v->resonance = 0.0f;
    v->filter = FilterType::none;
    v->start_time_ms = 0x000000;
    v->end_time_ms = 0xFFFFFF;
    v->mid_time_ms = 0x000000;
    v->looping = false;
    v->sound = preview_sound;

    // Prepare synth queueing variables
    double frames_per_tick = FramesPerTick();
    v->frame_counter = frames_per_tick;
    v->frames_per_tick = frames_per_tick;

    v->will_start_playing = true;
}

// Change BPM
// int bpm : New BPM value
void ChangeBPM(int new_bpm)
{
    std::scoped_lock lock(audiosampler->batch_mutex, audiosynth->batch_mutex);

	// Clamp bpm to 1-999
	if (new_bpm < 1)
		new_bpm = 1;
	if (new_bpm > 999)
		new_bpm = 999;
	// Set bpm
	bpm = new_bpm;
    // Get the length of the tick
    ticklength = BpmToTicklength(bpm);
}

// DoEffect
void DoFX(int channel_index, ChannelType inst_type, int f, int f_p, FX_Exec_Type t)
{
    // Init voice
    GravityEngine_Voice* v;
    if (inst_type == ChannelType::synth)
        v = audiosynth->voices[channel_index];
    else // Implicitly ChannelType::file
        v = audiosampler->voices[channel_index];

    // Execute the effect before the instrument settings are applied
    if (t == before)
    {
        // Switch behavior on the fx
        switch (fx[f])
        {
        case 'D': // D : Delay
            // Set the delay on the channel
            if (channellist[channel_index].first_delay_passed == true)
            {
                channellist[channel_index].delay_ticks = f_p;
                channellist[channel_index].delay_tick_offsetter = ticknumber;
                channellist[channel_index].first_delay_passed = false;
            }
            else
            {
                channellist[channel_index].first_delay_passed = true;
            }
            break;
        case 'T': // T : Time
			ChangeBPM(f_p);
            break;
        }
    }

	// Execute the effect after the instrument settings are applied
    if (t == after)
    {
        std::string outstr;
        // Switch behavior on the fx
        switch (fx[f])
        {
        case 'E': // E : Envelope
            // Set volume freq
            outstr = IntToHexString(f_p);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            v->volume_freq = (std::stoi(outstr.substr(0, 4), 0, 16) - 32768) / 32768.f;
            break;
        case 'V': // V : Volume
            // Set volume
            outstr = IntToHexString(f_p);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            v->stg_volume = std::stoi(outstr.substr(0, 4), 0, 16) / 65535.f;
            break;
        case 'K': // K : Kill
            // Kill the voice
            v->will_stop_timer = f_p;
            break;
        }
    }
}

// Get FX from Phrase and apply
// channelnumber : The particular channel to play the step on
// f : frequency to play
// fx1 : FX1 to apply
// fx1_p : FX1 parameter to apply
// fx2 : FX2 to apply
// fx2_p : FX2 parameter to apply
// fx3 : FX3 to apply
// fx3_p : FX3 parameter to apply
// FX_Exec_Type t : Type of execution
void GetFX(int channel_index, ChannelType inst_type, int f, int fx1, int fx1_p, int fx2, int fx2_p, int fx3, int fx3_p, FX_Exec_Type t)
{
    // Init voice
    GravityEngine_Voice* v;
    if (inst_type == ChannelType::synth)
        v = audiosynth->voices[channel_index];
	else // Implicitly ChannelType::file
        v = audiosampler->voices[channel_index];

	// Override frequency if a new one is provided
    if (f != -9999)
    {
        v->stg_base_freq = NoteFreq(f);
        v->stg_swpd_freq = v->stg_base_freq.load();
    }

    // Apply effect 1
    if (fx1 != -1 && fx1_p != -1)
    {
        // Apply FX1 logic
		DoFX(channel_index, inst_type, fx1, fx1_p, t);
    }

    // Apply effect 2
    if (fx2 != -1 && fx2_p != -1)
    {
        // Apply FX2 logic
		DoFX(channel_index, inst_type, fx2, fx2_p, t);
    }

    // Apply effect 3
    if (fx3 != -1 && fx3_p != -1)
    {
        // Apply FX3 logic
		DoFX(channel_index, inst_type, fx3, fx3_p, t);
    }

}

// Play step phrase
// channelnumber : The particular channel to play the step on
// playing_phrase : Phrase to play
// step_ptr : Phrase progress index
void PlayStepPhrase(int channel_index, int playing_phrase, int step_ptr)
{
    std::scoped_lock lock(audiosampler->batch_mutex, audiosynth->batch_mutex);

    // Get frequency to play (the phrase may have been removed or the pointers may be out of range)
    auto* cur_phrase = GetAt(&phraselist, playing_phrase);
    if (cur_phrase == nullptr || step_ptr < 0 || step_ptr >= phrase::len_y)
        return;
    auto f = cur_phrase->arr[step_ptr][0];
    auto i = cur_phrase->arr[step_ptr][1];
	auto fx1 = cur_phrase->arr[step_ptr][2];
    auto fx1_p = cur_phrase->arr[step_ptr][3];
	auto fx2 = cur_phrase->arr[step_ptr][4];
    auto fx2_p = cur_phrase->arr[step_ptr][5];
    auto fx3 = cur_phrase->arr[step_ptr][6];
    auto fx3_p = cur_phrase->arr[step_ptr][7];
    // If no note is present, or the sequencer is catching up from a delayed state, no need to play audibly
    if (f != -9999 && GetAt(&instrumentlist, i) != nullptr)
    {
        // Set the type of audio playing on the channel
        channellist[channel_index].type = instrumentlist[i]->type;
        // Copy the settings that the channels needs to know to keep doing the transposition beyond the start
        channellist[channel_index].playing_table = instrumentlist[i]->table_index;
        channellist[channel_index].playing_freq = f;
        channellist[channel_index].playing_instrument = i;
        channellist[channel_index].tick_ptr = 0;
        // Table logic
        auto t = GetAt(&tablelist, channellist[channel_index].playing_table);
        // Get playing chain tranposition
        if (play_context != pt_phrase && play_context != pt_phrase_all && play_context != pt_preview && running && !pause_song)
            ApplyChainTransposition(channel_index, &f);

        // Reset synth state and action queue
        auto synth_voice = audiosynth->voices[channel_index];
        StopVoiceAndWait(synth_voice->will_stop_playing);

        // Reset sampler state and action queue
        auto sampler_voice = audiosampler->voices[channel_index];
        StopVoiceAndWait(sampler_voice->will_stop_playing);

        // Play step on channel
        if (instrumentlist[i]->type == ChannelType::synth)
        {
            geptr->SetChannelVolume(0, 1);
            geptr->SetChannelPitchRatio(0, 1);

            // Init voice
            auto v = audiosynth->voices[channel_index];

            v->stg_base_freq = NoteFreq(f) + instrumentlist[i]->detune;
            v->stg_swpd_freq = v->stg_base_freq.load();
            v->stg_volume = instrumentlist[i]->volume;
            v->stg_cutoff = instrumentlist[i]->cutoff;
            v->stg_resonance = instrumentlist[i]->resonance;
            v->volume_freq = instrumentlist[i]->volume_freq;
            v->cutoff_freq = instrumentlist[i]->cutoff_freq;
            v->resonance_freq = instrumentlist[i]->resonance_freq;
            v->stg_panning = instrumentlist[i]->panning;
            v->pan_phase = 0.5f - asinf(std::clamp((instrumentlist[i]->panning - 0.5f) * 2.0f, -1.0f, 1.0f)) / (2.0f * PI);
            v->pw_phase = 0.5f - asinf(std::clamp((instrumentlist[i]->pulse_width - 0.5f) / 0.495f, -1.0f, 1.0f)) / (2.0f * PI);
            v->pan_freq = instrumentlist[i]->pan_freq;
            v->stg_pulse_width = instrumentlist[i]->pulse_width;
            v->pulse_width_freq = instrumentlist[i]->pulse_width_freq;
            v->pitch_freq = instrumentlist[i]->pitch_freq;
            v->cutoff = instrumentlist[i]->cutoff;
            v->resonance = instrumentlist[i]->resonance;
            v->waveform = instrumentlist[i]->waveform;
            v->filter = instrumentlist[i]->filter;
			v->my_bpm = bpm.load();

            // Prepare synth queueing variables
            double frames_per_tick = FramesPerTick();
            v->frame_counter = frames_per_tick;
            v->frames_per_tick = frames_per_tick;

            // Read and apply step effects
            GetFX(channel_index, channellist[channel_index].type, -9999, fx1, fx1_p, fx2, fx2_p, fx3, fx3_p, after);

            // Read and apply table effects
            if (t != nullptr)
            {
                channellist[channel_index].skip_next_fx = true;
                // Read and apply instrument functions
                int table_fx1 = t->arr[0][1];
                int table_fx1_p = t->arr[0][2];
                int table_fx2 = t->arr[0][3];
                int table_fx2_p = t->arr[0][4];
                int table_fx3 = t->arr[0][5];
                int table_fx3_p = t->arr[0][6];
                GetFX(channel_index, channellist[channel_index].type, -9999, table_fx1, table_fx1_p, table_fx2, table_fx2_p, table_fx3, table_fx3_p, after);
            }

            // Signal the voice to play
            v->will_start_playing = true;
        }
        else if (instrumentlist[i]->type == ChannelType::file && instrumentlist[i]->sample_index != -1)
        {
            geptr->SetChannelVolume(1, 1);
            geptr->SetChannelPitchRatio(1, 1);

            // Init voice
            auto v = audiosampler->voices[channel_index];

            v->stg_step_freq = NoteFreq(f) + instrumentlist[i]->detune;
            v->stg_swpd_freq = v->stg_step_freq.load();
            v->base_freq = NoteFreq(instrumentlist[i]->base_pitch);
            v->stg_volume = instrumentlist[i]->volume;
            v->stg_cutoff = instrumentlist[i]->cutoff;
            v->stg_resonance = instrumentlist[i]->resonance;
            v->volume_freq = instrumentlist[i]->volume_freq;
            v->cutoff_freq = instrumentlist[i]->cutoff_freq;
            v->resonance_freq = instrumentlist[i]->resonance_freq;
            v->stg_panning = instrumentlist[i]->panning;
            v->pan_phase = 0.5f - asinf(std::clamp((instrumentlist[i]->panning - 0.5f) * 2.0f, -1.0f, 1.0f)) / (2.0f * PI);
            v->pan_freq = instrumentlist[i]->pan_freq;
            v->pitch_freq = instrumentlist[i]->pitch_freq;
            v->cutoff = instrumentlist[i]->cutoff;
            v->resonance = instrumentlist[i]->resonance;
            v->start_time_ms = instrumentlist[i]->start_time_ms;
            v->end_time_ms = instrumentlist[i]->end_time_ms;
            v->mid_time_ms = instrumentlist[i]->mid_time_ms;
            v->looping = instrumentlist[i]->loop;
            // The sample slot may be empty or not loaded yet: nothing to play then
            auto* smp = GetAt(&samplelist, instrumentlist[i]->sample_index);
            auto* snd = smp != nullptr ? geptr->GetSound(smp->sound_index) : nullptr;
            if (snd == nullptr)
                return;
            v->sound = snd;

            v->cutoff = instrumentlist[i]->cutoff;
            v->resonance = instrumentlist[i]->resonance;
            v->filter = instrumentlist[i]->filter;
            v->my_bpm = bpm.load();

            // Prepare synth queueing variables
            double frames_per_tick = FramesPerTick();
            v->frame_counter = frames_per_tick;
            v->frames_per_tick = frames_per_tick;

            // Read and apply instrument functions
            GetFX(channel_index, channellist[channel_index].type, -9999, fx1, fx1_p, fx2, fx2_p, fx3, fx3_p, after);

            // Read and apply table effects
            if (t != nullptr)
            {
                channellist[channel_index].skip_next_fx = true;
                // Read and apply instrument functions
                int table_fx1 = t->arr[0][1];
                int table_fx1_p = t->arr[0][2];
                int table_fx2 = t->arr[0][3];
                int table_fx2_p = t->arr[0][4];
                int table_fx3 = t->arr[0][5];
                int table_fx3_p = t->arr[0][6];
                GetFX(channel_index, channellist[channel_index].type, -9999, table_fx1, table_fx1_p, table_fx2, table_fx2_p, table_fx3, table_fx3_p, after);
            }

            // Signal the voice to play
            v->will_start_playing = true;
        }
    }
	// Should we apply a fequency change to the channel or an effect? If yes, then apply it
    else if (f != -9999 || (fx1 != -1 && fx1_p != -1) || (fx2 != -1 && fx2_p != -1) || (fx3 != -1 && fx3_p != -1))
    {
        // Get playing chain tranposition
        if (f != -9999 && play_context != pt_phrase && play_context != pt_phrase_all && play_context != pt_preview && running && !pause_song)
            ApplyChainTransposition(channel_index, &f);

        // Read and apply instrument functions
        GetFX(channel_index, channellist[channel_index].type, f, fx1, fx1_p, fx2, fx2_p, fx3, fx3_p, after);
    }
}

// Update playing step - Pitch
// channelnumber : The particular channel to play the step on
// playing_phrase : Phrase to play
// step_ptr : Phrase progress index
// tt : Table transposition to apply
// freq : Frequency to update
void UpdateStepPitch(int channel_index, int playing_phrase, int step_ptr, int tt, double* new_freq)
{
    // Get frequency to play
    auto f = channellist[channel_index].playing_freq.load();
    auto i = channellist[channel_index].playing_instrument.load();
    // If no note is present, no need to play
    if (f != -9999 && GetAt(&instrumentlist, i) != nullptr)
    {
        // Apply playing chain transposition
        if (play_context != pt_phrase && play_context != pt_phrase_all && running && !pause_song)
            ApplyChainTransposition(channel_index, &f);
        // Get playing table transposition
        ApplyTableTransposition(channel_index, &f, tt);
        // Update the frequency change that will be written later on
        (*new_freq) = NoteFreq(f) + instrumentlist[i]->detune;
    }
}

// Play step chain
// channelnumber : The particular channel to play the step on
// playing_chain : Chain to play
// phrase_ptr : Chain progress index
// step_ptr : Phrase progress index
void PlayStepChain(int channel_index, int playing_chain, int phrase_ptr, int step_ptr)
{
    // Play the step at this chain position
    auto play_phrase = chainlist[playing_chain]->arr[phrase_ptr];
    if (play_phrase != -1 && GetAt(&phraselist, play_phrase) != nullptr)
        PlayStepPhrase(channel_index, play_phrase, step_ptr);
}

// Play step song
// channelnumber : The particular channel to play the step on
// chain_ptr : Song progress index
// phrase_ptr : Chain progress index
// step_ptr : Phrase progress index
void PlayStepSong(int channel_index, int chain_ptr, int phrase_ptr, int step_ptr)
{
    // Play the step at this song position
    auto play_chain = songgrid[chain_ptr][channel_index];
    if (play_chain != -1 && GetAt(&chainlist, play_chain) != nullptr)
        PlayStepChain(channel_index, play_chain, phrase_ptr, step_ptr);
}

// Apply playing chain transposition - Implementation
// channel_index : The channel to get the transposition from
// f : The original frequency
void ApplyChainTransposition(int channel_index, int* f)
{
    int playing_chain_index = play_context == pt_song ? songgrid[channellist[channel_index].chain_ptr.load()][channel_index] : channellist[channel_index].playing_chain.load();
    // If the context is not within a playing channel, then break
    if (playing_chain_index == -1)
        return;

    // Get transposition from the chain
    auto* playing_chain_ptr = GetAt(&chainlist, playing_chain_index);
    const int chain_step = channellist[channel_index].phrase_ptr;
    if (playing_chain_ptr == nullptr || chain_step < 0 || chain_step >= chain::length)
        return;
    int transpose = playing_chain_ptr->arr_transpose[chain_step];

    // Mirror transpose along 0 such that FFFF becomes -1, FFFE becomes -2, etc. until 8001 is -32767, and 8000 is 32768, and 7FFF is 32767, 
    // and 0000 is 0, and 0001 is 1, and 0002 is 2, etc.
    if (transpose > 0x8000)
        transpose = -((0x8000 - transpose) + 0x8000);
    (*f) += transpose;
    // Wrap f into the note range
    WrapNote(f);
}

// Apply playing table transposition - Implementation
// channel_index : The channel to get the transposition from
// f : The original frequency
// tt : The table transposition to apply
void ApplyTableTransposition(int channel_index, int* f, int tt)
{
    // Get transposition sources and add them together
    int transpose = tt;
    // Mirror transpose along 0 such that FFFF becomes -1, FFFE becomes -2, etc. until 8001 is -32767, and 8000 is 32768, and 7FFF is 32767, 
    // and 0000 is 0, and 0001 is 1, and 0002 is 2, etc.
    if (transpose > 0x8000)
        transpose = -((0x8000 - transpose) + 0x8000);
    (*f) += transpose;
    // Wrap f into the note range
    WrapNote(f);
}

// Deep Copy Phrase
// phrase_index : The ID of the phrase
int DeepCopyPhrase(int phrase_index)
{
    // Try get the phrase 
    auto phrase_to_copy = GetAt(&phraselist, phrase_index);

    // If the phrase exists, recreate it and return the new index
    if (phrase_to_copy != nullptr)
    {
        // Create new
        phrase* target_phrase = new phrase();
        // Copy the phrase
        for (int i = 0; i < phrase::len_x; i++)
            for (int q = 0; q < phrase::len_y; q++)
                target_phrase->arr[q][i] = phrase_to_copy->arr[q][i];
        // Insert the phrase into the phraselist
        auto n = GetNextEmpty(&phraselist);
        InsertAt(&phraselist, n, target_phrase);
        // Return the location of the new phrase
        return n;
    }
    else
    {
        return -1;
    }
}

// Shallow Copy Chain
// chain_index : The ID of the chain
int ShallowCopyChain(int chain_index)
{
    // Try get the chain 
    auto chain_to_copy = GetAt(&chainlist, chain_index);

    // If the chain exists, recreate it and return the new index
    if (chain_to_copy != nullptr)
    {
        // Create new
        chain* target_chain = new chain();
        // Copy the chain
        for (int i = 0; i < chain::length; i++)
        {
            // Copy values
            target_chain->arr[i] = chain_to_copy->arr[i];
            target_chain->arr_transpose[i] = chain_to_copy->arr_transpose[i];
        }
        // Insert the chain into the chainlist
        auto n = GetNextEmpty(&chainlist);
        InsertAt(&chainlist, n, target_chain);
        // Return the location of the new chain
        return n;
    }
    else
    {
        return -1;
    }
}

// Deep Copy Chain
// chain_index : The ID of the chain
int DeepCopyChain(int chain_index)
{
    // First Shallow Copy the Chain
    int r = ShallowCopyChain(chain_index);

    // Check return value
    if (r != -1)
    {
        std::vector<int> copied_phrases_key;
        std::vector<int> copied_phrases_value;
        // Clone the phrases within the chain
        for (int i = 0; i < chain::length; i++)
        {
            // Has this phrase already been copied?
            if (std::find(copied_phrases_key.begin(), copied_phrases_key.end(), chainlist[r]->arr[i]) != copied_phrases_key.end())
                chainlist[r]->arr[i] = copied_phrases_value[std::find(copied_phrases_key.begin(), copied_phrases_key.end(), chainlist[r]->arr[i]) - copied_phrases_key.begin()];
            // Only clone if the index exists
            else if (GetAt(&phraselist, chainlist[r]->arr[i]) != nullptr)
            {
				int to_copy = chainlist[r]->arr[i];
                chainlist[r]->arr[i] = DeepCopyPhrase(to_copy);
                copied_phrases_key.push_back(to_copy);
                copied_phrases_value.push_back(chainlist[r]->arr[i]);
            }
            // Insert new if not empty but not exist
            else if (chainlist[r]->arr[i] != -1)
            {
                int to_copy = chainlist[r]->arr[i];
                chainlist[r]->arr[i] = GetNextEmpty(&phraselist);
                copied_phrases_key.push_back(to_copy);
                copied_phrases_value.push_back(chainlist[r]->arr[i]);
            }
        }
    }

    // Return the chain index
    return r;
}

// Beats-per-minute to Tick length in nanoseconds
// b : bpm
double BpmToTicklength(int b)
{
    // 295 BPM * 6 TPS * 4 Steps per Beat = 7080 ticks ber minute
    // 7080 ticks per minute / 60 seconds = 118 ticks per second
    // = 1 tick every 1/118th of a second
    // TickLength = 1000000000 / 118 nano seconds
    return 1000000000.0 / GravityTicksPerSecond(b, tps);
}

// A package of all the changes that will be applied to a voice during a substep. The audio thread will read these values and apply them to the voice.
// int channel_index : channel to simulate
void SubStepTickHelper(int channel_index)
{
    double new_freq = 0.0;
    double new_pan = 0.5;
    double new_vol = 1.0;
    double new_ctf = 1.0;
    double new_res = 0.5;
    double new_pw = 0.5;

    // Staging variables for the changes that will be written to the synths and samples
    if (channellist[channel_index].type == ChannelType::synth)
    {
        new_freq = audiosynth->voices[channel_index]->stg_base_freq;
        new_pan = audiosynth->voices[channel_index]->stg_panning;
        new_vol = audiosynth->voices[channel_index]->stg_volume;
        new_ctf = audiosynth->voices[channel_index]->stg_cutoff;
        new_res = audiosynth->voices[channel_index]->stg_resonance;
        new_pw = audiosynth->voices[channel_index]->stg_pulse_width;
    }
    else
    {
        new_freq = audiosampler->voices[channel_index]->stg_step_freq;
        new_pan = audiosampler->voices[channel_index]->stg_panning;
        new_vol = audiosampler->voices[channel_index]->stg_volume;
        new_ctf = audiosampler->voices[channel_index]->stg_cutoff;
        new_res = audiosampler->voices[channel_index]->stg_resonance;
    }

    // Get all changes to the sound
    if (!pause_song || play_context == pt_preview)
        channellist[channel_index].sub_step(&new_freq);
    if (channellist[channel_index].type == ChannelType::synth) audiosynth->SynthAutomation(&new_freq, &new_pan, &new_pw, &new_vol, &new_ctf, &new_res, channel_index); // Run synth automation on animated variables
    if (channellist[channel_index].type == ChannelType::file) audiosampler->SampleAutomation(&new_freq, &new_pan, &new_vol, &new_ctf, &new_res, channel_index); // Run sample automation on animated variables

    // Submit changes throughout the substep
    if (channellist[channel_index].type == ChannelType::synth)
    {
        live_change s =
        {
            new_freq // Frequency changes,
            , new_pan // Panning changes,
            , new_vol // Volume changes,
            , new_pw // Pulse width changes,
            , new_ctf// Cutoff changes,
            , new_res // Resonance changes
        };
        audiosynth->voices[channel_index]->live_changes.push(s);
    }
    else
    {
        live_change s =
        {
            new_freq // Frequency changes,
            , new_pan // Panning changes,
            , new_vol // Volume changes,
            , new_ctf // Cutoff changes,
            , new_res // Resonance changes
        };
        audiosampler->voices[channel_index]->live_changes.push(s);
    }
}

// Execute tick
// ------------
// This is the tick loop; All song execution should go inside this function
// Table>Phrase>Chain>Song <- Per channel
void DoTick()
{
    std::scoped_lock lock(audiosampler->batch_mutex, audiosynth->batch_mutex);

    // Do Sub-step Code --

    // Tick the channel sequencers
    for (int i = 0; i < channelcount; i++)
    {
        if ((play_context == pt_preview || play_context == pt_phrase || play_context == pt_chain) && i != playing_channel)
            continue;
        if ((play_context == pt_song || play_context == pt_phrase_all || play_context == pt_chain_all) && channellist[i].cant_play)
            continue;
        if (channellist[i].delay_ticks > 0)
        {
            channellist[i].delay_ticks--;
            channellist[i].catchup_ticks++;
            // Run anything on this channel that should run regardless of delay
            SubStepTickHelper(i);
            // Continue to next channel
            continue;
        }

        // Save the ticknumber in case we need to catch up
		int saved_ticknumber = ticknumber;

        // Does a catchup need to occur?
        if (channellist[i].catchup_ticks > 0)
        {
			ticknumber = channellist[i].delay_tick_offsetter;
        }

        // Advance
        do
        {
            // Do not simulate before effects on preview
            if (play_context != pt_preview)
            {
                // Handle different depending on whether this is real-time or we are catching up to real-time.
                if (ticknumber % std::max(1, tps) == 0)
                {
                    // Phrase before FX
                    auto fx1 = phraselist[channellist[i].playing_phrase]->arr[channellist[i].step_ptr][2];
                    auto fx1_p = phraselist[channellist[i].playing_phrase]->arr[channellist[i].step_ptr][3];
                    auto fx2 = phraselist[channellist[i].playing_phrase]->arr[channellist[i].step_ptr][4];
                    auto fx2_p = phraselist[channellist[i].playing_phrase]->arr[channellist[i].step_ptr][5];
                    auto fx3 = phraselist[channellist[i].playing_phrase]->arr[channellist[i].step_ptr][6];
                    auto fx3_p = phraselist[channellist[i].playing_phrase]->arr[channellist[i].step_ptr][7];
                    GetFX(i, channellist[i].type, -9999, fx1, fx1_p, fx2, fx2_p, fx3, fx3_p, before);
                }

                // Table before FX
                if (channellist[i].playing_table != -1)
                {
                    auto fx1 = tablelist[channellist[i].playing_table]->arr[channellist[i].tick_ptr][1];
                    auto fx1_p = tablelist[channellist[i].playing_table]->arr[channellist[i].tick_ptr][2];
                    auto fx2 = tablelist[channellist[i].playing_table]->arr[channellist[i].tick_ptr][3];
                    auto fx2_p = tablelist[channellist[i].playing_table]->arr[channellist[i].tick_ptr][4];
                    auto fx3 = tablelist[channellist[i].playing_table]->arr[channellist[i].tick_ptr][5];
                    auto fx3_p = tablelist[channellist[i].playing_table]->arr[channellist[i].tick_ptr][6];
                    GetFX(i, channellist[i].type, -9999, fx1, fx1_p, fx2, fx2_p, fx3, fx3_p, before);
                }
            }

			// If we encountered a delay, we need to break out of this channel's execution
            if (channellist[i].delay_ticks > 0)
            {
                // Don't forget to account for the first tick skipped!
                channellist[i].delay_ticks--;
                channellist[i].catchup_ticks++;
                SubStepTickHelper(i);
                break;
            }

			// Handle different depending on whether this is real-time or we are catching up to real-time.
            if (ticknumber % std::max(1, tps) == 0)
            {
                // Step the channel sequencers
                if (!pause_song) channellist[i].step();
                if (channellist[i].catchup_ticks > 0) SubStepTickHelper(i);
            }

            // Idle voices need no automation and no queue pushes. They used to be pushed to on every tick
            // forever (filling their 1024-entry queues, and sweeping stg_swpd_freq off to Inf/0).
            if (channellist[i].type == ChannelType::synth)
            {
                if (!audiosynth->voices[i]->start_playing && !audiosynth->voices[i]->will_start_playing && channellist[i].catchup_ticks <= 0)
                    continue;
            }
            else if (!audiosampler->voices[i]->start_playing && !audiosampler->voices[i]->will_start_playing && channellist[i].catchup_ticks <= 0)
                continue;

			// Do not run tick-level automation stuff on catchup ticks - This is to be played relative to the start of a step, so if the step is delayed, the automation should be delayed as well.
            if (channellist[i].catchup_ticks <= 0)
            {
                SubStepTickHelper(i);
            }

			// Decrement catchup ticks
            if (channellist[i].catchup_ticks > 0)
            {
                ticknumber++;
                channellist[i].catchup_ticks--;
            }
		} while (channellist[i].catchup_ticks > 0);

        // Restore ticknumber
        ticknumber = saved_ticknumber;
    }

    // Increment global song position in ticks --
    if (!pause_song) ticknumber++;
}

// Convert i to hex string
// int i : Number to convert
std::string IntToHexString(int i)
{
    // Uppercase hex of the two's-complement value, same output as the old ostringstream + toupper loop
    char buf[16];
    snprintf(buf, sizeof(buf), "%X", (unsigned)i);
    return buf;
}

// Draw Options Editor UI
void DrawOptionsUI()
{
    // Menu title
    geptr->DrawTextString(0, 1, geptr->entity, "OPTIONS", primary_text_a);

    // Draw all the settings
    int options_count = global_settings->setting_labels.size();
    for (int i = 0; i < options_count; i++)
    {
        // Draw the setting
        geptr->DrawTextString(0, 3 + i, geptr->entity, global_settings->setting_labels[i], primary_text_a);
        // Draw the value
        if (global_settings->setting_types[i] == settings::fourbithex)
        {
            geptr->DrawChar(4, 3 + i, geptr->entity, ':');
            geptr->DrawSetColor(4, 3 + i, geptr->entity, primary_text_a);
            auto outstr = IntToHexString(*global_settings->setting_pointers[i]);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            geptr->DrawTextString(6, 3 + i, geptr->entity, outstr, primary_text_a);
        }
    }
}

// Draw Song Editor UI
// off_x : UI offset on the x axis
// off_y : UI offset on the y axis
void DrawSongUI(int off_x, int off_y)
{
    // Width and height of the screen
    int h = song_grid_h;
    int w = song_grid_w;

    // Song position pointer
    if (play_context == pt_song &&
        pause_song == false)
    {
        // Loop through all visible channels
        for (int x = 0; x < w && x + off_x < channelcount; x++)
        {
            // Only draw if the channel has reported that it is allowed to play
            if (channellist[x + off_x].cant_play == false)
            {
                geptr->DrawChar(4 + x * 5, 3 + channellist[x + off_x].chain_ptr, geptr->entity, '>');
                geptr->DrawSetColor(4 + x * 5, 3 + channellist[x + off_x].chain_ptr, geptr->entity, primary_text_a);
            }
        }
    }

    // Menu title
    geptr->DrawTextString(0, 1, geptr->entity, "SONG", primary_text_a);

    // Draw the row numbers
    for (int i = 0; i < h && i + off_y < rowcount; i++)
    {
        int tempint = i + off_y;

        auto upperstr = IntToHexString(tempint);

        upperstr.insert(upperstr.begin(), 4 - upperstr.size(), '0');
        geptr->DrawTextString(0, 3 + i, geptr->entity, upperstr, primary_text_a);
    }

    // Channel headers
    for (int i = 0; i < w && i + off_x < channelcount; i++)
    {
        std::string tempstr = std::to_string(i + off_x);
        tempstr.insert(tempstr.begin(), 2 - tempstr.size(), '0');
        geptr->DrawTextString(5 + i * 5, 2, geptr->entity, "CH" + tempstr, header_text_a);
    }

    //Draw the song grid
    for (int y = 0; y < h && y + off_y < rowcount; y++)
    {
        for (int x = 0; x < w && x + off_x < channelcount; x++)
        {
            // Is the cursor currently hovering this cell? Set color accordingly
            color thiscolor = (x == cursor_x && y == cursor_y ? primary_text_b : primary_text_a);
            // Get the current song grid value
            int chain = songgrid[y + off_y][x + off_x];
            if (chain == -1)
            {
                // Draw null chain
                geptr->DrawTextString(5 + x * 5, 3 + y, geptr->entity, "----", thiscolor);
            }
            else
            {
                // Draw chain number
                auto outstr = IntToHexString(chain);
                outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                geptr->DrawTextString(5 + x * 5, 3 + y, geptr->entity, outstr, thiscolor);
            }

            // Is a modifier key held on the selected cell?
            if (x == cursor_x && y == cursor_y)
            {
                // Modify right part of the number
                if (leftrightcenter == left)
                {
                    geptr->DrawSetColor(5 + x * 5, 3 + y, geptr->entity, primary_text_a);
                    geptr->DrawSetColor(5 + x * 5 + 1, 3 + y, geptr->entity, primary_text_a);
                }
                // Modify left part of the number
                else if (leftrightcenter == right)
                {
                    geptr->DrawSetColor(5 + x * 5 + 2, 3 + y, geptr->entity, primary_text_a);
                    geptr->DrawSetColor(5 + x * 5 + 3, 3 + y, geptr->entity, primary_text_a);
                }
            }
        }
    }
}

// Draw Chain Editor UI
// off_y : UI offset on the y axis
void DrawChainUI(int off_y)
{
    // Width and height of the screen
    int h = chain_grid_h;
    int w = 1;

    // Song position pointer
    if (((open_channel == playing_channel && open_chain_index == channellist[playing_channel].chain_ptr)
        || (channellist[open_channel].chain_ptr == open_chain_index && channellist[open_channel].cant_play == false && play_context == pt_song)) &&
        play_context != pt_phrase && play_context != pt_phrase_all &&
        pause_song == false)
    {
        geptr->DrawChar(4, 3 + channellist[playing_channel].phrase_ptr, geptr->entity, '>');
        geptr->DrawSetColor(4, 3 + channellist[playing_channel].phrase_ptr, geptr->entity, primary_text_a);
    }

    // Draw menu title and chain number
    auto outstr = IntToHexString(open_chain);
    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
    geptr->DrawTextString(0, 1, geptr->entity, "CHAIN - " + outstr, primary_text_a);

    // Draw the row numbers
    for (int i = 0; i < h && i + off_y < rowcount; i++)
    {
        int tempint = i + off_y;

        auto upperstr = IntToHexString(tempint);

        upperstr.insert(upperstr.begin(), 4 - upperstr.size(), '0');
        geptr->DrawTextString(0, 3 + i, geptr->entity, upperstr, primary_text_a);
    }

    // Channel headers
    geptr->DrawTextString(5, 2, geptr->entity, "PHSE", header_text_a);
    geptr->DrawTextString(10, 2, geptr->entity, "TRPS", header_text_a);

    //Draw the chain grid
    for (int y = 0; y < h && y + off_y < rowcount; y++)
    {
        // Is the cursor currently hovering this cell? Set color accordingly
        color thiscolor = (0 == cursor_x && y == cursor_y ? primary_text_b : primary_text_a);
        color thiscolor_t = (1 == cursor_x && y == cursor_y ? primary_text_b : primary_text_a);

        // Get the current chain grid value
        int chain = chainlist[open_chain]->arr[y + off_y];
        if (chain == -1)
        {
            // Draw null chain
            geptr->DrawTextString(5, 3 + y, geptr->entity, "----", thiscolor);
        }
        else
        {
            // Draw chain number
            auto outstr = IntToHexString(chain);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            geptr->DrawTextString(5, 3 + y, geptr->entity, outstr, thiscolor);
        }

        // Draw chain transposition
        int chain_trsp = chainlist[open_chain]->arr_transpose[y + off_y];
        auto outstr = IntToHexString(chain_trsp);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(10, 3 + y, geptr->entity, outstr, thiscolor_t);

        // Is a modifier key held on the selected cell?
        if (y == cursor_y)
        {
            int trsp_offset = 5 * (1 == cursor_x);
            // Modify right part of the number
            if (leftrightcenter == left)
            {
                geptr->DrawSetColor(5 + trsp_offset, 3 + y, geptr->entity, primary_text_a);
                geptr->DrawSetColor(6 + trsp_offset, 3 + y, geptr->entity, primary_text_a);
            }
            // Modify left part of the number
            else if (leftrightcenter == right)
            {
                geptr->DrawSetColor(7 + trsp_offset, 3 + y, geptr->entity, primary_text_a);
                geptr->DrawSetColor(8 + trsp_offset, 3 + y, geptr->entity, primary_text_a);
            }
        }
    }
}

// Converts an integer to a note string
// int note : Piano key number
std::string IntToNoteString(int n)
{
    // Get the octave
    int octave = ((n - 3) >= 0 ? std::floor((n - 3) / 12) + 1 : 0);
    char octave_char = 48 + octave;
    // Get the note letter
    int note_int = (n >= 0 ? n : n + 12) % 12;
    std::string note_char = "";
    // Get the note string
    switch (note_int)
    {
    case 0:
        note_char = "A-";
        break;
    case 1:
        note_char = "A#";
        break;
    case 2:
        note_char = "B-";
        break;
    case 3:
        note_char = "C-";
        break;
    case 4:
        note_char = "C#";
        break;
    case 5:
        note_char = "D-";
        break;
    case 6:
        note_char = "D#";
        break;
    case 7:
        note_char = "E-";
        break;
    case 8:
        note_char = "F-";
        break;
    case 9:
        note_char = "F#";
        break;
    case 10:
        note_char = "G-";
        break;
    case 11:
        note_char = "G#";
        break;
    }

    // Build string
    std::string s = "";
    s += note_char;
    s += octave_char;
    // Return string
    return s;
}

// Draw Phrase Editor UI
// off_y : UI offset on the y axis
void DrawPhraseUI(int off_y)
{
    // Width and height of the screen
    int h = phrase_grid_h;
    int w = phrase_grid_w;

    // Song position pointer
    if (((open_phrase_index == channellist[playing_channel].phrase_ptr &&
        open_chain_index == channellist[playing_channel].chain_ptr && open_channel == playing_channel) ||
        (channellist[open_channel].phrase_ptr == open_phrase_index && channellist[open_channel].chain_ptr == open_chain_index &&
            channellist[open_channel].cant_play == false && play_context == pt_song)
        ) &&
        pause_song == false)
    {
        geptr->DrawChar(4, 3 + channellist[playing_channel].step_ptr, geptr->entity, '>');
        geptr->DrawSetColor(4, 3 + channellist[playing_channel].step_ptr, geptr->entity, primary_text_a);
    }

    // Menu title and phrase number
    auto outstr = IntToHexString(open_phrase);
    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
    geptr->DrawTextString(0, 1, geptr->entity, "PHRASE - " + outstr, primary_text_a);

    // Draw the row numbers
    for (int i = 0; i < h && i + off_y < rowcount; i++)
    {
        int tempint = i + off_y;

        auto upperstr = IntToHexString(tempint);

        upperstr.insert(upperstr.begin(), 4 - upperstr.size(), '0');
        geptr->DrawTextString(0, 3 + i, geptr->entity, upperstr, primary_text_a);
    }

    // Channel headers
    geptr->DrawTextString(5, 2, geptr->entity, "NTE", header_text_a);
    geptr->DrawTextString(9, 2, geptr->entity, "ISTR", header_text_a);
    geptr->DrawTextString(14, 2, geptr->entity, "EFFT1", header_text_a);
    geptr->DrawTextString(20, 2, geptr->entity, "EFFT2", header_text_a);
    geptr->DrawTextString(26, 2, geptr->entity, "EFFT3", header_text_a);

    //Draw the phrase grid
    for (int y = 0; y < h && y + off_y < rowcount; y++)
    {
        // Note -
        auto thiscolor = cursor_y == y && cursor_x == 0 ? primary_text_b : primary_text_a;

        // Get the current phrase grid value
        int note = phraselist[open_phrase]->arr[y + off_y][0];
        if (note == -9999)
        {
            // Draw null note
            geptr->DrawTextString(5, 3 + y, geptr->entity, "---", thiscolor);
        }
        else
        {
            // Draw note
            geptr->DrawTextString(5, 3 + y, geptr->entity, IntToNoteString(note), thiscolor);
        }

        // Instrument -
        thiscolor = cursor_y == y && cursor_x == 1 ? primary_text_b : primary_text_a;

        // Get the current phrase grid value
        int instr = phraselist[open_phrase]->arr[y + off_y][1];
        if (instr == -1)
        {
            // Draw null instrument
            geptr->DrawTextString(9, 3 + y, geptr->entity, "----", thiscolor);
        }
        else
        {
            // Draw instrument number
            auto outstr = IntToHexString(instr);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            geptr->DrawTextString(9, 3 + y, geptr->entity, outstr, thiscolor);
        }

        // Modify right part of the number
        if (leftrightcenter == left)
        {
            geptr->DrawSetColor(9, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(10, 3 + y, geptr->entity, primary_text_a);
        }
        // Modify left part of the number
        else if (leftrightcenter == right)
        {
            geptr->DrawSetColor(11, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(12, 3 + y, geptr->entity, primary_text_a);
        }

        // FX1 -
        thiscolor = cursor_y == y && cursor_x == 2 ? header_text_b : header_text_a;

        // Get the current phrase grid value
        int effect1 = phraselist[open_phrase]->arr[y + off_y][2];
        if (effect1 == -1)
        {
            // Draw null effect
            geptr->DrawTextString(14, 3 + y, geptr->entity, "-", thiscolor);
        }
        else
        {
            // Draw effect char
            std::string outchr = "";
            outchr += fx[effect1];
            geptr->DrawTextString(14, 3 + y, geptr->entity, outchr, thiscolor);
        }

        // FX1 Param -
        thiscolor = cursor_y == y && cursor_x == 3 ? primary_text_b : primary_text_a;

        // Get the current phrase grid value
        int effect1_param = phraselist[open_phrase]->arr[y + off_y][3];
        if (effect1_param == -1)
        {
            // Draw null parameter
            geptr->DrawTextString(15, 3 + y, geptr->entity, "----", thiscolor);
        }
        else
        {
            // Draw effect parameter
            auto outstr = IntToHexString(effect1_param);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            geptr->DrawTextString(15, 3 + y, geptr->entity, outstr, thiscolor);
        }

        // Modify right part of the number
        if (leftrightcenter == left)
        {
            geptr->DrawSetColor(15, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(16, 3 + y, geptr->entity, primary_text_a);
        }
        // Modify left part of the number
        else if (leftrightcenter == right)
        {
            geptr->DrawSetColor(17, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(18, 3 + y, geptr->entity, primary_text_a);
        }

        // FX2 -
        thiscolor = cursor_y == y && cursor_x == 4 ? header_text_b : header_text_a;

        // Get the current phrase grid value
        int effect2 = phraselist[open_phrase]->arr[y + off_y][4];
        if (effect2 == -1)
        {
            // Draw null effect
            geptr->DrawTextString(20, 3 + y, geptr->entity, "-", thiscolor);
        }
        else
        {
            // Draw effect char
            std::string outchr = "";
            outchr += fx[effect2];
            geptr->DrawTextString(20, 3 + y, geptr->entity, outchr, thiscolor);
        }

        // FX2 Param -
        thiscolor = cursor_y == y && cursor_x == 5 ? primary_text_b : primary_text_a;

        // Get the current phrase grid value
        int effect2_param = phraselist[open_phrase]->arr[y + off_y][5];
        if (effect2_param == -1)
        {
            // Draw null parameter
            geptr->DrawTextString(21, 3 + y, geptr->entity, "----", thiscolor);
        }
        else
        {
            // Draw effect parameter
            auto outstr = IntToHexString(effect2_param);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            geptr->DrawTextString(21, 3 + y, geptr->entity, outstr, thiscolor);
        }

        // Modify right part of the number
        if (leftrightcenter == left)
        {
            geptr->DrawSetColor(21, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(22, 3 + y, geptr->entity, primary_text_a);
        }
        // Modify left part of the number
        else if (leftrightcenter == right)
        {
            geptr->DrawSetColor(23, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(24, 3 + y, geptr->entity, primary_text_a);
        }

        // FX3 -
        thiscolor = cursor_y == y && cursor_x == 6 ? header_text_b : header_text_a;

        // Get the current phrase grid value
        int effect3 = phraselist[open_phrase]->arr[y + off_y][6];
        if (effect3 == -1)
        {
            // Draw null effect
            geptr->DrawTextString(26, 3 + y, geptr->entity, "-", thiscolor);
        }
        else
        {
            // Draw effect char
            std::string outchr = "";
            outchr += fx[effect3];
            geptr->DrawTextString(26, 3 + y, geptr->entity, outchr, thiscolor);
        }

        // FX3 Param -
        thiscolor = cursor_y == y && cursor_x == 7 ? primary_text_b : primary_text_a;

        // Get the current phrase grid value
        int effect3_param = phraselist[open_phrase]->arr[y + off_y][7];
        if (effect3_param == -1)
        {
            // Draw null parameter
            geptr->DrawTextString(27, 3 + y, geptr->entity, "----", thiscolor);
        }
        else
        {
            // Draw effect parameter
            auto outstr = IntToHexString(effect3_param);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            geptr->DrawTextString(27, 3 + y, geptr->entity, outstr, thiscolor);
        }

        // Modify right part of the number
        if (leftrightcenter == left)
        {
            geptr->DrawSetColor(27, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(28, 3 + y, geptr->entity, primary_text_a);
        }
        // Modify left part of the number
        else if (leftrightcenter == right)
        {
            geptr->DrawSetColor(29, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(30, 3 + y, geptr->entity, primary_text_a);
        }
    }
}

// Draw Instrument Editor UI
void DrawInstrumentUI()
{
    // Menu title and instrument number
    auto outstr = IntToHexString(open_instrument);
    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
    geptr->DrawTextString(0, 1, geptr->entity, "INSTR - " + outstr, primary_text_a);

    // Editor body

    // Inits
    instrument_edit_y = -1;
    instrument_edit_digit_count = -1;

    // Draw instrument options
    int ty = 3;

    // Channel Type
    geptr->DrawTextString(0, ty, geptr->entity, "TYP:", primary_text_a); // Synth or Sample
    outstr = instrumentlist[open_instrument]->type == ChannelType::synth ? "SYNTH" : "FILE";
    geptr->DrawTextString(instrumentlist[open_instrument]->type == ChannelType::synth ? 5 : 7, ty, geptr->entity, outstr,
        cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
    if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 0; }
    ty++;

    // Draw synth UI
    if (instrumentlist[open_instrument]->type == ChannelType::synth)
    {
        // Waveform
        geptr->DrawTextString(0, ty, geptr->entity, "WAV:", primary_text_a); // Sine, Square, Saw, etc.
        outstr = waveform_to_string[instrumentlist[open_instrument]->waveform];
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 0; }
        ty++;
        // Volume
        geptr->DrawTextString(0, ty, geptr->entity, "VOL:", primary_text_a); // Volume : 0x00 = 0, 0xFF = 1 | Fade : 0x80 = 0, 0x00 = -128, 0xFF = 127
        outstr = IntToHexString(instrumentlist[open_instrument]->volume_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;
        // Panning
        geptr->DrawTextString(0, ty, geptr->entity, "PAN:", primary_text_a); // Pan : 0x00 = 0, 0x80 = 0.5, 0xFF = 1 | Pan Mod : 0x00 = 1, 0xFF = X units per tick
        outstr = IntToHexString(instrumentlist[open_instrument]->pan_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;
        // Pulse width
        geptr->DrawTextString(0, ty, geptr->entity, "WID:", primary_text_a); // 0x00 = 0, 0x80 = 0.5, 0xFF = 1 | Pulse Width Mod : 0x00 = 1, 0xFF = X units per tick
        outstr = IntToHexString(instrumentlist[open_instrument]->pw_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;
        // Tuning
        geptr->DrawTextString(0, ty, geptr->entity, "TUN:", primary_text_a); // 0x80 = 0, 0x00 = -128, 0xFF = 127 semitones
        outstr = IntToHexString(instrumentlist[open_instrument]->detune_edit);
        outstr.insert(outstr.begin(), 2 - outstr.size(), '0');
        geptr->DrawTextString(5 + 2, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 1; }
        ty++;
        // Pitch sweep
        geptr->DrawTextString(0, ty, geptr->entity, "SWP:", primary_text_a); // 0x80 = 0, 0x00 = -32768, 0xFF = 32767 units per tick
        outstr = IntToHexString(instrumentlist[open_instrument]->pitch_freq_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;

        // Filter type
        ty++;
        geptr->DrawTextString(0, ty, geptr->entity, "FLT:", primary_text_a); // Lowpass, Bandpass, Highpass, None
        outstr = instrumentlist[open_instrument]->filter == FilterType::lowpass ? "LOWPASS" :
            instrumentlist[open_instrument]->filter == FilterType::bandpass ? "BANDPASS" :
            instrumentlist[open_instrument]->filter == FilterType::highpass ? "HIGHPASS" : "NONE";
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 4 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 4) { instrument_edit_y = ty; instrument_edit_digit_count = 0; }
        ty++;
        // Filter cutoff
        geptr->DrawTextString(0, ty, geptr->entity, "CTF:", primary_text_a); // Cutoff
        outstr = IntToHexString(instrumentlist[open_instrument]->cutoff_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 4 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 4) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;
        // Filter resonance
        geptr->DrawTextString(0, ty, geptr->entity, "RES:", primary_text_a); // Resonance
        outstr = IntToHexString(instrumentlist[open_instrument]->resonance_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 4 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 4) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;

        // Table reference
        ty++;
        geptr->DrawTextString(0, ty, geptr->entity, "TBL:", primary_text_a); // Table
        outstr = instrumentlist[open_instrument]->table_index == -1 ? "----" : IntToHexString(instrumentlist[open_instrument]->table_index);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 5 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 5) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;

        if (instrument_edit_y != -1 && instrument_edit_digit_count == 2)
        {
            // Modify left part of the number
            if (leftrightcenter == left)
            {
                geptr->DrawSetColor(5 + cursor_x, instrument_edit_y, geptr->entity, primary_text_a);
                geptr->DrawSetColor(6 + cursor_x, instrument_edit_y, geptr->entity, primary_text_a);
            }
            // Modify right part of the number
            if (leftrightcenter == right)
            {
                geptr->DrawSetColor(7 + cursor_x, instrument_edit_y, geptr->entity, primary_text_a);
                geptr->DrawSetColor(8 + cursor_x, instrument_edit_y, geptr->entity, primary_text_a);
            }
        }
    }
    else
    {
        // Sample index
        geptr->DrawTextString(0, ty, geptr->entity, "SPL:", primary_text_a); // File ref
        outstr = instrumentlist[open_instrument]->sample_index == -1 ? "----" : IntToHexString(instrumentlist[open_instrument]->sample_index);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5 + 2, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;
        // Volume
        geptr->DrawTextString(0, ty, geptr->entity, "VOL:", primary_text_a); // Volume : 0x00 = 0, 0xFF = 1 | Fade : 0x80 = 0, 0x00 = -128, 0xFF = 127
        outstr = IntToHexString(instrumentlist[open_instrument]->volume_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5 + 2, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;
        // Panning
        geptr->DrawTextString(0, ty, geptr->entity, "PAN:", primary_text_a); // Pan : 0x00 = 0, 0x80 = 0.5, 0xFF = 1 | Pan Mod : 0x00 = 1, 0xFF = X units per tick
        outstr = IntToHexString(instrumentlist[open_instrument]->pan_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5 + 2, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;
        // Tuning
        geptr->DrawTextString(0, ty, geptr->entity, "TUN:", primary_text_a); // 0x80 = 0, 0x00 = -128, 0xFF = 127 semitones
        outstr = IntToHexString(instrumentlist[open_instrument]->detune_edit);
        outstr.insert(outstr.begin(), 2 - outstr.size(), '0');
        geptr->DrawTextString(5 + 4, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 1; }
        ty++;
        // Pitch sweep
        geptr->DrawTextString(0, ty, geptr->entity, "SWP:", primary_text_a); // 0x80 = 0, 0x00 = -32768, 0xFF = 32767 units per tick
        outstr = IntToHexString(instrumentlist[open_instrument]->pitch_freq_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5 + 2, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;
        // Base pitch
        geptr->DrawTextString(0, ty, geptr->entity, "BSP:", primary_text_a); // 39 = C-4
        outstr = IntToNoteString(instrumentlist[open_instrument]->base_pitch);
        geptr->DrawTextString(5 + 3, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 1.5; }
        ty++;
        // Start time
        geptr->DrawTextString(0, ty, geptr->entity, "BEG:", primary_text_a); // 0x000000 = 0 ms
        outstr = IntToHexString(instrumentlist[open_instrument]->start_time_ms);
        outstr.insert(outstr.begin(), 6 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 3; }
        ty++;
        // mid time
        geptr->DrawTextString(0, ty, geptr->entity, "MID:", primary_text_a); // 0x000000 = 0 ms
        outstr = IntToHexString(instrumentlist[open_instrument]->mid_time_ms);
        outstr.insert(outstr.begin(), 6 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 3; }
        ty++;
        // End time
        geptr->DrawTextString(0, ty, geptr->entity, "END:", primary_text_a); // 0xFFFFFF = 16777215 ms
        outstr = IntToHexString(instrumentlist[open_instrument]->end_time_ms);
        outstr.insert(outstr.begin(), 6 - outstr.size(), '0');
        geptr->DrawTextString(5, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 3; }
        ty++;
        // Loop
        geptr->DrawTextString(0, ty, geptr->entity, "LOP:", primary_text_a); // TRUE, FALSE
        outstr = instrumentlist[open_instrument]->loop == 0 ? "FALSE" : "TRUE";
        geptr->DrawTextString(6 + (int)instrumentlist[open_instrument]->loop, ty, geptr->entity, outstr,
            cursor_y == ty - 3 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 3) { instrument_edit_y = ty; instrument_edit_digit_count = 4; }
        ty++;

        // Filter type
        ty++;
        geptr->DrawTextString(0, ty, geptr->entity, "FLT:", primary_text_a); // Lowpass, Bandpass, Highpass, None
        outstr = instrumentlist[open_instrument]->filter == FilterType::lowpass ? "LOWPASS" :
            instrumentlist[open_instrument]->filter == FilterType::bandpass ? "BANDPASS" :
            instrumentlist[open_instrument]->filter == FilterType::highpass ? "HIGHPASS" : "NONE";
        geptr->DrawTextString(5 + 2, ty, geptr->entity, outstr,
            cursor_y == ty - 4 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 4) { instrument_edit_y = ty; instrument_edit_digit_count = 0; }
        ty++;
        // Filter cutoff
        geptr->DrawTextString(0, ty, geptr->entity, "CTF:", primary_text_a); // Cutoff
        outstr = IntToHexString(instrumentlist[open_instrument]->cutoff_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5 + 2, ty, geptr->entity, outstr,
            cursor_y == ty - 4 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 4) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;
        // Filter resonance
        geptr->DrawTextString(0, ty, geptr->entity, "RES:", primary_text_a); // Resonance
        outstr = IntToHexString(instrumentlist[open_instrument]->resonance_edit);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5 + 2, ty, geptr->entity, outstr,
            cursor_y == ty - 4 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 4) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;

        // Table reference
        ty++;
        geptr->DrawTextString(0, ty, geptr->entity, "TBL:", primary_text_a); // Table
        outstr = instrumentlist[open_instrument]->table_index == -1 ? "----" : IntToHexString(instrumentlist[open_instrument]->table_index);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(5 + 2, ty, geptr->entity, outstr,
            cursor_y == ty - 5 && cursor_x == 0 ? primary_text_b : primary_text_a); // Show value
        if (cursor_y == ty - 5) { instrument_edit_y = ty; instrument_edit_digit_count = 2; }
        ty++;

        if (instrument_edit_y != -1 && instrument_edit_digit_count != 4 && instrument_edit_digit_count >= 2)
        {
            int offset = 0;
            if (alt_mode)
                offset = -2;
            // Modify left part of the number
            if (leftrightcenter == left)
            {
                geptr->DrawSetColor(5 + 2 + cursor_x + offset, instrument_edit_y, geptr->entity, primary_text_a);
                geptr->DrawSetColor(6 + 2 + cursor_x + offset, instrument_edit_y, geptr->entity, primary_text_a);
            }
            // Modify right part of the number
            if (leftrightcenter == right)
            {
                geptr->DrawSetColor(7 + 2 + cursor_x + offset, instrument_edit_y, geptr->entity, primary_text_a);
                geptr->DrawSetColor(8 + 2 + cursor_x + offset, instrument_edit_y, geptr->entity, primary_text_a);
            }
        }
    }
}

// Draw Table Editor UI
// off_y : UI offset on the y axis
void DrawTableUI(int off_y)
{
    // Width and height of the screen
    int h = table_grid_h;
    int w = table_grid_w;

    // // Song position pointer
    // if (((open_phrase_index == channellist[playing_channel].phrase_ptr &&
    //     open_chain_index == channellist[playing_channel].chain_ptr && open_channel == playing_channel) ||
    //     (channellist[open_channel].phrase_ptr == open_phrase_index && channellist[open_channel].chain_ptr == open_chain_index &&
    //         channellist[open_channel].cant_play == false && play_context == pt_song)
    //     ) &&
    //     pause_song == false)
    // {
    //     geptr->DrawChar(4, 3 + channellist[playing_channel].step_ptr, geptr->entity, '>');
    //     geptr->DrawSetColor(4, 3 + channellist[playing_channel].step_ptr, geptr->entity, primary_text_a);
    // }

    // Menu title and table number
    auto outstr = IntToHexString(open_table);
    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
    geptr->DrawTextString(0, 1, geptr->entity, "TABLE - " + outstr, primary_text_a);

    // Draw the row numbers
    for (int i = 0; i < h && i + off_y < rowcount; i++)
    {
        int tempint = i + off_y;

        auto upperstr = IntToHexString(tempint);

        upperstr.insert(upperstr.begin(), 4 - upperstr.size(), '0');
        geptr->DrawTextString(0, 3 + i, geptr->entity, upperstr, primary_text_a);
    }

    // Table headers
    geptr->DrawTextString(5, 2, geptr->entity, "TRSP", header_text_a);
    geptr->DrawTextString(10, 2, geptr->entity, "EFFT1", header_text_a);
    geptr->DrawTextString(16, 2, geptr->entity, "EFFT2", header_text_a);
    geptr->DrawTextString(22, 2, geptr->entity, "EFFT3", header_text_a);

    //Draw the table grid
    for (int y = 0; y < h && y + off_y < rowcount; y++)
    {
        // Note Offset -
        auto thiscolor = cursor_y == y && cursor_x == 0 ? primary_text_b : primary_text_a;

        // Get the current table grid value
        int note = tablelist[open_table]->arr[y + off_y][0];
        if (note == -9999)
        {
            // Draw null pitch
            geptr->DrawTextString(5, 3 + y, geptr->entity, "----", thiscolor);
        }
        else
        {
            // Draw pitch
            auto upperstr = IntToHexString(note);
            upperstr.insert(upperstr.begin(), 4 - upperstr.size(), '0');
            geptr->DrawTextString(5, 3 + y, geptr->entity, upperstr, thiscolor);
        }

        // FX1 -
        thiscolor = cursor_y == y && cursor_x == 1 ? header_text_b : header_text_a;

        // Get the current table grid value
        int effect1 = tablelist[open_table]->arr[y + off_y][1];
        if (effect1 == -1)
        {
            // Draw null effect
            geptr->DrawTextString(10, 3 + y, geptr->entity, "-", thiscolor);
        }
        else
        {
            // Draw effect char
            std::string outchr = "";
            outchr += fx[effect1];
            geptr->DrawTextString(10, 3 + y, geptr->entity, outchr, thiscolor);
        }

        // FX1 Param -
        thiscolor = cursor_y == y && cursor_x == 2 ? primary_text_b : primary_text_a;

        // Get the current table grid value
        int effect1_param = tablelist[open_table]->arr[y + off_y][2];
        if (effect1_param == -1)
        {
            // Draw null parameter
            geptr->DrawTextString(11, 3 + y, geptr->entity, "----", thiscolor);
        }
        else
        {
            // Draw effect parameter
            auto outstr = IntToHexString(effect1_param);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            geptr->DrawTextString(11, 3 + y, geptr->entity, outstr, thiscolor);
        }

        // Modify right part of the number
        if (leftrightcenter == left)
        {
            geptr->DrawSetColor(11, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(12, 3 + y, geptr->entity, primary_text_a);
        }
        // Modify left part of the number
        else if (leftrightcenter == right)
        {
            geptr->DrawSetColor(13, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(14, 3 + y, geptr->entity, primary_text_a);
        }

        // FX2 -
        thiscolor = cursor_y == y && cursor_x == 3 ? header_text_b : header_text_a;

        // Get the current table grid value
        int effect2 = tablelist[open_table]->arr[y + off_y][3];
        if (effect2 == -1)
        {
            // Draw null effect
            geptr->DrawTextString(16, 3 + y, geptr->entity, "-", thiscolor);
        }
        else
        {
            // Draw effect char
            std::string outchr = "";
            outchr += fx[effect2];
            geptr->DrawTextString(16, 3 + y, geptr->entity, outchr, thiscolor);
        }

        // FX2 Param -
        thiscolor = cursor_y == y && cursor_x == 4 ? primary_text_b : primary_text_a;

        // Get the current table grid value
        int effect2_param = tablelist[open_table]->arr[y + off_y][4];
        if (effect2_param == -1)
        {
            // Draw null parameter
            geptr->DrawTextString(17, 3 + y, geptr->entity, "----", thiscolor);
        }
        else
        {
            // Draw effect parameter
            auto outstr = IntToHexString(effect2_param);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            geptr->DrawTextString(17, 3 + y, geptr->entity, outstr, thiscolor);
        }

        // Modify right part of the number
        if (leftrightcenter == left)
        {
            geptr->DrawSetColor(17, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(18, 3 + y, geptr->entity, primary_text_a);
        }
        // Modify left part of the number
        else if (leftrightcenter == right)
        {
            geptr->DrawSetColor(19, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(20, 3 + y, geptr->entity, primary_text_a);
        }

        // FX3 -
        thiscolor = cursor_y == y && cursor_x == 5 ? header_text_b : header_text_a;

        // Get the current table grid value
        int effect3 = tablelist[open_table]->arr[y + off_y][5];
        if (effect3 == -1)
        {
            // Draw null effect
            geptr->DrawTextString(22, 3 + y, geptr->entity, "-", thiscolor);
        }
        else
        {
            // Draw effect char
            std::string outchr = "";
            outchr += fx[effect3];
            geptr->DrawTextString(22, 3 + y, geptr->entity, outchr, thiscolor);
        }

        // FX3 Param -
        thiscolor = cursor_y == y && cursor_x == 6 ? primary_text_b : primary_text_a;

        // Get the current table grid value
        int effect3_param = tablelist[open_table]->arr[y + off_y][6];
        if (effect3_param == -1)
        {
            // Draw null parameter
            geptr->DrawTextString(23, 3 + y, geptr->entity, "----", thiscolor);
        }
        else
        {
            // Draw effect parameter
            auto outstr = IntToHexString(effect3_param);
            outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
            geptr->DrawTextString(23, 3 + y, geptr->entity, outstr, thiscolor);
        }

        // Modify right part of the number
        if (leftrightcenter == left)
        {
            geptr->DrawSetColor(23, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(24, 3 + y, geptr->entity, primary_text_a);
        }
        // Modify left part of the number
        else if (leftrightcenter == right)
        {
            geptr->DrawSetColor(25, 3 + y, geptr->entity, primary_text_a);
            geptr->DrawSetColor(26, 3 + y, geptr->entity, primary_text_a);
        }
    }
}

// Draw Wave Editor UI
void DrawWaveUI()
{
    // Synth or File instrument?
    if (instrumentlist[open_instrument]->type == ChannelType::synth)
    {
        // TODO: Implement UI for waveform editing
    }
    else
    {
        // Menu title and sample number
        auto outstr = IntToHexString(open_sample);
        outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
        geptr->DrawTextString(0, 1, geptr->entity, "SMPLE - " + outstr, primary_text_a);

        // Draw current directory -

        // -= Directory listing (cached) =-
        // This used to scan the folder, sort it (with a stat() per comparison) and rebuild the strings
        // on EVERY frame. Now it is rebuilt when the folder changes or once per second, and it uses the
        // non-throwing filesystem overloads so a folder deleted behind our back can't crash the UI.
        struct FileEntry { std::string path; std::string upper; bool is_dir; bool is_file; };
        static std::vector<FileEntry> listing;
        static std::string listing_dir;
        static std::chrono::steady_clock::time_point listing_time;

        const auto now = std::chrono::steady_clock::now();
        if (listing_dir != current_dir || now - listing_time > std::chrono::seconds(1))
        {
            std::error_code ec;

            // Create sample library if it does not exist
            if (!std::filesystem::exists(current_dir, ec))
                std::filesystem::create_directory(current_dir, ec);

            listing.clear();
            ec.clear();
            for (std::filesystem::directory_iterator it(current_dir, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code entry_ec;
                FileEntry e;
                e.path = it->path().string();
                e.is_dir = it->is_directory(entry_ec);
                e.is_file = it->is_regular_file(entry_ec);
                e.upper = e.path;
                for (auto& c : e.upper)
                    c = (char)toupper((unsigned char)c);
                listing.push_back(std::move(e));
            }

            // Folders first, then alphabetical. (The old comparator wasn't a valid ordering for a
            // folder compared against a file.)
            std::sort(listing.begin(), listing.end(), [](const FileEntry& a, const FileEntry& b)
                {
                    if (a.is_dir != b.is_dir)
                        return a.is_dir;
                    return a.upper < b.upper;
                });

            listing_dir = current_dir;
            listing_time = now;
        }

        // Get length of the directory
        current_dir_length = (int)listing.size();

        // Draw them
        const int first_row = std::max(0, sample_offset_y);
        for (int i = first_row; i < (int)listing.size() && i < file_display_count + first_row; i++)
        {
            const FileEntry& entry = listing[i];

            // Get the highlighted path
            if (i - first_row == cursor_y)
            {
                selected_path = entry.path;
                selected_path_isdir = entry.is_dir;
                if (selected_path_isdir)
                    selected_path += "\\";
            }

            // Draw the entry, relative to the current folder
            const size_t skip = std::min(entry.upper.size(), current_dir.size());
            const std::string name = entry.upper.substr(skip);
            if (entry.is_file)
                geptr->DrawTextString(1, 3 + i - first_row, geptr->entity, name, i - first_row == cursor_y ? primary_text_b : primary_text_a);
            else
                geptr->DrawTextString(1, 3 + i - first_row, geptr->entity, name, i - first_row == cursor_y ? header_text_b : header_text_a);
        }

        // Draw loaded header and current file text
        geptr->DrawTextString(1, 3 + file_display_count + 1, geptr->entity, "LOADED:", header_text_a);
        if (GetAt(&samplelist, open_sample) != nullptr)
            geptr->DrawTextString(1, 3 + file_display_count + 2, geptr->entity, samplelist[open_sample]->path, primary_text_a);
    }
}

// Draw the map for where you are in the UI
void DrawMap()
{
    // Draw the map
    geptr->DrawChar(geptr->GetCanvasW() - 1, geptr->GetCanvasH() - 1, geptr->entity, 'T'); // Table
    geptr->DrawSetColor(geptr->GetCanvasW() - 1, geptr->GetCanvasH() - 1, geptr->entity, state == m_table ? header_text_b : header_text_a);
    geptr->DrawChar(geptr->GetCanvasW() - 1, geptr->GetCanvasH() - 2, geptr->entity, 'I'); // Instrument
    geptr->DrawSetColor(geptr->GetCanvasW() - 1, geptr->GetCanvasH() - 2, geptr->entity, state == m_instrument ? header_text_b : header_text_a);
    geptr->DrawChar(geptr->GetCanvasW() - 1, geptr->GetCanvasH() - 3, geptr->entity, 'W'); // Wave
    geptr->DrawSetColor(geptr->GetCanvasW() - 1, geptr->GetCanvasH() - 3, geptr->entity, state == m_wave ? header_text_b : header_text_a);
    geptr->DrawChar(geptr->GetCanvasW() - 2, geptr->GetCanvasH() - 2, geptr->entity, 'P'); // Phrase
    geptr->DrawSetColor(geptr->GetCanvasW() - 2, geptr->GetCanvasH() - 2, geptr->entity, state == m_phrase ? header_text_b : header_text_a);
    geptr->DrawChar(geptr->GetCanvasW() - 3, geptr->GetCanvasH() - 2, geptr->entity, 'C'); // Chain
    geptr->DrawSetColor(geptr->GetCanvasW() - 3, geptr->GetCanvasH() - 2, geptr->entity, state == m_chain ? header_text_b : header_text_a);
    geptr->DrawChar(geptr->GetCanvasW() - 4, geptr->GetCanvasH() - 2, geptr->entity, 'S'); // Song
    geptr->DrawSetColor(geptr->GetCanvasW() - 4, geptr->GetCanvasH() - 2, geptr->entity, state == m_song ? header_text_b : header_text_a);
    geptr->DrawChar(geptr->GetCanvasW() - 4, geptr->GetCanvasH() - 3, geptr->entity, 'O'); // Options
    geptr->DrawSetColor(geptr->GetCanvasW() - 4, geptr->GetCanvasH() - 3, geptr->entity, state == m_options ? header_text_b : header_text_a);
}

// Handle tick hitting - This should be called from a separate thread
void TrackTicks()
{
    // The next time is defined by starting at the current time
    auto next = std::chrono::steady_clock::now();

    // Mark that the thread is running
    play_thread = true;

    while (running == true)
    {
        // Execute tick
        DoTick();

        // Sync timing
        const auto tick_ns = std::chrono::nanoseconds((int64_t)std::max(1000.0, ticklength));
        next += tick_ns;

        // If we fell far behind (a stalled DoTick, the PC sleeping, ...) resync instead of firing a
        // burst of catch-up ticks that would all push their changes at once
        if (std::chrono::steady_clock::now() - next > tick_ns * 4)
            next = std::chrono::steady_clock::now();

        while (running == true)
        {
            // Get the sleep time remaining
            auto rem = next - std::chrono::steady_clock::now();

            // Break if we have no more time
            if (rem <= std::chrono::nanoseconds(0))
                break;

            // Sleep while there is plenty of time, then yield-spin for the last few ms.
            // (The old spin was an empty loop that held a whole core at 100%.)
            if (rem > std::chrono::milliseconds(5))
                SDL_Delay(1);
            else
                std::this_thread::yield();
        }
    }

    // Mark that the thread is not running
    play_thread = false;
}

// Handle repeating movements
void HandleMovementRepeaters()
{
    // Look the input object up once per call instead of once per check
    auto* inp = dynamic_cast<input*>(geptr->GetObjectReference(inputgetter));
    // Reset movement repeaters
    if (inp->is_right_pressed())
        inputholdtimer = 0;
    else if (inp->is_left_pressed())
        inputholdtimer = 0;
    else if (inp->is_up_pressed())
        inputholdtimer = 0;
    else if (inp->is_down_pressed())
        inputholdtimer = 0;

    // Increment hold timer
    if (inp->is_right_down())
        inputholdtimer++;
    else if (inp->is_left_down())
        inputholdtimer++;
    else if (inp->is_up_down())
        inputholdtimer++;
    else if (inp->is_down_down())
        inputholdtimer++;
    else
        inputholdtimer = 0;
}

// Stop a specific channel
void StopChannel(int channel)
{
    if (channellist[channel].type == ChannelType::synth)
    {
        // Stop synth voice
        audiosynth->voices[channel]->start_playing = false;
    }
    else
    {
        // Stop sampler voice
        audiosampler->voices[channel]->start_playing = false;
    }
}

// Stop all audio playback
void StopAllChannels()
{
    for (int i = 0; i < channelcount; i++)
        StopChannel(i);
}

// Start the sequence thread
void StartSequenceThread()
{
    // Don't start a second tick thread
    if (play_thread)
        return;

    // Start tick tracker. play_thread is set before the thread exists so StopSequenceThread can't
    // miss a thread that has been created but hasn't started running yet.
    running = true;
    play_thread = true;
    std::thread(TrackTicks).detach();
}

// Stop the sequence thread
void StopSequenceThread()
{
    // Stop playing
    running = false;
    while (play_thread)
        std::this_thread::yield();
    // Reset BPM in case it was touched in playback
    ChangeBPM(set_bpm);
}

// Handle deep copy inputs
void GetDeepCopyInputs()
{
    // Look the input object up once per call instead of once per check
    auto* inp = dynamic_cast<input*>(geptr->GetObjectReference(inputgetter));
    // Handle copy
    if (inp->is_select_down())
    {
        // If a is pressed after b, then increase
        if (inp->is_a_pressed() && do_deep_copy == 1)
        {
            do_deep_copy = 2;
        }
        // If b is pressed again, reset
        if (inp->is_b_pressed() && do_deep_copy == 1)
        {
            do_deep_copy = 0;
        }
        // If b is pressed start checking for deep copy
        if (inp->is_b_pressed() && do_deep_copy == 0)
        {
            do_deep_copy = 1;
        }
        // If anything else is pressed, reset
        if (inp->presscode != "000000000" &&
            inp->presscode != "000010000" &&
            inp->presscode != "000001000")
            do_deep_copy = 0;
    }
    else
    {
        // Reset deep copy tracker
        do_deep_copy = 0;
    }
}

// Handle movement
void EditorControl()
{
    // Look the input object up once per call instead of once per check
    auto* inp = dynamic_cast<input*>(geptr->GetObjectReference(inputgetter));
    // Set checker variables
    bool breakend = false;
    int goright = 0;
    int goleft = 0;
    int goup = 0;
    int godown = 0;
    bool willpause = false;

    // Handle play button
    if (inp->is_start_pressed() && pause_song == false)
    {
        // Pause the song playback
        willpause = true;
        // Stop the playing thread
        StopSequenceThread();
    }

    // Move the cursor
    if (inp->is_right_pressed() ||
        (inp->is_right_down() &&
            (inputholdtimer > inputholdthreshold && inputholdtimer % inputholddelay == 0)))
        goright = 1;
    if (inp->is_left_pressed() ||
        (inp->is_left_down() &&
            (inputholdtimer > inputholdthreshold && inputholdtimer % inputholddelay == 0)))
        goleft = 1;
    if (inp->is_up_pressed() ||
        (inp->is_up_down() &&
            (inputholdtimer > inputholdthreshold && inputholdtimer % inputholddelay == 0)))
        goup = 1;
    if (inp->is_down_pressed() ||
        (inp->is_down_down() &&
            (inputholdtimer > inputholdthreshold && inputholdtimer % inputholddelay == 0)))
        godown = 1;

    // Edit part
    if (inp->is_a_down() &&
        inp->is_shift_down())
        leftrightcenter = right;
    else if (inp->is_a_down())
        leftrightcenter = left;
    else
        leftrightcenter = center;

    // Handle input for the settings menu
    if (state == m_options && !breakend)
    {
        // Handle edits
        if (inp->is_a_down())
        {
        }
        // Handle navigation
        else if (inp->is_select_down())
        {
            // Go to the page below
            if (godown)
            {
                // Song
                state = m_song;
                cursor_x = SDL_clamp(cursor_x, 0, song_grid_w - 1);
                cursor_y = SDL_clamp(cursor_y, 0, song_grid_h - 1);
                breakend = true;
            }
        }
        // Moving
        else
        {
            cursor_y += godown - goup;
        }

        // Draw UI
        DrawOptionsUI();
    }

    // Handle input for the song menu
    if (state == m_song && !breakend)
    {
        // Handle deep copy input logic
        GetDeepCopyInputs();

        // If we have a successful deep copy, do it
        if (do_deep_copy == 2 && GetAt(&chainlist, songgrid[cursor_y + offset_y][cursor_x + offset_x]) != nullptr)
        {
            // Deep copy the phrase and replace it here in the chain
            songgrid[cursor_y + offset_y][cursor_x + offset_x] = DeepCopyChain(songgrid[cursor_y + offset_y][cursor_x + offset_x]);
        }
        else
        {
            // Handle play button
            if (inp->is_start_pressed() && pause_song == true)
            {
                StopAllChannels();
                // Should play flag
                bool dont_play = true;
                // Set the song ptr position for only this channel
                for (int i = 0; i < channelcount; i++)
                    dont_play = channellist[i].init_song_play(cursor_y + offset_y) && dont_play;
                // Should we play or not?
                if (dont_play == false)
                {
                    // Set the scope of play to only this phrase
                    play_context = pt_song;
                    // Unpause the song playback
                    pause_song = false;
                }
                // Start the playing thread
                StartSequenceThread();
            }

            // Modify value
            if (inp->is_a_pressed())
            {
                // If the songgrid value is unfilled, insert 0
                if (songgrid[cursor_y + offset_y][cursor_x + offset_x] == -1)
                {
                    if (copied_chain == -1)
                    {
                        // Set UI reference to Hex0
                        songgrid[cursor_y + offset_y][cursor_x + offset_x] = 0x0000;
                        // If this chain doesn't exist yet, insert it
                        if (GetAt(&chainlist, songgrid[cursor_y + offset_y][cursor_x + offset_x]) == nullptr)
                            InsertAt(&chainlist, songgrid[cursor_y + offset_y][cursor_x + offset_x], new chain());
                    }
                    else
                    {
                        // Set UI reference to copied chain
                        songgrid[cursor_y + offset_y][cursor_x + offset_x] = copied_chain;
                    }
                }
                // If double click, add a new chain
                else if (inp->doubleclick)
                {
                    // Set UI reference to the next empty
                    songgrid[cursor_y + offset_y][cursor_x + offset_x] = GetNextEmpty(&chainlist);
                    // Add the new chain
                    InsertAt(&chainlist, songgrid[cursor_y + offset_y][cursor_x + offset_x], new chain());
                    // Copy to clipboard
                    copied_chain = songgrid[cursor_y + offset_y][cursor_x + offset_x];
                }
                else
                {
                    // Copy to clipboard
                    copied_chain = songgrid[cursor_y + offset_y][cursor_x + offset_x];
                }
            }

            // Do actions given the context --

            // Editing
            if (inp->is_a_down())
            {
                // Movement keys
                if (goup || godown || goright || goleft)
                {
                    // Mod the left two digits
                    if (inp->is_shift_down())
                    {
                        if (goup) songgrid[cursor_y + offset_y][cursor_x + offset_x] += 0x1000;
                        if (godown) songgrid[cursor_y + offset_y][cursor_x + offset_x] -= 0x1000;
                        if (goright) songgrid[cursor_y + offset_y][cursor_x + offset_x] += 0x0100;
                        if (goleft) songgrid[cursor_y + offset_y][cursor_x + offset_x] -= 0x0100;
                    }
                    // Mod the right two digits
                    else
                    {
                        if (goup) songgrid[cursor_y + offset_y][cursor_x + offset_x] += 0x0010;
                        if (godown) songgrid[cursor_y + offset_y][cursor_x + offset_x] -= 0x0010;
                        if (goright) songgrid[cursor_y + offset_y][cursor_x + offset_x] += 0x0001;
                        if (goleft) songgrid[cursor_y + offset_y][cursor_x + offset_x] -= 0x0001;
                    }

                    // Wrap the cell between 0x0000 and 0xFFFF
                    if (songgrid[cursor_y + offset_y][cursor_x + offset_x] < 0)
                        songgrid[cursor_y + offset_y][cursor_x + offset_x] = 0xFFFF + (songgrid[cursor_y + offset_y][cursor_x + offset_x] + 1);
                    if (songgrid[cursor_y + offset_y][cursor_x + offset_x] > 0xFFFF)
                        songgrid[cursor_y + offset_y][cursor_x + offset_x] = (songgrid[cursor_y + offset_y][cursor_x + offset_x] - 1) - 0xFFFF;

                    // Copy to clipboard
                    copied_chain = songgrid[cursor_y + offset_y][cursor_x + offset_x];
                }

                // Handle deletes
                if (inp->is_b_pressed())
                    songgrid[cursor_y + offset_y][cursor_x + offset_x] = -1;
            }
            // Goto page
            else if (inp->is_select_down())
            {
                // Go to the next page over
                if (goright)
                {
                    // Set open audio channel
                    open_channel = cursor_x + offset_x;
                    // Set open chain
                    if (songgrid[cursor_y + offset_y][cursor_x + offset_x] != -1)
                    {
                        open_chain = songgrid[cursor_y + offset_y][cursor_x + offset_x];
                        // Set open chain index
                        open_chain_index = cursor_y + offset_y;
                    }
                    // If the open_chain is valid
                    if (open_chain != -1)
                    {
                        // Check if the chain does not exist
                        if (GetAt(&chainlist, open_chain) == nullptr)
                        {
                            InsertAt(&chainlist, open_chain, new chain());
                        }
                        // Chain
                        state = m_chain;
                        cursor_x = SDL_clamp(cursor_x, 0, chain_grid_w - 1);
                        cursor_y = SDL_clamp(cursor_y, 0, chain_grid_h - 1);
                        breakend = true;
                    }
                }

                // Go to the page above
                if (goup)
                {
                    // Options
                    state = m_options;
                    cursor_x = 0;
                    cursor_y = SDL_clamp(cursor_y, 0, global_settings->setting_labels.size() - 1);
                    breakend = true;
                }
            }
            // Moving
            else
            {
                cursor_x += goright - goleft;
                cursor_y += godown - goup;
            }

            // Move the page
            if (cursor_y > song_grid_h - 1)
                offset_y += 1;
            if (cursor_x > song_grid_w - 1)
                offset_x += 1;
            if (cursor_y < 0)
                offset_y -= 1;
            if (cursor_x < 0)
                offset_x -= 1;

            // Clamp the cursor and offsets
            cursor_x = SDL_clamp(cursor_x, 0, song_grid_w - 1);
            cursor_y = SDL_clamp(cursor_y, 0, song_grid_h - 1);
            offset_x = SDL_clamp(offset_x, 0, channelcount - song_grid_w);
            offset_y = SDL_clamp(offset_y, 0, rowcount - song_grid_h);
        }

        // Reset deep copy action flag
        if (do_deep_copy == 2)
            do_deep_copy = 0;

        // Update UI
        DrawSongUI(offset_x, offset_y);
    }

    // Handle input for the chain menu
    if (state == m_chain && !breakend)
    {
        // Handle deep copy input logic
        GetDeepCopyInputs();

        // If we have a successful deep copy, do it
        if (do_deep_copy == 2 && GetAt(&phraselist, chainlist[open_chain]->arr[cursor_y + chain_offset_y]) != nullptr)
        {
            // Deep copy the phrase and replace it here in the chain
            chainlist[open_chain]->arr[cursor_y + chain_offset_y] = DeepCopyPhrase(chainlist[open_chain]->arr[cursor_y + chain_offset_y]);
        }
        else
        {
            // Handle play button
            if (inp->is_start_pressed() && pause_song == true)
            {
                StopAllChannels();
                // Set the song ptr position for only this channel
                playing_channel = open_channel;
                channellist[playing_channel].chain_ptr = open_chain_index;
                channellist[playing_channel].playing_chain = open_chain;
                channellist[playing_channel].phrase_ptr = cursor_y + chain_offset_y;
                channellist[playing_channel].playing_phrase = -1;
                channellist[playing_channel].step_ptr = 0;
                channellist[playing_channel].tick_ptr = 0;
                channellist[playing_channel].delay_ticks = 0;
                channellist[playing_channel].delay_tick_offsetter = 0;
                channellist[playing_channel].catchup_ticks = 0;
                channellist[playing_channel].first_delay_passed = true;
                ticknumber = 0;
                // Set the scope of play to only this phrase
                play_context = pt_chain;
                // Unpause the song playback
                pause_song = false;
                // Start the playing thread
                StartSequenceThread();
            }

            // Modify value
            if (inp->is_a_pressed())
            {
                // Edit actual phrase
                if (cursor_x == 0)
                {
                    // If the chainlist value is unfilled, insert 0
                    if (chainlist[open_chain]->arr[cursor_y + chain_offset_y] == -1)
                    {
                        if (copied_phrase == -1)
                        {
                            // Set UI reference to Hex0
                            chainlist[open_chain]->arr[cursor_y + chain_offset_y] = 0x0000;
                            // If this phrase doesn't exist yet, insert it
                            if (GetAt(&phraselist, chainlist[open_chain]->arr[cursor_y + chain_offset_y]) == nullptr)
                                InsertAt(&phraselist, chainlist[open_chain]->arr[cursor_y + chain_offset_y], new phrase());
                        }
                        else
                        {
                            // Set UI reference to copied chain
                            chainlist[open_chain]->arr[cursor_y + chain_offset_y] = copied_phrase;
                        }
                    }
                    // If double click, add a new chain
                    else if (inp->doubleclick)
                    {
                        // Set UI reference to the next empty
                        chainlist[open_chain]->arr[cursor_y + chain_offset_y] = GetNextEmpty(&phraselist);
                        // Add the new phrase
                        InsertAt(&phraselist, chainlist[open_chain]->arr[cursor_y + chain_offset_y], new phrase());
                        // Copy to clipboard
                        copied_phrase = chainlist[open_chain]->arr[cursor_y + chain_offset_y];
                    }
                    else
                    {
                        // Copy to clipboard
                        copied_phrase = chainlist[open_chain]->arr[cursor_y + chain_offset_y];
                    }
                }
            }

            // Do actions given the context --

            // Editing
            if (inp->is_a_down())
            {
                // Edit actual chain
                if (cursor_x == 0)
                {
                    // Movement keys
                    if (goup || godown || goright || goleft)
                    {
                        // Mod the left two digits
                        if (inp->is_shift_down())
                        {
                            if (goup) chainlist[open_chain]->arr[cursor_y + chain_offset_y] += 0x1000;
                            if (godown) chainlist[open_chain]->arr[cursor_y + chain_offset_y] -= 0x1000;
                            if (goright) chainlist[open_chain]->arr[cursor_y + chain_offset_y] += 0x0100;
                            if (goleft) chainlist[open_chain]->arr[cursor_y + chain_offset_y] -= 0x0100;
                        }
                        // Mod the right two digits
                        else
                        {
                            if (goup) chainlist[open_chain]->arr[cursor_y + chain_offset_y] += 0x0010;
                            if (godown) chainlist[open_chain]->arr[cursor_y + chain_offset_y] -= 0x0010;
                            if (goright) chainlist[open_chain]->arr[cursor_y + chain_offset_y] += 0x0001;
                            if (goleft) chainlist[open_chain]->arr[cursor_y + chain_offset_y] -= 0x0001;
                        }

                        // Wrap the cell between 0x0000 and 0xFFFF
                        if (chainlist[open_chain]->arr[cursor_y + chain_offset_y] < 0)
                            chainlist[open_chain]->arr[cursor_y + chain_offset_y] = 0xFFFF + (chainlist[open_chain]->arr[cursor_y + chain_offset_y] + 1);
                        if (chainlist[open_chain]->arr[cursor_y + chain_offset_y] > 0xFFFF)
                            chainlist[open_chain]->arr[cursor_y + chain_offset_y] = (chainlist[open_chain]->arr[cursor_y + chain_offset_y] - 1) - 0xFFFF;

                        // Copy to clipboard
                        copied_phrase = chainlist[open_chain]->arr[cursor_y + chain_offset_y];
                    }

                    // Handle deletes
                    if (inp->is_b_pressed())
                        chainlist[open_chain]->arr[cursor_y + chain_offset_y] = -1;
                }
                // Edit transpose
                else
                {
                    // Movement keys
                    if (goup || godown || goright || goleft)
                    {
                        // Mod the left two digits
                        if (inp->is_shift_down())
                        {
                            if (goup) chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] += 0x0C00;
                            if (godown) chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] -= 0x0C00;
                            if (goright) chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] += 0x0100;
                            if (goleft) chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] -= 0x0100;
                        }
                        // Mod the right two digits
                        else
                        {
                            if (goup) chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] += 0x000C;
                            if (godown) chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] -= 0x000C;
                            if (goright) chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] += 0x0001;
                            if (goleft) chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] -= 0x0001;
                        }

                        // Wrap the cell between 0x0000 and 0xFFFF
                        if (chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] < 0)
                            chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] = 0xFFFF + (chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] + 1);
                        if (chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] > 0xFFFF)
                            chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] = (chainlist[open_chain]->arr_transpose[cursor_y + chain_offset_y] - 1) - 0xFFFF;
                    }
                }
            }
            // Goto page
            else if (inp->is_select_down())
            {
                // Go to the left page over
                if (goleft)
                {
                    // Song
                    state = m_song;
                    cursor_x = SDL_clamp(cursor_x, 0, song_grid_w - 1);
                    cursor_y = SDL_clamp(cursor_y, 0, song_grid_h - 1);
                    breakend = true;
                }
                // Go to the right page over
                else if (goright)
                {
                    // Set open phrase
                    if (chainlist[open_chain]->arr[cursor_y + chain_offset_y] != -1)
                    {
                        open_phrase = chainlist[open_chain]->arr[cursor_y + chain_offset_y];
                        // Set open phrase index
                        open_phrase_index = cursor_y + chain_offset_y;
                    }
                    // If the open_chain is valid
                    if (open_phrase != -1)
                    {
                        // Check if the phrase does not exist
                        if (GetAt(&phraselist, open_phrase) == nullptr)
                        {
                            InsertAt(&phraselist, open_phrase, new phrase());
                        }
                        // Phrase
                        state = m_phrase;
                        cursor_x = SDL_clamp(cursor_x, 0, phrase_grid_w - 1);
                        cursor_y = SDL_clamp(cursor_y, 0, phrase_grid_h - 1);
                        breakend = true;
                    }
                }
            }
            // Moving
            else
            {
                cursor_x += goright - goleft;
                cursor_y += godown - goup;
            }

            // wrap the cursor and clamp offsets
            if (cursor_x > chain_grid_w - 1)
                cursor_x = 0;
            if (cursor_x < 0)
                cursor_x = chain_grid_w - 1;
            if (cursor_y + chain_offset_y > chain::length - 1)
            {
                cursor_y = 0;
                chain_offset_y = 0;
            }
            if (cursor_y + chain_offset_y < 0)
            {
                cursor_y = chain_grid_h - 1;
                chain_offset_y = chain::length - chain_grid_h;
            }

            // Move the page
            if (cursor_y > chain_grid_h - 1)
                chain_offset_y += 1;
            if (cursor_y < 0)
                chain_offset_y -= 1;

            cursor_y = SDL_clamp(cursor_y, 0, chain_grid_h - 1);
            chain_offset_y = SDL_clamp(chain_offset_y, 0, chain::length - chain_grid_h);
        }

        // Reset deep copy action flag
        if (do_deep_copy == 2)
            do_deep_copy = 0;

        // Update UI
        DrawChainUI(chain_offset_y);
    }

    // Handle input for the phrase menu
    if (state == m_phrase && !breakend)
    {
        // Handle deep copy input logic
        GetDeepCopyInputs();

        // If we have a successful deep copy, do it
        if (do_deep_copy == 2 && cursor_x == 1 && GetAt(&instrumentlist, phraselist[open_phrase]->arr[cursor_y + chain_offset_y][cursor_x]) != nullptr)
        {
            // Deep copy the instrument
            auto i = GetAt(&instrumentlist, phraselist[open_phrase]->arr[cursor_y + chain_offset_y][cursor_x])->DeepCopyInstrument();
            auto index = GetNextEmpty(&instrumentlist);
            InsertAt(&instrumentlist, index, i);
            phraselist[open_phrase]->arr[cursor_y + chain_offset_y][cursor_x] = index;
        }
        else
        {
            // Handle play button
            if (inp->is_start_pressed() && pause_song == true)
            {
                StopAllChannels();
                // Set the song ptr position for only this channel
                playing_channel = open_channel;
                channellist[playing_channel].chain_ptr = open_chain_index;
                channellist[playing_channel].playing_chain = open_chain;
                channellist[playing_channel].phrase_ptr = open_phrase_index;
                channellist[playing_channel].playing_phrase = open_phrase;
                channellist[playing_channel].step_ptr = 0;
                channellist[playing_channel].tick_ptr = 0;
                channellist[playing_channel].delay_ticks = 0;
                channellist[playing_channel].delay_tick_offsetter = 0;
                channellist[playing_channel].catchup_ticks = 0;
                channellist[playing_channel].first_delay_passed = true;
                ticknumber = 0;
                // Set the scope of play to only this phrase
                play_context = pt_phrase;
                // Unpause the song playback
                pause_song = false;
                // Start the playing thread
                StartSequenceThread();
            }

            // Modify value
            if (inp->is_a_pressed())
            {
                // Edit instr
                if (cursor_x == 1)
                {
                    // If the phraselist value is unfilled, insert 0
                    if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] == -1)
                    {
                        if (copied_instr == -1)
                        {
                            // Set UI reference to Hex0
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = 0x0000;
                            // If this instrument doesn't exist yet, insert it
                            if (GetAt(&instrumentlist, phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x]) == nullptr)
                                InsertAt(&instrumentlist, phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x], new instrument());
                        }
                        else
                        {
                            // Set UI reference to copied instrument
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = copied_instr;
                        }
                    }
                    // If double click, add a new instrument
                    else if (inp->doubleclick)
                    {
                        // Set UI reference to the next empty
                        phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = GetNextEmpty(&instrumentlist);
                        // Add the new instrument
                        InsertAt(&instrumentlist, phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x], new instrument());
                        // Copy to clipboard
                        copied_instr = phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x];
                    }
                    else
                    {
                        // Copy to clipboard
                        copied_instr = phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x];
                    }
                }
            }

            // Do actions given the context --

            // Editing
            if (inp->is_a_down())
            {
                // Edit note
                if (cursor_x == 0)
                {
                    // If the phraselist instrument value is unfilled, insert 0
                    if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][1] == -1 && inp->is_a_pressed())
                    {
                        if (copied_instr == -1)
                        {
                            // Set UI reference to Hex0
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][1] = 0x0000;
                            // If this instrument doesn't exist yet, insert it
                            if (GetAt(&instrumentlist, phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][1]) == nullptr)
                                InsertAt(&instrumentlist, phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][1], new instrument());
                        }
                        else
                        {
                            // Set UI reference to copied instrument
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][1] = copied_instr;
                        }
                    }

                    // Set to C-4 if it isn't set
                    if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] == -9999 && inp->is_a_pressed())
                        phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] = copied_note != -9999 ? copied_note : 39;

                    // Movement keys
                    if ((goup || godown || goright || goleft) && phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] != -9999)
                    {
                        // Edit note pitch
                        if (goup) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] += 12;
                        if (godown) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] -= 12;
                        if (goright) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] += 1;
                        if (goleft) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] -= 1;

                        // Wrap the cell between min_note and max_note
                        if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] < min_note)
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] = max_note + ((phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] - min_note) + 1);
                        if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] > max_note)
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] = min_note + (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] - 1) - max_note;
                    }

                    // Copy to clipboard
                    if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] != -9999)
                        copied_note = phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0];
                    if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][1] != -9999)
                        copied_instr = phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][1];

                    // Preview note
                    if (pause_song && (inp->is_a_pressed() || goup || godown || goright || goleft))
                    {
                        // Stop the playing thread
                        StopSequenceThread();
                        // pt_preview allows sub-step to run regardless of if the song is paused
                        playing_channel = open_channel;
                        play_context = pt_preview;
                        channellist[open_channel].tick_ptr = 0;
                        ticknumber = 0;
                        // Play the note
                        PlayStepPhrase(open_channel, open_phrase, cursor_y + phrase_offset_y);
                        // Start the playing thread
                        StartSequenceThread();
                    }

                    // Handle deletes
                    if (inp->is_b_pressed())
                    {
                        phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][1] = -1;
                        phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][0] = -9999;
                    }
                }

                // Edit instr
                if (cursor_x == 1)
                {
                    // Movement keys
                    if (goup || godown || goright || goleft)
                    {
                        // Mod the left two digits
                        if (inp->is_shift_down())
                        {
                            if (goup) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 0x1000;
                            if (godown) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 0x1000;
                            if (goright) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 0x0100;
                            if (goleft) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 0x0100;
                        }
                        // Mod the right two digits
                        else
                        {
                            if (goup) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 0x0010;
                            if (godown) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 0x0010;
                            if (goright) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 0x0001;
                            if (goleft) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 0x0001;
                        }

                        // Wrap the cell between 0x0000 and 0xFFFF
                        if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] < 0)
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = 0xFFFF + (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] + 1);
                        if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] > 0xFFFF)
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] - 1) - 0xFFFF;

                        // Copy to clipboard
                        copied_instr = phraselist[open_phrase]->arr[cursor_y + offset_y][cursor_x + offset_x];
                    }

                    // Handle deletes
                    if (inp->is_b_pressed())
                        phraselist[open_phrase]->arr[cursor_y + offset_y][cursor_x + offset_x] = -1;
                }

                // Edit fx1,2,3
                if (cursor_x == 2 || cursor_x == 4 || cursor_x == 6)
                {
                    // Set to 0 if it isn't set
                    if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] == -1 && inp->is_a_pressed())
                        phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = copied_effect != -1 ? copied_effect : 0;
                    if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x + 1] == -1 && inp->is_a_pressed())
                        phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x + 1] = copied_effect_param != -1 ? copied_effect_param : 0x0000;

                    // Movement keys
                    if (goup || godown || goright || goleft)
                    {
                        if (goup) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 5;
                        if (godown) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 5;
                        if (goright) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 1;
                        if (goleft) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 1;

                        // Wrap the cell between 0 and effect list length
                        int effect_list_len = fx_length - 1;
                        if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] < 0)
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = effect_list_len + (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] + 1);
                        if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] > effect_list_len)
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] - 1) - effect_list_len;
                    }

                    // Copy to clipboard
                    if (phraselist[open_phrase]->arr[cursor_y + offset_y][cursor_x + offset_x] != -1)
                        copied_effect = phraselist[open_phrase]->arr[cursor_y + offset_y][cursor_x + offset_x];

                    // Handle deletes
                    if (inp->is_b_pressed())
                    {
						phraselist[open_phrase]->arr[cursor_y + offset_y][cursor_x + offset_x] = -1; // Clear effect
						phraselist[open_phrase]->arr[cursor_y + offset_y][cursor_x + offset_x + 1] = -1; // Clear effect param
                    }
                }

                // Edit fx1,2,3 param
                if (cursor_x == 3 || cursor_x == 5 || cursor_x == 7)
                {
                    // Set to 0 if it isn't set
                    if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] == -1 && inp->is_a_pressed())
                        phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = copied_effect_param != -1 ? copied_effect_param : 0x0000;

                    // Movement keys
                    if (goup || godown || goright || goleft)
                    {
                        // Mod the left two digits
                        if (inp->is_shift_down())
                        {
                            if (goup) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 0x1000;
                            if (godown) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 0x1000;
                            if (goright) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 0x0100;
                            if (goleft) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 0x0100;
                        }
                        // Mod the right two digits
                        else
                        {
                            if (goup) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 0x0010;
                            if (godown) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 0x0010;
                            if (goright) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] += 0x0001;
                            if (goleft) phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] -= 0x0001;
                        }

                        // Wrap the cell between 0x0000 and 0xFFFF
                        if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] < 0)
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = 0xFFFF + (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] + 1);
                        if (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] > 0xFFFF)
                            phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] = (phraselist[open_phrase]->arr[cursor_y + phrase_offset_y][cursor_x] - 1) - 0xFFFF;
                    }

                    // Copy to clipboard
                    if (phraselist[open_phrase]->arr[cursor_y + offset_y][cursor_x + offset_x] != -1)
                        copied_effect_param = phraselist[open_phrase]->arr[cursor_y + offset_y][cursor_x + offset_x];

                    // Handle deletes
                    if (inp->is_b_pressed())
                        phraselist[open_phrase]->arr[cursor_y + offset_y][cursor_x + offset_x] = -1;
                }
            }
            // Goto page
            else if (inp->is_select_down())
            {
                // Go to the left page over
                if (goleft)
                {
                    // If the open_chain is valid
                    if (open_chain != -1)
                    {
                        // Check if the chain does not exist
                        if (GetAt(&chainlist, open_chain) == nullptr)
                        {
                            InsertAt(&chainlist, open_chain, new chain());
                        }
                        // Chain
                        state = m_chain;
                        cursor_x = SDL_clamp(cursor_x, 0, chain_grid_w - 1);
                        cursor_y = SDL_clamp(cursor_y, 0, chain_grid_h - 1);
                        breakend = true;
                    }
                }
                // Go to the right page over
                else if (goright)
                {
                    // Set open instrument
                    if (phraselist[open_phrase]->arr[cursor_y + chain_offset_y][1] != -1)
                    {
                        open_instrument = phraselist[open_phrase]->arr[cursor_y + chain_offset_y][1];
                    }
                    // If the open_instrument is valid
                    if (open_instrument != -1)
                    {
                        // Check if the instrument does not exist
                        if (GetAt(&instrumentlist, open_instrument) == nullptr)
                        {
                            InsertAt(&instrumentlist, open_instrument, new instrument());
                        }
                        // Chain
                        state = m_instrument;
                        cursor_x = SDL_clamp(cursor_x, 0, GetAt(&instrumentlist, open_instrument)->type == ChannelType::synth ? instrument::synth_menu_width - 1 : instrument::sample_menu_width - 1);
                        cursor_y = SDL_clamp(cursor_y, 0, GetAt(&instrumentlist, open_instrument)->type == ChannelType::synth ? instrument::synth_menu_height - 1 : instrument::sample_menu_height - 1);
                        breakend = true;
                    }
                }
            }
            // Moving
            else
            {
                cursor_x += goright - goleft;
                cursor_y += godown - goup;
            }

            // Stop previewing
            if (pause_song && inp->is_a_released())
            {
                // Reset play context because if you don't, play_context will keep going
                play_context = pt_song;
                // Stop audio
                StopAllChannels();
                // Stop the playing thread
                StopSequenceThread();
            }

            // Next and last phrase
            auto next_phrase_index = (open_phrase_index + 1) % chain::length;
            auto next_phrase = GetAt(&phraselist, chainlist[open_chain]->arr[next_phrase_index]);
            auto last_phrase_index = (open_phrase_index - 1 < 0 ? chain::length - 1 : open_phrase_index - 1) % chain::length;
            auto last_phrase = GetAt(&phraselist, chainlist[open_chain]->arr[last_phrase_index]);

            // wrap the cursor and clamp offsets
            if (cursor_x > phrase_grid_w - 1)
                cursor_x = 0;
            if (cursor_x < 0)
                cursor_x = phrase_grid_w - 1;
            if (cursor_y + phrase_offset_y > phrase::len_y - 1 && next_phrase != nullptr)
            {
                cursor_y = 0;
                phrase_offset_y = 0;
                open_phrase = chainlist[open_chain]->arr[next_phrase_index];
                open_phrase_index = next_phrase_index;
            }
            if (cursor_y + phrase_offset_y < 0 && last_phrase != nullptr)
            {
                cursor_y = phrase_grid_h - 1;
                phrase_offset_y = phrase::len_y - phrase_grid_h;
                open_phrase = chainlist[open_chain]->arr[last_phrase_index];
                open_phrase_index = last_phrase_index;
            }

            // Move the page
            if (cursor_y > phrase_grid_h - 1)
                phrase_offset_y += 1;
            if (cursor_y < 0)
                phrase_offset_y -= 1;

            cursor_y = SDL_clamp(cursor_y, 0, phrase_grid_h - 1);
            phrase_offset_y = SDL_clamp(phrase_offset_y, 0, phrase::len_y - phrase_grid_h);
        }

        // Reset deep copy action flag
        if (do_deep_copy == 2)
            do_deep_copy = 0;

        // Update UI
        DrawPhraseUI(phrase_offset_y);
    }

    // Handle input for the instrument menu
    if (state == m_instrument && !breakend)
    {
        // For editing synth instrument
        if (instrumentlist[open_instrument]->type == ChannelType::synth)
        {
            // Handle deep copy input logic
            GetDeepCopyInputs();

            // If we have a successful deep copy, do it
            if (do_deep_copy == 2 && cursor_y == 10 && cursor_x == 0 && GetAt(&tablelist, instrumentlist[open_instrument]->table_index) != nullptr)
            {
                // Deep copy the sample
                auto i = GetAt(&tablelist, instrumentlist[open_instrument]->table_index)->DeepCopyTable();
                auto index = GetNextEmpty(&tablelist);
                InsertAt(&tablelist, index, i);
                instrumentlist[open_instrument]->table_index = index;
            }
            else
            {
                // Modify value
                if (inp->is_a_pressed())
                {
                    // Edit table
                    if (cursor_y == 10 && cursor_x == 0)
                    {
                        // If the sample value is unfilled, insert 0
                        if (instrumentlist[open_instrument]->table_index == -1)
                        {
                            if (copied_tble == -1)
                            {
                                // Set UI reference to Hex0
                                instrumentlist[open_instrument]->table_index = 0x0000;
                                // If this table doesn't exist yet, insert it
                                if (GetAt(&tablelist, instrumentlist[open_instrument]->table_index) == nullptr)
                                    InsertAt(&tablelist, instrumentlist[open_instrument]->table_index, new table());
                            }
                            else
                            {
                                // Set UI reference to copied sample
                                instrumentlist[open_instrument]->table_index = copied_tble;
                            }
                        }
                        // If double click, add a new sample
                        else if (inp->doubleclick)
                        {
                            // Set UI reference to the next empty
                            instrumentlist[open_instrument]->table_index = GetNextEmpty(&tablelist);
                            // Add the new sample
                            InsertAt(&tablelist, instrumentlist[open_instrument]->table_index, new table());
                            // Copy to clipboard
                            copied_tble = instrumentlist[open_instrument]->table_index;
                        }
                        else
                        {
                            // Copy to clipboard
                            copied_tble = instrumentlist[open_instrument]->table_index;
                        }
                    }
                }
            }

            // Editing
            if (inp->is_a_down())
            {
                // Delete table index
                if (cursor_y == 10 && cursor_x == 0 &&
                    inp->is_b_pressed())
                {
                    instrumentlist[open_instrument]->table_index = -1;
                }

                // Get edit ptr
                int* edit;

                // Default
                edit = &(instrumentlist[open_instrument]->volume_edit);

                // Get instrument edit pointer
                switch (instrument_edit_y)
                {
                case 3: // Channel Type
                    edit = reinterpret_cast<int*>(&instrumentlist[open_instrument]->type);
                    break;
                case 4: // Waveform
                    edit = reinterpret_cast<int*>(&instrumentlist[open_instrument]->waveform);
                    break;
                case 5: // Volume
                    edit = &(instrumentlist[open_instrument]->volume_edit);
                    break;
                case 6: // Panning
                    edit = &(instrumentlist[open_instrument]->pan_edit);
                    break;
                case 7: // Pulse Width
                    edit = &(instrumentlist[open_instrument]->pw_edit);
                    break;
                case 8: // Tuning
                    edit = &(instrumentlist[open_instrument]->detune_edit);
                    break;
                case 9: // Pitch Sweep
                    edit = &(instrumentlist[open_instrument]->pitch_freq_edit);
                    break;
                case 11: // Filter Type
                    edit = reinterpret_cast<int*>(&instrumentlist[open_instrument]->filter);
                    break;
                case 12: // Filter Cutoff
                    edit = &(instrumentlist[open_instrument]->cutoff_edit);
                    break;
                case 13: // Filter Resonance
                    edit = &(instrumentlist[open_instrument]->resonance_edit);
                    break;
                case 15: // Table Index
                    edit = &instrumentlist[open_instrument]->table_index;
                    break;
                }

                // Movement keys
                if (goup || godown || goright || goleft)
                {
                    // Mod the left two digits
                    if (inp->is_shift_down() && instrument_edit_digit_count == 2)
                    {
                        if (goup) (*edit) += 0x1000;
                        if (godown) (*edit) -= 0x1000;
                        if (goright) (*edit) += 0x0100;
                        if (goleft) (*edit) -= 0x0100;
                    }
                    // Mod the right two digits
                    else
                    {
                        if (goup && instrument_edit_digit_count != 0) (*edit) += 0x0010;
                        if (godown && instrument_edit_digit_count != 0)  (*edit) -= 0x0010;
                        if (goright) (*edit) += 0x0001;
                        if (goleft) (*edit) -= 0x0001;
                    }

                    // Correct number ranges
                    switch (instrument_edit_y)
                    {
                    case 3: // Channel Type
                        if ((*edit) < static_cast<int>(ChannelType::min))
                            (*edit) = static_cast<int>(ChannelType::max) + ((*edit) + 1);
                        if ((*edit) > static_cast<int>(ChannelType::max))
                            (*edit) = ((*edit) - 1) - static_cast<int>(ChannelType::max);
                        break;
                    case 4: // Waveform
                        if ((*edit) < static_cast<int>(SynthWaveForm::min))
                            (*edit) = static_cast<int>(SynthWaveForm::max) + ((*edit) + 1);
                        if ((*edit) > static_cast<int>(SynthWaveForm::max))
                            (*edit) = ((*edit) - 1) - static_cast<int>(SynthWaveForm::max);
                        break;
                    case 11: // Filter Type
                        if ((*edit) < static_cast<int>(FilterType::min))
                            (*edit) = static_cast<int>(FilterType::max) + ((*edit) + 1);
                        if ((*edit) > static_cast<int>(FilterType::max))
                            (*edit) = ((*edit) - 1) - static_cast<int>(FilterType::max);
                        break;
                    case 5: // Volume
                    case 6: // Panning
                    case 7: // Pulse Width
                    case 9: // Pitch Sweep
                    case 12: // Filter Cutoff
                    case 13: // Filter Resonance
                    case 15: // Table index
                        // Wrap the cell between 0x0000 and 0xFFFF
                        if ((*edit) < 0)
                            (*edit) = 0xFFFF + ((*edit) + 1);
                        if ((*edit) > 0xFFFF)
                            (*edit) = ((*edit) - 1) - 0xFFFF;
                        break;
                    case 8: // Tuning
                        // Wrap the cell between 0x00 and 0xFF
                        if ((*edit) < 0)
                            (*edit) = 0xFF + ((*edit) + 1);
                        if ((*edit) > 0xFF)
                            (*edit) = ((*edit) - 1) - 0xFF;
                        break;
                    }

                    // Convert edits to actual values
                    instrumentlist[open_instrument]->detune = (instrumentlist[open_instrument]->detune_edit - 128) / 2.f;
                    std::string outstr;
                    outstr = IntToHexString(instrumentlist[open_instrument]->volume_edit);
                    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                    instrumentlist[open_instrument]->volume = std::stoi(outstr.substr(0, 2), 0, 16) / 255.f;
                    instrumentlist[open_instrument]->volume_freq = (std::stoi(outstr.substr(2, 2), 0, 16) - 128) / 128.f;
                    outstr = IntToHexString(instrumentlist[open_instrument]->pan_edit);
                    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                    if (outstr.substr(0, 2) == "80")
                        instrumentlist[open_instrument]->panning = 0.5f;
                    else
                        instrumentlist[open_instrument]->panning = std::stoi(outstr.substr(0, 2), 0, 16) / 255.f;
                    instrumentlist[open_instrument]->pan_freq = (std::stoi(outstr.substr(2, 2), 0, 16) - 128) / 128.f; // std::stoi(outstr.substr(2, 2), 0, 16) / 16.f;
                    outstr = IntToHexString(instrumentlist[open_instrument]->pw_edit);
                    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                    instrumentlist[open_instrument]->pulse_width = (std::stoi(outstr.substr(0, 2), 0, 16) / 255.f) - 0.5f;
                    instrumentlist[open_instrument]->pulse_width_freq = std::stoi(outstr.substr(2, 2), 0, 16) / 16.f;
                    instrumentlist[open_instrument]->pitch_freq = (instrumentlist[open_instrument]->pitch_freq_edit - 32768) / 32768.f;
                    outstr = IntToHexString(instrumentlist[open_instrument]->cutoff_edit);
                    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                    instrumentlist[open_instrument]->cutoff = std::stoi(outstr.substr(0, 2), 0, 16) / 255.f;
                    instrumentlist[open_instrument]->cutoff_freq = (std::stoi(outstr.substr(2, 2), 0, 16) - 128) / 128.f;
                    outstr = IntToHexString(instrumentlist[open_instrument]->resonance_edit);
                    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                    instrumentlist[open_instrument]->resonance = instrumentlist[open_instrument]->resonance_edit / 65535.f;
                    instrumentlist[open_instrument]->resonance_freq = (std::stoi(outstr.substr(2, 2), 0, 16) - 128) / 128.f;
                }
            }
            // Goto page
            else if (inp->is_select_down())
            {
                // Go to the left page over
                if (goleft)
                {
                    // If the open_chain is valid
                    if (open_phrase != -1)
                    {
                        // Check if the phrase does not exist
                        if (GetAt(&phraselist, open_phrase) == nullptr)
                        {
                            InsertAt(&phraselist, open_phrase, new phrase());
                        }
                        // Chain
                        state = m_phrase;
                        cursor_x = SDL_clamp(cursor_x, 0, phrase_grid_w - 1);
                        cursor_y = SDL_clamp(cursor_y, 0, phrase_grid_h - 1);
                        breakend = true;
                    }
                }
                // Go to the above page over
                else if (goup)
                {
                    // TODO: Implement going to the wave editor
                }
                // Go to the below page
                else if (godown)
                {
                    // Set open sample
                    if (cursor_y == 10 && cursor_x == 0 && instrumentlist[open_instrument]->table_index != -1)
                    {
                        open_table = instrumentlist[open_instrument]->table_index;
                    }
                    // If the open_sample is valid
                    if (open_table != -1)
                    {
                        // Check if the sample does not exist
                        if (GetAt(&tablelist, open_table) == nullptr)
                        {
                            InsertAt(&tablelist, open_table, new table());
                        }
                        // Table
                        state = m_table;
                        //sample_offset_y = 0;
                        cursor_x = SDL_clamp(cursor_x, 0, table_grid_w - 1);
                        cursor_y = SDL_clamp(cursor_y, 0, table_grid_h - 1);
                        breakend = true;
                    }
                }
            }
            // Moving
            else
            {
                cursor_x += goright - goleft;
                cursor_y += godown - goup;
            }

            // wrap the cursor and clamp offsets
            if (cursor_x > instrument::synth_menu_width - 1)
                cursor_x = 0;
            if (cursor_x < 0)
                cursor_x = instrument::synth_menu_width - 1;
            if (cursor_y > instrument::synth_menu_height - 1)
                cursor_y = 0;
            if (cursor_y < 0)
                cursor_y = instrument::synth_menu_height - 1;
        }
        // For editing file instrument
        else if (instrumentlist[open_instrument]->type == ChannelType::file)
        {
            // Handle deep copy input logic
            GetDeepCopyInputs();

            // If we have a successful deep copy, do it
            if (do_deep_copy == 2 && cursor_y == 14 && cursor_x == 0 && GetAt(&tablelist, instrumentlist[open_instrument]->table_index) != nullptr)
            {
                // Deep copy the table
                auto i = GetAt(&tablelist, instrumentlist[open_instrument]->table_index)->DeepCopyTable();
                auto index = GetNextEmpty(&tablelist);
                InsertAt(&tablelist, index, i);
                instrumentlist[open_instrument]->table_index = index;
            }
            else
            {
                // Modify value
                if (inp->is_a_pressed())
                {
                    // Edit sample
                    if (cursor_y == 1 && cursor_x == 0)
                    {
                        // If the sample value is unfilled, insert 0
                        if (instrumentlist[open_instrument]->sample_index == -1)
                        {
                            if (copied_smple == -1)
                            {
                                // Set UI reference to Hex0
                                instrumentlist[open_instrument]->sample_index = 0x0000;
                                // If this sample doesn't exist yet, insert it
                                if (GetAt(&samplelist, instrumentlist[open_instrument]->sample_index) == nullptr)
                                    InsertAt(&samplelist, instrumentlist[open_instrument]->sample_index, new sample());
                            }
                            else
                            {
                                // Set UI reference to copied sample
                                instrumentlist[open_instrument]->sample_index = copied_smple;
                            }
                        }
                        // If double click, add a new sample
                        else if (inp->doubleclick)
                        {
                            // Set UI reference to the next empty
                            instrumentlist[open_instrument]->sample_index = GetNextEmpty(&samplelist);
                            // Add the new sample
                            InsertAt(&samplelist, instrumentlist[open_instrument]->sample_index, new sample());
                            // Copy to clipboard
                            copied_smple = instrumentlist[open_instrument]->sample_index;
                        }
                        else
                        {
                            // Copy to clipboard
                            copied_smple = instrumentlist[open_instrument]->sample_index;
                        }
                    }

                    // Edit table
                    if (cursor_y == 14 && cursor_x == 0)
                    {
                        // If the table value is unfilled, insert 0
                        if (instrumentlist[open_instrument]->table_index == -1)
                        {
                            if (copied_tble == -1)
                            {
                                // Set UI reference to Hex0
                                instrumentlist[open_instrument]->table_index = 0x0000;
                                // If this table doesn't exist yet, insert it
                                if (GetAt(&tablelist, instrumentlist[open_instrument]->table_index) == nullptr)
                                    InsertAt(&tablelist, instrumentlist[open_instrument]->table_index, new table());
                            }
                            else
                            {
                                // Set UI reference to copied table
                                instrumentlist[open_instrument]->table_index = copied_tble;
                            }
                        }
                        // If double click, add a new table
                        else if (inp->doubleclick)
                        {
                            // Set UI reference to the next empty
                            instrumentlist[open_instrument]->table_index = GetNextEmpty(&tablelist);
                            // Add the new table
                            InsertAt(&tablelist, instrumentlist[open_instrument]->table_index, new table());
                            // Copy to clipboard
                            copied_tble = instrumentlist[open_instrument]->table_index;
                        }
                        else
                        {
                            // Copy to clipboard
                            copied_tble = instrumentlist[open_instrument]->table_index;
                        }
                    }
                }
            }

            // Toggle alt mode
            if ((instrument_edit_y == 12 || instrument_edit_y == 11 || instrument_edit_y == 10) && inp->is_select_pressed())
                alt_mode = !alt_mode;

            // Editing
            if (inp->is_a_down())
            {
                // Delete table index
                if (cursor_y == 14 && cursor_x == 0 &&
                    inp->is_b_pressed())
                {
                    instrumentlist[open_instrument]->table_index = -1;
                }

                // Get edit ptr
                int* edit;

                // Default
                edit = &(instrumentlist[open_instrument]->volume_edit);

                // Get instrument edit pointer
                switch (instrument_edit_y)
                {
                case 3: // Channel Type
                    edit = reinterpret_cast<int*>(&instrumentlist[open_instrument]->type);
                    break;
                case 4: // Sample Index
                    edit = &instrumentlist[open_instrument]->sample_index;
                    break;
                case 5: // Volume
                    edit = &(instrumentlist[open_instrument]->volume_edit);
                    break;
                case 6: // Panning
                    edit = &(instrumentlist[open_instrument]->pan_edit);
                    break;
                case 7: // Tuning
                    edit = &(instrumentlist[open_instrument]->detune_edit);
                    break;
                case 8: // Pitch Sweep
                    edit = &(instrumentlist[open_instrument]->pitch_freq_edit);
                    break;
                case 9: // Base Pitch
                    edit = &(instrumentlist[open_instrument]->base_pitch);
                    break;
                case 10: // Start Time
                    edit = &(instrumentlist[open_instrument]->start_time_ms);
                    break;
                case 11: // Mid Time
                    edit = &(instrumentlist[open_instrument]->mid_time_ms);
                    break;
                case 12: // End Time
                    edit = &(instrumentlist[open_instrument]->end_time_ms);
                    break;
                case 13: // Loop
                    edit = &(instrumentlist[open_instrument]->loop);
                    break;
                case 15: // Filter Type
                    edit = reinterpret_cast<int*>(&instrumentlist[open_instrument]->filter);
                    break;
                case 16: // Filter Cutoff
                    edit = &(instrumentlist[open_instrument]->cutoff_edit);
                    break;
                case 17: // Filter Resonance
                    edit = &(instrumentlist[open_instrument]->resonance_edit);
                    break;
                case 19: // Table Index
                    edit = &instrumentlist[open_instrument]->table_index;
                    break;
                }

                // Movement keys
                if (goup || godown || goright || goleft)
                {
                    // Mod the left two digits
                    if (inp->is_shift_down() && instrument_edit_digit_count == 3)
                    {
                        if (alt_mode)
                        {
                            if (goup) (*edit) += 0x100000;
                            if (godown) (*edit) -= 0x100000;
                            if (goright) (*edit) += 0x010000;
                            if (goleft) (*edit) -= 0x010000;
                        }
                        else
                        {
                            if (goup) (*edit) += 0x001000;
                            if (godown) (*edit) -= 0x001000;
                            if (goright) (*edit) += 0x000100;
                            if (goleft) (*edit) -= 0x000100;
                        }
                    }
                    else if (instrument_edit_digit_count == 4)
                    {
                        (*edit) = (*edit) == 0 ? 1 : 0;
                    }
                    else if (inp->is_shift_down() && instrument_edit_digit_count == 2)
                    {
                        if (goup) (*edit) += 0x1000;
                        if (godown) (*edit) -= 0x1000;
                        if (goright) (*edit) += 0x0100;
                        if (goleft) (*edit) -= 0x0100;
                    }
                    // Mod a note
                    else if (instrument_edit_digit_count == 1.5)
                    {
                        // Edit note pitch
                        if (goup) (*edit) += 12;
                        if (godown) (*edit) -= 12;
                        if (goright) (*edit) += 1;
                        if (goleft) (*edit) -= 1;
                    }
                    // Mod the right two digits
                    else if (instrument_edit_digit_count == 3)
                    {
                        if (alt_mode)
                        {
                            if (goup) (*edit) += 0x001000;
                            if (godown) (*edit) -= 0x001000;
                            if (goright) (*edit) += 0x000100;
                            if (goleft) (*edit) -= 0x000100;
                        }
                        else
                        {
                            if (goup) (*edit) += 0x000010;
                            if (godown) (*edit) -= 0x000010;
                            if (goright) (*edit) += 0x000001;
                            if (goleft) (*edit) -= 0x000001;
                        }
                    }
                    else
                    {
                        if (goup && instrument_edit_digit_count != 0) (*edit) += 0x0010;
                        if (godown && instrument_edit_digit_count != 0)  (*edit) -= 0x0010;
                        if (goright) (*edit) += 0x0001;
                        if (goleft) (*edit) -= 0x0001;
                    }

                    // Correct number ranges
                    switch (instrument_edit_y)
                    {
                    case 3: // Channel Type
                        if ((*edit) < static_cast<int>(ChannelType::min))
                            (*edit) = static_cast<int>(ChannelType::max) + ((*edit) + 1);
                        if ((*edit) > static_cast<int>(ChannelType::max))
                            (*edit) = ((*edit) - 1) - static_cast<int>(ChannelType::max);
                        break;
                    case 15: // Filter Type
                        if ((*edit) < static_cast<int>(FilterType::min))
                            (*edit) = static_cast<int>(FilterType::max) + ((*edit) + 1);
                        if ((*edit) > static_cast<int>(FilterType::max))
                            (*edit) = ((*edit) - 1) - static_cast<int>(FilterType::max);
                        break;
                    case 9: // Base pitch
                        if ((*edit) < min_note)
                            (*edit) = max_note + (((*edit) - min_note) + 1);
                        if ((*edit) > max_note)
                            (*edit) = min_note + ((*edit) - 1) - max_note;
                        break;
                    case 10: // Start Time
                    case 11: // Mid Time
                    case 12: // End Time
                        // Wrap the cell between 0x000000 and 0xFFFFFF
                        if ((*edit) < 0)
                            (*edit) = 0xFFFFFF + ((*edit) + 1);
                        if ((*edit) > 0xFFFFFF)
                            (*edit) = ((*edit) - 1) - 0xFFFFFF;
                        break;
                    case 4: // Sample index
                    case 5: // Volume
                    case 6: // Panning
                    case 8: // Pitch Sweep
                    case 16: // Filter Cutoff
                    case 17: // Filter Resonance
                    case 19: // Table index
                        // Wrap the cell between 0x0000 and 0xFFFF
                        if ((*edit) < 0)
                            (*edit) = 0xFFFF + ((*edit) + 1);
                        if ((*edit) > 0xFFFF)
                            (*edit) = ((*edit) - 1) - 0xFFFF;
                        break;
                    case 7: // Tuning
                        // Wrap the cell between 0x00 and 0xFF
                        if ((*edit) < 0)
                            (*edit) = 0xFF + ((*edit) + 1);
                        if ((*edit) > 0xFF)
                            (*edit) = ((*edit) - 1) - 0xFF;
                        break;
                    }

                    // Convert edits to actual values
                    instrumentlist[open_instrument]->detune = (instrumentlist[open_instrument]->detune_edit - 128) / 2.f;
                    std::string outstr;
                    outstr = IntToHexString(instrumentlist[open_instrument]->volume_edit);
                    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                    instrumentlist[open_instrument]->volume = std::stoi(outstr.substr(0, 2), 0, 16) / 255.f;
                    instrumentlist[open_instrument]->volume_freq = (std::stoi(outstr.substr(2, 2), 0, 16) - 128) / 128.f;
                    outstr = IntToHexString(instrumentlist[open_instrument]->pan_edit);
                    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                    if (outstr.substr(0, 2) == "80")
                        instrumentlist[open_instrument]->panning = 0.5f;
                    else
                        instrumentlist[open_instrument]->panning = std::stoi(outstr.substr(0, 2), 0, 16) / 255.f;
                    instrumentlist[open_instrument]->pan_freq = (std::stoi(outstr.substr(2, 2), 0, 16) - 128) / 8.f; // std::stoi(outstr.substr(2, 2), 0, 16) / 16.f;
                    instrumentlist[open_instrument]->pitch_freq = (instrumentlist[open_instrument]->pitch_freq_edit - 32768) / (65535.f / 2.f);
                    outstr = IntToHexString(instrumentlist[open_instrument]->cutoff_edit);
                    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                    instrumentlist[open_instrument]->cutoff = std::stoi(outstr.substr(0, 2), 0, 16) / 255.f;
                    instrumentlist[open_instrument]->cutoff_freq = (std::stoi(outstr.substr(2, 2), 0, 16) - 128) / 128.f;
                    outstr = IntToHexString(instrumentlist[open_instrument]->resonance_edit);
                    outstr.insert(outstr.begin(), 4 - outstr.size(), '0');
                    instrumentlist[open_instrument]->resonance = instrumentlist[open_instrument]->resonance_edit / 65535.f;
                    instrumentlist[open_instrument]->resonance_freq = (std::stoi(outstr.substr(2, 2), 0, 16) - 128) / 128.f;
                }
            }
            // Goto page
            else if (inp->is_select_down())
            {
                // Go to the left page over
                if (goleft)
                {
                    // Reset alt mode
                    alt_mode = false;
                    // If the open_chain is valid
                    if (open_phrase != -1)
                    {
                        // Check if the phrase does not exist
                        if (GetAt(&phraselist, open_phrase) == nullptr)
                        {
                            InsertAt(&phraselist, open_phrase, new phrase());
                        }
                        // Chain
                        state = m_phrase;
                        cursor_x = SDL_clamp(cursor_x, 0, phrase_grid_w - 1);
                        cursor_y = SDL_clamp(cursor_y, 0, phrase_grid_h - 1);
                        breakend = true;
                    }
                }
                // Go to the above page over
                else if (goup)
                {
                    // Reset alt mode
                    alt_mode = false;
                    // Set open sample
                    if (cursor_y == 1 && cursor_x == 0 && instrumentlist[open_instrument]->sample_index != -1)
                    {
                        open_sample = instrumentlist[open_instrument]->sample_index;
                    }
                    // If the open_sample is valid
                    if (open_sample != -1)
                    {
                        // Check if the sample does not exist
                        if (GetAt(&samplelist, open_sample) == nullptr)
                        {
                            InsertAt(&samplelist, open_sample, new sample());
                        }
                        // Sample
                        state = m_wave;
                        sample_offset_y = 0;
                        cursor_x = 0;
                        cursor_y = 0;
                        breakend = true;
                    }
                }
                // Go to the below page
                else if (godown)
                {
                    // Set open sample
                    if (cursor_y == 14 && cursor_x == 0 && instrumentlist[open_instrument]->table_index != -1)
                    {
                        open_table = instrumentlist[open_instrument]->table_index;
                    }
                    // If the open_sample is valid
                    if (open_table != -1)
                    {
                        // Check if the sample does not exist
                        if (GetAt(&tablelist, open_table) == nullptr)
                        {
                            InsertAt(&tablelist, open_table, new table());
                        }
                        // Table
                        state = m_table;
                        //sample_offset_y = 0;
                        cursor_x = SDL_clamp(cursor_x, 0, table_grid_w - 1);
                        cursor_y = SDL_clamp(cursor_y, 0, table_grid_h - 1);
                        breakend = true;
                    }
                }
            }
            // Moving
            else
            {
                // Reset alt mode
                if (goright || goleft || godown || goup)
                    alt_mode = false;
                cursor_x += goright - goleft;
                cursor_y += godown - goup;
            }

            // wrap the cursor and clamp offsets
            if (cursor_x > instrument::sample_menu_width - 1)
                cursor_x = 0;
            if (cursor_x < 0)
                cursor_x = instrument::sample_menu_width - 1;
            if (cursor_y > instrument::sample_menu_height - 1)
                cursor_y = 0;
            if (cursor_y < 0)
                cursor_y = instrument::sample_menu_height - 1;
        }

        // Reset deep copy action flag
        if (do_deep_copy == 2)
            do_deep_copy = 0;

        // Update UI
        DrawInstrumentUI();
    }

    // Handle input for the table menu
    if (state == m_table && !breakend)
    {
        // Do actions given the context --

        // Editing
        if (inp->is_a_down())
        {
            // Edit transposition pitch
            if (cursor_x == 0)
            {
                // Movement keys
                if (goup || godown || goright || goleft)
                {
                    // Mod the left two digits
                    if (inp->is_shift_down())
                    {
                        if (goup) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 0x0C00;
                        if (godown) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 0x0C00;
                        if (goright) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 0x0100;
                        if (goleft) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 0x0100;
                    }
                    // Mod the right two digits
                    else
                    {
                        if (goup) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 0x000C;
                        if (godown) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 0x000C;
                        if (goright) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 0x0001;
                        if (goleft) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 0x0001;
                    }

                    // Wrap the cell between 0x0000 and 0xFFFF
                    if (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] < 0)
                        tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] = 0xFFFF + (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] + 1);
                    if (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] > 0xFFFF)
                        tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] = (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] - 1) - 0xFFFF;
                }
            }

            // Edit fx1,2,3
            if (cursor_x == 1 || cursor_x == 3 || cursor_x == 5)
            {
                // Set to 0 if it isn't set
                if (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] == -1 && inp->is_a_pressed())
                    tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] = copied_effect != -1 ? copied_effect : 0;
                if (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x + 1] == -1 && inp->is_a_pressed())
                    tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x + 1] = copied_effect_param != -1 ? copied_effect_param : 0;

                // Movement keys
                if (goup || godown || goright || goleft)
                {
                    if (goup) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 5;
                    if (godown) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 5;
                    if (goright) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 1;
                    if (goleft) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 1;

                    // Wrap the cell between 0 and effect list length
                    int effect_list_len = fx_length - 1;
                    if (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] < 0)
                        tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] = effect_list_len + (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] + 1);
                    if (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] > effect_list_len)
                        tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] = (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] - 1) - effect_list_len;
                }

                // Copy to clipboard
                if (tablelist[open_table]->arr[cursor_y + offset_y][cursor_x + offset_x] != -1)
                    copied_effect = tablelist[open_table]->arr[cursor_y + offset_y][cursor_x + offset_x];

                // Handle deletes
                if (inp->is_b_pressed())
                {
                    tablelist[open_table]->arr[cursor_y + offset_y][cursor_x + offset_x] = -1; // Clear effect
                    tablelist[open_table]->arr[cursor_y + offset_y][cursor_x + offset_x + 1] = -1; // Clear param
                }
            }

            // Edit fx1,2,3 param
            if (cursor_x == 2 || cursor_x == 4 || cursor_x == 6)
            {
                // Set to 0 if it isn't set
                if (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] == -1 && inp->is_a_pressed())
                    tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] = copied_effect_param != -1 ? copied_effect_param : 0x0000;

                // Movement keys
                if (goup || godown || goright || goleft)
                {
                    // Mod the left two digits
                    if (inp->is_shift_down())
                    {
                        if (goup) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 0x1000;
                        if (godown) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 0x1000;
                        if (goright) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 0x0100;
                        if (goleft) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 0x0100;
                    }
                    // Mod the right two digits
                    else
                    {
                        if (goup) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 0x0010;
                        if (godown) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 0x0010;
                        if (goright) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] += 0x0001;
                        if (goleft) tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] -= 0x0001;
                    }

                    // Wrap the cell between 0x0000 and 0xFFFF
                    if (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] < 0)
                        tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] = 0xFFFF + (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] + 1);
                    if (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] > 0xFFFF)
                        tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] = (tablelist[open_table]->arr[cursor_y + table_offset_y][cursor_x] - 1) - 0xFFFF;
                }

                // Copy to clipboard
                if (tablelist[open_table]->arr[cursor_y + offset_y][cursor_x + offset_x] != -1)
                    copied_effect_param = tablelist[open_table]->arr[cursor_y + offset_y][cursor_x + offset_x];

                // Handle deletes
                if (inp->is_b_pressed())
                    tablelist[open_table]->arr[cursor_y + offset_y][cursor_x + offset_x] = -1;
            }
        }
        // Goto page
        else if (inp->is_select_down())
        {
            // Go to the above page
            if (goup)
            {
                // If the open_instrument is valid
                if (open_instrument != -1)
                {
                    // Check if the instrument does not exist
                    if (GetAt(&instrumentlist, open_instrument) == nullptr)
                    {
                        InsertAt(&instrumentlist, open_instrument, new instrument());
                    }
                    // Instrument
                    state = m_instrument;
                    cursor_x = SDL_clamp(cursor_x, 0, GetAt(&instrumentlist, open_instrument)->type == ChannelType::synth ? instrument::synth_menu_width - 1 : instrument::sample_menu_width - 1);
                    cursor_y = SDL_clamp(cursor_y, 0, GetAt(&instrumentlist, open_instrument)->type == ChannelType::synth ? instrument::synth_menu_height - 1 : instrument::sample_menu_height - 1);
                    breakend = true;
                }
            }
        }
        // Moving
        else
        {
            cursor_x += goright - goleft;
            cursor_y += godown - goup;
        }

        // wrap the cursor and clamp offsets
        if (cursor_x > table_grid_w - 1)
            cursor_x = 0;
        if (cursor_x < 0)
            cursor_x = table_grid_w - 1;
        if (cursor_y + table_offset_y > table::len_y - 1)
        {
            cursor_y = 0;
            table_offset_y = 0;
        }
        if (cursor_y + table_offset_y < 0)
        {
            cursor_y = table_grid_h - 1;
            table_offset_y = table::len_y - table_grid_h;
        }

        // Move the page
        if (cursor_y > table_grid_h - 1)
            table_offset_y += 1;
        if (cursor_y < 0)
            table_offset_y -= 1;

        cursor_y = SDL_clamp(cursor_y, 0, table_grid_h - 1);
        table_offset_y = SDL_clamp(table_offset_y, 0, table::len_y - table_grid_h);

        // Update UI
        DrawTableUI(table_offset_y);
    }

    // Handle input for the wave menu
    if (state == m_wave && !breakend)
    {
        if (instrumentlist[open_instrument]->type == ChannelType::file) // Sample loader
        {
            // TODO: Implement safety for missing files or directories (Like if the user deletes the directory or file)

            // Modify value or open dir
            if (inp->is_a_pressed())
            {
                // Open directory
                if (selected_path_isdir)
                {
                    current_dir_length = 0;
                    sample_offset_y = 0;
                    cursor_y = 0;
                    current_dir = selected_path;
                }
                // Open file
                else
                {
                    samplelist[open_sample]->path = selected_path;
                    int at = 0;
                    for (auto c : samplelist[open_sample]->path)
                        samplelist[open_sample]->path[at++] = (char)toupper(c);

                    // Load sample audio into memory and assign index to sample object
                    if (samplelist[open_sample]->sound_index != -1 && geptr->CheckSound(samplelist[open_sample]->sound_index) == true)
                    {
                        StopAllChannels();
                        ReleaseSound(samplelist[open_sample]->sound_index); // waits until no voice is reading it
                    }
                    samplelist[open_sample]->sound_index = geptr->AddSound(samplelist[open_sample]->path.c_str());

                    // Preview the audio
                    if (pause_song && inp->is_a_pressed())
                    {
                        // Play the note
                        PreviewSample(open_channel, open_sample);
                    }
                }
            }
            // Go up a directory
            if (inp->is_b_pressed())
            {
                if (current_dir != main_dir)
                {
                    // Remove trailing backslash
                    current_dir = current_dir.substr(0, current_dir.length() - 1);
                    // Remove all to the last backslash
                    current_dir = current_dir.substr(0, current_dir.find_last_of('\\') + 1);
                    // Reset
                    current_dir_length = 0;
                    sample_offset_y = 0;
                    cursor_y = 0;
                }
            }
            // Goto page
            else if (inp->is_select_down())
            {
                // Go to the down page over
                if (godown)
                {
                    // If the open_instrument is valid
                    if (open_instrument != -1)
                    {
                        // Check if the instrument does not exist
                        if (GetAt(&instrumentlist, open_instrument) == nullptr)
                        {
                            InsertAt(&instrumentlist, open_instrument, new instrument());
                        }
                        // Chain
                        state = m_instrument;
                        cursor_x = SDL_clamp(cursor_x, 0, GetAt(&instrumentlist, open_instrument)->type == ChannelType::synth ? instrument::synth_menu_width - 1 : instrument::sample_menu_width - 1);
                        cursor_y = SDL_clamp(cursor_y, 0, GetAt(&instrumentlist, open_instrument)->type == ChannelType::synth ? instrument::synth_menu_height - 1 : instrument::sample_menu_height - 1);
                        breakend = true;
                    }
                }
            }
            // Moving
            else
            {
                cursor_y += godown - goup;
            }

            // wrap the cursor and clamp offsets
            if (cursor_y + sample_offset_y > current_dir_length - 1)
            {
                cursor_y = 0;
                sample_offset_y = 0;
            }
            if (cursor_y + sample_offset_y < 0)
            {
                cursor_y = std::min(current_dir_length - 1, file_display_count - 1);
                sample_offset_y = std::max(0, current_dir_length - file_display_count);
            }

            // Move the page
            if (cursor_y > file_display_count - 1)
                sample_offset_y += 1;
            if (cursor_y < 0)
                sample_offset_y -= 1;

            cursor_y = SDL_clamp(cursor_y, 0, file_display_count - 1);
            sample_offset_y = SDL_clamp(sample_offset_y, 0, std::max(0, current_dir_length - file_display_count));
        }
        else // Waveform editor
        {

        }

        // Stop previewing the audio
        if (inp->is_a_released())
        {
            StopChannel(open_channel);
        }

        // Update UI
        DrawWaveUI();
    }

    // Draw the map for where you are in the UI
    DrawMap();

    // Handle repeating movement from hold presses
    HandleMovementRepeaters();

    // If we want to pause then pause
    if (willpause)
    {
        // Stop audio
        StopAllChannels();
        // Pause song
        pause_song = willpause;
    }
}