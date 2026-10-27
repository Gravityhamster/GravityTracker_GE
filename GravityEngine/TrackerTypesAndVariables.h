#pragma once
#include "GravityEngineSDL.h"
#include "TrackerFunctionSignatures.h"
#include "base64/base64.h"

// Reference for the core of the Gravity Engine
extern GravityEngine_Core* geptr;

// Enums --

// Editor held modifier
enum edit_mod
{
    left,
    right,
    center
};

// Playing type
enum playing_type
{
    pt_song,
    pt_chain,
    pt_phrase,
    pt_chain_all,
    pt_phrase_all,
    pt_preview
};

// Song editor menu
enum menu
{
    m_song,
    m_chain,
    m_phrase,
    m_instrument,
    m_options,
    m_table,
    m_wave
};

// Compile-time constant (must be defined, not extern)
const int channelcount = 64; // How many audio channels in the song

// Global Variables --

extern int step_has_happened;
extern playing_type play_context; // What type of play are we doing
extern int ticknumber; // Track tick progress
extern int cursor_x; // X location of the user's cursor
extern int cursor_y; // Y location of the user's cursor
extern int offset_x; // X offset of the editor scroll
extern int offset_y; // Y offset of the editor scroll
extern int chain_offset_y; // Y offset when editing the chain
extern int phrase_offset_y; // Y offset of the editor scroll
extern int table_offset_y; // Y offset of the editor scroll
extern int sample_offset_y; // Y offset of the samples selector scroll
extern int inputholdtimer; // The timer for checking if an input should be considered held-down
extern int inputholdthreshold; // Frames til in input should start repeating
extern int inputholddelay; // How many frames to skip on hold (2 == every other, 3 == every other 3, etc.) 
extern int rowcount; // How many rows in the song - 65535 chains * 16 phrases * 16 steps = 16776960 steps / 4 steps = 4194240 beats
extern std::atomic<int> bpm; // Beats per minute of the song
extern int set_bpm;
extern int tps; // Ticks per step of the song
extern int fps; // Frame rate in hz of the UI
extern double ticklength; // Nanoseconds per tick
extern int song_grid_h; // Height of the Song Editor UI
extern int song_grid_w; // Width of the Song Editor UI
extern int chain_grid_h; // Height of the Chain Editor UI
extern int chain_grid_w; // Width of the Chain Editor UI
extern int phrase_grid_h; // Height of the Phrase Editor UI
extern int phrase_grid_w; // Width of the Phrase Editor UI
extern int table_grid_h; // Height of the Table Editor UI
extern int table_grid_w; // Width of the table Editor UI
extern bool running; // Is the song currently playing?
extern edit_mod leftrightcenter; // Editing state for which part of the number we are editing
extern int copied_chain; // Clipboard for copying a chain
extern int copied_phrase; // Clipboard for copying a phrase
extern int copied_note; // Clipboard for copying a note
extern int copied_instr; // Clipboard for copying an instrument
extern int copied_effect; // Clipboard for copying an instrument
extern int copied_effect_param; // Clipboard for copying an instrument
extern int copied_smple; // Clipboard for copying a sample
extern int copied_tble; // Clipboard for copying a table
extern int open_channel; // Track which channel we've opened on
extern int open_chain; // Tracking which chain we have open
extern int open_chain_index; // Index of chain in the song
extern int open_phrase; // Tracking which phrase we have open
extern int open_phrase_index; // Index of phrase in the chain in the song
extern int playing_channel; // Track which channel we're playing
extern int open_instrument; // Tracking which instrument we have open
extern int open_sample; // Tracking which sample we have open
extern int open_table; // Tracking which table we have open
extern int fx_length; // Number of effects
extern char fx[]; // List of effects
extern int min_note; // Minimum note that can be inserted
extern int max_note; // Maximum note that can be inserted
extern bool pause_song; // Pause the song progression
extern bool play_thread; // Play thread check flag
extern int do_deep_copy; // Track progress for deep copy
extern float instrument_edit_digit_count; // Flag for how many digits to expect an edit to be in the instr editor
extern int instrument_edit_y; // Flag for which row to edit in the instr editor
extern int file_display_count; // How many files to display in the file browser
extern int current_dir_length; // Track the length of the current directory string for file browser
extern std::string main_dir; // Main directory for file browser
extern std::string current_dir; // Current directory for file browser
extern std::string selected_path; // Path of the selected file or directory
extern bool selected_path_isdir; // Flag for if the selected path is a directory or a file
extern bool alt_mode; // Alt mode is a special mode that allows for more advanced editing features
extern menu state; // What menu are we in
extern int inputgetter; // Input getter
extern int** songgrid; //[0xffff][64];

