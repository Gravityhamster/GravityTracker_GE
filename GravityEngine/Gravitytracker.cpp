#include "TrackerTypesAndVariables.h"
#include <list>

// Global pointer definition
GravityEngine_Core* geptr;

// Global variable definitions
playing_type play_context = pt_song;
int ticknumber = 0;
int cursor_x;
int cursor_y;
int offset_x;
int offset_y;
int chain_offset_y;
int phrase_offset_y;
int table_offset_y;
int sample_offset_y;
int inputholdtimer = 0;
int inputholdthreshold = 15;
int inputholddelay = 2;
int rowcount = 0xffff;
std::atomic<int> bpm = 125;
int set_bpm = bpm.load();
int tps = 6;
int fps = 60;
double ticklength = 0;
int song_grid_h;
int song_grid_w;
int chain_grid_h;
int chain_grid_w;
int phrase_grid_h;
int phrase_grid_w;
int table_grid_h;
int table_grid_w;
bool running = false;
edit_mod leftrightcenter = center;
int copied_chain = -1;
int copied_phrase = -1;
int copied_note = -9999;
int copied_instr = -1;
int copied_effect = -1;
int copied_effect_param = -1;
int copied_smple = -1;
int copied_tble = -1;
int open_channel = -1;
int open_chain = -1;
int open_chain_index = 0;
int open_phrase = -1;
int open_phrase_index = 0;
int playing_channel = -1;
int open_instrument = -1;
int open_sample = -1;
int open_table = -1;
int fx_length = 5;
char fx[] = { 'E', 'V', 'K', 'D', 'T'};
int min_note = -12;
int max_note = 107;
bool pause_song = true;
bool play_thread = false;
int do_deep_copy = 0;
float instrument_edit_digit_count = -1;
int instrument_edit_y = -1;
int file_display_count = 10;
int current_dir_length = 0;
std::string main_dir = ".\\SampleLibrary\\";
std::string current_dir = main_dir;
std::string selected_path = "";
bool selected_path_isdir = false;
bool alt_mode = false;
menu state = m_song;
int inputgetter;
int** songgrid;
settings* global_settings;

// Data structures
std::vector<instrument*> instrumentlist;
std::vector<sample*> samplelist;
std::vector<chain*> chainlist;
std::vector<phrase*> phraselist;
std::vector<table*> tablelist;
GravityEngine_Synth* audiosynth;
GravityEngine_Sampler* audiosampler;
channelsequencer channellist[channelcount];

// Tracker colors
color primary_text_a = { {255, 255, 255}, {0, 0, 0} };
color header_text_a = { {255, 255, 255}, {0, 0, 100} };
color primary_text_b = { {0, 0, 0}, {255, 255, 255} };
color header_text_b = { {0, 0, 100}, {255, 255, 255} };
color body_text_a = { {255, 255, 255}, {0, 0, 0} };
color body_text_b = { {0, 0, 0}, {255, 255, 255} };

// Master pre code
void GameInit()
{
    // Set UI grid w and h
    song_grid_h = std::min(rowcount, geptr->GetCanvasH() - 4);
    song_grid_w = std::min(channelcount, (geptr->GetCanvasW() - 8) / 5);
    phrase_grid_h = std::min(16, geptr->GetCanvasH() - 4);
    phrase_grid_w = phrase::len_x;
    table_grid_h = std::min(16, geptr->GetCanvasH() - 4);
    table_grid_w = table::len_x;
    chain_grid_h = std::min(16, geptr->GetCanvasH() - 4);
    chain_grid_w = 2;
    file_display_count = song_grid_h - 6;

	// Init synth
	audiosynth = new GravityEngine_Synth();
	audiosampler = new GravityEngine_Sampler();
    global_settings = new settings();

    // Init channel sequencers
    for (int i = 0; i < channelcount; i++)
    {
        channellist[i].channelnumber = i;
		audiosynth->voices.insert(audiosynth->voices.end(), new GravityEngine_Voice());
		audiosampler->voices.insert(audiosampler->voices.end(), new GravityEngine_Voice());
    }

	// Bind the synth to channel 0
    geptr->BindSynthToChannel(audiosynth, 0);
	// Bind the sampler to channel 1
    geptr->BindSamplerToChannel(audiosampler, 1);

    // Init song phrase list
    songgrid = new int* [rowcount];
    for (int i = 0; i < rowcount; i++)
        songgrid[i] = new int[channelcount];
    for (int y = 0; y < rowcount; y++)
    {
        for (int x = 0; x < channelcount; x++)
        {
            songgrid[y][x] = -1;
        }
    }

    // Start song UI
    DrawSongUI(0, 0);

    // Add the input check object
    inputgetter = geptr->AddObject(new input());

    // Get the length of the tick
    ticklength = BpmToTicklength(bpm);
}

// Master pre code
void PreGameLoop()
{
    // Get input and apply to the editor as appropriate
    EditorControl();

    // Debug : Draw channel 0's sequence
    geptr->DrawTextString(10, 0, geptr->entity,
        std::to_string(channellist[0].chain_ptr) + " - " +
        std::to_string(channellist[0].phrase_ptr) + " - " +
        std::to_string(channellist[0].step_ptr) + " - " +
        std::to_string(channellist[0].tick_ptr) + "     " + std::to_string(bpm.load()),
        primary_text_a);
}

// Master post code
void PostGameLoop()
{
}

// Master exit game code
void ExitGameLoop()
{
    // Stop playing
    StopSequenceThread();
}

int main()
{
    // Init engine - 128x72 is generally the largest you can get and still maintain good performance
    auto cw = 96 / 2 + 1;
    auto ch = 54 / 2;
    GravityEngine_Core ge_inst = GravityEngine_Core("Game", "com.example.game", "1.0", std::max(cw, 37), std::max(ch, 28), fps, 1920/2, 1080/2, "./GameFont.ttf", 2);

    ge_inst.debug_mode = true; // Show debug overlay
    ge_inst.debug_complex = false; // Show all information
    geptr = &ge_inst; // Set the pointer to the console engine class

    // Start game loop
    ge_inst.Start(&GameInit, &PreGameLoop, &PostGameLoop, &ExitGameLoop);

    // Cleanup all dynamically allocated data
    delete[] songgrid;
    // Free all lists
    for (auto o : chainlist)
        delete o;
    for (auto o : phraselist)
        delete o;
    for (auto o : instrumentlist)
        delete o;
    for (auto o : tablelist)
        delete o;
    delete global_settings;
    running = false;

    // Report success to host
    return 0;
}
