#include <Arduino.h>
#include <M5Unified.h>
#include "WiFi.h"
#include "AsyncUDP.h"
#include <string>
#include <EEPROM.h>
#include <ESP32Ping.h>
#include <vector>

// Battery Monitor mode GPIOs & voltage constants
#define ADC_PIN 36
#define STARLINK_POWER_PIN 26

#define ADC_10V 957
#define ADC_15V 1519
#define ADC_15_10V (ADC_15V - ADC_10V)
#define DELTA_V_REF 5.0

// battery voltage variables
const int avgArraySize = 10;
const double lowVLimit = 12.5;
const double shutdownVLimit = 5.0;
double batteryVolts;
int avgArrayIndex = 0;
double batteryVoltsArray[avgArraySize];  // array of last n voltage measurements used for trailing average

// power control variables
bool powerEnableStatus;  // currrent power-enable state

// global enums
enum ButtonCommand {SELECT, NEXT, PREVIOUS};
enum DeviceMode {LOCAL_FOB_MODE, REMOTE_FOB_MODE, BATTERY_MON_MODE};
String DeviceModeText[3] = {String("Local FOB"), String("Remote FOB"), String("Battery Monitor")};
DeviceMode configuredMode;

// Display variables
char statusMsg[50];

// Time-related variables
int64_t secondsSinceStart = 0;
int64_t nextSecondTime;
const int initialPoweroffTime = 120;
const int longPoweroffTime = 300;
int poweroffTimer = initialPoweroffTime;
const int cancelDelaySec = 60;
int cancelTimer = cancelDelaySec;
const int pingPeriod = 4;
int lastHour = 0;
bool midnightOff = false;

// RTC time variables
m5::rtc_time_t RTC_TimeStruct;
m5::rtc_date_t RTC_DateStruct;

// EEPROM variables
#define EEPROM_VERSION 3
const uint16_t magicValue = 0xbeef;
const int maxEpromStringLen = 32;
char configuredSSID[maxEpromStringLen];
char configuredSSIDPwd[maxEpromStringLen];

struct EepromConfig {
  uint16_t magic;
  int version;
  DeviceMode configuredMode;
  bool midnightOff;
  char configuredLocalSsid[maxEpromStringLen];
  char localPasswd[maxEpromStringLen];
  char configuredRemoteSsid[maxEpromStringLen];
  char remotePasswd[maxEpromStringLen];
} eepromConfig;

// Network variables
bool wifiSetupComplete = false;
const int udpPort = 6970;
std::vector<String> ssidNames;

IPAddress linkyM5IP(192, 168, 8, 10);
IPAddress linkyIP(192, 168, 100, 1);
IPAddress linkyRouterIP(192, 168, 8, 1);
IPAddress rvRouterIP(192, 168, 12, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);   // optional
IPAddress secondaryDNS(8, 8, 4, 4); // optional

struct PingTarget {
  String displayHostname;
  bool useIP;  // ping by IP if true, by FQN if false
  IPAddress pingIP;
  String fqn;
  bool pinged;  // Have sent a ping to this host
  bool pingOK;  // result of ping
};
PingTarget rvRouter = {.displayHostname="rvRouter", .useIP=true, .pingIP=rvRouterIP, .pinged=false, .pingOK=false};
PingTarget linkyRouter = {.displayHostname="linkyRouter", .useIP=true, .pingIP=linkyRouterIP, .pinged=false, .pingOK=false};
PingTarget linkyM5 = {.displayHostname="linkyM5stick", .useIP=true, .pingIP=linkyM5IP, .pinged=false, .pingOK=false};
PingTarget linky = {.displayHostname="linky", .useIP=true, .pingIP=linkyIP, .pinged=false, .pingOK=false};
PingTarget dns = {.displayHostname="DNS server", .useIP=true, .pingIP=primaryDNS, .pinged=false, .pingOK=false};
PingTarget google = {.displayHostname="google", .useIP=false, .fqn="www.google.com", .pinged=false, .pingOK=false};

// Poor design :-( Order of ping targets in array is important, coupled to skipping rvRouter in local mode
PingTarget* pingTargetArray[] = {&rvRouter, &linkyRouter, &linkyM5, &linky, &dns, &google};
int pingTargetArrayLen = sizeof(pingTargetArray) / sizeof(&linkyRouter);
int pingTargetNum = 0;

AsyncUDP udp;

// EEPROM functions
void writeEepromConfig()
{
  // write variables to EEPROM
  uint8_t * eepromConfig_p = (uint8_t*)&eepromConfig;
  for (int i=0; i<sizeof(EepromConfig); i++)
  {
    EEPROM.write(i, *(eepromConfig_p+i));  // write the EEPROM
  }
  EEPROM.commit();

  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(0, 0, 2);
  M5.Display.println("REBOOTING");
  delay(2000);
  ESP.restart();
}

// Networking functions
int scanSsids()
{
  Serial.println("Scan start");
  ssidNames.clear();
  WiFi.disconnect();

  // WiFi.scanNetworks will return the number of networks found.
  int n = WiFi.scanNetworks();
  Serial.println("Scan done");
  if (n == 0)
    Serial.println("no networks found");
  else if (n < 0)
    Serial.println("Error scanning");
  else
  {
    Serial.print(n);
    Serial.println(" networks found");
    Serial.println("Nr | SSID");
    for (int i = 0; i < n; ++i)
    {
      String SSID = WiFi.SSID(i);
      ssidNames.push_back(SSID);

      // Print SSID for each network found
      Serial.printf("%2d | %-32.32s\n", i, SSID.c_str());
    }
    // Delete the scan result to free memory
    WiFi.scanDelete();
  }
  return n;
}

void sequencePings()
{
  // Ping hosts and record live-ness
  // Note: Ping has to come before UDP send/receive or ESP32 crashes
  if (wifiSetupComplete)
  {
    // every few sec, pick the next host and ping it
    if (!(secondsSinceStart%pingPeriod))
    {
      if (++pingTargetNum == pingTargetArrayLen)
      {
        // Skip pinging rvRouter if local mode
        // Poor design: coupled to order of ping target array
        if (LOCAL_FOB_MODE == configuredMode)
          pingTargetNum = 1;
        else
          pingTargetNum = 0;
      }

      doPing(pingTargetNum);
    }
  }
}

