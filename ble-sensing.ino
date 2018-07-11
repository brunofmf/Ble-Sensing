/*************************************************
    This sketch was written and developed by Bruno Fernandes in July 2018.
    
    Portions of this code are partially based in others such as
    CrowdSensing (by Bruno Fernandes),
    BLE_client and WatchdogTimer (example sketches).
    
    MIT Licensed
*************************************************/
#include <BLEDevice.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

/** Probe Data **/
#define ARRAY_SIZE        50
#define SENSE_TYPE        "BLE"

/** Available Commands **/
#define CMD_START         "Start"
#define CMD_STOP          "Stop"
#define CMD_SCAN          "Scan"
#define CMD_COUNT         "Count"
#define CMD_PRINT         "Print"
#define CMD_CLEAR         "Clear"
#define CMD_SEND          "Send"
#define CMD_START_TIMER   "Start Timer"
#define CMD_STOP_TIMER    "Stop Timer"

/** MQTT Setup **/
#define MQTTSERVER        "dummy.mqttbroker.com"
#define MQTTPORT          11111
#define MQTTUSER          "dummy_user"
#define MQTTPASSWORD      "dummy_pass"
#define TOPIC             "ESP32/BLESENSING"

/** WiFi Connection Data **/
#define STATION_NETWORK   "dummy_wifi_net"   //Set station network
#define STATION_PASSWORD  "dummy_wifi_pass"  //Set station password

/** Data Struct **/
struct probeData {
  String mac;
  int rssi;
  long previousMillisDetected;
} probeArray[ARRAY_SIZE];

/** Control Variables **/
int currIndex                       = 0;
int dumpVersion                     = 1;
bool handlersStopped                = false;
String command;
bool isConnected                    = false;
bool mqttConnected                  = false;
bool timerIsActive                  = false;
bool scanNow                        = false;

/** Time Variables **/
unsigned long sightingsInterval     = 60000;          //1 minute
unsigned long connectionWait        = 35000;          //35 seconds
unsigned long scanTime              = 45;             //45 seconds
unsigned long sendTimer             = 30 + scanTime;  //30 seconds + (45s of the scan)

/** Os Timer **/
hw_timer_t *theTimer                = NULL;

/** BLE Variables **/
BLEScan* pBLEScan;
static BLEAddress *pServerAddress;

/** MQTT Variables **/
WiFiClient espClient;
PubSubClient client(espClient);

/** Sketch Functions **/
void setup() {
  Serial.begin(115200);  
  Serial.println(F("*** ESP32 BLE Capture by Bruno Fernandes ***")); 

  //Set up a WiFi Station
  WiFi.mode(WIFI_STA);
  
  //Connect to the network
  long startTime = millis();
  WiFi.begin(STATION_NETWORK, STATION_PASSWORD);
  
  //Wait, at most, connectionWait seconds
  while ( (WiFi.status() != WL_CONNECTED) && (millis() - startTime < connectionWait) ) {
    Serial.print(".");
    delay(500);
  }
  if(WiFi.status() == WL_CONNECTED) {
    Serial.print(" Connected to "); Serial.print(STATION_NETWORK);
    Serial.print("; IP address: "); Serial.println(WiFi.localIP());
    isConnected = true;
  } else{
    Serial.print(" Connection to "); Serial.print(STATION_NETWORK);
    Serial.println(" failed! No WiFi connection......");
  }

  //Setup the MQTT Connection
  setupMqtt();
  //Setup the BLE scan
  setupBLEScan();
  
  Serial.print("*** All setup has been made! ");
  if(isConnected){
    Serial.print("Timer is enabled and scan will happen every ");
    Serial.print(sendTimer); Serial.print(" seconds with a scan duration of ");
    Serial.print(scanTime); Serial.println(" seconds ***");
    startTimer();
  } else {
    Serial.println("Timer is DISABLED. Type Scan to look around! ***");
  }
}

void loop() {
  if(Serial.available() > 0) {
    //read incoming data as string
    command = Serial.readString();
    Serial.println(command);
    if(command.equalsIgnoreCase(CMD_START)){
      startHandlers();
    } else if(command.equalsIgnoreCase(CMD_STOP)){
      stopHandlers();
    } else if(command.equalsIgnoreCase(CMD_SCAN)){
      startScan(true);
    } else if(command.equalsIgnoreCase(CMD_COUNT)){
      Serial.println(currIndex);  
      delay(1000);      
    } else if(command.equalsIgnoreCase(CMD_PRINT)){
      printProbeArray();
      delay(1000);  
    } else if(command.equalsIgnoreCase(CMD_CLEAR)){
      clearData();
      delay(1000);  
    } else if(command.equalsIgnoreCase(CMD_SEND)){
      sendDataCmd();
      delay(1000); 
    } else if(command.equalsIgnoreCase(CMD_START_TIMER)){
      startTimer(); 
    } else if(command.equalsIgnoreCase(CMD_STOP_TIMER)){
      stopTimer();
    } else{
      Serial.println(F("Unknown command!"));
    }
  } 
  //If timer told us it is time to scan
  if(scanNow){
    scanNow = false;
    startScan(false);  
    buildAndPublish(true);
    client.loop();
  }
}

