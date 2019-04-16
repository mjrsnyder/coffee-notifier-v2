#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <Arduino.h>

#include "SSD1306Wire.h"

#include "EmonLib.h"

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

//needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>
//for LED status
#include <Ticker.h>
Ticker ticker;

int DISPLAY_HEIGHT = 64;
int DISPLAY_WIDTH = 128;

//flag for saving data
bool shouldSaveConfig = false;

SSD1306Wire display(0x3c, D6, D5);
WiFiClientSecure secClient;
EnergyMonitor emon1;

int RESET_PIN = D2;

char slack_url[128];
char slack_username[64] = "Barista Joe";
char burner_min_amps[5] = "0.3";
char burner_max_amps[5] = "0.4";
char heater_min_amps[5] = "3.0";
char current_threshold[5] = "0.1";
char current_delta_timeout[5] = "500"; //ms

double previousCurrent = 0.0;
double latestCurrent = 0.0;
double baseCurrent = 0.0;
double delta = 0.0;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void tick()
{
  //toggle state
  int state = digitalRead(BUILTIN_LED);  // get the current state of GPIO1 pin
  digitalWrite(BUILTIN_LED, !state);     // set pin to the opposite state
}

//gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
  //entered config mode, make led toggle faster
  ticker.attach(0.2, tick);
}

void displayPower(double Irms){
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.drawString(DISPLAY_WIDTH/2, 16, "Amps");
  display.drawString(DISPLAY_WIDTH/2, 32, String(Irms));
  display.display();
}

bool sendNotification(String msg){
  const char* host = "hooks.slack.com";
  const int httpsPort = 443;

  if (!secClient.connect(host, httpsPort)) {
    return false;
  }
  String postData="payload={\"link_names\": 1, \"icon_emoji\": \":coffee:\", \"username\": \"" + String(slack_username) + "\", \"text\": \"" + msg + "\"}";

  secClient.print(String("POST ") + String(slack_url) + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "Content-Type: application/x-www-form-urlencoded\r\n" +
               "Connection: close" + "\r\n" +
               "Content-Length:" + postData.length() + "\r\n" +
               "\r\n" + postData);
  String line = secClient.readStringUntil('\n');
  if (line.startsWith("HTTP/1.1 200 OK")) {
    return true;
  } else {
    return false;
  }
}

