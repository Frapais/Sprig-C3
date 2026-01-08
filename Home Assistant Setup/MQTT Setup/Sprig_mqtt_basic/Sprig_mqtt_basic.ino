#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_MAX1704X.h"
#include "secrets.h"

Adafruit_MAX17048 maxlipo;

WiFiClient WiFi_client;
PubSubClient MQTT_client(WiFi_client);
long prev = 0;
char msg[50];
int value = 0;


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

  // Initialize WiFi and MQTT
  setup_wifi();
  MQTT_client.setServer(mqtt_server, 1883);

}

void setup_wifi() {
  delay(10);
  Serial.print("Connecting to ");
  Serial.println(SSID);
  WiFi.begin(SSID, PASSWORD);

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
    if (MQTT_client.connect(SSID, MQTT_user, MQTT_password)) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(MQTT_client.state());
      Serial.println(" trying again in 5 seconds");
      delay(5000);
    }
  }
}


void loop() {
  // Ensure MQTT connection
  if (!MQTT_client.connected()) {
    reconnect();
  }
  MQTT_client.loop();
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