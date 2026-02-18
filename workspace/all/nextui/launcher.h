#ifndef LAUNCHER_H
#define LAUNCHER_H

#include "defines.h"
#include "types.h"
#include "content.h"

// Globals shared between launcher and main loop
// Owned by nextui.c, accessed by launcher.c
extern Directory* top;
extern Array* stack;
extern int quit;
extern int startgame;

typedef struct {
	int can_resume;
	int should_resume;
	int has_preview;
	int has_boxart;
	char slot_path[MAX_PATH];
	char preview_path[MAX_PATH];
	char boxart_path[MAX_PATH];
} ResumeState;

typedef struct {
	int depth;
	int relative;
	int selected;
	int start;
	int end;
} RestoreState;

extern ResumeState resume;
extern RestoreState restore;

// Set cleanup function (called by toggleQuick for Reboot/Poweroff)
typedef void (*CleanupPoolFunc)(void);
void Launcher_setCleanupFunc(CleanupPoolFunc func);

// Navigation
void queueNext(char* cmd);
void openDirectory(char* path, int auto_launch);
void closeDirectory(void);
Array* pathToStack(const char* path);

// Resume
void readyResumePath(char* rom_path, int type);
void readyResume(Entry* entry);
int autoResume(void);

// Game launching
void openPak(char* path);
void openRom(char* path, char* last);
void Entry_open(Entry* self);
void toggleQuick(Entry* self);

// State persistence
void saveLast(char* path);
void loadLast(void);

#endif // LAUNCHER_H
