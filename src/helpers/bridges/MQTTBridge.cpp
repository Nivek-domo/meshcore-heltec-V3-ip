#include "MQTTBridge.h"

#if defined(WITH_MQTT_BRIDGE)

#include <Arduino.h>
#include <WiFi.h>
#include <Arduino.h>

#include <helpers/bridges/BridgeBase.h>

// Access the global mesh instance so we can request a flood transmit for
// MQTT-injected packets. `mesh::Mesh::sendFlood()` is public so we can call
// it via the mesh base class without including example-specific headers.
#include <Mesh.h>
// Need access to example `MyMesh::injectReceivedRaw()` to emulate a radio
// reception when messages arrive on the `/sender` topic. Include the
// example header and reference the concrete instance.
#include "../../../examples/simple_repeater/MyMesh.h"
extern MyMesh the_mesh;

// Default values if not provided via build flags
#ifndef WIFI_SSID
#define WIFI_SSID "your_ssid"
#endif
#ifndef WIFI_PWD
#define WIFI_PWD "your_password"
#endif
#ifndef MQTT_BROKER
#define MQTT_BROKER "192.168.1.100"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_BASE_TOPIC
#define MQTT_BASE_TOPIC "meshcore/gateway"
#endif
#ifndef MQTT_PUBLISH_RAW_TOPIC
#define MQTT_PUBLISH_RAW_TOPIC "/raw"
#endif
#ifndef MQTT_PUBLISH_READABLE_TOPIC
#define MQTT_PUBLISH_READABLE_TOPIC "/readable"
#endif

MQTTBridge *g_instance = nullptr;

static void static_mqtt_callback(char *topic, uint8_t *payload, unsigned int length) {
  if (g_instance) {
    g_instance->handleMqttMessage(topic, payload, length);
  }
}

MQTTBridge::MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc), _client(_wifiClient) {
  g_instance = this;
}

void MQTTBridge::begin() {
  // connect to WiFi
  BRIDGE_DEBUG_PRINTLN("Starting WiFi connect to %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    BRIDGE_DEBUG_PRINTLN("WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    BRIDGE_DEBUG_PRINTLN("WiFi connection FAILED after %lu ms\n", millis() - start);
  }

  _client.setServer(MQTT_BROKER, MQTT_PORT);
  _client.setCallback(static_mqtt_callback);

  BRIDGE_DEBUG_PRINTLN("MQTT server set to %s:%d\n", MQTT_BROKER, MQTT_PORT);

  // prepare status topic using a stable gateway id derived from MAC
  String macHex = String((uint64_t)ESP.getEfuseMac(), HEX);
  macHex.toUpperCase();
  String gwId = macHex;
  _statusTopic = String(MQTT_BASE_TOPIC) + "/" + gwId + "/status";

  reconnect();

  _initialized_local = true;
  _initialized = true;
}

static int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void MQTTBridge::end() {
  _client.disconnect();
  WiFi.disconnect(true);
  _initialized_local = false;
  _initialized = false;
}

