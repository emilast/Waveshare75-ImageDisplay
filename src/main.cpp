/* Includes ------------------------------------------------------------------*/
#include "DEV_Config.h"
#include "EPD.h"
#include "GUI_Paint.h"
#include <stdlib.h>

#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define DISPLAY_WIDTH EPD_7IN5_V2_WIDTH
#define DISPLAY_HEIGHT EPD_7IN5_V2_HEIGHT

#define SOLID               0x1
#define TRANSPARENT         0x2 // Transparent background, no pixels drawn
#define TRANSPARENT2        0x3 // Semi-transparent, half of the pixels drawn
#define TRANSPARENT3        0x4 // More transparent
#define TRANSPARENT4        0x5 // Even more transparent, one fourth of the pixels drawn

const char* NETWORK_NAME = "Waveshare75-HA-dashboard";

// Array of strings BMP_URL_1 and BMP_URL_2 as items
const char* bmpUrls[] = {
  "https://qrwmbaypgxaexdohocwh.supabase.co/functions/v1/xkcd-bmp",
   "http://homeassistant:5000/4",
  //  "http://homeassistant:5000/1",
   "http://homeassistant:5000/3",
  //  "http://homeassistant:5000/2"
};
static const int NUM_URLS = sizeof(bmpUrls) / sizeof(bmpUrls[0]);

// Time zone (POSIX TZ string) used for the clock overlay
static const char* TIME_ZONE = "CET-1CEST,M3.5.0,M10.5.0/3";

// Brightness threshold for dithering - pixels darker than this will be forced to black without dithering
static const int DITHERING_BLACK_LEVEL = 16;

// Images smaller than the display are centered and then shifted down by this
// many pixels (but never so far that the bottom of the image is cut off).
static const int IMAGE_Y_OFFSET = 20;

// Time between screen updates
static const unsigned long REFRESH_INTERVAL_MS = 60000;

// Network timeouts
static const uint32_t HTTP_CONNECT_TIMEOUT_MS = 5000;
static const uint16_t HTTP_READ_TIMEOUT_MS = 10000;
static const int WIFI_CONNECT_TIMEOUT_S = 20;
static const int WIFI_PORTAL_TIMEOUT_S = 180; // Restart if nobody configures WiFi within this time

// Restart the device after this many failed update cycles in a row
static const int MAX_CONSECUTIVE_FAILURES = 5;

// Maximum number of bytes between the BMP header and the pixel data (palette etc.)
static const int MAX_HEADER_SKIP = 4096;

static const int BMP_HEADER_SIZE = 54;

// 4x4 Bayer ordered dithering matrix (0-15)
static const uint8_t BAYER_MATRIX[4][4] = {
  {0,  8,  2,  10},
  {12, 4,  14, 6},
  {3,  11, 1,  9},
  {15, 7,  13, 5}
};

// Display buffer, 2 bits per pixel (0=black, 1=dark gray, 2=light gray, 3=white)
uint8_t* buffer = nullptr; // Will be allocated dynamically
size_t bufferSize = 0;

int currentUrlIndex = 0;
int consecutiveFailures = 0;

// Result of trying to fetch and decode an image
enum DownloadResult {
  DOWNLOAD_OK,          // Image decoded into buffer, ready to display
  DOWNLOAD_HTTP_ERROR,  // Error message drawn into buffer, should be displayed
  DOWNLOAD_FAILED       // Nothing usable in buffer, keep current screen content
};

DownloadResult downloadBMP(const char *url);
bool decodeBMPImageData(WiFiClient *stream, int bitDepth, int width, int height, bool topDown);

