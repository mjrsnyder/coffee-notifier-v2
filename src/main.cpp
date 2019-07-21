#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <Arduino.h>

#include "CSE7766.h"              //https://github.com/ingeniuske/CSE7766

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

//needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>
//for LED status
#include <Ticker.h>
Ticker ticker;

#include "RemoteDebug.h"        //https://github.com/JoaoLopesF/RemoteDebug
RemoteDebug Debug;
#define HOST_NAME "remotedebug-CSE7766"

#define RELAY_PIN       12
#define LED             13
#define SWITCH          0

// Log values every 2 seconds
#define UPDATE_TIME     2000

//flag for saving data
bool shouldSaveConfig = false;


WiFiClientSecure secClient;
WiFiManager wifiManager;

CSE7766 myCSE7766;

char slack_url[128];
char slack_username[64] = "Barista Sam";
char burner_min_amps[5] = "0.6";
char burner_max_amps[5] = "0.8";
char heater_min_amps[5] = "3.0";
char current_threshold[5] = "0.4";
char current_delta_timeout[5] = "500"; //ms

double previousCurrent = 0.0;
double latestCurrent = 0.0;
double delta = 0.0;


//callback notifying us of the need to save config
void saveConfigCallback () {
  shouldSaveConfig = true;
}

void tick()
{
  //toggle state
  int state = digitalRead(LED);  // get the current state of GPIO1 pin
  digitalWrite(LED, !state);     // set pin to the opposite state
}

//gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
  //entered config mode, make led toggle faster
  ticker.attach(0.2, tick);
}

bool sendNotification(String msg){
  ticker.attach(0.1, tick);
  const char* host = "hooks.slack.com";
  const int httpsPort = 443;

  if (!secClient.connect(host, httpsPort)) {
    ticker.detach();
    debugA("Failed to connect to Slack %s", slack_url);
    return false;
  }
  String postData="payload={\"link_names\": 1, \"icon_emoji\": \":coffee:\", \"username\": \"" + String(slack_username) + "\", \"text\": \"" + msg + "\"}";
  debugA("Posting to Slack");
  secClient.print(String("POST ") + String(slack_url) + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "Content-Type: application/x-www-form-urlencoded\r\n" +
               "Connection: close" + "\r\n" +
               "Content-Length:" + postData.length() + "\r\n" +
               "\r\n" + postData);
  debugA("Posted to Slack");
  String line = secClient.readStringUntil('\n');
  ticker.detach();
  if (line.startsWith("HTTP/1.1 200 OK")) {
    debugA("Successfully sent notification to Slack");
    return true;

  } else {
    debugA("Failed to send notification to Slack");
    return false;
  }
}

void resetSettings(){
  wifiManager.resetSettings();
  delay(1000);
  ESP.reset();
  delay(1000);
}

double changeDetected(double previousCurrent, double latestCurrent){
  double validationCurrent = latestCurrent;
  // first check to see if this is the first check after a reset
  if (previousCurrent != 0.0) {
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
    double absDelta = fabs(delta);
    String msg = "";
    if (absDelta > atof(heater_min_amps)) {
      // notifiiii main heater (coffee is probably brewing)
      // unless someone is making tea TODO: <===
      delay(30000); //haxx

      myCSE7766.handle();
      validationCurrent = myCSE7766.getCurrent();

      double validateDelta = previousCurrent - validationCurrent;

      if (fabs(validateDelta) > atof(heater_min_amps)) {
        if (increase) {
          msg = "Coffee is brewing";
        } else {
          msg = "Coffee is done brewing";
        }
        sendNotification(msg);
      }

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
    }
  }
  return validationCurrent;
}

void setup() {
  // Close the relay to switch on the load
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(SWITCH, INPUT_PULLUP);
  digitalWrite(RELAY_PIN, HIGH);

  //set led pin as output
  pinMode(LED, OUTPUT);
  // start ticker with 0.5 because we start in AP mode and try to connect
  ticker.attach(0.6, tick);
  //clean FS, for testing

  //read configuration from FS json
  if (SPIFFS.begin()) {
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          strcpy(slack_url, json["slack_url"]);
          strcpy(slack_username, json["slack_username"]);
          strcpy(burner_min_amps, json["burner_min_amps"]);
          strcpy(burner_max_amps, json["burner_max_amps"]);
          strcpy(heater_min_amps, json["heater_min_amps"]);
          strcpy(current_threshold, json["current_threshold"]);
          strcpy(current_delta_timeout, json["current_delta_timeout"]);

        } else {
        }
        configFile.close();
      }
    }
  }

  WiFiManagerParameter custom_slack_url("slack_url", "Slack Webhook Url", slack_url, 128);
  WiFiManagerParameter custom_slack_username("slack_username", "Slack Username", slack_username, 64);
  WiFiManagerParameter custom_burner_min_amps("burner_min_amps", "Lower Burner Amperage Threshold", burner_min_amps, 5);
  WiFiManagerParameter custom_burner_max_amps("burner_max_amps", "Upper Burner Amperage Threshold", burner_max_amps, 5);
  WiFiManagerParameter custom_heater_min_amps("heater_min_amps", "Lower Heater Amperage Threshold", heater_min_amps, 5);
  WiFiManagerParameter custom_current_threshold("current_threshold", "Current Delta Detection Threshold", current_threshold, 5);
  WiFiManagerParameter custom_current_delta_timeout("current_delta_timeout", "Time between delta verification", current_delta_timeout, 5);

  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  //wifiManager.setAPCallback(configModeCallback);

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
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }

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

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }

  ticker.detach();
  //keep LED on
  digitalWrite(LED, LOW);

  myCSE7766.setRX(1);
  myCSE7766.begin();

  secClient.setInsecure();

  Debug.begin(HOST_NAME);
  Debug.setResetCmdEnabled(true); // Enable the reset command
  debugA("Debugging Enabled");

}

void loop() {
  // get current current (he he, I probably will make this pun a lot)
  myCSE7766.handle();
  latestCurrent = myCSE7766.getCurrent();

  delta = previousCurrent - latestCurrent;

  // maybe make this recursive?
  if (fabs(delta) > atof(current_threshold)) {
    // check a few times to make sure there is a sustained load
    delay(500);
    myCSE7766.handle();
    latestCurrent = myCSE7766.getCurrent();
    delta = previousCurrent - latestCurrent;
    if ( fabs(delta) > atof(current_threshold)) {
      debugA("Change Detected");
      latestCurrent = changeDetected(previousCurrent, latestCurrent);
    }
  }
  previousCurrent = latestCurrent;

  static unsigned long mLastTime = 0;
  if ((millis() - mLastTime) >= UPDATE_TIME) {

    // Time
    mLastTime = millis();
    debugA("Voltage: %f", myCSE7766.getVoltage());
    debugA("Current: %f", myCSE7766.getCurrent());
    debugA("Energy:  %f", myCSE7766.getEnergy());
  }

  if(!digitalRead(SWITCH)){
    resetSettings();
  }
  Debug.handle();
  yield();
}