void changeDetected(double previousCurrent, double latestCurrent, double &baseCurrent){
  // first check to see if this is the first check after a reset
  if (previousCurrent == 0.0) {
    //baseCurrent = latestCurrent;
    // display base current detected
  } else {
    double delta = previousCurrent - latestCurrent;
    bool increase;
    if (delta < 0.0) {
      increase = true;
    } else {
      increase = false;
    }

    // Try and guess what change happened
    // TODO: refactor to something more configurable
    // For now do these checks in decending order
    double absDelta = abs(delta);
    displayPower(previousCurrent);
    String msg = "";
    if (absDelta > atof(heater_min_amps)) {
      // notifiiii main heater (coffee is probably brewing)
      // unless someone is making tea TODO: <===
      if (increase) {
        msg = "Coffee is brewing";
      } else {
        msg ="Coffee is almost done brewing";
      }
      sendNotification(msg);
    } else if (absDelta > atof(burner_min_amps) && absDelta < atof(burner_max_amps)) {
      // notifiii burner
      if (increase) {
        msg = "Burner turned on (hopefuly coffee is coming)";
      } else {
        msg = "Burner turned off (coffee is probably old or gone)";
      }
      sendNotification(msg);
    } else {
      // notifiii ¯\_(ツ)_/¯
      msg = "Something changed but I don't know what\n*Previous Current:* " + String(previousCurrent) + "\n*Latest Current:* " + String(latestCurrent);
      sendNotification(msg);
    }
  }
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);

  //set led pin as output
  pinMode(BUILTIN_LED, OUTPUT);
  // start ticker with 0.5 because we start in AP mode and try to connect
  ticker.attach(0.6, tick);
  //clean FS, for testing

  pinMode(RESET_PIN, INPUT_PULLUP);

  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(slack_url, json["slack_url"]);
          strcpy(slack_username, json["slack_username"]);
          strcpy(burner_min_amps, json["burner_min_amps"]);
          strcpy(burner_max_amps, json["burner_max_amps"]);
          strcpy(heater_min_amps, json["heater_min_amps"]);
          strcpy(current_threshold, json["current_threshold"]);
          strcpy(current_delta_timeout, json["current_delta_timeout"]);

        } else {
          Serial.println("failed to load json config");
        }
        configFile.close();
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }

  WiFiManagerParameter custom_slack_url("slack_url", "Slack Webhook Url", slack_url, 128);
  WiFiManagerParameter custom_slack_username("slack_username", "Slack Username", slack_username, 64);
  WiFiManagerParameter custom_burner_min_amps("burner_min_amps", "Lower Burner Amperage Threshold", burner_min_amps, 5);
  WiFiManagerParameter custom_burner_max_amps("burner_max_amps", "Upper Burner Amperage Threshold", burner_max_amps, 5);
  WiFiManagerParameter custom_heater_min_amps("heater_min_amps", "Lower Heater Amperage Threshold", heater_min_amps, 5);
  WiFiManagerParameter custom_current_threshold("current_threshold", "Current Delta Detection Threshold", current_threshold, 5);
  WiFiManagerParameter custom_current_delta_timeout("current_delta_timeout", "Time between delta verification", current_delta_timeout, 5);

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  //reset settings - for testing
  //wifiManager.resetSettings();

  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);

  //add all your parameters here
  wifiManager.addParameter(&custom_slack_url);
  wifiManager.addParameter(&custom_slack_username);
  wifiManager.addParameter(&custom_burner_min_amps);
  wifiManager.addParameter(&custom_burner_max_amps);
  wifiManager.addParameter(&custom_heater_min_amps);
  wifiManager.addParameter(&custom_current_threshold);
  wifiManager.addParameter(&custom_current_delta_timeout);

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect()) {
    Serial.println("failed to connect and hit timeout");
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");

  //read updated parameters
  strcpy(slack_url, custom_slack_url.getValue());
  strcpy(slack_username, custom_slack_username.getValue());
  strcpy(burner_min_amps, custom_burner_min_amps.getValue());
  strcpy(burner_max_amps, custom_burner_max_amps.getValue());
  strcpy(heater_min_amps, custom_heater_min_amps.getValue());
  strcpy(current_threshold, custom_current_threshold.getValue());
  strcpy(current_delta_timeout, custom_current_delta_timeout.getValue());
  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["slack_url"] = slack_url;
    json["slack_username"] = slack_username;
    json["burner_min_amps"] = burner_min_amps;
    json["burner_max_amps"] = burner_max_amps;
    json["heater_min_amps"] = heater_min_amps;
    json["current_threshold"] = current_threshold;
    json["current_delta_timeout"] = current_delta_timeout;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }

  ticker.detach();
  //keep LED on
  digitalWrite(BUILTIN_LED, LOW);

  display.init();
  display.flipScreenVertically();
  display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
  display.setFont(ArialMT_Plain_16);

  emon1.current(A0,111.1);
}

void loop() {
  // get current current (he he, I probably will make this pun a lot)
  latestCurrent = emon1.calcIrms(1480);

  delta = previousCurrent - latestCurrent;

  // maybe make this recursive?
  if (abs(delta) > atof(current_threshold)) {
    // check a few times to make sure there is a sustained load
    delay(int(atof(current_delta_timeout)));
    latestCurrent = emon1.calcIrms(1480);
    delta = previousCurrent - latestCurrent;
    if ( abs(delta) > atof(current_threshold)) {
      changeDetected(previousCurrent, latestCurrent, baseCurrent);
    }
  }
  previousCurrent = latestCurrent;
  displayPower(latestCurrent);
  if(digitalRead(RESET_PIN) == LOW){
    Serial.println("Resetting...");
    SPIFFS.format();
    delay(100);
    Serial.println("FS Formatted");
    WiFi.disconnect();
    delay(100);
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    delay(100);
    Serial.println("WiFi Disconnected");
    ESP.reset();
    delay(1000);
  }
}
