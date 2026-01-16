#pragma once

#include "helpers/bridges/BridgeBase.h"

#if defined(WITH_MQTT_BRIDGE)

#include <WiFi.h>
#include <PubSubClient.h>

class MQTTBridge : public BridgeBase {
public:
  MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);
  ~MQTTBridge() override = default;
  bool publishRxHex(const uint8_t *buf, size_t len);

  void begin();
  void end();
  void loop();

  void sendPacket(mesh::Packet *packet) override;
  void sendPacketForce(mesh::Packet *packet);

  void onPacketReceived(mesh::Packet *packet) override;

  void reconnect();
  void handleMqttMessage(char *topic, uint8_t *payload, unsigned int length);

private:
  void sendHeartbeat();

private:
  WiFiClient _wifiClient;
  PubSubClient _client;
  bool _initialized_local = false;
  unsigned long _lastHeartbeatMillis = 0;
  unsigned long _heartbeatIntervalMs = 60000; // default 60s
  String _statusTopic;
};

#endif
