#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "utils.h"

// ============================================
// Rendering helpers
// ============================================

// ASSET_BUTTON/ASSET_HOLE sprite is 20x20 unscaled
#define BUTTON_SPRITE_SIZE 20
// Offset to center button sprite within a PILL_SIZE pill
#define BUTTON_INSET ((PILL_SIZE - BUTTON_SPRITE_SIZE) / 2)

static void fillCircle(SDL_Surface* dst, int cx, int cy, int radius, uint32_t color) {
	for (int dy = -radius; dy <= radius; dy++) {
		int dx = (int)sqrt((double)radius * radius - (double)dy * dy);
		SDL_FillRect(dst, &(SDL_Rect){cx - dx, cy + dy, dx * 2, 1}, color);
	}
}

static int getButtonWidth(char* label) {
	int w = 0;

	if (strlen(label) <= 2) {
		w = SCALE1(BUTTON_SPRITE_SIZE);
	} else {
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.tiny, label, COLOR_BUTTON_TEXT);
		w = text->w + SCALE1(BUTTON_INSET) * 2;
		SDL_FreeSurface(text);
	}
	return w;
}

static void blitButton(char* label, SDL_Surface* dst, int pressed, int x, int y, int w) {
	SDL_Rect point = {x, y};
	SDL_Surface* text;

	int len = strlen(label);
	if (len <= 2) {
		text = TTF_RenderUTF8_Blended(len == 2 ? font.small : font.medium, label, COLOR_BUTTON_TEXT);
		GFX_blitAsset(pressed ? ASSET_BUTTON : ASSET_HOLE, NULL, dst, &point);
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){point.x + (SCALE1(BUTTON_SPRITE_SIZE) - text->w) / 2, point.y + (SCALE1(BUTTON_SPRITE_SIZE) - text->h) / 2});
	} else {
		text = TTF_RenderUTF8_Blended(font.tiny, label, COLOR_BUTTON_TEXT);
		w = w ? w : text->w + SCALE1(BUTTON_INSET) * 2;
		GFX_blitPill(pressed ? ASSET_BUTTON : ASSET_HOLE, dst, &(SDL_Rect){point.x, point.y, w, SCALE1(BUTTON_SPRITE_SIZE)});
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){point.x + (w - text->w) / 2, point.y + (SCALE1(BUTTON_SPRITE_SIZE) - text->h) / 2, text->w, text->h});
	}

	SDL_FreeSurface(text);
}

// ============================================
// Joystick Calibration (serial port raw ADC)
// ============================================

#define JOYPAD_LEFT_SERIAL "/dev/ttyAS5"
#define JOYPAD_RIGHT_SERIAL "/dev/ttyAS7"
#define JOYPAD_LEFT_CONFIG "/mnt/UDISK/joypad.config"
#define JOYPAD_RIGHT_CONFIG "/mnt/UDISK/joypad_right.config"

#define CAL_PKT_SIZE 19
#define CAL_PKT_START 0xFF
#define CAL_PKT_END 0xFE
#define CAL_LEFT_X_OFF 6
#define CAL_LEFT_Y_OFF 8
#define CAL_RIGHT_X_OFF 10
#define CAL_RIGHT_Y_OFF 12

#define CAL_RANGE_SECS 5
#define CAL_CENTER_SECS 2

typedef struct {
	int x_min, x_max, y_min, y_max, x_zero, y_zero;
	float deadzone;
} JoypadCal;

static int cal_open_serial(const char* path) {
	int fd = open(path, O_RDONLY | O_NOCTTY);
	if (fd < 0)
		return -1;

	struct termios tio;
	memset(&tio, 0, sizeof(tio));
	tio.c_cflag = B19200 | CS8 | CLOCAL | CREAD;
	tio.c_iflag = 0;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 1;
	tcsetattr(fd, TCSANOW, &tio);
	tcflush(fd, TCIFLUSH);

	return fd;
}