// Set pixel in 2bpp buffer. Pixels outside the display are ignored.
// x, y = pixel coordinate
// color = 0=black, 1=dark gray, 2=light gray, 3=white
void SetPixel2bpp(uint8_t* buffer, int x, int y, uint8_t color) {
  if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
    return;
  }
  int index = (y * DISPLAY_WIDTH + x);
  int byteIndex = index / 4;          // 4 pixels per byte
  int shift = (3 - (index % 4)) * 2;  // which 2-bit field
  buffer[byteIndex] &= ~(0x3 << shift);       // clear bits
  buffer[byteIndex] |= ((color & 0x3) << shift); // set color
}

// Draw a string with any font size from sFONT. Text running past the right
// edge of the display is cut off.
void DrawString2bpp(uint8_t* buffer, int x, int y,
      const char* text, uint8_t textColor, uint8_t bgColor, uint8_t transparentBg,
      sFONT* font) {
  int charWidth = font->Width;
  int charHeight = font->Height;
  int bytesPerRow = (charWidth + 7) / 8;
  int bytesPerChar = bytesPerRow * charHeight;

  while (*text && x < DISPLAY_WIDTH) {
    char c = *text;
    if (c < 32 || c > 126) {
      c = '?'; // fallback
    }
    int charIndex = (c - 32) * bytesPerChar;

    for (int row = 0; row < charHeight; row++) {
      for (int col = 0; col < charWidth; col++) {
        int byteInRow = col / 8;
        int bitInByte = 7 - (col % 8);
        uint8_t lineByte = font->table[charIndex + row * bytesPerRow + byteInRow];

        bool useBgPixel =
          (transparentBg == SOLID) || // Solid background, all pixels drawn
          (transparentBg == TRANSPARENT2 && ((y + row + x + col) % 2 == 0)) ||
          (transparentBg == TRANSPARENT3 && ((y + row + x + col) % 5 == 0)) ||
          (transparentBg == TRANSPARENT4 && ((y + row + x + col) % 9 == 0));

        if (lineByte & (1 << bitInByte)) {
          SetPixel2bpp(buffer, x + col, y + row, textColor);
        } else if (useBgPixel) {
          SetPixel2bpp(buffer, x + col, y + row, bgColor);
        }
      }
    }
    x += charWidth; // next character
    text++;
  }
}

// Little-endian helpers for parsing the BMP header without unaligned access
static uint32_t readLE32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t readLE16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Read exactly len bytes from the stream, waiting up to the stream timeout.
static bool readExact(WiFiClient* stream, uint8_t* dest, size_t len) {
  return stream->readBytes(dest, len) == len;
}

// Discard len bytes from the stream, waiting up to the stream timeout.
static bool skipBytes(WiFiClient* stream, size_t len) {
  uint8_t scratch[64];
  while (len > 0) {
    size_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);
    if (!readExact(stream, scratch, chunk)) {
      return false;
    }
    len -= chunk;
  }
  return true;
}

// True once NTP has provided a plausible wall-clock time
static bool isTimeValid() {
  return time(nullptr) > 1700000000; // Nov 2023
}

