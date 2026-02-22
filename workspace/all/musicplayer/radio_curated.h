#ifndef __RADIO_CURATED_H__
#define __RADIO_CURATED_H__

#include "radio.h" // For CuratedCountry, CuratedStation types

// Initialize curated stations (load from JSON files)
void radio_curated_init(void);

// Cleanup curated stations
void radio_curated_cleanup(void);

// Get number of available countries
int radio_curated_get_country_count(void);

// Get array of all countries
const CuratedCountry* radio_curated_get_countries(void);

// Get number of stations for a specific country
int radio_curated_get_station_count(const char* country_code);

// Get stations for a specific country
// Returns pointer to first station and sets count
const CuratedStation* radio_curated_get_stations(const char* country_code, int* count);

#endif
