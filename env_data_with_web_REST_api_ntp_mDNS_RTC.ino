/*
  Full combined sketch:
  BME280 + SGP30 sensors, REST API, NTP, RTC, mDNS
  Arduino UNO R4 WiFi — board package renesas_uno 1.5.3

  Access via:
    http://weather.local/sensors/all
    http://weather.local/sensors/env
    http://weather.local/sensors/air

  Author: Alok Kumar Mishra (akmishra_99@yahoo.com)
  Date:   Monday May 4, 2026
*/

/*
  WiFi REST API Server
  Serves sensor data as JSON from BME280 and SGP30 sensors.

  Endpoints:
    GET /sensors/all  → all readings (temp, pressure, humidity, altitude, TVOC, eCO2, H2, ethanol)
    GET /sensors/env  → BME280 only (temp, pressure, humidity, altitude)
    GET /sensors/air  → SGP30 only (TVOC, eCO2, rawH2, rawEthanol)

  Modified/enhanced By Alok Kumar Mishra (akmishra_99@yahoo.com)
  Based on original WiFi Web Server sketch.
*/

#include "WiFiS3.h"
#include "WiFiUdp.h"
#include "RTC.h"
#include <Wire.h>
#include "Adafruit_SGP30.h"
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <iostream>
#include <sstream> // Required header
#include <string>

#include <ArduinoMDNS.h> 
#include "arduino_secrets.h"

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

int status = WL_IDLE_STATUS;

float temp = 0, pressure = 0, humidity = 0;
float altitude = 0, TOC = 0, eCO2 = 0, raw_H2 = 0, raw_ethanol = 0;

WiFiServer server(80);
Adafruit_BME280 bme;
Adafruit_SGP30 sgp;

#define SEALEVELPRESSURE_HPA (1013.25)
// Timezone offset from UTC in hours:
//   UTC-8  = -8.0   (US Pacific Standard Time)
//   UTC-7  = -7.0   (US Pacific Daylight Time)
//   UTC-5  = -5.0   (US Eastern Standard Time)
//   UTC-4  = -4.0   (US Eastern Daylight Time)
//   UTC+0  =  0.0   (UK / GMT)
//   UTC+5.5=  5.5   (India Standard Time)
//   UTC+9  =  9.0   (Japan Standard Time)
const float TIMEZONE_OFFSET_HOURS = -7.0;

const unsigned long NTP_SYNC_INTERVAL_MS = 60UL * 60UL * 1000UL;  // 1 hour
const char NTP_SERVER[] = "pool.ntp.org";

// ── NTP constants ─────────────────────────────────────────────────────────────

const int           NTP_PACKET_SIZE = 48;
const int           UDP_PORT        = 2390;
const int           NTP_PORT        = 123;
const unsigned long SEVENTY_YEARS   = 2208988800UL;

// ── Globals ───────────────────────────────────────────────────────────────────

WiFiUDP udp;
byte    packetBuffer[NTP_PACKET_SIZE];
int     wifiStatus     = WL_IDLE_STATUS;

unsigned long lastSyncMillis = 0;
unsigned long epochUTC       = 0;
unsigned long epochMillis    = 0;
bool          rtcWasSet      = false;
std::ostringstream oss_g;



// ── Custom time struct  ───────────────────────────────────────────────────────
// Named TimeComponents to avoid any conflict with the RTC library's RTCTime

struct TimeComponents {
  int year, month, day;
  int hour, minute, second;
  int weekday;   // 0 = Sunday
};
// The hostname advertised on the network.
// Devices will reach the board at:  http://<MDNS_HOSTNAME>.local
// Rules: lowercase letters, digits, hyphens only — no spaces, no underscores.
const char MDNS_HOSTNAME[] = "weather";
// ── WiFi ──────────────────────────────────────────────────────────────────────

// ── mDNS ──────────────────────────────────────────────────────────────────────
// ── mDNS globals ──────────────────────────────────────────────────────────────

// ArduinoMDNS needs its own UDP instance — must be separate from the NTP UDP
WiFiUDP        udpMDNS;
MDNS           mdns(udpMDNS);

