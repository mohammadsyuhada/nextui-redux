#include "spectrum.h"
#include "player.h"
#include "defines.h"
#include "api.h"
#include "audio/kiss_fftr.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define SPECTRUM_SETTINGS_FILE SHARED_USERDATA_PATH "/spectrum_settings.txt"

#define SMOOTHING_FACTOR 0.7f
#define PEAK_DECAY 0.97f
#define MIN_DB -60.0f
#define MAX_DB 0.0f
#define FREQ_COMPENSATION 1.0f // dB boost per octave for high frequencies
#define FREQ_DISTRIBUTION 0.6f // <1.0 = more bars for high freq, >1.0 = more bars for low freq

static kiss_fftr_cfg fft_cfg = NULL;
static kiss_fft_scalar fft_input[SPECTRUM_FFT_SIZE];
static kiss_fft_cpx fft_output[SPECTRUM_FFT_SIZE / 2 + 1];
static float hann_window[SPECTRUM_FFT_SIZE];
static float prev_bars[SPECTRUM_BARS];
static SpectrumData spectrum_data;
static int16_t sample_buffer[SPECTRUM_FFT_SIZE * 2];

static int bin_ranges[SPECTRUM_BARS + 1];
static float freq_compensation[SPECTRUM_BARS]; // Per-band gain compensation

static int spec_x = 0, spec_y = 0, spec_w = 0, spec_h = 0;
static bool position_set = false;

static SpectrumStyle current_style = SPECTRUM_STYLE_VERTICAL;
static bool spectrum_visible = true;

static const char* style_names[] = {
	"Vertical",
	"White",
	"Rainbow",
	"Magnitude"};

// Save spectrum settings to file
static void save_settings(void) {
	FILE* f = fopen(SPECTRUM_SETTINGS_FILE, "w");
	if (!f)
		return;
	fprintf(f, "%d\n%d\n", (int)current_style, spectrum_visible ? 1 : 0);
	fclose(f);
}

// Load spectrum settings from file
static void load_settings(void) {
	FILE* f = fopen(SPECTRUM_SETTINGS_FILE, "r");
	if (!f)
		return;

	int style = 0, visible = 1;
	if (fscanf(f, "%d\n%d\n", &style, &visible) == 2) {
		if (style >= 0 && style < SPECTRUM_STYLE_COUNT) {
			current_style = (SpectrumStyle)style;
		}
		spectrum_visible = (visible != 0);
	}
	fclose(f);
}

// HSV to RGB conversion (h: 0-360, s: 0-1, v: 0-1)
static void hsv_to_rgb(float h, float s, float v, uint8_t* r, uint8_t* g, uint8_t* b) {
	float c = v * s;
	float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
	float m = v - c;
	float rf, gf, bf;

	if (h < 60) {
		rf = c;
		gf = x;
		bf = 0;
	} else if (h < 120) {
		rf = x;
		gf = c;
		bf = 0;
	} else if (h < 180) {
		rf = 0;
		gf = c;
		bf = x;
	} else if (h < 240) {
		rf = 0;
		gf = x;
		bf = c;
	} else if (h < 300) {
		rf = x;
		gf = 0;
		bf = c;
	} else {
		rf = c;
		gf = 0;
		bf = x;
	}

	*r = (uint8_t)((rf + m) * 255);
	*g = (uint8_t)((gf + m) * 255);
	*b = (uint8_t)((bf + m) * 255);
}