// Tracker object concepts --

// Settings
class settings
{
public:
    enum setting_type
    {
        fourbithex,
        number,
        button
    };

    std::vector<std::string> setting_labels;
    std::vector<std::atomic<int>*> setting_pointers;
    std::vector<std::tuple<int, int>> setting_ranges;
    std::vector<setting_type> setting_types;

private:
    void LoadFromSaveString(std::string s)
    {
        // TODO : Load a string
    }

public:
    settings()
    {
        // BPM
        setting_labels.push_back("BPM");
        setting_pointers.push_back(&bpm);
        setting_ranges.push_back({ 40, 300 });
        setting_types.push_back(number);

        // Save
        setting_labels.push_back("SAVE");
        setting_pointers.push_back(nullptr);
        setting_ranges.push_back({ -1, -1 });
        setting_types.push_back(button);

        // Load
        setting_labels.push_back("LOAD");
        setting_pointers.push_back(nullptr);
        setting_ranges.push_back({ -1, -1 });
        setting_types.push_back(button);
    }

    // Load from save string constructor
    settings(std::string s) : settings()
    {
        LoadFromSaveString(s);
    }

    ~settings() {}

    std::string GetSettingsSaveString()
    {
        std::string s = "-=SETTINGS=-\n";

        // Loop through settings and write them
        int i = 0;
        for (auto l : setting_labels)
        {
            if (setting_pointers[i] != nullptr)
                s += l + " : " + std::to_string(setting_pointers[i]->load()) + "\n";
            i++;
        }

        s += ";\n";

        return s;
    }
};

extern settings* global_settings;

// Chains - Lists of phrases
class chain
{
public:
    static const int length = 16;
    int arr[length]; // 1w x 16h - List of phrases
    int arr_transpose[length]; // 1w x 16h - List of phrases

    // Create chain
    chain()
    {
        // Fill the chain with blanks
        for (int x = 0; x < length; x++)
            arr[x] = -1;
        for (int x = 0; x < length; x++)
            arr_transpose[x] = 0x0000;
    };

    // Create chain from save string
    chain(std::string save_string)
    {
        // TODO : Implement load from a save_string
    }

    // Create save string for chain
    std::string SaveString()
    {
        // Build arr 
        std::string s = "arr{";
        for (int a : arr)
            s += std::to_string(a) + ",";
        s += "},";
        // Build arr transpose
        s += "arr_transpose{";
        for (int a : arr_transpose)
            s += std::to_string(a) + ",";
        s += "}_";

        return s;
    }

    // Destruct chain
    ~chain() {};
};

// Phrases - Lists of notes (i.e. 4/4 Measures)
class phrase
{
public:
    static const int len_x = 8;
    static const int len_y = 16;
    int arr[len_y][len_x]; // 8w x 16h - List of notes

    // Create pharse
    phrase()
    {
        // Fill the phrase with blanks
        for (int y = 0; y < len_y; y++)
            for (int x = 0; x < len_x; x++)
                arr[y][x] = x == 0 ? -9999 : -1;
    };

    // Create phrase from save string
    phrase(std::string save_string)
    {
        // TODO : Implement load from a save_string
    }

    // Create save string for phrase
    std::string SaveString()
    {
        // Build arr 
        std::string s = "";
        for (int y = 0; y < len_y; y++)
        {
            s += "arr[" + std::to_string(y) + "]{";
            for (int x = 0; x < len_x; x++)
            {
                s += std::to_string(arr[y][x]) + ",";
            }
            s += "},";
        }

        s += "_";

        return s;
    }

    // Destruct phrase
    ~phrase() {};
};

// Samples - Sound sample references
class sample
{
public:
    std::string path = "";
    int sound_index = -1;

