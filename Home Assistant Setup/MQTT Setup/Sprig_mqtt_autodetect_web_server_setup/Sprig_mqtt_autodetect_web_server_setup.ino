#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_MAX1704X.h"
#include <ArduinoJson.h>
#include "secrets.h"
#include <WebServer.h>
#include <Preferences.h>

Adafruit_MAX17048 maxlipo;

WiFiClient WiFi_client;
PubSubClient MQTT_client(WiFi_client);
long prev = 0;
char msg[50];
int value = 0;
String lastPublishedIP = "";

// MQTT Auto-Discovery variables
bool auto_discovery = true;      // enable auto-discovery by default
byte macAddr[6];                 // Device MAC address
char uidPrefix[] = "sprigC3";  // Prefix for unique ID generation
char devUniqueID[30];            // Generated Unique ID for this device

// MQTT control topics for discovery toggle
const char discoverySetTopic[] = "sprigC3/discovery/set";
const char discoveryStateTopic[] = "sprigC3/discovery/state";

// Configuration portal and persistent storage
WebServer server(80);
Preferences prefs;
String cfg_wifi_ssid;
String cfg_wifi_pass;
String cfg_mqtt_server;
String cfg_mqtt_user;
String cfg_mqtt_pass;
int cfg_mqtt_port = 1883;
String ip;
const char* AP_SSID = "SprigC3-Setup";
const char* AP_PASS = ""; // open AP

// Factory-reset (clear stored WiFi/MQTT) button
const int BOOT = 9;                // button connected to GPIO9
unsigned long bootPressStart = 0;
const unsigned long BOOT_HOLD_MS = 5000; // hold time (ms) for factory reset