// Get color for a bar based on current style
static void get_bar_color(int bar_index, float magnitude, uint8_t* r, uint8_t* g, uint8_t* b) {
	float t;
	switch (current_style) {
	case SPECTRUM_STYLE_RAINBOW:
		// Rainbow: red -> orange -> yellow -> green -> cyan -> blue -> purple
		t = (float)bar_index / (SPECTRUM_BARS - 1);
		hsv_to_rgb(t * 270.0f, 1.0f, 1.0f, r, g, b); // 0-270 hue range
		break;

	case SPECTRUM_STYLE_MAGNITUDE:
		// VU meter style: green (low) -> yellow -> red (high)
		if (magnitude < 0.5f) {
			// Green to yellow
			*r = (uint8_t)(magnitude * 2 * 255);
			*g = 255;
			*b = 0;
		} else {
			// Yellow to red
			*r = 255;
			*g = (uint8_t)((1.0f - (magnitude - 0.5f) * 2) * 255);
			*b = 0;
		}
		break;

	case SPECTRUM_STYLE_VERTICAL:
	case SPECTRUM_STYLE_WHITE:
	default:
		*r = 255;
		*g = 255;
		*b = 255;
		break;
	}
}

static void init_hann_window(void) {
	for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
		hann_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (SPECTRUM_FFT_SIZE - 1)));
	}
}

static void init_bin_ranges(void) {
	float min_freq = 80.0f;
	float max_freq = 16000.0f;
	float sample_rate = 48000.0f;
	float bin_resolution = sample_rate / SPECTRUM_FFT_SIZE;

	int min_bin = (int)(min_freq / bin_resolution);
	int max_bin = (int)(max_freq / bin_resolution);
	if (max_bin > SPECTRUM_FFT_SIZE / 2)
		max_bin = SPECTRUM_FFT_SIZE / 2;

	for (int i = 0; i <= SPECTRUM_BARS; i++) {
		float t = (float)i / SPECTRUM_BARS;
		// Apply distribution curve: <1.0 compresses low freq, expands high freq
		t = powf(t, FREQ_DISTRIBUTION);
		float freq = min_freq * powf(max_freq / min_freq, t);
		int bin = (int)(freq / bin_resolution);
		if (bin < min_bin)
			bin = min_bin;
		if (bin > max_bin)
			bin = max_bin;
		bin_ranges[i] = bin;
	}

	// Initialize frequency compensation (boost higher frequencies)
	// This compensates for the natural 1/f energy distribution in audio
	for (int i = 0; i < SPECTRUM_BARS; i++) {
		float t = (float)i / (SPECTRUM_BARS - 1);
		// Apply progressive boost: 0 dB at lowest, FREQ_COMPENSATION*octaves at highest
		float octaves = log2f(max_freq / min_freq);
		freq_compensation[i] = t * octaves * FREQ_COMPENSATION;
	}
}

void Spectrum_init(void) {
	if (fft_cfg)
		return; // Already initialized

	fft_cfg = kiss_fftr_alloc(SPECTRUM_FFT_SIZE, 0, NULL, NULL);
	init_hann_window();
	init_bin_ranges();
	memset(prev_bars, 0, sizeof(prev_bars));
	memset(&spectrum_data, 0, sizeof(spectrum_data));
	load_settings();
}

void Spectrum_quit(void) {
	if (fft_cfg) {
		kiss_fftr_free(fft_cfg);
		fft_cfg = NULL;
	}
	// Clear the GPU layer
	PLAT_clearLayers(LAYER_SPECTRUM);
	PLAT_GPU_Flip();
}

