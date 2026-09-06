#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <strings.h>  // strncasecmp
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include "ConfigSettings.h"
#include "MQTT.h"
#include "Somfy.h"
#include "Network.h"
#include "Utils.h"

WiFiClient tcpClient;
// Allocated the first time an mqtts:// broker is configured.  Keeping it off the plain
// path means installations that do not use TLS pay neither heap nor handshake for it.
static WiFiClientSecure *tlsClient = nullptr;
PubSubClient mqttClient(tcpClient);

#define MQTT_MAX_RESPONSE 2048
static char g_content[MQTT_MAX_RESPONSE];

extern ConfigSettings settings;
extern SomfyShadeController somfy;
extern Network net;
extern rebootDelay_t rebootDelay;


bool MQTTClass::begin() {
  this->suspended = false;
  return true;
}
bool MQTTClass::end() {
  this->suspended = true;
  this->disconnect();
  return true;
}
void MQTTClass::reset() {
  this->disconnect();
  this->lastConnect = 0;
  this->connect();
}
bool MQTTClass::loop() {
  if(settings.MQTT.enabled && !rebootDelay.reboot && !this->suspended && !mqttClient.connected()) {
    esp_task_wdt_reset();
    if(!this->connected() && net.connected()) this->connect();
  }
  esp_task_wdt_reset();
  if(settings.MQTT.enabled) mqttClient.loop();
  return true;
}
void MQTTClass::receive(const char *topic, byte*payload, uint32_t length) {
  esp_task_wdt_reset(); // Make sure we do not reboot here.
  Serial.print("MQTT Topic:");
  Serial.print(topic);
  Serial.print(" payload:");
  for(uint32_t i=0; i<length; i++)
    Serial.print((char)payload[i]);
  Serial.println();

  // We need to start at the last slash in the data
  uint8_t len = strlen(topic);
  
  uint8_t slashes = 0;
  uint16_t ndx = strlen(topic) - 1;
  while(ndx > 0) {
    if(topic[ndx] == '/') slashes++;
    if(slashes == 4) break;
    ndx--;
  }
  char entityId[4];
  char command[32];
  char entityType[7];
  char value[10];
  memset(command, 0x00, sizeof(command));
  memset(entityId, 0x00, sizeof(entityId));
  memset(entityType, 0x00, sizeof(entityType));
  memset(value, 0x00, sizeof(value));
  uint8_t i = 0;
  while(topic[ndx] == '/' && ndx < len) ndx++;
  while(ndx < len) {
    if(topic[ndx] != '/' && i < sizeof(entityType))
      entityType[i++] = topic[ndx];
    ndx++;
    if(topic[ndx] == '/') break;
  }
  i = 0;
  while(topic[ndx] == '/' && ndx < len) ndx++;
  while(ndx < len) {
    if(topic[ndx] != '/' && i < sizeof(entityId))
      entityId[i++] = topic[ndx];
    ndx++;
    if(topic[ndx] == '/') break;
  }
  i = 0;
  while(topic[ndx] == '/' && ndx < len) ndx++;
  while(ndx < len) {
    if(topic[ndx] != '/' && i < sizeof(command))
      command[i++] = topic[ndx];
    ndx++;
    if(topic[ndx] == '/') break;
  }
  for(uint8_t j = 0; j < length && j < sizeof(value); j++)
    value[j] = payload[j];
  
  Serial.print("MQTT type:[");
  Serial.print(entityType);
  Serial.print("] command:[");
  Serial.print(command);
  Serial.print("] entityId:");
  Serial.print(entityId);
  Serial.print(" value:");
  Serial.println(value);
  if(strncmp(entityType, "shades", sizeof(entityType)) == 0) {
    SomfyShade* shade = somfy.getShadeById(atoi(entityId));
    if (shade) {
      int val = atoi(value);
      if(strncmp(command, "target", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100)
          shade->moveToTarget(shade->transformPosition(atoi(value)));
      }
      if(strncmp(command, "tiltTarget", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100)
          shade->moveToTiltTarget(atoi(value));
      }
      else if(strncmp(command, "direction", sizeof(command)) == 0) {
        if(val < 0)
          shade->sendCommand(somfy_commands::Up);
        else if(val > 0)
          shade->sendCommand(somfy_commands::Down);
        else
          shade->sendCommand(somfy_commands::My);
      }
      else if(strncmp(command, "mypos", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100)
          shade->setMyPosition(val);
      }
      else if(strncmp(command, "myTiltPos", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100)
          shade->setMyPosition(shade->myPos, val);
      }
      else if(strncmp(command, "sunFlag", sizeof(command)) == 0) {
        if(val > 0) shade->sendCommand(somfy_commands::SunFlag);
        else shade->sendCommand(somfy_commands::Flag);
      }
      else if(strncmp(command, "position", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100) {
          shade->target = shade->currentPos = shade->transformPosition((float)val);
          shade->emitState();
        }
      }
      else if(strncmp(command, "tiltPosition", sizeof(command)) == 0) {
        if(val >= 0 && val <= 100) {
          shade->tiltTarget = shade->currentTiltPos = (float)val;
          shade->emitState();
        }
      }
      else if(strncmp(command, "sunny", sizeof(command)) == 0) {
        if(val >= 0) shade->sendSensorCommand(-1, val, shade->repeats);
      }
      else if(strncmp(command, "windy", sizeof(command)) == 0) {
        if(val >= 0) shade->sendSensorCommand(val, -1, shade->repeats);
      }
    }
  }
  else if(strncmp(entityType, "groups", sizeof(entityType)) == 0) {
    SomfyGroup* group = somfy.getGroupById(atoi(entityId));
    if (group) {
      int val = atoi(value);
      if(strncmp(command, "direction", sizeof(command)) == 0) {
        if(val < 0)
          group->sendCommand(somfy_commands::Up);
        else if(val > 0)
          group->sendCommand(somfy_commands::Down);
        else
          group->sendCommand(somfy_commands::My);
      }
      else if(strncmp(command, "sunFlag", sizeof(command)) == 0) {
        if(val > 0)
          group->sendCommand(somfy_commands::Flag);
        else
          group->sendCommand(somfy_commands::SunFlag);
      }
      else if(strncmp(command, "sunny", sizeof(command)) == 0) {
        if(val >= 0) group->sendSensorCommand(-1, val, group->repeats);
      }
      else if(strncmp(command, "windy", sizeof(command)) == 0) {
        if(val >= 0) group->sendSensorCommand(val, -1, group->repeats);
      }
    }
  }
  esp_task_wdt_reset(); // Make sure we do not reboot here.
}
bool MQTTClass::connect() {
  esp_task_wdt_reset(); // Make sure we do not reboot here.
  if(mqttClient.connected()) {
    if(!settings.MQTT.enabled || this->suspended)
      return this->disconnect();
    else
      return true;
  }
  if(settings.MQTT.enabled && !this->suspended) {
    if(this->lastConnect + 10000 > millis()) return false;    
    uint64_t mac = ESP.getEfuseMac();
    snprintf(this->clientId, sizeof(this->clientId), "client-%08x%08x", (uint32_t)((mac >> 32) & 0xFFFFFFFF), (uint32_t)(mac & 0xFFFFFFFF));
    if(strlen(settings.MQTT.protocol) > 0 && strlen(settings.MQTT.hostname) > 0) {
      // Pick the transport from the configured protocol.  mqtts:// wraps the session in TLS
      // so the broker credentials do not travel in the clear, which matters as soon as the
      // broker is not on the local network.  The TLS stack is already linked in for the
      // GitHub OTA client (GitOTA.cpp), so this costs no additional flash.
      if(strncasecmp(settings.MQTT.protocol, "mqtts", 5) == 0) {
        if(!tlsClient) {
          tlsClient = new WiFiClientSecure();
          // No CA certificate is stored on the device, so the broker is not authenticated.
          // The session is still encrypted, which is what keeps the credentials off the
          // wire.  This is the same trade the OTA client already makes with GitHub.
          if(tlsClient) tlsClient->setInsecure();
        }
        if(tlsClient) mqttClient.setClient(*tlsClient);
      }
      else mqttClient.setClient(tcpClient);
      mqttClient.setServer(settings.MQTT.hostname, settings.MQTT.port);
      char lwtTopic[128] = "status";
      if(strlen(settings.MQTT.rootTopic) > 0)
        snprintf(lwtTopic, sizeof(lwtTopic), "%s/status", settings.MQTT.rootTopic);
      esp_task_wdt_reset();
      // A TLS handshake takes a second or more on an ESP32 and it happens inside connect().
      // Step out of the task watchdog for its duration, the same way the file streaming code
      // does, so that a slow or unreachable broker cannot reboot the controller.
      esp_task_wdt_delete(NULL);
      bool didConnect = mqttClient.connect(this->clientId, settings.MQTT.username, settings.MQTT.password, lwtTopic, 0, true, "offline");
      esp_task_wdt_add(NULL);
      esp_task_wdt_reset();
      if(didConnect) {
        Serial.print("Successfully connected MQTT client ");
        Serial.println(this->clientId);
        this->publish("status", "online", true);
        this->publish("ipAddress", settings.IP.ip.toString().c_str(), true);
        this->publish("host", settings.hostname, true);
        this->publish("firmware", settings.fwVersion.name, true);
        this->publish("serverId", settings.serverId, true);
        this->publish("mac", net.mac.c_str());
        somfy.publish();
        this->subscribe("shades/+/target/set");
        this->subscribe("shades/+/tiltTarget/set");
        this->subscribe("shades/+/direction/set");
        this->subscribe("shades/+/mypos/set");
        this->subscribe("shades/+/myTiltPos/set");
        this->subscribe("shades/+/sunFlag/set");
        this->subscribe("shades/+/sunny/set");
        this->subscribe("shades/+/windy/set");
        this->subscribe("shades/+/position/set");
        this->subscribe("shades/+/tiltPosition/set");
        this->subscribe("groups/+/direction/set");
        this->subscribe("groups/+/sunFlag/set");
        this->subscribe("groups/+/sunny/set");
        this->subscribe("groups/+/windy/set");
        mqttClient.setCallback(MQTTClass::receive);
        Serial.println("MQTT Startup Completed");
        esp_task_wdt_reset();
        this->lastConnect = millis();
        return true;
      }
      else {
        Serial.print("MQTT Connection failed for: ");
        Serial.println(mqttClient.state());
        this->lastConnect = millis();
        return false;
      }
    }
    else
      return true;
  }
  return true;
}
bool MQTTClass::disconnect() {
  if(mqttClient.connected()) {
    this->unsubscribe("shades/+/target/set");
    this->unsubscribe("shades/+/direction/set");
    this->unsubscribe("shades/+/tiltTarget/set");
    this->unsubscribe("shades/+/mypos/set");
    this->unsubscribe("shades/+/myTiltPos/set");
    this->unsubscribe("shades/+/sunFlag/set");
    this->unsubscribe("groups/+/direction/set");
    this->unsubscribe("shades/+/sunny/set");
    this->unsubscribe("shades/+/windy/set");
    this->unsubscribe("shades/+/position/set");
    this->unsubscribe("shades/+/tiltPosition/set");
    this->unsubscribe("groups/+/direction/set");
    this->unsubscribe("groups/+/sunFlag/set");
    this->unsubscribe("groups/+/sunny/set");
    this->unsubscribe("groups/+/windy/set");
    mqttClient.disconnect();
  }
  return true;
}
bool MQTTClass::unsubscribe(const char *topic) {
  if(mqttClient.connected()) {
    char top[128];
    if(strlen(settings.MQTT.rootTopic) > 0)
      snprintf(top, sizeof(top), "%s/%s", settings.MQTT.rootTopic, topic);
    else
      strlcpy(top, topic, sizeof(top));
    Serial.print("MQTT Unsubscribed from:");
    Serial.println(top);
    return mqttClient.unsubscribe(top);
  }
  return true;
}
bool MQTTClass::subscribe(const char *topic) {
  if(mqttClient.connected()) {
    esp_task_wdt_reset(); // Make sure we do not reboot here.
    char top[128];
    if(strlen(settings.MQTT.rootTopic) > 0)
      snprintf(top, sizeof(top), "%s/%s", settings.MQTT.rootTopic, topic);
    else
      strlcpy(top, topic, sizeof(top));
    Serial.print("MQTT Subscribed to:");
    Serial.println(top);
    return mqttClient.subscribe(top);
  }
  return true;
}
bool MQTTClass::publish(const char *topic, const char *payload, bool retain) {
  if(mqttClient.connected()) {
    char top[128];
    if(strlen(settings.MQTT.rootTopic) > 0)
      snprintf(top, sizeof(top), "%s/%s", settings.MQTT.rootTopic, topic);
    else
      strlcpy(top, topic, sizeof(top));
    esp_task_wdt_reset(); // Make sure we do not reboot here.
    mqttClient.publish(top, payload, retain);
    return true;
  }
  return false;
}
bool MQTTClass::publish(const char *topic, uint32_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%u", val);
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::unpublish(const char *topic) {
  if(mqttClient.connected()) {
    char top[128];
    if(strlen(settings.MQTT.rootTopic) > 0)
      snprintf(top, sizeof(top), "%s/%s", settings.MQTT.rootTopic, topic);
    else
      strlcpy(top, topic, sizeof(top));
    esp_task_wdt_reset(); // Make sure we do not reboot here.
    mqttClient.publish(top, (const uint8_t *)"", 0, true);
    return true;
  }
  return false;

//  mqttClient.beginPublish(topic, 0, true);
//  mqttClient.endPublish();
}

bool MQTTClass::publishBuffer(const char *topic, uint8_t *data, uint16_t len, bool retain) {
  size_t res;
  uint16_t offset = 0;
  uint16_t to_write = len;
  uint16_t buff_len;
  esp_task_wdt_reset(); // Make sure we do not reboot here.
  mqttClient.beginPublish(topic, len, retain);
  do { 
    buff_len = to_write;
    // Every write() becomes its own TLS record once the transport is encrypted, and each
    // record carries about 29 bytes of overhead.  Larger chunks keep the ~2KB discovery
    // payloads down to a handful of records instead of sixteen.
    if(buff_len > 512) buff_len = 512;
    res = mqttClient.write(data+offset, buff_len);
    offset += buff_len;
    to_write -= buff_len;
  } while(res == buff_len && to_write > 0);
  mqttClient.endPublish();
  return true;
}
bool MQTTClass::publishDisco(const char *topic, JsonObject &obj, bool retain) {
  serializeJson(obj, g_content, sizeof(g_content));
  this->publishBuffer(topic, (uint8_t *)g_content, strlen(g_content), retain);
  return true;
}
bool MQTTClass::publish(const char *topic, int8_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%d", val);
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::publish(const char *topic, uint8_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%u", val);
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::publish(const char *topic, uint16_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%u", val);
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::publish(const char *topic, bool val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%s", val ? "true" : "false");
  return this->publish(topic, g_content, retain);
}
bool MQTTClass::connected() {
  if(settings.MQTT.enabled) return mqttClient.connected();
  return false;
}
