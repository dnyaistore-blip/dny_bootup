#define DEBUG_SW 1

#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Espalexa.h>
#include <AceButton.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>

// --- Configuration Constants ---
const char* NVS_NAMESPACE = "product_config";
const char* NVS_KEY_IS_CONFIGURED = "is_configured";
const char* DEFAULT_AP_SSID = "MyProduct_Setup";
const char* DEFAULT_AP_PASSWORD = "configureme";
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;
const unsigned long WIFI_CONNECT_DELAY_MS = 30000; // 30 second delay on boot

// GitHub repository and version file configuration
const char* VERSION_URL = "https://dnyaistore-blip.github.io/dny_bootup/version.json";
const int CURRENT_VERSION = 1;

// Pins of Relays (Appliance Control)
const int R1 = 26;
const int R2 = 25;
const int R3 = 33;
const int R4 = 32;

// Pins of Relays (Fan Speed Control)
const int Speed1_PIN = 21;
const int Speed2_PIN = 19;
const int Speed4_PIN = 18;

// Pins of Switches
const int S1 = 32;
const int S2 = 35;
const int S3 = 34;
const int S4 = 39;

// Pins of Fan Regulator Knob (Inputs)
const int F1 = 27;
const int F2 = 14;
const int F3 = 12;
const int F4 = 13;

// --- Global Objects ---
Espalexa espalexa;
WiFiManager wm;
using namespace ace_button;
ButtonConfig btnConfig;

// ACEBUTTON FIX: Initialize with pin and config object pointer.
AceButton button1(S1, &btnConfig);
AceButton button2(S2, &btnConfig);
AceButton button3(S3, &btnConfig);
AceButton button4(S4, &btnConfig);

// ESPALEXA FIX: Declare an array of pointers to hold device objects.
EspalexaDevice* espalexaDevices[5];

// --- Device State Variables ---
int currentFanSpeed = 0;
bool applianceStates[4] = {false, false, false, false};
static bool otaChecked = false;

// --- Function Prototypes ---
void initNVS();
void handleProvisioning();
void startNormalOperation();
boolean connectToStoredWifi();
void espalexaDeviceCallbacks();
void checkForOTAUpdate();
void downloadAndApplyUpdate(const char* url);
void handleFirmwareUpdates();
void announceAlert(const char* message);

void firstLightChanged(uint8_t brightness);
void secondLightChanged(uint8_t brightness);
void thirdLightChanged(uint8_t brightness);
void fourthLightChanged(uint8_t brightness);
void fanChanged(uint8_t brightness);

void updateFanPhysicalState(int speed);
void updateAppliancePhysicalState(int applianceIndex, bool state);

void handleButtonEvent(AceButton* button, uint8_t eventType, uint8_t buttonState);

void checkPhysicalFanSwitches();
void setupOTA();

// --- Setup Function ---
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Device Boot ---");

  // ADDED DELAY: Add a delay before starting any WiFi connection logic
  Serial.printf("Waiting for %d seconds before attempting WiFi connection...\n", WIFI_CONNECT_DELAY_MS / 1000);
  delay(WIFI_CONNECT_DELAY_MS);

  // Initialize NVS for persistent storage
  initNVS();

  // Initialize GPIOs
  pinMode(R1, OUTPUT); digitalWrite(R1, LOW);
  pinMode(R2, OUTPUT); digitalWrite(R2, LOW);
  pinMode(R3, OUTPUT); digitalWrite(R3, LOW);
  pinMode(R4, OUTPUT); digitalWrite(R4, LOW);
  pinMode(Speed1_PIN, OUTPUT); digitalWrite(Speed1_PIN, LOW);
  pinMode(Speed2_PIN, OUTPUT); digitalWrite(Speed2_PIN, LOW);
  pinMode(Speed4_PIN, OUTPUT); digitalWrite(Speed4_PIN, LOW);

  // Configure AceButtons
  btnConfig.setEventHandler(handleButtonEvent);
  pinMode(F1, INPUT_PULLUP);
  pinMode(F2, INPUT_PULLUP);
  pinMode(F3, INPUT_PULLUP);
  pinMode(F4, INPUT_PULLUP);

  // Check if device is configured
  nvs_handle_t nvs_handle;
  nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  int32_t isConfigured = 0;
  esp_err_t err = nvs_get_i32(nvs_handle, NVS_KEY_IS_CONFIGURED, &isConfigured);
  nvs_close(nvs_handle);

  if (err != ESP_OK || isConfigured == 0) {
    Serial.println("Device not configured. Starting provisioning...");
    handleProvisioning();
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    nvs_set_i32(nvs_handle, NVS_KEY_IS_CONFIGURED, 1);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    Serial.println("Provisioning complete. Restarting...");
    ESP.restart();
  } else {
    Serial.println("Device configured. Attempting to connect to WiFi...");
    if (connectToStoredWifi()) {
      startNormalOperation();
    } else {
      Serial.println("Failed to connect to stored WiFi. Entering provisioning mode as fallback.");
      handleProvisioning();
    }
  }
}

