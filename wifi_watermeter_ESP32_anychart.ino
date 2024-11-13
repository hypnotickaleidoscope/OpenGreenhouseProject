// Load Wi-Fi library
#include <WiFi.h>
// ntp time lib
#include <NTPClient.h>
// UDP weeeeee
#include <WiFiUdp.h>
// EEPROM memory mgmt
#include <EEPROM.h>


#define EEPROM_SIZE 2048 // in bytes
#define triggerArrayOffset 10


// Replace with your network credentials
const char* ssid     = "SSID";
const char* password = "WIFI-PASSWORD";


// Set web server port number to 80
WiFiServer server(80);


// Variable to store the HTTP request
String header;

String MYTZ = "CST6CDT,M3.2.0,M11.1.0";  // <-- changed to CST-CDT

int sensorPin = A0;   // select the input pin for the moisture sensor reading
const int pumpTriggerPin = 4 ;  // DO trigger pin to control the water pump


int sensorValue = 0;  // variable to store the value coming from the sensor
int historicVals[96];
int historicIdx = 0;

int moistVal;
int readFlag = 0;

typedef struct 
{
  uint8_t hour;     // 0-23
  uint8_t minute;   // 0-59
  uint8_t duration; // 0-254 (in seconds)
  uint8_t enabled;  // 0-1 set but off/on
  uint8_t set;      // 0-1 off/on
}__attribute__((packed)) pumpData;

#define NT 255                   // Number of Trigger
pumpData pumpTrigger[NT];      // pump trigger data struct array
long int triggerTimer[NT]; 
int oneShotTrigger = 0;
long int oneShotTimer = 0;
int oneShotFlag = 0;
int numTriggersSet = 0;         // running total of trigger events curently set/saved
int triggerFlag = 0;            // flag indicated if the trigger is currently set ON
int triggerCheckFlag = 1;       // flag indicates if we have checked for a trigger event this second
int triggerIdx = 0;
int triggerDisable[NT];
int triggerMasterDisable;
bool processRequest = false;

/* 
scaling for the moisture reading hardware
I would love to find a better way to auto calibrate
because it seems somewhat arbitrary and might change
over time
*/
const int minScale = 291;
const int maxScale = 685;

// Define NTP Client to get time
const long utcOffsetInSeconds = -21600;
int hour, minute, second;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

// Current time
unsigned long currentTime = millis();
// Previous time
unsigned long previousTime = 0; 
// Define timeout time in milliseconds (example: 2000ms = 2s)
const long timeoutTime = 2000;

String dataValue = ""; // Variable to store the data from GET request

void setup() 
{
  // clear out any historic data that might still be initialized
  memset(&historicVals, 0, sizeof(historicVals));
  memset(&pumpTrigger, 0, sizeof(pumpTrigger));

  // initialize serial comms
  Serial.begin(115200);
  // initialize EEPROM with predefined size
  

  // Initialize the output variables as outputs
  pinMode(sensorPin, INPUT);
  // Initialize the output tgrigger pin as off
  pinMode(pumpTriggerPin, OUTPUT);
  digitalWrite(pumpTriggerPin, 0);

  // manually setting the trigger for now
  // in the future I would like to be able to set this from the web UI
  pumpTrigger[0].hour = 4;       // 4pm
  pumpTrigger[0].minute = 30;      // :00 minutes
  pumpTrigger[0].duration = 5;   // 60 second duration
  pumpTrigger[0].enabled = 1;
  pumpTrigger[0].set = 1;
  triggerDisable[0] = 0;
  numTriggersSet++;
  triggerMasterDisable = 1;

  // Connect to Wi-Fi network with SSID and password
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  // Print local IP address and start web server
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  //configTime(MYTZ, "pool.ntp.org");

  timeClient.begin();
  timeClient.setTimeOffset(21600);
  timeClient.update();
  hour = timeClient.getHours();
  minute = timeClient.getMinutes();
  second = timeClient.getSeconds();
  
  Serial.print("NTP Time: ");
  Serial.print(hour);
  Serial.print(":");
  Serial.print(minute);
  Serial.print(":");
  Serial.println(second);

  // read data
  sensorValue = analogRead(sensorPin);
  float scaled = (float)((float)sensorValue - (float)minScale)/ (float)((float)maxScale - (float)minScale);
  moistVal = (1-scaled)*100;

  eepromRead();

  // init test the relay trigger
  //digitalWrite(pumpTriggerPin, 1);
  //Serial.println("trigger on");
  //delay(5 * 1000);
  //digitalWrite(pumpTriggerPin, 0);
  //Serial.println("trigger off");

  server.begin();
}