void MQTTBridge::reconnect() {
  if (_client.connected()) return;
  String clientId = "meshcore-gw-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  int attempt = 0;
  while (!_client.connected() && attempt < 5) {
    BRIDGE_DEBUG_PRINTLN("MQTT connecting attempt %d...\n", attempt + 1);
    if (_client.connect(clientId.c_str())) {
      // subscribe to incoming topic
      String macHex_local = String((uint64_t)ESP.getEfuseMac(), HEX);
      macHex_local.toUpperCase();
      String gwId_local = macHex_local;
      String inTopic = String(MQTT_BASE_TOPIC) + "/" + gwId_local + "/in";
      bool ok = _client.subscribe(inTopic.c_str());
      BRIDGE_DEBUG_PRINTLN("MQTT connected, clientId=%s, subscribed=%d to %s\n", clientId.c_str(), ok, inTopic.c_str());
      // subscribe also to the /hear topic to inject packets received via MQTT
      String hearTopic = String(MQTT_BASE_TOPIC) + "/" + gwId_local + "/hear";
      bool ok2 = _client.subscribe(hearTopic.c_str());
      BRIDGE_DEBUG_PRINTLN("MQTT subscribed to hear topic=%s ok=%d\n", hearTopic.c_str(), ok2);
      // subscribe to the /sender topic so external tools can request injection
      // (payload = ASCII-hex packet bytes). The handler may be implemented
      // later to call MyMesh::injectReceivedRaw or similar.
      String senderTopic = String(MQTT_BASE_TOPIC) + "/" + gwId_local + "/sender";
      bool ok3 = _client.subscribe(senderTopic.c_str());
      BRIDGE_DEBUG_PRINTLN("MQTT subscribed to sender topic=%s ok=%d\n", senderTopic.c_str(), ok3);
      // publish an immediate heartbeat/status message on connect
      sendHeartbeat();
      break;
    } else {
      BRIDGE_DEBUG_PRINTLN("MQTT connect FAILED, rc=%d\n", _client.state());
      attempt++;
      delay(1000);
    }
  }
}

void MQTTBridge::loop() {
  if (!_initialized_local) return;

  if (!_client.connected()) {
    reconnect();
  }

  _client.loop();

  // periodic heartbeat
  if (_client.connected()) {
    unsigned long now = millis();
    if (now - _lastHeartbeatMillis >= _heartbeatIntervalMs) {
      sendHeartbeat();
      _lastHeartbeatMillis = now;
    }
  }
}