// --- Loop Function ---
void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    handleFirmwareUpdates();
    espalexa.loop();
    ArduinoOTA.handle();
  } else {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 5000) {
      Serial.println("WiFi disconnected. Reconnecting...");
      if (connectToStoredWifi()) {
        startNormalOperation();
      }
      lastReconnectAttempt = millis();
    }
  }

  button1.check();
  button2.check();
  button3.check();
  button4.check();
  checkPhysicalFanSwitches();
}

// --- Implementation of New Functions ---

void initNVS() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NOT_INITIALIZED) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    Serial.printf("NVS Initialization Error: %s\n", esp_err_to_name(err));
  }
}

void handleProvisioning() {
  Serial.println("Starting WiFiManager AP...");
  wm.setConfigPortalTimeout(120);
  if (!wm.startConfigPortal(DEFAULT_AP_SSID, DEFAULT_AP_PASSWORD)) {
    Serial.println("Failed to connect to WiFi via config portal or timeout occurred.");
  } else {
    Serial.println("Successfully provisioned WiFi.");
  }
}

boolean connectToStoredWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  Serial.print("Connecting to WiFi");
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nWiFi Connection failed.");
    return false;
  }
}

void startNormalOperation() {
  Serial.println("Starting Alexa and OTA services.");
  espalexaDeviceCallbacks();
  espalexa.begin();
  setupOTA();
}

void espalexaDeviceCallbacks() {
  // ESPALEXA FIX: addDevice returns an index (uint8_t) in some versions.
  // The correct way is to get the device pointer using getDevice() after adding.
  espalexa.addDevice("Light 1", firstLightChanged);
  espalexa.addDevice("Light 2", secondLightChanged);
  espalexa.addDevice("Light 3", thirdLightChanged);
  espalexa.addDevice("Light 4", fourthLightChanged);
  espalexa.addDevice("Fan", fanChanged);
}

void setupOTA() {
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready.");
}

void handleFirmwareUpdates() {
  if (!otaChecked) {
    checkForOTAUpdate();
    otaChecked = true;
  }
}

void checkForOTAUpdate() {
  WiFiClient client;
  HTTPClient http;
  Serial.println("Checking for firmware updates...");
  http.begin(client, VERSION_URL);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("Got version file:");
    Serial.println(payload);

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.println("Failed to parse version file.");
      return;
    }

    int newVersion = doc["version"];
    const char* firmwareUrl = doc["url"];

    if (newVersion > CURRENT_VERSION) {
      Serial.printf("New firmware version %d available. Downloading from %s\n", newVersion, firmwareUrl);
      announceAlert("New firmware update available. Downloading now.");
      downloadAndApplyUpdate(firmwareUrl);
    } else {
      Serial.println("No new update available.");
    }
  } else {
    Serial.printf("Failed to get version file. HTTP Code: %d\n", httpCode);
  }
  http.end();
}

void downloadAndApplyUpdate(const char* url) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    Serial.printf("Downloading update (%d bytes)...\n", contentLength);

    bool updateSuccess = Update.begin(contentLength);
    if (updateSuccess) {
      WiFiClient& updateClient = http.getStream();
      size_t written = Update.writeStream(updateClient);
      if (written == contentLength) {
        Serial.println("Written update successfully.");
      } else {
        Serial.printf("Written only %d of %d bytes.\n", written, contentLength);
      }

      if (Update.end()) {
        Serial.println("Update finished successfully. Rebooting...");
        announceAlert("Firmware update successful. Rebooting.");
        delay(1000);
        ESP.restart();
      } else {
        Serial.printf("Update failed. Error: %d\n", Update.getError());
        announceAlert("Firmware update failed. Check serial output for details.");
      }
    } else {
      Serial.println("Not enough space to begin OTA.");
      announceAlert("OTA failed. Not enough space. Check serial output.");
    }
  } else {
    Serial.printf("Failed to download firmware. HTTP Code: %d\n", httpCode);
    announceAlert("Firmware download failed. Check serial output for details.");
  }
  http.end();
}

void announceAlert(const char* message) {
  Serial.printf("ALEXA ALERT: %s\n", message);
  // Placeholder for Alexa announcement, as Espalexa itself doesn't do voice alerts.
}

// --- Espalexa Callbacks Implementation ---

