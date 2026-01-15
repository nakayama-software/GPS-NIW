// forklift_sender.ino (GPS ONLY)
// last updated 1/15/2026

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <TinyGPSPlus.h>

#define DEVICE_ID 1
#define SEND_INTERVAL 500

static const uint8_t BCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// ===== GPS setup  =====
static const int GPS_RX_PIN = 16;  // ESP32 RX (From TX GPS)
static const int GPS_TX_PIN = 17;  // ESP32 TX (To RX GPS)
static const uint32_t GPS_BAUD = 9600;

HardwareSerial GPSSerial(2);
TinyGPSPlus gps;

// ===== Payload ESPNOW =====
typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  int32_t lat_e7;
  int32_t lon_e7;
  uint8_t flags;
} pkt_t;

static pkt_t pkt;
static uint32_t seqNo = 0;

// ===== GPS Section =====
static int32_t to_e7(double deg) {
  return (int32_t)llround(deg * 1e7);
}

static bool readGpsFix(double &lat, double &lon) {
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  if (gps.location.isValid() && gps.location.age() < 5000) {
    lat = gps.location.lat();
    lon = gps.location.lng();
    return true;
  }
  return false;
}

// ===== ESP NOW Section =====
// *Channel hopping list*
static const uint8_t HOP_CHANNELS[] = { 2, 6, 11 };
static const size_t HOP_COUNT = sizeof(HOP_CHANNELS) / sizeof(HOP_CHANNELS[0]);

#define HOP_SETTLE_MS 3

void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  Serial.printf("SEND_CB: %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

static void setChannel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  delay(HOP_SETTLE_MS);
}

static void sendBroadcastOnChannel(uint8_t ch) {
  setChannel(ch);

  esp_err_t s = esp_now_send(BCAST_ADDR, (uint8_t *)&pkt, sizeof(pkt));
  Serial.printf("[SENDER] CH=%u esp_now_send=%d (seq=%u len=%u)\n", ch, (int)s, pkt.seq, (unsigned)sizeof(pkt));
}

// ===== MAIN Section =====
void setup() {
  Serial.begin(115200);
  delay(200);

  // GPS init
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[SENDER] GPS UART started");

  // WiFi/ESPNOW init
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, true);

  esp_wifi_set_ps(WIFI_PS_NONE);

  setChannel(HOP_CHANNELS[0]);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[SENDER] ESP-NOW init failed");
    return;
  }
  
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST_ADDR, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0;
  peer.encrypt = false;

  esp_err_t e = esp_now_add_peer(&peer);
  Serial.printf("[SENDER] add_peer=%d\n", (int)e);

  pkt.node_id = DEVICE_ID;
  pkt.seq = 0;
  pkt.lat_e7 = 0;
  pkt.lon_e7 = 0;
  pkt.flags = 0;

  Serial.printf("[SENDER] Ready. node_id=%u hopping channels: ", pkt.node_id);
  for (size_t i = 0; i < HOP_COUNT; i++) {
    Serial.printf("%u%s", HOP_CHANNELS[i], (i + 1 < HOP_COUNT) ? "," : "\n");
  }
}

void loop() {
  double lat = 0, lon = 0;
  bool gpsValid = readGpsFix(lat, lon);

  pkt.seq = ++seqNo;

  if (gpsValid) {
    pkt.flags = 0x01;
    pkt.lat_e7 = to_e7(lat);
    pkt.lon_e7 = to_e7(lon);
    Serial.printf("[SENDER] GPS OK  seq=%u lat=%.7f lon=%.7f\n", pkt.seq, lat, lon);
  } else {
    pkt.flags = 0x00;
    pkt.lat_e7 = 0;
    pkt.lon_e7 = 0;
    Serial.printf("[SENDER] HB ONLY seq=%u (gps invalid)\n", pkt.seq);
  }

  // Channel hopping send
  for (size_t i = 0; i < HOP_COUNT; i++) {
    sendBroadcastOnChannel(HOP_CHANNELS[i]);
  }

  delay(SEND_INTERVAL);
}