/* Entry point ----------------------------------------------------------------*/
void setup()
{
  DEV_Module_Init();

  // Allocate image buffer dynamically
  bufferSize = DISPLAY_WIDTH * DISPLAY_HEIGHT / 4;
  buffer = (uint8_t*)malloc(bufferSize);
  if (buffer == nullptr) {
    Serial.println("Failed to allocate buffer memory!");
    ESP.restart();
  }
  memset(buffer, 0xFF, bufferSize);
  Paint_NewImage(buffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, WHITE);
  Serial.printf("Allocated %u bytes for display buffer\n", (unsigned)bufferSize);

  // Startup message
  EPD_7IN5_V2_Init_Fast(); // Fast => Less flicker when displaying the screen
  Paint_SelectImage(buffer);
  Paint_Clear(WHITE);

  // WiFi connection via WifiManager
  Paint_DrawString_EN(70, 50, "Connecting to Wifi network...", &Font16, WHITE, BLACK);
  Paint_DrawString_EN(70, 90, "In case of failure, connect to the following WiFI:", &Font12, WHITE, BLACK);
  Paint_DrawString_EN(90, 130, NETWORK_NAME, &Font16, WHITE, BLACK);
  Paint_DrawString_EN(70, 170, "to configure network settings.", &Font12, WHITE, BLACK);

  EPD_7IN5_V2_Display(buffer);
  EPD_7IN5_V2_Sleep(); // Don't keep the panel powered while waiting for WiFi

  WiFiManager wm;
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_S);
  // Without a timeout the configuration portal waits forever, e.g. when the
  // device boots faster than the router after a power cut.
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  bool res = wm.autoConnect(NETWORK_NAME);
  if (!res) {
    Serial.println("Failed to connect to WiFi, restarting");
    ESP.restart();
  }
  Serial.println("Connected to WiFi");
  WiFi.setAutoReconnect(true);

  // Get current time from time server and set local time zone
  configTzTime(TIME_ZONE, "pool.ntp.org", "time.nist.gov");

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    Serial.printf("Current date and time: %04d-%02d-%02dT%02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    Serial.println("Failed to obtain time, will keep trying in the background");
  }

  EPD_7IN5_V2_Init_Fast(); // Fast => Less flicker when displaying the screen
  Serial.println("Startup screen...");
  Paint_SelectImage(buffer);
  Paint_Clear(WHITE);
  Paint_DrawString_EN(70, 50, "Initializing...", &Font16, WHITE, BLACK);

  int yStart = 70;
  for (const char* url : bmpUrls) {
    Paint_DrawString_EN(70, yStart, url, &Font12, WHITE, BLACK);
    yStart += 20;
  }

  EPD_7IN5_V2_Display(buffer);
  EPD_7IN5_V2_Sleep();
}

// Called after each update cycle. Tries to recover the WiFi connection and
// restarts the device if updates keep failing.
static void handleUpdateResult(bool success) {
  if (success) {
    consecutiveFailures = 0;
    return;
  }

  consecutiveFailures++;
  Serial.printf("Update failed (%d in a row)\n", consecutiveFailures);

  if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
    Serial.println("Too many consecutive failures, restarting");
    ESP.restart();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting");
    WiFi.reconnect();
  }
}

void loop()
{
  memset(buffer, 0xFF, bufferSize);

  // Iterate over the image URLs
  const char* url = bmpUrls[currentUrlIndex];
  currentUrlIndex = (currentUrlIndex + 1) % NUM_URLS;

  // Download and decode first; the panel is only powered up when there is
  // something to show.
  DownloadResult result = downloadBMP(url);

  if (result == DOWNLOAD_OK) {
    Serial.println("BMP downloaded successfully!");

    if (isTimeValid()) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      char timeStr[16];
      strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
      DrawString2bpp(buffer, 70, 440, timeStr, BLACK, WHITE, TRANSPARENT2, &Font24);
    }
  } else {
    Serial.println("Failed to download BMP image.");
  }

  if (result != DOWNLOAD_FAILED) {
    EPD_7IN5_V2_Init_4Gray();
    EPD_7IN5_V2_Display_4Gray(buffer);
    EPD_7IN5_V2_Sleep(); // Turn off screen power until next call to EPD_7IN5_V2_Init*().
  }

  handleUpdateResult(result == DOWNLOAD_OK);

  DEV_Delay_ms(REFRESH_INTERVAL_MS);
}

// Draws an HTTP/network error message into the display buffer
static void drawHttpError(const char* url, int httpCode, const char* message) {
  char codeStr[64];
  snprintf(codeStr, sizeof(codeStr), "HTTP error, status code: %d", httpCode);
  DrawString2bpp(buffer, 70, 50, codeStr, BLACK, WHITE, TRANSPARENT2, &Font16);
  DrawString2bpp(buffer, 70, 70, url, BLACK, WHITE, TRANSPARENT2, &Font12);
  DrawString2bpp(buffer, 70, 90, message, BLACK, WHITE, TRANSPARENT2, &Font12);
}

