#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WiFiUdp.h>
//#include <ArduinoOTA.h> // This does work, but I /think/ it effects the reliability of BTLE. So disabled here.
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <string>
#include "wificreds.h"


// Define your WiFi settings in wificreds.h

#ifndef HOSTNAME
  #define HOSTNAME "blegateway"
#endif
#define LED 2


// Store IP address globally
IPAddress ipAddr;

// In order to support the automatic discovery stuff I want to keep a list of the known
// devices and their RSSIs local to each ESP32.  I've pondered the right way of doing this
// for too long, so I'm just going to set a limit of 5 devices per ESP32.  If you need more than
// this then you can increase the size of the array.  I don't know how much impact this will
// have, probably not much.  I haven't yet done any testing to see where the limits are, how much
// free memory we have to play with, etc.  I want to try this out, so just going with it.  It can
// be optimised another day.  I've considered a dictionary like solution, a vector, two arrays
// (one for mac, one for rssi) and so on.  In the end I've settled on a struct for the mac and rssi
// and a single array of 5 structs for the devices.
// This is all new to me, if you can improve it please send a PR and teach me.
const uint8_t maxDevices = 5;
// struct TrionesDevice {
//     std::string macAddr;
//     int rssi;
// }
typedef struct {
    std::string macAddr;
    int rssi;
//    std::string location;
} TrionesDevice;

TrionesDevice localDevices[maxDevices];
//uint8_t deviceCount = 0;

//NimBLE Stuff
const NimBLEUUID NOTIFY_SERVICE ("FFD0");
const NimBLEUUID NOTIFY_CHAR    ("FFD4");
const NimBLEUUID MAIN_SERVICE   ("FFD5");
const NimBLEUUID WRITE_CHAR     ("FFD9");

// MQTT Stuff
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
const char broker[] = "mqtt";
const int  port = 1883;
std::string mqttPubTopic;
std::string mqttControlTopic;
std::string mqttGlobalTopic;

// Forward declarations
// I don't know how to C++
bool findTrionesDevices();
bool do_write(NimBLEAddress bleAddr, const uint8_t *payload, size_t len);
unsigned long previousMillis = 0;

void sendMqttMessage(const JsonDocument& local_doc ) {
    mqttClient.beginMessage(mqttPubTopic.c_str());
    serializeJson(local_doc, mqttClient);
    mqttClient.endMessage();
};
void sendMqttMessage(const JsonDocument& local_doc, std::string topic) {
    mqttClient.beginMessage(topic.c_str());
    serializeJson(local_doc, mqttClient);
    mqttClient.endMessage();
};
void sendMqttMessage(std::string message) {
    mqttClient.beginMessage(mqttPubTopic.c_str());
    mqttClient.print(message.c_str());
    mqttClient.endMessage();
};
void sendMqttMessage(std::string message, std::string topic) {
    mqttClient.beginMessage(topic.c_str());
    mqttClient.print(message.c_str());
    mqttClient.endMessage();
}
void sendMqttMessage(const __FlashStringHelper * ifsh) {
    const char *p = (const char PROGMEM *) ifsh;
    mqttClient.beginMessage(mqttPubTopic.c_str());
    mqttClient.print(p);
    mqttClient.endMessage();
};


class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient * pClient) {
        //pClient->updateConnParams(120,120,120,60); // This is more lenient 
        //pClient->setConnectionParams(7,7,0,200);// This is what the lights would like us to use.  It does work, but it doesn't leave much room to manoeuvre 
        digitalWrite(LED, !digitalRead(LED));
    };

    void onDisconnect(NimBLEClient * pClient) {
        digitalWrite(LED, !digitalRead(LED));
        StaticJsonDocument<64> temp_doc;
        temp_doc["mac"] = pClient->getPeerAddress().toString();
        temp_doc["disconnected"] = true;
        sendMqttMessage(temp_doc);
    };

    bool onConnParamsUpdateRequest(NimBLEClient * pClient, const ble_gap_upd_params * params) {
        Serial.println("Doing some connection params stuff");
        // https://github.com/h2zero/NimBLE-Arduino/issues/140#issuecomment-720100023
        /*
            The first 2 parameters are multiples of 0.625ms, they dictate how often a packet is sent to maintain the connection.
            The third lets the peripheral skip the amount of connection packets you specify, usually 0.
            The fourth is a multiple of 10ms that dictates how long to wait before considering the connection dropped if no packet is received, I usually keep this around 100-200 (1-2 seconds).
            The last 2 parameters I would suggest not specifying, they are the scan parameters used when calling connect() as it will scan to find the device you're connecting to, the default values work well.
        */
        // if(params->itvl_min < 24) { /** 1.25ms units */
        //     return false;
        // } else if(params->itvl_max > 40) { /** 1.25ms units */
        //     return false;
        // } else if(params->latency > 2) { /** Number of intervals allowed to skip */
        //     return false;
        // } else if(params->supervision_timeout > 100) { /** 10ms units */
        //     return false;
        // }

        // We're just going to accept what they ask for, anything to try and get it to work.
        // Update: this might be a bad idea, because we have to trust that what the devices are asking for is actually reasonable
        // and that's probably not a safe assumption.  So, once the call back stuff is fixed, think about this some more...
        // Thought about is some more.. some initial testing says letting the lights do what they want makes them a bit happier and easier to talk to. So leaving this like this for now.
        digitalWrite(LED, !digitalRead(LED));
        return true;        
    };
};