void doPing(int pingTargetId)
{
  PingTarget* pingTarget_p = pingTargetArray[pingTargetId];
  if (pingTarget_p->useIP)
  {
    pingTarget_p->pingOK = Ping.ping(pingTarget_p->pingIP, 1);
  }
  else
  {
    pingTarget_p->pingOK = Ping.ping(pingTarget_p->fqn.c_str(), 1);
  }
  pingTarget_p->pinged = true;
}

// Battery and power control functions
double adc2Volts(int adcReading)
{
  double volts = 10.0 + (DELTA_V_REF / ADC_15_10V) * (adcReading - ADC_10V);
  // Serial.println(volts);
  return volts;
}

double readVolts()
{
  double v = 0.0;
  // read voltage & compute display value
  int adcValue = analogRead(ADC_PIN);
  double currentBatteryVolts = adc2Volts(adcValue);
  batteryVoltsArray[avgArrayIndex++] = currentBatteryVolts;
  avgArrayIndex %= avgArraySize;
  // compute trailing average of battery voltage measurements
  for (int i=0; i<avgArraySize; i++)
    v += batteryVoltsArray[i];
  v /= avgArraySize;
  // Serial.printf("current voltage: %0.2f averge: %0.2f\n", currentBatteryVolts, v);
  return v;
}

void setPowerEnable(bool enable)
{
  if (enable != powerEnableStatus)
  {
    if (true == enable)
    {
      digitalWrite(STARLINK_POWER_PIN, HIGH);
      powerEnableStatus = true;
    }
    else
    {
      digitalWrite(STARLINK_POWER_PIN, LOW);
      powerEnableStatus = false;
    }
    Serial.printf("setPowerEnable(): enable: %d, powerEnableStatus %d\n", enable, powerEnableStatus);
  }
}

// @brief If it's midnight, turn power off if midnightOff is true
void midnightCheck()
{
  int currentHour = RTC_TimeStruct.hours;
  if (currentHour != lastHour)
  {
    Serial.printf("New hour: %d\n", currentHour);

    if (0 == currentHour)
    {
      Serial.println("Midnight");
      if (midnightOff)
        setPowerEnable(false);
    }
    
    lastHour = currentHour;
  }
}

// status display functions
void displayPing(int pingTargetId)
{
  if (!wifiSetupComplete)
    return;

  PingTarget* pingTarget_p = pingTargetArray[pingTargetId];

  // only print ping result if we've actually pinged the host
  if (pingTarget_p->pinged)
  {
    M5.Display.printf("%s: %s\n", pingTarget_p->displayHostname.c_str(), pingTarget_p->pingOK?"OK":"FAIL");
  }
}

void displayStatus()
{
  M5.Display.setCursor(0, 0, 2);
  M5.Display.print(statusMsg);

  // display ping status of current ping target
  M5.Display.setCursor(0, 80, 2);
  if (wifiSetupComplete)
  {
    if (BATTERY_MON_MODE != configuredMode)
    {
      displayPing(pingTargetNum);
    }
  }
  else
  {
    M5.Lcd.println("No Wifi");
  }
}

class WifiInitSm
{
public:
  enum WifiStateName {IDLE, SCAN, CONNECTING, UDP, WIFI_DONE};  // SSID choice state names
  String stateNamesArray[5] = {"WiFi IDLE", "SCAN", "CONNECTING", "UDP", "WIFI_DONE"};

  WifiInitSm()
  {
    currentState = IDLE;
    nextState = IDLE;
  }

  //! @return true: advance super SM, false: no state change
  bool tick()
  {
    currentState = nextState;
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);
    M5.Lcd.println(stateNamesArray[currentState]);

    switch(currentState)
    {
      case IDLE:
        // Set WiFi to station mode and disconnect from an AP if it was previously connected.
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);
        nextState = SCAN;
        break;
      case SCAN:
        if (findConfiguredNetwork())
        {
          Serial.print("connecting to network: ");
          Serial.print(configuredSSID);
          Serial.print(" with passwd: ");
          Serial.println(configuredSSIDPwd);
          if (BATTERY_MON_MODE == configuredMode)
          {
            // set static IP for battery monitor M5Stick
            if (!WiFi.config(linkyM5IP, linkyRouterIP, subnet, primaryDNS, secondaryDNS)) 
            {
              Serial.println("STA Failed to configure");
              delay(1000);
              return(true);  //Advance to status state with no wifi
            }
          }
          WiFi.begin(configuredSSID, configuredSSIDPwd);  // connect to network
          nextState = CONNECTING;
        }
        else
        {
          // couldn't find configured SSID
          M5.Lcd.setCursor(0, 0, 2);
          M5.Lcd.println("M5: EXIT SCAN");
          resetNextTickTime();  // allow loop to cycle before rescanning
        }
        break;
      case CONNECTING:
        // polled until wifi connects
        if (WiFi.status() != WL_CONNECTED)
        {
          break;
        }
        Serial.print("connected to wifi with IP: ");
        Serial.println(WiFi.localIP());
        nextState = UDP;
        break;
      case UDP:
        // set up lambda to call on receipt of a udp packet
        Serial.println("Setting up UDP server");
        if (udp.listen(udpPort))
        {
          Serial.print("UDP listening on port ");
          Serial.println(udpPort);

          udp.onPacket([](AsyncUDPPacket packet)
          {
            // this is a bloody lambda!
            bool verbose = false;
            if (verbose)
            {
              // print each received msg for debug
              Serial.print("Received packet from ");
              Serial.print(packet.remoteIP());
              Serial.print(", Length: ");
              Serial.print(packet.length());
              Serial.print(", Data: ");
              Serial.write(packet.data(), packet.length());
              Serial.println();
            }

            if (BATTERY_MON_MODE == configuredMode)
            {
              // test what command came in from FOB and react
              if (!strncmp("status", (char*)packet.data(), 6))
              {
                Serial.println("Got status request");
                udp.writeTo((uint8_t*)statusMsg, strnlen(statusMsg, sizeof(statusMsg)), packet.remoteIP(), udpPort);
              }
              else if (!strncmp("toggle", (char*)packet.data(), 6))
              {
                Serial.println("Got toggle request");
                setPowerEnable(!powerEnableStatus);
              }
            }
            else
            {
              // save msg received from Battery M5Stick into statusMsg for display
              int i;
              uint8_t *msg_p = packet.data();
              for(i=0; i<packet.length(); i++)
              {
                statusMsg[i] = static_cast<char>(*msg_p++);
              }
              statusMsg[i] = '\0';
            }
          });
        }
        nextState = WIFI_DONE;
        break;
      case WIFI_DONE:
        M5.Lcd.setTextColor(TFT_WHITE,TFT_BLACK);
        wifiSetupComplete = true;
        Serial.println("Configured WiFi");
        return true;  // advance superSm
      default:
        nextState = SCAN; // should never get here
    }
    return false;
  }

  //! @return true: advance super SM
  bool buttonPress(ButtonCommand buttonCommand)
  {
    if (SELECT == buttonCommand)
    {
      Serial.println("CANCELLING WIFI"); 
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(0, 0, 2);
      M5.Lcd.println("CANCELLING WIFI"); 
      delay(1000);
      return true; 
    }
    return false;
  } 