void startMDNS() {
  // Begin mDNS with the chosen hostname.
  // After this the board is reachable at http://weather.local
  if (!mdns.begin(WiFi.localIP(), MDNS_HOSTNAME)) {
    Serial.println("ERROR: mDNS responder failed to start.");
    return;
  }

  // Advertise an HTTP service on port 80 so network browsers
  // (e.g. Bonjour Browser, avahi-browse) can discover it automatically.
  // Arguments: service type, protocol, port
  // mdns.addService("http", "tcp", 80);

  Serial.println("mDNS responder started.");
  Serial.print("Access the board at: http://");
  Serial.print(MDNS_HOSTNAME);
  Serial.println(".local");
}



void connectWiFi() {
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("ERROR: WiFi module not found.");
    while (true);
  }
  if (WiFi.firmwareVersion() < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Warning: WiFi firmware is outdated.");
  }
  Serial.print("Connecting to "); Serial.println(ssid);
  while (wifiStatus != WL_CONNECTED) {
    wifiStatus = WiFi.begin(ssid, pass);
    Serial.print(".");
    delay(10000);
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP address : "); Serial.println(WiFi.localIP());
  Serial.print("Signal RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
}

// ── NTP ───────────────────────────────────────────────────────────────────────

void buildNTPRequest() {
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0]  = 0b11100011;  // LI=3, Version=4, Mode=3 (client)
  packetBuffer[1]  = 0;
  packetBuffer[2]  = 6;
  packetBuffer[3]  = 0xEC;
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
}

unsigned long sendNTPRequest() {
  IPAddress ntpIP;
  if (WiFi.hostByName(NTP_SERVER, ntpIP) != 1) {
    Serial.println("NTP DNS lookup failed.");
    return 0;
  }
  Serial.print("NTP server: "); Serial.println(ntpIP);

  udp.begin(UDP_PORT);
  buildNTPRequest();
  udp.beginPacket(ntpIP, NTP_PORT);
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();

  unsigned long sent = millis();
  while (millis() - sent < 1500) {
    if (udp.parsePacket() >= NTP_PACKET_SIZE) {
      udp.read(packetBuffer, NTP_PACKET_SIZE);
      unsigned long hi  = (unsigned long)packetBuffer[40] << 8 | packetBuffer[41];
      unsigned long lo  = (unsigned long)packetBuffer[42] << 8 | packetBuffer[43];
      unsigned long raw = (hi << 16) | lo;
      udp.stop();
      return raw - SEVENTY_YEARS;
    }
    delay(10);
  }
  udp.stop();
  Serial.println("NTP reply timed out.");
  return 0;
}

// ── Time helpers ──────────────────────────────────────────────────────────────

TimeComponents epochToTimeComponents(unsigned long epoch) {
  TimeComponents tc;
  tc.second = epoch % 60; epoch /= 60;
  tc.minute = epoch % 60; epoch /= 60;
  tc.hour   = epoch % 24; epoch /= 24;

  tc.weekday = (epoch + 4) % 7;   // Jan 1 1970 was a Thursday (4)

  unsigned long days = epoch;
  int y = 1970;
  while (true) {
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    unsigned long diy = leap ? 366 : 365;
    if (days < diy) break;
    days -= diy;
    y++;
  }
  tc.year = y;

  bool leap = (tc.year % 4 == 0 && (tc.year % 100 != 0 || tc.year % 400 == 0));
  const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  for (int m = 0; m < 12; m++) {
    int d = (m == 1 && leap) ? 29 : dim[m];
    if ((int)days < d) { tc.month = m + 1; tc.day = (int)days + 1; break; }
    days -= d;
  }
  return tc;
}

const char* WEEKDAY_NAMES[] = {"Sunday","Monday","Tuesday","Wednesday",
                                "Thursday","Friday","Saturday"};
