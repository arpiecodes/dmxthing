#include <M5Unified.h>
#include <esp_dmx.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <WiFiUdp.h>

// WiFi credentials in AP Mode (fallback if no other WiFi is available)
const char* ssid = "DMXController";
const char* password = "dmx12345";

// Forward declarations
void startScene(int scene);
void stopScene();
void resetAll();
void drawButtons();
void updateScene();
void hsvToRgb(float h, float s, float v, float& r, float& g, float& b);
void setColor(uint8_t r, uint8_t g, uint8_t b, int fixture = 0);
void setDimmer(uint8_t value, int fixture = 0);
void notifyClients();
void updateDMX();
void startTransition(int fixture);
void updateTransition();
void setChannelValue(int channel, uint8_t value, int fixture = 0);
void setRedManual();
void setGreenManual();
void setBlueManual();

// DMX pins
#define DMX_TX_PIN 7
#define DMX_RX_PIN 10
#define DMX_EN_PIN 6

// DMX configuration
dmx_port_t dmxPort = 1;
#define DMX_PACKET_SIZE 512

#define MAX_FIXTURES 64
#define MAX_DMX_CHANNELS 512

// Channel definitions (fixed mapping)
#define CHANNEL_DIMMER 1
#define CHANNEL_RED 2
#define CHANNEL_GREEN 3
#define CHANNEL_BLUE 4
#define CHANNEL_WHITE 5
#define CHANNEL_STROBE 6
#define CHANNEL_FUNCTION 7
#define CHANNEL_SPEED 8

// Channel state matrix (per fixture)
struct ChannelState {
    uint8_t currentValue;
    uint8_t targetValue;
    bool needsUpdate;
};
ChannelState channelStates[MAX_DMX_CHANNELS] = {0};

// DMX buffer for all possible channels
uint8_t dmxData[MAX_DMX_CHANNELS] = {0};

// Scene timing
#define TRANSITION_STEPS 50  // Number of steps for interpolation
unsigned long lastSceneUpdate = 0;
unsigned long lastRainbowUpdate = 0;
int transitionSpeed = 250;  // Default to 250ms for smoother transitions
const int CHASE_SPEED_MULTIPLIER = 3;  // Chase scene runs 3x slower than rainbow

// Rainbow scene update interval (ms)
const int RAINBOW_UPDATE_INTERVAL = 10;

// Touch screen buttons
#define BUTTON_WIDTH 80
#define BUTTON_HEIGHT 40
#define BUTTON_MARGIN 10

struct Button {
    int x;
    int y;
    const char* label;
    void (*action)();
};

void startSceneWrapper1() { startScene(1); }
void startSceneWrapper2() { startScene(2); }
void resetAllWrapper() { resetAll(); }

Button buttons[] = {
    {10, 10, "Rainbow", startSceneWrapper1},
    {110, 10, "Chase", startSceneWrapper2},
    {210, 10, "Reset", resetAllWrapper},
    {10, 60, "Red", setRedManual},
    {110, 60, "Green", setGreenManual},
    {210, 60, "Blue", setBlueManual}
};

// Global variables
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Scene variables
bool isRunningScene = false;
int currentScene = 0;
float sceneHue = 0.0;
int chasePosition = 0;

// Per-fixture transition state
int transitionStep[MAX_FIXTURES] = {0};
bool isTransitioning[MAX_FIXTURES] = {false};

// Manual override tracking
bool manualOverride = false;

// --- Slider UI variables ---
#define SLIDER_WIDTH 200
#define SLIDER_HEIGHT 20
#define SLIDER_MARGIN 20
#define SLIDER_HANDLE_RADIUS 10
#define SLIDER_LABEL_HEIGHT 16

// Dimmer slider
int dimmerSliderX = 10;
int dimmerSliderY = 130;
int dimmerValue = 0;

// Transition speed slider
int speedSliderX = 10;
int speedSliderY = 180;
int speedValue = 500;

// --- Multi-fixture support ---
int fixtureCount = 1; // Default 1 fixture

// --- LCD fixture count buttons ---
// Move + and - buttons to the right of the sliders
#define FIXTURE_BTN_X (dimmerSliderX + SLIDER_WIDTH + 30)
#define FIXTURE_BTN_WIDTH 40
#define FIXTURE_BTN_HEIGHT 40
#define FIXTURE_PLUS_Y dimmerSliderY
#define FIXTURE_MINUS_Y speedSliderY

