// this is file battery_status_in_a_table.ino
/*
  WiFi Web Server

 A simple web server that shows the value of the analog input pins.

 This example is written for a network using WPA encryption. For
 WEP or WPA, change the WiFi.begin() call accordingly.


 created 13 July 2010
 by dlf (Metodo2 srl)
 modified 31 May 2012
 by Tom Igoe


  Find the full UNO R4 WiFi Network documentation here:
  https://docs.arduino.cc/tutorials/uno-r4-wifi/wifi-examples#wi-fi-web-server
 */

#include "WiFiS3.h"
/*
*  this sketch uses some of the code developed by SparkFun electronics 
*   Modified by Alok Kumar Mishra
*   e-mail:- akmishra_99@yahoo.com
*   Monday Nov 17,2025
*   This program/sketch uses sparkfun's battery baby sitter (BQ27771) with code/library developed by SparkFun and displays 
*   results in a web browser, http server is running on port 80
*/
#include <SparkFunBQ27441.h>

// Set BATTERY_CAPACITY to the design capacity of your battery in mAh.
// const uint16_t BATTERY_CAPACITY = 2850;
const uint16_t BATTERY_CAPACITY = 2000;
//lowest operational voltage in mV
const uint16_t TERMINATE_VOLTAGE = 3000;

//current at which charger stops charging battery in mA
//in case of Sparkfun Battery Babysitter board:
// 100mA charge current --> 12mA
// 500mA charge current --> 60mA
const uint16_t TAPER_CURRENT = 60;


#include "arduino_secrets.h"
///////please enter your sensitive data in the Secret tab/arduino_secrets.h
  
char ssid[] = SECRET_SSID;   // your network SSID (name)
char pass[] = SECRET_PASS;  // your network password (use for WPA, or use as key for WEP)
int keyIndex = 0;          // your network key index number (needed only for WEP)

int status = WL_IDLE_STATUS;

WiFiServer server(80);
unsigned int soc =0;              // Read state-of-charge (%)

unsigned int volts = 0;           // Read battery voltage (mV)
int current = 0;                  // Read average current (mA)
unsigned int fullCapacity =0;     // Read full capacity (mAh)
unsigned int capacity = 0;        // Read remaining capacity (mAh)
int power = 0;                    // Read average power draw (mW)
int health = 0;                   // Read state-of-health (%)

enum BatteryState {
  BATTERY_CHARGING,
  BATTERY_DISCHARGING,
  BATTERY_FULLY_CHARGED,
  BATTERY_FULLY_DISCHARGED,
  BATTERY_UNKNOWN_STATE
};

BatteryState battery_state = BATTERY_DISCHARGING;

String convert_battery_status_to_string( BatteryState battery_state  = BATTERY_FULLY_DISCHARGED)
{
    switch(battery_state) {

      case BATTERY_CHARGING:
        return String("charging");
      case BATTERY_DISCHARGING:
        return String("Discharging");
      case BATTERY_FULLY_CHARGED:
        return String("Fully charged");


    }

    return String("online");


}
extern void printWifiStatus();
String battery_state_string = convert_battery_status_to_string( BATTERY_UNKNOWN_STATE);
void setup() {
  //Initialize serial and wait for port to open:
  Serial.begin(115200);
  while (!Serial) {
    ;  // wait for serial port to connect. Needed for native USB port only
  }

  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true)
      ;
  }

  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade the firmware");
  }

  // attempt to connect to WiFi network:
  while (status != WL_CONNECTED) {
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(ssid);
    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    status = WiFi.begin(ssid, pass);

    // wait 10 seconds for connection:
    delay(10000);
  }
  server.begin();
  // you're connected now, so print out the status:
  printWifiStatus();
  if (!lipo.begin())  // begin() will return true if communication is successful
  {
    // If communication fails, print an error message and loop forever.
    Serial.println("Error: Unable to communicate with BQ27441.");
    Serial.println("  Check wiring and try again.");
    Serial.println("  (Battery must be plugged into Battery Babysitter!)");
    while (1)
      ;
  }
  Serial.println("Connected to BQ27441!");

  if (lipo.itporFlag())  //write config parameters only if needed
  {
    Serial.println("Writing gague config");

    lipo.enterConfig();                  // To configure the values below, you must be in config mode
    lipo.setCapacity(BATTERY_CAPACITY);  // Set the battery capacity

    /*
            Design Energy should be set to be Design Capacity × 3.7 if using the bq27441-G1A or Design
            Capacity × 3.8 if using the bq27441-G1B
        */
    lipo.setDesignEnergy(BATTERY_CAPACITY * 3.7f);

    /*
            Terminate Voltage should be set to the minimum operating voltage of your system. This is the target
            where the gauge typically reports 0% capacity
        */
    lipo.setTerminateVoltage(TERMINATE_VOLTAGE);

    /*
            Taper Rate = Design Capacity / (0.1 * Taper Current)
        */
    lipo.setTaperRate(10 * BATTERY_CAPACITY / TAPER_CURRENT);

    lipo.exitConfig();  // Exit config mode to save changes
  } else {
    Serial.println("Using existing gague config");
  }
}
String build_HTML_page()
{
  String HTML_page = R"(<!DOCTYPE html>
<html>
<head>
    <title>Rechargable LiIon battery Information</title>
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

    <h1>Battery Charge Status </h1>

    <table>
        <thead>
            <tr>
                <th>Time in seconds</th>
                <th>Charge (%)</th>
                <th>Voltage (mV)</th>
                <th>Current (mA)</th>
                <th>Full capacity (mAh) </th>
                <th>capacity remaining (mAh) </th>
                <th>Power (mW)</th>
                <th>State of Health (%)</th>
                <th>Charge Status </th>
            </tr>
        </thead>
        <tbody>
            <tr>
                <td>)" + String(millis() / 1000)  + R"(</td> 
                <td>)" + String(soc) + R"(</td>
                <td>)" + String(volts) + R"(</td>
                <td>)" + String(current) + R"(</td>
                <td>)" + String(fullCapacity) + R"(</td>
                <td>)" + String(capacity) + R"(</td>
                <td>)" + String(power) + R"(</td>
                <td>)" + String(health) + R"(</td>
                <td>)" + battery_state_string  + R"(</td>
                
            </tr>
        </tbody>
    </table>

</body>
</html>)";
  return  HTML_page;

}