private:
  WifiStateName currentState;
  WifiStateName nextState;

  bool findConfiguredNetwork()
  {
    int n = scanSsids();
    if (n < 1)
      return false;  // scan failure

    for(auto i=ssidNames.begin(); i!=ssidNames.end(); ++i)
    {
      // check if the configured network was found in the scan
      if (!strncmp((*i).c_str(), configuredSSID, maxEpromStringLen))
      {
        Serial.printf("Found configured SSID %s\n", configuredSSID);
        return true;
      }
    }
    Serial.println("Didn't find configured network");

    return false;
  }
};

class ShutdownSm
{
public:
  enum ShutdownStateName {TIMING, TIMEOUT};  // SSID choice state names

  ShutdownSm()
  {
    currentState = TIMING;
    nextState = TIMING;
  }

  //! @return true: advance super SM, false: no state change
  bool buttonPress()
  {
    switch(currentState)
    {
      case TIMING:
        Serial.println("ERROR - ShutdownSm unexpected button");
        break;
      case TIMEOUT:
        Serial.println("shutdown cancelled");
        reset();
        nextState = TIMING;
        return true;
    }
    return false;
  }

  // do power-off timer processing
  //! @return true: advance super SM, false: no state change
  bool tick()
  {
    currentState = nextState;
    switch(currentState)
    {
      case TIMING:
        if (poweroffTimer > 0)
        {
          --poweroffTimer;
        }
        else
        {
          nextState = TIMEOUT;
          return true;
        }
        break;
      case TIMEOUT:
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0, 2);
        M5.Lcd.println("Shutting Down!!!\nPress M5 button\nto cancel");
        // power off after cancelTimer timeout with no ButtonA pushed
        if (cancelTimer > 0)
        {
          --cancelTimer;
        }
        else
        {
          Serial.println("Shutdown after timeout");
          M5.Power.powerOff();
          // never reached
        }
        break;
    }
    return false;
  }

  void reset()
  {
    poweroffTimer = longPoweroffTime;
    cancelTimer = cancelDelaySec;
  }

private:
  ShutdownStateName currentState;
  ShutdownStateName nextState;
  String stateNamesArray[2] = {"TIMING", "TIMEOUT"};

  const int initialPoweroffTime = 120;
  int poweroffTimer = initialPoweroffTime;
  const int longPoweroffTime = 300;
  const int cancelDelaySec = 60;
  int cancelTimer = cancelDelaySec;
};

class SsidSm
{
public:
  enum SsidStateName {IDLE, SELECT_SSID};  // SSID choice state names
  String stateNamesArray[2] = {"IDLE", "SELECT_SSID"};  // SSID choice state names

  SsidSm()
  {
    currentState = IDLE;
    nextState = IDLE;
    ssidMenuIndex = 0;
  }

  void tick()
  {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);

    currentState = nextState;
    switch(currentState)
    {
      case IDLE:
        M5.Lcd.printf("SSID: %s\n",configuredSSID);
        M5.Lcd.println("M5: Configure\nB: Next; PWR: Prev");
        break;
      case SELECT_SSID:
        M5.Lcd.printf("M5: select:\n%s\n",ssidMenu.at(ssidMenuIndex).c_str());
        M5.Lcd.println("B: next SSID\nPWR: prev SSID");
        break;
      default:
        nextState = IDLE;
    }
    return;
  }

  //! @return true: advance super SM, false: no super SM state change
  bool buttonPress(ButtonCommand buttonCommand)
  {
    switch(currentState)
    {
      case IDLE:
        if (SELECT == buttonCommand)
        {
          M5.Lcd.fillScreen(BLACK);
          M5.Lcd.setCursor(0, 0, 2);
          M5.Lcd.println("CONFIGURE SSID\nWAIT...");

          int n = scanSsids();  // scan for SSIDs
          if (n < 1)
          {
            M5.Lcd.println("SCAN FAILURE");
            delay(2000);
            return true;
          }

          ssidMenu.clear();

          for(auto i=ssidNames.begin(); i!=ssidNames.end(); ++i)
          {
            ssidMenu.push_back(*i);
            Serial.printf("Pushed %s onto menu\n", (*i).c_str());
          }
          ssidMenu.push_back("<Exit>");
          ssidMenuIndex = 0;

          nextState = SELECT_SSID;
          Serial.printf("%d entries in SSID menu\n", ssidMenu.size());
          Serial.printf("ssidSm next State: %s\n", stateNamesArray[nextState]);
          return false;   // SSID change selected, stay in this sm
        }
        else if (NEXT == buttonCommand || PREVIOUS == buttonCommand)
          return true;  // exit Sm on NEXT or PREVIOUS button
        else            // unknown button
          Serial.printf("ERROR - unknown button in SsidSm - IDLE\n");
          return true;  // exit SSID, advance super sm
        break;
      case SELECT_SSID:
        if (SELECT == buttonCommand)
        {
          M5.Lcd.fillScreen(BLACK);
          M5.Lcd.setCursor(0, 0, 2);
          M5.Lcd.printf("SELECTED SSID:\n%s", ssidMenu.at(ssidMenuIndex).c_str());
          delay(1000);
          if (ssidMenuIndex == ssidMenu.size()-1)
          {
            nextState = IDLE;
            return true;  // <exit> was selected
          }
          else
          {
            saveSelectedSsid(ssidMenu.at(ssidMenuIndex));  // does not return
          }
        }
        else if (NEXT == buttonCommand)
        {
          Serial.println("NEXT in SsidSm - SELECT_SSID");
          ++ssidMenuIndex;
          if (ssidMenuIndex == ssidMenu.size())
            ssidMenuIndex = 0;
        }
        else if (PREVIOUS == buttonCommand)
        {
          Serial.println("NEXT in SsidSm - SELECT_SSID");
          --ssidMenuIndex;
          if (ssidMenuIndex < 0)
            ssidMenuIndex = ssidMenu.size()-1;
        }
        else
        {
          Serial.printf("ERROR - unknown button in SsidSm - SELECT_SSID\n");
          nextState = IDLE;
          return true;
        }
        break;
      default:
        nextState = IDLE;
        return true;
    }
    return false;
  }