    // Create save string for sample
    std::string SaveString()
    {
        std::string s = "";

        s += "path=" + path + ",";
        s += "index=" + std::to_string(sound_index) + ";";
        
        return s;
    }
};

// Instruments - Note audio definitions
class instrument
{
public:
    // Audio parameters
    static const int synth_menu_width = 1;
    static const int synth_menu_height = 11;
    static const int sample_menu_width = 1;
    static const int sample_menu_height = 15;

    // Note to self: 0x80 (128) is the middle number in 0xFF (255).

    // Synth or Sample
    ChannelType type = ChannelType::synth;

    // Parameters for both
    int detune_edit = 0x80; // 0x80 = 0, 0x00 = -128, 0xFF = 127
    float detune = 0.f; // Add to freq
    int volume_edit = 0xFF80; // 0xFF = 1 | 0x80 = 0
    float volume = 1.f; // Volume
    float volume_freq = 0.0; // Linear volume sweep (positive up, negative down)
    int pan_edit = 0x8080; // 0x80 = 0.5 | 0x80 = 0
    float panning = 0.5f; // Panning amount (0.0 = L, 0.5 = C, 1.0 = R)
    float pan_freq = 0.0; // Ping-pong pan frequency
    int pitch_freq_edit = 0x8000; // 0x8000 = 0, 0x0000 = -32768, 0xFFFF = 32767
    float pitch_freq = 0.0; // Linear pitch sweep speed (positive up, negative down)
    int cutoff_edit = 0x8080; // 0x80 = 0.5 | 0x80 = 0
    float cutoff = 0.0f; // Filter cutoff
    float cutoff_freq = 0.0; // Linear cutoff sweep (positive up, negative down)
    int resonance_edit = 0x8080; // 0x80 = 0.5 | 0x80 = 0
    float resonance = 0.0f; // Filter resonance
    float resonance_freq = 0.0; // Linear resonance sweep (positive up, negative down)
    SynthWaveForm waveform = SynthWaveForm::sine; // Synth wave type
    FilterType filter = FilterType::none; // Lowpass, bandpass, or highpass filter

    // Synth parameters
    int pw_edit = 0x8000; // 0x80 = 0.5, 0x00 = 0;
    float pulse_width = 0.5; // Pulse Width (Only for Pulse Wave Synth)
    float pulse_width_freq = 0.0; // Ping-pong pulse-width pan frequency (Only for Pulse Wave Synth)

    // Sample parameters
    int base_pitch = 39;
    int sample_index = -1;
    int start_time_ms = 0x000000;
    int mid_time_ms = 0x000000;
    int end_time_ms = 0xFFFFFF;
    int loop = false;

    // Table reference
    int table_index = -1;

    // Methods
    instrument* DeepCopyInstrument()
    {
        // New instrument
        auto i = new instrument();

        // Apply parameters
        i->type = type;
        i->detune_edit = detune_edit;
        i->detune = detune;
        i->volume_edit = volume_edit;
        i->volume = volume;
        i->volume_freq = volume_freq;
        i->pan_edit = pan_edit;
        i->panning = panning;
        i->pan_freq = pan_freq;
        i->pw_edit = pw_edit;
        i->pulse_width = pulse_width;
        i->pulse_width_freq = pulse_width_freq;
        i->pitch_freq_edit = pitch_freq_edit;
        i->pitch_freq = pitch_freq;
        i->cutoff_edit = cutoff_edit;
        i->cutoff = cutoff;
        i->resonance_edit = resonance_edit;
        i->resonance = resonance;
        i->waveform = waveform;
        i->filter = filter;
        i->base_pitch = base_pitch;
        i->sample_index = sample_index;
        i->start_time_ms = start_time_ms;
        i->mid_time_ms = mid_time_ms;
        i->end_time_ms = end_time_ms;
        i->loop = loop;
        i->table_index = table_index;

        // Return copy
        return i;
    }