void setup() {
  Serial.begin(115200);
  // Initialize MAX17048
  Wire.begin(2,3);
  while (!maxlipo.begin(&Wire)) {
    Serial.println(F("Damaged fuel cell!\n Check I2C pins."));
    delay(1000);
  }
  Serial.print(F(" Fuel-cell chip ID: 0x")); 
  Serial.println(maxlipo.getChipID(), HEX);

  // Configure BOOT button (factory-reset) pin
  pinMode(BOOT, INPUT_PULLUP);

  // Initialize WiFi and MQTT
  // Load stored credentials (if any)
  prefs.begin("sprig", false);
  cfg_wifi_ssid = prefs.getString("wifi_ssid", "");
  cfg_wifi_pass = prefs.getString("wifi_pass", "");
  cfg_mqtt_server = prefs.getString("mqtt_server", MQTT_SERVER);
  cfg_mqtt_user = prefs.getString("mqtt_user", MQTT_USER);
  cfg_mqtt_pass = prefs.getString("mqtt_pass", MQTT_PASSWORD);
  cfg_mqtt_port = prefs.getInt("mqtt_port", 1883);

  if (cfg_wifi_ssid.length() == 0) {
    // No stored WiFi credentials — start configuration AP
    Serial.println("No WiFi credentials found — starting config portal");
    WiFi.softAP(AP_SSID, AP_PASS);
    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP IP address: "); Serial.println(apIP);
    // start web server handlers
    server.on("/", []() {
      String page = "<html><head><title>Sprig C3 Setup</title></head><body>";
      page += "<h2>Sprig C3 Setup</h2>";
      page += "<form method='POST' action='/save'>";
      page += "WiFi SSID:<br><input name='wifi_ssid' length=32><br>";
      page += "WiFi Password:<br><input name='wifi_pass' type='password' length=64><br>";
      page += "MQTT Server:<br><input name='mqtt_server' value='" + String(MQTT_SERVER) + "'><br>";
      page += "MQTT Port:<br><input name='mqtt_port' value='1883'><br>";
      page += "MQTT User:<br><input name='mqtt_user' value='" + String(MQTT_USER) + "'><br>";
      page += "MQTT Pass:<br><input name='mqtt_pass' type='password'><br>";
      page += "<br><input type='submit' value='Save and Reboot'>";
      page += "</form></body></html>";
      server.send(200, "text/html", page);
    });
    server.on("/save", HTTP_POST, []() {
      String ssid = server.arg("wifi_ssid");
      String pass = server.arg("wifi_pass");
      String mserver = server.arg("mqtt_server");
      String mport = server.arg("mqtt_port");
      String muser = server.arg("mqtt_user");
      String mpass = server.arg("mqtt_pass");
      prefs.putString("wifi_ssid", ssid);
      prefs.putString("wifi_pass", pass);
      prefs.putString("mqtt_server", mserver);
      prefs.putInt("mqtt_port", mport.toInt());
      prefs.putString("mqtt_user", muser);
      prefs.putString("mqtt_pass", mpass);
      String resp = "<html><body><h3>Saved. Rebooting...</h3></body></html>";
      server.send(200, "text/html", resp);
      delay(500);
      ESP.restart();
    });
    server.begin();
    // enter loop until configured (setup ends and loop will service server)
    return;
  }

  // If WiFi creds exist, attempt normal setup
  setup_wifi();
  ip = WiFi.localIP().toString();
  // Increase MQTT buffer size to accommodate discovery payloads
  MQTT_client.setBufferSize(512);
  // Use configured MQTT server/port
  MQTT_client.setServer(cfg_mqtt_server.c_str(), cfg_mqtt_port);
  // Set MQTT message callback for control topics
  MQTT_client.setCallback(mqttCallback);
  
  // Get MAC address and create unique device ID for discovery
  WiFi.macAddress(macAddr);
  createDiscoveryUniqueID();
  
  // Start web server handlers for runtime control (discovery toggle)
  server.on("/discovery", HTTP_GET, []() {
    String page = "<html><head><title>Sprig Discovery</title></head><body>";
    page += "<h2>MQTT Discovery</h2>";
    page += "<p>Current state: " + String(auto_discovery ? "ON" : "OFF") + "</p>";
    // form posts desired state (opposite of current)
    String desired = auto_discovery ? "OFF" : "ON";
    String btnLabel = auto_discovery ? "Turn OFF" : "Turn ON";
    page += "<form method='POST' action='/discovery/set'>";
    page += "<input type='hidden' name='state' value='" + desired + "'>";
    page += "<input type='submit' value='" + btnLabel + "'>";
    page += "</form></body></html>";
    server.send(200, "text/html", page);
  });

  server.on("/discovery/set", HTTP_POST, []() {
    String state = server.arg("state");
    state.toUpperCase();
    if (state == "ON") {
      if (!auto_discovery) {
        auto_discovery = true;
        haDiscovery();
      }
    } else if (state == "OFF") {
      if (auto_discovery) {
        auto_discovery = false;
        haDiscovery();
      }
    }
    // Publish the control topic so MQTT subscribers (and this device) see it
    MQTT_client.publish(discoverySetTopic, (auto_discovery ? "ON" : "OFF"), true);
    // Publish the resulting state (retained)
    MQTT_client.publish(discoveryStateTopic, (auto_discovery ? "ON" : "OFF"), true);
    String resp = "<html><body><h3>Set discovery to " + String(auto_discovery ? "ON" : "OFF") + ". Returning...</h3><script>setTimeout(function(){location.href='/discovery'},1000);</script></body></html>";
    server.send(200, "text/html", resp);
  });

  server.begin();
  
}

void setup_wifi() {
  delay(10);
  Serial.print("Connecting to ");
  Serial.println(cfg_wifi_ssid.length() ? cfg_wifi_ssid : String(SSID));
  if (cfg_wifi_ssid.length()) WiFi.begin(cfg_wifi_ssid.c_str(), cfg_wifi_pass.c_str());
  else WiFi.begin(SSID, PASSWORD);

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  // Loop until we're reconnected
  while (!MQTT_client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Use configured MQTT server/user/pass
    String mqttUserStr = cfg_mqtt_user.length() ? cfg_mqtt_user : String(MQTT_USER);
    String mqttPassStr = cfg_mqtt_pass.length() ? cfg_mqtt_pass : String(MQTT_PASSWORD);
    String clientId = String(devUniqueID);
    if (MQTT_client.connect(clientId.c_str(), mqttUserStr.c_str(), mqttPassStr.c_str())) {
      Serial.println("connected");
      // Subscribe to discovery control topic and publish discovery/state
      MQTT_client.subscribe(discoverySetTopic);
      // Publish discovery configuration (adds/updates entities)
      haDiscovery();
      // Publish device IP for diagnostics
      // publishDeviceIP();
      // Publish current discovery state (retained)
      MQTT_client.publish(discoveryStateTopic, (auto_discovery ? "ON" : "OFF"), true);
    } else {
      Serial.print("failed, rc=");
      Serial.print(MQTT_client.state());
      Serial.println(" trying again in 5 seconds");
      delay(5000);
    }
  }
}