Preferences prefs;

#define NUM_CHANNELS 8

// Add a global variable to track the last measured DMX update interval
unsigned long lastDMXInterval = 25;
unsigned long lastDMXUpdateTime = 0;

WiFiUDP artnetUDP;
const int ARTNET_PORT = 6454;
bool artnetPassthrough = false;
unsigned long lastArtnetPacket = 0;
uint8_t artnetBuffer[530]; // Enough for Art-Net DMX packet

void resetAll() {
    // Stop any running scene
    isRunningScene = false;
    currentScene = 0;
    // Reset all channels to 0 except dimmer
    for (int f = 0; f < fixtureCount; ++f) {
        for (int i = 0; i < 8; i++) {
            setChannelValue(i + 1, 0, f);  // Set all channels to 0
        }
    }
    manualOverride = false;
    dimmerValue = 0;
    notifyClients();
    drawButtons();
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            data[len] = 0;
            String message = (char*)data;
            Serial.println("Received WebSocket message: " + message);

            // Parse JSON
            StaticJsonDocument<200> doc;
            DeserializationError error = deserializeJson(doc, message);
            if (error) {
                Serial.println("Failed to parse JSON");
                return;
            }

            // Handle different commands
            if (doc.containsKey("color")) {
                manualOverride = true;
                uint8_t r = doc["color"]["r"];
                uint8_t g = doc["color"]["g"];
                uint8_t b = doc["color"]["b"];
                for (int f = 0; f < fixtureCount; ++f) {
                    setChannelValue(CHANNEL_RED, r, f);
                    setChannelValue(CHANNEL_GREEN, g, f);
                    setChannelValue(CHANNEL_BLUE, b, f);
                }
            } else if (doc.containsKey("dimmer")) {
                uint8_t newValue = doc["dimmer"];
                if (dimmerValue != newValue) {
                    dimmerValue = newValue;
                    for (int f = 0; f < fixtureCount; ++f) setDimmer(dimmerValue, f);
                }
            } else if (doc.containsKey("white")) {
                manualOverride = true;
                uint8_t value = doc["white"];
                for (int f = 0; f < fixtureCount; ++f) setChannelValue(CHANNEL_WHITE, value, f);
            } else if (doc.containsKey("strobe")) {
                uint8_t value = doc["strobe"];
                for (int f = 0; f < fixtureCount; ++f) setChannelValue(CHANNEL_STROBE, value, f);
            } else if (doc.containsKey("function")) {
                manualOverride = true;
                uint8_t value = doc["function"];
                for (int f = 0; f < fixtureCount; ++f) setChannelValue(CHANNEL_FUNCTION, value, f);
            } else if (doc.containsKey("speed")) {
                manualOverride = true;
                uint8_t value = doc["speed"];
                for (int f = 0; f < fixtureCount; ++f) setChannelValue(CHANNEL_SPEED, value, f);
            } else if (doc.containsKey("transitionSpeed")) {
                transitionSpeed = doc["transitionSpeed"];
                Serial.println("Setting transition speed to: " + String(transitionSpeed) + "ms");
            } else if (doc.containsKey("scene")) {
                int scene = doc["scene"];
                startScene(scene);
            } else if (doc.containsKey("stop")) {
                stopScene();
            } else if (doc.containsKey("reset")) {
                resetAll();
            } else if (doc.containsKey("fixtureCount")) {
                int newCount = doc["fixtureCount"];
                if (newCount >= 1 && newCount <= MAX_FIXTURES) { fixtureCount = newCount; }
            } else if (doc.containsKey("saveSettings")) {
                // Save all settings to NVS
                prefs.begin("dmx", false);
                prefs.putUInt("dimmer", dimmerValue);
                prefs.putUInt("fixtures", fixtureCount);
                prefs.putUInt("trSpeed", transitionSpeed);
                prefs.putUInt("scene", currentScene);
                prefs.putUInt("artnet", artnetPassthrough ? 1 : 0);
                for (int f = 0; f < fixtureCount; ++f) {
                    for (int i = 0; i < 8; ++i) {
                        int idx = f * 8 + i;
                        prefs.putUInt((String("ch") + idx).c_str(), channelStates[idx].currentValue);
                    }
                }
                prefs.end();
            } else if (doc.containsKey("artnetPassthrough")) {
                artnetPassthrough = doc["artnetPassthrough"];
            } else if (doc.containsKey("wifiConfig")) {
                String newSsid = doc["wifiConfig"]["ssid"] | "";
                String newPassword = doc["wifiConfig"]["password"] | "";
                prefs.begin("dmx", false);
                prefs.putString("wifiSsid", newSsid);
                prefs.putString("wifiPassword", newPassword);
                prefs.end();
                ESP.restart();
            }
            notifyClients();
            drawButtons();
        }
    }
}