private:
  SsidStateName currentState;
  SsidStateName nextState;
  std::vector<String> ssidMenu;
  int ssidMenuIndex;

  void saveSelectedSsid(String ssid)
  {
    Serial.printf("Writing SSID %s to EEPROM for mode %d\n", ssid.c_str(), configuredMode);
    if (LOCAL_FOB_MODE == configuredMode)
      strncpy(eepromConfig.configuredLocalSsid, ssid.c_str(), maxEpromStringLen);
    else
      strncpy(eepromConfig.configuredRemoteSsid, ssid.c_str(), maxEpromStringLen);
    
    writeEepromConfig();  // does not return
  }
};

class PasswdSm
{
public:
  enum PasswdStateName {IDLE, CHAR_CLASS, SELECT_CHAR};  // SSID choice state names
  String stateNamesArray[3] = {"IDLE", "CHAR_CLASS", "SELECT_CHAR"};
  enum CharClassName {LOWER, UPPER, NUMBER, SPECIAL, SAVE, BACKSPACE, CHAR_CLASS_END};  // add items before CHAR_CLASS_END
  const String classMenu[CHAR_CLASS_END] = {"lower", "UPPER", "number", "special", "<Save>", "<DEL>"};
  static const int numSpecials = 32;
  const char specialMenuSymbols[numSpecials] = 
    {
    '!', '"', '#', '$', '%', '&', '\'', '('
    , ')', '*', '+', ',', '-', '.', '/', ':'
    , ';', '<', '=', '>', '?', '@', '[', '\\'
    , ']', '^', '_', '`', '{', '|', '}', '~' 
    };

  PasswdSm()
  {
    currentState = IDLE;
    nextState = IDLE;
    charClass = LOWER;
  }

  void tick()
  {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);

    currentState = nextState;
    switch(currentState)
    {
      case IDLE:
        M5.Lcd.printf("Password:\n%s\n", configuredSSIDPwd);
        M5.Lcd.println("M5: Change\nB: next; PWR: prev");
        break;
      case CHAR_CLASS:
        M5.Lcd.printf("Password: %s\n", newPasswd);
        M5.Lcd.printf("M5: select: %s\n",classMenu[classMenuIndex].c_str());
        M5.Lcd.println("B: next type\nPWR: prev type");
        break;
      case SELECT_CHAR:
        switch (charClass)
        {
          case LOWER:
            selectableChar = 'a' + alphaIndex;
            break;
          case UPPER:
            selectableChar = 'A' + alphaIndex;
            break;
          case NUMBER:
            selectableChar = '0' + numberIndex;
            break;
          case SPECIAL:
            selectableChar = specialMenuSymbols[specialIndex];
            break;
          default:
            alphaIndex = 0;
            numberIndex = 0;
            specialIndex = 0;
        }
        M5.Lcd.printf("Password: %s\n", newPasswd);
        M5.Lcd.printf("M5: select char: %c\n",selectableChar);
        M5.Lcd.println("B: next char\nPWR: prev char");
        break;
      default:
        nextState = IDLE;
    }
    return;
  }

  //! @return true: advance super SM, false: no state change
  bool buttonPress(ButtonCommand buttonCommand)
  {
    switch (currentState)
    {
      case IDLE:
        if (SELECT == buttonCommand)
        {
          M5.Lcd.fillScreen(BLACK);
          M5.Lcd.setCursor(0, 0, 2);
          M5.Lcd.println("CHANGE PASSWORD");
          delay(1000);

          nextState = CHAR_CLASS;
          Serial.printf("PasswdSm next State: %s\n", stateNamesArray[nextState]);

          classMenuIndex = 0;
          newPasswdIndex = 0;
          alphaIndex = 0;
          numberIndex = 0;
          specialIndex = 0;

          for (int i=0; i<maxEpromStringLen; i++)
            newPasswd[i] = 0;
        }
        else
        {
          return true;
        }
        break;
      case CHAR_CLASS:
        if (SELECT == buttonCommand)
        {
          M5.Lcd.fillScreen(BLACK);
          M5.Lcd.setCursor(0, 0, 2);
          switch (charClass)
          {
            case LOWER:
            case UPPER:
            case NUMBER:
            case SPECIAL:
              M5.Lcd.println("SELECT CHARACTER");
              delay(1000);
              nextState = SELECT_CHAR;
              Serial.printf("PasswdSm next State: %s\n", stateNamesArray[nextState]);
              break;
            case SAVE:
              M5.Lcd.println("SAVE - REBOOTING");
              if (configuredMode == LOCAL_FOB_MODE || configuredMode == BATTERY_MON_MODE)
                strncpy(eepromConfig.localPasswd, newPasswd, maxEpromStringLen);
              else
                strncpy(eepromConfig.remotePasswd, newPasswd, maxEpromStringLen);
              writeEepromConfig();  // Does not return
              break;  // never reached
            case BACKSPACE:
              M5.Lcd.println("BACKSPACE");
              --newPasswdIndex;
              newPasswd[newPasswdIndex] = 0;
              break;
          }
        }
        else if (NEXT == buttonCommand)
        {
          ++classMenuIndex;
          if (classMenuIndex == CHAR_CLASS_END)
            classMenuIndex = 0;
          charClass = (CharClassName)classMenuIndex;
          Serial.printf("classMenuIndex: %d\n", classMenuIndex);
        }
        else if (PREVIOUS == buttonCommand)
        {
          --classMenuIndex;
          if (classMenuIndex == -1)
            classMenuIndex = CHAR_CLASS_END - 1;
          charClass = (CharClassName)classMenuIndex;
          Serial.printf("classMenuIndex: %d\n", classMenuIndex);
        }
        break;
      case SELECT_CHAR:
        if (SELECT == buttonCommand)
        {
          newPasswd[newPasswdIndex++] = selectableChar;

          M5.Lcd.fillScreen(BLACK);
          M5.Lcd.setCursor(0, 0, 2);
          M5.Lcd.printf("SELECTED %c\n", selectableChar);
          Serial.printf("SELECTED character %c\n", selectableChar);
          delay(1000);

          nextState = CHAR_CLASS;
          Serial.printf("PasswdSm next State: %s\n", stateNamesArray[nextState]);
        }
        else if (NEXT == buttonCommand)
        {
          switch(charClass)
          {
            case LOWER:
            case UPPER:
              ++alphaIndex;
              if (alphaIndex == 26)
                alphaIndex = 0;
              break;
            case NUMBER:
              ++numberIndex;
              if (numberIndex == 10)
                numberIndex = 0;
              break;
            case SPECIAL:
              ++specialIndex;
              if (specialIndex == numSpecials)
                specialIndex = 0;
              break;
          }
        }
        else if (PREVIOUS == buttonCommand)
        {
          switch(charClass)
          {
            case LOWER:
            case UPPER:
              --alphaIndex;
              if (alphaIndex == -1)
                alphaIndex = 25;
              break;
            case NUMBER:
              --numberIndex;
              if (numberIndex == -1)
                numberIndex = 9;
              break;
            case SPECIAL:
              --specialIndex;
              if (specialIndex == -1)
                specialIndex = numSpecials-1;
              break;
          }
        }
        break;
      default:
        nextState = IDLE;
    }
    return false;
  }

