#include "background.h"
#include "player.h"
#include "radio.h"
#include "podcast.h"
#include "resume.h"
#include "module_player.h"
#include "module_podcast.h"
#include "module_common.h"

static BackgroundPlayerType active_bg = BG_NONE;

void Background_setActive(BackgroundPlayerType type) {
	active_bg = type;
}

BackgroundPlayerType Background_getActive(void) {
	return active_bg;
}

void Background_stopAll(void) {
	switch (active_bg) {
	case BG_MUSIC:
		// Save resume position before stopping
		if (Player_getState() == PLAYER_STATE_PLAYING || Player_getState() == PLAYER_STATE_PAUSED) {
			Resume_updatePosition(Player_getPosition());
		}
		Player_stop();
		break;
	case BG_RADIO:
		Radio_stop();
		break;
	case BG_PODCAST:
		// Podcast_stop() saves progress in memory; flush to disk
		Podcast_stop();
		Podcast_flushProgress();
		break;
	case BG_NONE:
		break;
	}
	if (active_bg != BG_NONE) {
		ModuleCommon_setAutosleepDisabled(false);
	}
	active_bg = BG_NONE;
}

bool Background_isPlaying(void) {
	switch (active_bg) {
	case BG_MUSIC:
		return PlayerModule_isActive();
	case BG_RADIO:
		return Radio_isActive();
	case BG_PODCAST:
		return Podcast_isActive();
	case BG_NONE:
		break;
	}
	return false;
}

void Background_tick(void) {
	switch (active_bg) {
	case BG_MUSIC:
		PlayerModule_backgroundTick();
		break;
	case BG_RADIO:
		// Radio streams are self-sustaining, just check if still active
		if (!Radio_isActive()) {
			ModuleCommon_setAutosleepDisabled(false);
			active_bg = BG_NONE;
		}
		break;
	case BG_PODCAST:
		PodcastModule_backgroundTick();
		break;
	case BG_NONE:
		break;
	}
}
