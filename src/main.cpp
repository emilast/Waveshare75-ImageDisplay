/* Includes ------------------------------------------------------------------*/
#include "DEV_Config.h"
#include "EPD.h"
#include "GUI_Paint.h"
#include <stdlib.h>

#include <WiFiManager.h>
#include <HTTPClient.h>

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

// Brightness threshold for dithering - pixels darker than this will be forced to black without dithering
static const int DITHERING_BLACK_LEVEL = 16;


int currentUrlIndex = 0;

uint8_t* buffer = nullptr; // Will be allocated dynamically
size_t bufferSize = 0;

bool downloadBMP(const char *url);
void handle8BitImageData(int width, WiFiClient *stream);
void handle24BitImageData(int width, int height, WiFiClient *stream, int x, int y, uint8_t* buffer, size_t bufferSize);

// Set pixel in 2bpp buffer
// x, y = pixel coordinate
// color = 0=black, 1=dark gray, 2=light gray, 3=white
void SetPixel2bpp(uint8_t* buffer, int width, int x, int y, uint8_t color) {
  int index = (y * width + x);
  int byteIndex = index / 4;          // 4 pixels per byte
  int shift = (3 - (index % 4)) * 2;  // which 2-bit field
  buffer[byteIndex] &= ~(0x3 << shift);       // clear bits
  buffer[byteIndex] |= ((color & 0x3) << shift); // set color
}

// Draw a string with any font size from sFONT
void DrawString2bpp(uint8_t* buffer, int width, int x, int y, 
      const char* text, uint8_t textColor, uint8_t bgColor, uint8_t transparentBg,
      sFONT* font) {
  int charWidth = font->Width;
  int charHeight = font->Height;
  int bytesPerRow = (charWidth + 7) / 8;
  int bytesPerChar = bytesPerRow * charHeight;

  // log bytes info, including width and height, bytesPerRow
  // Serial.printf("Character: %c, Bytes per char: %d, Width: %d, Height: %d, Bytes per row: %d\n", *text, bytesPerChar, charWidth, charHeight, bytesPerRow);

  while (*text) {
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
        SetPixel2bpp(buffer, width, x + col, y + row, textColor);
      } else {
        if (useBgPixel) {
          SetPixel2bpp(buffer, width, x + col, y + row, bgColor);
        }
      }
    }
  }
  x += charWidth; // next character
  text++;
  }
}

/* Entry point ----------------------------------------------------------------*/
void setup()
{
  DEV_Module_Init();

  // Startup message
  EPD_7IN5_V2_Init_Fast(); // Fast => Less flicker when displaying the screen
  Paint_SelectImage(buffer);
  Paint_Clear(WHITE);

  // Allocate image buffer dynamically
  bufferSize = DISPLAY_WIDTH * DISPLAY_HEIGHT / 4;
  buffer = (uint8_t*)malloc(bufferSize);
  if (buffer == nullptr) {
    Serial.println("Failed to allocate buffer memory!");
    ESP.restart();
  }
  memset(buffer, 0xFF, bufferSize);
  Paint_NewImage(buffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, WHITE);
  Serial.printf("Allocated %d bytes for display buffer\n", bufferSize);

  // WiFi connection via WifiManager
  Paint_DrawString_EN(70, 50, "Connecting to Wifi network...", &Font16, WHITE, BLACK);
  Paint_DrawString_EN(70, 90, "In case of failure, connect to the following WiFI:", &Font12, WHITE, BLACK);
  Paint_DrawString_EN(90, 130, NETWORK_NAME, &Font16, WHITE, BLACK);
  Paint_DrawString_EN(70, 170, "to configure network settings.", &Font12, WHITE, BLACK);

  EPD_7IN5_V2_Display(buffer); // Offsetfel

  WiFiManager wm;
  bool res = wm.autoConnect(NETWORK_NAME);
  if (!res) {
    Serial.println("Failed to connect to WiFi");
    ESP.restart();
  }
  Serial.println("Connected to WiFi");


  // Get current time from time server
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  
  // Set timezone to CEST (Central European Summer Time)
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("Failed to obtain time");
  }

  Serial.printf("Current date and time: %04d-%02d-%02dT%02d:%02d:%02d\n",
                timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  EPD_7IN5_V2_Init_Fast(); // Fast => Less flicker when displaying the screen
  printf("Startup screen...\r\n");
  Paint_SelectImage(buffer);
  Paint_Clear(WHITE);
  Paint_DrawString_EN(70, 50, "Initializing...", &Font16, WHITE, BLACK);

  int yStart = 70;
  for (const char* url : bmpUrls) {
    Paint_DrawString_EN(70, yStart, url, &Font12, WHITE, BLACK);
    yStart += 20;
  }

  EPD_7IN5_V2_Display(buffer); // Offsetfel
  EPD_7IN5_V2_Sleep();
}