private:
  PasswdStateName currentState;
  PasswdStateName nextState;
  CharClassName charClass;
  char selectableChar;
  char newPasswd[maxEpromStringLen];
  int newPasswdIndex;
  int classMenuIndex;
  const int maxAlphaIndex = 25;
  int alphaIndex;
  const int maxNumberIndex = 9;
  int numberIndex;
  int specialIndex;

};

class SetModeSm
{
public:
  enum SetModeStateName {IDLE, SET_LOCAL_FOB, SET_REMOTE_FOB, SET_BATT_MON};  // SSID choice state names
  String stateNamesArray[4] = {"IDLE", "SET_LOCAL_FOB", "SET_REMOTE_FOB", "SET_BATT_MON"};  // Device mode

  SetModeSm()
  {
    currentState = IDLE;
    nextState = IDLE;
  }

  // display mode selection prompts
  bool tick()
  {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);

    currentState = nextState;
    switch(currentState)
    {
      case IDLE:
        M5.Lcd.println("Current mode:");
        M5.Lcd.println(DeviceModeText[configuredMode]);
        M5.Lcd.println("M5: CHANGE\nB: Next; PWR: Prev");
        break;
      case SET_LOCAL_FOB:
        M5.Lcd.println("M5: set mode:");
        M5.Lcd.println(DeviceModeText[LOCAL_FOB_MODE]);
        M5.Lcd.println("B: Next; PWR: Prev");
        break;
      case SET_REMOTE_FOB:
        M5.Lcd.println("M5: set mode:");
        M5.Lcd.println(DeviceModeText[REMOTE_FOB_MODE]);
        M5.Lcd.println("B: Next; PWR: Prev");
        break;
      case SET_BATT_MON:
        // for some unknown reason, printf(DeviceModeText[BATT....]) printed corrupt data but println is ok
        M5.Lcd.println("M5: set mode:");
        M5.Lcd.println(DeviceModeText[BATTERY_MON_MODE]);
        M5.Lcd.println("B: Next; PWR: Prev");
        break;
      default:
        currentState = IDLE;
        nextState = IDLE;
    }
    return false;
  }

  //! @return true: advance super SM, false: no state change
  // no return from Select
  bool buttonPress(ButtonCommand buttonCommand)
  {
    switch(currentState)
    {
      case IDLE:
        if (SELECT == buttonCommand)
        {
          nextState = SET_LOCAL_FOB;

          M5.Lcd.fillScreen(BLACK);
          M5.Lcd.setCursor(0, 0, 2);
          M5.Lcd.println("CHANGE MODE");
          delay(2000);
          return false;
        }
        else
          return true;  // NEXT or PREV change major states
        break;
      case SET_LOCAL_FOB:
        if (SELECT == buttonCommand)
        {
          Serial.println("SET LOCAL FOB mode");
          eepromConfig.configuredMode = LOCAL_FOB_MODE;
          writeEepromConfig(); // Does not return
        }
        else if (NEXT == buttonCommand)
          nextState = SET_REMOTE_FOB;
        else
          nextState = IDLE;  // PREVIOUS
        break;
      case SET_REMOTE_FOB:
        if (SELECT == buttonCommand)
        {
          Serial.println("SET REMOTE FOB mode");
          eepromConfig.configuredMode = REMOTE_FOB_MODE;
          writeEepromConfig(); // Does not return
        }
        else if (NEXT == buttonCommand)
        {
          nextState = SET_BATT_MON;
        }
        else
          nextState = SET_LOCAL_FOB;  // PREVIOUS
        break;
      case SET_BATT_MON:
        if (SELECT == buttonCommand)
        {
          Serial.println("SET BATT MON mode");
          eepromConfig.configuredMode = BATTERY_MON_MODE;
          writeEepromConfig(); // Does not return
        }
        else if (NEXT == buttonCommand)
          nextState = IDLE;
        else
          nextState = SET_REMOTE_FOB;  // PREVIOUS
        break;
    }
    return false;
  }

private:
  SetModeStateName currentState;
  SetModeStateName nextState;
};

class SetDateTimeSm
{
public:
  enum SetDateTimeStateName {IDLE, SET_YEAR, SET_MONTH, SET_DAY, SET_HOUR, SET_MINUTE};  // Date/Time state names
  String stateNamesArray[6] = {"IDLE", "SET_YEAR", "SET_MONTH", "SET_DAY", "SET_HOUR", "SET_MINUTE"};

  SetDateTimeSm()
  {
    currentState = IDLE;
    nextState = IDLE;
  }

  // display mode selection prompts
  bool tick()
  {
    M5.Lcd.fillScreen(BLACK);

    M5.Rtc.getTime(&RTC_TimeStruct);
    M5.Rtc.getDate(&RTC_DateStruct);

    M5.Lcd.setCursor(0, 1, 1);
    M5.Lcd.printf("%04d-%02d-%02d\n", RTC_DateStruct.year,
      RTC_DateStruct.month, RTC_DateStruct.date);
    M5.Lcd.printf("%02d:%02d:%02d\n", RTC_TimeStruct.hours,
      RTC_TimeStruct.minutes, RTC_TimeStruct.seconds);

    M5.Lcd.setCursor(0, 35);    // place cursor for prompt
    currentState = nextState;
    switch(currentState)
    {
      case IDLE:
        M5.Lcd.println("M5: CHANGE DATE/TIME\nB: Next, PWR: Prev");
        break;
      case SET_YEAR:
        M5.Lcd.println("M5: Accept YEAR\nB: Next YEAR\nPWR: Prev YEAR");
        break;
      case SET_MONTH:
        M5.Lcd.println("M5: Accept MONTH\nB: Next MONTH\nPWR: Prev MONTH");
        break;
      case SET_DAY:
        M5.Lcd.println("M5: Accept DAY-OF-MO\nB: Next DAY-OF-MO\nPWR: Prev DAY-OF-MO");
        break;
      case SET_HOUR:
        M5.Lcd.println("M5: Accept HOUR\nB: Next HOUR\nPWR: Prev HOUR");
        break;
      case SET_MINUTE:
        M5.Lcd.println("M5: Accept MINUTE\nB: Next MINUTE\nPWR: Prev MINUTE");
        break;
      default:
        currentState = IDLE;
        nextState = IDLE;
    }
    return false;
  }

