#define ARDUINOJSON_USE_LONG_LONG 1

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <stdlib.h>
#include "config.h"

AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;

// WiFiEventHandler wifiConnectHandler;
// WiFiEventHandler wifiDisconnectHandler;
Ticker wifiReconnectTimer;

MFRC522 mfrc522(SS_PIN, RST_PIN);  // Create MFRC522 instance
#include "MFRC522_func.h"

MFRC522::MIFARE_Key key;
//MFRC522::StatusCode status;
byte sector         = 1;
byte blockAddr      = 4;
byte dataBlock[]    = {
        0x00, 0x00, 0x00, 0x00, //  byte 1 for color encoding
        0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x10  // byte 15 for event track bit[0] = burnerot2018, bit[1] = contra2019, bit[2] = Midburn2022, bit[3] = burnerot2022
    };
byte trailerBlock   = 7;
byte buffer[18];
byte size = sizeof(buffer);
bool read_success, write_success, auth_success;
byte PICC_version;
unsigned int readCard[4];

bool send_chip_data = true;
bool is_old_chip = false;
byte chip_color = 0x0;


void connectToMqtt() {
  Serial.println("[MQTT] Connecting to MQTT...");
  mqttClient.setClientId(WIFI_CLIENT_ID);
  mqttClient.setKeepAlive(5);
  mqttClient.setWill(MQTT_TOPIC_MONITOR,MQTT_TOPIC_MONITOR_QoS,true,"{\"alive\": false}");
  mqttClient.connect();
}

void connectToWifi()
{
  if (WiFi.status() == WL_CONNECTED)
    return;

  while (true)
  {
    unsigned int connectStartTime = millis();
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    // WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.begin(WIFI_SSID);
    Serial.printf("Attempting to connect to SSID: ");
    Serial.printf(WIFI_SSID);
    while (millis() - connectStartTime < 10000)
    {
      Serial.print(".");
      delay(1000);
      if (WiFi.status() == WL_CONNECTED)
      {
        Serial.println("connected to wifi");
        Serial.println(WiFi.localIP());
        wifiReconnectTimer.detach();
        connectToMqtt();
        return;
      }
    }
    Serial.println(" could not connect for 10 seconds. retry");
  }
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("[MQTT] Connected to MQTT!");

  mqttReconnectTimer.detach();

  Serial.print("Session present: ");
  Serial.println(sessionPresent);
//   uint16_t packetIdSub = mqttClient.subscribe(MQTT_TOPIC_LEDS, MQTT_TOPIC_LEDS_QoS);
//   Serial.print("Subscribing to ");
//   Serial.println(MQTT_TOPIC_LEDS);
  Serial.println("Sending alive message");
  mqttClient.publish(MQTT_TOPIC_MONITOR, MQTT_TOPIC_MONITOR_QoS, true, "{\"alive\": true}");
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("[MQTT] Disconnected from MQTT!");

  if (WiFi.isConnected()) {
    Serial.println("[MQTT] Trying to reconnect...");
    mqttReconnectTimer.once(MQTT_RECONNECT_TIME, connectToMqtt);
  }
}

// void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
//   if (!strcmp(topic, MQTT_TOPIC_LEDS)) {
//     Serial.print("Leds message: ");
//     Serial.print(len);
//     Serial.print(" [");
//     Serial.print(topic);
//     Serial.print("] ");
//     for (int i = 0; i < len; i++) {
//       Serial.print((char)payload[i]);
//     }
//     Serial.println();

//     DynamicJsonDocument doc(50);
//     deserializeJson(doc, payload);
    // color = doc["color"];
    // master_state = doc["master_state"];
    // Serial.print("color: ");
    // Serial.println(color);
    // Serial.print("master_state: ");
    // Serial.println(master_state);
//   }
// }

void setup() {
  Serial.begin(115200);
  Serial.println("Startup!");
  SPI.begin();        // Init SPI bus
  mfrc522.PCD_Init(); // Init MFRC522 card
  mfrc522.PCD_DumpVersionToSerial();	// Show details of PCD - MFRC522 Card Reader details
  // Serial.println(F("Scan PICC to see UID, SAK, type, and data blocks..."));
  // Prepare the key (used both as key A and as key B)
  // using FFFFFFFFFFFFh which is the default at chip delivery from the factory
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
//   mqttClient.onMessage(onMqttMessage);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);

  if (MQTT_USER != "") {
    mqttClient.setCredentials(MQTT_USER, MQTT_PASS);
  }

  connectToWifi();
}

/**
 * Main loop.
 */