static int cal_read_pkt(int fd, int x_off, int y_off, int* x, int* y) {
	unsigned char buf[CAL_PKT_SIZE];
	unsigned char b;

	for (int i = 0; i < CAL_PKT_SIZE * 4; i++) {
		if (read(fd, &b, 1) != 1)
			return -1;
		if (b == CAL_PKT_START) {
			buf[0] = b;
			int remaining = CAL_PKT_SIZE - 1;
			int off = 1;
			while (remaining > 0) {
				int n = read(fd, buf + off, remaining);
				if (n <= 0)
					return -1;
				off += n;
				remaining -= n;
			}
			if (buf[CAL_PKT_SIZE - 1] == CAL_PKT_END) {
				*x = buf[x_off] | (buf[x_off + 1] << 8);
				*y = buf[y_off] | (buf[y_off + 1] << 8);
				return 0;
			}
		}
	}
	return -1;
}

static int cal_read_config(const char* path, JoypadCal* cal) {
	FILE* f = fopen(path, "r");
	if (!f)
		return -1;

	char line[64];
	int found = 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "x_min=%d", &cal->x_min) == 1)
			found++;
		else if (sscanf(line, "x_max=%d", &cal->x_max) == 1)
			found++;
		else if (sscanf(line, "y_min=%d", &cal->y_min) == 1)
			found++;
		else if (sscanf(line, "y_max=%d", &cal->y_max) == 1)
			found++;
		else if (sscanf(line, "x_zero=%d", &cal->x_zero) == 1)
			found++;
		else if (sscanf(line, "y_zero=%d", &cal->y_zero) == 1)
			found++;
		else if (sscanf(line, "deadzone=%f", &cal->deadzone) == 1)
			found++;
	}
	fclose(f);
	return (found >= 7) ? 0 : -1;
}

static int cal_write_config(const char* path, const JoypadCal* cal) {
	FILE* f = fopen(path, "w");
	if (!f)
		return -1;
	fprintf(f, "x_min=%d\nx_max=%d\ny_min=%d\ny_max=%d\nx_zero=%d\ny_zero=%d\ndeadzone=%.2f\n",
			cal->x_min, cal->x_max, cal->y_min, cal->y_max,
			cal->x_zero, cal->y_zero, cal->deadzone);
	fclose(f);
	return 0;
}

static void cal_render_msg(SDL_Surface* screen, const char* title, const char* subtitle, int countdown) {
	GFX_clear(screen);
	UI_renderMenuBar(screen, "Joystick Calibration");

	int cy = FIXED_HEIGHT / 2 - SCALE1(FONT_LARGE + PADDING);

	SDL_Surface* t = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
	SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){(FIXED_WIDTH - t->w) / 2, cy});
	SDL_FreeSurface(t);
	cy += SCALE1(FONT_LARGE + PADDING);

	if (subtitle[0]) {
		SDL_Surface* s = TTF_RenderUTF8_Blended(font.small, subtitle, COLOR_GRAY);
		SDL_BlitSurface(s, NULL, screen, &(SDL_Rect){(FIXED_WIDTH - s->w) / 2, cy});
		SDL_FreeSurface(s);
		cy += SCALE1(FONT_SMALL + PADDING);
	}

	if (countdown > 0) {
		char buf[8];
		snprintf(buf, sizeof(buf), "%d", countdown);
		SDL_Surface* c = TTF_RenderUTF8_Blended(font.xlarge, buf, COLOR_WHITE);
		SDL_BlitSurface(c, NULL, screen, &(SDL_Rect){(FIXED_WIDTH - c->w) / 2, cy + SCALE1(PADDING)});
		SDL_FreeSurface(c);
	}

	GFX_flip(screen);
}

// Read serial data for duration_ms, tracking min/max range
static void cal_track_range(int fd, int x_off, int y_off, JoypadCal* cal,
							SDL_Surface* screen, const char* title, int secs) {
	cal->x_min = cal->y_min = 65535;
	cal->x_max = cal->y_max = 0;

	for (int i = secs; i > 0; i--) {
		cal_render_msg(screen, title, "Rotate stick in full circles", i);
		uint32_t start = SDL_GetTicks();
		while (SDL_GetTicks() - start < 1000) {
			int x, y;
			if (cal_read_pkt(fd, x_off, y_off, &x, &y) == 0) {
				if (x < cal->x_min)
					cal->x_min = x;
				if (x > cal->x_max)
					cal->x_max = x;
				if (y < cal->y_min)
					cal->y_min = y;
				if (y > cal->y_max)
					cal->y_max = y;
			}
		}
	}
}