  //! @return true: advance super SM, false: no state change
  bool buttonPress(ButtonCommand buttonCommand)
  {
    Serial.printf("SetDateTimeSm in state %s got button %d\n", stateNamesArray[currentState], buttonCommand);
    switch(currentState)
    {
      case IDLE:
        if (SELECT == buttonCommand)
          nextState = SET_YEAR;
        else
          return true;  // advance superSm
        break;
      case SET_YEAR:
        if (SELECT == buttonCommand)
          nextState = SET_MONTH;
        else if (NEXT == buttonCommand)
          RTC_DateStruct.year++;
        else if (PREVIOUS == buttonCommand)
          RTC_DateStruct.year--;
        
        if (RTC_DateStruct.year > 2030)
          RTC_DateStruct.year = 2030;
        if (RTC_DateStruct.year < 2024)
          RTC_DateStruct.year = 2024;
        M5.Rtc.setDate(&RTC_DateStruct);
        break;
      case SET_MONTH:
        if (SELECT == buttonCommand)
          nextState = SET_DAY;
        else if (NEXT == buttonCommand)
          RTC_DateStruct.month++;
        else if (PREVIOUS == buttonCommand)
          RTC_DateStruct.month--;
        
        if (RTC_DateStruct.month > 12)
          RTC_DateStruct.month = 1;
        if (RTC_DateStruct.month < 1)
          RTC_DateStruct.month = 12;
        M5.Rtc.setDate(&RTC_DateStruct);
        break;
      case SET_DAY:
        if (SELECT == buttonCommand)
          nextState = SET_HOUR;
        else if (NEXT == buttonCommand)
          RTC_DateStruct.date++;
        else if (PREVIOUS == buttonCommand)
          RTC_DateStruct.date--;
        
        if (RTC_DateStruct.date > 31)
          RTC_DateStruct.date = 1;
        if (RTC_DateStruct.date < 1)
          RTC_DateStruct.date = 31;
        M5.Rtc.setDate(&RTC_DateStruct);
        break;
      case SET_HOUR:
        if (SELECT == buttonCommand)
          nextState = SET_MINUTE;
        else if (NEXT == buttonCommand)
          RTC_TimeStruct.hours++;
        else if (PREVIOUS == buttonCommand)
          RTC_TimeStruct.hours--;
        
        if (RTC_TimeStruct.hours > 23)
          RTC_TimeStruct.hours = 0;
        if (RTC_TimeStruct.hours < 0)
          RTC_TimeStruct.hours = 23;
        M5.Rtc.setTime(&RTC_TimeStruct);
        break;
      case SET_MINUTE:
        if (SELECT == buttonCommand)
          nextState = IDLE;
        else if (NEXT == buttonCommand)
          RTC_TimeStruct.minutes++;
        else if (PREVIOUS == buttonCommand)
          RTC_TimeStruct.minutes--;
        
        if (RTC_TimeStruct.minutes > 59)
          RTC_TimeStruct.minutes = 0;
        if (RTC_TimeStruct.minutes < 0)
          RTC_TimeStruct.minutes = 59;
        M5.Rtc.setTime(&RTC_TimeStruct);
        break;
      default:
        nextState = IDLE;
    }
    return false;
  }

private:
  SetDateTimeStateName currentState;
  SetDateTimeStateName nextState;
};

class SuperSm
{
public:
  enum SuperStateName {WIFI_INIT, FOB_STATUS, BATT_MON_STATUS, SHUTDOWN, SET_MODE, SSID, PASSWD, MIDNIGHT_OFF, DATE_TIME, FACTORY}; // Superstate state names
  String stateNamesArray[10] = {"WIFI_INIT", "FOB_STATUS", "BATT_MON_STATUS", "SHUTDOWN", "SET_MODE", "SSID", "PASSWD", "MIDNIGHT_OFF", "DATE_TIME", "FACTORY"};

  SuperSm()
  {
    currentState = WIFI_INIT;
    nextState = WIFI_INIT;
  }