static ClientCallbacks clientCB;

void onScanComplete(NimBLEScanResults results) {
    Serial.println("Done scanning");
    char buffer[80];
    sprintf(buffer, "{\"state\":true,\"message\":\"Scan complete\",\"scanningHost\":\"%s\"}", ipAddr.toString().c_str());
    sendMqttMessage(buffer);
    Serial.println("Here's what's in the table:");
    for (int i=0; i < maxDevices; i++) {
        Serial.println(i);
        Serial.println(localDevices[i].macAddr.c_str());
        Serial.println(localDevices[i].rssi);
    };
};

class MyAdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        digitalWrite(LED, !digitalRead(LED));
        // This is only for devices found with a scan on this ESP32
        // not devices seen over MQTT from other ESP32s

        std::string trionesName ("Triones");
        if (advertisedDevice->haveName()) {
            std::string deviceName = advertisedDevice->getName();
            if (deviceName.find(trionesName) != std::string::npos) {
                digitalWrite(LED, !digitalRead(LED));
                StaticJsonDocument<128> doc; // per https://arduinojson.org/v6/assistant/
                doc["mac"]  = advertisedDevice->getAddress().toString();
                doc["name"] = advertisedDevice->getName();
                doc["rssi"] = advertisedDevice->getRSSI();
                doc["scanningDevice"] = WiFi.localIP().toString();

                //if (deviceCount >= maxDevices) deviceCount = 0; //wrap around
                TrionesDevice discoveredDevice;

                discoveredDevice.macAddr = doc["mac"].as<std::string>();
                discoveredDevice.rssi = doc["rssi"];
                for (int i=0; i < maxDevices; i++) {
                    std::string mac = localDevices[i].macAddr;
                    int rssi = localDevices[i].rssi;
                    if (discoveredDevice.macAddr == mac) {
                        // We've already got this in our list
                        // but it might have come from somewhere else.  Who wins?
                        //Serial.println("Checking dupe");
                        int realRssi = (rssi > 0 ? -rssi : rssi); // invert realRssi if it's remote
                        if (discoveredDevice.rssi > realRssi) {
                            // We are better
                            localDevices[i].rssi = discoveredDevice.rssi;
                            Serial.println("Updated table with a better rssi from a local scan");
                            std::string scanTopic = mqttPubTopic;
                            scanTopic += "/scan";
                            sendMqttMessage(doc,scanTopic);
                            // The rssi for this device is now minus whatever, meaning we have the best connection
                            // done with this device now;
                        };
                        // } else {
                        //     // The other end has the better connection
                        //     // do nothing.
                        // };

                        // if (rssi > 0) {
                        //     // > 0 means that this device is on a remote ESP32, not us
                        //     // Do we have a better signal though?
                        //     int realRssi = -rssi;
                        //     if (discoveredDevice.rssi > realRssi) {
                        //         // We are better
                        //         localDevices[i].rssi = discoveredDevice.rssi;
                        //         Serial.println("Updated table with a better rssi from a local scan");
                        //         std::string scanTopic = mqttPubTopic;
                        //         scanTopic += "/scan";
                        //         sendMqttMessage(doc,scanTopic);
                        //         // The rssi for this device is now minus whatever, meaning we have the best connection
                        //         // done with this device now;
                        //     } else {
                        //         // The other end has the better connection
                        //         // do nothing.
                        //     };
                        // } else {
                        //     // rssi is < 0, which means that this device is known by us already
                        //     // we only keep the best rssi that we see during a scan
                        //     if (discoveredDevice.rssi > rssi) {
                        //         // This scan result has better rssi that the previous one, so update the table
                        //         localDevices[i].rssi = discoveredDevice.rssi;
                        //         Serial.println("Updated table with a better rssi from a local scan 2");
                        //         std::string scanTopic = mqttPubTopic;
                        //         scanTopic += "/scan";
                        //         sendMqttMessage(doc,scanTopic);
                        //     } else {
                        //         // The existing entry has better rssi, leave it as it is.
                        //         // do nothing
                        //     };
                        // };
                        // We found an existing device, either updated it or didnt, but we're done.
                        return;
                    }
                };

                // If we are here, then we didnt have an existing entry for this mac, so find a slot for it in the table, and add it.
                for (int i=0; i < maxDevices; i++) {
                    std::string mac = localDevices[i].macAddr;
                    if (mac == "FISH") {
                        // spare slot, let's write in here
                        localDevices[i] = discoveredDevice;
                        Serial.println("Added new device to table");
                        std::string scanTopic = mqttPubTopic;
                        scanTopic += "/scan";
                        sendMqttMessage(doc,scanTopic);
                        return;
                    };
                };
                Serial.println("Did have room in the table for this device");
            };
        };
    };
};

void notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify){
    if (length == 12) {
        if ((pData[0] == 0x66) && (pData[11] == 0x99)) { // static
            digitalWrite(LED, !digitalRead(LED));
            String power_state = (pData[2] == 0x23) ? "true" : "false"; // 0x23 is on, 0x24 is off
            const uint8_t mode  = pData[3]; // See mode section for full list
            const uint8_t speed = pData[5]; // 1 fast, 255 slow
            const uint8_t red   = pData[6]; // R
            const uint8_t green = pData[7]; // G
            const uint8_t blue  = pData[8]; // B
            const std::string addr = pRemoteCharacteristic->getRemoteService()->getClient()->getPeerAddress().toString();
            char buffer[95];
            sprintf(buffer, "{\"mac\":\"%s\", \"power\":%s, \"rgb\":[%u,%u,%u], \"speed\", %u, \"mode\":%u}", addr.c_str(), power_state, red, green, blue, speed, mode);
            sendMqttMessage(buffer);
            digitalWrite(LED, !digitalRead(LED));
        }
    }
}

void mqttCallback(int messageSize) {
    digitalWrite(LED, !digitalRead(LED));
    std::string mt = mqttClient.messageTopic().c_str();
    //Serial.print("X ");
    //Serial.println(mt.c_str());
    std::string scanTopic = mqttPubTopic;
    scanTopic += "/scan";
    // Schema looks like this:
    //   triones/control/<myIpAddr> - control devices on this ESP32.  JSON payload with instructions.
    //   triones/control/global     - send message to all ESP32s
    //   triones/stats/<myIpAddr>   - Status messages as JSON
    //
    // Payload looks like:
    /* {
        // Needed for every message
        "action" : "ping" | "scan" | "set" ,

        // Needed for most messages
        "mac" : "aa:bb:cc:dd:ee:ff" (a mac address as a string),

        // action specific
        "disconnect" - (needs a mac),

    */

    if ( (mt != mqttControlTopic) && (mt != mqttGlobalTopic) && (mt != scanTopic) ) {
        // TODO: this should probably just return.  It happens too often really.
        //Serial.println("NAMFM");
        return;
    }

    StaticJsonDocument<200> doc;  // Shrink this later? 200 seems OK though.  100 caused stack overflows
    DeserializationError err = deserializeJson(doc, mqttClient);

    if (err) {
        Serial.println(F("Couldn't understand the JSON object. Giving up"));
        sendMqttMessage(F("{\"state\":false, \"message\":\"Invalid JSON\"}"));
        return;
    }

    if (mt == scanTopic) {
        // A scan result was received
        std::string myIpAddr = ipAddr.toString().c_str();
        if (doc["scanningDevice"] == myIpAddr) {
            //Serial.println("Received scan result, but it was one that I sent");
            return;
        } else {
            // Should be from elsewhere
            // A scan results has come in from another device.  Do we know about the same device?
            // If so, how does our RSSI compare?  If the other ESP32 has a better connection then
            // bodge our mac to some invalid thing.  Hmm, this is going to be super racy. 
            // Meh, it'll probably work most of the time.  Remember, devices which are paired do
            // not show up in scans.  What the worst that could happen?
            std::string remoteMac = doc["mac"];
            int8_t remoteRssi = doc["rssi"];
            Serial.print("Dealing with scan results from another esp32:\n");
            Serial.print("MAC: ");
            Serial.println(remoteMac.c_str());
            Serial.print("RSSI: ");
            Serial.println(remoteRssi);

            // iterate our list of devices to see who is the best
            for (int i=0; i < maxDevices; i++) {
                TrionesDevice localDevice = localDevices[i];
                std::string localMac = localDevice.macAddr;
                if (localMac == remoteMac) {
                    Serial.println("Found the same device in our list");
                    int8_t localRssi = localDevice.rssi;
                    if (localRssi <= remoteRssi) {
                        // Our device has worse rssi, so replace it with the remote one.
                        // We indicate remote by inverting the rssi so it's above zero
                        Serial.println("Other end is better");
                        localDevice.macAddr = -remoteRssi;
                        return;  // Don't need to keep iterating the array I think;
                    } else {
                        // mac matches but new remote rssi is worse
                        // so leave everything as is.
                        return;
                    };
                };
            };
            // If we got here, then we ran through the table without finding a match, so we need to add this device as new
            for (int i=0; i < maxDevices; i++) {
                std::string m = localDevices[i].macAddr;
                if (m == "FISH") {
                    TrionesDevice device;
                    device.macAddr = remoteMac;
                    device.rssi = -remoteRssi; // negate to represent remote
                    localDevices[i] = device;
                    Serial.println("Found free slot for remote and added it");
                    return;
                };
            };
            // If we get here then I think we couldn't find a slot
            Serial.println("Couldn't find a slot for remote device");
        };
        return;
    };

    std::string action = doc["action"];
    if ( doc["action"].isNull() ) {
        Serial.println("No action supplied.");
        sendMqttMessage(F("{\"state\":false, \"message\":\"No action set\"}"));
        return;
    };

    // Deal with global requests
    if (mt == mqttGlobalTopic) {
        if (action == "ping") {
            StaticJsonDocument<64> ping_doc;
            ping_doc["ipAddr"] = ipAddr.toString();
            ping_doc["active"] = true;
            sendMqttMessage(ping_doc);
            digitalWrite(LED, !digitalRead(LED));
            return;
        } else if (action == "scan") {
            findTrionesDevices();
            digitalWrite(LED, !digitalRead(LED));
            return;
        } else if (action == "set") {
            // Global set action called, can we service this request?
            Serial.println("Global set called, checking if we can help");
            std::string requestMac = doc["mac"].as<std::string>();
            bool match = false;
            for (int i=0; i<maxDevices; i++) {
                TrionesDevice device = localDevices[i];
                std::string ourMac = device.macAddr;
                if (ourMac == requestMac) {
                    // We found a match. Do we have a negative rssi?  If so, then we can help
                    if (device.rssi < 0) {
                        match = true;
                        break;
                    };
                };
            if (!match) {
                Serial.println("No matching device locally, can't help");
                return;
            }
            // If we get here we should have a device in our list so we can pass this request on as if it was intended for us
            // in the first place.  Do that by bodging the topic
            mt == mqttControlTopic;
            // :chef_kiss:
            }
        } else {
            Serial.println("Fell through to the end of the global topic options");
            return;
        }
       //return;
    };


    // Deal with my requests
    if (mt == mqttControlTopic) {
        if (action == "disconnect") {
            const char* mac = doc["mac"];
            NimBLEAddress addr = NimBLEAddress(mac);
            NimBLEClient * pClient = nullptr;
            pClient = NimBLEDevice::getClientByPeerAddress(addr);
            if (pClient) {
                if (pClient->isConnected()) {
                    Serial.println("Disconnecting client");
                    pClient->disconnect();
                }
            }
            return;
        };

        if (action == "status") {
            digitalWrite(LED, !digitalRead(LED));
            if (doc["mac"].isNull()){
                Serial.println("Can't get status, no MAC");
                sendMqttMessage(F("\"state\":false, \"message\":\"No MAC for status\"}"));
            } else {
                const char* mac = doc["mac"];
                NimBLEAddress addr = NimBLEAddress(mac);
                uint8_t payload[] = {0xEF, 0x01, 0x77};
                if (do_write(addr, payload, sizeof(payload))) {
                    Serial.println(F("Status request successful"));
                } else {
                    Serial.println(F("Status failed"));
                    sendMqttMessage(F("{\"state\":false}"));
                }
            }
            return;
        };

        if (action == "set") {
            if (doc["mac"].isNull()){
                Serial.println("Cant do SET, no MAC address");
                sendMqttMessage(F("{\"state\":false, \"message\":\"No MAC for SET\"}"));
                return;
            }

            const char* mac = doc["mac"];
            NimBLEAddress addr = NimBLEAddress(mac);

            if (!doc["power"].isNull()) {
                digitalWrite(LED, !digitalRead(LED));
                Serial.println("Setting power");
                uint8_t power = ( doc["power"] == true ? 0x23 : 0x24 );
                uint8_t payload[] = {0xCC, power, 0x33};
                if (do_write(addr, payload, sizeof(payload))) {
                    Serial.println("Successfully set power");
                    sendMqttMessage(F("{\"state\":true, \"message\":\"Power set on device\"}"));
                } else {
                    Serial.println("Power set failed");
                    sendMqttMessage(F("{\"state\":false, \"message\":\"Failed to set power on device\"}"));
                }
            };

            if (doc["rgb"]) {
                digitalWrite(LED, !digitalRead(LED));
                uint8_t payload[7] = {0x56, 00, 00, 00, 00, 0xF0, 0xAA};
                uint8_t percentage;
                if (doc["percentage"] == nullptr) {
                    percentage = 100;
                } else {
                    percentage = doc["percentage"];
                }
                // TODO the lights only scale in chunks of 15, so make this do that
                float scale_factor = percentage / 100.0;
                payload[1] = (int) doc["rgb"][0] * scale_factor;
                payload[2] = (int) doc["rgb"][1] * scale_factor;
                payload[3] = (int) doc["rgb"][2] * scale_factor;
                if (do_write(addr, payload, sizeof(payload))) {
                    Serial.println("Set RGB worked");
                    sendMqttMessage(F("{\"state\":true, \"message\":\"RGB set on device\"}"));
                } else {
                    Serial.println("Set RGB failed");
                    sendMqttMessage(F("{\"state\":false, \"message\":\"RGB set failed\"}"));
                }
            };

            if (doc["mode"]) {
                digitalWrite(LED, !digitalRead(LED));
                // # 37 : 0x25: Seven color cross fade
                // # 38 : 0x26: Red gradual change
                // # 39 : 0x27: Green gradual change
                // # 40 : 0x28: Blue gradual change
                // # 41 : 0x29: Yellow gradual change
                // # 42 : 0x2A: Cyan gradual change
                // # 43 : 0x2B: Purple gradual change
                // # 44 : 0x2C: White gradual change
                // # 45 : 0x2D: Red, Green cross fade
                // # 46 : 0x2E: Red blue cross fade
                // # 47 : 0x2F: Green blue cross fade
                // # 48 : 0x30: Seven color strobe flash
                // # 49 : 0x31: Red strobe flash
                // # 50 : 0x32: Green strobe flash
                // # 51 : 0x33: Blue strobe flash
                // # 52 : 0x34: Yellow strobe flash
                // # 53 : 0x35: Cyan strobe flash
                // # 54 : 0x36: Purple strobe flash
                // # 55 : 0x37: White strobe flash
                // # 56 : 0x38: Seven color jumping change
                // # 65 : 0x41: Looks like this might be solid colour as set by remote control
                // # 97 : 0x61: RGB fade
                // # 98 : 0x62: RGB cycle
                //
                // Speed : 1 = fast --> 31 = slow
                uint8_t mode  = doc["mode"];
                uint8_t speed = doc["speed"];
                if ( (speed<1) || (speed>31) || (doc["speed"].isNull()) ){
                    speed = 15;
                }
                if ( ((mode >= 0x25) && (mode <= 0x38)) || ( (mode >= 0x61) && (mode <= 0x62) ) ) {
                    uint8_t payload[4] = {0xBB, 0x27, 0x7F, 0x44};
                    payload[1] = mode;
                    payload[2] = speed;
                    if (do_write(addr, payload, sizeof(payload))) {
                        Serial.println("Set mode and speed correct");
                        sendMqttMessage(F("{\"state\":true, \"message\":\"mode set on device\"}"));
                    } else {
                        Serial.println("Set mode failed");
                        sendMqttMessage(F("{\"state\":false, \"message\":\"mode set failed\"}"));
                    }
                };
            };            
        };
    };
};



