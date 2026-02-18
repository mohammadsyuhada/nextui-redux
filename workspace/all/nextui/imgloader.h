#ifndef IMGLOADER_H
#define IMGLOADER_H

#include "api.h"
#include <stdbool.h>

// Animation direction enum
enum {
	ANIM_NONE = 0,
	SLIDE_LEFT = 1,
	SLIDE_RIGHT = 2,
};

// Task structures (needed by main loop to create AnimTask)
typedef void (*BackgroundLoadedCallback)(SDL_Surface* surface);

typedef struct finishedTask {
	int startX;
	int targetX;
	int startY;
	int targetY;
	int targetTextY;
	int move_y;
	int move_w;
	int move_h;
	int frames;
	int done;
	char* entry_name;
	SDL_Rect dst;
} finishedTask;

typedef void (*AnimTaskCallback)(finishedTask* task);
typedef struct AnimTask {
	int startX;
	int targetX;
	int startY;
	int targetY;
	int targetTextY;
	int move_w;
	int move_h;
	int frames;
	AnimTaskCallback callback;
	char* entry_name;
	SDL_Rect dst;
} AnimTask;

// Screen surface (owned by nextui.c)
extern SDL_Surface* screen;

// Shared surfaces (owned by imgloader.c)
extern SDL_Surface* folderbgbmp;
extern SDL_Surface* thumbbmp;
extern SDL_Surface* globalpill;
extern SDL_Surface* globalText;

// Synchronization primitives (owned by imgloader.c)
extern SDL_mutex* bgMutex;		   // protects: folderbgbmp, folderbgchanged
extern SDL_mutex* thumbMutex;	   // protects: thumbbmp, thumbchanged
extern SDL_mutex* animMutex;	   // protects: pillRect, pilltargetY, pilltargetTextY, pillanimdone, globalpill, globalText
extern SDL_mutex* frameMutex;	   // protects: frameReady; paired with flipCond
extern SDL_mutex* fontMutex;	   // protects: font rendering calls
extern SDL_cond* flipCond;		   // signalled when frameReady becomes true
extern SDL_mutex* bgqueueMutex;	   // alias to internal bgQueue.mutex (used by render loop)
extern SDL_mutex* thumbqueueMutex; // alias to internal thumbQueue.mutex (used by render loop)
extern SDL_mutex* animqueueMutex;  // protects: anim task queue, currentAnimQueueSize

// Shared state flags (see mutex comments above for which mutex protects each)
extern int folderbgchanged;
extern int thumbchanged;
extern SDL_Rect pillRect;
extern int pilltargetY;
extern int pilltargetTextY;
extern bool frameReady;
extern bool pillanimdone;
extern int currentAnimQueueSize;

// Atomic state accessors
void setAnimationDraw(int v);
int getAnimationDraw(void);
void setNeedDraw(int v);
int getNeedDraw(void);

// Lifecycle
void initImageLoaderPool(void);
void cleanupImageLoaderPool(void);

// Background loading
void startLoadFolderBackground(const char* imagePath, BackgroundLoadedCallback callback);
void onBackgroundLoaded(SDL_Surface* surface);

// Thumbnail loading
void startLoadThumb(const char* thumbpath, BackgroundLoadedCallback callback);
void onThumbLoaded(SDL_Surface* surface);

// Pill animation
void updatePillTextSurface(const char* entry_name, int move_w, SDL_Color text_color);
void animPill(AnimTask* task);

#endif // IMGLOADER_H
