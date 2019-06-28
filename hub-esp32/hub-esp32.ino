#include <EEPROM.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include "AWS_IOT.h"
#include "WiFi.h"
#include "NTPClient.h"
#include "WiFiUdp.h"
#include "ArduinoJson.h"
#include "HubSerial.h"
#include "CheckStream.h"
#include "Sensaur.h"
#include "SensaurDevice.h"
#include "settings.h"


#define FIRMWARE_VERSION 1  // firmware version for this application: EEPROM will be erased if version incremented
#define MAX_DEVICE_COUNT 6
#define CONSOLE_BAUD 9600
#define DEV_BAUD 38400
#define ENABLE_BLE


#define BUTTON_PIN 4
#define STATUS_LED_PIN 5      
#define SERIAL_PIN_1 23
#define SERIAL_PIN_2 25
#define SERIAL_PIN_3 26
#define SERIAL_PIN_4 27
#define SERIAL_PIN_5 32
#define SERIAL_PIN_6 33
#define LED_PIN_1 16
#define LED_PIN_2 17
#define LED_PIN_3 18
#define LED_PIN_4 19
#define LED_PIN_5 21
#define LED_PIN_6 22


#define BLE_SERVICE_UUID "9ec18803-e34a-4882-b61d-864247da821d"
#define WIFI_NETWORK_UUID "e2ccd120-412f-4a99-922b-aca100637e2a"
#define WIFI_PASSWORD_UUID "30db3cd0-8eb1-41ff-b56a-a2a818873c34"
#define OWNER_ID_UUID "af74141f-3c60-425a-9402-62ec79b58c1a"
#define HUB_ID_UUID "e4636699-367b-4838-a421-1904cf95f869"
#define HUB_CERT_UUID "d1c4d088-fd9c-4881-8fc2-656441fa2cf4"
#define HUB_KEY_UUID "f97fee16-f4c3-48ff-a315-38dc2b985770"


// configuration storage (will be in EEPROM)
struct Config {
  int version;
  bool consoleEnabled;
  bool wifiEnabled;
  char wifiNetwork[64];
  char wifiPassword[64];
  int responseTimeout;
  char ownerId[64];
  char hubId[64];
  char thingCrt[1500];
  char thingPrivateKey[2000];
} config;

// dump configuration to console for debugging purposes
void dumpConfig(const Config* c) {
  if (config.consoleEnabled) {
    uint64_t chipid = ESP.getEfuseMac();
    uint16_t id_high2 = (uint16_t)(chipid>>32);
    uint32_t id_low4 = (uint32_t)chipid;
    Serial.printf("mac=%04X%08X, size=%d; %d,...,%s,%s,%s,%s\n%s\n%s\n", id_high2, id_low4,
    //        Serial.printf("size=%d; %d,...,%s,%s,%s,%s\n%s\n%s\n", 
      sizeof(Config), 
      c->version, 
      c->ownerId, 
      c->hubId,
      c->wifiNetwork,
      c->wifiPassword,
      c->thingCrt,
      c->thingPrivateKey
      );
  }  
}
//#define EEPROM_SIZE 3000
#define EEPROM_SIZE sizeof(Config)

// serial connections to each device
HubSerial devSerial[] = {
  HubSerial(SERIAL_PIN_1),
  HubSerial(SERIAL_PIN_2),
  HubSerial(SERIAL_PIN_3),
  HubSerial(SERIAL_PIN_4),
  HubSerial(SERIAL_PIN_5),
  HubSerial(SERIAL_PIN_6),
};


// serial connections wrapped with objects that add checksums to outgoing messages
CheckStream devStream[] = {
  CheckStream(devSerial[0]),
  CheckStream(devSerial[1]),
  CheckStream(devSerial[2]),
  CheckStream(devSerial[3]),
  CheckStream(devSerial[4]),
  CheckStream(devSerial[5]),
};


// buffer for message coming from USB serial port
#define CONSOLE_MESSAGE_BUF_LEN 40
char consoleMessage[CONSOLE_MESSAGE_BUF_LEN];
byte consoleMessageIndex = 0;


