/*
  NOTE: You must rename 'credentials_template.h' to 'credentials.h' and put in your own network credentials!

  This ESP32 code controls a heat pump that supports the "Smart Grid Ready" (SG Ready) feature.
  We only support two of the SG Ready modes:

    mode 0: normal operation
    mode 1: electricity is free or inexpensive, use is encouraged

  The SG Ready standard requires that we not change the state of the switch more often than every
  10 minutes. In order to clearly show what is going on with the controller at any given moment,
  we implement a device with two entities:

    - A switch that controls the desired mode ("Excess": boolean, true when electricity is free/inexpensive, false otherwise)
    - A sensor that reflects the current mode ("Mode": integer, 0 = normal operation, 1 = excess mode)

  We periodically publish the sensor state in order to solicit an MQTT ACK. We use the presence of this ACK as proof
  that the MQTT broker is still available and functioning. If we receive no ACKs after threee publishes, we consider
  the MQTT broker offline and we:
    - Revert the heat pump to Normal mode, obeying the state transition time requirement
    - Ensure that the pump is in normal mode every so often as an added precaution
  
   This device is published to the Home Assistant MQTT discovery topic.
*/

#include <WiFi.h>
extern "C" {
	#include "freertos/FreeRTOS.h"
	#include "freertos/timers.h"
}

#include <limits.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>

#include <Wire.h>
#include <SSD1306.h>

#include "credentials.h" // NOTE: You must rename 'credentials_template.h' to 'credentials.h' and put in your own network credentials!

#define REMOVE_HA_DEVICE 0  // set to 1 to erase the device added to Home Assistant

// the defines below are not user-configurable
#define MIN_STATE_SECONDS 600  // update the 'SG Ready' mode no more often than every 10 minutes
#define MQTT_KEEPALIVE_INTERVAL uint32_t(MIN_STATE_SECONDS/10) // how often we send keepalive messages to the mqtt server
#define MQTT_DEAD_TIME uint32_t(MQTT_KEEPALIVE_INTERVAL*3) // how long we go without an mqtt response before considering it offline

#define SG_PIN_LSB 25  // the low bit of the two digit SG Ready mode value; we never alter the high bit (pin is ok while using wifi if not software-connected to internal ADC2 circuit)

#define OLED_HEIGHT 64
#define OLED_WIDTH 128

// mqtt sensor data for HomeAssistant (https://www.youtube.com/watch?v=5JHKJy21vKA)
const char*         g_deviceModel = "ESP32Device";      // Hardware Model
const char*         g_swVersion = "1.0";                // Firmware Version
const char*         g_manufacturer = "Bud Millwood";    // Manufacturer Name
const char*         g_deviceName = "SGReady";           // Device Name
const char*         g_excessName = "Excess";            // Excess entity switch
const char*         g_modeName = "Mode";                // SG Ready mode state
bool                g_excess = false;                   // true = electricity overproduction / use encouraged, false = normal operation
int                 g_currentMode = 0;                  // current SG Ready mode
uint32_t            g_mqttLastResponseTime = 0;             // set to g_currentStateTime when mqtt responds
uint32_t            g_currentStateTime = 0;          // number of seconds we have been in the current state; unsigned is very important for wrap-around behavior!

AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;
TimerHandle_t countdownTimer;

SSD1306  display(0x3c, 5, 4);
static char display_buf[100];

void DrawDisplay() {
  display.clear();
  int y = 0;
  display.drawStringf(0, y+=10, display_buf, "WiFi: %s", WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "0.0.0.0");
  display.drawStringf(0, y+=10, display_buf, "MQTT: %s", mqttClient.connected() ? "connected" : "disconnected");
  display.drawStringf(0, y+=10, display_buf, "SG Mode: %i",g_currentMode);
  display.drawStringf(0, y+=10, display_buf, "Excess: %s",g_excess ? "true" : "false");
  display.drawStringf(0, y+=10, display_buf, "Remaining: %i",MIN_STATE_SECONDS-g_currentStateTime);
  display.display();
}