    // Create save string
    std::string SaveString()
    {
        std::string s = "";

        // Build the save string
        s += "type=" + std::to_string((int)type) + ",";
        s += "detune_edit=" + std::to_string(detune_edit) + ",";
        s += "detune=" + std::to_string(detune) + ",";
        s += "volume_edit=" + std::to_string(volume_edit) + ",";
        s += "volume=" + std::to_string(volume) + ",";
        s += "volume_freq=" + std::to_string(volume_freq) + ",";
        s += "pan_edit=" + std::to_string(pan_edit) + ",";
        s += "panning=" + std::to_string(panning) + ",";
        s += "pan_freq=" + std::to_string(pan_freq) + ",";
        s += "pitch_freq_edit=" + std::to_string(pitch_freq_edit) + ",";
        s += "pitch_freq=" + std::to_string(pitch_freq) + ",";
        s += "cutoff_edit=" + std::to_string(cutoff_edit) + ",";
        s += "cutoff=" + std::to_string(cutoff) + ",";
        s += "cutoff_freq=" + std::to_string(cutoff_freq) + ",";
        s += "resonance_edit=" + std::to_string(resonance_edit) + ",";
        s += "resonance=" + std::to_string(resonance) + ",";
        s += "resonance_freq=" + std::to_string(resonance_freq) + ",";
        s += "waveform=" + std::to_string((int)waveform) + ",";
        s += "filter=" + std::to_string((int)filter) + ",";
        s += "pw_edit=" + std::to_string(pw_edit) + ",";
        s += "pulse_width=" + std::to_string(pulse_width) + ",";
        s += "pulse_width_freq=" + std::to_string(pulse_width_freq) + ",";
        s += "base_pitch=" + std::to_string(base_pitch) + ",";
        s += "sample_index=" + std::to_string(sample_index) + ",";
        s += "start_time_ms=" + std::to_string(start_time_ms) + ",";
        s += "mid_time_ms=" + std::to_string(mid_time_ms) + ",";
        s += "end_time_ms=" + std::to_string(end_time_ms) + ",";
        s += "loop=" + std::to_string(loop) + ";";

        return s;
    }
};


// Tables - List of modulations for the currently playing instrument
class table
{
public:
    static const int len_x = 7;
    static const int len_y = 16;
    int arr[len_y][len_x]; // 7w x 16h - List of ticks for sound automation 

    // 0 : 00 to FF - Transpose
    // 1 : Effect Type
    // 2 : 0000 to FFFF - Effect Value
    // 3 : Effect Type
    // 4 : 0000 to FFFF - Effect Value
    // 5 : Effect Type
    // 6 : 0000 to FFFF - Effect Value

    // Create phrase
    table()
    {
        // Fill the chain with blanks
        for (int y = 0; y < len_y; y++)
            for (int x = 0; x < len_x; x++)
                arr[y][x] = (x == 0 ? 0x0000 : -1);
    };

    // Destruct phrase
    ~table() {};

    // Methods
    table* DeepCopyTable()
    {
        // New sample
        auto t = new table();

        // Apply parameters
        for (int i = 0; i < len_y; i++)
            for (int q = 0; q < len_x; q++)
                t->arr[i][q] = arr[i][q];

        // Return copy
        return t;
    }
};

// List of chains and phrases
extern std::vector<chain*> chainlist;
extern std::vector<phrase*> phraselist;
extern std::vector<instrument*> instrumentlist;
extern std::vector<sample*> samplelist;
extern std::vector<table*> tablelist;

// ChannelSequencer - Track position of the channel in time in the song
class channelsequencer
{
public:
    ChannelType type = ChannelType::synth;
    std::atomic<int> channelnumber = -1; // Which channel is this sequencer assigned to?
    std::atomic<int> chain_ptr = 0; // Int position of the channel in the song
    std::atomic<int> phrase_ptr = 0; // Int position of the channel in the chain
    std::atomic<int> step_ptr = 0; // Int position of the channel in the phrase
    std::atomic<int> tick_ptr = 0; // Int position of the channel in the table
    std::atomic<int> playing_chain = -1;
    std::atomic<int> playing_phrase = -1;
    std::atomic<int> playing_table = -1;
    std::atomic<int> playing_freq = -9999;
    std::atomic<int> playing_instrument = -1;
    std::atomic<bool> cant_play = false;
    std::atomic<int> delay_ticks = 0;
    std::atomic<int> delay_tick_offsetter = 0;
    std::atomic<int> catchup_ticks = 0;
    std::atomic<bool> first_delay_passed = true;
    std::atomic<bool> skip_next_fx = false;