  void tick()
  {
    bool rv;
    String soc;

    currentState = nextState;
    switch (currentState)
    {
      case WIFI_INIT:
        rv = wifiInitSm.tick();
        if (rv)
        {
          // Done with wifi init, move to status state for FOB or Battery Monitor
          if (BATTERY_MON_MODE == configuredMode)
            nextState = BATT_MON_STATUS;
          else
            nextState = FOB_STATUS;
          Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case FOB_STATUS:
        sequencePings();
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0, 2);

        if (wifiSetupComplete)
        {
          // Serial.println("Writing status request to linkyM5");
          udp.writeTo((uint8_t*)"status", 6, linkyM5IP, udpPort);
        }

        displayStatus();
        
        rv = shutdownSm.tick();  // run the shutdown timer in status state
        if (rv)
        {
          nextState = SHUTDOWN;
          Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case BATT_MON_STATUS:
        // Get the time-of-day from the real-time clock for use by various called functions.
        M5.Rtc.getTime(&RTC_TimeStruct);
        M5.Rtc.getDate(&RTC_DateStruct);

        midnightCheck();  // is it midnight?

        // read battery voltage and react if low
        batteryVolts = readVolts();

        if (batteryVolts < shutdownVLimit)
        {
          // power off when master switch turned off
          const int cancelDelaySec = 5;
          M5.Lcd.fillScreen(BLACK);
          M5.Lcd.setCursor(0, 0, 2);
          M5.Lcd.println("Shutting\nDown");
          delay(2000);

          M5.Power.powerOff();
          // never reached
        }
        else if (batteryVolts < lowVLimit)
        {
          // shut down linky when battery gets low
          setPowerEnable(false);
        }

        // compute battery status msg
        if (batteryVolts >= 13.5) soc = "100%";
        else if (batteryVolts >=13.4) soc = "99%";
        else if (batteryVolts >=13.3) soc = "90-99%";
        else if (batteryVolts >=13.2) soc = "70-90%";
        else if (batteryVolts >=13.1) soc = "40-70%";
        else if (batteryVolts >=13.0) soc = "30-40%";
        else if (batteryVolts >=12.9) soc = "20-30%";
        else if (batteryVolts >=12.8) soc = "10-20%";
        else if (batteryVolts >=10.8) soc = "1-10%";
        else soc = "Unknown";

        sprintf(statusMsg, "%s, %0.2fV\nSoC %s\0", powerEnableStatus?"ON":"OFF", batteryVolts, soc.c_str());
        displayStatus();
        break;
      case SHUTDOWN:
        shutdownSm.tick();  // run the cancel timer
        break;
      case SET_MODE:
        setModeSm.tick();
        break;
      case SSID:
        ssidSm.tick();
        break;
      case PASSWD:
        passwdSm.tick();
        break;
      case MIDNIGHT_OFF:
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0, 2);
        M5.Lcd.print("Midnight: ");
        M5.Lcd.println(midnightOff?"Turn OFF":"Stay ON");
        M5.Lcd.println("M5: Change");
        M5.Lcd.println("B: next; PWR: prev");
        break;
      case DATE_TIME:
        setDateTimeSm.tick();
        break;
      case FACTORY:
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0, 2);
        M5.Lcd.println("M5: FACTORY\nRESET");
        M5.Lcd.println("B: next; PWR: prev");
        break;
      default:
        currentState = FOB_STATUS;
    }
  }

  void buttonPress(ButtonCommand buttonCommand)
  {
    bool rv;
    Serial.printf("SuperSm button press %d in state %s\n"
                  , buttonCommand, stateNamesArray[currentState].c_str());
    switch (currentState)
    {
      case WIFI_INIT:
        rv = wifiInitSm.buttonPress(buttonCommand);
        if (rv)
        {
          nextState = FOB_STATUS;
          Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case FOB_STATUS:
      case BATT_MON_STATUS:
        rv = statusButton(buttonCommand);
        Serial.printf("button push in SuperSm returned %d\n", rv);
        if (rv)   // change super-state
        {
          if (buttonCommand == NEXT)
          {
            nextState = SSID;
            Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
          else if (buttonCommand == PREVIOUS)
          {
            nextState = FACTORY;
            Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
        }
        break;
      case SHUTDOWN:
        rv = shutdownSm.buttonPress();
        if (rv)
        {
          nextState = FOB_STATUS;
          Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case SSID:
        rv = ssidSm.buttonPress(buttonCommand);
        if (rv)
        {
          if (buttonCommand == NEXT)
            nextState = PASSWD;
          else if (buttonCommand == PREVIOUS)
          {
            if (BATTERY_MON_MODE == configuredMode)
              nextState = BATT_MON_STATUS;
            else
              nextState = FOB_STATUS;
          }
          Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case PASSWD:
        rv = passwdSm.buttonPress(buttonCommand);
        if (rv)
        {
          if (NEXT == buttonCommand)
            nextState = SET_MODE;
          else if (PREVIOUS == buttonCommand)
            nextState = SSID;
          Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case SET_MODE:
        rv = setModeSm.buttonPress(buttonCommand);
        if (rv)
        {
          if (NEXT == buttonCommand)
          {
            if (BATTERY_MON_MODE == configuredMode)
              nextState = MIDNIGHT_OFF;
            else
              nextState = FACTORY;
          }
          else if (PREVIOUS == buttonCommand)
            nextState = PASSWD;
          Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case MIDNIGHT_OFF:
        if (SELECT == buttonCommand)
        {
          if (midnightOff)
            midnightOff = 0;
          else
            midnightOff = 1;
          eepromConfig.midnightOff = midnightOff;

          M5.Lcd.fillScreen(BLACK);
          M5.Lcd.setCursor(0, 0, 2);
          M5.Lcd.println("Midnight:");
          M5.Lcd.println(midnightOff?"Turn OFF":"Stay ON");
          delay(2000);

          writeEepromConfig();  // does not return
        }
        else if (NEXT == buttonCommand)
          nextState = DATE_TIME;
        else
          nextState = SET_MODE;
        Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        break;
      case DATE_TIME:
        rv = setDateTimeSm.buttonPress(buttonCommand);
        if (rv)
        {
          if (NEXT == buttonCommand)
            nextState = FACTORY;
          else if (PREVIOUS == buttonCommand)
            nextState = MIDNIGHT_OFF;
          Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case FACTORY:
        rv = factoryButton(buttonCommand);  // does not return from SELECT
        if (rv)
        {
          if (NEXT == buttonCommand)
          {
            if (BATTERY_MON_MODE == configuredMode)
              nextState = BATT_MON_STATUS;
            else
              nextState = FOB_STATUS;
          }
          else if (PREVIOUS == buttonCommand)
          {
            if (BATTERY_MON_MODE == configuredMode)
              nextState = DATE_TIME;
            else
              nextState = SET_MODE;
          }
          Serial.printf("SuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      default:
        nextState = FOB_STATUS;
    }
  }

  void skipWifiInit()
  {
    nextState = FOB_STATUS;
  }

private:
  SuperStateName currentState;
  SuperStateName nextState;

  bool statusButton(ButtonCommand buttonCommand);

  WifiInitSm wifiInitSm = WifiInitSm();
  ShutdownSm shutdownSm = ShutdownSm();
  SsidSm ssidSm = SsidSm();
  PasswdSm passwdSm = PasswdSm();
  SetModeSm setModeSm = SetModeSm();
  SetDateTimeSm setDateTimeSm = SetDateTimeSm();

  bool factoryButton(ButtonCommand buttonCommand)
  {
    Serial.printf("factoryButton button press %d\n", buttonCommand);
    if (SELECT == buttonCommand)
    {
      Serial.println("Factory reset button press");
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(0, 0, 2);
      M5.Lcd.println("FACTORY RESET");
      eepromConfig.magic = (uint16_t)0xbeef;
      eepromConfig.version = EEPROM_VERSION;
      eepromConfig.midnightOff = false;
      eepromConfig.configuredMode = LOCAL_FOB_MODE;
      strcpy(eepromConfig.configuredLocalSsid, "No SSID");
      strcpy(eepromConfig.localPasswd, "None");
      strcpy(eepromConfig.configuredRemoteSsid, "No SSID");
      strcpy(eepromConfig.remotePasswd, "None");

      writeEepromConfig();  // does not return
    }
    else if (NEXT == buttonCommand || PREVIOUS == buttonCommand)
    {
      Serial.println("Ignoring button B or PWR press");
    }
    return true;
  }

};

//! @return true: advance super SM, false: no state change
bool
SuperSm::statusButton(ButtonCommand buttonCommand)
{
  Serial.printf("statusButton button press %d\n", buttonCommand);
  if (SELECT == buttonCommand)
  {
    Serial.println("Toggle power button press");
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);
    M5.Lcd.println("TOGGLING POWER");
    if (BATTERY_MON_MODE == configuredMode)
      setPowerEnable(!powerEnableStatus);
    else
      udp.writeTo((uint8_t*)"toggle", 6, linkyM5IP, udpPort);
    delay(1000);
    return false;
  }
  else if (NEXT == buttonCommand || PREVIOUS == buttonCommand)
  {
    Serial.print("NEXT or PREVIOUS button press in state ");
    Serial.println(stateNamesArray[currentState]);
    return true;  // advance to next state
  }
  return false;   // stay in this state if not asked to move
}

SuperSm fobSuperSm = SuperSm();

void printEeprom()
{
  Serial.printf("EEPROM: 0x%x %d %d %d %d\n", eepromConfig.magic, eepromConfig.version
  , eepromConfig.midnightOff, eepromConfig.configuredMode);
  Serial.println(eepromConfig.configuredLocalSsid);
  Serial.println(eepromConfig.localPasswd);
  Serial.println(eepromConfig.configuredRemoteSsid);
  Serial.println(eepromConfig.remotePasswd);
}

void setup() {
  // initialize M5StickCPlus2
  M5.begin();
  M5.Display.setRotation(1);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_RED,TFT_BLACK);
  M5.update();

  sprintf(statusMsg, "starting");

  Serial.begin(9600);
  delay(500);
  Serial.println("\nStarting");

  // initialize time
  int64_t now = esp_timer_get_time();
  nextSecondTime = now + 1000000;  // time to increment the seconds counter

  // Initialize EEPROM
  if (!EEPROM.begin(sizeof(EepromConfig))) {  // Request storage of SIZE size(success return)
    Serial.println("\nFailed to initialize EEPROM!");
    M5.Display.println("EEPROM Fail\nFactory Reset needed");
    delay(30000);
    fobSuperSm.skipWifiInit();
    return;
  }

  // Initialize variables from EEPROM
  uint8_t * eepromConfig_p = (uint8_t*)&eepromConfig;
  for (int i=0; i<sizeof(EepromConfig); i++)
  {
    *(eepromConfig_p + i) = EEPROM.read(i);  // read the EEPROM
  }

  printEeprom();

  if (eepromConfig.magic != magicValue || eepromConfig.version != EEPROM_VERSION)
  {
    Serial.println("\nEEPROM not initialized!");
    M5.Display.println("EEPROM invalid\nFactory Reset needed");
    delay(30000);
    fobSuperSm.skipWifiInit();
    return;
  }

  configuredMode = eepromConfig.configuredMode;
  Serial.print("Starting Mode: ");
  Serial.println(DeviceModeText[configuredMode]);

  if (LOCAL_FOB_MODE == configuredMode || BATTERY_MON_MODE == configuredMode)
  {
    strncpy(configuredSSID, eepromConfig.configuredLocalSsid, maxEpromStringLen);
    strncpy(configuredSSIDPwd, eepromConfig.localPasswd, maxEpromStringLen);
  }
  else
  {
    strncpy(configuredSSID, eepromConfig.configuredRemoteSsid, maxEpromStringLen);
    strncpy(configuredSSIDPwd, eepromConfig.remotePasswd, maxEpromStringLen);
  }
  Serial.printf("configuredMode: %d SSID: %s\n", configuredMode, configuredSSID);

  // initialize battery monitor mode variables & pins
  if (BATTERY_MON_MODE == configuredMode)
  {
    midnightOff = eepromConfig.midnightOff;

    // configure battery monitor mode pins
    // G36 is battery voltage input, G26 is Starlink power enable output
    gpio_pulldown_dis(GPIO_NUM_25);
    gpio_pullup_dis(GPIO_NUM_25);
    gpio_pulldown_dis(GPIO_NUM_36);
    gpio_pullup_dis(GPIO_NUM_36);

    pinMode(STARLINK_POWER_PIN, OUTPUT);
    digitalWrite(STARLINK_POWER_PIN, HIGH);
    powerEnableStatus = true;

    // initialize array for battery voltage trailing average
    for (int i=0; i<avgArraySize; i++)
      batteryVoltsArray[i] = 13.0;  // initialize with good value that doesn't trigger shutdown

    // Get the time-of-day from the real-time clock.
    M5.Rtc.getTime(&RTC_TimeStruct);
    M5.Rtc.getDate(&RTC_DateStruct);
    lastHour = RTC_TimeStruct.hours;
  }

  
}

// resetNextTickTime updates when loop() will call superSm.tick()
// to be a second from now. It is needed because
// some operations, like scan ssids, take more than 1 second
// so SuperSm is late to return and loop doesn't get to spin
// and detect button pushes. If called, this method ensures a
// period where loop() idles to allow display and buttons
void resetNextTickTime()
{
  nextSecondTime = esp_timer_get_time() + 1000000;
}

void loop() {
  int64_t now_us;

  // Read buttons
  M5.update();

  if (M5.BtnA.wasReleased())
  {
    Serial.println("buttonM5");
    fobSuperSm.buttonPress(SELECT);
  }

  if (M5.BtnB.wasReleased())
  {
    Serial.println("buttonB");
    fobSuperSm.buttonPress(NEXT);
  }

  if (M5.BtnPWR.wasReleased())
  {
    Serial.println("buttonPWR");
    fobSuperSm.buttonPress(PREVIOUS);
  }

  // Check if it's time to increment the seconds-counter
  // and process everything that should be done each second
  now_us = esp_timer_get_time();
  if (now_us > nextSecondTime)
  {
    secondsSinceStart++;
    nextSecondTime = now_us + 1000000;
    // Serial.printf("%lld toggleTime: %d state %d\n", secondsSinceStart, nextFlowToggleTime, generateFlowPulses);
    fobSuperSm.tick();
  }
}