String printBatteryStats() {
  String toPrint = "";
    // Read battery stats from the BQ27441-G1A
  soc = lipo.soc();                    // Read state-of-charge (%)
    
  volts = lipo.voltage();              // Read battery voltage (mV)
  current = lipo.current(AVG);                  // Read average current (mA)
  fullCapacity = lipo.capacity(FULL);  // Read full capacity (mAh)
  capacity = lipo.capacity(REMAIN);    // Read remaining capacity (mAh)
  power = lipo.power();                         // Read average power draw (mW)
  health = lipo.soh();                          // Read state-of-health (%)

  // Assemble a string to print
  toPrint = "[" + String(millis() / 1000) + "] ";
  toPrint += String(soc) + "% | ";
  toPrint += String(volts) + " mV | ";
  toPrint += String(current) + " mA | ";
  toPrint += String(capacity) + " / ";
  toPrint += String(fullCapacity) + " mAh | ";
  toPrint += String(power) + " mW | ";
  toPrint += String(health) + "%";

  //fast charging allowed
  if (lipo.chgFlag()) {
    toPrint += " CHG";
    battery_state_string = convert_battery_status_to_string(BATTERY_CHARGING);
  }
  //full charge detected
  if (lipo.fcFlag()) {
    toPrint += " FC";
    battery_state_string = convert_battery_status_to_string(BATTERY_FULLY_CHARGED);
  }
  //battery is discharging
  if (lipo.dsgFlag()) {
    toPrint += " DSG";
    battery_state_string = convert_battery_status_to_string(BATTERY_DISCHARGING);
  }
 
  Serial.println(toPrint);
  return toPrint;
}


void loop() {
  // listen for incoming clients
  WiFiClient client = server.available();
  if (client) {
    Serial.println("new client");
    // an HTTP request ends with a blank line
    boolean currentLineIsBlank = true;
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        Serial.write(c);
        // if you've gotten to the end of the line (received a newline
        // character) and the line is blank, the HTTP request has ended,
        // so you can send a reply
        if (c == '\n' && currentLineIsBlank) {
          // send a standard HTTP response header
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: text/html");
          client.println("Connection: close");  // the connection will be closed after completion of the response
          client.println("Refresh: 5");         // refresh the page automatically every 5 sec
          client.println();


          printBatteryStats();

          client.println(build_HTML_page() );

          break;
        }
        if (c == '\n') {
          // you're starting a new line
          currentLineIsBlank = true;
        } else if (c != '\r') {
          // you've gotten a character on the current line
          currentLineIsBlank = false;
        }
      }
    }
    // give the web browser time to receive the data
    delay(1);

    // close the connection:
    client.stop();
    Serial.println("client disconnected");
  }
}


void printWifiStatus() {
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength:
  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
}
