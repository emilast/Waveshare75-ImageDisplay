#ifndef CONFIG_H
#define CONFIG_H

// User configuration for Waveshare75-ImageDisplay.
// Edit the values in this file to adapt the firmware to your setup.

// Name of the WiFi access point the device opens when it can't connect to a
// known network. Connect to it with a phone or laptop to enter WiFi settings.
static const char* NETWORK_NAME = "Waveshare75-HA-dashboard";

// Images to show. The device cycles through them, showing one per refresh.
// Each URL must return an uncompressed 8-bit (grayscale) or 24-bit BMP no
// larger than 800x480 pixels. Both http:// and https:// are supported
// (https certificates are not verified).
static const char* bmpUrls[] = {
  "https://qrwmbaypgxaexdohocwh.supabase.co/functions/v1/xkcd-bmp",
  "http://homeassistant:5000/4",
  // "http://homeassistant:5000/1",
  "http://homeassistant:5000/3",
  // "http://homeassistant:5000/2"
};

// Time between screen updates
static const unsigned long REFRESH_INTERVAL_MS = 60000;

// Time zone for the clock overlay, as a POSIX TZ string.
// Default: Central European Time with daylight saving.
static const char* TIME_ZONE = "CET-1CEST,M3.5.0,M10.5.0/3";

// Time servers used to set the clock
static const char* NTP_SERVER_1 = "pool.ntp.org";
static const char* NTP_SERVER_2 = "time.nist.gov";

// Position of the clock overlay (top-left corner of the text)
static const int CLOCK_X = 70;
static const int CLOCK_Y = 440;

// Images smaller than the display are centered and then shifted down by this
// many pixels (but never so far that the bottom of the image is cut off).
static const int IMAGE_Y_OFFSET = 20;

// Brightness threshold for dithering - pixels darker than this (0-255) are
// forced to black without dithering
static const int DITHERING_BLACK_LEVEL = 16;

#endif