// Read serial data for duration, averaging for center position
static void cal_read_center(int fd, int x_off, int y_off, JoypadCal* cal,
							SDL_Surface* screen, const char* title, int secs) {
	long x_sum = 0, y_sum = 0;
	int count = 0;

	for (int i = secs; i > 0; i--) {
		cal_render_msg(screen, title, "Leave stick at center", i);
		uint32_t start = SDL_GetTicks();
		while (SDL_GetTicks() - start < 1000) {
			int x, y;
			if (cal_read_pkt(fd, x_off, y_off, &x, &y) == 0) {
				x_sum += x;
				y_sum += y;
				count++;
			}
		}
	}

	if (count > 0) {
		cal->x_zero = (int)(x_sum / count);
		cal->y_zero = (int)(y_sum / count);
	}
}

// Blocking calibration routine: rotate stick → stop at center, left then right
static void cal_run(SDL_Surface* screen) {
	JoypadCal left = {0}, right = {0};

	// Preserve existing deadzone values
	JoypadCal existing;
	left.deadzone = (cal_read_config(JOYPAD_LEFT_CONFIG, &existing) == 0) ? existing.deadzone : 0.10f;
	right.deadzone = (cal_read_config(JOYPAD_RIGHT_CONFIG, &existing) == 0) ? existing.deadzone : 0.10f;

	// Brief countdown before stopping inputd
	for (int i = 2; i > 0; i--) {
		cal_render_msg(screen, "Starting Calibration", "Get ready...", i);
		SDL_Delay(1000);
	}

	// Stop inputd and open serial ports
	cal_render_msg(screen, "Starting Calibration", "Opening joystick ports...", 0);
	system("killall trimui_inputd");
	usleep(200000);

	int fd_left = cal_open_serial(JOYPAD_LEFT_SERIAL);
	int fd_right = cal_open_serial(JOYPAD_RIGHT_SERIAL);

	if (fd_left < 0 || fd_right < 0) {
		cal_render_msg(screen, "Error", "Failed to open joystick serial ports", 0);
		SDL_Delay(2000);
		if (fd_left >= 0)
			close(fd_left);
		if (fd_right >= 0)
			close(fd_right);
		system("/usr/trimui/bin/trimui_inputd &");
		usleep(500000);
		return;
	}

	// Left stick: rotate for range
	cal_track_range(fd_left, CAL_LEFT_X_OFF, CAL_LEFT_Y_OFF, &left,
					screen, "Left Stick - Rotate", CAL_RANGE_SECS);

	// Left stick: stop for center
	cal_read_center(fd_left, CAL_LEFT_X_OFF, CAL_LEFT_Y_OFF, &left,
					screen, "Left Stick - Stop", CAL_CENTER_SECS);

	// Right stick: rotate for range
	cal_track_range(fd_right, CAL_RIGHT_X_OFF, CAL_RIGHT_Y_OFF, &right,
					screen, "Right Stick - Rotate", CAL_RANGE_SECS);

	// Right stick: stop for center
	cal_read_center(fd_right, CAL_RIGHT_X_OFF, CAL_RIGHT_Y_OFF, &right,
					screen, "Right Stick - Stop", CAL_CENTER_SECS);

	close(fd_left);
	close(fd_right);

	// Save configs
	cal_render_msg(screen, "Saving...", "", 0);
	cal_write_config(JOYPAD_LEFT_CONFIG, &left);
	cal_write_config(JOYPAD_RIGHT_CONFIG, &right);

	// Restart inputd
	system("/usr/trimui/bin/trimui_inputd &");
	usleep(500000);

	cal_render_msg(screen, "Calibration Complete!", "", 0);
	SDL_Delay(1500);
}