void notifyClients() {
    StaticJsonDocument<200> doc;
    doc["color"]["r"] = channelStates[CHANNEL_RED - 1].currentValue;
    doc["color"]["g"] = channelStates[CHANNEL_GREEN - 1].currentValue;
    doc["color"]["b"] = channelStates[CHANNEL_BLUE - 1].currentValue;
    doc["white"] = channelStates[CHANNEL_WHITE - 1].currentValue;
    doc["dimmer"] = channelStates[CHANNEL_DIMMER - 1].currentValue;
    doc["strobe"] = channelStates[CHANNEL_STROBE - 1].currentValue;
    doc["function"] = channelStates[CHANNEL_FUNCTION - 1].currentValue;
    doc["speed"] = channelStates[CHANNEL_SPEED - 1].currentValue;
    doc["scene"] = currentScene;
    doc["fixtureCount"] = fixtureCount;
    doc["artnetPassthrough"] = artnetPassthrough;
    
    String output;
    serializeJson(doc, output);
    ws.textAll(output);
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(2);
    M5.Display.println("DMX Thing");
    M5.Display.println("Initializing...");

    // Initialize Serial
    Serial.begin(115200);
    
    // Initialize DMX
    dmx_config_t config = DMX_CONFIG_DEFAULT;
    dmx_driver_install(dmxPort, &config, 0);
    dmx_set_pin(dmxPort, DMX_TX_PIN, DMX_RX_PIN, DMX_EN_PIN);
    
    // Clear DMX buffer
    memset(dmxData, 0, DMX_PACKET_SIZE);
    dmx_write(dmxPort, dmxData, DMX_PACKET_SIZE);
    dmx_send(dmxPort, DMX_PACKET_SIZE);
    
    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        M5.Display.println("SPIFFS failed!");
        return;
    }
    
    prefs.begin("dmx", true);

    // Setup WiFi Access Point
    String savedSsid = prefs.getString("wifiSsid", "DMXController");
    String savedPassword = prefs.getString("wifiPassword", "dmx12345");
    artnetPassthrough = prefs.getUInt("artnet", 0) == 1;

    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());

    M5.Display.println("Connecting to WiFi");
    M5.Display.println(savedSsid.c_str());
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 50000) {
        delay(250);
        M5.Display.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        M5.Display.println("");
        M5.Display.println("WiFi Connected!");
        M5.Display.print("IP: ");
        M5.Display.println(WiFi.localIP());
    } else {
        M5.Display.println("");
        M5.Display.println("WiFi connect failed, starting AP mode...");
        WiFi.mode(WIFI_AP);
        bool result = WiFi.softAP(ssid, password);
        if (!result) {
            M5.Display.println("WiFi AP failed!");
            M5.Display.println("Status: " + String(WiFi.status()));
            return;
        }
        M5.Display.println("WiFi AP Started");
        M5.Display.print("SSID: ");
        M5.Display.println(ssid);
        M5.Display.print("Password: ");
        M5.Display.println(password);
        M5.Display.print("IP: ");
        M5.Display.println(WiFi.softAPIP());
    }
    
    // Setup web server
    server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

    if (!artnetPassthrough) {
        dimmerValue = prefs.getUInt("dimmer", 0);
        fixtureCount = prefs.getUInt("fixtures", 1);
        transitionSpeed = prefs.getUInt("trSpeed", 250);
        int savedScene = prefs.getUInt("scene", 0);

        resetAll();

        if (savedScene != 0) {
            startScene(savedScene);
        } else {
            for (int f = 0; f < fixtureCount; ++f) {
                for (int i = 0; i < 8; ++i) {
                    int idx = f * 8 + i;
                    int value = prefs.getUInt((String("ch") + idx).c_str(), 0);
                    channelStates[idx].currentValue = value;
                    channelStates[idx].targetValue = value;
                    channelStates[idx].needsUpdate = false;
                }
            }
        }

        for (int f = 0; f < fixtureCount; ++f) setDimmer(dimmerValue, f);
    }

    prefs.end();

    // Setup WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.begin();

    // Setup artnet passthrough
    artnetUDP.begin(ARTNET_PORT);

    // draw buttons and notify clients
    drawButtons();
    notifyClients();
}

