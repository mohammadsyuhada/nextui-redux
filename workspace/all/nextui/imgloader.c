#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "config.h"
#include "imgloader.h"

///////////////////////////////////////
// Internal task queue structures

typedef struct {
	char imagePath[MAX_PATH];
	BackgroundLoadedCallback callback;
} LoadBackgroundTask;

typedef struct TaskNode {
	LoadBackgroundTask* task;
	struct TaskNode* next;
} TaskNode;

///////////////////////////////////////
// Generic task queue

typedef struct TaskQueue {
	TaskNode* head;
	TaskNode* tail;
	int size;
	SDL_mutex* mutex;
	SDL_cond* cond;
} TaskQueue;

///////////////////////////////////////
// Internal state

static TaskQueue bgQueue = {0};
static TaskQueue thumbQueue = {0};
SDL_mutex* bgqueueMutex = NULL;
SDL_mutex* thumbqueueMutex = NULL;

static SDL_Thread* bgLoadThread = NULL;
static SDL_Thread* thumbLoadThread = NULL;

static SDL_atomic_t workerThreadsShutdown; // Flag to signal threads to exit (atomic for thread safety)

static SDL_atomic_t needDrawAtomic;

// Cached screen properties (set once in initImageLoaderPool, safe to read from worker threads)
static Uint32 cachedScreenFormat = 0;
static int cachedScreenBitsPerPixel = 0;
static int cachedScreenW = 0;
static int cachedScreenH = 0;

///////////////////////////////////////
// Thumbnail cache

#define THUMB_CACHE_SIZE 8

typedef struct {
	char path[MAX_PATH];
	SDL_Surface* surface;
	int lru_counter;
	bool occupied;
} ThumbCacheEntry;

static ThumbCacheEntry thumb_cache[THUMB_CACHE_SIZE];
static int thumb_lru_counter = 0;
static char desiredThumbPath[MAX_PATH] = {0};
static SDL_atomic_t thumbAsyncLoaded;

///////////////////////////////////////
// Shared state (non-static, externed in imgloader.h)

SDL_mutex* bgMutex = NULL;
SDL_mutex* thumbMutex = NULL;
SDL_mutex* frameMutex = NULL;
SDL_mutex* fontMutex = NULL;
SDL_cond* flipCond = NULL;

SDL_Surface* folderbgbmp = NULL;
SDL_Surface* thumbbmp = NULL;

int folderbgchanged = 0;
int thumbchanged = 0;

bool frameReady = true;

///////////////////////////////////////
// Atomic state accessors

void setNeedDraw(int v) {
	SDL_AtomicSet(&needDrawAtomic, v);
}
int getNeedDraw(void) {
	return SDL_AtomicGet(&needDrawAtomic);
}

///////////////////////////////////////
// Queue management

#define MAX_QUEUE_SIZE 1

void enqueueTask(TaskQueue* q, LoadBackgroundTask* task) {
	SDL_LockMutex(q->mutex);
	TaskNode* node = (TaskNode*)malloc(sizeof(TaskNode));
	if (!node) {
		free(task);
		SDL_UnlockMutex(q->mutex);
		return;
	}
	node->task = task;
	node->next = NULL;

	// If queue is full, drop the oldest task (head)
	if (q->size >= MAX_QUEUE_SIZE) {
		TaskNode* oldNode = q->head;
		if (oldNode) {
			q->head = oldNode->next;
			if (!q->head) {
				q->tail = NULL;
			}
			if (oldNode->task) {
				free(oldNode->task);
			}
			free(oldNode);
			q->size--;
		}
	}

	// Enqueue the new task
	if (q->tail) {
		q->tail->next = node;
		q->tail = node;
	} else {
		q->head = q->tail = node;
	}

	q->size++;
	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);
}

///////////////////////////////////////
// Worker thread (shared by BG and Thumb loaders)

int loadWorker(void* arg) {
	TaskQueue* q = (TaskQueue*)arg;
	while (!SDL_AtomicGet(&workerThreadsShutdown)) {
		SDL_LockMutex(q->mutex);
		while (!q->head && !SDL_AtomicGet(&workerThreadsShutdown)) {
			SDL_CondWait(q->cond, q->mutex);
		}
		if (SDL_AtomicGet(&workerThreadsShutdown)) {
			SDL_UnlockMutex(q->mutex);
			break;
		}
		TaskNode* node = q->head;
		q->head = node->next;
		if (!q->head)
			q->tail = NULL;
		q->size--;
		SDL_UnlockMutex(q->mutex);

		LoadBackgroundTask* task = node->task;
		free(node);

		SDL_Surface* result = NULL;
		SDL_Surface* image = IMG_Load(task->imagePath);
		if (image) {
			SDL_Surface* imageRGBA = SDL_ConvertSurfaceFormat(image, cachedScreenFormat, 0);
			SDL_FreeSurface(image);
			result = imageRGBA;
		}

		if (task->callback) {
			task->callback(result);
		}
		free(task);
	}
	return 0;
}