class advertisedDeviceCallback: public BLEAdvertisedDeviceCallbacks {
  //Called for each advertising BLE device
  void onResult(BLEAdvertisedDevice advertisedDevice) {    
    pServerAddress = new BLEAddress(advertisedDevice.getAddress());
    String mac = pServerAddress->toString().c_str();
    long theMillis = millis();
    
    onCapturedDataPrint(advertisedDevice, mac, theMillis);
    
    if(currIndex < ARRAY_SIZE){
      if(newSighting(mac)){
        probeArray[currIndex].mac = mac;
        probeArray[currIndex].rssi = advertisedDevice.getRSSI();
        probeArray[currIndex++].previousMillisDetected = theMillis;
      } else{
        Serial.println(F("This BLE Device has already been SIGHTED recently!!!!!!!")); 
      }
    } else{
      Serial.println(F("*** Array Limit Achieved!! Send and clear it to process more probe requests! ***"));    
    }
  }
};

void onCapturedDataPrint(BLEAdvertisedDevice advertisedDevice, String mac, long theMillis) {
  Serial.print("-- BLE Advertised Device found - Address: ");  
  Serial.print(mac);
  Serial.print("; RSSI: ");
  Serial.print(advertisedDevice.getRSSI());
  Serial.print("; Millis Last Detected: ");
  Serial.print(theMillis); Serial.println(";");
  Serial.print("TX Power: ");
  Serial.print(advertisedDevice.getTXPower());
  Serial.print(". BLE data captured from: ");
  Serial.println(advertisedDevice.toString().c_str());
}

void setupBLEScan(){
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan(); //create new scan
  pBLEScan->setAdvertisedDeviceCallbacks(new advertisedDeviceCallback());
  pBLEScan->setActiveScan(true); //active scan uses more power, but get results faster
  Serial.println("*** BLE Setup has been configured ***");
}

void setupMqtt(){
  int connectionAttempts = 6;
  if(isConnected){
    client.setServer(MQTTSERVER, MQTTPORT);
    while (!client.connected() && connectionAttempts > 0) {
      Serial.print("*** Connecting to MQTT... "); 
      if (client.connect("ESP32Client", MQTTUSER, MQTTPASSWORD)) { 
        Serial.println("Connected!! ***"); 
        mqttConnected = true;
      } else { 
        Serial.print("Failed with state: ");
        Serial.println(client.state());
        delay(2000); 
      }
      connectionAttempts--;
    }
    if(!client.connected()){
      Serial.println(F("*** It is not possible to connect to the MQTT Broker! ***"));
    }
  } else{
    Serial.println(F("*** It is not possible to connect to the MQTT Broker! There is no WiFi connection! ***"));
  } 
}

boolean publishMqtt(char topic[], char payload[]){
  if(mqttConnected){  
    //Required...
    setupMqtt();
    client.publish(topic, payload);
    Serial.print("*** Payload Successfully Published in topic: ");
    Serial.print(TOPIC); Serial.println(" ***"); 
    return true;
  } else{
    Serial.println(F("*** It is not possible to publish data! There is no MQTT connection! ***"));
    return false;
  } 
}

void startHandlers(){
  if(handlersStopped){
    handlersStopped = false;
    Serial.print(F("*** Services Started! "));  
    if(isConnected){
      Serial.print("Timer is enabled and scan will happen every ");
      Serial.print(sendTimer); Serial.print(" seconds with a scan duration of ");
      Serial.print(scanTime); Serial.println(" seconds ***");
      startTimer();
    } else {
      Serial.println("Timer is DISABLED. Type Scan to look around! ***");
    }
  } else{
    Serial.println(F("*** Services are already running! ***"));
  }
}

void stopHandlers(){
  if(!handlersStopped){
    handlersStopped = true;
    stopTimer();
    Serial.println(F("*** Services Stopped! ***"));
  } else{
    Serial.println(F("*** Services Already Stopped! ***"));
  }
}

