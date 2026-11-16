# Waveshare75-ImageDisplay

A C++ project for ESP32 microcontrollers to download and display images using HTTP.

The main motivation for this project was to display Home Assistant dashboard data on Waveshare 7.5" e-paper displays, but other images are also possible.

Currently, only images in BMP format are supported.

<p align="center">
  <img src="content/frame.png" alt="Dashboard Frame" style="border:1px solid #000;"/>
</p>




## Overview

This project provides firmware for ESP32 boards to create an always-on dashboard that displays Home Assistant data on Waveshare e-paper displays by downloading BMP files with the dashboard in image format.

The code is based on the [demo code provided by Waveshare](https://github.com/waveshareteam/e-Paper), and modified to work with the specific use case of downloading screen shot images from Home Assistant and displaying them on the e-paper display.

The images are generated in **Home Assistant** using the [Home Assistant Lovelace Kindle Screensaver](https://github.com/sibbl/hass-lovelace-kindle-screensaver) plugin that creates a scheduled job that
takes screenshots of the dashboards and saves them to BMP files. These can then be downloaded to the ESP32 via the local network and displayed on the e-paper display. The reason for this setup is that the ESP32 does not have enough processing power to render the Home Assistant dashboards directly, so we rely on Home Assistant to generate the images. 

# Requirements

## Hardware
- **Microcontroller**: ESP32 development board
  - I used the [Waveshare Universal e-Paper Raw Panel Driver Board](https://www.waveshare.com/e-paper-esp32-driver-board.htm)
- **Display**: Waveshare 7.5" e-paper display with black + 2 gray levels
  - Amazon: [Waveshare 7.5 Inch E-Paper Raw Display Panel V2 800x480 Resolution](https://www.amazon.se/Waveshare-Resolution-Electronic-Controller-Communicating/dp/B075R69T93/)
- **Power**: USB power supply or battery pack
- **Connectivity**: WiFi connection for Home Assistant access
- Optional: A **picture frame** and passe-partout to make the display look more like a real device.
  - I used the [IKEA Lomviken](https://www.ikea.com/se/sv/p/lomviken-ram-svart-70518202/) 13x18 cm frame which looks pretty good. Unfortunately the passe-partout hides the edges of the display, so I needed to make sure that there are empty margins around display contents. 


## Software

- **PlatformIO**: For building and uploading the firmware


## Project Structure

```
include/
└── config.h              # User configuration (URLs, refresh interval, time zone, ...)
src/
├── main.cpp              # Main application code
├── EPD.h                 # E-paper display interface
├── GUI_Paint.cpp         # Graphics and drawing utilities
├── DEV_Config.cpp        # Device configuration (pins, SPI)
├── font*.cpp, fonts.h    # Font definitions for display
└── utility/              # Display driver implementations
```

## Getting Started

1. **Install PlatformIO**: Follow the [PlatformIO installation guide](https://platformio.org/install)

2. **Clone the Repository**:
   ```bash
   git clone https://github.com/emilast/Waveshare75-ImageDisplay.git
   cd Waveshare75-ImageDisplay
   ```

3. **Configure the Project**:
   - Edit `platformio.ini` to match your board configuration
   - Edit `include/config.h` to set the image URLs and other settings (see [Configuration](#configuration))
   - Set up the Home Assistant screenshot tool so that it serves BMP images

4. **Build and Upload**:
   ```bash
   pio run --target upload
   ```

5. **Connect to WiFi**: WiFi credentials are not stored in the code. On first
   start (or when the saved network can't be reached), the device opens its
   own WiFi access point, named by `NETWORK_NAME` in `include/config.h`
   (`Waveshare75-HA-dashboard` by default), and shows its name on the display.
   Connect to it with a phone or laptop, and a setup page opens (otherwise
   browse to `http://192.168.4.1`). Select your network and enter the password.
   The device saves the settings and connects automatically from then on.
   If nobody completes the setup within 3 minutes, the device restarts and
   tries the saved network again.

## Configuration

All user settings are in `include/config.h`:

| Setting | Description |
| --- | --- |
| `NETWORK_NAME` | Name of the setup access point used to configure WiFi |
| `bmpUrls` | Image URLs to cycle through, one per refresh |
| `REFRESH_INTERVAL_MS` | Time between screen updates |
| `TIME_ZONE` | POSIX time zone string for the clock overlay |
| `NTP_SERVER_1`, `NTP_SERVER_2` | Time servers |
| `CLOCK_X`, `CLOCK_Y` | Position of the clock overlay |
| `IMAGE_Y_OFFSET` | Downward shift for images smaller than the display |
| `DITHERING_BLACK_LEVEL` | Pixels darker than this are drawn solid black |

Images must be uncompressed 8-bit (grayscale) or 24-bit BMP files, at most
800 pixels wide. Smaller images are centered on the display. Images taller
than 480 pixels are centered vertically and cropped at the top and bottom.


## Building

This project uses PlatformIO for building and deployment:

```bash
# Build the project
pio run

# Upload to device
pio run --target upload

# Monitor serial output
pio device monitor
```

## Troubleshooting

- **Display not working**: Check wiring connections and ensure correct display model is selected
- **WiFi connection issues**: Connect to the setup access point (see step 5 above) to re-enter the network settings. Serial output (115200 baud) shows connection and download errors.
- **Build errors**: Ensure PlatformIO is properly installed and dependencies are resolved

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Waveshare for the e-paper display hardware and [drivers](https://github.com/waveshareteam/e-Paper)
- PlatformIO for the development framework
- Home Assistant community for inspiration