const char* MONTH_NAMES[]   = {"","Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};

void printPadded(int v) { if (v < 10) Serial.print('0'); Serial.print(v); }

void printTimeComponents(const TimeComponents &tc, const char *label) {
  Serial.print(label);
  Serial.print(WEEKDAY_NAMES[tc.weekday]); Serial.print(", ");
  Serial.print(MONTH_NAMES[tc.month]);     Serial.print(" ");
  if (tc.day < 10) Serial.print('0');
  Serial.print(tc.day);   Serial.print(" ");
  Serial.print(tc.year);  Serial.print("  ");
  printPadded(tc.hour);   Serial.print(":");
  printPadded(tc.minute); Serial.print(":");
  printPadded(tc.second); Serial.println();
}

// ── RTC helpers ───────────────────────────────────────────────────────────────

void setRTCFromTimeComponents(const TimeComponents &tc) {
  RTCTime rtcTime(
    tc.day,
    static_cast<Month>(tc.month),        // Month enum: JANUARY=1 … DECEMBER=12
    tc.year,
    tc.hour,
    tc.minute,
    tc.second,
    static_cast<DayOfWeek>(tc.weekday),  // DayOfWeek enum: SUNDAY=0 … SATURDAY=6
    SaveLight::SAVING_TIME_INACTIVE      // change to SAVING_TIME_ACTIVE if DST applies
  );

  if (RTC.setTime(rtcTime)) {
    rtcWasSet = true;
    Serial.println("RTC set successfully.");
  } else {
    Serial.println("ERROR: RTC.setTime() failed.");
  }
}

std::ostringstream printRTCTime() {
  RTCTime currentTime;
  std::ostringstream oss;
  if (!RTC.getTime(currentTime)) {
    Serial.println("RTC: failed to read time.");
    return oss;
  }
  int dow = static_cast<int>(currentTime.getDayOfWeek());
  int mon = static_cast<int>(currentTime.getMonth());
  oss << WEEKDAY_NAMES[dow] << " " <<  MONTH_NAMES[mon]  << " ";
  Serial.print("RTC:   ");
  Serial.print(WEEKDAY_NAMES[dow]);      Serial.print(", ");
  Serial.print(MONTH_NAMES[mon]);        Serial.print(" ");
  int d = currentTime.getDayOfMonth();
  if (d < 10) { Serial.print("  0"); oss << " 0" ; } else oss << " ";

  Serial.print(d);                       Serial.print(" ");
  oss << d << " " ;
  Serial.print(currentTime.getYear());   Serial.print("  ");
  oss << " " << currentTime.getYear() ;
  int temp = currentTime.getHour();
  printPadded(temp);    Serial.print(":");
  if ( temp < 10)  oss << " 0" << temp;  else oss << " " << temp ; 
 
  temp = currentTime.getMinutes();
  printPadded(temp); Serial.print(":");
  oss << ":" ; 
  if ( temp < 10 )  oss << "0" << temp; else oss << temp ;
  temp = currentTime.getSeconds();
  printPadded(temp); Serial.println();
  oss << ":" ;
  if ( temp < 10 )  oss << "0" << temp; else oss << temp ;

  return oss;
}

// ── NTP sync + RTC update ─────────────────────────────────────────────────────

void syncNTP() {
  Serial.println("\n── NTP Sync ─────────────────────────");
  unsigned long newEpoch = sendNTPRequest();

  if (newEpoch != 0) {
    epochUTC       = newEpoch;
    epochMillis    = millis();
    lastSyncMillis = epochMillis;
    Serial.print("UTC epoch : "); Serial.println(epochUTC);

    // Store LOCAL time in the RTC (RTC has no timezone concept)
    long          offsetSec  = (long)(TIMEZONE_OFFSET_HOURS * 3600.0);
    unsigned long localEpoch = (unsigned long)((long)epochUTC + offsetSec);
    TimeComponents tcLocal   = epochToTimeComponents(localEpoch);
    setRTCFromTimeComponents(tcLocal);

    Serial.println("NTP sync + RTC update complete.");
  } else {
    Serial.println("NTP sync failed — RTC unchanged.");
  }
  Serial.println("─────────────────────────────────────");
}

// ── millis()-based fallback clock ─────────────────────────────────────────────

unsigned long currentEpochUTC() {
  if (epochUTC == 0) return 0;
  return epochUTC + (millis() - epochMillis) / 1000UL;
}

unsigned long currentEpochLocal() {
  long offsetSec = (long)(TIMEZONE_OFFSET_HOURS * 3600.0);
  return (unsigned long)((long)currentEpochUTC() + offsetSec);
}

// ── Helpers ──────────────────────────────────────────────────────────────────

uint32_t getAbsoluteHumidity(float temperature, float humidity) {
  const float ah = 216.7f *
    ((humidity / 100.0f) * 6.112f *
     exp((17.62f * temperature) / (243.12f + temperature)) /
     (273.15f + temperature));
  return static_cast<uint32_t>(1000.0f * ah);
}

// Converts Celsius to Fahrenheit
float toFahrenheit(float c) { return c * 9.0 / 5.0 + 32.0; }

void printWifiStatus() {
  Serial.print("SSID: ");       Serial.println(WiFi.SSID());
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");       Serial.print(WiFi.RSSI()); Serial.println(" dBm");
}

// ── JSON builders ─────────────────────────────────────────────────────────────
// Returns current RTC time as a JSON string, or empty string if RTC not set
String rtcTimeJSON() {
  if (!rtcWasSet) return "\"rtc_time\":\"not synced\"";
  RTCTime t;
  if (!RTC.getTime(t)) return "\"rtc_time\":\"read error\"";

  // Build ISO-like string: "2026-05-04 10:42:11"
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
    t.getYear(), (int)t.getMonth(), t.getDayOfMonth(),
    t.getHour(), t.getMinutes(), t.getSeconds());
  return String("\"rtc_time\":\"") + buf + "\"";
}