///////////////////////////////////////
// Thumbnail cache helpers (must be called under thumbMutex)

static void thumbCacheInsert(const char* path, SDL_Surface* surface) {
	int target = -1;
	int min_lru = INT_MAX;

	// Check if already cached (update in place)
	for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
		if (thumb_cache[i].occupied && strcmp(thumb_cache[i].path, path) == 0) {
			SDL_FreeSurface(thumb_cache[i].surface);
			thumb_cache[i].surface = surface;
			thumb_cache[i].lru_counter = ++thumb_lru_counter;
			return;
		}
	}

	// Find empty slot
	for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
		if (!thumb_cache[i].occupied) {
			target = i;
			break;
		}
	}

	// No empty slot - evict LRU
	if (target < 0) {
		for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
			if (thumb_cache[i].lru_counter < min_lru) {
				min_lru = thumb_cache[i].lru_counter;
				target = i;
			}
		}
		if (thumb_cache[target].surface)
			SDL_FreeSurface(thumb_cache[target].surface);
	}

	strncpy(thumb_cache[target].path, path, sizeof(thumb_cache[target].path) - 1);
	thumb_cache[target].path[sizeof(thumb_cache[target].path) - 1] = '\0';
	thumb_cache[target].surface = surface;
	thumb_cache[target].lru_counter = ++thumb_lru_counter;
	thumb_cache[target].occupied = true;
}

///////////////////////////////////////
// Dedicated thumbnail worker thread

static int thumbLoadWorker(void* arg) {
	TaskQueue* q = (TaskQueue*)arg;
	while (!SDL_AtomicGet(&workerThreadsShutdown)) {
		SDL_LockMutex(q->mutex);
		while (!q->head && !SDL_AtomicGet(&workerThreadsShutdown)) {
			SDL_CondWait(q->cond, q->mutex);
		}
		if (SDL_AtomicGet(&workerThreadsShutdown)) {
			SDL_UnlockMutex(q->mutex);
			break;
		}
		TaskNode* node = q->head;
		q->head = node->next;
		if (!q->head)
			q->tail = NULL;
		q->size--;
		SDL_UnlockMutex(q->mutex);

		LoadBackgroundTask* task = node->task;
		free(node);

		SDL_Surface* result = NULL;
		SDL_Surface* image = IMG_Load(task->imagePath);
		if (image) {
			SDL_Surface* imageRGBA =
				SDL_ConvertSurfaceFormat(image, cachedScreenFormat, 0);
			SDL_FreeSurface(image);

			if (imageRGBA) {
				// Downscale to display dimensions before processing
				int img_w = imageRGBA->w;
				int img_h = imageRGBA->h;
				double aspect_ratio = (double)img_h / img_w;
				int max_w = (int)(cachedScreenW * CFG_getGameArtWidth());
				int max_h = (int)(cachedScreenH * 0.6);
				int new_w = max_w;
				int new_h = (int)(new_w * aspect_ratio);
				if (new_h > max_h) {
					new_h = max_h;
					new_w = (int)(new_h / aspect_ratio);
				}

				if (new_w > 0 && new_h > 0 &&
					(new_w < img_w || new_h < img_h)) {
					SDL_Surface* downscaled = SDL_CreateRGBSurfaceWithFormat(
						0, new_w, new_h,
						imageRGBA->format->BitsPerPixel,
						imageRGBA->format->format);
					if (downscaled) {
						SDL_BlitScaled(imageRGBA, NULL, downscaled, NULL);
						SDL_FreeSurface(imageRGBA);
						imageRGBA = downscaled;
					}
				}

				// Apply rounded corners at display resolution (much faster)
				GFX_ApplyRoundedCorners_8888(
					imageRGBA,
					&(SDL_Rect){0, 0, imageRGBA->w, imageRGBA->h},
					SCALE1(CFG_getThumbnailRadius()));

				result = imageRGBA;
			}
		}

		// Cache result and conditionally update thumbbmp
		SDL_LockMutex(thumbMutex);
		bool is_current = (strcmp(task->imagePath, desiredThumbPath) == 0);
		bool had_any = (thumbbmp != NULL);

		if (result) {
			if (is_current) {
				// Duplicate for thumbbmp before cache takes ownership
				SDL_Surface* thumb_copy =
					SDL_ConvertSurface(result, result->format, 0);
				thumbCacheInsert(task->imagePath, result);
				if (thumbbmp)
					SDL_FreeSurface(thumbbmp);
				thumbbmp = thumb_copy;
			} else {
				thumbCacheInsert(task->imagePath, result);
			}
		}

		if (is_current) {
			if (!result) {
				if (thumbbmp)
					SDL_FreeSurface(thumbbmp);
				thumbbmp = NULL;
			}
			thumbchanged = 1;
			setNeedDraw(1);
			// Signal layout recalculation only if thumb presence changed
			if (had_any != (thumbbmp != NULL))
				SDL_AtomicSet(&thumbAsyncLoaded, 1);
		}

		SDL_UnlockMutex(thumbMutex);
		free(task);
	}
	return 0;
}

