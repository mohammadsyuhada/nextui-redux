#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "defines.h"
#include "ui_icons.h"

#define ICON_FOLDER RES_PATH "/icon-folder.png"
#define ICON_AUDIO RES_PATH "/icon-audio.png"
#define ICON_PLAY_ALL RES_PATH "/icon-play-all.png"
#define ICON_MP3 RES_PATH "/icon-mp3.png"
#define ICON_FLAC RES_PATH "/icon-flac.png"
#define ICON_OGG RES_PATH "/icon-ogg.png"
#define ICON_WAV RES_PATH "/icon-wav.png"
#define ICON_M4A RES_PATH "/icon-m4a.png"
#define ICON_AAC RES_PATH "/icon-aac.png"
#define ICON_OPUS RES_PATH "/icon-ops.png"
#define ICON_COMPLETE RES_PATH "/icon-complete.png"
#define ICON_DOWNLOAD RES_PATH "/icon-download.png"
#define ICON_EMPTY RES_PATH "/icon-empty.png"

// Icon storage - original (black) and inverted (white) versions
typedef struct {
	SDL_Surface* folder;
	SDL_Surface* folder_inv;
	SDL_Surface* audio;
	SDL_Surface* audio_inv;
	SDL_Surface* play_all;
	SDL_Surface* play_all_inv;
	SDL_Surface* mp3;
	SDL_Surface* mp3_inv;
	SDL_Surface* flac;
	SDL_Surface* flac_inv;
	SDL_Surface* ogg;
	SDL_Surface* ogg_inv;
	SDL_Surface* wav;
	SDL_Surface* wav_inv;
	SDL_Surface* m4a;
	SDL_Surface* m4a_inv;
	SDL_Surface* aac;
	SDL_Surface* aac_inv;
	SDL_Surface* opus;
	SDL_Surface* opus_inv;
	// Podcast badge icons
	SDL_Surface* complete;
	SDL_Surface* complete_inv;
	SDL_Surface* download;
	SDL_Surface* download_inv;
	SDL_Surface* empty;
	SDL_Surface* empty_inv;
	bool loaded;
} IconSet;

static IconSet icons = {0};

// Invert colors of a surface (black <-> white)
// Creates a new surface with inverted colors, preserving alpha
static SDL_Surface* invert_surface(SDL_Surface* src) {
	if (!src)
		return NULL;

	// Create a new surface with same format
	SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(
		0, src->w, src->h, 32, SDL_PIXELFORMAT_RGBA32);

	if (!dst)
		return NULL;

	// Lock surfaces for direct pixel access
	SDL_LockSurface(src);
	SDL_LockSurface(dst);

	Uint32* src_pixels = (Uint32*)src->pixels;
	Uint32* dst_pixels = (Uint32*)dst->pixels;
	int pixel_count = src->w * src->h;

	for (int i = 0; i < pixel_count; i++) {
		Uint8 r, g, b, a;
		SDL_GetRGBA(src_pixels[i], src->format, &r, &g, &b, &a);

		// Invert RGB, keep alpha
		r = 255 - r;
		g = 255 - g;
		b = 255 - b;

		dst_pixels[i] = SDL_MapRGBA(dst->format, r, g, b, a);
	}

	SDL_UnlockSurface(dst);
	SDL_UnlockSurface(src);

	return dst;
}

// Load an icon and create its inverted version
static void load_icon_pair(const char* path, SDL_Surface** original, SDL_Surface** inverted) {
	*original = IMG_Load(path);
	if (*original) {
		// Convert to RGBA32 for consistent pixel access
		SDL_Surface* converted = SDL_ConvertSurfaceFormat(*original, SDL_PIXELFORMAT_RGBA32, 0);
		if (converted) {
			SDL_FreeSurface(*original);
			*original = converted;
		}
		*inverted = invert_surface(*original);
	} else {
		*inverted = NULL;
	}
}

// Initialize icons
void Icons_init(void) {
	if (icons.loaded)
		return;

	load_icon_pair(ICON_FOLDER, &icons.folder, &icons.folder_inv);
	load_icon_pair(ICON_AUDIO, &icons.audio, &icons.audio_inv);
	load_icon_pair(ICON_PLAY_ALL, &icons.play_all, &icons.play_all_inv);
	load_icon_pair(ICON_MP3, &icons.mp3, &icons.mp3_inv);
	load_icon_pair(ICON_FLAC, &icons.flac, &icons.flac_inv);
	load_icon_pair(ICON_OGG, &icons.ogg, &icons.ogg_inv);
	load_icon_pair(ICON_WAV, &icons.wav, &icons.wav_inv);
	load_icon_pair(ICON_M4A, &icons.m4a, &icons.m4a_inv);
	load_icon_pair(ICON_AAC, &icons.aac, &icons.aac_inv);
	load_icon_pair(ICON_OPUS, &icons.opus, &icons.opus_inv);
	// Podcast badge icons
	load_icon_pair(ICON_COMPLETE, &icons.complete, &icons.complete_inv);
	load_icon_pair(ICON_DOWNLOAD, &icons.download, &icons.download_inv);
	load_icon_pair(ICON_EMPTY, &icons.empty, &icons.empty_inv);

	// Consider loaded if at least folder icon exists
	icons.loaded = (icons.folder != NULL);

	if (!icons.loaded) {
	}
}