void loop()
{
  EPD_7IN5_V2_Init_4Gray();

  memset(buffer, 0xFF, bufferSize);

  Paint_SelectImage(buffer);

  // Iterate over
  const char* url = bmpUrls[currentUrlIndex];
  currentUrlIndex = (currentUrlIndex + 1) % NUM_URLS;

  if (downloadBMP(url)) {
      printf("BMP downloaded successfully!\n");
      // Get current time
      time_t now = time(nullptr);
      struct tm* timeinfo = localtime(&now);
      char timeStr[16];
      strftime(timeStr, sizeof(timeStr), "%H:%M", timeinfo);

      // DrawString2bpp(buffer, DISPLAY_WIDTH, 70, 440, timeStr, BLACK, GRAY3, TRANSPARENT, &Font24);
      DrawString2bpp(buffer, DISPLAY_WIDTH, 70, 440, timeStr, BLACK, WHITE, TRANSPARENT2, &Font24);

      EPD_7IN5_V2_Display_4Gray(buffer);
      EPD_7IN5_V2_Sleep(); // Turn off screen power until next call to EPD_7IN5_V2_Init*().
  }
    else {
      printf("Failed to download BMP image.\n");
  }

  DEV_Delay_ms(60000);
  // delay(60000);
}

// Downloads a BMP image from the given URL and decodes it into the display buffer.
// Supports 8-bit (grayscale) and 24-bit (RGB) BMPs with dimensions matching the display.
bool downloadBMP(const char* url) {
  Serial.printf("Downloading BMP from: %s\n", url);

  HTTPClient http;
  http.useHTTP10(true); // Disable chunked transfer encoding for compatibility
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d\n", httpCode);

    // Get error message from server/network if available
    String errorMsg = http.getString();
    if (errorMsg.length() == 0) {
      errorMsg = "(No error message)";
    }

    // Show error message on screen
    char codeStr[64];
    snprintf(codeStr, sizeof(codeStr), "HTTP error, status code: %d", httpCode);
    DrawString2bpp(buffer, DISPLAY_WIDTH, 70, 50, codeStr, BLACK, WHITE, TRANSPARENT2, &Font16);

    DrawString2bpp(buffer, DISPLAY_WIDTH, 70, 70, url, BLACK, WHITE, TRANSPARENT2, &Font12);

    // Truncate errorMsg if too long for display
    char errBuf[128];
    strncpy(errBuf, errorMsg.c_str(), sizeof(errBuf) - 1);
    errBuf[sizeof(errBuf) - 1] = '\0';
    DrawString2bpp(buffer, DISPLAY_WIDTH, 70, 90, errBuf, BLACK, WHITE, TRANSPARENT2, &Font12);

    EPD_7IN5_V2_Display_4Gray(buffer);

    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();

  // Read BMP header (54 bytes)
  uint8_t header[54];
  stream->readBytes(header, 54);

  // Simple check: should start with 'BM'
  if (header[0] != 'B' || header[1] != 'M') {
    Serial.println("Not a BMP file");
    http.end();
    return false;
  }

  int dataOffset = *(int*)&header[10];
  int width = *(int*)&header[18];
  int height = *(int*)&header[22];
  uint16_t bitDepth = *(uint16_t*)&header[28];

  Serial.printf("Image downloaded: %dx%d, %d-bit color depth\n", width, height, bitDepth);

  // Validate image size and color depth
  if ((bitDepth != 8 && bitDepth != 24)) {
    Serial.println("Incorrect image size or color depth");
    Serial.printf("Expected: %dx%d, 8- or 24-bit color depth\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    Serial.printf("Data offset: %d\n", dataOffset);
    http.end();
    return false;
  }

  // Skip to image data (skip palette if present)
  int skip = dataOffset - 54;
  while (skip-- > 0) stream->read(); // Skip extra palette data

  // Decode image data into buffer
  if (bitDepth == 8) {
    handle8BitImageData(width, stream);
  } else if (bitDepth == 24) {
    handle24BitImageData(width, height, stream, 
      (DISPLAY_WIDTH-width)/2,
      (DISPLAY_HEIGHT-height)/2 + 20,
      buffer, bufferSize);
  }

  http.end();
  return true;
}
void handle8BitImageData(int width, WiFiClient *stream)
{
  // 4x4 Bayer ordered dithering matrix (0-15)
  const uint8_t bayerMatrix[4][4] = {
    {0,  8,  2,  10},
    {12, 4,  14, 6},
    {3,  11, 1,  9},
    {15, 7,  13, 5}
  };

  int rowSize = ((width + 3) / 4) * 4;
  uint8_t row[rowSize];

  for (int y = DISPLAY_HEIGHT - 1; y >= 0; y--)
  {
    stream->readBytes(row, rowSize);
    for (int x = 0; x < DISPLAY_WIDTH; x++)
    {
      if (x < width) { // Bounds check
        uint8_t pixel = row[x];     // 0–255
        
        uint8_t level;
        
        // Never brighten black pixels with dithering
        if (pixel < DITHERING_BLACK_LEVEL) {
          level = 0; // Force black, no dithering
        } else {
          // Apply ordered dithering for non-black pixels
          uint8_t ditherThreshold = bayerMatrix[y % 4][x % 4];
          int ditheredPixel = pixel + (ditherThreshold * 16);
          if (ditheredPixel > 255) ditheredPixel = 255;
          
          level = ditheredPixel >> 6; // Divide 0-255 into 0-3
        }

        int index = y * DISPLAY_WIDTH + x;
        int byteIndex = index / 4;
        int shift = (3 - (index % 4)) * 2;

        buffer[byteIndex] &= ~(0x3 << shift);
        buffer[byteIndex] |= (level << shift);
      }
    }
  }
}


void handle24BitImageData(int width, int height, WiFiClient *stream, int xStart, int yStart, uint8_t* buffer, size_t bufferSize)
{
  // 4x4 Bayer ordered dithering matrix (0-15)
  const uint8_t bayerMatrix[4][4] = {
    {0,  8,  2,  10},
    {12, 4,  14, 6},
    {3,  11, 1,  9},
    {15, 7,  13, 5}
  };

  int rowSize = ((width * 3 + 3) / 4) * 4;
  uint8_t row[rowSize];

  memset(buffer, 0xFF, bufferSize);

  for (int y = height - 1; y >= 0; y--)
  {
    // if (y % 10 == 0) printf("Processing row %d. Free heap: %d\n", y, ESP.getFreeHeap());
  
    stream->readBytes(row, rowSize);

    for (int x = 0; x < width; x++)
    {
      // BMP stores in BGR order
      uint8_t b = row[x * 3 + 0];
      uint8_t g = row[x * 3 + 1];
      uint8_t r = row[x * 3 + 2];
      // Convert to grayscale (simple average)
      uint8_t gray = (uint16_t(r) + uint16_t(g) + uint16_t(b)) / 3;
      
      int pixelX = xStart + x;
      int pixelY = yStart + y;
      
      // Bounds check: only write if pixel is within display area
      if (pixelX >= 0 && pixelX < DISPLAY_WIDTH && pixelY >= 0 && pixelY < DISPLAY_HEIGHT) {
        uint8_t level;
        
        // Never brighten black pixels with dithering
        if (gray < DITHERING_BLACK_LEVEL) {
          level = 0; // Force black, no dithering
        } else {
          // Apply ordered dithering for non-black pixels
          uint8_t ditherThreshold = bayerMatrix[pixelY % 4][pixelX % 4];
          int ditheredGray = gray + (ditherThreshold * 4);
          if (ditheredGray > 255) ditheredGray = 255;
          
          // Map to 2-bit level (0-3) with dithering
          if (ditheredGray < 64) {
            level = 0;
          } else if (ditheredGray < 128) {
            level = 1;
          } else if (ditheredGray < 212) {
            level = 2;
          } else {
            level = 3;
          }
        }

        int index = pixelY * DISPLAY_WIDTH + pixelX;
        int byteIndex = index / 4;
        int shift = (3 - (index % 4)) * 2;

        buffer[byteIndex] &= ~(0x3 << shift);
        buffer[byteIndex] |= (level << shift);
      }
    }
  }
}

