#pragma once
#include <string>
#include <vector>
#include "GravityEngineSDL.h"

// Find string f in s
bool str_contains(const std::string& s, const std::string& f);

// Remove char from string
std::string str_remove(std::string s, char c);

// Get note freq
double NoteFreq(double n);

// Get freq note
double FreqNote(double f);

// Get sample ratio
// int base_pitch : Base pitch of the sample tuned to a piano. For example, C4 == 39
// int new_pitch : New pitch of the sample tuned to a piano. For example, C#4 == 40
// int detune : Cents to detune the pitch by
double GetSampleRatioChange(int base_pitch, int new_pitch, double detune);

// Insert at arbitrary location
template <typename T> void
InsertAt(std::vector<T*>* vec, int index, T* ptr);

// Get at arbitrary location
template <typename T> T*
GetAt(std::vector<T*>* vec, int index);

// Get next empty index
template <typename T> int
GetNextEmpty(std::vector<T*>* vec);

// Apply transposition from a chain
void ApplyChainTransposition(int channel_index, int* f);

// Get FX from Phrase and apply
// channelnumber : The particular channel to play the step on
// f : frequency to play
// fx1 : FX1 to apply
// fx1_p : FX1 parameter to apply
// fx2 : FX2 to apply
// fx2_p : FX2 parameter to apply
// fx3 : FX3 to apply
// fx3_p : FX3 parameter to apply
void GetFX(int channel_index, ChannelType inst_type, int f, int fx1, int fx1_p, int fx2, int fx2_p, int fx3, int fx3_p);

// Play step phrase
// channelnumber : The particular channel to play the step on
// playing_phrase : Phrase to play
// step_ptr : Phrase progress index
void PlayStepPhrase(int channel_index, int playing_phrase, int step_ptr);

// Play step chain
// channelnumber : The particular channel to play the step on
// playing_chain : Chain to play
// phrase_ptr : Chain progress index
// step_ptr : Phrase progress index
void PlayStepChain(int channel_index, int playing_chain, int phrase_ptr, int step_ptr);

// Play step song
// channelnumber : The particular channel to play the step on
// chain_ptr : Song progress index
// phrase_ptr : Chain progress index
// step_ptr : Phrase progress index
void PlayStepSong(int channel_index, int chain_ptr, int phrase_ptr, int step_ptr);

// Update playing step - Pitch
// channelnumber : The particular channel to play the step on
// playing_phrase : Phrase to play
// step_ptr : Phrase progress index
// tt : Table transposition to apply
// freq : Frequency to update with the step's pitch
void UpdateStepPitch(int channel_index, int playing_phrase, int step_ptr, int tt, double* new_freq);

// Apply playing chain transposition - Implementation
// channel_index : The channel to get the transposition from
// f : The original frequency
void ApplyChainTransposition(int channel_index, int* f);

// Apply playing table transposition - Implementation
// channel_index : The channel to get the transposition from
// f : The original frequency
// tt : The table transposition to apply
void ApplyTableTransposition(int channel_index, int* f, int tt);

// Deep Copy Phrase
// phrase_index : The ID of the phrase
int DeepCopyPhrase(int phrase_index);

// Shallow Copy Chain
// chain_index : The ID of the chain
int ShallowCopyChain(int chain_index);

// Deep Copy Chain
// chain_index : The ID of the chain
int DeepCopyChain(int chain_index);

// Beats-per-minute to Tick length in nanoseconds
// b : bpm
double BpmToTicklength(int b);

// Execute tick
// ------------
// This is the tick loop; All song execution should go inside this function
// Table>Phrase>Chain>Song <- Per channel
void DoTick();

// Start the sequence thread
void StartSequenceThread();

// Stop the sequence thread
void StopSequenceThread();

// Convert i to hex string
// int i : Number to convert
std::string IntToHexString(int i);

// Draw Song Editor UI
// off_x : UI offset on the x axis
// off_y : UI offset on the y axis
void DrawSongUI(int off_x, int off_y);

// Draw Chain Editor UI
// off_y : UI offset on the y axis
void DrawChainUI(int off_y);

// Converts an integer to a note string
// int note : Piano key number
std::string IntToNoteString(int n);

// Draw Phrase Editor UI
// off_y : UI offset on the y axis
void DrawPhraseUI(int off_y);

// Draw Instrument Editor UI
void DrawInstrumentUI();

// Draw Table Editor UI
// off_y : UI offset on the y axis
void DrawTableUI(int off_y);

// Draw Wave Editor UI
void DrawWaveUI();

// Draw the map for where you are in the UI
void DrawMap();

// Handle tick hitting - This should be called from a separate thread
void TrackTicks();

// Handle repeating movements
void HandleMovementRepeaters();

// Stop a channel
void StopChannel(int channel);

// Stop all audio playback
void StopAllChannels();

// Handle movement
void EditorControl();