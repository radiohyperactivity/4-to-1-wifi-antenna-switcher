/*
 * ESP32 Antenna Switcher
 * 
 * Features:
 * - Web interface for antenna selection (4 outputs)
 * - Serial commands for configuration and control
 * - WiFi AP mode for initial configuration
 * - Persistent storage of WiFi credentials and unit name
 * - HTTP API for programmatic control
 * - Status LED indication
 * 
 * LED Status:
 * - Fast blink (200ms): AP mode, waiting for configuration
 * - Slow blink (1000ms): Attempting to connect to WiFi
 * - Solid ON: Connected to WiFi
 * 
 * GPIO Pins (easily configurable via defines):
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ============================================================================
// GPIO PIN DEFINITIONS - Change these to match your hardware requirements
// ============================================================================
#define ANT1_PIN 16
#define ANT2_PIN 17
#define ANT3_PIN 18
#define ANT4_PIN 19
#define STATUS_LED_PIN 2  // Built-in LED on most ESP32 boards

// ============================================================================
// CONFIGURATION
// ============================================================================
#define AP_SSID_PREFIX "ESP32-AntennaSwitch-"
#define WIFI_CONNECT_TIMEOUT 60000    // 60 seconds
#define WIFI_RETRY_INTERVAL 120000    // 120 seconds
#define LED_FAST_BLINK 200            // AP mode blink rate (ms)
#define LED_SLOW_BLINK 1000           // Connecting blink rate (ms)

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
Preferences preferences;
WebServer server(80);

String wifiSSID = "";
String wifiPassword = "";
String unitName = "";
String antenna1Name = "";
String antenna2Name = "";
String antenna3Name = "";
String antenna4Name = "";

enum WiFiState {
  WIFI_AP_MODE,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_FAILED
};

WiFiState currentWiFiState = WIFI_AP_MODE;
unsigned long lastWiFiAttempt = 0;
unsigned long lastLedToggle = 0;
bool ledState = false;

int activeAntenna = 0; // 0 = none, 1-4 = antenna number

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void setupGPIO();
void loadConfiguration();
void saveConfiguration();
void startAPMode();
void connectToWiFi();
void checkWiFiStatus();
void updateStatusLED();
void activateAntenna(int antenna);
void handleSerialCommands();
void handleRoot();
void handleConfig();
void handleActivate();
void handleNotFound();
String getStatusJSON();

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println("\n\n");
  Serial.println("===========================================");
  Serial.println("ESP32 Antenna Switcher");
  Serial.println("===========================================");
  
  // Initialize GPIO pins FIRST to minimize floating time
  setupGPIO();
  
  // Initialize preferences
  preferences.begin("ant-switch", false);
  loadConfiguration();
  
  // Print configuration
  Serial.println("\nCurrent Configuration:");
  Serial.println("Unit Name: " + (unitName.length() > 0 ? unitName : "(not set)"));
  Serial.println("WiFi SSID: " + (wifiSSID.length() > 0 ? wifiSSID : "(not set)"));
  Serial.println("WiFi Password: " + (wifiPassword.length() > 0 ? wifiPassword : "(not set)"));
  
  // Setup web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/activate/0", HTTP_PUT, []() { activateAntenna(0); server.send(200); });
  server.on("/activate/1", HTTP_PUT, []() { activateAntenna(1); server.send(200); });
  server.on("/activate/2", HTTP_PUT, []() { activateAntenna(2); server.send(200); });
  server.on("/activate/3", HTTP_PUT, []() { activateAntenna(3); server.send(200); });
  server.on("/activate/4", HTTP_PUT, []() { activateAntenna(4); server.send(200); });
  server.on("/status", HTTP_GET, []() { server.send(200, "application/json", getStatusJSON()); });
  server.onNotFound(handleNotFound);
  
  // Try to connect to WiFi if credentials are set
  if (wifiSSID.length() > 0) {
    Serial.println("\nAttempting to connect to configured WiFi...");
    currentWiFiState = WIFI_CONNECTING;
    connectToWiFi();
  } else {
    Serial.println("\nNo WiFi credentials configured. Starting AP mode...");
    startAPMode();
  }
  
  server.begin();
  Serial.println("Web server started");
  Serial.println("\nSerial commands available:");
  Serial.println("  set ssid <SSID>");
  Serial.println("  set password <PASSWORD>");
  Serial.println("  set name <NAME>");
  Serial.println("  get status");
  Serial.println("  set active <1-4>");
  Serial.println("===========================================\n");
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  server.handleClient();
  checkWiFiStatus();
  updateStatusLED();
  handleSerialCommands();
  delay(10);
}

// ============================================================================
// GPIO FUNCTIONS
// ============================================================================
void setupGPIO() {
  pinMode(ANT1_PIN, OUTPUT);
  pinMode(ANT2_PIN, OUTPUT);
  pinMode(ANT3_PIN, OUTPUT);
  pinMode(ANT4_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  
  // Set all antenna outputs to LOW at boot
  digitalWrite(ANT1_PIN, LOW);
  digitalWrite(ANT2_PIN, LOW);
  digitalWrite(ANT3_PIN, LOW);
  digitalWrite(ANT4_PIN, LOW);
  digitalWrite(STATUS_LED_PIN, LOW);
  
  Serial.println("GPIO initialized:");
  Serial.println("  ANT1: GPIO " + String(ANT1_PIN));
  Serial.println("  ANT2: GPIO " + String(ANT2_PIN));
  Serial.println("  ANT3: GPIO " + String(ANT3_PIN));
  Serial.println("  ANT4: GPIO " + String(ANT4_PIN));
}

void activateAntenna(int antenna) {
  if (antenna < 0 || antenna > 4) {
    Serial.println("ERROR: Invalid antenna number: " + String(antenna));
    return;
  }
  
  // Deactivate all antennas first
  digitalWrite(ANT1_PIN, LOW);
  digitalWrite(ANT2_PIN, LOW);
  digitalWrite(ANT3_PIN, LOW);
  digitalWrite(ANT4_PIN, LOW);
  
  // Activate requested antenna if not 0
  if (antenna > 0) {
    int pin = 0;
    switch(antenna) {
      case 1: pin = ANT1_PIN; break;
      case 2: pin = ANT2_PIN; break;
      case 3: pin = ANT3_PIN; break;
      case 4: pin = ANT4_PIN; break;
    }
    digitalWrite(pin, HIGH);
    Serial.println("Activated antenna " + String(antenna) + " (GPIO " + String(pin) + ")");
  } else {
    Serial.println("All antennas deactivated");
  }
  
  activeAntenna = antenna;
}

// ============================================================================
// CONFIGURATION FUNCTIONS
// ============================================================================
void loadConfiguration() {
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  unitName = preferences.getString("name", "");
  antenna1Name = preferences.getString("ant1", "Antenna 1");
  antenna2Name = preferences.getString("ant2", "Antenna 2");
  antenna3Name = preferences.getString("ant3", "Antenna 3");
  antenna4Name = preferences.getString("ant4", "Antenna 4");
  
  Serial.println("Configuration loaded from NVS");
}

void saveConfiguration() {
  preferences.putString("ssid", wifiSSID);
  preferences.putString("password", wifiPassword);
  preferences.putString("name", unitName);
  preferences.putString("ant1", antenna1Name);
  preferences.putString("ant2", antenna2Name);
  preferences.putString("ant3", antenna3Name);
  preferences.putString("ant4", antenna4Name);
  
  Serial.println("Configuration saved to NVS");
}

// ============================================================================
// WIFI FUNCTIONS
// ============================================================================
void startAPMode() {
  WiFi.mode(WIFI_AP);
  
  // Generate unique SSID using chip ID
  uint64_t chipid = ESP.getEfuseMac();
  String apSSID = String(AP_SSID_PREFIX) + String((uint32_t)(chipid >> 32), HEX);
  
  WiFi.softAP(apSSID.c_str());
  IPAddress IP = WiFi.softAPIP();
  
  currentWiFiState = WIFI_AP_MODE;
  
  Serial.println("AP Mode started");
  Serial.println("SSID: " + apSSID);
  Serial.println("IP address: " + IP.toString());
  Serial.println("Connect to this network and navigate to " + IP.toString());
}

void connectToWiFi() {
  if (wifiSSID.length() == 0) {
    Serial.println("No SSID configured, cannot connect");
    startAPMode();
    return;
  }
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  
  Serial.println("Connecting to WiFi: " + wifiSSID);
  lastWiFiAttempt = millis();
}

void checkWiFiStatus() {
  unsigned long currentMillis = millis();
  
  if (currentWiFiState == WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      currentWiFiState = WIFI_CONNECTED;
      Serial.println("WiFi connected!");
      Serial.println("IP address: " + WiFi.localIP().toString());
      if (unitName.length() > 0) {
        Serial.println("Access at: http://" + WiFi.localIP().toString() + " (" + unitName + ")");
      }
    } else if (currentMillis - lastWiFiAttempt > WIFI_CONNECT_TIMEOUT) {
      currentWiFiState = WIFI_FAILED;
      Serial.println("WiFi connection failed, reverting to AP mode");
      startAPMode();
    }
  } else if (currentWiFiState == WIFI_CONNECTED) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection lost");
      currentWiFiState = WIFI_FAILED;
      startAPMode();
    }
  } else if (currentWiFiState == WIFI_FAILED || currentWiFiState == WIFI_AP_MODE) {
    // Retry connection every WIFI_RETRY_INTERVAL if we have credentials
    if (wifiSSID.length() > 0 && currentMillis - lastWiFiAttempt > WIFI_RETRY_INTERVAL) {
      Serial.println("Retrying WiFi connection...");
      currentWiFiState = WIFI_CONNECTING;
      connectToWiFi();
    }
  }
}

void updateStatusLED() {
  unsigned long currentMillis = millis();
  int blinkRate = 0;
  
  switch(currentWiFiState) {
    case WIFI_AP_MODE:
      blinkRate = LED_FAST_BLINK;
      break;
    case WIFI_CONNECTING:
      blinkRate = LED_SLOW_BLINK;
      break;
    case WIFI_CONNECTED:
      // Solid ON when connected
      digitalWrite(STATUS_LED_PIN, HIGH);
      return;
    case WIFI_FAILED:
      blinkRate = LED_FAST_BLINK;
      break;
  }
  
  if (currentMillis - lastLedToggle > blinkRate) {
    ledState = !ledState;
    digitalWrite(STATUS_LED_PIN, ledState);
    lastLedToggle = currentMillis;
  }
}

// ============================================================================
// SERIAL COMMAND HANDLER
// ============================================================================
void handleSerialCommands() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    
    if (command.length() == 0) return;
    
    Serial.println("\nReceived command: " + command);
    
    if (command.startsWith("set ssid ")) {
      wifiSSID = command.substring(9);
      saveConfiguration();
      Serial.println("SSID set to: " + wifiSSID);
      Serial.println("Restarting to apply changes...");
      delay(1000);
      ESP.restart();
    }
    else if (command.startsWith("set password ")) {
      wifiPassword = command.substring(13);
      saveConfiguration();
      Serial.println("Password set to: " + wifiPassword);
      Serial.println("Restarting to apply changes...");
      delay(1000);
      ESP.restart();
    }
    else if (command.startsWith("set name ")) {
      unitName = command.substring(9);
      saveConfiguration();
      Serial.println("Unit name set to: " + unitName);
    }
    else if (command.startsWith("set active ")) {
      String numStr = command.substring(11);
      int antennaNum = numStr.toInt();
      if (antennaNum >= 1 && antennaNum <= 4) {
        activateAntenna(antennaNum);
        Serial.println("Activated antenna " + String(antennaNum));
      } else if (antennaNum == 0) {
        activateAntenna(0);
        Serial.println("All antennas deactivated");
      } else {
        Serial.println("ERROR: Invalid antenna number. Use 1-4 or 0 to deactivate all.");
      }
    }
    else if (command == "get status") {
      Serial.println("\n--- Current Status ---");
      Serial.println("Unit Name: " + (unitName.length() > 0 ? unitName : "(not set)"));
      Serial.println("WiFi SSID: " + (wifiSSID.length() > 0 ? wifiSSID : "(not set)"));
      Serial.println("WiFi Password: " + (wifiPassword.length() > 0 ? wifiPassword : "(not set)"));
      Serial.println("Antenna 1 Name: " + antenna1Name);
      Serial.println("Antenna 2 Name: " + antenna2Name);
      Serial.println("Antenna 3 Name: " + antenna3Name);
      Serial.println("Antenna 4 Name: " + antenna4Name);
      
      Serial.print("Connection Status: ");
      switch(currentWiFiState) {
        case WIFI_AP_MODE:
          Serial.println("AP Mode");
          Serial.println("AP IP: " + WiFi.softAPIP().toString());
          break;
        case WIFI_CONNECTING:
          Serial.println("Connecting...");
          break;
        case WIFI_CONNECTED:
          Serial.println("Connected");
          Serial.println("IP Address: " + WiFi.localIP().toString());
          break;
        case WIFI_FAILED:
          Serial.println("Connection Failed (AP Mode)");
          Serial.println("AP IP: " + WiFi.softAPIP().toString());
          break;
      }
      
      Serial.println("Active Antenna: " + (activeAntenna > 0 ? String(activeAntenna) : "None"));
      Serial.println("---------------------\n");
    }
    else {
      Serial.println("Unknown command. Available commands:");
      Serial.println("  set ssid <SSID>");
      Serial.println("  set password <PASSWORD>");
      Serial.println("  set name <NAME>");
      Serial.println("  get status");
      Serial.println("  set active <1-4>");
    }
  }
}

// ============================================================================
// WEB SERVER HANDLERS
// ============================================================================
void handleRoot() {
  String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  page += "<title>" + (unitName.length() > 0 ? unitName : String("Antenna Switcher")) + "</title>";
  page += "<style>";
  page += "* { margin: 0; padding: 0; box-sizing: border-box; }";
  page += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;";
  page += "background: #1a1a1a; color: #e0e0e0; min-height: 100vh;";
  page += "display: flex; flex-direction: column; align-items: center; padding: 20px; }";
  page += "header { width: 100%; max-width: 600px; margin-bottom: 30px; text-align: center; }";
  page += "h1 { color: #ffffff; font-size: 24px; font-weight: 400; }";
  page += ".container { background: #242424; border-radius: 12px; padding: 30px;";
  page += "max-width: 600px; width: 100%; border: 1px solid #3a3a3a; }";
  page += ".antenna-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }";
  page += ".antenna-btn { background: #2a2a2a; border: 2px solid #3a3a3a; border-radius: 8px;";
  page += "padding: 24px 16px; font-size: 16px; font-weight: 500; cursor: pointer;";
  page += "transition: all 0.2s ease; color: #b0b0b0; font-family: inherit; }";
  page += ".antenna-btn:hover { background: #333; border-color: #4a4a4a; transform: translateY(-2px); }";
  page += ".antenna-btn.active { background: #1565c0; border-color: #1976d2; color: #fff; }";
  page += ".antenna-btn:active { transform: translateY(0); }";
  page += ".info-box { background: #2a2a2a; border-radius: 8px; padding: 16px; margin-top: 20px;";
  page += "border: 1px solid #3a3a3a; }";
  page += ".info-label { color: #888; font-size: 12px; text-transform: uppercase;";
  page += "letter-spacing: 0.5px; margin-bottom: 6px; }";
  page += ".info-value { color: #e0e0e0; font-size: 18px; font-weight: 500; }";
  page += ".config-link { display: block; text-align: center; margin-top: 24px;";
  page += "color: #888; text-decoration: none; font-size: 13px; padding: 8px; }";
  page += ".config-link:hover { color: #b0b0b0; }";
  page += "@media (max-width: 500px) { .antenna-grid { grid-template-columns: 1fr; } }";
  page += "</style></head><body>";
  page += "<header>";
  page += "<h1>" + (unitName.length() > 0 ? unitName : String("Antenna Switcher")) + "</h1>";
  page += "</header>";
  page += "<div class='container'>";
  page += "<div class='antenna-grid'>";
  page += "<button class='antenna-btn' onclick='activateAntenna(1)' id='ant1'>" + antenna1Name + "</button>";
  page += "<button class='antenna-btn' onclick='activateAntenna(2)' id='ant2'>" + antenna2Name + "</button>";
  page += "<button class='antenna-btn' onclick='activateAntenna(3)' id='ant3'>" + antenna3Name + "</button>";
  page += "<button class='antenna-btn' onclick='activateAntenna(4)' id='ant4'>" + antenna4Name + "</button>";
  page += "</div>";
  page += "<div class='info-box'>";
  page += "<div class='info-label'>Currently Active</div>";
  page += "<div class='info-value' id='activeAnt'>None</div>";
  page += "</div>";
  page += "<a href='/config' class='config-link'>⚙ Configuration</a>";
  page += "<div style='text-align: center; margin-top: 12px; font-size: 11px; color: #777;'>";
  page += "<a href='https://www.youtube.com/@radiohyperactivity' target='_blank' style='color: #888; text-decoration: none;'>Radiohyperactivity</a> 4 Port Antenna Switch";
  page += "</div>";
  page += "</div>";
  page += "<script>";
  page += "let currentActive = 0;";
  page += "const antennaNames = ['" + antenna1Name + "', '" + antenna2Name + "', '" + antenna3Name + "', '" + antenna4Name + "'];";
  page += "function activateAntenna(num) {";
  page += "if (currentActive === num) { num = 0; }";
  page += "fetch('/activate/' + num, { method: 'PUT' })";
  page += ".then(response => { if (response.ok) { updateActiveButton(num); } })";
  page += ".catch(err => console.error('Error:', err)); }";
  page += "function updateActiveButton(num) {";
  page += "for (let i = 1; i <= 4; i++) { document.getElementById('ant' + i).classList.remove('active'); }";
  page += "if (num > 0) { document.getElementById('ant' + num).classList.add('active'); }";
  page += "currentActive = num;";
  page += "document.getElementById('activeAnt').textContent = num > 0 ? antennaNames[num-1] : 'None'; }";
  page += "function updateStatus() {";
  page += "fetch('/status').then(response => response.json()).then(data => {";
  page += "updateActiveButton(data.activeAntenna);";
  page += "}).catch(err => console.error('Error:', err)); }";
  page += "updateStatus(); setInterval(updateStatus, 1000);";
  page += "</script></body></html>";

  server.send(200, "text/html", page);
}

void handleConfig() {
  String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  page += "<title>Configuration - " + (unitName.length() > 0 ? unitName : String("Antenna Switcher")) + "</title>";
  page += "<style>";
  page += "* { margin: 0; padding: 0; box-sizing: border-box; }";
  page += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;";
  page += "background: #1a1a1a; color: #e0e0e0; min-height: 100vh; padding: 20px; }";
  page += ".container { max-width: 500px; margin: 0 auto; }";
  page += "h1 { color: #fff; font-size: 24px; font-weight: 400; margin-bottom: 8px; }";
  page += ".subtitle { color: #888; font-size: 14px; margin-bottom: 30px; }";
  page += ".config-box { background: #242424; border: 1px solid #3a3a3a; border-radius: 12px; padding: 24px; }";
  page += ".form-group { margin-bottom: 20px; }";
  page += ".form-group label { display: block; color: #b0b0b0; font-size: 13px; margin-bottom: 8px; }";
  page += ".form-group input { width: 100%; background: #2a2a2a; border: 1px solid #3a3a3a;";
  page += "border-radius: 6px; padding: 10px 12px; color: #e0e0e0; font-size: 14px; font-family: inherit; }";
  page += ".form-group input:focus { outline: none; border-color: #1976d2; background: #2d2d2d; }";
  page += ".btn { width: 100%; padding: 12px; border: none; border-radius: 6px; font-size: 14px;";
  page += "font-weight: 500; cursor: pointer; transition: all 0.2s; font-family: inherit; }";
  page += ".btn-primary { background: #1565c0; color: #fff; margin-bottom: 10px; }";
  page += ".btn-primary:hover { background: #1976d2; }";
  page += ".btn-secondary { background: #2a2a2a; color: #b0b0b0; border: 1px solid #3a3a3a; }";
  page += ".btn-secondary:hover { background: #333; border-color: #4a4a4a; }";
  page += ".warning { background: #332200; border: 1px solid #554400; border-radius: 6px;";
  page += "padding: 12px; margin-bottom: 20px; font-size: 13px; color: #ffaa00; }";
  page += "</style></head><body>";
  page += "<div class='container'>";
  page += "<h1>Configuration</h1>";
  page += "<div class='subtitle'>WiFi and device settings</div>";
  page += "<div class='config-box'>";
  page += "<div class='warning'>⚠ Saving will restart the device</div>";
  page += "<form onsubmit='saveConfig(event)'>";
  page += "<div class='form-group'>";
  page += "<label>WiFi SSID</label>";
  page += "<input type='text' id='ssid' value='" + wifiSSID + "' required>";
  page += "</div>";
  page += "<div class='form-group'>";
  page += "<label>WiFi Password</label>";
  page += "<input type='password' id='password' value='" + wifiPassword + "'>";
  page += "</div>";
  page += "<div class='form-group'>";
  page += "<label>Unit Name</label>";
  page += "<input type='text' id='name' value='" + unitName + "'>";
  page += "</div>";
  page += "<div class='form-group'>";
  page += "<label>Antenna 1 Name</label>";
  page += "<input type='text' id='ant1' value='" + antenna1Name + "'>";
  page += "</div>";
  page += "<div class='form-group'>";
  page += "<label>Antenna 2 Name</label>";
  page += "<input type='text' id='ant2' value='" + antenna2Name + "'>";
  page += "</div>";
  page += "<div class='form-group'>";
  page += "<label>Antenna 3 Name</label>";
  page += "<input type='text' id='ant3' value='" + antenna3Name + "'>";
  page += "</div>";
  page += "<div class='form-group'>";
  page += "<label>Antenna 4 Name</label>";
  page += "<input type='text' id='ant4' value='" + antenna4Name + "'>";
  page += "</div>";
  page += "<button type='submit' class='btn btn-primary'>Save & Restart</button>";
  page += "<button type='button' class='btn btn-secondary' onclick='window.location=\"/\"'>Cancel</button>";
  page += "</form>";
  page += "</div></div>";
  page += "<script>";
  page += "function saveConfig(event) {";
  page += "event.preventDefault();";
  page += "if (!confirm('This will restart the device. Continue?')) return;";
  page += "const formData = new URLSearchParams();";
  page += "formData.append('ssid', document.getElementById('ssid').value);";
  page += "formData.append('password', document.getElementById('password').value);";
  page += "formData.append('name', document.getElementById('name').value);";
  page += "formData.append('ant1', document.getElementById('ant1').value);";
  page += "formData.append('ant2', document.getElementById('ant2').value);";
  page += "formData.append('ant3', document.getElementById('ant3').value);";
  page += "formData.append('ant4', document.getElementById('ant4').value);";
  page += "fetch('/config', { method: 'POST', body: formData })";
  page += ".then(() => { setTimeout(() => window.location='/', 5000); })";
  page += ".catch(err => alert('Error: ' + err)); }";
  page += "</script></body></html>";
  
  server.send(200, "text/html", page);
}

void handleActivate() {
  String uri = server.uri();
  // This is now handled by individual routes in setup()
  server.send(404, "text/plain", "Not Found");
}

void handleNotFound() {
  // Check if this is a POST to /config from web interface
  if (server.uri() == "/config" && server.method() == HTTP_POST) {
    if (server.hasArg("ssid")) {
      wifiSSID = server.arg("ssid");
    }
    if (server.hasArg("password")) {
      wifiPassword = server.arg("password");
    }
    if (server.hasArg("name")) {
      unitName = server.arg("name");
    }
    if (server.hasArg("ant1")) {
      antenna1Name = server.arg("ant1");
    }
    if (server.hasArg("ant2")) {
      antenna2Name = server.arg("ant2");
    }
    if (server.hasArg("ant3")) {
      antenna3Name = server.arg("ant3");
    }
    if (server.hasArg("ant4")) {
      antenna4Name = server.arg("ant4");
    }
    
    saveConfiguration();
    
    server.send(200, "text/plain", "Configuration saved. Restarting...");
    
    Serial.println("Configuration updated via web interface:");
    Serial.println("  SSID: " + wifiSSID);
    Serial.println("  Password: " + wifiPassword);
    Serial.println("  Name: " + unitName);
    Serial.println("  Antenna 1: " + antenna1Name);
    Serial.println("  Antenna 2: " + antenna2Name);
    Serial.println("  Antenna 3: " + antenna3Name);
    Serial.println("  Antenna 4: " + antenna4Name);
    Serial.println("Restarting...");
    
    delay(1000);
    ESP.restart();
    return;
  }
  
  server.send(404, "text/plain", "Not Found");
}

String getStatusJSON() {
  String json = "{";
  json += "\"activeAntenna\":" + String(activeAntenna) + ",";
  json += "\"unitName\":\"" + unitName + "\",";
  json += "\"wifiSSID\":\"" + wifiSSID + "\",";
  
  String status = "";
  switch(currentWiFiState) {
    case WIFI_AP_MODE:
      status = "AP Mode";
      break;
    case WIFI_CONNECTING:
      status = "Connecting";
      break;
    case WIFI_CONNECTED:
      status = "Connected";
      json += "\"ipAddress\":\"" + WiFi.localIP().toString() + "\",";
      break;
    case WIFI_FAILED:
      status = "Failed";
      break;
  }
  
  json += "\"wifiStatus\":\"" + status + "\"";
  json += "}";
  
  return json;
}