static void toHex(const uint8_t *in, size_t len, char *out) {
  const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = hex[(in[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[in[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

void MQTTBridge::sendPacket(mesh::Packet *packet) {
  if (_initialized == false) return;
  if (!packet) return;

  if (!_seen_packets.hasSeen(packet)) {
    uint8_t buffer[512];
    uint16_t len = packet->writeTo(buffer);
    // publish raw binary
    String rawTopic = String(MQTT_BASE_TOPIC) + String(MQTT_PUBLISH_RAW_TOPIC);
    bool pub_ok = false;
    if (_client.connected()) {
      pub_ok = _client.publish(rawTopic.c_str(), buffer, len);
    } else {
      BRIDGE_DEBUG_PRINTLN("MQTT not connected, cannot publish raw\n");
    }
      BRIDGE_DEBUG_PRINTLN("TX publish raw len=%d ok=%d topic=%s\n", len, pub_ok, rawTopic.c_str());

    // publish readable hex representation
    const size_t hex_len = len * 2 + 1;
    char *hexbuf = (char *)malloc(hex_len);
    if (hexbuf) {
      toHex(buffer, len, hexbuf);
      String readableTopic = String(MQTT_BASE_TOPIC) + String(MQTT_PUBLISH_READABLE_TOPIC);
      String msg = String("time:") + getLogDateTime() + ",len:" + String(len) + ",data:" + String(hexbuf);
      bool pub_ok2 = false;
      if (_client.connected()) {
        pub_ok2 = _client.publish(readableTopic.c_str(), msg.c_str());
      }
      BRIDGE_DEBUG_PRINTLN("TX publish readable len=%d ok=%d topic=%s msg=%s\n", len, pub_ok2, readableTopic.c_str(), msg.c_str());
      free(hexbuf);
    }
  }
}
/*
void MQTTBridge::sendPacketForce(mesh::Packet *packet) {
  if (!_initialized || !_client.connected() || !packet) {
    return;
  }

  uint8_t buffer[512];
  uint16_t len = packet->writeTo(buffer);

  // Convertir le buffer binaire en ASCII hex
  const size_t hex_len = len * 2 + 1;
  char *hexbuf = (char *)malloc(hex_len);
  if (!hexbuf) return;

  // Cette fonction doit écrire un '\0' à la fin
  for (uint16_t i = 0; i < len; i++) {
    sprintf(hexbuf + i * 2, "%02X", buffer[i]);
  }
  hexbuf[len * 2] = '\0';  // Terminaison C-string

  String macHex_local = String((uint64_t)ESP.getEfuseMac(), HEX);
  macHex_local.toUpperCase();
  String gwId_local = macHex_local;
  String rawTopic = String(MQTT_BASE_TOPIC) + "/" + gwId_local + "/tx" + String(MQTT_PUBLISH_RAW_TOPIC);
  //bool ok = _client.publish(rawTopic.c_str(), hexbuf);
  bool ok = _client.publish(rawTopic.c_str(), buffer, len);

  BRIDGE_DEBUG_PRINTLN(
    "MQTT FORCE publish raw len=%d ok=%d topic=%s\n",
    len, ok, rawTopic.c_str()
  );
}*/
void MQTTBridge::sendPacketForce(mesh::Packet *packet) {
  if (!_initialized || !_client.connected() || !packet) {
    return;
  }

  uint8_t buffer[512];
  uint16_t len = packet->writeTo(buffer);

  // Conversion binaire → ASCII hex
  const size_t hex_len = len * 2 + 1;
  char hexbuf[hex_len];

  for (uint16_t i = 0; i < len; i++) {
    sprintf(&hexbuf[i * 2], "%02X", buffer[i]);
  }
  hexbuf[len * 2] = '\0';

  String macHex_local = String((uint64_t)ESP.getEfuseMac(), HEX);
  macHex_local.toUpperCase();

  String rawTopic = String(MQTT_BASE_TOPIC) + "/" +
                    macHex_local +
                    String(MQTT_PUBLISH_RAW_TOPIC)+ 
                    "/tx";

  bool ok = _client.publish(rawTopic.c_str(), hexbuf);

  BRIDGE_DEBUG_PRINTLN(
    "MQTT FORCE publish HEX len=%d ok=%d topic=%s\n",
    strlen(hexbuf), ok, rawTopic.c_str()
  );
}


void MQTTBridge::onPacketReceived(mesh::Packet *packet) {
  if (!packet) return;

  // Dump packet as hex for debug
  uint8_t tmpbuf[512];
  uint16_t len = packet->writeTo(tmpbuf);
  const size_t hex_len = len * 2 + 1;
  char *hexbuf = (char *)malloc(hex_len);
  if (hexbuf) {
    toHex(tmpbuf, len, hexbuf);
    BRIDGE_DEBUG_PRINTLN("RX packet received len=%d data=%s\n", len, hexbuf);
    free(hexbuf);
  }

  // Publish inbound packet to MQTT (raw + readable) so broker sees what we receive
  if (_client.connected()) {
    if (!_seen_packets.hasSeen(packet)) {
      String macHex_local = String((uint64_t)ESP.getEfuseMac(), HEX);
      macHex_local.toUpperCase();
      String gwId_local = macHex_local;
      String rawTopic = String(MQTT_BASE_TOPIC) + "/" + gwId_local + "/rx" + String(MQTT_PUBLISH_RAW_TOPIC);
      bool pub_ok = _client.publish(rawTopic.c_str(), tmpbuf, len);
      BRIDGE_DEBUG_PRINTLN("RX publish raw len=%d ok=%d topic=%s\n", len, pub_ok, rawTopic.c_str());

      char *hexbuf2 = (char *)malloc(hex_len);
      if (hexbuf2) {
        toHex(tmpbuf, len, hexbuf2);
        String readableTopic = String(MQTT_BASE_TOPIC) + "/" + gwId_local + "/rx" + String(MQTT_PUBLISH_READABLE_TOPIC);
        String msg = String("time:") + getLogDateTime() + ",len:" + String(len) + ",data:" + String(hexbuf2);
        bool pub_ok2 = _client.publish(readableTopic.c_str(), msg.c_str());
        BRIDGE_DEBUG_PRINTLN("RX publish readable len=%d ok=%d topic=%s msg=%s\n", len, pub_ok2, readableTopic.c_str(), msg.c_str());
        free(hexbuf2);
      }
    } else {
      BRIDGE_DEBUG_PRINTLN("RX packet already seen, not publishing to MQTT\n");
    }
  } else {
    BRIDGE_DEBUG_PRINTLN("MQTT not connected, skipping RX publish\n");
  }

  // Continue normal processing (forwarding, etc.)
  handleReceivedPacket(packet);
}

void MQTTBridge::handleMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
  if (length == 0) return;
  BRIDGE_DEBUG_PRINTLN("MQTT RX topic=%s len=%d\n", topic, length);

  // If this is a /hear message, treat payload as ASCII hex and inject
  // it as a received mesh packet into the bridge processing.
  String t(topic);
  if (t.endsWith("/hear")) {
    BRIDGE_DEBUG_PRINTLN("MQTT /hear payload received, parsing hex\n");
    if (length % 2 != 0) {
      BRIDGE_DEBUG_PRINTLN("MQTT /hear invalid hex length=%d\n", length);
      return;
    }
    size_t bytelen = length / 2;
    uint8_t *buf = (uint8_t *)malloc(bytelen);
    if (!buf) return;
    for (size_t i = 0; i < bytelen; i++) {
      int hi = hexValue(((char *)payload)[i * 2]);
      int lo = hexValue(((char *)payload)[i * 2 + 1]);
      if (hi < 0 || lo < 0) {
        BRIDGE_DEBUG_PRINTLN("MQTT /hear invalid hex char at index %d\n", (int)(i * 2));
        free(buf);
        return;
      }
      buf[i] = (uint8_t)((hi << 4) | lo);
    }

    mesh::Packet *pkt = _mgr->allocNew();
    if (!pkt) {
      free(buf);
      return;
    }
    if (pkt->readFrom(buf, (uint8_t)bytelen)) {
      BRIDGE_DEBUG_PRINTLN("MQTT /hear injecting packet len=%d\n", (int)bytelen);
      // Force retransmit: allocate a copy and send as flood so the MQTT-injected
      // packet is transmitted by radio like a normal RX. This bypasses
      // example-only headers and uses the public Mesh API.
      mesh::Packet *out = _mgr->allocNew();
      if (out) {
        out->header = pkt->header;
        out->path_len = pkt->path_len;
        out->payload_len = pkt->payload_len;
        if (out->hasTransportCodes()) {
          out->transport_codes[0] = pkt->transport_codes[0];
          out->transport_codes[1] = pkt->transport_codes[1];
        }
        if (out->path_len) memcpy(out->path, pkt->path, out->path_len);
        if (out->payload_len) memcpy(out->payload, pkt->payload, out->payload_len);
        // Dump both the original injected packet and the outgoing copy as HEX
        // so differences (transport codes, header, etc.) can be inspected.
        uint8_t tmp_in[512]; int in_len = pkt->writeTo(tmp_in);
        uint8_t tmp_out[512]; int out_len = out->writeTo(tmp_out);
        char hex_in[in_len*2 + 1];
        char hex_out[out_len*2 + 1];
        mesh::Utils::toHex(hex_in, tmp_in, in_len);
        mesh::Utils::toHex(hex_out, tmp_out, out_len);
        BRIDGE_DEBUG_PRINTLN("MQTT /hear injected HEX len=%d data=%s\n", in_len, hex_in);
        BRIDGE_DEBUG_PRINTLN("MQTT /hear outgoing HEX len=%d data=%s\n", out_len, hex_out);
        BRIDGE_DEBUG_PRINTLN("MQTT /hear forcing flood transmit len=%d\n", (int)out->getRawLength());
        the_mesh.sendFlood(out, (uint32_t)0);
      }
      onPacketReceived(pkt);
    } else {
      BRIDGE_DEBUG_PRINTLN("MQTT /hear failed to parse packet\n");
      _mgr->free(pkt);
    }
    free(buf);
    return;
  }

  // If this is a /sender message, decode ASCII-hex and inject as a raw
  // radio reception using the example MyMesh API so the packet follows the
  // exact same processing path as a normal radio RX.
  if (t.endsWith("/sender")) {
    BRIDGE_DEBUG_PRINTLN("MQTT /sender payload received, parsing hex\n");
    if (length % 2 != 0) {
      BRIDGE_DEBUG_PRINTLN("MQTT /sender invalid hex length=%d\n", length);
      return;
    }
    size_t bytelen = length / 2;
    uint8_t *buf2 = (uint8_t *)malloc(bytelen);
    if (!buf2) return;
    for (size_t i = 0; i < bytelen; i++) {
      int hi = hexValue(((char *)payload)[i * 2]);
      int lo = hexValue(((char *)payload)[i * 2 + 1]);
      if (hi < 0 || lo < 0) {
        BRIDGE_DEBUG_PRINTLN("MQTT /sender invalid hex char at index %d\n", (int)(i * 2));
        free(buf2);
        return;
      }
      buf2[i] = (uint8_t)((hi << 4) | lo);
    }

    // Default SNR/RSSI values for injected packets; these can be tuned.
    float snr = 8.0f;
    float rssi = -80.0f;

    BRIDGE_DEBUG_PRINTLN("MQTT /sender injecting as radio RX len=%d\n", (int)bytelen);
    the_mesh.injectReceivedRaw(buf2, (int)bytelen, snr, rssi);
    free(buf2);
    return;
  }

  // Otherwise assume payload is raw binary packet bytes - dump for debug then parse
  const size_t hex_len = length * 2 + 1;
  char *hexbuf = (char *)malloc(hex_len);
  if (hexbuf) {
    toHex(payload, length, hexbuf);
    BRIDGE_DEBUG_PRINTLN("MQTT RX data=%s\n", hexbuf);
    free(hexbuf);
  }

  mesh::Packet *pkt = _mgr->allocNew();
  if (!pkt) return;

  if (pkt->readFrom(payload, (uint8_t)length)) {
    onPacketReceived(pkt);
  } else {
    _mgr->free(pkt);
  }
}

void MQTTBridge::sendHeartbeat() {
  if (!_client.connected()) return;

  String ip = WiFi.localIP().toString();
  String msg = String("time:") + getLogDateTime() + ",ip:" + ip + ",status:online";
  bool ok = _client.publish(_statusTopic.c_str(), msg.c_str());
  //BRIDGE_DEBUG_PRINTLN("Heartbeat publish ok=%d topic=%s msg=%s\n", ok, _statusTopic.c_str(), msg.c_str());
}

#endif

bool MQTTBridge::publishRxHex(const uint8_t *buf, size_t len) {
  if (!_initialized || !_client.connected() || !buf || len == 0) {
    return false;
  }

  // Conversion binaire → ASCII hex
  char hexbuf[len * 2 + 1];
  mesh::Utils::toHex(hexbuf, buf, len);

  // Gateway ID
  String macHex_local = String((uint64_t)ESP.getEfuseMac(), HEX);
  macHex_local.toUpperCase();

  // Topic RX RAW
  String rxTopic = String(MQTT_BASE_TOPIC) + "/" +
                   macHex_local +
                   String(MQTT_PUBLISH_RAW_TOPIC) + "/rx";

  bool ok = _client.publish(rxTopic.c_str(), hexbuf);

  BRIDGE_DEBUG_PRINTLN(
    "MQTT RX HEX publish len=%d ok=%d topic=%s\n",
    len * 2, ok, rxTopic.c_str()
  );

  return ok;
}

