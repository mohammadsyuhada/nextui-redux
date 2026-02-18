#include "types.h"

///////////////////////////////////////
// Array

Array* Array_new(void) {
	Array* self = malloc(sizeof(Array));
	if (!self)
		return NULL;
	self->count = 0;
	self->capacity = 8;
	self->items = malloc(sizeof(void*) * self->capacity);
	if (!self->items) {
		free(self);
		return NULL;
	}
	return self;
}
void Array_push(Array* self, void* item) {
	if (self->count >= self->capacity) {
		int new_capacity = self->capacity * 2;
		void** tmp = realloc(self->items, sizeof(void*) * new_capacity);
		if (!tmp)
			return;
		self->items = tmp;
		self->capacity = new_capacity;
	}
	self->items[self->count++] = item;
}
void Array_unshift(Array* self, void* item) {
	if (self->count == 0)
		return Array_push(self, item);
	Array_push(self, NULL); // ensures we have enough capacity
	for (int i = self->count - 2; i >= 0; i--) {
		self->items[i + 1] = self->items[i];
	}
	self->items[0] = item;
}
void* Array_pop(Array* self) {
	if (self->count == 0)
		return NULL;
	return self->items[--self->count];
}
void Array_remove(Array* self, void* item) {
	if (self->count == 0 || item == NULL)
		return;
	int i = 0;
	while (i < self->count && self->items[i] != item)
		i++;
	if (i == self->count)
		return;
	for (int j = i; j < self->count - 1; j++)
		self->items[j] = self->items[j + 1];
	self->count--;
}
void Array_reverse(Array* self) {
	if (self->count < 2)
		return;
	int end = self->count - 1;
	int mid = self->count / 2;
	for (int i = 0; i < mid; i++) {
		void* item = self->items[i];
		self->items[i] = self->items[end - i];
		self->items[end - i] = item;
	}
}
void Array_free(Array* self) {
	free(self->items);
	free(self);
}
void Array_yoink(Array* self, Array* other) {
	// append entries to self and take ownership
	for (int i = 0; i < other->count; i++)
		Array_push(self, other->items[i]);
	Array_free(other); // `self` now owns the entries
}

int StringArray_indexOf(Array* self, const char* str) {
	for (int i = 0; i < self->count; i++) {
		if (exactMatch(self->items[i], str))
			return i;
	}
	return -1;
}
void StringArray_free(Array* self) {
	for (int i = 0; i < self->count; i++) {
		free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////
// Hash

Hash* Hash_new(void) {
	Hash* self = malloc(sizeof(Hash));
	self->keys = Array_new();
	self->values = Array_new();
	return self;
}
void Hash_free(Hash* self) {
	StringArray_free(self->keys);
	StringArray_free(self->values);
	free(self);
}
void Hash_set(Hash* self, const char* key, const char* value) {
	int i = StringArray_indexOf(self->keys, key);
	if (i >= 0) {
		free(self->values->items[i]);
		self->values->items[i] = strdup(value);
		return;
	}
	Array_push(self->keys, strdup(key));
	Array_push(self->values, strdup(value));
}
char* Hash_get(Hash* self, const char* key) {
	int i = StringArray_indexOf(self->keys, key);
	if (i == -1)
		return NULL;
	return self->values->items[i];
}

///////////////////////////////////////
// Entry

Entry* Entry_new(const char* path, int type) {
	char display_name[MAX_PATH];
	getDisplayName(path, display_name);
	Entry* self = malloc(sizeof(Entry));
	self->path = strdup(path);
	self->name = strdup(display_name);
	self->unique = NULL;
	self->type = type;
	self->alpha = 0;
	self->quickId = QUICK_NONE;
	return self;
}

Entry* Entry_newNamed(const char* path, int type, const char* displayName) {
	Entry* self = Entry_new(path, type);
	free(self->name);
	self->name = strdup(displayName);
	return self;
}

void Entry_free(Entry* self) {
	free(self->path);
	free(self->name);
	if (self->unique)
		free(self->unique);
	free(self);
}

int EntryArray_indexOf(Array* self, const char* path) {
	for (int i = 0; i < self->count; i++) {
		Entry* entry = self->items[i];
		if (exactMatch(entry->path, path))
			return i;
	}
	return -1;
}
int EntryArray_sortEntry(const void* a, const void* b) {
	Entry* item1 = *(Entry**)a;
	Entry* item2 = *(Entry**)b;
	return strcasecmp(item1->name, item2->name);
}
void EntryArray_sort(Array* self) {
	qsort(self->items, self->count, sizeof(void*), EntryArray_sortEntry);
}

void EntryArray_free(Array* self) {
	for (int i = 0; i < self->count; i++) {
		Entry_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////
// IntArray

void IntArray_init(IntArray* self) {
	self->count = 0;
	memset(self->items, 0, sizeof(int) * INT_ARRAY_MAX);
}
void IntArray_push(IntArray* self, int i) {
	if (self->count >= INT_ARRAY_MAX)
		return;
	self->items[self->count++] = i;
}

///////////////////////////////////////
// Directory

void Directory_free(Directory* self) {
	free(self->path);
	free(self->name);
	EntryArray_free(self->entries);
	free(self);
}

///////////////////////////////////////
// DirectoryArray helpers

void DirectoryArray_pop(Array* self) {
	Directory* dir = Array_pop(self);
	if (dir)
		Directory_free(dir);
}
void DirectoryArray_free(Array* self) {
	for (int i = 0; i < self->count; i++) {
		Directory_free(self->items[i]);
	}
	Array_free(self);
}
