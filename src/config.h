#pragma once

// Starting NOAA CO-OPS tide station, used until one is chosen on the device (GPIO 34 button).
// Find ids at https://tidesandcurrents.noaa.gov/tide_predictions.html
// Default is the station saved in the Tides Mac app.
#define STATION_ID    "9419059"
#define STATION_NAME  "Trinidad Harbor, CA"

// POSIX time zone of the starting station (US Pacific with daylight saving).
#define TIME_ZONE     "PST8PDT,M3.2.0,M11.1.0"

// Show metres instead of feet.
#define USE_METERS    false

// How often the screen redraws (the "now" marker and countdowns move).
#define REDRAW_MINUTES 30

// How often tide data and the clock are re-fetched over Wi-Fi.
#define FETCH_HOURS    6

// Name of the temporary Wi-Fi network the board opens when it needs Wi-Fi details.
#define SETUP_AP_NAME "Tides-Setup"