void firstLightChanged(uint8_t brightness) {
  bool state = (brightness > 0);
  applianceStates[0] = state; // FIX: Use correct array index
  updateAppliancePhysicalState(0, state);
  Serial.printf("Light 1 state: %s (Brightness: %d)\n", state ? "ON" : "OFF", brightness);
}

void secondLightChanged(uint8_t brightness) {
  bool state = (brightness > 0);
  applianceStates[1] = state; // FIX: Use correct array index
  updateAppliancePhysicalState(1, state);
  Serial.printf("Light 2 state: %s (Brightness: %d)\n", state ? "ON" : "OFF", brightness);
}

void thirdLightChanged(uint8_t brightness) {
  bool state = (brightness > 0);
  applianceStates[2] = state; // FIX: Use correct array index
  updateAppliancePhysicalState(2, state);
  Serial.printf("Light 3 state: %s (Brightness: %d)\n", state ? "ON" : "OFF", brightness);
}

void fourthLightChanged(uint8_t brightness) {
  bool state = (brightness > 0);
  applianceStates[3] = state; // FIX: Use correct array index
  updateAppliancePhysicalState(3, state);
  Serial.printf("Light 4 state: %s (Brightness: %d)\n", state ? "ON" : "OFF", brightness);
}

void fanChanged(uint8_t brightness) {
  int speed;
  if (brightness == 0) {
    speed = 0;
  } else if (brightness <= 64) {
    speed = 1;
  } else if (brightness <= 128) {
    speed = 2;
  } else if (brightness <= 192) {
    speed = 3;
  } else {
    speed = 4;
  }
  currentFanSpeed = speed;
  updateFanPhysicalState(speed);
  Serial.printf("Fan speed changed to: %d (Brightness: %d)\n", speed, brightness);
}

// --- Physical State Control Functions ---

void updateFanPhysicalState(int speed) {
  digitalWrite(Speed1_PIN, LOW);
  digitalWrite(Speed2_PIN, LOW);
  digitalWrite(Speed4_PIN, LOW);

  switch (speed) {
    case 1:
      digitalWrite(Speed1_PIN, HIGH);
      break;
    case 2:
      digitalWrite(Speed2_PIN, HIGH);
      break;
    case 3:
      digitalWrite(Speed1_PIN, HIGH);
      digitalWrite(Speed2_PIN, HIGH);
      break;
    case 4:
      digitalWrite(Speed4_PIN, HIGH);
      break;
  }
}

void updateAppliancePhysicalState(int applianceIndex, bool state) {
  int relayPin;
  switch (applianceIndex) {
    case 0: relayPin = R1; break;
    case 1: relayPin = R2; break;
    case 2: relayPin = R3; break;
    case 3: relayPin = R4; break;
    default: return;
  }
  digitalWrite(relayPin, state ? HIGH : LOW);
}

// --- Button Event Handler ---
void handleButtonEvent(AceButton* button, uint8_t eventType, uint8_t buttonState) {
  if (eventType == AceButton::kEventClicked) {
    uint8_t pin = button->getPin();
    int applianceIndex = -1;

    if (pin == S1) { applianceIndex = 0; }
    else if (pin == S2) { applianceIndex = 1; }
    else if (pin == S3) { applianceIndex = 2; }
    else if (pin == S4) { applianceIndex = 3; }

    if (applianceIndex != -1) {
      applianceStates[applianceIndex] = !applianceStates[applianceIndex];
      updateAppliancePhysicalState(applianceIndex, applianceStates[applianceIndex]);
      Serial.printf("Button %d clicked. Light %d state toggled to: %s\n", applianceIndex + 1, applianceIndex + 1, applianceStates[applianceIndex] ? "ON" : "OFF");
      espalexa.getDevice(applianceIndex)->setValue(applianceStates[applianceIndex] ? 255 : 0);
    }
  }
}

// --- Physical Fan Switch Monitoring ---

void checkPhysicalFanSwitches() {
  static int lastKnownFanSwitchState = 0;
  int newFanSwitchState = 0;

  if (digitalRead(F1) == LOW) {
    newFanSwitchState = 1;
  } else if (digitalRead(F2) == LOW) {
    newFanSwitchState = 2;
  } else if (digitalRead(F3) == LOW) {
    newFanSwitchState = 3;
  } else if (digitalRead(F4) == LOW) {
    newFanSwitchState = 4;
  }

  if (newFanSwitchState != lastKnownFanSwitchState) {
    currentFanSpeed = newFanSwitchState;
    updateFanPhysicalState(currentFanSpeed);
    espalexa.getDevice(4)->setValue(currentFanSpeed * 64);
    lastKnownFanSwitchState = newFanSwitchState;
    Serial.printf("Physical fan switch changed. Fan speed set to: %d\n", currentFanSpeed);
  }
}