void loop()
{
  pumpController();


  WiFiClient client = server.available();   // Listen for incoming clients

  //Serial.println(client.status());
  timeClient.update();
  second = timeClient.getSeconds();

  if (!client && second==0 && !readFlag) 
  {
    sensorRead();
  }
  else if (client)
  {                             // If a new client connects,
    Serial.println("New Client.");          // print a message out in the serial port
    String currentLine = "";                // make a String to hold incoming data from the client
    currentTime = millis();
    previousTime = currentTime;
    processRequest = false;
    while (client.connected() && currentTime - previousTime <= timeoutTime) { // loop while the client's connected
      currentTime = millis();         
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        Serial.write(c);                    // print it out the serial monitor
        header += c;

        String request = "";

        if(c == '?')
        {
          processRequest = true;
        }

        request = header;
    
        if (c == '\n') {                    // if the byte is a newline character
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:

          if (currentLine.length() == 0) {

          int valueStartIndex,valueEndIndex;

          if(processRequest)
          {
            // Match the request

            String triggerUpdateAction = "";

            // TRIGGER UPDATE ACTION
            valueStartIndex = request.indexOf("triggeraction=");
            if (valueStartIndex != -1) {
              valueEndIndex = request.indexOf('&', valueStartIndex);
              if (valueEndIndex == -1) {
                valueEndIndex = request.length();
              }
              dataValue = request.substring(valueStartIndex + 14, valueEndIndex);
              //Serial.println("Trigger action: " + dataValue);
              triggerUpdateAction = dataValue;
            }

            // TRIGGER INDEX
            valueStartIndex = request.indexOf("triggerindex=");
            if (valueStartIndex != -1) {
              valueEndIndex = request.indexOf('&', valueStartIndex);
              if (valueEndIndex == -1) {
                valueEndIndex = request.length();
              }
              dataValue = request.substring(valueStartIndex + 13, valueEndIndex);
              Serial.println("Trigger index: " + dataValue);
              Serial.println("Trigger action: " + triggerUpdateAction);
              Serial.println(strcmp(triggerUpdateAction.c_str(),triggerUpdateAction.c_str()));
              Serial.println(strcmp(triggerUpdateAction.c_str(),"Disable"));
              Serial.println(strcmp(triggerUpdateAction.c_str(),"Enable"));
              Serial.println(strcmp(triggerUpdateAction.c_str(),"Delete"));
              if(!triggerUpdateAction.compareTo("Disable"))
              {
                pumpTrigger[dataValue.toInt()].enabled = 0;
                Serial.println("Trigger index disabled: " + dataValue);
              }
              else if(!triggerUpdateAction.compareTo("Enable"))
              {
                pumpTrigger[dataValue.toInt()].enabled = 1;
                Serial.println("Trigger index enabled: " + dataValue);
              }
              else if(!triggerUpdateAction.compareTo("Delete"))
              {
                memset(&pumpTrigger[dataValue.toInt()], 0, sizeof(pumpData));
                triggerSort();
                Serial.println("Trigger index deleted: " + dataValue);
                Serial.print("number of triggers: ");
                Serial.println(numTriggersSet);
              }
              triggerSort();
            }

            // HOUR
            valueStartIndex = request.indexOf("hour=");
            if (valueStartIndex != -1) {
              valueEndIndex = request.indexOf('&', valueStartIndex);
              if (valueEndIndex == -1) {
                valueEndIndex = request.length();
              }
              triggerSort();
              dataValue = request.substring(valueStartIndex + 5, valueEndIndex);
              Serial.println("hours: " + dataValue);
              pumpTrigger[numTriggersSet].hour = dataValue.toInt();
            }
            
            // MINUTE
            valueStartIndex = request.indexOf("minute=");
            if (valueStartIndex != -1) {
              valueEndIndex = request.indexOf('&', valueStartIndex);
              if (valueEndIndex == -1) {
                valueEndIndex = request.length();
              }
              dataValue = request.substring(valueStartIndex + 7, valueEndIndex);
              Serial.println("minutes: " + dataValue);
              pumpTrigger[numTriggersSet].minute = dataValue.toInt();
            }
            
            // DURATION
            valueStartIndex = request.indexOf("duration=");
            if (valueStartIndex != -1) {
              valueEndIndex = request.indexOf('&', valueStartIndex);
              if (valueEndIndex == -1) {
                valueEndIndex = request.length();
              }
              dataValue = request.substring(valueStartIndex + 9, valueEndIndex);
              Serial.println("duration: " + dataValue);
              pumpTrigger[numTriggersSet].duration = dataValue.toInt();
              pumpTrigger[numTriggersSet].set = 1;
              numTriggersSet++;
              Serial.print("number of triggers: ");
              Serial.println(numTriggersSet);
              triggerSort();
            }

            // ONE SHOT TRIGGER
            valueStartIndex = request.indexOf("oneshot=");
            if (valueStartIndex != -1) {
              valueEndIndex = request.indexOf('&', valueStartIndex);
              if (valueEndIndex == -1) {
                valueEndIndex = request.length();
              }
              dataValue = request.substring(valueStartIndex + 8, valueEndIndex);
              Serial.println("duration: " + dataValue);
              oneShotTrigger = dataValue.toInt();
            }

            // MASTER DISABLE
            valueStartIndex = request.indexOf("masterdisable=");
            if (valueStartIndex != -1) {
              valueEndIndex = request.indexOf('&', valueStartIndex);
              if (valueEndIndex == -1) {
                valueEndIndex = request.length();
              }
              dataValue = request.substring(valueStartIndex + 14, valueEndIndex);
              Serial.println("master disable: " + dataValue);
              triggerMasterDisable = dataValue.toInt();
            }

            // DELETE ALL TIME TRIGGERS
            valueStartIndex = request.indexOf("deletealltimetriggers");
            if (valueStartIndex != -1) {
              valueEndIndex = request.indexOf('&', valueStartIndex);
              if (valueEndIndex == -1) {
                valueEndIndex = request.length();
              }
              //dataValue = request.substring(valueStartIndex + 22, valueEndIndex);
              //Serial.println("delete all time triggers: " + dataValue);
              //if(dataValue.compareTo("true"))
              {
                  memset(&pumpTrigger, 0, sizeof(pumpTrigger));
              }
            }

            eepromWrite();
          }

            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();
            
            // Display the HTML web page
            client.println("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Smart Moisture Meter</title><meta name=\"description\" content=\"\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");

            client.println("<script src=\"https://cdn.anychart.com/releases/8.11.1/js/anychart-bundle.min.js\"></script>");
            client.println("<!-- styles for this little demo page -->");
            client.println("<style type=\"text/css\">");

            client.println("body{");
            client.println("    background-color: #f5f5f5;");
            client.println("    margin: 0;");
            client.println("    padding: 0;");
            client.println("    font-family: Arial, \"Helvetica Neue\", Helvetica, sans-serif;");
            client.println("}");

            client.println(".page {");
            client.println("    margin: 10px 0;");
            client.println("    width:100%;");
            client.println("    display: flex;");
            client.println("    flex-direction: row;");
            client.println("    flex-wrap: wrap;");
            client.println("    justify-content: center;");
            client.println("    align-items: center;");
            client.println("    horizontal-align: center;");
            client.println("    padding: 15px 0;");
            client.println("    top: -40px;");
            client.println("}");

            client.println(".rounded {");
            client.println("    margin: 0px 10px;");
            client.println("    border-radius: 25px;");
            client.println("}");

            client.println("form {text-align: center;}");

            client.println("h1{");
            client.println("    margin: 40px 0 60px 0;");
            client.println("}");

            client.println(".dark-area {");
            client.println("    background-color: #666;");
            client.println("    padding: 40px;");
            client.println("    margin: 0 -40px 20px -40px;");
            client.println("    clear: both;");
            client.println("}");

            client.println(".clearfix:before,.clearfix:after {content: \" \"; display: table;}");
            client.println(".clearfix:after {clear: both;}");
            client.println(".clearfix {*zoom: 1;}");

            client.println("html, body, #container { ");
            client.println("    width: 100%; height: 100%; margin: 0; padding: 0; ");
            client.println("} ");

            client.println(".barcontainer, #barcontainer { ");
            client.println("    width: 100%; height: 100%; margin: 25px 0 20px 0; padding: 0; ");
            client.println("} ");

            client.println("</style>");
       
            client.println("</head>");

            client.println("<body>");

            if(processRequest)
            {
              client.println("<script>history.replaceState({}, null, \"/\");</script>");
            }

            client.println("<div class=\"page\" style=\"margin-bottom: 0; background-color: #212121; color: #ffffff; margin-top: 0; margin-left: 0; margin-right: 0; padding: -40 0 -40 0; height: 75px; position: relative; top: -100;   \">");

            client.print("<h1>Smart Soil Moisture Meter</h1>");
            client.println("</div>");
            client.print("<div class=\"page\" style=\"margin-bottom: 0; font-size: 6em; font-weight: 700; height: 25px; position: relative; top: -115; color: #");
            if(moistVal > 70)
              client.print("0143F2");
            else if(moistVal > 30)
              client.print("056608");
            else
              client.print("FF4F48");
            client.println("\">");
            client.print(moistVal);
            client.println("%");
            client.println("</div>");
            client.println("<div class=\"page\" style=\"margin: 0 0 0 0\">");
            client.println("<div id=\"barcontainer\" style=\"height: 200px;\"></div>");
            client.println("<br/>");
            client.println("<div id=\"container\" style=\"height: 300px;\"></div>");
            client.println("<script>anychart.onDocumentReady(function () {");

            client.println("  // add data");
            client.println("      var data = [");
            
            for(int i=96, k=24; i>0; i--)
            //for(int i=0; i<96; i++)
            {
              if( i % 4 == 0)
              {
              client.print("  [\"");
              client.print(k);
              client.print("h\",   ");
              client.print(historicVals[i-1]);
              client.println("  ],  ");
              k--;
              }
              else
              {

              client.print("  [\"");
              client.print(k);
              client.print(".");
              if((i % 4) == 1)
              {
              client.print(25);
              }
              else if((i % 4) == 2)
              {
              client.print(5);
              } 
              else if((i % 4) == 3)
              {
                client.print(75);
              }              
              
              client.print("h\",   ");
              client.print(historicVals[i-1]);
              client.println("  ],  ");
              }
            }

            client.print("  [\"now\",   ");
            client.print(moistVal);
            client.println("  ],  ");

            //client.println("        [\"20h\", 44],");
            //client.println("        [\"19h\", 40],");
            //client.println("        [\"18h\", 61],");
            //client.println("        [\"17h\", 91],");
            //client.println("        [\"16h\", 78],");
            //client.println("        [\"15h\", 67],");
            //client.println("        [\"14h\", 56],");
            //client.println("        [\"13h\", 51],");
            //client.println("        [\"12h\", 53],");
            //client.println("        [\"11h\", 58],");
            //client.println("        [\"10h\", 49],");
            //client.println("        [\"9h\", 63],");
            //client.println("        [\"8h\", 64],");
            //client.println("        [\"7h\", 70],");
            //client.println("        [\"6h\", 61],");
            //client.println("        [\"5h\", 60],");
            //client.println("        [\"4h\", 30],");
            //client.println("        [\"3h\", 25],");
            //client.println("        [\"2h\", 99],");
            //client.println("        [\"0h\", 50],");
            client.println("      ];");
            
            client.println("// create a data set");
            client.println("      var dataSet = anychart.data.set(data);");
            client.println("      // map the data for all series");
            client.println("      var firstSeriesData = dataSet.mapAs({x: 0, value: 1});");
            client.println("      // create a line chart");
            client.println("      var chart = anychart.line();");
            client.println("      // create the series and name them");
            client.println("      var firstSeries = chart.line(firstSeriesData);");
            client.println("      firstSeries.name(\"Moisture\");");
            client.println("      // add a legend");
            client.println("      chart.legend().enabled(true);");
            client.println("      chart.yAxis().title(\"Soil Moisture\");");
            client.println("      chart.xAxis().title(\"Last Sensor Time\");");
            client.println("      chart.yScale().minimum(0).maximum(100);");
            client.println("      // Set width bound");
            client.println("      chart.width('100%');");
            client.println("      // Set height bound");
            client.println("      chart.height('300px');");
            client.println("      // add a title");
            client.println("      //chart.title(\"24hr Data\");");
            client.println("      var ctitle = chart.title();");
            client.println("      ctitle.enabled(true);");
            client.println("      ctitle.text('24hr Data');");
            client.println("      // Set text font weight.");
            client.println("      ctitle.fontWeight(600);");
            client.println("      ctitle.fontColor('#000000');");
            client.println("      ctitle.fontOpacity(1.0);");
            client.println("      // specify where to display the chart");
            client.println("      chart.container(\"container\");");
            client.println("      // draw the resulting chart");
            client.println("      chart.draw();");
            client.println("    });");
            client.println("    // create data");
            client.println("    var barData = [");
            client.println("    ");
            client.println(moistVal);

            client.println("    ];");
            client.println("    // set the gauge type");
            client.println("    var barGauge = anychart.gauges.linear();");
            client.println("    // set the data for the gauge");
            client.println("    barGauge.data(barData);");
            client.println("    // set the layout");
            client.println("    barGauge.layout('horizontal');");
            client.println("    // create a color scale");
            client.println("    var scaleBarColorScale = anychart.scales.ordinalColor().ranges(");
            client.println("    [");
            client.println("        {");
            client.println("            from: 0,");
            client.println("            to: 25,");
            client.println("            color: ['#ff4545', '#ffc175']");
            client.println("        },");
            client.println("        {");
            client.println("            from: 25,");
            client.println("            to: 50,");
            client.println("            color: ['#ffc175', '#6fff52']");
            client.println("        },");
            client.println("        {");
            client.println("            from: 50,");
            client.println("            to: 75,");
            client.println("            color: ['#6fff52', '#0062ff']");
            client.println("        },");
            client.println("        {");
            client.println("            from: 75,");
            client.println("            to: 100,");
            client.println("            color: ['#0062ff', '#0012de']");
            client.println("        }");
            client.println("    ]");
            client.println("    );");
            client.println("    anychart.onDocumentReady(function () {");
            client.println("    var scaleBar = barGauge.scaleBar(100);");
            client.println("    // set the height and offset of the Scale Bar (both as percentages of the gauge height)");
            client.println("    scaleBar.width('75%');");
            client.println("    scaleBar.offset('31.5%');");
            client.println("    // use the color scale (defined earlier) as the color scale of the Scale Bar");
            client.println("    scaleBar.colorScale(scaleBarColorScale);");
            client.println("    // add a marker pointer");
            client.println("    var marker = barGauge.marker(0);");
            client.println("    // set the color of the marker");
            client.println("    marker.fill('#000000', 0.9);");
            client.println("    // set the width of the marker");
            client.println("    marker.width(40);");
            client.println("    // set the offset of the pointer as a percentage of the gauge width");
            client.println("    marker.offset('31.5%');");
            client.println("    // set the marker type");
            client.println("    marker.type('triangle-up');");
            client.println("    // set the zIndex of the marker");
            client.println("    marker.zIndex(10);");
            client.println("    // configure the scale");
            client.println("    var scale = barGauge.scale();");
            client.println("    scale.minimum(0);");
            client.println("    scale.maximum(100);");
            client.println("    scale.ticks().interval(10);");
            client.println("    // configure the axis");
            client.println("    var axis = barGauge.axis();");
            client.println("    axis.minorTicks(true)");
            client.println("    axis.minorTicks().stroke('#cecece');");
            client.println("    axis.width('1%');");
            client.println("    axis.offset('29.5%');");
            client.println("    axis.orientation('top');");
            client.println("    // format axis labels");
            client.println("    axis.labels().format('{%value}%');");
            client.println("    // set paddings");
            client.println("    barGauge.padding([0, 50]);");
            client.println("    var title = barGauge.title();");
            client.println("    title.enabled(true);");
            client.println("    title.text(\"Soil Moisture\");");
            client.println("    // Set text font weight.");
            client.println("    title.fontWeight(600);");
            client.println("    title.fontColor('#000000');");
            client.println("    title.fontOpacity(1.0);");
            client.println("    // set the container id");
            client.println("    barGauge.container('barcontainer');");
            client.println("    // initiate drawing the gauge");
            client.println("    barGauge.draw();");
            client.println("    });");
            client.println("  </script>");
            client.println("</div>");



            // for each timer set
            for(int i=0; i<NT; i++)
            {
              if(pumpTrigger[i].set)
              {
                client.print("<div class=\"page rounded\" style=\"margin: 10px 0; font-size: 1.5em; font-weight: 700; background-color: ");
                if(pumpTrigger[i].enabled)
                  client.print("#adf7ff");
                else
                  client.print("#ff8c8c");
                client.print(" ;");
                if(triggerMasterDisable)
                  client.print("opacity: .5;");
                client.print("background-image: url('http://placehold.it/75x75&number=");
                client.print(i);
                client.print("'); background-repeat: no-repeat; background-attachment: fixed; background-position: left;");
                client.print("\" ");
                client.print(" id=\"timeTriggerNoEdit");
                client.print(i);
                client.print("\" ");
                client.print(">");
                client.print("<form name=\"enableDeleteTrigger");
                client.print(i);
                client.print("\" style=\"clear: both; \" action=\"javascript:void(0);\">");

                client.print(" <input type=\"hidden\" name=\"triggerIndex\" value=\"");
                client.print(i);
                client.print("\">");

                client.print("Pump timer set for ");
                int hr;
                char ampm[5] = "";
                if(pumpTrigger[i].hour > 12)
                {
                  hr = pumpTrigger[i].hour - 12;
                  strcpy(ampm,"pm");
                }
                else
                {
                  hr = pumpTrigger[i].hour;
                  strcpy(ampm,"am");
                }
                client.print(hr);
                client.print(":");
                client.print(pumpTrigger[i].minute);
                client.print(ampm);
                client.print("<br/>water time is ");
                client.print(pumpTrigger[i].duration);
                client.print(" seconds");
                if(triggerFlag)
                {
                client.print("<br/>Currently watering!");
                }
                
                client.print("<br/>");

                if(pumpTrigger[i].enabled)
                {
                  client.print("ENABLED <br/>");
                  client.print("<input type=\"submit\" name=\"triggerAction\" value=\"Disable\" onclick=\"triggerActionClicked");
                client.print(i);
                client.print("='Disable'\" style=\"width: 145px\" />");
                }else{
                  client.print("DISABLED <br/>");
                  client.print("<input type=\"submit\" name=\"triggerAction\" value=\"Enable\" onclick=\"triggerActionClicked");
                client.print(i);
                client.print("='Enable'\" style=\"width: 145px\" />");
                }
                //if(numTriggersSet > 0 && i == numTriggersSet-1)
                //{
                  client.print("&nbsp;&nbsp;<input type=\"submit\" name=\"triggerAction2\" value=\"Delete\" onclick=\"triggerActionClicked");
                client.print(i);
                client.print("='Delete'\" style=\"width: 145px\" />");
                //}
                client.print("</form>");

                client.println("</div>");

              }
            }
            // end for each timer



            client.print("<div class=\"page rounded\" style=\"margin: 10px 0; font-size: 1.5em; font-weight: 400; background-color: white;\">");
            client.print("<form name=\"updateTrigger\" style=\"clear: both;\" action=\"javascript:void(0);\">");
            client.print("<b>Set new trigger time</b> <br/>");
            client.print("Time of day: <input type=\"text\" name=\"hour\" size=\"5\" /> : ");
            client.print("<input type=\"text\" name=\"minute\" size=\"5\" /></br>");
            client.print("Water duration: <input type=\"text\" name=\"duration\" size=\"5\" /> seconds </br>");
            client.print("<input type=\"submit\" style=\"width: 300px\" />");
            client.print("");
            client.print("</form>");
            client.println("</div>");

            client.print("<div class=\"page rounded\" style=\"margin: 10px 0; font-size: 1.5em; font-weight: 400; background-color: white;\">");
            client.print("<form name=\"oneShotTrigger\" style=\"clear: both;\" action=\"javascript:void(0);\">");
            client.print("<b>One time water trigger</b> <br/>");
            client.print("Water duration: <input type=\"text\" name=\"oneshot\" size=\"5\" /> seconds </br>");
            client.print("<input type=\"submit\" style=\"width: 300px\" />");
            client.print("");
            client.print("</form>");
            client.println("</div>");

            client.print("<div class=\"page rounded\" style=\"margin: 10px 0; font-size: 1.5em; font-weight: 700; background-color: ");
            if(triggerMasterDisable)
              client.print("#e03d47");
            else
              client.print("#90f096");
            client.print(";\">");
            client.print("<form name=\"masterDisableTriggers\" action=\"javascript:void(0);\">");
            client.print("Water System is ");
            if(triggerMasterDisable)
            {
              client.print("DISABLED <br/>"); 
              client.print(" <input type=\"hidden\" name=\"masterDisable\" value=\"0\">");
              client.print("<input type=\"submit\" value=\"Enable Watering System\" style=\"width: 300px\" />");
            }else{
              client.print("ENABLED <br/>");
              client.print(" <input type=\"hidden\" name=\"masterDisable\" value=\"1\">");
              client.print("<input type=\"submit\" value=\"Disable Watering System\" style=\"width: 300px\" />");
            }
            client.print("</form>");
            client.println("</div>");

            client.println("<br/><br/>");

            // process trigger disable & delete form
            for(int i=0; i<NT; i++)
            {
              if(pumpTrigger[i].set)
              {
                client.print("<script>");
                client.print("var triggerActionClicked");
                client.print(i);
                client.print(";");
                // use document.form[\"form-name\"] to reference the form
                client.print("const triggerDisable");
                client.print(i);
                client.print(" = document.forms[\"enableDeleteTrigger");
                client.print(i);
                client.print("\"];");
                // bind the onsubmit property to a function to do some logic
                client.print("triggerDisable");
                client.print(i);
                client.print(".onsubmit = function(e) {");
                  // access the desired input through the var we setup
                client.print("  let triggerFormIndex = enableDeleteTrigger");
                client.print(i);
                client.print(".triggerIndex.value;");
                client.print("  let triggerFormAction = triggerActionClicked");
                client.print(i);
                client.print(";");
                //client.println("  let triggerFormAction2 = enableDeleteTrigger.triggerAction2.value;");
                //client.println("  let triggerFormActionSubmit = \"\";");
                //client.println("  if(triggerFormAction == \"\")");
                //client.println("  { triggerFormActionSubmit = triggerFormAction2 }");
                //client.println("  else");
                //client.println("  { triggerFormActionSubmit = triggerFormAction }");
                //client.println("  ");
                //client.println("  ");
                client.print("  e.preventDefault();");
                client.print("window.location.href = './?triggeraction='+triggerFormAction+'&triggerindex='+triggerFormIndex+'&';");
                client.print(" }");
                client.print("</script>");
              }
            }

            // process trigger time change form
            client.println("<script>");
            // use document.form[\"form-name\"] to reference the form
            client.println("const updateForm = document.forms[\"updateTrigger\"];");
            // bind the onsubmit property to a function to do some logic
            client.println("updateForm.onsubmit = function(e) {");
              // access the desired input through the var we setup
            client.println("  let hourSelection = updateTrigger.hour.value;");
            client.println("  let minuteSelection = updateTrigger.minute.value;");
            client.println("  let durationSelection = updateTrigger.duration.value;");
            client.println("  e.preventDefault();");
            client.println("window.location.href = './?hour='+hourSelection+'&minute='+minuteSelection+'&duration='+durationSelection+'&';");
            client.println(" }");
            client.println("</script>");

            // process one shot trigger form
            client.println("<script>");
            // use document.form[\"form-name\"] to reference the form
            client.println("const oneShotForm = document.forms[\"oneShotTrigger\"];");
            // bind the onsubmit property to a function to do some logic
            client.println("oneShotForm.onsubmit = function(e) {");
              // access the desired input through the var we setup
            client.println("  let durationSelection = oneShotTrigger.oneshot.value;");
            client.println("  e.preventDefault();");
            client.println("window.location.href = './?oneshot='+durationSelection+'&';");
            client.println(" }");
            client.println("</script>");

            // process master disable form
            client.println("<script>");
            // use document.form[\"form-name\"] to reference the form
            client.println("const masterDisable = document.forms[\"masterDisableTriggers\"];");
            // bind the onsubmit property to a function to do some logic
            client.println("masterDisable.onsubmit = function(e) {");
              // access the desired input through the var we setup
            client.println("  let masterFormDisable = masterDisableTriggers.masterDisable.value;");
            client.println("  e.preventDefault();");
            client.println("window.location.href = './?masterdisable='+masterFormDisable+'&';");
            client.println(" }");
            client.println("</script>");

            client.println("</body>");
            client.println("</html>");
            
            // The HTTP response ends with another blank line
            client.println();
            // Break out of the while loop
            break;
          } else { // if you got a newline, then clear currentLine
            currentLine = "";
          }
        } else if (c != '\r') {  // if you got anything else but a carriage return character,
          currentLine += c;      // add it to the end of the currentLine
        }
      }
    }
    // Clear the header variable
    header = "";
    // Close the connection
    client.stop();
    Serial.println("Client disconnected.");
    Serial.println("");
  }
  if(second != 0)
  {
    readFlag = 0;
  }
}