String buildEnvJSON() {
  String j = "{";
  j += "\"temperature_C\":"  + String(temp,2)      + ",";
  j += "\"temperature_F\":"  + String(toFahrenheit(temp),2) + ",";
  j += "\"pressure_Pa\":"    + String(pressure,2)   + ",";
  j += "\"humidity_pct\":"   + String(humidity,2)   + ",";
  j += "\"altitude_m\":"     + String(altitude,2);
  j += "}";
  return j;
}

String buildAirJSON() {
  String j = "{";
  j += "\"TVOC_ppb\":"       + String(TOC,2)        + ",";
  j += "\"eCO2_ppm\":"       + String(eCO2,2)       + ",";
  j += "\"rawH2\":"          + String(raw_H2,2)     + ",";
  j += "\"rawEthanol\":"     + String(raw_ethanol,2);
  j += "}";
  return j;
}

String buildAllJSON() {
  String j = "{";
  j += rtcTimeJSON()           + ",";
  j += "\"environment\":"    + buildEnvJSON() + ",";
  j += "\"air_quality\":"    + buildAirJSON();
  j += "}";
  return j;
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────

// Sends standard JSON response headers + body, then closes the connection.
void sendJSON(WiFiClient &client, const String &body) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");   // allow cross-origin fetch
  client.println("Connection: close");
  client.println();
  client.println(body);
}

void send404(WiFiClient &client) {
  client.println("HTTP/1.1 404 Not Found");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.println("{\"error\":\"Not Found\"}");
}

// Reads the first line of the HTTP request and extracts the path.
// e.g. "GET /sensors/all HTTP/1.1" → "/sensors/all"
String readRequestPath(WiFiClient &client) {
  String requestLine = "";
  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') break;
      requestLine += c;
    }
  }
  // Drain remaining headers
  String headerLine = "";
  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      headerLine += c;
      if (headerLine.endsWith("\r\n\r\n")) break;
    }
  }
  // Parse path from "GET /path HTTP/1.1\r"
  int start = requestLine.indexOf(' ') + 1;
  int end   = requestLine.indexOf(' ', start);
  if (start < 1 || end < 0) return "";
  return requestLine.substring(start, end);
}
String build_HTML_page()
{
  String HTML_page = R"(<!DOCTYPE html>
<html>
<head>
    <title>Room Air Quality and envriornment data </title>
    <style>
        table, th, td {
            border: 1px solid black;
            border-collapse: collapse;
        }
        th, td {
            padding: 8px;
            text-align: left;
        }
    </style>
</head>
<body>

    <h1>Room environment state/data </h1>

    <table>
        <thead>
            <tr>
                <th> Time </th>
                <th>Temperature(degree Centrigrade)</th>
                <th>Pressure (Pascal)</th>
                <th>Humidity (%)</th>
                <th> Altitude (meter) </th>
                <th>Air Quality (TOC,ppb (parts per billion))</th>                
                <th>carbon di oxide content (eCO2,ppm (part per million))</th>
                <th> Raw H2 (hydrogen)       </th>
                <th> Raw ethanol  </th>
            </tr>
        </thead>
        <tbody>
            <tr>
                <td>)" + String((oss_g.str()).c_str())  + R"(</td>
                <td>)" + String(temp) + R"(</td> 
                <td>)" + String(pressure) + R"(</td>
                <td>)" + String(humidity) + R"(</td>
                <td>)" + String(altitude) + R"(</td>
                <td>)" + String(TOC) + R"(</td>
                <td>)" + String(eCO2) + R"(</td>
                <td>)" + String(raw_H2) + R"(</td>
                <td>)" + String(raw_ethanol) + R"(</td>
            </tr>
        </tbody>
    </table>

</body>
</html>)";
  return  HTML_page;

}
// ── setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  RTC.begin();      // start RTC peripheral before WiFi
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("WiFi module not found!");
  }
  if (WiFi.firmwareVersion() < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade WiFi firmware");
  }

  while (status != WL_CONNECTED) {
    Serial.print("Connecting to SSID: "); Serial.println(ssid);
    status = WiFi.begin(ssid, pass);
    delay(10000);
  }
  printWifiStatus();
  startMDNS();
  server.begin();
  
  syncNTP();
  // BME280 on default I2C (Wire)
  Wire.begin();
  if (!bme.begin(0x76)) {
    Serial.println("BME280 not found — check wiring / I2C address!");
  }

  // SGP30 on Wire1 (second I2C bus on UNO R4)
  Wire1.begin();
  if (!sgp.begin(&Wire1)) {
    Serial.println("SGP30 not found!");
  } else {
    Serial.print("SGP30 serial # ");
    Serial.print(sgp.serialnumber[0], HEX);
    Serial.print(sgp.serialnumber[1], HEX);
    Serial.println(sgp.serialnumber[2], HEX);
  }

  Serial.println("REST API running.");
  Serial.print("Base URL: http://"); Serial.println(WiFi.localIP());
  Serial.println("  GET /sensors/all");
  Serial.println("  GET /sensors/env");
  Serial.println("  GET /sensors/air");
}