bool do_write(NimBLEAddress bleAddr, const uint8_t* payload, size_t length){
    digitalWrite(LED, !digitalRead(LED));
    NimBLEClient * pClient = nullptr;
    if (NimBLEDevice::getClientListSize()) {
        // There are some existing clients available, try and find one to reuse
        pClient = NimBLEDevice::getClientByPeerAddress(bleAddr);
        if (pClient) {
            // It's possible (and in our case, likely) that we're still connected to the device, so we need to deal with that here
            if ( !pClient->isConnected()) {
                if (!pClient->connect(bleAddr, false)) {
                    Serial.println("Tried to reconnect to known device, but failed");
                    //return false;
                } else {
                    Serial.println("Correctly reconnected to device");
                }
            } else {
                Serial.println("Found existing connected client, so using that");
            }
        } else {
            Serial.println("No existing client, trying an old one");
            pClient = NimBLEDevice::getDisconnectedClient();
        }
    }
    // If we get here we are either connected OR we need to try and reuse an old client
    if (!pClient) {
        // There wasn't an old one to reuse, create a new one
        if(NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS) {
            Serial.println("Max connections reached.  No more available.");
            return false;
        }
        pClient = NimBLEDevice::createClient();
        Serial.println("Created new client");
        pClient->setConnectionParams(7,7,0,200); // As a test setting this here to see if we can connect 1st time more often.
        //  Testing shows that this does seem to allow initial connections and reconnections more readily. Even though the tolerances are much lower
        //  it does seem to work.  It will retry at RSSI -90, but will connect usually within the first 5 tries.

        pClient->setClientCallbacks(&clientCB, false);
        pClient->setConnectTimeout(5);

        if (!pClient->connect(bleAddr, false)) {
            // Created a new client to use, but it failed, so get rid of it
            //NimBLEDevice::deleteClient(pClient);
            Serial.println("Created a new client, but still failed to connect. Removed client");
            // Not returning here, because I want to try and re-try below.
            // When we get here we have a client, but we are not connected
            //return false;
        }
    }

    // I think we can wrap this in a retry.  Ways of getting here are:
    // - There isn't an existing connection so we created a new one
    // - There is an existing one, but we didn't connect
    // Assume that if there is an existing client, it should just work

    if(!pClient->isConnected()) {
        int i = 0;
        while ( (i <= 10) && (!pClient->isConnected()) ) {
            if (!pClient->connect(bleAddr, false)) {
                Serial.print("Failed to connect in retry loop: ");
                Serial.println(i);
                sendMqttMessage("{\"state\":\"pending\", \"message\":\"Connect failed. Retrying...\"}");
                digitalWrite(LED, !digitalRead(LED));
                delay(1000);
                i++;
            }
        };
        if (!pClient->isConnected()){
            Serial.println("Retrying failed.  Sorry");
            sendMqttMessage("{\"state\":false, \"message\":\"Connect retry failed to connect\"}");
            // Throw away the non-working client.  This must exist for us to have got this far...
            NimBLEDevice::deleteClient(pClient);
            return false;
        }
    }

    // If we get here, then we really should be connected by now
    Serial.print("Connected to device: ");
    Serial.println(pClient->getPeerAddress().toString().c_str());
    // Serial.print("RSSI: ");
    // Serial.println(pClient->getRssi());

    // Do the actual work...
    NimBLERemoteService* pSvc = nullptr;
    NimBLERemoteCharacteristic* pChr = nullptr;
    NimBLERemoteService* nSvc = nullptr;
    NimBLERemoteCharacteristic* nChr = nullptr;

    nSvc = pClient->getService(NOTIFY_SERVICE);

    if (nSvc) {
        Serial.println("nSvc correct");
        nChr = nSvc->getCharacteristic(NOTIFY_CHAR);
        if (nChr->canNotify()) {
            nChr->subscribe(true, &notifyCB);
        };
    } else {
        Serial.println("Failed to get nsvc");
        return false;
    }

    if (!pClient) {
        Serial.println("Client has gone away");
        return false;
    }
    pSvc = pClient->getService(MAIN_SERVICE);
    if (pSvc) {
        pChr = pSvc->getCharacteristic(WRITE_CHAR);
        if (pChr->canWrite()){
            if (pChr->writeValue(payload, length, false)) {
                Serial.println("Write complete");
            };
            digitalWrite(LED, !digitalRead(LED));
        } else Serial.println("WRITE: Couldnt write");
    } else {
        Serial.println("Failed to write data.");
    }
    //pClient->disconnect();
    return true;
}