int sensorRead()
{
  // update time
    timeClient.update();
    hour = timeClient.getHours();
    minute = timeClient.getMinutes();
    //second = timeClient.getSeconds(); 

    // read data
    sensorValue = analogRead(sensorPin);
    float scaled = (float)((float)sensorValue - (float)minScale)/ (float)((float)maxScale - (float)minScale);
    moistVal = (1-scaled)*100;

    readFlag = 1;

    Serial.print(hour);
    Serial.print(":");
    Serial.print(minute);
    Serial.print(":");
    Serial.print(second);
    Serial.print(" >> Moisture: ");
    Serial.println(moistVal);
    

    if((minute % 15)==0)
    {
      //shift arr
      for(int i=95; i>0; i--)
      {
        historicVals[i] = historicVals[i-1];
      }

      historicVals[0] = moistVal;
      return 1;
    }
    return 0;
}

int pumpController()
{
  timeClient.update();  // refresh the timeClient
  second = timeClient.getSeconds(); // update seconds var

  /*
  check if the pump trigger needs to be turned OFF first
  */
  if(triggerFlag==1)
  {
    hour = timeClient.getHours();     // update hours var
    minute = timeClient.getMinutes(); // update minutes var

    // get current time of day offset in seconds
    //long int currentSecondsOffset = (hour*60*60) + (minute*60) + second;
    // get trigger time of day seconds offset
    //long int triggerSecondsOffset = (pumpTrigger[triggerIdx].hour*60*60) + (pumpTrigger[triggerIdx].minute*60) + pumpTrigger[triggerIdx].duration;

    if ((triggerTimer[triggerIdx] + (pumpTrigger[triggerIdx].duration*1000) - millis()) % 1000 == 0)
    {
      Serial.print("timer set time (ms): ");
      Serial.println(millis());
      Serial.print("trigger off time offset (ms): ");
      Serial.println(triggerTimer[triggerIdx] + (pumpTrigger[triggerIdx].duration*1000));
    }

    if (millis() >= triggerTimer[triggerIdx] + (pumpTrigger[triggerIdx].duration*1000))
    {
      digitalWrite(pumpTriggerPin, 0);
      triggerFlag=0;
      sensorRead();
      return 1;
    }
  }

  if(oneShotFlag)
  {
    hour = timeClient.getHours();     // update hours var
    minute = timeClient.getMinutes(); // update minutes var

    if ((oneShotTimer + (pumpTrigger[triggerIdx].duration*1000) - millis()) % 1000 == 0)
    {
      Serial.print("one-shot timer set time (ms): ");
      Serial.println(millis());
      Serial.print("trigger off time offset (ms): ");
      Serial.println(oneShotTimer + (oneShotTrigger*1000));
    }

    if (millis() >= oneShotTimer + (oneShotTrigger*1000))
    {
      digitalWrite(pumpTriggerPin, 0);
      oneShotFlag=0;
      oneShotTrigger=0;
      oneShotTimer=0;
      sensorRead();
      return 1;
    }
  }

  /*
  else check if we are hitting a trigger ON point
  */
  if (second==0 && triggerFlag==0 && triggerCheckFlag==0 && !triggerMasterDisable) 
  {
    hour = timeClient.getHours();
    minute = timeClient.getMinutes();

    //Serial.print(". . NTP time: ");
    //Serial.print(hour);
    //Serial.print(":");
    //Serial.print(minute);
    //Serial.println();
    
    for (int i=0; i<NT; i++)
    {
      if (hour == pumpTrigger[i].hour && minute == pumpTrigger[i].minute && !triggerFlag && pumpTrigger[i].enabled)
      {
        triggerTimer[i] = millis();

        // to start we wont thread this, later on we will..  just a blocking delay so the webserver and  sensor reads will be delayed in this case.
        digitalWrite(pumpTriggerPin, 1);

        /* 
        old way, blocking timer for trigger ON/OFF
        this is now non-blocking and controlled using the triggerFlag var
        //delay(pumpTrigger[i].duration * 1000);
        //digitalWrite(pumpTriggerPin, 0);
        */

        triggerIdx = i;
        triggerFlag = 1;
        sensorRead();
        return 1;
      }
    }
    triggerCheckFlag = 1;
  }
  else if(second != 0)
  {
    triggerCheckFlag = 0;
  }

  if(oneShotTrigger && processRequest)
  {
    oneShotTimer = millis();

    // to start we wont thread this, later on we will..  just a blocking delay so the webserver and  sensor reads will be delayed in this case.
    digitalWrite(pumpTriggerPin, 1);
    oneShotFlag = 1;
  }
  return 0;
}

