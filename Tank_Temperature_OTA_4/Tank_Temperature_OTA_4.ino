#include <AsyncEventSource.h>
#include <AsyncJson.h>
#include <AsyncWebSocket.h>
#include <AsyncWebSynchronization.h>
#include <ESPAsyncWebServer.h>
#include <StringArray.h>
#include <WebAuthentication.h>
#include <WebHandlerImpl.h>
#include <WebResponseImpl.h>

/*
   Boat Hot Water Heater Controller for ESP8266
   Controls a water heater relay based on DS18B20 temperature sensor readings.
   Provides a web interface to set temperature and start/stop the heater.
   Supports Over-The-Air (OTA) updates and Tailscale via Nginx reverse proxy.

   Fix: Start button works over Tailscale/Nginx but not locally.
   Solution: Dynamically set WebSocket and HTTP paths based on URL (/HotWater/ for Nginx, / for local).

   Features:
   - Web interface with slider (20-39°C) and Start/Stop button.
   - WebSocket for real-time temperature, status, and timer updates.
   - 30-minute timeout to save energy.
   - Watchdog for reliability.
   - OTA updates via ArduinoOTA.
   - Debug logging for Tailscale/Nginx issues.

   Hardware:
   - ESP8266 (e.g., NodeMCU) with 1MB+ flash.
   - DS18B20 on D2 (4.7k pull-up to 3.3V).
   - Relay on D4 (active LOW, normally open).

   Setup:
   - change the "oatpassword" to yours
   - Update ssid, password, and ota_password.
   - Initial upload via USB to enable OTA.
   - Access locally: http://192.168.1.184/ this is my address change to yours in the IP config area
   - Access via Tailscale: http://<nginx-tailscale-ip>/HotWater/
   - Uncomment heaterOn = false in loop() to keep heater OFF after setpoint.

   Debugging:
   - Serial Monitor (115200 baud) for HTTP/WebSocket logs.
   - Browser console (F12 > Console) for client-side errors.
   - Test Start button and slider on local and Tailscale networks.
   - Chaged from Dallas to DS18B20 temperature library (Was having issues which hopefully this cures)
*/

// Libraries
#include <ESP8266WiFi.h>        // Wi-Fi functionality
#include <ESPAsyncTCP.h>        // Async TCP for web server
#include <ESPAsyncWebServer.h>  // Async HTTP and WebSocket server
#include <OneWire.h>            // OneWire for DS18B20 communication
#include <DS18B20.h>            // DS18B20 temperature readings
#include <ArduinoOTA.h>         // OTA updates

// Network credentials
const char* ssid = "Sailrover2G";      // Wi-Fi SSID
const char* password = "robitaille";   // Wi-Fi password

// Pin definitions
const int switch1 = 2;        // D4, relay (active LOW)
const int oneWireBus = 4;     // D2, DS18B20 (4.7k pull-up)

// DS18B20 setup
OneWire oneWire(oneWireBus);  // Initialize OneWire on pin D2
DS18B20 sensor(&oneWire);     // Initialize DS18B20 with OneWire object

// Static IP configuration
IPAddress local_IP(192, 168, 1, 184);  // ESP8266 IP
IPAddress gateway(192, 168, 1, 1);     // Gateway
IPAddress subnet(255, 255, 255, 0);    // Subnet
IPAddress primaryDNS(8, 8, 8, 8);      // DNS 1
IPAddress secondaryDNS(8, 8, 4, 4);    // DNS 2

// Web server and WebSocket
AsyncWebServer server(80);    // HTTP on port 80
AsyncWebSocket ws("/ws");     // WebSocket at /ws

// Control variables
unsigned long lastAliveMillis = 0;         // Watchdog timer
const unsigned long ALIVE_TIMEOUT = 60000; // 60s watchdog
unsigned long startMillis = 0;             // Heater start time
const unsigned long TIMEOUT = 30 * 60 * 1000; // 30min timeout
String tankStatus = "OFF";                 // Heater status
bool heaterOn = false;                     // Heater state
float currentTemp = 0.0;                   // Current temperature
String setpointTemp = "38";                // Default setpoint (38°C)
const char* PARAM_INPUT = "value";         // Slider param
const char* PARAM_ACTION = "action";       // Toggle param
unsigned long lastWiFiCheck = 0;           // Wi-Fi check timer
const unsigned long WIFI_CHECK_INTERVAL = 10000; // Check Wi-Fi every 10s
const unsigned long RESET_INTERVAL = 1800000; // 1/2 hour in milliseconds (adjustable)