void loop() {
    M5.update();

    unsigned long currentMillis = millis();
    static unsigned long lastNotify = 0;
    if (currentMillis - lastNotify >= 1000) {
        notifyClients();
        lastNotify = currentMillis;
    }

    if (artnetPassthrough) {
        int packetSize = artnetUDP.parsePacket();
        if (packetSize > 0) {
            artnetUDP.read(artnetBuffer, sizeof(artnetBuffer));
            // Check Art-Net header
            if (memcmp(artnetBuffer, "Art-Net\0", 8) == 0 && artnetBuffer[8] == 0x00 && artnetBuffer[9] == 0x50) {
                // OpCode 0x5000 = ArtDMX
                uint16_t dmxLen = artnetBuffer[17] << 8 | artnetBuffer[18];
                if (dmxLen > 512) dmxLen = 512;
                // Copy DMX data to output buffer (start at offset 18)
                memset(dmxData, 0, sizeof(dmxData));
                memcpy(dmxData + 1, artnetBuffer + 18, dmxLen); // DMX channel 1-based
                int dmxPacketSize = dmxLen + 1;
                dmx_write(dmxPort, dmxData, dmxPacketSize);
                dmx_send(dmxPort, dmxPacketSize);
                lastArtnetPacket = millis();
            }
        }
        delay(5);
        return; // Skip rest of loop if in Art-Net mode
    }

    // Handle touch input
    auto t = M5.Touch.getDetail();
    if (t.wasPressed() || t.isPressed()) {
        // Check buttons
        for (const auto& button : buttons) {
            if (t.x >= button.x && t.x <= button.x + BUTTON_WIDTH &&
                t.y >= button.y && t.y <= button.y + BUTTON_HEIGHT) {
                button.action();
                break;
            }
        }
        // Check fixture count +
        if (t.x >= FIXTURE_BTN_X && t.x <= FIXTURE_BTN_X + FIXTURE_BTN_WIDTH &&
            t.y >= FIXTURE_PLUS_Y && t.y <= FIXTURE_PLUS_Y + FIXTURE_BTN_HEIGHT) {
            if (fixtureCount < MAX_FIXTURES) {
                fixtureCount++;
                drawButtons();
            }
        }
        // Check fixture count -
        if (t.x >= FIXTURE_BTN_X && t.x <= FIXTURE_BTN_X + FIXTURE_BTN_WIDTH &&
            t.y >= FIXTURE_MINUS_Y && t.y <= FIXTURE_MINUS_Y + FIXTURE_BTN_HEIGHT) {
            if (fixtureCount > 1) {
                fixtureCount--;
                drawButtons();
            }
        }
        // Check dimmer slider
        if (t.x >= dimmerSliderX && t.x <= dimmerSliderX + SLIDER_WIDTH &&
            t.y >= dimmerSliderY && t.y <= dimmerSliderY + SLIDER_HEIGHT) {
            int newValue = ((t.x - dimmerSliderX) * 255) / (SLIDER_WIDTH - 2 * SLIDER_HANDLE_RADIUS);
            if (newValue < 0) newValue = 0;
            if (newValue > 255) newValue = 255;
            if (dimmerValue != newValue) {
                dimmerValue = newValue;
                for (int f = 0; f < fixtureCount; ++f) setDimmer(dimmerValue, f);
                drawButtons();
            }
        }
        // Check speed slider
        if (t.x >= speedSliderX && t.x <= speedSliderX + SLIDER_WIDTH &&
            t.y >= speedSliderY && t.y <= speedSliderY + SLIDER_HEIGHT) {
            int newSpeed = 10 + ((t.x - speedSliderX) * (1000 - 10)) / (SLIDER_WIDTH - 2 * SLIDER_HANDLE_RADIUS);
            if (newSpeed < 10) newSpeed = 10;
            if (newSpeed > 1000) newSpeed = 1000;
            if (speedValue != newSpeed) {
                speedValue = newSpeed;
                transitionSpeed = speedValue;
                drawButtons();
            }
        }
    }

    // Update scene if active
    if (currentScene != 0) {
        // Calculate effective speed based on scene type
        int effectiveSpeed = transitionSpeed;
        if (currentScene == 2) { // Chase scene
            effectiveSpeed *= CHASE_SPEED_MULTIPLIER;
        }

        if (currentScene == 1) { // Rainbow scene
            // Update color steps more frequently for smoothness
            if (currentMillis - lastRainbowUpdate >= RAINBOW_UPDATE_INTERVAL) {
                updateScene();
                lastRainbowUpdate = currentMillis;
            }
            // But keep transition animation at transitionSpeed
            if (currentMillis - lastSceneUpdate >= transitionSpeed) {
                lastSceneUpdate = currentMillis;
            }
        } else {
            if (currentMillis - lastSceneUpdate >= effectiveSpeed) {
                updateScene();
                lastSceneUpdate = currentMillis;
            }
        }
    }

    // Update transitions
    updateTransition();

    // 40Hz DMX update loop (25ms interval)
    static unsigned long lastDMXUpdate = 0;
    int currentDMXInterval = currentMillis - lastDMXUpdate;
    if (currentDMXInterval >= 25) {
        // Wait for previous DMX packet to be sent
        dmx_wait_sent(dmxPort, DMX_TIMEOUT_TICK);
        
        // Update DMX values from channel states (multi-fixture)
        memset(dmxData, 0, sizeof(dmxData));
        for (int f = 0; f < fixtureCount; ++f) {
            for (int i = 0; i < 8; ++i) {
                int idx = f * 8 + i;
                dmxData[idx + 1] = channelStates[idx].currentValue;
            }
        }
        
        // Write/send only the used part of the buffer
        int dmxPacketSize = fixtureCount * 8 + 1;
        dmx_write(dmxPort, dmxData, dmxPacketSize);
        dmx_send(dmxPort, dmxPacketSize);
        
        lastDMXInterval = currentDMXInterval;
        lastDMXUpdate = currentMillis;
    }
    
    delay(5);
}