// buffer for message coming from sensor/actuator device 
#define DEVICE_MESSAGE_BUF_LEN 200
char deviceMessage[DEVICE_MESSAGE_BUF_LEN];
byte deviceMessageIndex = 0;


// other globals
bool configMode = false;
unsigned long sendInterval = 0;
unsigned long lastSendTime = 0;
unsigned long pollInterval = 1000;
unsigned long lastPollTime = 0;
Device devices[MAX_DEVICE_COUNT];
int ledPin[MAX_DEVICE_COUNT] = {LED_PIN_1, LED_PIN_2, LED_PIN_3, LED_PIN_4, LED_PIN_5, LED_PIN_6};
AWS_IOT awsConn;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
unsigned long lastEpochSeconds = 0;  // seconds since epoch start (from NTP)
unsigned long lastTimeUpdate = 0;  // msec since boot
char commandTopicName[100];  // apparently the topic name string needs to stay in memory
char actuatorsTopicName[100];


// run once on startup
void setup() {
  initConfig();

  // prepare serial connections
  Serial.begin(CONSOLE_BAUD);
  Serial.println();
  Serial.println("starting");
  for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
    devSerial[i].begin(DEV_BAUD);
  }

  // prepare LED pins and button
  for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
    pinMode(ledPin[i], OUTPUT);
    digitalWrite(ledPin[i], LOW);
  }
  ledcAttachPin(STATUS_LED_PIN, 0);  // attach status LED to PWM channel 0
  ledcSetup(0, 5000, 8);  // set up channel 0 to use 5000 Hz with 8 bit resolution
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // connect to wifi
  if (config.wifiEnabled) {
    int status = WL_IDLE_STATUS;
    while (status != WL_CONNECTED) {
      status = WiFi.begin(config.wifiNetwork, config.wifiPassword);
      if (status != WL_CONNECTED) {
        delay(2000);
      }
    }
    Serial.println("connected to wifi");
  } else {
    Serial.println("not connected to wifi");    
  }
  setStatusLED(HIGH);
  
  // connect to AWS MQTT
  // note: some AWS IoT code based on https://github.com/jandelgado/esp32-aws-iot
  if (config.wifiEnabled) {
    if (awsConn.connect(HOST_ADDRESS, CLIENT_ID, aws_root_ca_pem, certificate_pem_crt, private_pem_key) == 0) {
      Serial.println("connected to AWS");
      delay(200);  // wait a moment before subscribing

      // prep topic names
      String topicName = String(config.ownerId) + "/hub/" + config.hubId + "/command";
      strcpy(commandTopicName, topicName.c_str());
      topicName = String(config.ownerId) + "/hub/" + config.hubId + "/actuators";
      strcpy(actuatorsTopicName, topicName.c_str());

      // do subscriptions
      subscribe(commandTopicName);
      subscribe(actuatorsTopicName);
    } else {
      Serial.println("failed to connect to AWS");
      freezeWithError();
    }
  }

  // get network time
  if (config.wifiEnabled) {
    timeClient.begin();
    timeClient.setTimeOffset(0);  // we want UTC
    updateTime();
  }

  // send current status
  sendStatus();
  Serial.println("ready");
}