void triggerSort()
{
  pumpData xferData[NT];
  uint8_t sz=0;
  memcpy(&xferData, &pumpTrigger, sizeof(pumpTrigger));
  memset(&pumpTrigger, 0, sizeof(pumpTrigger));
  for(int i=0; i<NT; i++)
  {
    if(xferData[i].set)
    { 
      memcpy(&pumpTrigger[sz++], &xferData[i], sizeof(pumpData));
    }
  }
  numTriggersSet = sz;
  triggerSortByTime();
  //triggerSortByEnabled();
}

void triggerSortByTime()
{
  pumpData tmptrigger;
  for(int i=0; i< numTriggersSet; i++)
  {
    for (int j=0; j<numTriggersSet-1; j++)
    {
      if((pumpTrigger[j+1].hour < pumpTrigger[j].hour) || ((pumpTrigger[j+1].hour == pumpTrigger[j].hour) && (pumpTrigger[j+1].minute < pumpTrigger[j].minute)))
      {
        memcpy(&tmptrigger, &pumpTrigger[j], sizeof(pumpData));
        memcpy(&pumpTrigger[j], &pumpTrigger[j+1], sizeof(pumpData));
        memcpy(&pumpTrigger[j+1], &tmptrigger, sizeof(pumpData));
      }
    }
  }
}