void drawSliders() {
    // Dimmer slider
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(dimmerSliderX, dimmerSliderY - SLIDER_LABEL_HEIGHT);
    M5.Display.printf("Dimmer: %d", dimmerValue);
    M5.Display.drawRect(dimmerSliderX, dimmerSliderY, SLIDER_WIDTH, SLIDER_HEIGHT, WHITE);
    int fillWidth = (dimmerValue * SLIDER_WIDTH) / 255;
    if (fillWidth > 0) {
        M5.Display.fillRect(dimmerSliderX, dimmerSliderY, fillWidth, SLIDER_HEIGHT, WHITE);
    }

    // Transition speed slider
    M5.Display.setCursor(speedSliderX, speedSliderY - SLIDER_LABEL_HEIGHT);
    M5.Display.printf("Transition Speed: %d", speedValue);
    M5.Display.drawRect(speedSliderX, speedSliderY, SLIDER_WIDTH, SLIDER_HEIGHT, WHITE);
    int speedFillWidth = ((speedValue - 10) * SLIDER_WIDTH) / (1000 - 10);
    if (speedFillWidth > 0) {
        M5.Display.fillRect(speedSliderX, speedSliderY, speedFillWidth, SLIDER_HEIGHT, WHITE);
    }
}

void drawButtons() {
    M5.Display.fillScreen(BLACK);

    if (artnetPassthrough) {
        M5.Display.setTextColor(WHITE);
        M5.Display.setTextSize(3);
        int centerX = 160;
        int centerY = 80;
        M5.Display.setCursor(centerX - 120, centerY - 30);
        M5.Display.println("ART-NET PASSTRU");
        M5.Display.setTextSize(2);
        String ipStr = (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        M5.Display.setCursor(centerX - 120, centerY + 10);
        M5.Display.printf("SSID: %s", ssid);
        M5.Display.setCursor(centerX - 120, centerY + 40);
        M5.Display.printf("IP: %s", ipStr.c_str());
        M5.Display.setCursor(centerX - 120, centerY + 70);
        M5.Display.printf("Port: 6454 UDP");
        return;
    }

    for (int i = 0; i < 6; ++i) {
        const auto& button = buttons[i];
        if (i < 3) { // Scene buttons: Rainbow, Chase, Stop
            M5.Display.drawRect(button.x, button.y, BUTTON_WIDTH, BUTTON_HEIGHT, WHITE);
            M5.Display.setTextColor(WHITE);
            M5.Display.setTextSize(1);
            M5.Display.setCursor(button.x + 10, button.y + 10);
            M5.Display.println(button.label);
        } else if (i == 3) { // Red
            M5.Display.fillRect(button.x, button.y, BUTTON_WIDTH, BUTTON_HEIGHT, RED);
            M5.Display.setTextColor(WHITE);
            M5.Display.setTextSize(1);
            M5.Display.setCursor(button.x + 10, button.y + 10);
            M5.Display.println(button.label);
        } else if (i == 4) { // Green
            M5.Display.fillRect(button.x, button.y, BUTTON_WIDTH, BUTTON_HEIGHT, GREEN);
            M5.Display.setTextColor(WHITE);
            M5.Display.setTextSize(1);
            M5.Display.setCursor(button.x + 10, button.y + 10);
            M5.Display.println(button.label);
        } else if (i == 5) { // Blue
            M5.Display.fillRect(button.x, button.y, BUTTON_WIDTH, BUTTON_HEIGHT, BLUE);
            M5.Display.setTextColor(WHITE);
            M5.Display.setTextSize(1);
            M5.Display.setCursor(button.x + 10, button.y + 10);
            M5.Display.println(button.label);
        }
    }
    // Draw fixture count controls (just + and - buttons to the right of sliders)
    // + button (right of dimmer slider)
    M5.Display.drawRect(FIXTURE_BTN_X, FIXTURE_PLUS_Y, FIXTURE_BTN_WIDTH, FIXTURE_BTN_HEIGHT, WHITE);
    M5.Display.setCursor(FIXTURE_BTN_X + 10, FIXTURE_PLUS_Y + 10);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(3);
    M5.Display.print("+");
    // - button (right of speed slider)
    M5.Display.drawRect(FIXTURE_BTN_X, FIXTURE_MINUS_Y, FIXTURE_BTN_WIDTH, FIXTURE_BTN_HEIGHT, WHITE);
    M5.Display.setCursor(FIXTURE_BTN_X + 12, FIXTURE_MINUS_Y + 10);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(3);
    M5.Display.print("-");

    // Draw fixture count label left-aligned with the sliders, below the speed slider
    int labelY = speedSliderY + SLIDER_HEIGHT + 30;
    int labelX = dimmerSliderX;
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(WHITE);
    String ssidStr =(WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : ssid;
    String ipStr = (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    M5.Display.setCursor(labelX, labelY);
    M5.Display.printf("SSID: %s | IP: %s | Fixtures: %d", ssidStr.c_str(), ipStr.c_str(), fixtureCount);

    // Draw sliders
    drawSliders();
}

void updateScene() {
    if (isRunningScene && !manualOverride) {
        switch (currentScene) {
            case 1: // Rainbow
                for (int f = 0; f < fixtureCount; ++f) {
                    float baseHue = sceneHue + (float)f / fixtureCount;
                    if (baseHue >= 1.0) baseHue -= 1.0;
                    float r, g, b;
                    hsvToRgb(baseHue, 1.0, 1.0, r, g, b);
                    uint8_t red = r * 255;
                    uint8_t green = g * 255;
                    uint8_t blue = b * 255;
                    channelStates[f * 8 + CHANNEL_RED - 1].targetValue = red;
                    channelStates[f * 8 + CHANNEL_GREEN - 1].targetValue = green;
                    channelStates[f * 8 + CHANNEL_BLUE - 1].targetValue = blue;
                    channelStates[f * 8 + CHANNEL_RED - 1].needsUpdate = true;
                    channelStates[f * 8 + CHANNEL_GREEN - 1].needsUpdate = true;
                    channelStates[f * 8 + CHANNEL_BLUE - 1].needsUpdate = true;
                    if (!isTransitioning[f]) startTransition(f);
                }
                sceneHue += 0.001;
                if (sceneHue >= 1.0) sceneHue = 0.0;
                break;
            case 2: // Chase
                for (int f = 0; f < fixtureCount; ++f) {
                    int chaseStep = (chasePosition + f) % 3;
                    if (chaseStep == 0) {
                        channelStates[f * 8 + CHANNEL_RED - 1].targetValue = 255;
                        channelStates[f * 8 + CHANNEL_GREEN - 1].targetValue = 0;
                        channelStates[f * 8 + CHANNEL_BLUE - 1].targetValue = 0;
                    } else if (chaseStep == 1) {
                        channelStates[f * 8 + CHANNEL_RED - 1].targetValue = 0;
                        channelStates[f * 8 + CHANNEL_GREEN - 1].targetValue = 255;
                        channelStates[f * 8 + CHANNEL_BLUE - 1].targetValue = 0;
                    } else if (chaseStep == 2) {
                        channelStates[f * 8 + CHANNEL_RED - 1].targetValue = 0;
                        channelStates[f * 8 + CHANNEL_GREEN - 1].targetValue = 0;
                        channelStates[f * 8 + CHANNEL_BLUE - 1].targetValue = 255;
                    }
                    channelStates[f * 8 + CHANNEL_RED - 1].needsUpdate = true;
                    channelStates[f * 8 + CHANNEL_GREEN - 1].needsUpdate = true;
                    channelStates[f * 8 + CHANNEL_BLUE - 1].needsUpdate = true;
                    if (!isTransitioning[f]) startTransition(f);
                }
                chasePosition = (chasePosition + 1) % 3;
                break;
        }
    }
}

void startTransition(int fixture) {
    transitionStep[fixture] = 0;
    isTransitioning[fixture] = true;
}

void updateTransition() {
    for (int f = 0; f < fixtureCount; ++f) {
        if (isTransitioning[f] && transitionStep[f] < TRANSITION_STEPS) {
            float progress = (float)transitionStep[f] / TRANSITION_STEPS;
            progress = progress < 0.5 
                ? 2 * progress * progress 
                : 1 - pow(-2 * progress + 2, 2) / 2;
            for (int i = 0; i < 8; i++) {
                if (channelStates[f * 8 + i].needsUpdate) {
                    uint8_t before = channelStates[f * 8 + i].currentValue;
                    channelStates[f * 8 + i].currentValue = channelStates[f * 8 + i].currentValue + 
                        (channelStates[f * 8 + i].targetValue - channelStates[f * 8 + i].currentValue) * progress;
                }
            }
            transitionStep[f]++;
        } else if (isTransitioning[f]) {
            isTransitioning[f] = false;
            for (int i = 0; i < 8; i++) {
                if (channelStates[f * 8 + i].needsUpdate) {
                    channelStates[f * 8 + i].currentValue = channelStates[f * 8 + i].targetValue;
                    channelStates[f * 8 + i].needsUpdate = false;
                }
            }
        }
    }
}

void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    int i = int(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        case 5: r = v, g = p, b = q; break;
    }
}

void startScene(int scene) {
    currentScene = scene;
    isRunningScene = true;
    sceneHue = 0.0;
    chasePosition = 0;
    manualOverride = false; // Reset manual override when starting a scene
    startTransition(0);  // Only start transition when starting a scene
}

void stopScene() {
    currentScene = 0;
    isRunningScene = false;
    startTransition(0);
    notifyClients();
    drawButtons();
}

void setChannelValue(int channel, uint8_t value, int fixture) {
    if (channel >= 1 && channel <= 8 && fixture >= 0 && fixture < MAX_FIXTURES) {
        int idx = fixture * 8 + (channel - 1);
        channelStates[idx].targetValue = value;
        channelStates[idx].currentValue = value;
        channelStates[idx].needsUpdate = false;
    }
}

void setColor(uint8_t r, uint8_t g, uint8_t b, int fixture) {
    setChannelValue(CHANNEL_RED, r, fixture);
    setChannelValue(CHANNEL_GREEN, g, fixture);
    setChannelValue(CHANNEL_BLUE, b, fixture);
}

void setDimmer(uint8_t value, int fixture) { setChannelValue(CHANNEL_DIMMER, value, fixture); }
void setRedManual() { for (int f = 0; f < fixtureCount; ++f) setColor(255, 0, 0, f); manualOverride = true; drawButtons(); notifyClients(); }
void setGreenManual() { for (int f = 0; f < fixtureCount; ++f) setColor(0, 255, 0, f); manualOverride = true; drawButtons(); notifyClients(); }
void setBlueManual() { for (int f = 0; f < fixtureCount; ++f) setColor(0, 0, 255, f); manualOverride = true; drawButtons(); notifyClients(); } 