void loop() {
  // read the RFID reader version to send over the heartbeat as data
  PICC_version = 0;
  PICC_version = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);

  // START RFID HANDLING
  // Look for new cards
  if ( ! mfrc522.PICC_IsNewCardPresent())
    return;

  // Select one of the cards
  if ( ! mfrc522.PICC_ReadCardSerial())
    return;

  // Dump debug info about the card; PICC_HaltA() is automatically called
  // mfrc522.PICC_DumpToSerial(&(mfrc522.uid));

  // get card uid
  Serial.print("found tag ID: ");
  dump_byte_array(mfrc522.uid.uidByte, mfrc522.uid.size);
  Serial.println();

  // get PICC card type
  // Serial.print(F("PICC type: "));
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  // Serial.println(mfrc522.PICC_GetTypeName(piccType));

  // Check for compatibility
  if (    piccType != MFRC522::PICC_TYPE_MIFARE_MINI
      &&  piccType != MFRC522::PICC_TYPE_MIFARE_1K
      &&  piccType != MFRC522::PICC_TYPE_MIFARE_4K) {
      Serial.println(F("Not a MIFARE Classic card."));
      return;
  }

  // perform authentication to open communication
  auth_success = authenticate(trailerBlock, key);
  if (!auth_success) {
    //Serial.println(F("Authentication failed"));
    return;
  }

  // read the tag to get coded information
  read_success = read_block(blockAddr, buffer, size);
  if (!read_success) {
    //Serial.println(F("Initial read failed, closing connection"));
    // Halt PICC
    mfrc522.PICC_HaltA();
    // Stop encryption on PCD
    mfrc522.PCD_StopCrypto1();
    return;
  }

  // consider not sending the chip data if its the same chip? prevent system load and errors?
  String UID;
  for (int i = 0; i < mfrc522.uid.size; i++) {  // for size of uid.size write uid.uidByte to UID
    if(mfrc522.uid.uidByte[i] < 0x10) {
        (UID +="0");
    }
    UID += String(mfrc522.uid.uidByte[i], HEX);
  }

  byte color = *(buffer + 0); // byte 0 for color encoding
  if(color == 0xff) {
    color = 0x4; // set uninitialized color to 0x4
  }
  byte level = *(buffer + 1); // byte 1 for level encoding
  if(level == 0xff) {
    level = 0;
  }
  byte song = *(buffer + 2); // byte 2 for song encoding
  if(song == 0xff) {
    song = 0;
  }
  // byte 15 for event track encoding bit[0] = burnerot2018, bit[1] = contra2019, bit[2] = midburn2022, bit[3] = burnerot2022
  // bit[4] = afrikaburn2023
  byte eventTrack = *(buffer + 15);
  Serial.print("Current chip color: "); Serial.println(color);
  Serial.print("Current chip level: "); Serial.println(level);
  Serial.print("Current chip eventTrack: "); Serial.println(eventTrack,HEX);


  // check if its an old chip and encode it with new format
  is_old_chip = (buffer[15] < 0x10);  // event tracking byte smaller than 0x10 means old chip
  if (is_old_chip) {
    eventTrack |= (1 << EVENT_ID_BIT); // add event id to eventTrack byte
    if (buffer[15] < 0x8) {
        dataBlock[0] = buffer[1];     // move color byte of old chips from byte 1 to byte 0
    } else {
        dataBlock[0] = buffer[0]; // after burnerot2022 event the color byte was moved to byte 0 no need to change
    }
    dataBlock[1] = 0x0;           // zero out level byte
    dataBlock[2] = 0x1;           // set song byte to something
    dataBlock[15] = eventTrack;   // last byte for event track, bit[0] = burnerot2018, bit[1] = contra2019, bit[2] = Midburn2022
    write_success = write_and_verify(blockAddr, dataBlock, buffer, size);
    if (write_success) {
      Serial.println(F("write worked, old chip converted to new format"));
    }
    else {
      Serial.println(F("write failed! aborting chip handling"));
      // Halt PICC
      mfrc522.PICC_HaltA();
      // Stop encryption on PCD
      mfrc522.PCD_StopCrypto1();
      return;
    }
  }

  // after changing old chips to new format testing for old chips is done on byte 15 bits[3:0]
  is_old_chip = (buffer[15] & 0x0F);
  
  if (mqttClient.connected() && send_chip_data) {
    StaticJsonDocument<128> chip_data;
    chip_data["uid"] = UID;
    chip_data["color"] = color;
    chip_data["level"] = level;
    chip_data["song"] = song;
    chip_data["old_chip"] = is_old_chip;
    char chip_data_buffer[100];
    serializeJson(chip_data, chip_data_buffer);
    mqttClient.publish(MQTT_TOPIC_CHIP, MQTT_TOPIC_CHIP_QoS, false, chip_data_buffer);
    Serial.print("Sending chip data: ");
    serializeJson(chip_data, Serial);
  }

  // Dump the sector data, good for debug
  // Serial.println(F("Current data in sector:"));
  // mfrc522.PICC_DumpMifareClassicSectorToSerial(&(mfrc522.uid), &key, sector);
  // Serial.println();

  // Halt PICC
  mfrc522.PICC_HaltA();
  // Stop encryption on PCD
  mfrc522.PCD_StopCrypto1();
}