// ============================================
// Main
// ============================================

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	SDL_Surface* screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "Input");

	InitSettings();
	PAD_init();
	PWR_init();

	setup_signal_handlers();
	PWR_disableSleep();
	PWR_disablePowerOff();

	// one-time capability detection
	int has_L2 = (BUTTON_L2 != BUTTON_NA || CODE_L2 != CODE_NA || JOY_L2 != JOY_NA || AXIS_L2 != AXIS_NA);
	int has_R2 = (BUTTON_R2 != BUTTON_NA || CODE_R2 != CODE_NA || JOY_R2 != JOY_NA || AXIS_R2 != AXIS_NA);
	int has_L3 = (BUTTON_L3 != BUTTON_NA || CODE_L3 != CODE_NA || JOY_L3 != JOY_NA);
	int has_R3 = (BUTTON_R3 != BUTTON_NA || CODE_R3 != CODE_NA || JOY_R3 != JOY_NA);

	int has_volume = (BUTTON_PLUS != BUTTON_NA || CODE_PLUS != CODE_NA || JOY_PLUS != JOY_NA);
	int has_power = HAS_POWER_BUTTON;
	int has_menu = HAS_MENU_BUTTON;
	int has_home = HAS_HOME_BUTTON;
	int has_analog = (AXIS_LX != AXIS_NA);
	int has_joystick = (has_analog && HAS_JOYSTICK);

	int oy = SCALE1(PADDING);
	if (!has_L3 && !has_R3)
		oy += SCALE1(PILL_SIZE);

	// load joystick position dot sprite
	SDL_Surface* joy_dot = IMG_Load(RES_PATH "/joystick-dot.png");

	PAD_Axis prev_laxis = {0, 0};
	PAD_Axis prev_raxis = {0, 0};

	bool quit = false;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (!quit && !app_quit) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_anyPressed() || PAD_anyJustReleased())
			dirty = true;
		if (has_analog && (pad.laxis.x != prev_laxis.x || pad.laxis.y != prev_laxis.y ||
						   pad.raxis.x != prev_raxis.x || pad.raxis.y != prev_raxis.y)) {
			dirty = true;
			prev_laxis = pad.laxis;
			prev_raxis = pad.raxis;
		}
		if (PAD_isPressed(BTN_SELECT) && PAD_isPressed(BTN_START))
			quit = true;

		// L3+R3 triggers joystick calibration
		if (has_joystick &&
			((PAD_justPressed(BTN_L3) && PAD_isPressed(BTN_R3)) ||
			 (PAD_justPressed(BTN_R3) && PAD_isPressed(BTN_L3)))) {
			cal_run(screen);
			PAD_reset();
			dirty = true;
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (UI_statusBarChanged())
			dirty = true;

		if (dirty) {
			GFX_clear(screen);

			UI_renderMenuBar(screen, "Input");

			// L group (centered over DPAD)
			{
				int y = oy + SCALE1(PILL_SIZE);
				int w = getButtonWidth("L1") + SCALE1(BUTTON_INSET) * 2;
				int ox = w;
				if (has_L2)
					w += getButtonWidth("L2") + SCALE1(BUTTON_INSET);

				int dpad_center = SCALE1(PADDING) + SCALE1(PILL_SIZE * 3) / 2;
				int x = dpad_center - w / 2;

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, w}, THEME_COLOR3, RGB_WHITE);

				blitButton("L1", screen, PAD_isPressed(BTN_L1), x + SCALE1(BUTTON_INSET), y + SCALE1(BUTTON_INSET), 0);
				if (has_L2)
					blitButton("L2", screen, PAD_isPressed(BTN_L2), x + ox, y + SCALE1(BUTTON_INSET), 0);
			}

			// R group (centered over ABXY)
			{
				int y = oy + SCALE1(PILL_SIZE);
				int w = getButtonWidth("R1") + SCALE1(BUTTON_INSET) * 2;
				int ox = w;
				if (has_R2)
					w += getButtonWidth("R2") + SCALE1(BUTTON_INSET);

				int abxy_center = FIXED_WIDTH - SCALE1(PADDING) - SCALE1(PILL_SIZE * 3) / 2;
				int x = abxy_center - w / 2;

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, w}, THEME_COLOR3, RGB_WHITE);

				blitButton(has_R2 ? "R2" : "R1", screen, PAD_isPressed(has_R2 ? BTN_R2 : BTN_R1), x + SCALE1(BUTTON_INSET), y + SCALE1(BUTTON_INSET), 0);
				if (has_R2)
					blitButton("R1", screen, PAD_isPressed(BTN_R1), x + ox, y + SCALE1(BUTTON_INSET), 0);
			}

			// DPAD group
			{
				int x = SCALE1(PADDING + PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE * 2 + PILL_SIZE / 2);
				int o = SCALE1(BUTTON_INSET);

				SDL_FillRect(screen, &(SDL_Rect){x, y + SCALE1(PILL_SIZE / 2), SCALE1(PILL_SIZE), SCALE1(PILL_SIZE * 2)}, THEME_COLOR3);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("U", screen, PAD_isPressed(BTN_DPAD_UP), x + o, y + o, 0);

				y += SCALE1(PILL_SIZE * 2);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("D", screen, PAD_isPressed(BTN_DPAD_DOWN), x + o, y + o, 0);

				x -= SCALE1(PILL_SIZE);
				y -= SCALE1(PILL_SIZE);

				SDL_FillRect(screen, &(SDL_Rect){x + SCALE1(PILL_SIZE / 2), y, SCALE1(PILL_SIZE * 2), SCALE1(PILL_SIZE)}, THEME_COLOR3);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("L", screen, PAD_isPressed(BTN_DPAD_LEFT), x + o, y + o, 0);

				x += SCALE1(PILL_SIZE * 2);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("R", screen, PAD_isPressed(BTN_DPAD_RIGHT), x + o, y + o, 0);
			}

			// ABXY group
			{
				int x = FIXED_WIDTH - SCALE1(PADDING + PILL_SIZE * 3) + SCALE1(PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE * 2 + PILL_SIZE / 2);
				int o = SCALE1(BUTTON_INSET);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("X", screen, PAD_isPressed(BTN_X), x + o, y + o, 0);

				y += SCALE1(PILL_SIZE * 2);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("B", screen, PAD_isPressed(BTN_B), x + o, y + o, 0);

				x -= SCALE1(PILL_SIZE);
				y -= SCALE1(PILL_SIZE);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("Y", screen, PAD_isPressed(BTN_Y), x + o, y + o, 0);

				x += SCALE1(PILL_SIZE * 2);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("A", screen, PAD_isPressed(BTN_A), x + o, y + o, 0);
			}

			// VOLUME group
			if (has_volume) {
				int x = (FIXED_WIDTH - SCALE1(99)) / 2;
				int y = oy + SCALE1(PILL_SIZE);
				int w = SCALE1(42);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, SCALE1(98)}, THEME_COLOR3, RGB_WHITE);
				x += SCALE1(BUTTON_INSET);
				y += SCALE1(BUTTON_INSET);
				blitButton("VOL. -", screen, PAD_isPressed(BTN_MINUS), x, y, w);
				x += w + SCALE1(BUTTON_INSET);
				blitButton("VOL. +", screen, PAD_isPressed(BTN_PLUS), x, y, w);
			}

			// SYSTEM group
			{
				int system_count = (has_menu ? 1 : 0) + (has_home ? 1 : 0) + (has_power ? 1 : 0);
				if (system_count > 0) {
					int bw = 42;
					int pw = bw * system_count + BUTTON_INSET * (system_count + 1);

					int x = (FIXED_WIDTH - SCALE1(pw)) / 2;
					int y = oy + SCALE1(PILL_SIZE * 3);
					int w = SCALE1(bw);

					GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, SCALE1(pw)}, THEME_COLOR3, RGB_WHITE);
					x += SCALE1(BUTTON_INSET);
					y += SCALE1(BUTTON_INSET);
					if (has_menu) {
						blitButton("MENU", screen, PAD_isPressed(BTN_MENU), x, y, w);
						x += w + SCALE1(BUTTON_INSET);
					}
					if (has_home) {
						blitButton("HOME", screen, PAD_isPressed(BTN_HOME), x, y, w);
						x += w + SCALE1(BUTTON_INSET);
					}
					if (has_power) {
						blitButton("POWER", screen, PAD_isPressed(BTN_POWER), x, y, w);
					}
				}
			}

			// META group
			{
				int bw = SCALE1(42);
				int pw = SCALE1(BUTTON_INSET) * 3 + bw * 2;

				int x = (FIXED_WIDTH - pw) / 2;
				int y = oy + SCALE1(PILL_SIZE * 5);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, pw}, THEME_COLOR3, RGB_WHITE);
				x += SCALE1(BUTTON_INSET);
				y += SCALE1(BUTTON_INSET);
				blitButton("SELECT", screen, PAD_isPressed(BTN_SELECT), x, y, bw);
				x += bw + SCALE1(BUTTON_INSET);
				blitButton("START", screen, PAD_isPressed(BTN_START), x, y, bw);
			}

			// L3
			if (has_L3) {
				int x = SCALE1(PADDING + PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE * 6);
				int o = SCALE1(BUTTON_INSET);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("L3", screen, PAD_isPressed(BTN_L3), x + o, y + o, 0);
			}

			// R3
			if (has_R3) {
				int x = FIXED_WIDTH - SCALE1(PADDING + PILL_SIZE * 3) + SCALE1(PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE * 6);
				int o = SCALE1(BUTTON_INSET);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("R3", screen, PAD_isPressed(BTN_R3), x + o, y + o, 0);
			}

			// JOYSTICK indicators (below L3/R3, centered between side and middle)
			if (has_joystick) {
				int jside = PILL_SIZE * 3;
				int jsz = SCALE1(jside);
				int jy = oy + SCALE1(PILL_SIZE * 6);

				// center each joystick between its side group and screen center
				int left_cx = SCALE1(PADDING) + SCALE1(PILL_SIZE * 3) / 2;
				int right_cx = FIXED_WIDTH - SCALE1(PADDING) - SCALE1(PILL_SIZE * 3) / 2;
				int mid_cx = FIXED_WIDTH / 2;
				int jl_cx = (left_cx + mid_cx) / 2;
				int jr_cx = (mid_cx + right_cx) / 2;

				for (int ji = 0; ji < 2; ji++) {
					int x = (ji == 0 ? jl_cx : jr_cx) - jsz / 2;
					int ax = ji == 0 ? pad.laxis.x : pad.raxis.x;
					int ay = ji == 0 ? pad.laxis.y : pad.raxis.y;

					// Circle background
					int radius = jsz / 2;
					fillCircle(screen, x + radius, jy + radius, radius, THEME_COLOR3);

					// Crosshair lines
					SDL_FillRect(screen, &(SDL_Rect){x + radius, jy + SCALE1(BUTTON_INSET), SCALE1(1), jsz - SCALE1(BUTTON_INSET) * 2}, RGB_DARK_GRAY);
					SDL_FillRect(screen, &(SDL_Rect){x + SCALE1(BUTTON_INSET), jy + radius, jsz - SCALE1(BUTTON_INSET) * 2, SCALE1(1)}, RGB_DARK_GRAY);

					// Position dot
					int dot_w = joy_dot ? joy_dot->w : SCALE1(BUTTON_SPRITE_SIZE);
					int dot_h = joy_dot ? joy_dot->h : SCALE1(BUTTON_SPRITE_SIZE);
					int margin = SCALE1(BUTTON_INSET + 2);
					int range = jsz / 2 - margin - dot_w / 2;
					int dx = (int)((long)ax * range / 32767);
					int dy = (int)((long)ay * range / 32767);
					int cx = x + jsz / 2 + dx - dot_w / 2;
					int cy = jy + jsz / 2 + dy - dot_h / 2;
					if (joy_dot) {
						SDL_BlitSurface(joy_dot, NULL, screen, &(SDL_Rect){cx, cy});
					} else {
						GFX_blitAsset(ASSET_BUTTON, NULL, screen, &(SDL_Rect){cx, cy});
					}
				}
			}

			if (has_joystick)
				UI_renderButtonHintBar(screen, (char*[]){"SELECT+START", "QUIT", "L3+R3", "CALIBRATE", NULL});
			else
				UI_renderButtonHintBar(screen, (char*[]){"SELECT+START", "QUIT", NULL});

			GFX_flip(screen);
			dirty = false;
		} else {
			GFX_sync();
		}
	}

	if (joy_dot)
		SDL_FreeSurface(joy_dot);

	PWR_enableSleep();
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