// run repeatedly
void loop() {

  // process any incoming data from the hub computer
  while (Serial.available()) {
    processByteFromComputer(Serial.read());
  }

  // yield to other tasks to allow AWS messages to be received
  taskYIELD();

  // do polling
  if (pollInterval) {
    unsigned long time = millis();
    if (time - lastPollTime > pollInterval) {
      doPolling();
      if (lastPollTime) {
        lastPollTime += pollInterval;  // increment poll time so we don't drift
      } else {
        lastPollTime = time;  // unless this is the first time polling
      }
    }

    // check for disconnects
    time = millis();
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
      Device &d = devices[i];
      if (d.connected() && time - d.lastMessageTime() > pollInterval * 2) {
        d.setConnected(false);
        digitalWrite(ledPin[i], LOW);
        sendDeviceInfo();
      }
    }
  }

  // send current sensor values to server
  unsigned long time = millis();
  if (sendInterval) {
    if (time - lastSendTime > sendInterval) {
      sendSensorValues(time);
      if (lastSendTime) {
        lastSendTime += sendInterval;  // increment send time so we don't drift
      } else {
        lastSendTime = time;  // unless this is the first time sending
      }
    }
  }

  // check for BLE config button (button will be LOW when pressed)
  if ((digitalRead(BUTTON_PIN) == LOW) && configMode == false) {
    configMode = true;
  }
  if (configMode) {
    ledcWrite(0, (millis() >> 3) & 255);  // fade LED when in config mode
  }

  // get network time once an hour
  if (time - lastTimeUpdate > 1000 * 60 * 60) {
    updateTime();
  }
}


// loop through all the devices, requesting a value from each one
void doPolling() {
  for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
    deviceMessageIndex = 0;
    if (devices[i].componentCount()) {
      devStream[i].println('v');  // request values (if any)
    } else {
      devStream[i].println('m');  // request metadata if not yet received
    }
    waitForResponse(i);
  }
}


void subscribe(const char *topicName) {
  if (awsConn.subscribe(topicName, messageHandler) == 0) {
    Serial.print("subscribed to topic: ");
    Serial.println(topicName);
  } else {
    Serial.print("failed to subscribe to topic: ");
    Serial.println(topicName);
    freezeWithError();
  }
}


// handle an incoming command MQTT message
void messageHandler(char *topicName, int payloadLen, char *payLoad) {
  DynamicJsonDocument doc(256);
  deserializeJson(doc, payLoad);
  String command = doc["command"];
  runCommand(command.c_str(), doc);
}


void waitForResponse(int deviceIndex) {

  // read a message into serial device's buffer
  unsigned long startTime = millis();
  do {
    devSerial[deviceIndex].busyReadByte(config.responseTimeout);
    if (devSerial[deviceIndex].peek() == 13) {
      break;
    }
  } while (millis() - startTime < config.responseTimeout);  // put this at end so we're less likely to miss first character coming back form device

  // copy into our internal buffer and process message
  deviceMessageIndex = 0;
  while (devStream[deviceIndex].available()) {
    char c = devStream[deviceIndex].read();
    if (c == 10 || c == 13) {
      if (deviceMessageIndex) {  // don't send empty messages
        deviceMessage[deviceMessageIndex] = 0;
        Device &d = devices[deviceIndex];
        if (d.connected() == false) {
          d.setConnected(true);
          d.resetComponents();
        }
        processMessageFromDevice(deviceIndex);
        break;
      }
    } else {
      deviceMessage[deviceMessageIndex] = c;
      if (deviceMessageIndex < DEVICE_MESSAGE_BUF_LEN - 1) {
        deviceMessageIndex++;
      }
    }
  }
}


void processMessageFromDevice(int deviceIndex) {

  // if enabled, echo the message to the USB serial console
  if (config.consoleEnabled) {
    Serial.print(deviceIndex);
    Serial.print('>');
    Serial.println(deviceMessage);
  }

  if (checksumOk(deviceMessage, true) == 0) {
    if (config.consoleEnabled) {
      Serial.println("e:device crc");
    }
    return;
  }

  // at this point we'll assume it's a valid message and update the last message time, which we use to detect disconnects
  Device &dev = devices[deviceIndex];
  dev.setLastMessageTime(millis());
  
  // process the message
  char *command;
  char *args[MAX_COMPONENT_COUNT + 2];  // the meta-data message has version, ID, and string per component
  if (deviceMessage[0] == 'v') {  // values
    int argCount = parseMessage(deviceMessage, &command, args, MAX_COMPONENT_COUNT + 1);
    int argIndex = 0;
    for (int i = 0; i < dev.componentCount(); i++) {
      Component &c = dev.component(i);
      if (c.dir() == 'i' && argIndex < argCount) {
        c.setValue(args[argIndex]);
        argIndex++;
      }
    }
    
  } else if (deviceMessage[0] == 'm') {  // metadata
    int argCount = parseMessage(deviceMessage, &command, args, MAX_COMPONENT_COUNT + 1, ';');  // note: using semicolon as separator here
    if (argCount > 2) {
      dev.setVersion(args[0]);
      dev.setId(args[1]);

      // populate component info
      int componentCount = argCount - 2;
      if (componentCount > MAX_COMPONENT_COUNT) {
        componentCount = MAX_COMPONENT_COUNT;
      }
      dev.setComponentCount(componentCount);
      for (int i = 0; i < componentCount; i++) {
        dev.component(i).setInfo(args[i + 2]);
      }

      // once we have metadata, we can indicate that the device has successfully connected
      digitalWrite(ledPin[deviceIndex], HIGH);

      // send device/component info to server
      sendDeviceInfo();
    }
  }
}