// Cleanup icons
void Icons_quit(void) {
	if (icons.folder) {
		SDL_FreeSurface(icons.folder);
		icons.folder = NULL;
	}
	if (icons.folder_inv) {
		SDL_FreeSurface(icons.folder_inv);
		icons.folder_inv = NULL;
	}
	if (icons.audio) {
		SDL_FreeSurface(icons.audio);
		icons.audio = NULL;
	}
	if (icons.audio_inv) {
		SDL_FreeSurface(icons.audio_inv);
		icons.audio_inv = NULL;
	}
	if (icons.play_all) {
		SDL_FreeSurface(icons.play_all);
		icons.play_all = NULL;
	}
	if (icons.play_all_inv) {
		SDL_FreeSurface(icons.play_all_inv);
		icons.play_all_inv = NULL;
	}
	if (icons.mp3) {
		SDL_FreeSurface(icons.mp3);
		icons.mp3 = NULL;
	}
	if (icons.mp3_inv) {
		SDL_FreeSurface(icons.mp3_inv);
		icons.mp3_inv = NULL;
	}
	if (icons.flac) {
		SDL_FreeSurface(icons.flac);
		icons.flac = NULL;
	}
	if (icons.flac_inv) {
		SDL_FreeSurface(icons.flac_inv);
		icons.flac_inv = NULL;
	}
	if (icons.ogg) {
		SDL_FreeSurface(icons.ogg);
		icons.ogg = NULL;
	}
	if (icons.ogg_inv) {
		SDL_FreeSurface(icons.ogg_inv);
		icons.ogg_inv = NULL;
	}
	if (icons.wav) {
		SDL_FreeSurface(icons.wav);
		icons.wav = NULL;
	}
	if (icons.wav_inv) {
		SDL_FreeSurface(icons.wav_inv);
		icons.wav_inv = NULL;
	}
	if (icons.m4a) {
		SDL_FreeSurface(icons.m4a);
		icons.m4a = NULL;
	}
	if (icons.m4a_inv) {
		SDL_FreeSurface(icons.m4a_inv);
		icons.m4a_inv = NULL;
	}
	if (icons.aac) {
		SDL_FreeSurface(icons.aac);
		icons.aac = NULL;
	}
	if (icons.aac_inv) {
		SDL_FreeSurface(icons.aac_inv);
		icons.aac_inv = NULL;
	}
	if (icons.opus) {
		SDL_FreeSurface(icons.opus);
		icons.opus = NULL;
	}
	if (icons.opus_inv) {
		SDL_FreeSurface(icons.opus_inv);
		icons.opus_inv = NULL;
	}
	// Podcast badge icons
	if (icons.complete) {
		SDL_FreeSurface(icons.complete);
		icons.complete = NULL;
	}
	if (icons.complete_inv) {
		SDL_FreeSurface(icons.complete_inv);
		icons.complete_inv = NULL;
	}
	if (icons.download) {
		SDL_FreeSurface(icons.download);
		icons.download = NULL;
	}
	if (icons.download_inv) {
		SDL_FreeSurface(icons.download_inv);
		icons.download_inv = NULL;
	}
	if (icons.empty) {
		SDL_FreeSurface(icons.empty);
		icons.empty = NULL;
	}
	if (icons.empty_inv) {
		SDL_FreeSurface(icons.empty_inv);
		icons.empty_inv = NULL;
	}
	icons.loaded = false;
}

// Check if icons are loaded
bool Icons_isLoaded(void) {
	return icons.loaded;
}

// Get folder icon
SDL_Surface* Icons_getFolder(bool selected) {
	if (!icons.loaded)
		return NULL;
	return selected ? icons.folder : icons.folder_inv;
}

// Get generic audio icon
SDL_Surface* Icons_getAudio(bool selected) {
	if (!icons.loaded)
		return NULL;
	return selected ? icons.audio : icons.audio_inv;
}

// Get play all icon
SDL_Surface* Icons_getPlayAll(bool selected) {
	if (!icons.loaded)
		return NULL;
	return selected ? icons.play_all : icons.play_all_inv;
}

// Get icon for specific audio format
// Falls back to generic audio icon if format-specific icon not available
SDL_Surface* Icons_getForFormat(AudioFormat format, bool selected) {
	if (!icons.loaded)
		return NULL;

	SDL_Surface* icon = NULL;
	SDL_Surface* icon_inv = NULL;

	switch (format) {
	case AUDIO_FORMAT_MP3:
		icon = icons.mp3;
		icon_inv = icons.mp3_inv;
		break;
	case AUDIO_FORMAT_FLAC:
		icon = icons.flac;
		icon_inv = icons.flac_inv;
		break;
	case AUDIO_FORMAT_OGG:
		icon = icons.ogg;
		icon_inv = icons.ogg_inv;
		break;
	case AUDIO_FORMAT_WAV:
		icon = icons.wav;
		icon_inv = icons.wav_inv;
		break;
	case AUDIO_FORMAT_M4A:
		icon = icons.m4a;
		icon_inv = icons.m4a_inv;
		break;
	case AUDIO_FORMAT_AAC:
		icon = icons.aac;
		icon_inv = icons.aac_inv;
		break;
	case AUDIO_FORMAT_OPUS:
		icon = icons.opus;
		icon_inv = icons.opus_inv;
		break;
	default:
		// Fall back to generic audio icon
		icon = icons.audio;
		icon_inv = icons.audio_inv;
		break;
	}

	// If format-specific icon not loaded, fall back to generic
	if (!icon) {
		icon = icons.audio;
		icon_inv = icons.audio_inv;
	}

	return selected ? icon : icon_inv;
}

// Get complete/played badge icon
SDL_Surface* Icons_getComplete(bool selected) {
	if (!icons.loaded)
		return NULL;
	return selected ? icons.complete : icons.complete_inv;
}

// Get download badge icon
SDL_Surface* Icons_getDownload(bool selected) {
	if (!icons.loaded)
		return NULL;
	return selected ? icons.download : icons.download_inv;
}

SDL_Surface* Icons_getEmpty(bool selected) {
	if (!icons.loaded)
		return NULL;
	return selected ? icons.empty : icons.empty_inv;
}