    // Count tick
    void sub_step(double* freq)
    {
        // If channel is in a locked state, do not play in the song context
        if (cant_play == true && play_context == pt_song)
            return;

        // Handle transposition
        if (playing_table != -1)
		{
			auto t = GetAt(&tablelist, playing_table);
			if (t != nullptr)
			{
				// Apply transposition
                int table_trsp = t->arr[tick_ptr][0];
                UpdateStepPitch(channelnumber, 
                    play_context == pt_preview ? open_phrase : playing_phrase.load(),
                    play_context == pt_preview ? cursor_y + offset_y : step_ptr.load() - 1,
                    table_trsp, freq);
            
                // When we play an instrument for the first time, it has already done the first tick of the table
                if (skip_next_fx)
                    skip_next_fx = false;
                else
                {
                    // Read and apply instrument functions
                    int table_fx1 = t->arr[tick_ptr][1];
                    int table_fx1_p = t->arr[tick_ptr][2];
                    int table_fx2 = t->arr[tick_ptr][3];
                    int table_fx2_p = t->arr[tick_ptr][4];
                    int table_fx3 = t->arr[tick_ptr][5];
                    int table_fx3_p = t->arr[tick_ptr][6];
                    GetFX(channelnumber, type, -9999, table_fx1, table_fx1_p, table_fx2, table_fx2_p, table_fx3, table_fx3_p, after);
                }
            }
        }

        // Increment tick in table
        tick_ptr++;
        // Go back to the 0th tick if we have reached the end of the table
        tick_ptr = tick_ptr % table::len_y;
    }

    // Count step
    void step()
    {
        if (cant_play == true && play_context == pt_song)
            return;

        // TODO : Play step and sub-step-level effects

        // Play context within this local phrase
        if ((play_context == pt_phrase && playing_channel == channelnumber) || play_context == pt_phrase_all)
        {
            PlayStepPhrase(channelnumber, playing_phrase, step_ptr);
        }
        if ((play_context == pt_chain && playing_channel == channelnumber) || play_context == pt_chain_all)
        {
            PlayStepChain(channelnumber, playing_chain, phrase_ptr, step_ptr);
        }
        if (play_context == pt_song && !cant_play)
        {
            PlayStepSong(channelnumber, chain_ptr, phrase_ptr, step_ptr);
        }

        // Increment step in phrase
        step_ptr++;
        // If the step ptr has reached the end of the phrase, go back to the 0th step
        if (step_ptr % phrase::len_y == 0)
        {
            step_ptr = 0;
            // Increment the phrase pointer in the chain
            inc_phrase();
        }
    }

    // Prepare for playing the song
    // int start_point : Where to start in the channel's chain list
    bool init_song_play(int start_point)
    {
        chain_ptr = start_point;
        phrase_ptr = 0;
        step_ptr = 0;
        tick_ptr = 0;
        ticknumber = 0;
        delay_ticks = 0;
        delay_tick_offsetter = 0;
        catchup_ticks = 0;
        first_delay_passed = true;
        bool first = true;

        // Keep moving back til we get to a valid play point to play at
        do
        {
            if (chain_ptr > 0 && cant_play && !first)
                chain_ptr--;
            // Report for now that we can play
            cant_play = false;
            // Is there a channel reference here?
            if (songgrid[chain_ptr][channelnumber] == -1)
                cant_play = true;
            else
            {
                // Does the chain exist
                auto c = GetAt(&chainlist, songgrid[chain_ptr][channelnumber]);
                if (c == nullptr)
                    cant_play = true;
                else
                {
                    // Is there a phrase here?
                    auto p = GetAt(&phraselist, c->arr[phrase_ptr]);
                    if (p == nullptr)
                        cant_play = true;
                }
            }
            first = false;
        } while (chain_ptr > 0 && cant_play);

        // Report that this channel needs to sit out
        return cant_play;
    }

private:

