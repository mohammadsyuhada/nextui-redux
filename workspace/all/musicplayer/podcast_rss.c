#define _GNU_SOURCE
#include "podcast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <ctype.h>

// yxml parser
#include "yxml.h"

// Parse state for RSS feed
typedef enum {
	RSS_STATE_NONE = 0,
	RSS_STATE_CHANNEL,
	RSS_STATE_CHANNEL_TITLE,
	RSS_STATE_CHANNEL_DESCRIPTION,
	RSS_STATE_CHANNEL_AUTHOR,
	RSS_STATE_CHANNEL_IMAGE,
	RSS_STATE_CHANNEL_IMAGE_URL,
	RSS_STATE_ITEM,
	RSS_STATE_ITEM_TITLE,
	RSS_STATE_ITEM_DESCRIPTION,
	RSS_STATE_ITEM_GUID,
	RSS_STATE_ITEM_PUBDATE,
	RSS_STATE_ITEM_ENCLOSURE,
	RSS_STATE_ITEM_DURATION,
	RSS_STATE_ITUNES_AUTHOR,
	RSS_STATE_ITUNES_IMAGE
} RSSParseState;

// Append string safely
static void safe_strcat(char* dest, const char* src, size_t dest_size) {
	size_t dest_len = strlen(dest);
	size_t src_len = strlen(src);
	if (dest_len + src_len >= dest_size) {
		src_len = dest_size - dest_len - 1;
	}
	if (src_len > 0) {
		memcpy(dest + dest_len, src, src_len);
		dest[dest_len + src_len] = '\0';
	}
}

// Parse RFC 2822 date format (common in RSS)
// Example: "Tue, 14 Jan 2025 08:00:00 GMT"
static uint32_t parse_rfc2822_date(const char* date_str) {
	if (!date_str || !date_str[0])
		return 0;

	struct tm tm = {0};
	char month_str[4] = {0};
	int day, year, hour, min, sec;

	// Try common formats
	// Format: "Day, DD Mon YYYY HH:MM:SS TZ"
	if (sscanf(date_str, "%*[^,], %d %3s %d %d:%d:%d",
			   &day, month_str, &year, &hour, &min, &sec) >= 4) {
		tm.tm_mday = day;
		tm.tm_year = year - 1900;
		tm.tm_hour = hour;
		tm.tm_min = min;
		tm.tm_sec = sec;

		// Parse month
		const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
								"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		for (int i = 0; i < 12; i++) {
			if (strcasecmp(month_str, months[i]) == 0) {
				tm.tm_mon = i;
				break;
			}
		}

		return (uint32_t)mktime(&tm);
	}

	// Try ISO 8601 format: "YYYY-MM-DDTHH:MM:SS"
	if (sscanf(date_str, "%d-%d-%dT%d:%d:%d",
			   &year, &tm.tm_mon, &day, &hour, &min, &sec) >= 3) {
		tm.tm_year = year - 1900;
		tm.tm_mon -= 1; // 0-indexed
		tm.tm_mday = day;
		tm.tm_hour = hour;
		tm.tm_min = min;
		tm.tm_sec = sec;
		return (uint32_t)mktime(&tm);
	}

	return 0;
}

// Parse iTunes duration format (HH:MM:SS or MM:SS or just seconds)
static int parse_duration(const char* duration_str) {
	if (!duration_str || !duration_str[0])
		return 0;

	int h = 0, m = 0, s = 0;

	// Try HH:MM:SS
	if (sscanf(duration_str, "%d:%d:%d", &h, &m, &s) == 3) {
		return h * 3600 + m * 60 + s;
	}

	// Try MM:SS
	if (sscanf(duration_str, "%d:%d", &m, &s) == 2) {
		return m * 60 + s;
	}

	// Try just seconds
	if (sscanf(duration_str, "%d", &s) == 1) {
		return s;
	}

	return 0;
}

// Stack to track element nesting
#define MAX_STACK_DEPTH 32
typedef struct {
	char elements[MAX_STACK_DEPTH][64];
	int depth;
} ElementStack;

static void stack_push(ElementStack* stack, const char* elem) {
	if (stack->depth < MAX_STACK_DEPTH) {
		strncpy(stack->elements[stack->depth], elem, 63);
		stack->elements[stack->depth][63] = '\0';
		stack->depth++;
	}
}

static void stack_pop(ElementStack* stack) {
	if (stack->depth > 0) {
		stack->depth--;
	}
}

static const char* stack_current(ElementStack* stack) {
	if (stack->depth > 0) {
		return stack->elements[stack->depth - 1];
	}
	return "";
}

static const char* stack_parent(ElementStack* stack) {
	if (stack->depth > 1) {
		return stack->elements[stack->depth - 2];
	}
	return "";
}

