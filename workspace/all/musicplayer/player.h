#ifndef __PLAYER_H__
#define __PLAYER_H__

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <SDL2/SDL.h>

// Audio format types
typedef enum {
	AUDIO_FORMAT_UNKNOWN = 0,
	AUDIO_FORMAT_WAV,
	AUDIO_FORMAT_MP3,
	AUDIO_FORMAT_OGG,
	AUDIO_FORMAT_FLAC,
	AUDIO_FORMAT_MOD,
	AUDIO_FORMAT_M4A,
	AUDIO_FORMAT_AAC,
	AUDIO_FORMAT_OPUS
} AudioFormat;

// Player states
typedef enum {
	PLAYER_STATE_STOPPED = 0,
	PLAYER_STATE_PLAYING,
	PLAYER_STATE_PAUSED
} PlayerState;

// Track metadata
typedef struct {
	char title[256];
	char artist[256];
	char album[256];
	int duration_ms; // Total duration in milliseconds
	int sample_rate;
	int channels;
	int bitrate;
} TrackInfo;

// Waveform overview data
#define WAVEFORM_BARS 128 // Number of bars in waveform display
typedef struct {
	float bars[WAVEFORM_BARS]; // Amplitude values 0.0-1.0 for each bar
	int bar_count;
	bool valid;
} WaveformData;

// Streaming decoder state (holds any decoder type)
typedef struct {
	AudioFormat format;
	void* decoder; // drmp3*, drwav*, drflac*, or stb_vorbis*
	int source_sample_rate;
	int source_channels;
	int64_t total_frames;
	int64_t current_frame;
} StreamDecoder;

// Circular buffer for streaming playback
#define STREAM_BUFFER_FRAMES (44100 * 3) // ~3 seconds at 44.1kHz stereo (~500KB)
typedef struct {
	int16_t* buffer;  // Stereo interleaved samples
	size_t capacity;  // Total frames capacity
	size_t write_pos; // Write position (frames)
	size_t read_pos;  // Read position (frames)
	size_t available; // Frames available to read
	pthread_mutex_t mutex;
} CircularBuffer;

// Player context
typedef struct {
	// State
	PlayerState state;
	AudioFormat format;

	// Current track
	char current_file[512];
	TrackInfo track_info;

	// Album art
	SDL_Surface* album_art; // Cached album art surface (NULL if none)

	// Playback
	int position_ms; // Current position in milliseconds
	float volume;	 // 0.0 to 1.0
	bool repeat;	 // Loop current track

	// Audio buffer for visualization
	int16_t vis_buffer[2048]; // Stereo samples for FFT
	int vis_buffer_pos;
	pthread_mutex_t vis_mutex;

	// SDL Audio
	int audio_device;
	bool audio_initialized;

	// Streaming playback
	StreamDecoder stream_decoder;
	CircularBuffer stream_buffer;
	void* resampler; // SRC_STATE* for libsamplerate
	pthread_t stream_thread;
	bool stream_running;
	bool stream_seeking;	   // Flag when seek is requested
	int64_t seek_target_frame; // Target frame for seeking
	bool use_streaming;		   // True if using streaming mode
	bool stream_eof;		   // True when decoder has reached end of file

	// Resampler leftover buffer (for unconsumed input frames)
	int16_t* resample_leftover;
	size_t resample_leftover_count;
	size_t resample_leftover_capacity;

	// Threading
	pthread_mutex_t mutex;
} PlayerContext;

// Initialize the player
int Player_init(void);

// Cleanup the player
void Player_quit(void);

// Load a file (does not start playing)
int Player_load(const char* filepath);

// Start/resume playback
int Player_play(void);

// Pause playback
void Player_pause(void);

// Stop playback and unload
void Player_stop(void);

// Toggle play/pause
void Player_togglePause(void);

// Seek to position (in milliseconds)
void Player_seek(int position_ms);

// Check if a seek operation is still in progress (for resume flow)
bool Player_resume(void);

// Set volume (0.0 to 1.0)
void Player_setVolume(float volume);

// Get current volume
float Player_getVolume(void);

// Get current state
PlayerState Player_getState(void);

// Get current position in milliseconds
int Player_getPosition(void);

// Get track duration in milliseconds
int Player_getDuration(void);

// Get track info
const TrackInfo* Player_getTrackInfo(void);

// Get current file path
const char* Player_getCurrentFile(void);

// Get visualization buffer (for spectrum analyzer)
// Returns number of samples copied
int Player_getVisBuffer(int16_t* buffer, int max_samples);

// Get waveform overview data (for static waveform progress display)
const WaveformData* Player_getWaveform(void);

// Get album art surface (NULL if no album art available)
SDL_Surface* Player_getAlbumArt(void);

// Check if a file format is supported
AudioFormat Player_detectFormat(const char* filepath);

// Update player (call this in main loop)
void Player_update(void);

// Resume/pause audio device (used by radio module)
void Player_resumeAudio(void);
void Player_pauseAudio(void);

// Reset audio device to default 48000 Hz sample rate (for radio use)
void Player_resetSampleRate(void);

// Set audio device to specific sample rate
void Player_setSampleRate(int sample_rate);

// Check if Bluetooth audio is currently active
bool Player_isBluetoothActive(void);

// Check if USB DAC audio is currently active
bool Player_isUSBDACActive(void);

// USB HID input events (for USB earphone buttons)
typedef enum {
	USB_HID_EVENT_NONE = 0,
	USB_HID_EVENT_VOLUME_UP,
	USB_HID_EVENT_VOLUME_DOWN,
	USB_HID_EVENT_NEXT_TRACK,
	USB_HID_EVENT_PLAY_PAUSE,
	USB_HID_EVENT_PREV_TRACK
} USBHIDEvent;

// Initialize USB HID input monitoring (call after USB DAC is detected)
void Player_initUSBHID(void);

// Poll for USB HID events (call in main loop)
USBHIDEvent Player_pollUSBHID(void);

// Cleanup USB HID monitoring
void Player_quitUSBHID(void);

#endif