///////////////////////////////////////
// Public loading functions

void startLoadFolderBackground(const char* imagePath, BackgroundLoadedCallback callback) {
	LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
	if (!task)
		return;

	snprintf(task->imagePath, sizeof(task->imagePath), "%s", imagePath);
	task->callback = callback;
	enqueueTask(&bgQueue, task);
}

void onBackgroundLoaded(SDL_Surface* surface) {
	SDL_LockMutex(bgMutex);
	folderbgchanged = 1;
	if (folderbgbmp)
		SDL_FreeSurface(folderbgbmp);
	if (!surface) {
		folderbgbmp = NULL;
		setNeedDraw(1);
		SDL_UnlockMutex(bgMutex);
		return;
	}
	folderbgbmp = surface;
	setNeedDraw(1);
	SDL_UnlockMutex(bgMutex);
}

bool startLoadThumb(const char* thumbpath) {
	SDL_LockMutex(thumbMutex);

	// Fast path: already showing the right thumb
	if (thumbbmp && strcmp(desiredThumbPath, thumbpath) == 0) {
		thumbchanged = 1;
		setNeedDraw(1);
		SDL_UnlockMutex(thumbMutex);
		return true;
	}

	// Different item selected
	strncpy(desiredThumbPath, thumbpath, sizeof(desiredThumbPath) - 1);
	desiredThumbPath[sizeof(desiredThumbPath) - 1] = '\0';

	// Check cache - swap immediately if found
	for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
		if (thumb_cache[i].occupied &&
			strcmp(thumb_cache[i].path, thumbpath) == 0) {
			thumb_cache[i].lru_counter = ++thumb_lru_counter;
			if (thumbbmp)
				SDL_FreeSurface(thumbbmp);
			thumbbmp = SDL_ConvertSurface(thumb_cache[i].surface,
										  thumb_cache[i].surface->format, 0);
			if (thumbbmp) {
				thumbchanged = 1;
				setNeedDraw(1);
			}
			SDL_UnlockMutex(thumbMutex);
			return thumbbmp != NULL;
		}
	}

	// Cache miss - keep old thumb visible while loading
	bool has_thumb = (thumbbmp != NULL);
	if (has_thumb)
		thumbchanged = 1; // redraw old thumb for this frame
	SDL_UnlockMutex(thumbMutex);

	// Enqueue for async loading
	LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
	if (!task)
		return has_thumb;
	snprintf(task->imagePath, sizeof(task->imagePath), "%s", thumbpath);
	task->callback = NULL;
	enqueueTask(&thumbQueue, task);
	return has_thumb;
}

int thumbCheckAsyncLoaded(void) {
	return SDL_AtomicCAS(&thumbAsyncLoaded, 1, 0);
}

static void thumbCacheClear(void) {
	for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
		if (thumb_cache[i].surface)
			SDL_FreeSurface(thumb_cache[i].surface);
		thumb_cache[i] = (ThumbCacheEntry){0};
	}
	thumb_lru_counter = 0;
	desiredThumbPath[0] = '\0';
}

///////////////////////////////////////
// Lifecycle

void initImageLoaderPool(void) {
	// Initialize shutdown flag to 0
	SDL_AtomicSet(&workerThreadsShutdown, 0);
	SDL_AtomicSet(&needDrawAtomic, 0);

	// Cache screen properties for thread-safe access from workers
	cachedScreenFormat = screen->format->format;
	cachedScreenBitsPerPixel = screen->format->BitsPerPixel;
	cachedScreenW = screen->w;
	cachedScreenH = screen->h;

	bgQueue.mutex = SDL_CreateMutex();
	bgQueue.cond = SDL_CreateCond();
	thumbQueue.mutex = SDL_CreateMutex();
	thumbQueue.cond = SDL_CreateCond();
	bgqueueMutex = bgQueue.mutex;
	thumbqueueMutex = thumbQueue.mutex;
	bgMutex = SDL_CreateMutex();
	thumbMutex = SDL_CreateMutex();
	frameMutex = SDL_CreateMutex();
	fontMutex = SDL_CreateMutex();
	flipCond = SDL_CreateCond();

	if (!bgQueue.mutex || !bgQueue.cond || !thumbQueue.mutex || !thumbQueue.cond ||
		!bgMutex || !thumbMutex ||
		!frameMutex || !fontMutex || !flipCond) {
		fprintf(stderr, "imgloader: failed to create SDL sync primitives\n");
		return;
	}

	SDL_AtomicSet(&thumbAsyncLoaded, 0);

	bgLoadThread = SDL_CreateThread(loadWorker, "BGLoadWorker", &bgQueue);
	thumbLoadThread = SDL_CreateThread(thumbLoadWorker, "ThumbLoadWorker", &thumbQueue);
	if (!bgLoadThread || !thumbLoadThread) {
		fprintf(stderr, "imgloader: failed to create worker threads\n");
	}
}

