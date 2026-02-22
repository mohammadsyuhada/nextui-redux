#define _GNU_SOURCE
#include "radio_curated.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "api.h"
#include "include/parson/parson.h"

// Maximum limits
#define MAX_CURATED_COUNTRIES 32
#define MAX_CURATED_STATIONS 256

// Module state
static CuratedCountry curated_countries[MAX_CURATED_COUNTRIES];
static int curated_country_count = 0;

static CuratedStation curated_stations[MAX_CURATED_STATIONS];
static int curated_station_count = 0;

// Stations directory path
static char stations_path[512] = "";

// Load stations from a JSON file for a specific country
static int load_country_stations(const char* filepath) {
	JSON_Value* root = json_parse_file(filepath);
	if (!root) {
		LOG_error("Failed to parse JSON: %s\n", filepath);
		return -1;
	}

	JSON_Object* obj = json_value_get_object(root);
	if (!obj) {
		json_value_free(root);
		return -1;
	}

	const char* country_name = json_object_get_string(obj, "country");
	const char* country_code = json_object_get_string(obj, "code");

	if (!country_name || !country_code) {
		json_value_free(root);
		return -1;
	}

	// Add country to list if not already present
	bool country_exists = false;
	for (int i = 0; i < curated_country_count; i++) {
		if (strcmp(curated_countries[i].code, country_code) == 0) {
			country_exists = true;
			break;
		}
	}

	if (!country_exists && curated_country_count < MAX_CURATED_COUNTRIES) {
		strncpy(curated_countries[curated_country_count].name, country_name, 63);
		curated_countries[curated_country_count].name[63] = '\0';
		strncpy(curated_countries[curated_country_count].code, country_code, 7);
		curated_countries[curated_country_count].code[7] = '\0';
		curated_country_count++;
	}

	// Load stations
	JSON_Array* stations_arr = json_object_get_array(obj, "stations");
	if (stations_arr) {
		int count = json_array_get_count(stations_arr);
		for (int i = 0; i < count && curated_station_count < MAX_CURATED_STATIONS; i++) {
			JSON_Object* station = json_array_get_object(stations_arr, i);
			if (!station)
				continue;

			const char* name = json_object_get_string(station, "name");
			const char* url = json_object_get_string(station, "url");
			const char* genre = json_object_get_string(station, "genre");
			const char* slogan = json_object_get_string(station, "slogan");

			if (name && url) {
				strncpy(curated_stations[curated_station_count].name, name, RADIO_MAX_NAME - 1);
				curated_stations[curated_station_count].name[RADIO_MAX_NAME - 1] = '\0';
				strncpy(curated_stations[curated_station_count].url, url, RADIO_MAX_URL - 1);
				curated_stations[curated_station_count].url[RADIO_MAX_URL - 1] = '\0';
				strncpy(curated_stations[curated_station_count].genre, genre ? genre : "", 63);
				curated_stations[curated_station_count].genre[63] = '\0';
				strncpy(curated_stations[curated_station_count].slogan, slogan ? slogan : "", 127);
				curated_stations[curated_station_count].slogan[127] = '\0';
				strncpy(curated_stations[curated_station_count].country_code, country_code, 7);
				curated_stations[curated_station_count].country_code[7] = '\0';
				curated_station_count++;
			}
		}
	}

	json_value_free(root);
	return 0;
}

// Scan stations directory and load all JSON files
static void load_curated_stations(void) {
	curated_country_count = 0;
	curated_station_count = 0;

	// Stations folder is in the pak directory (launch.sh sets cwd to pak folder)
	strcpy(stations_path, "./stations");

	DIR* dir = opendir(stations_path);
	if (!dir)
		return;

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		// Skip non-JSON files
		const char* ext = strrchr(ent->d_name, '.');
		if (!ext || strcasecmp(ext, ".json") != 0)
			continue;

		char filepath[768];
		snprintf(filepath, sizeof(filepath), "%s/%s", stations_path, ent->d_name);
		load_country_stations(filepath);
	}

	closedir(dir);
}

void radio_curated_init(void) {
	load_curated_stations();
}

void radio_curated_cleanup(void) {
	curated_country_count = 0;
	curated_station_count = 0;
	stations_path[0] = '\0';
}

int radio_curated_get_country_count(void) {
	return curated_country_count;
}

const CuratedCountry* radio_curated_get_countries(void) {
	return curated_countries;
}

int radio_curated_get_station_count(const char* country_code) {
	int count = 0;
	for (int i = 0; i < curated_station_count; i++) {
		if (strcmp(curated_stations[i].country_code, country_code) == 0) {
			count++;
		}
	}
	return count;
}

const CuratedStation* radio_curated_get_stations(const char* country_code, int* count) {
	// Find the first station for this country and count total
	const CuratedStation* first = NULL;
	*count = 0;

	for (int i = 0; i < curated_station_count; i++) {
		if (strcmp(curated_stations[i].country_code, country_code) == 0) {
			if (!first) {
				first = &curated_stations[i];
			}
			(*count)++;
		}
	}

	return first;
}
