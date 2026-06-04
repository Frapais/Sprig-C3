/*
 * ============================================================================
 *  Sprig-C6 Air Filter  -  Home Assistant MQTT device
 * ============================================================================
 *
 *  Hardware:
 *    - Sprig-C6 board (ESP32-C6)            https://github.com/Frapais/Sprig-C6
 *    - Sensirion SPS30 particulate sensor   (I2C, address 0x69)
 *    - 4-pin PWM PC fan                      (PWM control signal on GPIO 2)
 *
 *  Wiring:
 *    SPS30  -> Sprig-C6
 *      VDD  -> 5V   (USB 5V rail. The SPS30 REQUIRES ~5V to read correctly.
 *                    Its I2C lines are 3.3V tolerant, so 3.3V logic is fine.)
 *      SDA  -> GPIO 6
 *      SCL  -> GPIO 7
 *      SEL  -> GND  (MUST be tied to GND at power-up to select I2C mode)
 *      GND  -> GND
 *
 *    4-pin PC fan (12V fan):
 *      Pin 1 (black)  GND   -> common GND (shared with the ESP32 GND)
 *      Pin 2 (yellow) +12V  -> external 12V supply (NOT from the Sprig)
 *      Pin 3 (green)  TACH  -> not used here (optional)
 *      Pin 4 (blue)   PWM   -> GPIO 2
 *      The fan's PWM input is nominally 5V logic. Most fans accept the 3.3V
 *      level from the ESP32, but a 3.3V->5V level shifter on GPIO 2 is more
 *      reliable. GPIO 2 only carries the 25 kHz control signal; the fan is
 *      powered from its own 12V supply.
 *      NOTE: at 0% PWM many fans keep spinning at their minimum RPM. If you
 *      need a guaranteed hard OFF, switch the fan's 12V line with a logic-level
 *      MOSFET (see FAN_POWER_PIN below, disabled by default).
 *
 *  Home Assistant:
 *    - Uses MQTT auto-discovery, so the device + entities appear automatically
 *      under Settings -> Devices & Services -> MQTT.
 *    - Exposes one Fan entity (on/off + speed slider) and SPS30 sensors
 *      (PM1.0, PM2.5, PM4.0, PM10, typical particle size).
 *
 *  First-time setup (no WiFi credentials are stored in this sketch):
 *    1. Flash this sketch to the Sprig-C6.
 *    2. On first boot it creates a WiFi access point:  "SprigC6-AirFilter-Setup"
 *    3. Connect a phone/PC to that AP. A setup page opens automatically
 *       (or browse to http://192.168.4.1). Enter your WiFi + MQTT details.
 *    4. Save & Reboot. The device joins your network and connects to MQTT.
 *    To wipe the stored credentials, hold the BOOT button for ~5 seconds.
 *
 *  Required libraries (Arduino Library Manager):
 *    - "Sensirion I2C SPS30"   (pulls in dependency "Sensirion Core")
 *    - "PubSubClient"          (Nick O'Leary)
 *    Board package: esp32 by Espressif, v3.0.0 or newer (needed for ESP32-C6).
 *    Select board: "ESP32C6 Dev Module".
 * ============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <SensirionI2cSps30.h>

// Make sure NO_ERROR is 0 (the SPS30 driver uses this convention)
#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

// ----------------------------------------------------------------------------
//  Pin configuration  (Sprig-C6)
// ----------------------------------------------------------------------------
static const int I2C_SDA   = 6;     // Sprig-C6 I2C SDA
static const int I2C_SCL   = 7;     // Sprig-C6 I2C SCL
static const int FAN_PIN   = 2;     // 4-pin fan PWM (blue wire)
static const int BOOT_P  = 9;     // BOOT button -> hold to factory reset

// Optional: drive a MOSFET gate to fully cut fan 12V power on OFF.
// Set to a valid GPIO to enable, or -1 to disable.
static const int FAN_POWER_PIN = -1;

// ----------------------------------------------------------------------------
//  Fan PWM configuration
// ----------------------------------------------------------------------------
static const int FAN_PWM_FREQ = 25000;  // 25 kHz, standard for PC fans (silent)
static const int FAN_PWM_RES  = 8;      // 8-bit resolution -> duty 0..255

// ----------------------------------------------------------------------------
//  Defaults (left blank on purpose - they are filled in via the setup portal)
// ----------------------------------------------------------------------------
static const char* DEFAULT_MQTT_SERVER = "";
static const char* DEFAULT_MQTT_USER   = "";
static const char* DEFAULT_MQTT_PASS   = "";
static const int   DEFAULT_MQTT_PORT   = 1883;

// ----------------------------------------------------------------------------
//  Globals
// ----------------------------------------------------------------------------
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WebServer    server(80);
DNSServer    dnsServer;
Preferences  prefs;
SensirionI2cSps30 sps30;

// Stored configuration
String cfg_wifi_ssid;
String cfg_wifi_pass;
String cfg_mqtt_server;
String cfg_mqtt_user;
String cfg_mqtt_pass;
int    cfg_mqtt_port = DEFAULT_MQTT_PORT;

bool   apMode = false;                       // true while running the setup AP
const char* AP_SSID = "SprigC6-AirFilter-Setup";

// Unique device id (built from MAC) used for topics + HA discovery
byte   macAddr[6];
char   uidPrefix[] = "sprigC6";
char   devUniqueID[32];

// MQTT topic strings (built once the unique id is known)
String baseTopic;        // sprigC6XXXXXX
String statusTopic;      // <base>/status        (availability / LWT)
String sps30Topic;       // <base>/sps30         (JSON sensor payload)
String fanCmdTopic;      // <base>/fan/cmd       (ON / OFF)
String fanStateTopic;    // <base>/fan/state     (ON / OFF)
String fanPctCmdTopic;   // <base>/fan/pct/set   (1..100)
String fanPctStateTopic; // <base>/fan/pct       (1..100)

// Fan state
bool fanOn      = false;
int  fanPercent = 100;   // last requested speed (1..100)

// Timing
unsigned long lastPublish   = 0;
const unsigned long PUBLISH_INTERVAL = 5000;  // ms between SPS30 publishes
unsigned long bootPressStart = 0;
const unsigned long BOOT_HOLD_MS = 3000;      // hold time for factory reset

// SPS30 status
bool sps30Ok = false;
char errorMessage[64];

// ============================================================================
//  Fan helpers
// ============================================================================
int dutyFromPercent(int pct) {
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return (pct * ((1 << FAN_PWM_RES) - 1)) / 100;   // 0..255
}

void applyFan() {
  int duty = fanOn ? dutyFromPercent(fanPercent) : 0;
  ledcWrite(FAN_PIN, duty);
  if (FAN_POWER_PIN >= 0) {
    digitalWrite(FAN_POWER_PIN, fanOn ? HIGH : LOW);
  }
  Serial.printf("Fan -> %s  %d%%  (duty %d)\n", fanOn ? "ON" : "OFF", fanPercent, duty);
}

void saveFanState() {
  prefs.putBool("fan_on", fanOn);
  prefs.putInt("fan_pct", fanPercent);
}

void publishFanState() {
  if (!mqtt.connected()) return;
  mqtt.publish(fanStateTopic.c_str(), fanOn ? "ON" : "OFF", true);
  char pctStr[8];
  snprintf(pctStr, sizeof(pctStr), "%d", fanPercent);
  mqtt.publish(fanPctStateTopic.c_str(), pctStr, true);
}

// ============================================================================
//  Unique ID  (prefix + last 3 bytes of MAC)
// ============================================================================
void createDiscoveryUniqueID() {
  strcpy(devUniqueID, uidPrefix);
  int pre = strlen(uidPrefix);
  int j = 0;
  for (int i = 2; i >= 0; i--) {
    sprintf(&devUniqueID[pre + j], "%02X", macAddr[i]);
    j += 2;
  }
  Serial.print("Unique ID: ");
  Serial.println(devUniqueID);
}

void buildTopics() {
  baseTopic        = String(devUniqueID);
  statusTopic      = baseTopic + "/status";
  sps30Topic       = baseTopic + "/sps30";
  fanCmdTopic      = baseTopic + "/fan/cmd";
  fanStateTopic    = baseTopic + "/fan/state";
  fanPctCmdTopic   = baseTopic + "/fan/pct/set";
  fanPctStateTopic = baseTopic + "/fan/pct";
}

// ============================================================================
//  Home Assistant MQTT discovery
// ============================================================================

// Shared compact device block. The full metadata is sent with the fan config;
// the sensors only need the matching "ids" to attach to the same device.
String deviceBlockFull() {
  String d = "\"dev\":{";
  d += "\"ids\":\"" + String(devUniqueID) + "\",";
  d += "\"name\":\"Sprig-C6 Air Filter\",";
  d += "\"mf\":\"Sprig Labs\",";
  d += "\"mdl\":\"ESP32-C6\",";
  d += "\"sw\":\"1.0\"}";
  return d;
}
String deviceBlockShort() {
  return "\"dev\":{\"ids\":\"" + String(devUniqueID) + "\"}";
}

void publishFanDiscovery() {
  String topic = "homeassistant/fan/" + String(devUniqueID) + "/config";
  String p = "{";
  p += "\"name\":\"Fan\",";
  p += "\"uniq_id\":\"" + String(devUniqueID) + "_fan\",";
  p += "\"avty_t\":\"" + statusTopic + "\",";
  p += "\"cmd_t\":\""  + fanCmdTopic + "\",";
  p += "\"stat_t\":\"" + fanStateTopic + "\",";
  p += "\"pct_cmd_t\":\"" + fanPctCmdTopic + "\",";
  p += "\"pct_stat_t\":\"" + fanPctStateTopic + "\",";
  p += "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",";
  p += "\"spd_rng_min\":1,\"spd_rng_max\":100,";
  p += deviceBlockFull();
  p += "}";
  mqtt.publish(topic.c_str(), p.c_str(), true);
}

// devClass may be "" to omit it (e.g. PM4.0 and particle size have no HA class)
void publishSensorDiscovery(const char* idSuffix, const char* name,
                            const char* jsonKey, const char* unit,
                            const char* devClass) {
  String topic = "homeassistant/sensor/" + String(devUniqueID) + "_" + idSuffix + "/config";
  String p = "{";
  p += "\"name\":\"" + String(name) + "\",";
  p += "\"uniq_id\":\"" + String(devUniqueID) + "_" + idSuffix + "\",";
  p += "\"avty_t\":\"" + statusTopic + "\",";
  p += "\"stat_t\":\"" + sps30Topic + "\",";
  p += "\"unit_of_meas\":\"" + String(unit) + "\",";
  if (devClass && strlen(devClass) > 0) {
    p += "\"dev_cla\":\"" + String(devClass) + "\",";
  }
  p += "\"stat_cla\":\"measurement\",";
  p += "\"val_tpl\":\"{{ value_json." + String(jsonKey) + " }}\",";
  p += deviceBlockShort();
  p += "}";
  mqtt.publish(topic.c_str(), p.c_str(), true);
}

void publishDiscovery() {
  publishFanDiscovery();
  publishSensorDiscovery("pm1",  "PM1.0",               "pm1",  "µg/m³", "pm1");
  publishSensorDiscovery("pm25", "PM2.5",               "pm25", "µg/m³", "pm25");
  publishSensorDiscovery("pm4",  "PM4.0",               "pm4",  "µg/m³", "");
  publishSensorDiscovery("pm10", "PM10",                "pm10", "µg/m³", "pm10");
  publishSensorDiscovery("tps",  "Typical Particle Size","tps", "µm",    "");
}

// ============================================================================
//  WiFi setup portal (AP mode)
// ============================================================================
String configPage() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' "
                "content='width=device-width,initial-scale=1'>"
                "<title>Sprig-C6 Air Filter Setup</title>"
                "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
                "input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box}"
                "button{width:100%;padding:12px;font-size:16px}</style></head><body>";
  html += "<h2>Sprig-C6 Air Filter</h2><p>Enter your WiFi and MQTT broker details.</p>";
  html += "<form method='POST' action='/save'>";
  html += "<label>WiFi SSID</label><input name='wifi_ssid' maxlength='32'>";
  html += "<label>WiFi Password</label><input name='wifi_pass' type='password' maxlength='64'>";
  html += "<label>MQTT Server (IP or host)</label><input name='mqtt_server' value='" + cfg_mqtt_server + "'>";
  html += "<label>MQTT Port</label><input name='mqtt_port' value='" + String(cfg_mqtt_port) + "'>";
  html += "<label>MQTT User</label><input name='mqtt_user' value='" + cfg_mqtt_user + "'>";
  html += "<label>MQTT Password</label><input name='mqtt_pass' type='password'>";
  html += "<button type='submit'>Save &amp; Reboot</button></form></body></html>";
  return html;
}

void startConfigPortal() {
  apMode = true;
  Serial.println("Starting WiFi setup portal...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(apIP);

  dnsServer.start(53, "*", apIP);   // captive portal: redirect all DNS to us

  server.on("/", []() { server.send(200, "text/html", configPage()); });

  server.on("/save", HTTP_POST, []() {
    prefs.putString("wifi_ssid",   server.arg("wifi_ssid"));
    prefs.putString("wifi_pass",   server.arg("wifi_pass"));
    prefs.putString("mqtt_server", server.arg("mqtt_server"));
    prefs.putInt   ("mqtt_port",   server.arg("mqtt_port").toInt());
    prefs.putString("mqtt_user",   server.arg("mqtt_user"));
    prefs.putString("mqtt_pass",   server.arg("mqtt_pass"));
    server.send(200, "text/html",
                "<html><body style='font-family:sans-serif'>"
                "<h3>Saved. Rebooting...</h3></body></html>");
    delay(600);
    ESP.restart();
  });

  // Any other URL -> bounce to the setup page (captive-portal behaviour)
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
}

// ============================================================================
//  Status web page (normal/connected mode)
// ============================================================================
void startStatusServer() {
  server.on("/", []() {
    String html = "<!DOCTYPE html><html><head><meta name='viewport' "
                  "content='width=device-width,initial-scale=1'>"
                  "<meta http-equiv='refresh' content='5'>"
                  "<title>Sprig-C6 Air Filter</title>"
                  "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
                  "td{padding:4px 10px}</style></head><body>";
    html += "<h2>Sprig-C6 Air Filter</h2>";
    html += "<p>Device ID: " + String(devUniqueID) + "<br>";
    html += "IP: " + WiFi.localIP().toString() + "<br>";
    html += "MQTT: " + String(mqtt.connected() ? "connected" : "disconnected") + "<br>";
    html += "SPS30: " + String(sps30Ok ? "ok" : "not detected") + "</p>";
    html += "<p>Fan: <b>" + String(fanOn ? "ON" : "OFF") + "</b> at " + String(fanPercent) + "%</p>";
    html += "<p><a href='/reset'>Erase WiFi/MQTT settings</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/reset", []() {
    server.send(200, "text/html",
                "<html><body style='font-family:sans-serif'>"
                "<h3>Settings erased. Rebooting into setup mode...</h3></body></html>");
    delay(400);
    prefs.clear();
    delay(200);
    ESP.restart();
  });

  server.begin();
}

// ============================================================================
//  WiFi / MQTT connection
// ============================================================================
void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(cfg_wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg_wifi_ssid.c_str(), cfg_wifi_pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    // If WiFi never connects (e.g. wrong password), fall back to setup portal
    if (millis() - start > 30000) {
      Serial.println("\nWiFi failed - returning to setup portal.");
      startConfigPortal();
      return;
    }
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void reconnectMQTT() {
  while (!mqtt.connected() && !apMode) {
    Serial.print("Connecting to MQTT...");
    String user = cfg_mqtt_user;
    String pass = cfg_mqtt_pass;
    // Connect with a Last-Will so HA shows "unavailable" if we drop off
    bool ok = mqtt.connect(devUniqueID,
                           user.length() ? user.c_str() : NULL,
                           pass.length() ? pass.c_str() : NULL,
                           statusTopic.c_str(), 0, true, "offline");
    if (ok) {
      Serial.println(" connected.");
      mqtt.publish(statusTopic.c_str(), "online", true);
      mqtt.subscribe(fanCmdTopic.c_str());
      mqtt.subscribe(fanPctCmdTopic.c_str());
      publishDiscovery();
      publishFanState();
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" - retrying in 5s");
      delay(5000);
    }
  }
}

// ============================================================================
//  MQTT message callback
// ============================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String t = String(topic);
  Serial.printf("MQTT [%s]: %s\n", topic, msg.c_str());

  if (t == fanCmdTopic) {
    String v = msg; v.toUpperCase();
    if (v == "ON") {
      fanOn = true;
      if (fanPercent <= 0) fanPercent = 100;
    } else if (v == "OFF") {
      fanOn = false;
    }
    applyFan();
    saveFanState();
    publishFanState();
  }
  else if (t == fanPctCmdTopic) {
    int pct = msg.toInt();
    if (pct <= 0) {
      fanOn = false;
    } else {
      if (pct > 100) pct = 100;
      fanPercent = pct;
      fanOn = true;
    }
    applyFan();
    saveFanState();
    publishFanState();
  }
}

// ============================================================================
//  SPS30
// ============================================================================
void initSPS30() {
  sps30.begin(Wire, SPS30_I2C_ADDR_69);
  sps30.stopMeasurement();            // ensure clean idle state
  delay(20);

  int8_t serial[32] = {0};
  if (sps30.readSerialNumber(serial, 32) == NO_ERROR) {
    Serial.print("SPS30 serial: ");
    Serial.println((const char*)serial);
    sps30Ok = true;
  } else {
    Serial.println("SPS30 not responding - check 5V supply, SEL->GND, wiring.");
    sps30Ok = false;
  }

  int16_t err = sps30.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT);
  if (err != NO_ERROR) {
    errorToString(err, errorMessage, sizeof(errorMessage));
    Serial.print("startMeasurement error: ");
    Serial.println(errorMessage);
  }
}

void publishSPS30() {
  if (!mqtt.connected()) return;

  uint16_t ready = 0;
  if (sps30.readDataReadyFlag(ready) != NO_ERROR || ready == 0) {
    return;   // no fresh sample yet
  }

  float mc1p0, mc2p5, mc4p0, mc10p0;
  float nc0p5, nc1p0, nc2p5, nc4p0, nc10p0, tps;
  int16_t err = sps30.readMeasurementValuesFloat(
      mc1p0, mc2p5, mc4p0, mc10p0,
      nc0p5, nc1p0, nc2p5, nc4p0, nc10p0, tps);
  if (err != NO_ERROR) {
    errorToString(err, errorMessage, sizeof(errorMessage));
    Serial.print("readMeasurementValues error: ");
    Serial.println(errorMessage);
    return;
  }
  sps30Ok = true;

  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"pm1\":%.1f,\"pm25\":%.1f,\"pm4\":%.1f,\"pm10\":%.1f,\"tps\":%.2f}",
           mc1p0, mc2p5, mc4p0, mc10p0, tps);
  mqtt.publish(sps30Topic.c_str(), payload);
  Serial.print("SPS30: "); Serial.println(payload);
}

// ============================================================================
//  Factory reset (hold BOOT)
// ============================================================================
void handleFactoryReset() {
  if (digitalRead(BOOT_P) == LOW) {          // active low
    if (bootPressStart == 0) bootPressStart = millis();
    else if (millis() - bootPressStart >= BOOT_HOLD_MS) {
      Serial.println("Factory reset - clearing stored settings.");
      prefs.clear();
      delay(200);
      ESP.restart();
    }
  } else {
    bootPressStart = 0;
  }
}

// ============================================================================
//  setup()
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nSprig-C6 Air Filter starting...");

  pinMode(BOOT_P, INPUT_PULLUP);

  // Fan PWM (ESP32 Arduino core 3.x LEDC API)
  ledcAttach(FAN_PIN, FAN_PWM_FREQ, FAN_PWM_RES);
  ledcWrite(FAN_PIN, 0);
  if (FAN_POWER_PIN >= 0) {
    pinMode(FAN_POWER_PIN, OUTPUT);
    digitalWrite(FAN_POWER_PIN, LOW);
  }

  // I2C + SPS30
  Wire.begin(I2C_SDA, I2C_SCL);
  initSPS30();

  // Load stored configuration
  prefs.begin("sprig", false);
  cfg_wifi_ssid   = prefs.getString("wifi_ssid", "");
  cfg_wifi_pass   = prefs.getString("wifi_pass", "");
  cfg_mqtt_server = prefs.getString("mqtt_server", DEFAULT_MQTT_SERVER);
  cfg_mqtt_user   = prefs.getString("mqtt_user", DEFAULT_MQTT_USER);
  cfg_mqtt_pass   = prefs.getString("mqtt_pass", DEFAULT_MQTT_PASS);
  cfg_mqtt_port   = prefs.getInt("mqtt_port", DEFAULT_MQTT_PORT);
  fanOn      = prefs.getBool("fan_on", false);
  fanPercent = prefs.getInt("fan_pct", 100);
  applyFan();

  if (cfg_wifi_ssid.length() == 0) {
    // No credentials yet -> launch the setup portal and wait there
    startConfigPortal();
    return;
  }

  connectWiFi();
  if (apMode) return;   // connectWiFi fell back to the portal

  // Read the MAC now that WiFi is initialised (it reads as 00:00:00 if done
  // before WiFi starts), then build the unique id + topic strings.
  WiFi.macAddress(macAddr);
  createDiscoveryUniqueID();
  buildTopics();

  if (cfg_mqtt_server.length() == 0) {
    Serial.println("WARNING: no MQTT server configured. Hold BOOT 5s to re-run setup.");
  }
  Serial.printf("MQTT target: %s:%d  user:'%s'\n",
                cfg_mqtt_server.c_str(), cfg_mqtt_port, cfg_mqtt_user.c_str());

  mqtt.setBufferSize(1024);   // discovery payloads can be large
  mqtt.setServer(cfg_mqtt_server.c_str(), cfg_mqtt_port);
  mqtt.setCallback(mqttCallback);

  startStatusServer();
}

// ============================================================================
//  loop()
// ============================================================================
void loop() {
  handleFactoryReset();

  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (apMode) return;
  }

  if (!mqtt.connected()) {
    reconnectMQTT();
  }
  mqtt.loop();
  server.handleClient();

  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    publishSPS30();
  }
}