void cleanupImageLoaderPool(void) {
	// Signal all worker threads to exit (atomic set for thread safety)
	SDL_AtomicSet(&workerThreadsShutdown, 1);

	// Wake up all waiting threads (must hold corresponding mutex to avoid lost-wakeup race)
	if (bgQueue.mutex && bgQueue.cond) {
		SDL_LockMutex(bgQueue.mutex);
		SDL_CondSignal(bgQueue.cond);
		SDL_UnlockMutex(bgQueue.mutex);
	}
	if (thumbQueue.mutex && thumbQueue.cond) {
		SDL_LockMutex(thumbQueue.mutex);
		SDL_CondSignal(thumbQueue.cond);
		SDL_UnlockMutex(thumbQueue.mutex);
	}

	// Wait for all worker threads to finish
	if (bgLoadThread) {
		SDL_WaitThread(bgLoadThread, NULL);
		bgLoadThread = NULL;
	}
	if (thumbLoadThread) {
		SDL_WaitThread(thumbLoadThread, NULL);
		thumbLoadThread = NULL;
	}

	// Small delay to ensure llvmpipe/OpenGL threads have completed any pending operations
	SDL_Delay(10);

	// Drain any residual tasks left in queues
	while (bgQueue.head) {
		TaskNode* n = bgQueue.head;
		bgQueue.head = n->next;
		free(n->task);
		free(n);
	}
	bgQueue.tail = NULL;
	bgQueue.size = 0;

	while (thumbQueue.head) {
		TaskNode* n = thumbQueue.head;
		thumbQueue.head = n->next;
		free(n->task);
		free(n);
	}
	thumbQueue.tail = NULL;
	thumbQueue.size = 0;

	// Clear thumbnail cache
	thumbCacheClear();

	// Acquire and release each mutex before destroying to ensure no thread is in a critical section
	// This creates a memory barrier and ensures proper synchronization
	if (bgQueue.mutex) {
		SDL_LockMutex(bgQueue.mutex);
		SDL_UnlockMutex(bgQueue.mutex);
	}
	if (thumbQueue.mutex) {
		SDL_LockMutex(thumbQueue.mutex);
		SDL_UnlockMutex(thumbQueue.mutex);
	}
	if (bgMutex) {
		SDL_LockMutex(bgMutex);
		SDL_UnlockMutex(bgMutex);
	}
	if (thumbMutex) {
		SDL_LockMutex(thumbMutex);
		SDL_UnlockMutex(thumbMutex);
	}
	if (frameMutex) {
		SDL_LockMutex(frameMutex);
		SDL_UnlockMutex(frameMutex);
	}
	if (fontMutex) {
		SDL_LockMutex(fontMutex);
		SDL_UnlockMutex(fontMutex);
	}

	// Destroy mutexes and condition variables
	if (bgQueue.mutex)
		SDL_DestroyMutex(bgQueue.mutex);
	if (thumbQueue.mutex)
		SDL_DestroyMutex(thumbQueue.mutex);
	if (bgMutex)
		SDL_DestroyMutex(bgMutex);
	if (thumbMutex)
		SDL_DestroyMutex(thumbMutex);
	if (frameMutex)
		SDL_DestroyMutex(frameMutex);
	if (fontMutex)
		SDL_DestroyMutex(fontMutex);

	if (bgQueue.cond)
		SDL_DestroyCond(bgQueue.cond);
	if (thumbQueue.cond)
		SDL_DestroyCond(thumbQueue.cond);
	if (flipCond)
		SDL_DestroyCond(flipCond);

	// Set pointers to NULL after destruction
	bgQueue = (TaskQueue){0};
	thumbQueue = (TaskQueue){0};
	bgqueueMutex = NULL;
	thumbqueueMutex = NULL;
	bgMutex = NULL;
	thumbMutex = NULL;
	frameMutex = NULL;
	fontMutex = NULL;
	flipCond = NULL;
}