    void inc_phrase()
    {
        // If we are playing only the phrase, we don't want to increment
        if (play_context != pt_phrase && play_context != pt_phrase_all)
        {
            // Increment phrase in chain
            phrase_ptr++;
            if (phrase_ptr % chain::length == 0)
            {
                phrase_ptr = 0;
                // Increment the chain pointer in the song
                inc_chain();
            }
            else
            {
                // Check if the channel is checking a valid chain
                auto play_chain = GetAt(&chainlist, songgrid[chain_ptr][channelnumber]);
                if (play_chain != nullptr)
                {
                    // If the channel is currently on a null phrase, go back to start
                    if (play_chain->arr[phrase_ptr] == -1)
                    {
                        phrase_ptr = 0;
                        // Increment the chain pointer in the song
                        inc_chain();
                    }
                }
            }
        }
    }

    void inc_chain()
    {
        // If we are playing only the chain, we don't want to increment
        // We don't need to check phrase because this function is only called from phrase
        if (play_context != pt_chain && play_context != pt_chain_all)
        {
            // Increment chain in song
            chain_ptr++;
            // If the chain ptr has reached the end of the song, go back to the 0th chain
            if (chain_ptr % rowcount == 0)
            {
                chain_ptr = 0;
            }

            // If we are on an empty chain or phrase, move up til we find the beginning of the list of consecutive chains
            bool need_to_go_back = false;
            // Check if the channel is checking a valid chain
            {
                auto play_chain = GetAt(&chainlist, songgrid[chain_ptr][channelnumber]);
                if (play_chain != nullptr)
                {
                    auto play_phrase = GetAt(&phraselist, play_chain->arr[phrase_ptr]);
                    if (play_phrase == nullptr)
                        need_to_go_back = true;
                }
                else
                {
                    need_to_go_back = true;
                }
            }

            // If we need to go back...
            if (need_to_go_back)
            {
                // Go back a step
                chain_ptr--;
                // Keep going back every step until we have made it to the beginning or we have found the last blank
                while (need_to_go_back && chain_ptr != -1)
                {
                    // Check if the chain is valid
                    auto play_chain = GetAt(&chainlist, songgrid[chain_ptr][channelnumber]);
                    if (play_chain != nullptr)
                    {
                        // Check if the phrase is valid
                        auto play_phrase = GetAt(&phraselist, play_chain->arr[phrase_ptr]);
                        if (play_phrase == nullptr)
                            need_to_go_back = false; // Valid
                    }
                    else
                    {
                        need_to_go_back = false; // Valid
                    }
                    // If not valid, check next chain back
                    if (need_to_go_back)
                        chain_ptr--;
                }
                // Get back to the start
                chain_ptr++;
            }
        }
    }
};

// Object to handle all inputs
class input : public virtual GravityEngine_Object
{
public:

    // Keycodes for inputs --

    // Keycode for Up button
    const SDL_Scancode up = SDL_SCANCODE_UP;

    // Keycode for Down button
    const SDL_Scancode down = SDL_SCANCODE_DOWN;

    // Keycode for Left button
    const SDL_Scancode left = SDL_SCANCODE_LEFT;

    // Keycode for Right button
    const SDL_Scancode right = SDL_SCANCODE_RIGHT;

    // Key code for A button
    const SDL_Scancode a = SDL_SCANCODE_Z;

    // Key code for B button
    const SDL_Scancode b = SDL_SCANCODE_X;

    // Key code for Start button
    const SDL_Scancode start = SDL_SCANCODE_RETURN;

    // Key code for Select button
    const SDL_Scancode select = SDL_SCANCODE_SPACE;

    // Key code for Shift button
    const SDL_Scancode shift = SDL_SCANCODE_LSHIFT;

    // Get the last keycode pressed
    SDL_Scancode last_pressed = SDL_SCANCODE_UNKNOWN;
    bool doubleclick = false;
    std::string presscode = "0";
    std::string lastpresscode = "0";

private:

    // State variables for keys
    bool was_up = false; // Button was pressed last frame
    bool is_up = false; // Button is pressed this frame

    bool was_down = false; // Button was pressed last frame
    bool is_down = false; // Button is pressed this frame

    bool was_left = false; // Button was pressed last frame
    bool is_left = false; // Button is pressed this frame

    bool was_right = false; // Button was pressed last frame
    bool is_right = false; // Button is pressed this frame

    bool was_a = false; // Button was pressed last frame
    bool is_a = false; // Button is pressed this frame

    bool was_b = false; // Button was pressed last frame
    bool is_b = false; // Button is pressed this frame

