#ifndef __UI_PLAYLIST_H__
#define __UI_PLAYLIST_H__

#include <SDL2/SDL.h>
#include "playlist_m3u.h"
#include "playlist.h"

// Render the playlist list screen
void render_playlist_list(SDL_Surface* screen, int show_setting,
						  PlaylistInfo* playlists, int count,
						  int selected, int scroll);

// Render the playlist detail screen (tracks in a playlist)
void render_playlist_detail(SDL_Surface* screen, int show_setting,
							const char* playlist_name,
							PlaylistTrack* tracks, int count,
							int selected, int scroll);

// Check if playlist list has active scrolling
bool playlist_list_needs_scroll_refresh(void);

// Check if playlist list scroll needs render (delay phase)
bool playlist_list_scroll_needs_render(void);

// Animate playlist list scroll (GPU mode)
void playlist_list_animate_scroll(void);

#endif