// OTA configuration
const char* ota_password = "robitaille"; // Change to secure password

// HTML webpage
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Calentador Control</title>
  <style>
    html {font-family: Arial; display: inline-block; text-align: center;}
    h2 {font-size: 2.3rem;}
    p {font-size: 1.9rem;}
    body {max-width: 400px; margin:0px auto; padding-bottom: 25px;}
    .slider { -webkit-appearance: none; margin: 14px; width: 300px; height: 25px; background: #FFD65C;
      outline: none; -webkit-transition: .2s; transition: opacity .2s;}
    .slider::-webkit-slider-thumb {-webkit-appearance: none; appearance: none; width: 35px; height: 35px; background: #003249; cursor: pointer;}
    .slider::-moz-range-thumb { width: 35px; height: 35px; background: #003249; cursor: pointer;}
    .button {padding: 15px 25px; font-size: 24px; cursor: pointer; text-align: center; color: white; border: none; border-radius: 15px;}
    .button-on {background-color: #4CAF50;}
    .button-off {background-color: #ff0000;}
  </style>
</head>
<body>
  <h2>Calientador</h2>
  <p>Temp. Presente: <span id="currentTemp">0</span> C</p>
  <p>Temp. Quiero: <span id="textSliderValue">%SETPOINTTEMP%</span> C</p>
  <p><input type="range" onchange="updateSlider(this)" id="tempSlider" min="29" max="39" value="%SETPOINTTEMP%" step="1" class="slider"></p>
  <p>Status: <span id="tankStatus">%TANKSTATUS%</span></p>
  <p>Queda Tiempo: <span id="timeRemaining">--:--</span></p>
  <p><button id="controlButton" class="button button-off" onclick="toggleHeater()">Start</button></p>

<script>
  // Dynamically set paths based on URL
  var isNginx = window.location.pathname.startsWith('/HotWater');
  var wsPath = isNginx ? '/HotWater/ws' : '/ws';
  var basePath = isNginx ? '/HotWater' : '';
  var host = window.location.host;
  var wsUrl = 'ws://' + host + wsPath;
  var baseUrl = 'http://' + host + basePath;
  var ws = null;

  // Connect to WebSocket with retry
  function connectWebSocket() {
    console.log('Attempting WebSocket connection to ' + wsUrl);
    ws = new WebSocket(wsUrl);
    
    ws.onopen = function() {
      console.log('WebSocket connected');
    };

    ws.onmessage = function(event) {
      var data = JSON.parse(event.data);
      document.getElementById('currentTemp').innerHTML = data.temp.toFixed(1);
      document.getElementById('tankStatus').innerHTML = data.status;
      document.getElementById('timeRemaining').innerHTML = data.timer;
      var button = document.getElementById('controlButton');
      if (data.status === 'CALENTANDO') {
        button.className = 'button button-on';
        button.innerHTML = 'Stop';
      } else {
        button.className = 'button button-off';
        button.innerHTML = 'Start';
      }
    };

    ws.onerror = function(error) {
      console.log('WebSocket Error: ', error);
    };

    ws.onclose = function() {
      console.log('WebSocket closed, retrying in 5 seconds...');
      setTimeout(connectWebSocket, 5000);
    };
  }

  connectWebSocket();

  // Send HTTP request with retry
  function sendRequest(url, retries = 3, timeout = 10000) {
    return new Promise((resolve, reject) => {
      let attempts = 0;
      function attempt() {
        attempts++;
        var xhr = new XMLHttpRequest();
        xhr.open("GET", baseUrl + url, true);
        xhr.timeout = timeout;
        xhr.onreadystatechange = function() {
          if (xhr.readyState == 4) {
            if (xhr.status == 200) {
              resolve('OK');
            } else {
              console.log('Request failed: ' + url + ', Status: ' + xhr.status);
              if (attempts < retries) {
                console.log('Retrying (' + attempts + '/' + retries + ')...');
                setTimeout(attempt, 1000);
              } else {
                reject('Failed after ' + retries + ' attempts');
              }
            }
          }
        };
        xhr.ontimeout = function() {
          console.log('Request timed out: ' + url);
          if (attempts < retries) {
            console.log('Retrying (' + attempts + '/' + retries + ')...');
            setTimeout(attempt, 1000);
          } else {
            reject('Timed out after ' + retries + ' attempts');
          }
        };
        xhr.send();
      }
      attempt();
    });
  }

  // Update slider
  function updateSlider(element) {
    var sliderValue = document.getElementById('tempSlider').value;
    document.getElementById('textSliderValue').innerHTML = sliderValue;
    sendRequest("/slider?value=" + sliderValue)
      .catch(error => console.log('Slider update error: ', error));
  }

  // Toggle heater
  function toggleHeater() {
    var action = document.getElementById('controlButton').innerHTML === 'Start' ? 'start' : 'stop';
    sendRequest("/toggle?action=" + action)
      .catch(error => console.log('Toggle error: ', error));
  }
</script>
</body>
</html>
)rawliteral";

// Process HTML template variables
String processor(const String& var) {
  if (var == "SETPOINTTEMP") return setpointTemp;
  if (var == "TANKSTATUS") return tankStatus;
  return String();
}

// WebSocket event handler
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.println("WebSocket client connected, ID: " + String(client->id()) + ", IP: " + client->remoteIP().toString());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.println("WebSocket client disconnected, ID: " + String(client->id()));
  } else if (type == WS_EVT_ERROR) {
    Serial.println("WebSocket error, Client ID: " + String(client->id()));
  }
}

void checkPeriodicReset() {
  static unsigned long lastResetMillis = 0;

  
  if (!heaterOn) { // Timer not running (no heating cycle)
    if (millis() - lastResetMillis >= RESET_INTERVAL) {
      Serial.println("Periodic reset triggered: Timer not running.");
      digitalWrite(switch1, LOW); // Ensure heater is off
      delay(100); // Brief pause for Serial
      ESP.restart(); // Reset ESP8266
      lastResetMillis = millis(); // Update after reset (won’t execute due to restart)
    }
  } else {
    lastResetMillis = millis(); // Reset timer if heating to align with idle periods
  }
}



// Reconnect Wi-Fi
void reconnectWiFi() {
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
    Serial.println("WiFi disconnected, reconnecting...");
    WiFi.disconnect(true); // Force disconnect
    delay(100); // Brief pause to clear buffers
    WiFi.begin(ssid, password);
    unsigned long startAttempt = millis();
    const unsigned long RECONNECT_TIMEOUT = 30000; // 30s timeout
    int retryCount = 0;
    const int MAX_RETRIES = 3;
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < RECONNECT_TIMEOUT) {
      delay(100); // Reduced for responsiveness
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nReconnected, IP: " + WiFi.localIP().toString());
    } else {
      Serial.println("\nFailed to reconnect, retry: " + String(retryCount + 1));
      retryCount++;
      if (retryCount >= MAX_RETRIES) {
        Serial.println("Max retries reached, resetting Wi-Fi module...");
        WiFi.mode(WIFI_OFF); // Turn off Wi-Fi
        delay(1000);
        WiFi.mode(WIFI_STA); // Restart in station mode
        WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS); // Reapply static IP
        WiFi.begin(ssid, password);
      }
    }
    lastWiFiCheck = millis();
  }
}

