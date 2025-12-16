#include <TinyGPS++.h>
#include "painlessMesh.h"
#include <Arduino_JSON.h>

// ========== MESH ==========
#define MESH_PREFIX     "ciren"
#define MESH_PASSWORD   "ciren4171"
#define MESH_PORT       4171

int nodeNumber = 22;

// ========== GPS ==========
HardwareSerial GPS(2);   // UART2 ESP32
TinyGPSPlus gps;

// ========== MESH ==========
Scheduler userScheduler;
painlessMesh mesh;

// ====== PROTOTYPE ======
void sendMessage();
String getReadings(double lat, double lon, bool gpsValid);

// ====== TASK ======
Task taskSendMessage(TASK_SECOND * 1, TASK_FOREVER, &sendMessage);

// ====== JSON BUILDER ======
String getReadings(double lat, double lon, bool gpsValid) {
  JSONVar jsonReadings;
  jsonReadings["node"] = nodeNumber;
  
  if (gpsValid) {
    jsonReadings["latitude"] = lat;
    jsonReadings["longitude"] = lon;
    jsonReadings["gps_status"] = "valid";
  } else {
    jsonReadings["latitude"] = 0.0;
    jsonReadings["longitude"] = 0.0;
    jsonReadings["gps_status"] = "no_lock";
  }
  
  return JSON.stringify(jsonReadings);
}

// ====== SEND DATA ======
void sendMessage() {

  while (GPS.available()) {
    gps.encode(GPS.read());
  }

  if (gps.location.isValid()) {
    double lat = gps.location.lat();
    double lon = gps.location.lng();

    String msg = getReadings(lat, lon, true);
    mesh.sendBroadcast(msg);

    Serial.print("GPS LOCK - ");
    Serial.println(msg);
  } else {
    // Tetap kirim data walaupun GPS belum lock
    String msg = getReadings(0.0, 0.0, false);
    mesh.sendBroadcast(msg);
    
    Serial.print("GPS NO LOCK - ");
    Serial.println(msg);
  }
}

// ====== CALLBACK ======
void receivedCallback(uint32_t from, String &msg) {
  Serial.printf("Received from %u msg=%s\n", from, msg.c_str());

  JSONVar obj = JSON.parse(msg);

  Serial.print("Node: ");
  Serial.println((int)obj["node"]);

  Serial.print("GPS Status: ");
  Serial.println((const char*)obj["gps_status"]);

  Serial.print("Latitude: ");
  Serial.println((double)obj["latitude"], 6);

  Serial.print("Longitude: ");
  Serial.println((double)obj["longitude"], 6);
}

void newConnectionCallback(uint32_t nodeId) {
  Serial.printf("New Connection: %u\n", nodeId);
}

void changedConnectionCallback() {
  Serial.println("Changed connections");
}

void nodeTimeAdjustedCallback(int32_t offset) {
  Serial.printf("Time adjusted. Offset=%d\n", offset);
}

// ====== SETUP ======
void setup() {
  Serial.begin(115200);

  // GPS UART
  GPS.begin(9600, SERIAL_8N1, 16, 17);

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);

  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  userScheduler.addTask(taskSendMessage);
  taskSendMessage.enable();

  Serial.println("GPS + Mesh Started");
}

// ====== LOOP ======
void loop() {
  mesh.update();
}