// process any incoming data from the hub computer
void processByteFromComputer(char c) {
  if (c == 10 || c == 13) {
    if (consoleMessageIndex) {  // if we have a message from the hub computer
      consoleMessage[consoleMessageIndex] = 0;
      DynamicJsonDocument doc(10);
      runCommand(consoleMessage, doc);
      consoleMessageIndex = 0;
    }
  } else {
    if (consoleMessageIndex < CONSOLE_MESSAGE_BUF_LEN - 1) {
      consoleMessage[consoleMessageIndex] = c;
      consoleMessageIndex++;
    }
  }
}


void runCommand(const char *command, DynamicJsonDocument &doc) {
  if (strcmp(command, "p") == 0) {  // poll all the devices for their current values
    Serial.println("polling");
    doPolling();
  } else if (strcmp(command, "w") == 0) {  // eepromwrite test
    EEPROM.put(0, config);
    EEPROM.commit();     
    Serial.println("config saved in flash memory:");      
    dumpConfig(&config);        
  } else if (strcmp(command, "r") == 0) {  // eepromread test
    Config myConfig;
    EEPROM.get(0, myConfig);
    Serial.println("config loaded from flash memory:");
    dumpConfig(&myConfig);        
  } else if (strcmp(command, "s") == 0) {  // start sending sensor values once a second
    pollInterval = 1000;
    sendInterval = 1000;
  } else if (strcmp(command, "start_ble") == 0) {  // request a status message
    #ifdef ENABLE_BLE
      startBLE();
    #endif
  } else if (strcmp(command, "req_status") == 0) {  // request a status message
    sendStatus();
  } else if (strcmp(command, "req_devices") == 0) {  // request a devices message
    sendDeviceInfo();
  } else if (strcmp(command, "set_send_interval") == 0) {
    sendInterval = round(1000.0 * doc["send_interval"].as<float>());
    if (sendInterval < 1000) {
      pollInterval = 500;
    } else {
      pollInterval = 1000;
    }
  } else if (strcmp(command, "update_firmware") == 0) {
    String url = doc["url"];
    Serial.print("updating firmware: ");
    Serial.println(url);
  } else if (command[0] && command[1] == '>') {  // send a message to a specific device
    int deviceIndex = command[0] - '0';
    if (config.consoleEnabled) {
      Serial.print("sending message to device ");
      Serial.print(deviceIndex);
      Serial.print(": ");
      Serial.println(command + 2);
    }
    devStream[deviceIndex].println(command + 2);
    waitForResponse(deviceIndex);
  }  
}


void sendStatus() {
  if (config.consoleEnabled) {
    Serial.print("sending status: ");
  }
  DynamicJsonDocument doc(256);
  doc["wifi_network"] = WIFI_SSID;
  // doc["wifi_password"] = WIFI_PASSWORD;  // leave out wifi password for now
  doc["host"] = HOST_ADDRESS;
  String topicName = String(config.ownerId) + "/hub/" + config.hubId + "/status";
  String message;
  serializeJson(doc, message);
  if (config.wifiEnabled) {
    if (awsConn.publish(topicName.c_str(), message.c_str())) {
      Serial.println("error publishing");
    }
  }
  if (config.consoleEnabled) {
    Serial.println("done");
  }
}