// Setup
void setup() {
  Serial.begin(115200);
  pinMode(switch1, OUTPUT);
  digitalWrite(switch1, LOW); // Heater OFF
  sensor.begin(); // Initialize DS18B20 sensor
//sensors.setWaitForConversion(false);

  // Configure static IP
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  }

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "Connected, IP: " + WiFi.localIP().toString() : "Failed to connect");

  // OTA setup
  ArduinoOTA.setHostname("BoatHeater");
  ArduinoOTA.setPassword(ota_password);
  ArduinoOTA.onStart([]() {
    Serial.println("OTA: Starting update");
    digitalWrite(switch1, LOW); // Safety: Heater OFF
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA: Update complete");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA: Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready");

  // WebSocket setup
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // HTTP routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Serving / to " + request->client()->remoteIP().toString());
    request->send_P(200, "text/html", index_html, processor);
    lastAliveMillis = millis();
  });

  server.on("/slider", HTTP_GET, [](AsyncWebServerRequest *request) {
    String clientIP = request->client()->remoteIP().toString();
    if (request->hasParam(PARAM_INPUT)) {
      setpointTemp = request->getParam(PARAM_INPUT)->value();
      Serial.println("Slider updated to " + setpointTemp + "°C by " + clientIP);
      lastAliveMillis = millis();
    } else {
      Serial.println("Slider request missing value param from " + clientIP);
    }
    request->send(200, "text/plain", "OK");
  });

  server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
    String clientIP = request->client()->remoteIP().toString();
    if (request->hasParam(PARAM_ACTION)) {
      String action = request->getParam(PARAM_ACTION)->value();
      Serial.println("Toggle action: " + action + " from " + clientIP);
      if (action == "start") {
        heaterOn = true;
        startMillis = millis();
      } else {
        heaterOn = false;
        digitalWrite(switch1, LOW);
      }
      Serial.println("Toggle processed, heaterOn: " + String(heaterOn));
      lastAliveMillis = millis();
    } else {
      Serial.println("Toggle request missing action param from " + clientIP);
    }
    request->send(200, "text/plain", "OK");
  });

  server.begin();
  lastAliveMillis = millis();
}