// =====================================
//  Create Unique ID for topics/entities
// =====================================
void createDiscoveryUniqueID() {
  strcpy(devUniqueID, uidPrefix);
  int preSizeBytes = strlen(uidPrefix);
  int j = 0;
  for (int i = 2; i >= 0; i--) {
    sprintf(&devUniqueID[preSizeBytes + j], "%02X", macAddr[i]);
    j = j + 2;
  }
  Serial.print("Unique ID: ");
  Serial.println(devUniqueID);
}

// ===============================
//  Main HA MQTT Discovery Function
//    Publishes two sensors: voltage and level
// ===============================
void haDiscovery() {
  char topic[128];
  if (auto_discovery) {
    char buffer1[512];
    char buffer2[512];
    char uid[128];
    DynamicJsonDocument doc(512);

    // BATTERY VOLTAGE
    strcpy(topic, "homeassistant/sensor/");
    strcat(topic, devUniqueID);
    strcat(topic, "_voltage/config");
    strcpy(uid, devUniqueID);
    strcat(uid, "_voltage");
    doc.clear();
    doc["name"] = "Battery Voltage";
    doc["obj_id"] = "battery_voltage";
    doc["uniq_id"] = uid;
    doc["stat_t"] = "sprigC3/batVoltage";
    doc["unit_of_meas"] = "V";
    doc["dev_cla"] = "voltage";
    doc["val_tpl"] = "{{ value | float(2) }}";
    doc["frc_upd"] = true;
    JsonObject device = doc.createNestedObject("device");
    device["ids"] = devUniqueID;
    device["name"] = "Sprig-C3 Battery Monitor";
    device["mf"] = "Sprig Labs";
    device["mdl"] = "ESP32-C3";
    device["sw"] = "1.0";
    device["cu"] = String("http://") + ip + "/discovery";
    serializeJson(doc, buffer1);
    MQTT_client.publish(topic, buffer1, true);

    // BATTERY LEVEL
    strcpy(topic, "homeassistant/sensor/");
    strcat(topic, devUniqueID);
    strcat(topic, "_level/config");
    strcpy(uid, devUniqueID);
    strcat(uid, "_level");
    doc.clear();
    doc["name"] = "Battery Level";
    doc["obj_id"] = "battery_level";
    doc["uniq_id"] = uid;
    doc["stat_t"] = "sprigC3/batLevel";
    doc["unit_of_meas"] = "%";
    doc["dev_cla"] = "battery";
    doc["val_tpl"] = "{{ value | float(0) }}";
    doc["frc_upd"] = true;
    JsonObject deviceL = doc.createNestedObject("device");
    deviceL["ids"] = devUniqueID;
    deviceL["name"] = "Sprig-C3 Battery Monitor";
    serializeJson(doc, buffer2);
    MQTT_client.publish(topic, buffer2, true);

    // // DEVICE IP (diagnostic) - publish as a sensor so HA shows the device IP
    // strcpy(topic, "homeassistant/sensor/");
    // strcat(topic, devUniqueID);
    // strcat(topic, "_ip/config");
    // strcpy(uid, devUniqueID);
    // strcat(uid, "_ip");
    // char buffer3[512];
    // doc.clear();
    // doc["name"] = "Device IP";
    // doc["obj_id"] = "device_ip";
    // doc["uniq_id"] = uid;
    // doc["stat_t"] = "sprigC3/deviceIP";
    // doc["val_tpl"] = "{{ value }}";
    // doc["ent_cat"] = "diagnostic";
    // doc["frc_upd"] = true;
    // JsonObject deviceIP = doc.createNestedObject("device");
    // deviceIP["ids"] = devUniqueID;
    // deviceIP["name"] = "Sprig-C3 Battery Monitor";
    // serializeJson(doc, buffer3);
    // MQTT_client.publish(topic, buffer3, true);

  } else {
    // Remove (publish empty payloads)
    strcpy(topic, "homeassistant/sensor/");
    strcat(topic, devUniqueID);
    strcat(topic, "_voltage/config");
    MQTT_client.publish(topic, "");
    strcpy(topic, "homeassistant/sensor/");
    strcat(topic, devUniqueID);
    strcat(topic, "_level/config");
    MQTT_client.publish(topic, "");
    // strcpy(topic, "homeassistant/sensor/");
    // strcat(topic, devUniqueID);
    // strcat(topic, "_ip/config");
    // MQTT_client.publish(topic, "");
  }
}

