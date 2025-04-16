# DMXThing

A simple DMX controller for LED Par fixtures. Built for the M5Stack CoreS3 with DMX Base. Rocking a web interface, Art-Net support, and some simple scenes.

This is the result of an evening of fun with AI (aka vibe coding). It's purpose is to control some LED Par fixtures on house parties or similar situations where you just want some basic light effects/color control.

*NOTE: This is definitely not meant for anything in production, I'm just sharing this outcome for fun!*

![DMXThing](https://github.com/arpiecodes/dmxthing/raw/main/images/dmx-thing.png)

![WebInterface](https://github.com/arpiecodes/dmxthing/raw/main/images/web-interface.png)

## Features

- **DMX Control**: Control up to 512 DMX channels (one universe)
- **LCD Touchscreen Control**: Basic control ability on the S3's LCD
- **Web Interface**: Web UI accessible from any device
- **Art-Net Support**: Passthrough mode for Art-Net control (untested!)
- **Setting Persistence**: Save default boot settings in persistent storage
- **WiFi Configuration**: Easy WiFi setup with fallback to AP mode
- **Real-time Updates**: Live feedback of base DMX channels and device status

Future plans:

- Implement some form of beat matching/BPM detection to control the speed of scene transitions using the S3's built-in microphones
- Consider building in support for managing channel mapping from Web UI
- Advanced fixture support (e.g. moving heads, with moves, gobo's, etc)

## Channel Mapping

Used this project with some cheap Chinese LED Par fixtures (14X6W).

The layout they use is;

Channel 1: Dimmer
Channel 2: Red
Channel 3: Green
Channel 4: Blue
Channel 5: White
Channel 6: Strobe
Channel 7: Function (not used)
Channel 8: Function speed (not used)

You can easily change the channel layout to match your lights by changing the variables in the source code.

## Hardware Requirements

- M5Stack CoreS3 (or any ESP32 if you adjust the code a bit)
- M5Stack DMX Base (or compatible DMX interface)

## Installation

1. Clone this repository:
   ```bash
   git clone https://github.com/arpiecodes/dmxthing.git
   cd dmxthing
   ```

2. Install PlatformIO (if not already installed):
   ```bash
   pip install platformio
   ```

3. Upload the firmware:
   ```bash
   pio run -t upload
   ```

4. Upload the web interface:
   ```bash
   pio run -t uploadfs
   ```

## Usage

1. Power on the M5Stack CoreS3
2. Connect to the WiFi network:
   - Default SSID: `DMXController`
   - Default password: `dmx12345`
3. Open a web browser and navigate to the device's IP address (usually 192.168.4.1)
4. Configure your WiFi credentials
5. Use the web interface (or the LCD touch screen) to control your lights
6. Have fun!

## Project Structure

```
dmxthing/
├── data/           # Web interface files
├── src/            # Firmware source code
├── platformio.ini  # PlatformIO configuration
└── README.md       # This file
```

## Dependencies

- M5Unified
- esp_dmx
- ArduinoJson
- ESPAsyncWebServer
- Preferences
- SPIFFS
- WiFi

## License

This project is licensed under the MIT License and comes without any warranties or guarantees of its workings.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.