void triggerSortByEnabled()
{
  pumpData tmptrigger;
  for(int i=0; i< numTriggersSet; i++)
  {
    for (int j=0; j<numTriggersSet-1; j++)
    {
      if(pumpTrigger[j+1].enabled && !pumpTrigger[j].enabled)
      {
        memcpy(&tmptrigger, &pumpTrigger[j], sizeof(pumpData));
        memcpy(&pumpTrigger[j], &pumpTrigger[j+1], sizeof(pumpData));
        memcpy(&pumpTrigger[j+1], &tmptrigger, sizeof(pumpData));
      }
    }
  }
}

int eepromWrite()
{
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, triggerMasterDisable);
  EEPROM.put(triggerArrayOffset, pumpTrigger);
  EEPROM.put(triggerArrayOffset + sizeof(pumpTrigger), historicVals);
  Serial.println("Data saved to EEPROM successfully");
  EEPROM.commit();
  EEPROM.end();
  return 1;
}

int eepromRead()
{
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, triggerMasterDisable);
  EEPROM.get(triggerArrayOffset, pumpTrigger);
  EEPROM.get(triggerArrayOffset + sizeof(pumpTrigger), historicVals);
  Serial.println("Data loaded from EEPROM successfully");
  EEPROM.end();
  return 1;
}

int eepromClear()
{
  EEPROM.begin(EEPROM_SIZE);
  uint8_t clear[EEPROM_SIZE];
  memset(clear, 0, EEPROM_SIZE);
  EEPROM.put(0, clear);
  EEPROM.commit();
  EEPROM.end();
}