void sendDeviceInfo() {
  if (config.consoleEnabled) {
    Serial.print("sending device info: ");
  }
  String json = "{";
  bool first = true;
  int deviceCount = 0;
  for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
    Device &dev = devices[i];
    if (dev.connected()) {
      if (first == false) {
        json += ",";
      }
      json += '"';
      json += dev.id();
      json += "\":";
      json += String("{\"version\": ") + dev.version() + ", \"components\": [";
      for (int j = 0; j < dev.componentCount(); j++) {
        if (j)
          json += ',';
        json += dev.component(j).infoJson();
      }
      json += "]}";
      first = false;
      String topicName = String(config.ownerId) + "/device/" + dev.id();
      if (config.wifiEnabled) {
        if (awsConn.publish(topicName.c_str(), config.hubId)) {  // send hub ID for this device
          Serial.println("error publishing");
        }
      }
      deviceCount++;
    }
  }
  json += "}";
  String topicName = String(config.ownerId) + "/hub/" + config.hubId + "/devices";
  if (config.wifiEnabled) {
    Serial.println(json);
    if (awsConn.publish(topicName.c_str(), json.c_str())) {  // send list of device info dictionaries
      Serial.println("error publishing");    
    }
  }
  if (config.consoleEnabled) {
    Serial.print(deviceCount);
    Serial.println(" devices");
  }
}


void sendSensorValues(unsigned long time) {
  if (config.consoleEnabled) {
    Serial.print("sending values: ");
  }
  int valueCount = 0;
  DynamicJsonDocument doc(512);
  String wallTime = String(((double) (time - lastTimeUpdate) / 1000.0) + (double) lastEpochSeconds);  // convert to string since json code doesn't seem to handle doubles correctly
  doc["time"] = wallTime;
  for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
    Device &d = devices[i];
    if (d.connected()) {
      for (int j = 0; j < d.componentCount(); j++) {
        Component &c = d.component(j);
        if (c.dir() == 'i') {
          String compId = String(d.id()) + '-' + c.idSuffix();
          doc[compId] = c.value();
          valueCount++;
        }
      }
    }
  }
  String topicName = String(config.ownerId) + "/hub/" + config.hubId + "/sensors";
  String message;
  serializeJson(doc, message);
  if (config.wifiEnabled) {
    if (awsConn.publish(topicName.c_str(), message.c_str())) {
      Serial.println("error publishing");
    }
  }
  if (config.consoleEnabled) {
    Serial.print(valueCount);
    Serial.print(" values at ");
    Serial.println(wallTime);
  }
}


void testLEDs() {
  digitalWrite(STATUS_LED_PIN, HIGH);
  delay(500);
  digitalWrite(STATUS_LED_PIN, LOW);
  for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
    digitalWrite(ledPin[i], HIGH);
    delay(200);
    digitalWrite(ledPin[i], LOW);
  }
}


void initConfig() {
  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("Failed to initialise EEPROM");
    Serial.println("Restarting...");
    delay(1000);
    ESP.restart();
  }  
  config.version = FIRMWARE_VERSION;

  
  config.consoleEnabled = ENABLE_CONSOLE;
  config.wifiEnabled = ENABLE_WIFI;
  config.responseTimeout = RESPONSE_TIMEOUT;
  strncpy(config.ownerId, OWNER_ID, 64);
  strncpy(config.hubId, HUB_ID, 64);
  strncpy(config.wifiNetwork, WIFI_SSID, sizeof(config.wifiNetwork)-1);
  strncpy(config.wifiPassword, WIFI_PASSWORD, sizeof(config.wifiPassword)-1);
  strncpy(config.thingCrt, certificate_pem_crt, sizeof(config.thingCrt)-1);
  strncpy(config.thingPrivateKey, private_pem_key, sizeof(config.thingPrivateKey)-1);
}