void connectToWifi() {
  Serial.println("Connecting to Wi-Fi...");
  DrawDisplay();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void connectToMqtt() {
  Serial.println("Connecting to MQTT...");
  DrawDisplay();
  mqttClient.connect();
}

void setPins() {
  Serial.printf("Setting pins for mode %i.\n",g_currentMode);
  digitalWrite(SG_PIN_LSB, g_currentMode ? HIGH : LOW);
}

String uniqueID(AsyncMqttClient& c) {
//  auto s = String(c.getClientId());
//  s.replace('-','_');
//  return s;
  return "sgready_board";  // we're using a fixed id in order to be able to easily replace this board if it fails
}

String entityTopic(String name)
{
  return uniqueID(mqttClient) + "_" + name;
}

// publish the control switch state
void mqttPublishExcess() {
  Serial.printf("Publishing excess '%s'.\n",g_excess ? "ON":"OFF");
  auto topic = entityTopic(g_excessName) + "/state";
  mqttClient.publish(topic.c_str(), 1, true, g_excess ? "ON" : "OFF");
}

// publish the current SG Ready mode
void mqttPublishMode() {
  Serial.printf("Publishing mode %i.\n",g_currentMode);
  auto topic = entityTopic(g_modeName) + "/state";
  mqttClient.publish(topic.c_str(), 1, true, String(g_currentMode).c_str());
}

// auto-restarting countdown timer has expired
void updateMode() {
  DrawDisplay();

  // solicit keep-alive by publishing our mode
  if (g_currentStateTime % MQTT_KEEPALIVE_INTERVAL == 0)
    mqttPublishMode();

  // stay in the current state for at least 10 minutes
  if (++g_currentStateTime < MIN_STATE_SECONDS)
    return;

  // how long since we last heard an ACK from the MQTT server?
  uint32_t mqttDiff = g_currentStateTime - g_mqttLastResponseTime;

  if (mqttDiff > MQTT_DEAD_TIME) {  // if no mqtt response for this long it's dead
    if (g_excess) {
      Serial.printf("No MQTT response received in %u seconds, reverting to normal mode.\n",mqttDiff);
      g_excess = false;
    }
    else {  // ensure our pins are in normal mode every so often as an added precaution
      if (g_currentStateTime % 30 == 0) {
        Serial.print("Paranoid pin set: ");
        setPins();  // paranoid set pins
      }
      return;
    }
  }

  // do nothing if no state change requested
  if (g_currentMode == g_excess)
    return;

  g_currentStateTime = 0;
  g_currentMode = g_excess;
  setPins();
  mqttPublishMode();
  mqttPublishExcess();
  DrawDisplay();
}

void WiFiEvent(WiFiEvent_t event) {
  switch(event) {
    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.print("WiFi connected: ");
      Serial.println(WiFi.localIP());
      connectToMqtt();
    break;

    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("WiFi disconnected");
      WiFi.disconnect();  // clear everything, this is important because otherwise we can fail to reconnect using stale data
      xTimerStop(mqttReconnectTimer, 0); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
      xTimerStart(wifiReconnectTimer, 0);
    break;

    case SYSTEM_EVENT_WIFI_READY:
    case SYSTEM_EVENT_SCAN_DONE:
    case SYSTEM_EVENT_STA_START:
    case SYSTEM_EVENT_STA_STOP:
    case SYSTEM_EVENT_AP_STA_GOT_IP6: // we don't yet require ipv6
    case SYSTEM_EVENT_STA_CONNECTED:  // connected to the radio network only
    break;

    case SYSTEM_EVENT_STA_LOST_IP:
      Serial.println("Lost WiFi IP address.");
    break;

    default:
      Serial.printf("Unknown WiFi event %d\n", event);
    break;
  }
}

/* Device: SG Ready
   Entities: Excess (Control switch)
             Mode (Heat Pump SG Mode)
*/
void mqttHomeAssistantDiscovery()
{
  if(!mqttClient.connected())
  {
    Serial.println("Error: Failed to send Home Assistant Discovery. (MQTT not connected)");
    return;
  }

  StaticJsonDocument<600> jdoc;
  JsonObject device;
  JsonArray identifiers;
  String excessPayload,modePayload;

  // excess switch, json configuration
  jdoc["name"] = g_excessName;
  jdoc["uniq_id"] = entityTopic(g_excessName);
  jdoc["dev_cla"] = "switch";
  jdoc["state_topic"] = entityTopic(g_excessName) + "/state";
  jdoc["command_topic"] = entityTopic(g_excessName) + "/set";
//        jdoc["availability_topic"] = entityTopic(g_excessName) + "/available";
  device = jdoc.createNestedObject("device");
  device["name"] = g_deviceName;
  device["model"] = g_deviceModel;
  device["sw_version"] = g_swVersion;
  device["manufacturer"] = g_manufacturer;
  identifiers = device.createNestedArray("identifiers");
  identifiers.add(uniqueID(mqttClient));

//  Serial.println("Excess config");
//  serializeJsonPretty(jdoc,Serial);
  serializeJson(jdoc, excessPayload);

  jdoc.clear();

  // mode sensor, json configuration
  jdoc["name"] = g_modeName;
  jdoc["uniq_id"] = "enum";
  jdoc["state_topic"] = entityTopic(g_modeName) + "/state";
//        jdoc["availability_topic"] = entityTopic(g_modeName) + "/available";
  device = jdoc.createNestedObject("device");
  device["name"] = g_deviceName;
  device["model"] = g_deviceModel;
  device["sw_version"] = g_swVersion;
  device["manufacturer"] = g_manufacturer;
  identifiers = device.createNestedArray("identifiers");
  identifiers.add(uniqueID(mqttClient));

//  Serial.println("Mode config");
//  serializeJsonPretty(jdoc,Serial);
  serializeJson(jdoc, modePayload);

#if REMOVE_HA_DEVICE
  excessPayload = "";
  modePayload = "";
#endif

  Serial.println("Sending Home Assistant Discovery...");

  auto topic = String("homeassistant/switch/") + entityTopic("excess") + "/config";
  mqttClient.publish(topic.c_str(), 1, true, excessPayload.c_str());
  mqttPublishExcess();

  topic = String("homeassistant/sensor/") + entityTopic("mode") + "/config";
  mqttClient.publish(topic.c_str(), 1, true, modePayload.c_str());
  mqttPublishMode();
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("MQTT connected.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);

  mqttHomeAssistantDiscovery();

  String topic = entityTopic(g_excessName) + "/set";
  uint16_t packetIdSub = mqttClient.subscribe(topic.c_str(), 1);
  DrawDisplay();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("MQTT disconnected.");
  if (WiFi.isConnected()) {
    xTimerStart(mqttReconnectTimer, 0);
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  Serial.println("Subscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
  Serial.print("  qos: ");
  Serial.println(qos);
}

void onMqttUnsubscribe(uint16_t packetId) {
  Serial.println("Unsubscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  auto sTopic = String(topic);
  auto sPayload = String(payload);

  g_excess = false;

  if (sTopic == entityTopic(g_excessName) + "/set") { // correct topic?
    if (sPayload == "ON")
      g_excess = true;  // valid 'on' command received
    else {
      if (sPayload != "OFF")
        Serial.printf("Error: Invalid MQTT payload '%s'.",payload);
    }
  }
  else
    Serial.printf("Error: MQTT message for unknown topic '%s'.",topic);

  mqttPublishExcess();  // reflect the updated state back to HA
  DrawDisplay();
}

void onMqttPublish(uint16_t packetId) {
//  Serial.print("MQTT alive, publish acknowledged for id: ");
//  Serial.println(packetId);
  g_mqttLastResponseTime = g_currentStateTime;
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  display.init();
  display.flipScreenVertically();
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(5000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(5000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToWifi));

  /* We start the countdown timer immediately, regardless of connection state. If no connection has been achieved by the time of expiration we will
     treat that as an error condition and revert to the default "normal mode".

     This timer expires repeatedly, meaning we are upcalled every 10 minutes without requiring a timer restart. This repeated timer expiration at the
     timer interval gives us multiple chances to revert to normal mode on the heat pump inputs if need be.
  */
  countdownTimer = xTimerCreate("countdownTimer", pdMS_TO_TICKS(1000), pdTRUE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(updateMode));
  xTimerStart(countdownTimer, 0);

  pinMode (SG_PIN_LSB,OUTPUT);
  setPins();

  WiFi.onEvent(WiFiEvent);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onSubscribe(onMqttSubscribe);
  mqttClient.onUnsubscribe(onMqttUnsubscribe);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCleanSession(true);
  mqttClient.setCredentials(MQTT_USER,MQTT_PASS);

  connectToWifi();
}

void loop() {
}