void startTimer(){
  if(isConnected && !timerIsActive && !handlersStopped){
    //Time Initialization: 0 - timer selection (0-3); 80 - prescaler (ESP32 clock 80MHz); true for progressivo counter (false for regressive)
    theTimer = timerBegin(0, 80, true); 
    //Interrupt Attach: theTimer - instance of hw_timer; timerCallback - function address; edge - true raises interruption
    timerAttachInterrupt(theTimer, &timerCallback, true); 
    //Instantiated timer: theTimer - instance of hw_timer; 1000000 - value from us to one second; auto-reload - true to repeat alarm
    timerAlarmWrite(theTimer, 1000000*sendTimer, true); 
    //Enable the timer alarm
    timerAlarmEnable(theTimer);
    timerIsActive = true;    
    Serial.println(F("*** Timer started! ***"));
  } else{
    if (!isConnected){
      Serial.println(F("*** Not possible to set timer! There is no WiFi connection! ***"));  
    } else if (timerIsActive){
      Serial.println(F("*** Timer is already running! ***"));
    } else {
      Serial.println(F("*** Not possible to set timer! Handlers are stopped! ***"));
    }
  } 
}

void stopTimer(){
  if(timerIsActive){
    timerEnd(theTimer);
    theTimer = NULL;
    timerIsActive = false;
    Serial.println(F("*** Timer stopped! ***"));
  } else{
    Serial.println(F("*** Timer is already stopped! ***"));
  } 
}

void startScan(bool oneShotScan){
  if(!handlersStopped){
    if(oneShotScan && timerIsActive){
      Serial.println(F("*** Timer Scanning is Active - Wait for the timer! ***"));
    } else {
      Serial.println("*** Starting Bluetooth Scan ***");
      BLEScanResults scanResults = pBLEScan->start(scanTime);    
      Serial.print("Devices found: ");
      Serial.println(scanResults.getCount());
      Serial.println("*** Scan done! ***");
    }
  } else{
    Serial.println(F("*** Not possible to scan - Handlers are stopped! ***"));
  }
}

void timerCallback(){
  if(timerIsActive)
    scanNow = true;
}

void sendDataCmd(){
  if(isConnected){ //Send Data to MQTT server only if station connected
    buildAndPublish(false);
  } else{
    Serial.println(F("*** It is not possible to send data! There is no WiFi connection! ***"));
  }
}

void buildAndPublish(bool clearD){
  DynamicJsonBuffer jsonBuffer; //The default initial size for DynamicJsonBuffer is 256. It allocates a new block twice bigger than the previous one.
  JsonObject& root = jsonBuffer.createObject(); //Create the Json object
  root["type"] = SENSE_TYPE;
  JsonObject& tempTime = root.createNestedObject("timestamp");
  tempTime[".sv"] = millis();
  JsonArray& probes = root.createNestedArray("probes" + String(dumpVersion++)); //Create child probes array
  //Fill JsonArray with data
  for(int i = 0; i < currIndex; i++){
    JsonObject& probe = probes.createNestedObject();
    probe["mac"] = probeArray[i].mac;
    probe["rssi"] = probeArray[i].rssi;
    probe["previousMillisDetected"] = probeArray[i].previousMillisDetected;
  }
  //Push JSON
  char payload[root.measureLength()+1];
  root.printTo((char*)payload, root.measureLength()+1);
  //Handle error or success
  if(publishMqtt(TOPIC, payload)){
    if(clearD){
      Serial.println(F("*** Probe Data successfully published! Data will be cleared... ***"));
      clearData();
    } else{
      Serial.println(F("*** Probe Data successfully published! NO data was cleared... ***"));
    }
  } else{
    Serial.println(F("*** Failed to publish Probe Data in MQTT!! ***"));
  }
}

void clearData(){
  for(int i = 0; i < currIndex; i++){
    probeArray[i].mac = "";
    probeArray[i].rssi = 0;
    probeArray[i].previousMillisDetected = 0;
  }
  currIndex = 0;
  Serial.println(F("*** Data successfully cleared! ***"));
}

bool newSighting(String mac){
  long currTime = millis();
  //start by the end as array is ordered - new sightings are at the end - break as soon as it finds mac at the list
  for(int i = currIndex-1; i>=0; i--){
    //if mac has already been captured
    if(mac.equals(probeArray[i].mac)){
      //lets check if enough time has passed (sightingsInterval milliseconds since last sighting)
      if(currTime-probeArray[i].previousMillisDetected < sightingsInterval){
        return false;
      }      
      break;
    }
  }
  return true;
}

void printProbeArray(){
  Serial.println("*** Print detected devices: ***");
  for(int i = 0; i < currIndex; i++){
    Serial.print("MAC: ");
    Serial.print(probeArray[i].mac);
    Serial.print("; RSSI: ");
    Serial.print(probeArray[i].rssi);
    Serial.print("; Millis Last Detected: ");
    Serial.println(probeArray[i].previousMillisDetected);
    Serial.print("--- index: ");Serial.print(i);Serial.println(" ---");
  }
  Serial.println("*** All has been printed ***");
}

String macToString(const unsigned char* mac){
  char buf[20];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}