// ── Sensor polling ────────────────────────────────────────────────────────────

int counter = 0;

void pollSensors() {
  // BME280
  temp     = bme.readTemperature();
  pressure = bme.readPressure();
  humidity = bme.readHumidity();
  altitude = bme.readAltitude(SEALEVELPRESSURE_HPA);

  // Feed absolute humidity to SGP30 for compensation
  sgp.setHumidity(getAbsoluteHumidity(temp, humidity));

  // SGP30 IAQ
  if (sgp.IAQmeasure()) {
    TOC  = sgp.TVOC;
    eCO2 = sgp.eCO2;
  } else {
    Serial.println("SGP30 IAQ measurement failed");
  }
  if (sgp.IAQmeasureRaw()) {
    raw_H2      = sgp.rawH2;
    raw_ethanol = sgp.rawEthanol;
  } else {
    Serial.println("SGP30 raw measurement failed");
  }

  // Print baseline every 30 cycles
  counter++;
  if (counter >= 30) {
    counter = 0;
    uint16_t eCO2_base, TVOC_base;
    if (sgp.getIAQBaseline(&eCO2_base, &TVOC_base)) {
      Serial.print("Baseline eCO2: 0x"); Serial.print(eCO2_base, HEX);
      Serial.print("  TVOC: 0x");        Serial.println(TVOC_base, HEX);
    }
  }
}

// ── loop ──────────────────────────────────────────────────────────────────────

void loop() {
  // IMPORTANT: MDNS.update() must be called every loop iteration.
  // It handles incoming mDNS queries and sends responses.
  // Without this call the hostname will not resolve on the network.
  mdns.run();
  // pollSensors();
  delay(1000);
  if (rtcWasSet) {
    oss_g = printRTCTime();                          // read directly from hardware RTC
  } else if (epochUTC != 0) {
    TimeComponents tcLocal = epochToTimeComponents(currentEpochLocal());
    printTimeComponents(tcLocal, "Local (millis): ");
  } else {
    Serial.println("Waiting for NTP sync...");
  }
  WiFiClient client = server.available();
  if (!client) return; else pollSensors();

  Serial.println("Client connected");
  String path = readRequestPath(client);
  Serial.print("Request path: "); Serial.println(path);

  if (path == "/sensors/all") {
    sendJSON(client, buildAllJSON());
  } else if (path == "/sensors/env") {
    sendJSON(client, buildEnvJSON());
  } else if (path == "/sensors/air") {
    sendJSON(client, buildAirJSON());
  } else if (path == "/" ) {

    // send a standard HTTP response header
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");  // the connection will be closed after completion of the response
    client.println("Refresh: 5");  // refresh the page automatically every 5 sec
    client.println();
    client.println("<!DOCTYPE HTML>");
    client.println("<html>");

    
    client.println(build_HTML_page());
    

 
  } else {
    send404(client);
  }

  delay(10);
  client.stop();
  Serial.println("Client disconnected");
}