// Downloads a BMP image from the given URL and decodes it into the display buffer.
// Supports uncompressed 8-bit (grayscale) and 24-bit (RGB) BMPs that are no
// larger than the display. Smaller images are centered.
DownloadResult downloadBMP(const char* url) {
  Serial.printf("Downloading BMP from: %s\n", url);

  // Use an explicit client so that HTTPS behaves the same on all core versions.
  // The certificate is not verified; add setCACert() here if that is needed.
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  bool isHttps = strncmp(url, "https://", 8) == 0;
  WiFiClient& client = isHttps ? (WiFiClient&)secureClient : plainClient;

  HTTPClient http;
  http.useHTTP10(true); // Disable chunked transfer encoding for compatibility
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    Serial.println("Invalid URL");
    drawHttpError(url, 0, "Invalid URL");
    return DOWNLOAD_HTTP_ERROR;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d\n", httpCode);

    // Get error message from server/network if available. Only the start of
    // the body is read so a large error page can't exhaust memory.
    char errBuf[128] = "";
    if (httpCode < 0) {
      strlcpy(errBuf, HTTPClient::errorToString(httpCode).c_str(), sizeof(errBuf));
    } else {
      WiFiClient* stream = http.getStreamPtr();
      if (stream != nullptr) {
        size_t n = stream->readBytes((uint8_t*)errBuf, sizeof(errBuf) - 1);
        errBuf[n] = '\0';
      }
    }
    // Keep the message on one line
    for (char* p = errBuf; *p; p++) {
      if (*p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
    }
    if (errBuf[0] == '\0') {
      strlcpy(errBuf, "(No error message)", sizeof(errBuf));
    }
    Serial.printf("Error message: %s\n", errBuf);

    drawHttpError(url, httpCode, errBuf);
    http.end();
    return DOWNLOAD_HTTP_ERROR;
  }

  WiFiClient* stream = http.getStreamPtr();

  // Read BMP file header + BITMAPINFOHEADER (54 bytes)
  uint8_t header[BMP_HEADER_SIZE];
  if (!readExact(stream, header, sizeof(header))) {
    Serial.println("Failed to read BMP header");
    http.end();
    return DOWNLOAD_FAILED;
  }

  // Simple check: should start with 'BM'
  if (header[0] != 'B' || header[1] != 'M') {
    Serial.println("Not a BMP file");
    http.end();
    return DOWNLOAD_FAILED;
  }

  uint32_t dataOffset = readLE32(&header[10]);
  int32_t width = (int32_t)readLE32(&header[18]);
  int32_t height = (int32_t)readLE32(&header[22]);
  uint16_t bitDepth = readLE16(&header[28]);
  uint32_t compression = readLE32(&header[30]);

  // Negative height means rows are stored top-down instead of bottom-up
  bool topDown = height < 0;
  if (topDown) {
    height = -height;
  }

  Serial.printf("Image downloaded: %dx%d, %d-bit color depth%s\n",
                (int)width, (int)height, bitDepth, topDown ? ", top-down" : "");

  // Validate image size, color depth and format
  if ((bitDepth != 8 && bitDepth != 24) || compression != 0 ||
      width <= 0 || width > DISPLAY_WIDTH ||
      height <= 0 || height > DISPLAY_HEIGHT ||
      dataOffset < BMP_HEADER_SIZE || dataOffset > BMP_HEADER_SIZE + MAX_HEADER_SKIP) {
    Serial.println("Unsupported image size, color depth or format");
    Serial.printf("Expected: max %dx%d, uncompressed 8- or 24-bit color depth\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    Serial.printf("Got: compression %u, data offset %u\n", (unsigned)compression, (unsigned)dataOffset);
    http.end();
    return DOWNLOAD_FAILED;
  }

  // Skip to image data (skip palette if present)
  if (!skipBytes(stream, dataOffset - BMP_HEADER_SIZE)) {
    Serial.println("Failed to skip to image data");
    http.end();
    return DOWNLOAD_FAILED;
  }

  bool ok = decodeBMPImageData(stream, bitDepth, width, height, topDown);
  http.end();

  if (!ok) {
    Serial.println("Image data incomplete, keeping current screen content");
    return DOWNLOAD_FAILED;
  }
  return DOWNLOAD_OK;
}

// Map an 8-bit grayscale value to a 2-bit display level (8-bit BMPs)
static uint8_t quantize8Bit(uint8_t pixel, int x, int y) {
  // Never brighten black pixels with dithering
  if (pixel < DITHERING_BLACK_LEVEL) {
    return 0; // Force black, no dithering
  }
  // Apply ordered dithering for non-black pixels
  int ditheredPixel = pixel + (BAYER_MATRIX[y % 4][x % 4] * 16);
  if (ditheredPixel > 255) ditheredPixel = 255;
  return ditheredPixel >> 6; // Divide 0-255 into 0-3
}

// Map an 8-bit grayscale value to a 2-bit display level (24-bit BMPs)
static uint8_t quantize24Bit(uint8_t gray, int x, int y) {
  // Never brighten black pixels with dithering
  if (gray < DITHERING_BLACK_LEVEL) {
    return 0; // Force black, no dithering
  }
  // Apply ordered dithering for non-black pixels
  int ditheredGray = gray + (BAYER_MATRIX[y % 4][x % 4] * 4);
  if (ditheredGray > 255) ditheredGray = 255;

  // Map to 2-bit level (0-3) with dithering
  if (ditheredGray < 64) return 0;
  if (ditheredGray < 128) return 1;
  if (ditheredGray < 212) return 2;
  return 3;
}

// Reads the pixel rows of an 8- or 24-bit BMP from the stream and draws them
// into the display buffer. The image is centered horizontally and vertically
// (shifted down by IMAGE_Y_OFFSET when there is room). Returns false if the
// stream ended before all rows were received.
bool decodeBMPImageData(WiFiClient *stream, int bitDepth, int width, int height, bool topDown)
{
  const int bytesPerPixel = bitDepth / 8;
  const int rowSize = ((width * bytesPerPixel + 3) / 4) * 4; // Rows are padded to 4 bytes
  static uint8_t row[((DISPLAY_WIDTH * 3 + 3) / 4) * 4];     // Large enough for any accepted image

  int xStart = (DISPLAY_WIDTH - width) / 2;
  int yStart = (DISPLAY_HEIGHT - height) / 2 + IMAGE_Y_OFFSET;
  if (yStart > DISPLAY_HEIGHT - height) {
    yStart = DISPLAY_HEIGHT - height;
  }

  for (int i = 0; i < height; i++)
  {
    if (!readExact(stream, row, rowSize)) {
      Serial.printf("Unexpected end of image data at row %d of %d\n", i, height);
      return false;
    }

    // Bottom-up BMPs store the last row first
    int y = topDown ? i : height - 1 - i;
    int pixelY = yStart + y;

    for (int x = 0; x < width; x++)
    {
      int pixelX = xStart + x;
      uint8_t level;

      if (bitDepth == 8) {
        level = quantize8Bit(row[x], pixelX, pixelY);
      } else {
        // BMP stores in BGR order
        uint8_t b = row[x * 3 + 0];
        uint8_t g = row[x * 3 + 1];
        uint8_t r = row[x * 3 + 2];
        // Convert to grayscale (simple average)
        uint8_t gray = (uint16_t(r) + uint16_t(g) + uint16_t(b)) / 3;
        level = quantize24Bit(gray, pixelX, pixelY);
      }

      SetPixel2bpp(buffer, pixelX, pixelY, level);
    }
  }
  return true;
}