void loop() {
  // Check BOOT button for long-press factory reset
  if (digitalRead(BOOT) == LOW) { // pressed (active low)
    if (bootPressStart == 0) bootPressStart = millis();
    else if (millis() - bootPressStart >= BOOT_HOLD_MS) {
      Serial.println("Factory reset: clearing stored WiFi/MQTT credentials...");
      prefs.begin("sprig", false);
      prefs.clear();
      prefs.end();
      delay(200);
      ESP.restart();
    }
  } else {
    bootPressStart = 0;
  }

  // If we're in config-AP mode (no stored WiFi creds), handle web requests
  if (cfg_wifi_ssid.length() == 0) {
    server.handleClient();
    delay(2);
    return;
  }
  // Ensure MQTT connection
  if (!MQTT_client.connected()) {
    reconnect();
  }
  MQTT_client.loop();
  // Handle any incoming HTTP requests for runtime controls
  server.handleClient();
  // Publish device IP when it changes
  // publishDeviceIP();
  // Read and publish battery data every 5 seconds
  long now = millis();
  if (now - prev > 5000) {
    prev = now;
    // Read battery voltage and percentage
    float batVoltage = maxlipo.cellVoltage();
    char voltString[8];
    dtostrf(batVoltage, 1, 2, voltString);
    Serial.print(F("Batt Voltage: ")); Serial.print(batVoltage, 3); Serial.println(" V");
    Serial.println(voltString);
    // Publish to MQTT topics
    MQTT_client.publish("sprigC3/batVoltage", voltString);

    float batLevel = maxlipo.cellPercent();
    char levString[8];
    dtostrf(batLevel, 1, 2, levString);
    Serial.print(F("Batt Percent: ")); Serial.print(batLevel, 1); Serial.println(" %");
    MQTT_client.publish("sprigC3/batLevel", levString);
  }
}

// MQTT message callback — listens for discovery toggle commands
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.print("MQTT msg ["); Serial.print(topic); Serial.print("]: "); Serial.println(msg);

  if (String(topic) == String(discoverySetTopic)) {
    String v = msg;
    v.toLowerCase();
    if (v == "1" || v == "on" || v == "true") {
      if (!auto_discovery) {
        auto_discovery = true;
        haDiscovery();
        MQTT_client.publish(discoveryStateTopic, "ON", true);
      }
    } else if (v == "0" || v == "off" || v == "false") {
      if (auto_discovery) {
        auto_discovery = false;
        haDiscovery(); // publishes empty payloads to remove entities
        MQTT_client.publish(discoveryStateTopic, "OFF", true);
      }
    }
  }
}

// // Publish retained device IP when it changes (or when called)
// void publishDeviceIP() {
//   if (!MQTT_client.connected()) return;
//   String ip = WiFi.localIP().toString();
//   if (ip.length() == 0) return;
//   String url = String("http://") + ip + "/discovery";
//   if (url != lastPublishedIP) {
//     lastPublishedIP = url;
//     MQTT_client.publish("sprigC3/deviceIP", url.c_str(), true);
//   }
// }