// Loop
void loop() {
  ArduinoOTA.handle();
  ws.cleanupClients();
  reconnectWiFi();

  // Read temperature
  sensor.requestTemperatures(); // Request temperature conversion
  delay(750);                  // Wait for conversion (12-bit resolution)
  currentTemp = sensor.getTempC(); // Read temperature in Celsius


  // Validate temperature
  if (currentTemp == -127.0 || currentTemp == 85.0) {
    Serial.println("Error: Invalid DS18B20 reading");
    return;
  }

  // Heater control
  if (heaterOn) {
    unsigned long currentMillis = millis();
    if (currentMillis - startMillis >= TIMEOUT) {
      heaterOn = false;
      digitalWrite(switch1, LOW);
      tankStatus = "APAGADOS";
    } else if (currentTemp >= setpointTemp.toFloat()) {
      digitalWrite(switch1, LOW);
      tankStatus = "APAGADOS";
      // heaterOn = false; // Uncomment to stay OFF
    } else {
      digitalWrite(switch1, HIGH);
      tankStatus = "CALENTANDO";
    }
  } else {
    digitalWrite(switch1, LOW);
    tankStatus = "APAGADOS";
  }

  // Update clients
  static unsigned long lastUpdate = 0;
  unsigned long updateInterval = (ws.count() > 0) ? 1000 : 5000; // Slower updates when no clients
  if (millis() - lastUpdate >= updateInterval) {
    // Log heap and RSSI for debugging
    Serial.print("Free heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.print(", WiFi RSSI: ");
    Serial.println(WiFi.RSSI());

    String timerStr = "--:--";
    if (heaterOn) {
      unsigned long elapsed = millis() - startMillis;
      if (elapsed < TIMEOUT) {
        unsigned long remaining = TIMEOUT - elapsed;
        unsigned long minutes = remaining / 60000;
        unsigned long seconds = (remaining % 60000) / 1000;
        timerStr = String(minutes) + ":" + (seconds < 10 ? "0" : "") + String(seconds);
      } else {
        timerStr = "0:00";
      }
    }

    if (ws.count() > 0) {
      String json = "{\"temp\":" + String(currentTemp) + ",\"status\":\"" + tankStatus + "\",\"timer\":\"" + timerStr + "\"}";
      ws.textAll(json);
      Serial.println("Sent WebSocket update: " + json);
    }

    Serial.print("Temp: ");
    Serial.print(currentTemp);
    Serial.print("°C, Setpoint: ");
    Serial.print(setpointTemp);
    Serial.print("°C, Status: ");
    Serial.print(tankStatus);
    Serial.print(", Timer: ");
    Serial.print(timerStr);
    Serial.print(", WiFi: ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    Serial.print(", Clients: ");
    Serial.println(ws.count());

    lastUpdate = millis();
    lastAliveMillis = millis();
  }

  // Watchdog
  if (millis() - lastAliveMillis > ALIVE_TIMEOUT) {
    Serial.println("Watchdog triggered. WiFi status: " + String(WiFi.status()) + ", Free heap: " + String(ESP.getFreeHeap()));
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Attempting Wi-Fi reset before restart...");
      WiFi.mode(WIFI_OFF);
      delay(1000);
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
      delay(5000); // Give 5s to reconnect
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi reset failed, restarting ESP...");
      ESP.restart();
    } else {
      Serial.println("Wi-Fi reconnected, canceling restart");
      lastAliveMillis = millis();
    }
  }
  checkPeriodicReset();//to stop the freezing periodic bug
}