bool findTrionesDevices(){
    digitalWrite(LED, !digitalRead(LED));
    NimBLEScan* pBLEScan = NimBLEDevice::getScan();
    if (!pBLEScan) {
        return false;
    }
    if (pBLEScan->isScanning()) {
        return false;
    }
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), false);
    // We're ready to scan, but first let's clear our local array of devices and old scan results
    pBLEScan->clearDuplicateCache();
    pBLEScan->clearResults();
    for (int i=0; i < maxDevices; i++) {
        localDevices[i].macAddr = "FISH";
    };
    //deviceCount = 0;
    int s = random(10)*1000;
    Serial.print("Delay: ");
    Serial.println(s);
    delay(s);  // If a global scan happens, try not to all start at the same time.  Help to avoid the race?
    Serial.println("Doing scan for 30 seconds");
    pBLEScan->start(30, *onScanComplete, false);
    return true;
}

void setup (){
    Serial.begin(115200);

    // Setup Nimble
    Serial.println("Starting NimBLE Client");
    NimBLEDevice::setScanDuplicateCacheSize(10);
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */
    NimBLEDevice::setMTU(23);  // This is what the lights ask for, so set up front. This made it much more reliable to connect first time.
    
    // Set up Wifi
    Serial.println("Setting up Wifi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(STASSID, STAPSK);
    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
        Serial.println("Couldn't connect to Wifi! Rebooting...");
        delay(5000);
        ESP.restart();
    }
    ipAddr = WiFi.localIP();
    uint8_t oct = ipAddr[3];
    std::string hostname = HOSTNAME;
    hostname += std::to_string(oct);;
    WiFi.hostname(hostname.c_str());
    Serial.print("IP address: ");
    Serial.println(ipAddr);
    Serial.print("Hostname: ");
    Serial.println(hostname.c_str());

    //Set up OTA
    //  After all the effort I went to in order to enable OTA, it seems that OTA is quite chatty on the wifi
    //  and makes BT LE harder.  So disabling it for now.  Maybe it can come back later.  If you need it, then there
    //  only a few lines to uncomment
    //ArduinoOTA.setHostname(HOSTNAME);
    //ArduinoOTA.begin();

    // Setup MQTT
    // Schema looks like this:
    //   triones/control/<myIpAddr> - control devices on this ESP32.  JSON payload with instructions.
    //   triones/control/global     - send message to all ESP32s
    //   triones/stats/<myIpAddr>   - Status messages as JSON

    if (mqttClient.connect(broker, port)) {
        mqttControlTopic = "triones/control/";
        mqttGlobalTopic = mqttControlTopic;
        mqttControlTopic += ipAddr.toString().c_str();     
        mqttGlobalTopic += "global";
        mqttPubTopic = "triones/status";
        std::string subToPub = mqttPubTopic;
        subToPub += "/#";

        Serial.print("Control topic: ");
        Serial.println(mqttControlTopic.c_str());
        Serial.print("Global topic: ");
        Serial.println(mqttGlobalTopic.c_str());
        Serial.print("Pub topic: ");
        Serial.println(mqttPubTopic.c_str());
        mqttClient.subscribe(mqttControlTopic.c_str());
        mqttClient.subscribe(mqttGlobalTopic.c_str());
        mqttClient.subscribe(mqttGlobalTopic.c_str());
        mqttClient.subscribe(subToPub.c_str());
        mqttClient.onMessage(mqttCallback);
    } else {
        Serial.println("MQTT connection failed!");
    }
    pinMode(LED, OUTPUT);
    digitalWrite(LED, !digitalRead(LED));
    sendMqttMessage("BLE Relay Ready"); // TODO:  Replace this with a proper JSON message including the IP address
    sendMqttMessage(WiFi.localIP().toString().c_str());
    Serial.println("READY");
}


void loop (){
    mqttClient.poll();
    //ArduinoOTA.handle();

    // Flash the light, of course
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= 2500) {
        previousMillis = millis();
        digitalWrite(LED, !digitalRead(LED));
    }
}