    bool was_start = false; // Button was pressed last frame
    bool is_start = false; // Button is pressed this frame

    bool was_select = false; // Button was pressed last frame
    bool is_select = false; // Button is pressed this frame

    bool was_shift = false; // Button was pressed last frame
    bool is_shift = false; // Button is pressed this frame

public:
    input() {};
    ~input() {};
    void begin_step()
    {
        was_up = is_up; // Save last frame
        is_up = geptr->GetKeyState(up); // Get this frame
        was_down = is_down; // Save last frame
        is_down = geptr->GetKeyState(down); // Get this frame
        was_left = is_left; // Save last frame
        is_left = geptr->GetKeyState(left); // Get this frame
        was_right = is_right; // Save last frame
        is_right = geptr->GetKeyState(right); // Get this frame
        was_a = is_a; // Save last frame
        is_a = geptr->GetKeyState(a); // Get this frame
        was_b = is_b; // Save last frame
        is_b = geptr->GetKeyState(b); // Get this frame
        was_start = is_start; // Save last frame
        is_start = geptr->GetKeyState(start); // Get this frame
        was_select = is_select; // Save last frame
        is_select = geptr->GetKeyState(select); // Get this frame
        was_shift = is_shift; // Save last frame
        is_shift = geptr->GetKeyState(shift); // Get this frame

        // Handle all double taps
        std::string temppresscode =
            std::to_string(is_up && !was_up) +
            std::to_string(is_down && !was_down) +
            std::to_string(is_left && !was_left) +
            std::to_string(is_right && !was_right) +
            std::to_string(is_a && !was_a) +
            std::to_string(is_b && !was_b) +
            std::to_string(is_start && !was_start) +
            std::to_string(is_select && !was_select) +
            std::to_string(is_shift && !was_shift);
        // Keep a history of inputs
        if (std::stoi(temppresscode) != 0)
        {
            lastpresscode = presscode;
            presscode = temppresscode;
        }
        temppresscode = presscode;
        // Truncate the presscode down to 1s if the value contains 1s
        if (str_contains(temppresscode, "1")) temppresscode = str_remove(temppresscode, '0');
        // If the history is equal and the total number of inputs == 1, then a doubleclick has occured
        if (lastpresscode == presscode && std::stoi(temppresscode) == 1)
            doubleclick = true;
        else
            doubleclick = false;
    };
    void step() {};
    void end_step() {};

    bool is_up_pressed() { return is_up && !was_up; }
    bool is_up_down() { return is_up; }
    bool is_up_released() { return was_up && !is_up; }

    bool is_down_pressed() { return is_down && !was_down; }
    bool is_down_down() { return is_down; }
    bool is_down_released() { return was_down && !is_down; }

    bool is_left_pressed() { return is_left && !was_left; }
    bool is_left_down() { return is_left; }
    bool is_left_released() { return was_left && !is_left; }

    bool is_right_pressed() { return is_right && !was_right; }
    bool is_right_down() { return is_right; }
    bool is_right_released() { return was_right && !is_right; }

    bool is_a_pressed() { return is_a && !was_a; }
    bool is_a_down() { return is_a; }
    bool is_a_released() { return was_a && !is_a; }

    bool is_b_pressed() { return is_b && !was_b; }
    bool is_b_down() { return is_b; }
    bool is_b_released() { return was_b && !is_b; }

    bool is_start_pressed() { return is_start && !was_start; }
    bool is_start_down() { return is_start; }
    bool is_start_released() { return was_start && !is_start; }

    bool is_select_pressed() { return is_select && !was_select; }
    bool is_select_down() { return is_select; }
    bool is_select_released() { return was_select && !is_select; }

    bool is_shift_pressed() { return is_shift && !was_shift; }
    bool is_shift_down() { return is_shift; }
    bool is_shift_released() { return was_shift && !is_shift; }
};

// Data structures --
extern GravityEngine_Synth* audiosynth;
extern GravityEngine_Sampler* audiosampler;
extern channelsequencer channellist[channelcount];

// Tracker colors --
extern color primary_text_a;
extern color header_text_a;
extern color primary_text_b;
extern color header_text_b;
extern color body_text_a;
extern color body_text_b;