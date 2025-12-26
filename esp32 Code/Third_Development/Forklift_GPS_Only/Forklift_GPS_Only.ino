// forklift_sender.ino (Channel Hopping)
// - Broadcast ESPNOW ke beberapa channel (mis: 6 dan 11)
// - GPS valid -> kirim update
// - GPS invalid -> heartbeat-only (flags=0)

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <TinyGPSPlus.h>   // Library: TinyGPSPlus

#define DEVICE_ID      1
#define SEND_INTERVAL  500  // ms (interval antar "cycle" kirim)

// ===== Channel hopping list =====
// Tambahkan/ubah sesuai channel Wi-Fi yang dipakai gateway kamu.
// Umumnya: 1 / 6 / 11
static const uint8_t HOP_CHANNELS[] = {2, 6, 11 };
static const size_t  HOP_COUNT = sizeof(HOP_CHANNELS) / sizeof(HOP_CHANNELS[0]);

// Delay kecil setelah pindah channel biar radio settle
#define HOP_SETTLE_MS  3

static const uint8_t BCAST_ADDR[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ===== GPS setup (ubah sesuai wiring) =====
static const int GPS_RX_PIN = 16;  // ESP32 RX (dari TX GPS)
static const int GPS_TX_PIN = 17;  // ESP32 TX (ke RX GPS)
static const uint32_t GPS_BAUD = 9600;

HardwareSerial GPSSerial(2);
TinyGPSPlus gps;

// ===== Payload ESPNOW =====
typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  int32_t  lat_e7;   // latitude * 1e7 (0 kalau invalid)
  int32_t  lon_e7;   // longitude * 1e7 (0 kalau invalid)
  uint8_t  flags;    // bit0: gps_valid
} pkt_t;

static pkt_t pkt;
static uint32_t seqNo = 0;

// helper convert
static int32_t to_e7(double deg) {
  return (int32_t) llround(deg * 1e7);
}

// Baca GPS (valid bila location valid & masih fresh)
static bool readGpsFix(double &lat, double &lon) {
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  // valid + tidak stale (age < 5 detik)
  if (gps.location.isValid() && gps.location.age() < 5000) {
    lat = gps.location.lat();
    lon = gps.location.lng();
    return true;
  }
  return false;
}

void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  // Callback ini dipanggil setiap esp_now_send() (jadi bisa 2x per cycle karena hopping)
  Serial.printf("SEND_CB: %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

static void setChannel(uint8_t ch) {
  // Set channel STA radio (ESPNOW ikut channel ini)
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  delay(HOP_SETTLE_MS);
}

static void sendBroadcastOnChannel(uint8_t ch) {
  setChannel(ch);

  esp_err_t s = esp_now_send(BCAST_ADDR, (uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[SENDER] CH=%u esp_now_send=%d (seq=%u len=%u)\n",
                ch, (int)s, pkt.seq, (unsigned)sizeof(pkt));
}

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

  // Set channel awal ke channel pertama (optional)
  setChannel(HOP_CHANNELS[0]);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[SENDER] ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onSent);

  // Add broadcast peer (wajib untuk broadcast send)
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST_ADDR, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0;     // 0 = current channel (kita hopping manual)
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

  // Naikkan seq 1x per cycle (paket yang sama dikirim ke semua channel)
  pkt.seq = ++seqNo;

  if (gpsValid) {
    pkt.flags = 0x01; // gps_valid
    pkt.lat_e7 = to_e7(lat);
    pkt.lon_e7 = to_e7(lon);
    Serial.printf("[SENDER] GPS OK  seq=%u lat=%.7f lon=%.7f\n", pkt.seq, lat, lon);
  } else {
    pkt.flags = 0x00;
    pkt.lat_e7 = 0;
    pkt.lon_e7 = 0;
    Serial.printf("[SENDER] HB ONLY seq=%u (gps invalid)\n", pkt.seq);
  }

  // ===== Channel hopping send =====
  for (size_t i = 0; i < HOP_COUNT; i++) {
    sendBroadcastOnChannel(HOP_CHANNELS[i]);
  }

  delay(SEND_INTERVAL);
}