static bool stack_contains(ElementStack* stack, const char* elem) {
	for (int i = 0; i < stack->depth; i++) {
		if (strcmp(stack->elements[i], elem) == 0) {
			return true;
		}
	}
	return false;
}

// Parse RSS/Atom XML feed
// episodes_out: array to store parsed episodes (caller-provided)
// max_episodes: size of episodes_out array (0 for unlimited if using dynamic allocation)
// episode_count_out: receives the actual number of episodes parsed
int podcast_rss_parse_with_episodes(const char* xml_data, int xml_len, PodcastFeed* feed,
									PodcastEpisode* episodes_out, int max_episodes,
									int* episode_count_out) {
	if (!xml_data || xml_len <= 0 || !feed) {
		return -1;
	}

	int episode_count = 0;

	// Allocate parser state on heap
	yxml_t* parser = (yxml_t*)malloc(sizeof(yxml_t));
	char* stack_buf = (char*)malloc(4096); // Stack buffer for yxml
	if (!parser || !stack_buf) {
		free(parser);
		free(stack_buf);
		return -1;
	}

	yxml_init(parser, stack_buf, 4096);

	ElementStack elem_stack = {0};
	RSSParseState state = RSS_STATE_NONE;

	// Temporary buffers for collecting content
	char content_buf[4096] = {0};
	char attr_name[64] = {0};
	char attr_value[512] = {0};

	// Current episode being parsed
	PodcastEpisode* current_episode = NULL;
	bool in_item = false;

	// Process XML character by character
	for (int i = 0; i < xml_len; i++) {
		yxml_ret_t r = yxml_parse(parser, xml_data[i]);

		if (r < 0) {
			// Parse error - but continue trying
			continue;
		}

		switch (r) {
		case YXML_ELEMSTART: {
			const char* elem = parser->elem;
			stack_push(&elem_stack, elem);

			// Determine parse state based on element hierarchy
			if (strcmp(elem, "channel") == 0) {
				state = RSS_STATE_CHANNEL;
			} else if (strcmp(elem, "item") == 0 || strcmp(elem, "entry") == 0) {
				// New episode
				if (episodes_out && (max_episodes == 0 || episode_count < max_episodes)) {
					current_episode = &episodes_out[episode_count];
					memset(current_episode, 0, sizeof(PodcastEpisode));
					in_item = true;
					state = RSS_STATE_ITEM;
				} else {
					in_item = false;
				}
			} else if (in_item) {
				if (strcmp(elem, "title") == 0) {
					state = RSS_STATE_ITEM_TITLE;
				} else if (strcmp(elem, "description") == 0 || strcmp(elem, "summary") == 0) {
					state = RSS_STATE_ITEM_DESCRIPTION;
				} else if (strcmp(elem, "guid") == 0 || strcmp(elem, "id") == 0) {
					state = RSS_STATE_ITEM_GUID;
				} else if (strcmp(elem, "pubDate") == 0 || strcmp(elem, "published") == 0) {
					state = RSS_STATE_ITEM_PUBDATE;
				} else if (strcmp(elem, "enclosure") == 0) {
					state = RSS_STATE_ITEM_ENCLOSURE;
				} else if (strcmp(elem, "duration") == 0 ||
						   strcmp(elem, "itunes:duration") == 0 ||
						   strstr(elem, "duration") != NULL) {
					// Match "duration", "itunes:duration", or any element containing "duration"
					state = RSS_STATE_ITEM_DURATION;
				}
			} else if (stack_contains(&elem_stack, "channel") &&
					   !stack_contains(&elem_stack, "item") &&
					   !stack_contains(&elem_stack, "entry")) {
				// Only handle channel-level elements when NOT inside an item/entry
				// (even if in_item is false due to exceeding PODCAST_MAX_EPISODES)
				if (strcmp(elem, "title") == 0 && !stack_contains(&elem_stack, "image")) {
					state = RSS_STATE_CHANNEL_TITLE;
				} else if (strcmp(elem, "description") == 0) {
					state = RSS_STATE_CHANNEL_DESCRIPTION;
				} else if (strcmp(elem, "author") == 0) {
					state = RSS_STATE_ITUNES_AUTHOR;
				} else if (strcmp(elem, "image") == 0) {
					state = RSS_STATE_CHANNEL_IMAGE;
				} else if (strcmp(elem, "url") == 0 && stack_contains(&elem_stack, "image")) {
					state = RSS_STATE_CHANNEL_IMAGE_URL;
				}
			}

			content_buf[0] = '\0';
			attr_name[0] = '\0';
			attr_value[0] = '\0';
			break;
		}

		case YXML_ELEMEND: {
			const char* elem = stack_current(&elem_stack);

			// Save collected content
			if (state == RSS_STATE_CHANNEL_TITLE && content_buf[0]) {
				strncpy(feed->title, content_buf, PODCAST_MAX_TITLE - 1);
			} else if (state == RSS_STATE_CHANNEL_DESCRIPTION && content_buf[0]) {
				strncpy(feed->description, content_buf, PODCAST_MAX_DESCRIPTION - 1);
			} else if (state == RSS_STATE_ITUNES_AUTHOR && content_buf[0]) {
				strncpy(feed->author, content_buf, PODCAST_MAX_AUTHOR - 1);
			} else if (state == RSS_STATE_CHANNEL_IMAGE_URL && content_buf[0]) {
				strncpy(feed->artwork_url, content_buf, PODCAST_MAX_URL - 1);
			} else if (in_item && current_episode) {
				if (state == RSS_STATE_ITEM_TITLE && content_buf[0]) {
					strncpy(current_episode->title, content_buf, PODCAST_MAX_TITLE - 1);
				} else if (state == RSS_STATE_ITEM_DESCRIPTION && content_buf[0]) {
					strncpy(current_episode->description, content_buf, PODCAST_MAX_DESCRIPTION - 1);
				} else if (state == RSS_STATE_ITEM_GUID && content_buf[0]) {
					strncpy(current_episode->guid, content_buf, PODCAST_MAX_GUID - 1);
				} else if (state == RSS_STATE_ITEM_PUBDATE && content_buf[0]) {
					current_episode->pub_date = parse_rfc2822_date(content_buf);
				} else if (state == RSS_STATE_ITEM_DURATION && content_buf[0]) {
					current_episode->duration_sec = parse_duration(content_buf);
				}
			}

			// Handle end of item
			if ((strcmp(elem, "item") == 0 || strcmp(elem, "entry") == 0) && in_item) {
				// Only count episodes that have a URL
				if (current_episode && current_episode->url[0]) {
					// Generate GUID if not present
					if (!current_episode->guid[0]) {
						strncpy(current_episode->guid, current_episode->url, PODCAST_MAX_GUID - 1);
					}
					episode_count++;
				}
				in_item = false;
				current_episode = NULL;
			}

			stack_pop(&elem_stack);

			// Reset state based on parent
			if (stack_contains(&elem_stack, "item") || stack_contains(&elem_stack, "entry")) {
				state = RSS_STATE_ITEM;
			} else if (stack_contains(&elem_stack, "channel")) {
				state = RSS_STATE_CHANNEL;
			} else {
				state = RSS_STATE_NONE;
			}
			break;
		}

		case YXML_CONTENT: {
			// Append content data
			safe_strcat(content_buf, parser->data, sizeof(content_buf));
			break;
		}

		case YXML_ATTRSTART: {
			strncpy(attr_name, parser->attr, sizeof(attr_name) - 1);
			attr_value[0] = '\0';
			break;
		}

		case YXML_ATTRVAL: {
			safe_strcat(attr_value, parser->data, sizeof(attr_value));
			break;
		}

		case YXML_ATTREND: {
			// Handle enclosure URL attribute
			if (state == RSS_STATE_ITEM_ENCLOSURE && current_episode) {
				if (strcmp(attr_name, "url") == 0) {
					strncpy(current_episode->url, attr_value, PODCAST_MAX_URL - 1);
				}
			}
			// Handle itunes:image href attribute at channel level
			else if (!in_item && strcmp(attr_name, "href") == 0) {
				const char* current = stack_current(&elem_stack);
				// Check for "image", "itunes:image", or any element containing "image"
				if ((strcmp(current, "image") == 0 ||
					 strcmp(current, "itunes:image") == 0 ||
					 strstr(current, "image") != NULL) &&
					!feed->artwork_url[0]) {
					strncpy(feed->artwork_url, attr_value, PODCAST_MAX_URL - 1);
				}
			}
			// Handle Atom link for enclosure
			else if (in_item && current_episode &&
					 strcmp(stack_current(&elem_stack), "link") == 0) {
				if (strcmp(attr_name, "href") == 0) {
					// Check if this is an enclosure link
					if (!current_episode->url[0]) {
						strncpy(current_episode->url, attr_value, PODCAST_MAX_URL - 1);
					}
				}
			}
			break;
		}

		default:
			break;
		}
	}

	free(parser);
	free(stack_buf);

	// Set output episode count
	if (episode_count_out) {
		*episode_count_out = episode_count;
	}
	feed->episode_count = episode_count;

	// Validate feed
	if (!feed->title[0]) {
		return -1;
	}

	return 0;
}

// Simple wrapper for backward compatibility (no episodes output)
int podcast_rss_parse(const char* xml_data, int xml_len, PodcastFeed* feed) {
	return podcast_rss_parse_with_episodes(xml_data, xml_len, feed, NULL, 0, NULL);
}
