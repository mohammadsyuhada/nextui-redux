#ifndef __MODULE_PLAYER_H__
#define __MODULE_PLAYER_H__

#include <SDL2/SDL.h>
#include <stdbool.h>
#include "module_common.h"
#include "playlist.h"
#include "resume.h"

// Run the local files player module
// Handles: File browser, music playback, playlist, shuffle/repeat
ModuleExitReason PlayerModule_run(SDL_Surface* screen);

// Run the player directly with a pre-built playlist (used by PlaylistModule)
// Enters playing state immediately, returns when user presses B or all tracks end.
ModuleExitReason PlayerModule_runWithPlaylist(SDL_Surface* screen,
											  PlaylistTrack* tracks,
											  int track_count,
											  int start_index);

// Run player with resume state (restores folder/playlist, seeks to position)
ModuleExitReason PlayerModule_runResume(SDL_Surface* screen, const ResumeState* resume);

// Set the M3U playlist path for resume tracking (call before runWithPlaylist)
void PlayerModule_setResumePlaylistPath(const char* m3u_path);

// Check if music player module is active (playing/paused)
bool PlayerModule_isActive(void);

// Play next track (for USB HID button support)
void PlayerModule_nextTrack(void);

// Play previous track (for USB HID button support)
void PlayerModule_prevTrack(void);

// Background tick: handle track advancement and resume saving while in menu
void PlayerModule_backgroundTick(void);

#endif