void Spectrum_update(void) {
	if (!fft_cfg)
		return;

	if (Player_getState() != PLAYER_STATE_PLAYING) {
		for (int i = 0; i < SPECTRUM_BARS; i++) {
			prev_bars[i] *= 0.9f;
			spectrum_data.bars[i] = prev_bars[i];
			spectrum_data.peaks[i] *= PEAK_DECAY;
		}
		spectrum_data.valid = true;
		return;
	}

	int samples = Player_getVisBuffer(sample_buffer, SPECTRUM_FFT_SIZE * 2);
	if (samples < SPECTRUM_FFT_SIZE) {
		spectrum_data.valid = false;
		return;
	}

	for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
		float left = sample_buffer[i * 2];
		float right = sample_buffer[i * 2 + 1];
		float mono = (left + right) * 0.5f;
		fft_input[i] = (mono / 32768.0f) * hann_window[i];
	}

	kiss_fftr(fft_cfg, fft_input, fft_output);

	for (int i = 0; i < SPECTRUM_BARS; i++) {
		int start_bin = bin_ranges[i];
		int end_bin = bin_ranges[i + 1];
		if (end_bin <= start_bin)
			end_bin = start_bin + 1;

		float sum = 0.0f;
		int count = 0;
		for (int j = start_bin; j < end_bin && j < SPECTRUM_FFT_SIZE / 2 + 1; j++) {
			float re = fft_output[j].r;
			float im = fft_output[j].i;
			float mag = sqrtf(re * re + im * im);
			sum += mag;
			count++;
		}

		float avg_mag = (count > 0) ? sum / count : 0.0f;

		float db = 20.0f * log10f(avg_mag + 1e-10f);
		// Apply frequency compensation to boost higher frequencies
		db += freq_compensation[i];
		float normalized = (db - MIN_DB) / (MAX_DB - MIN_DB);
		if (normalized < 0.0f)
			normalized = 0.0f;
		if (normalized > 1.0f)
			normalized = 1.0f;

		if (normalized > prev_bars[i]) {
			prev_bars[i] = normalized;
		} else {
			prev_bars[i] = prev_bars[i] * SMOOTHING_FACTOR + normalized * (1.0f - SMOOTHING_FACTOR);
		}

		spectrum_data.bars[i] = prev_bars[i];

		if (prev_bars[i] > spectrum_data.peaks[i]) {
			spectrum_data.peaks[i] = prev_bars[i];
		} else {
			spectrum_data.peaks[i] *= PEAK_DECAY;
		}
	}

	spectrum_data.valid = true;
}

const SpectrumData* Spectrum_getData(void) {
	return &spectrum_data;
}

void Spectrum_setPosition(int x, int y, int w, int h) {
	spec_x = x;
	spec_y = y;
	spec_w = w;
	spec_h = h;
	position_set = true;
}

bool Spectrum_needsRefresh(void) {
	return position_set && spectrum_visible && (Player_getState() == PLAYER_STATE_PLAYING);
}

void Spectrum_cycleStyle(void) {
	current_style = (current_style + 1) % SPECTRUM_STYLE_COUNT;
	save_settings(); // Persist preference
}

void Spectrum_toggleVisibility(void) {
	spectrum_visible = !spectrum_visible;
	if (!spectrum_visible) {
		// Clear the spectrum layer when hiding
		PLAT_clearLayers(LAYER_SPECTRUM);
		PLAT_GPU_Flip();
	}
	save_settings(); // Persist preference
}

void Spectrum_cycleNext(void) {
	if (!spectrum_visible) {
		// Off -> first style
		spectrum_visible = true;
		current_style = SPECTRUM_STYLE_VERTICAL;
	} else {
		int next = (int)current_style + 1;
		if (next >= SPECTRUM_STYLE_COUNT) {
			// Last style -> off
			spectrum_visible = false;
			PLAT_clearLayers(LAYER_SPECTRUM);
			PLAT_GPU_Flip();
		} else {
			current_style = (SpectrumStyle)next;
		}
	}
	save_settings();
}

bool Spectrum_isVisible(void) {
	return spectrum_visible;
}

SpectrumStyle Spectrum_getStyle(void) {
	return current_style;
}

const char* Spectrum_getStyleName(void) {
	return style_names[current_style];
}