void setStatusLED(int state) {
  if (state) {
    ledcWrite(0, 255);
  } else {
    ledcWrite(0, 0);
  }
}


void updateTime() {

  // get new time from network
  int counter = 0;
  while (!timeClient.update()) {
    timeClient.forceUpdate();
    delay(1000);
    counter++;
    if (counter > 10) {
      break;  // failed; try again later
    }
  }

  lastTimeUpdate = millis();
  lastEpochSeconds = timeClient.getEpochTime();
  if (config.consoleEnabled) {
    Serial.print("updated time: ");
    Serial.println(lastEpochSeconds);
  }
}


void freezeWithError() {
  while (true) {
    setStatusLED(HIGH);
    delay(1000);
    setStatusLED(LOW);
    delay(1000);
  }
}


#ifdef ENABLE_BLE


class WifiNetworkCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    std::string value = characteristic->getValue();
    strncpy(config.wifiNetwork, value.c_str(), sizeof(config.wifiNetwork));
    Serial.println(config.wifiNetwork);
  }
};


class WifiPasswordCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    std::string value = characteristic->getValue();
    strncpy(config.wifiPassword, value.c_str(), sizeof(config.wifiPassword));
    Serial.println(config.wifiPassword);
  }
};


class OwnerIdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    std::string value = characteristic->getValue();
    strncpy(config.ownerId, value.c_str(), sizeof(config.ownerId));
    Serial.println(config.ownerId);
  }
};


class HubIdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    std::string value = characteristic->getValue();
    strncpy(config.hubId, value.c_str(), sizeof(config.hubId));
    Serial.println(config.hubId);
  }
};


class HubCertCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    std::string value = characteristic->getValue();
    if (value == "clear") {
      config.thingCrt[0] = 0;
    } else {
      if (strlen(config.thingCrt) + strlen(value.c_str()) < sizeof(config.thingCrt) - 1) {
        strcat(config.thingCrt, value.c_str());
      }
    }
    Serial.println(strlen(config.thingCrt));
  }
};


class HubKeyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    std::string value = characteristic->getValue();
    if (value == "clear") {
      config.thingPrivateKey[0] = 0;
    } else {
      if (strlen(config.thingPrivateKey) + strlen(value.c_str()) < sizeof(config.thingPrivateKey) - 1) {
        strcat(config.thingPrivateKey, value.c_str());
      }
    }
    Serial.println(strlen(config.thingPrivateKey));
  }
};


void startBLE() {
  BLEDevice::init("Sensaurus");
  BLEServer *server = BLEDevice::createServer();
  BLEService *service = server->createService(BLE_SERVICE_UUID);

  // add characteristics
  BLECharacteristic *characteristic = service->createCharacteristic(WIFI_NETWORK_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  characteristic->setCallbacks(new WifiNetworkCallbacks());
  characteristic->setValue(config.wifiNetwork);
  characteristic = service->createCharacteristic(WIFI_PASSWORD_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  characteristic->setCallbacks(new WifiPasswordCallbacks());
  characteristic->setValue(config.wifiPassword);
  characteristic = service->createCharacteristic(OWNER_ID_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  characteristic->setCallbacks(new OwnerIdCallbacks());
  characteristic->setValue(config.ownerId);
  characteristic = service->createCharacteristic(HUB_ID_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  characteristic->setCallbacks(new HubIdCallbacks());
  characteristic->setValue(config.hubId);
  characteristic = service->createCharacteristic(HUB_CERT_UUID, BLECharacteristic::PROPERTY_WRITE);
  characteristic->setCallbacks(new HubCertCallbacks());
  characteristic = service->createCharacteristic(HUB_KEY_UUID, BLECharacteristic::PROPERTY_WRITE);
  characteristic->setCallbacks(new HubKeyCallbacks());

  service->start();
  BLEAdvertising *advertising = server->getAdvertising();
  advertising->start();
}


#endif  // ENABLE_BLE