// Draw a vertical gradient bar (for SPECTRUM_STYLE_VERTICAL)
// Uses system theme colors: primary accent (top) to secondary accent (bottom)
static void draw_vertical_gradient_bar(SDL_Surface* surface, int x, int y, int w, int h, int bar_index) {
	if (h <= 0 || w <= 0)
		return;

	// Get raw theme colors (format: 0xRRGGBB)
	// THEME_COLOR1 = main, THEME_COLOR2 = primary accent, THEME_COLOR3 = secondary accent
	uint32_t color1 = CFG_getColor(2); // Primary accent (top)
	uint32_t color2 = CFG_getColor(3); // Secondary accent (bottom)

	uint8_t top_r = (color1 >> 16) & 0xFF;
	uint8_t top_g = (color1 >> 8) & 0xFF;
	uint8_t top_b = color1 & 0xFF;

	uint8_t bot_r = (color2 >> 16) & 0xFF;
	uint8_t bot_g = (color2 >> 8) & 0xFF;
	uint8_t bot_b = color2 & 0xFF;

	for (int row = 0; row < h; row++) {
		// t goes from 0.0 (top) to 1.0 (bottom)
		float t = (h > 1) ? (float)row / (float)(h - 1) : 0.0f;

		// Interpolate between top color and bottom color
		uint8_t r = (uint8_t)(top_r + t * (bot_r - top_r));
		uint8_t g = (uint8_t)(top_g + t * (bot_g - top_g));
		uint8_t b = (uint8_t)(top_b + t * (bot_b - top_b));

		// Use SDL_MapRGBA for correct pixel format
		uint32_t color = SDL_MapRGBA(surface->format, r, g, b, 255);

		SDL_Rect row_rect = {x, y + row, w, 1};
		SDL_FillRect(surface, &row_rect, color);
	}
}

void Spectrum_renderGPU(void) {
	if (!position_set || !spectrum_visible)
		return;

	Spectrum_update();
	if (!spectrum_data.valid)
		return;

	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0,
														  spec_w, spec_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surface)
		return;

	SDL_FillRect(surface, NULL, 0);

	int total_bars = SPECTRUM_BARS;
	float bar_width_f = (float)spec_w / total_bars;
	int bar_gap = 1;
	int bar_draw_w = (int)bar_width_f - bar_gap;
	if (bar_draw_w < 1)
		bar_draw_w = 1;

	for (int i = 0; i < total_bars; i++) {
		float magnitude = spectrum_data.bars[i];
		int bar_h = (int)(magnitude * spec_h * 0.9f);
		if (bar_h < 2)
			bar_h = 2;

		int bar_x_pos = (int)(i * bar_width_f);
		int bar_y_pos = spec_h - bar_h;

		if (current_style == SPECTRUM_STYLE_VERTICAL) {
			// Vertical gradient - draw pixel by pixel
			draw_vertical_gradient_bar(surface, bar_x_pos, bar_y_pos, bar_draw_w, bar_h, i);
		} else {
			// Solid color styles
			uint8_t r, g, b;
			get_bar_color(i, magnitude, &r, &g, &b);
			uint32_t color = SDL_MapRGBA(surface->format, r, g, b, 255);

			SDL_Rect bar_rect = {bar_x_pos, bar_y_pos, bar_draw_w, bar_h};
			SDL_FillRect(surface, &bar_rect, color);
		}

		// Draw peak indicator
		if (spectrum_data.peaks[i] > magnitude + 0.02f) {
			int peak_y = spec_h - (int)(spectrum_data.peaks[i] * spec_h * 0.9f);
			uint8_t r, g, b;
			get_bar_color(i, spectrum_data.peaks[i], &r, &g, &b);
			uint32_t peak_color = SDL_MapRGBA(surface->format, r, g, b, 255);
			SDL_Rect peak_rect = {bar_x_pos, peak_y, bar_draw_w, 2};
			SDL_FillRect(surface, &peak_rect, peak_color);
		}
	}

	PLAT_clearLayers(LAYER_SPECTRUM);
	PLAT_drawOnLayer(surface, spec_x, spec_y, spec_w, spec_h, 1.0f, false, LAYER_SPECTRUM);
	SDL_FreeSurface(surface);

	PLAT_GPU_Flip();
}
