#include <Arduino.h>
#include <M5Unified.h>
#include "WiFi.h"
#include "AsyncUDP.h"
#include <string>
#include <EEPROM.h>
#include <ESP32Ping.h>
#include <vector>

// global enums
enum ButtonCommand {SELECT, NEXT, PREVIOUS};
enum LocalRemoteMode {LOCAL_MODE, REMOTE_MODE};

// Display variables
int displayMode = 0;  // 0: voltage, ping, 1: SSID
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

// EEPROM variables
const uint16_t magicValue = 0xbeef;
const int maxEpromStringLen = 32;
LocalRemoteMode configuredMode;
char configuredSSID[maxEpromStringLen];
char configuredSSIDPwd[maxEpromStringLen];

struct EepromConfig {
  uint16_t magic;
  int version;
  LocalRemoteMode configuredMode;
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
    displayPing(pingTargetNum);
  }
  else
  {
    M5.Lcd.println("No Wifi");
  }
}

void secondsUpdate()
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
        if (LOCAL_MODE == configuredMode)
          pingTargetNum = 1;
        else
          pingTargetNum = 0;
      }

      doPing(pingTargetNum);
    }
  }
}

class WifiInitSm
{
public:
  enum WifiStateName {SCAN, CONNECTING, UDP, WIFI_DONE};  // SSID choice state names

  WifiInitSm()
  {
    currentState = SCAN;
    nextState = SCAN;
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
      case SCAN:
        if (findConfiguredNetwork())
        {
          Serial.print("connecting to network: ");
          Serial.print(configuredSSID);
          Serial.print(" with passwd: ");
          Serial.println(configuredSSIDPwd);
          WiFi.begin(configuredSSID, configuredSSIDPwd);
          nextState = CONNECTING;
        }
        else
        {
          M5.Lcd.setCursor(0, 0, 2);
          M5.Lcd.println("M5-Long:\n  exit SCAN");
          M5.update();
          delay(2000);
          if (M5.BtnA.isPressed())
          {
            Serial.println("CANCELLING WIFI from inside ssidSm"); 
            M5.Lcd.fillScreen(BLACK);
            M5.Lcd.setCursor(0, 0, 2);
            M5.Lcd.println("CANCELLING WIFI"); 
            return true; 
          }
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

            // save received msg into statusMsg
            int i;
            uint8_t *msg_p = packet.data();
            for(i=0; i<packet.length(); i++)
            {
              statusMsg[i] = static_cast<char>(*msg_p++);
            }
            statusMsg[i] = '\0';
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
  String stateNamesArray[4] = {"SCAN", "CONNECTING", "UDP", "WIFI_DONE"};

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
  String stateNamesArray[3] = {"TIMING", "TIMEOUT"};

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
    if (LOCAL_MODE == configuredMode)
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
        M5.Lcd.printf("Password: %s\n", configuredSSIDPwd);
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
              if (configuredMode == LOCAL_MODE)
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

class FobSuperSm
{
public:
  enum SuperStateName {WIFI_INIT, SYS_STATUS, SHUTDOWN, LOCAL_REMOTE, SSID, PASSWD, FACTORY}; // Superstate state names
  String stateNamesArray[7] = {"WIFI_INIT", "SYS_STATUS", "SHUTDOWN", "LOCAL_REMOTE", "SSID", "PASSWD", "FACTORY"};

  FobSuperSm()
  {
    currentState = WIFI_INIT;
    nextState = WIFI_INIT;
  }

  void tick()
  {
    bool rv;
    currentState = nextState;
    switch (currentState)
    {
      case WIFI_INIT:
        rv = wifiInitSm.tick();
        if (rv)
        {
          nextState = SYS_STATUS;
          Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case SYS_STATUS:
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
          Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case SHUTDOWN:
        shutdownSm.tick();  // run the cancel timer
        break;
      case LOCAL_REMOTE:
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0, 2);
        M5.Lcd.printf("Mode: %s\n", configuredMode?"Remote":"Local");
        M5.Lcd.println("M5: change");
        M5.Lcd.println("B: next; PWR: prev");
        break;
      case SSID:
        ssidSm.tick();
        break;
      case PASSWD:
        passwdSm.tick();
        break;
      case FACTORY:
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0, 2);
        M5.Lcd.println("M5: FACTORY RESET");
        M5.Lcd.println("B: next; PWR: prev");
        break;
      default:
        currentState = SYS_STATUS;
    }
  }

  void buttonPress(ButtonCommand buttonCommand)
  {
    bool rv;
    Serial.printf("FobSuperSm button press %d in state %s\n"
                  , buttonCommand, stateNamesArray[currentState].c_str());
    switch (currentState)
    {
      case WIFI_INIT:
        rv = wifiInitSm.buttonPress(buttonCommand);
        if (rv)
        {
          nextState = SYS_STATUS;
          Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case SYS_STATUS:
        rv = statusButton(buttonCommand);
        Serial.printf("button push in FobSuperSm returned %d\n", rv);
        if (rv)   // change super-state
        {
          if (buttonCommand == NEXT)
          {
            nextState = SSID;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
          else if (buttonCommand == PREVIOUS)
          {
            nextState = FACTORY;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
        }
        break;
      case SHUTDOWN:
        rv = shutdownSm.buttonPress();
        if (rv)
        {
          nextState = SYS_STATUS;
          Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
        }
        break;
      case SSID:
        rv = ssidSm.buttonPress(buttonCommand);
        if (rv)
        {
          if (buttonCommand == NEXT)
          {
            nextState = PASSWD;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
          else if (buttonCommand == PREVIOUS)
          {
            nextState = SYS_STATUS;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
        }
        break;
      case PASSWD:
        rv = passwdSm.buttonPress(buttonCommand);
        if (rv)
        {
          if (NEXT == buttonCommand)
          {
            nextState = LOCAL_REMOTE;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
          else if (PREVIOUS == buttonCommand)
          {
            nextState = SSID;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
        }
        break;
      case LOCAL_REMOTE:
        rv = modeButton(buttonCommand);  // does not return from SELECT
        if (rv)
        {
          if (NEXT == buttonCommand)
          {
            nextState = FACTORY;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
          else if (PREVIOUS == buttonCommand)
          {
            nextState = PASSWD;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
        }
        break;
      case FACTORY:
        rv = factoryButton(buttonCommand);  // does not return from SELECT
        if (rv)
        {
          if (NEXT == buttonCommand)
          {
            nextState = SYS_STATUS;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
          else if (PREVIOUS == buttonCommand)
          {
            nextState = LOCAL_REMOTE;
            Serial.printf("FobSuperSm next state: %s\n", stateNamesArray[nextState].c_str());
          }
        }
        break;
      default:
        nextState = SYS_STATUS;
    }
  }

  void skipWifiInit()
  {
    nextState = SYS_STATUS;
  }

private:
  SuperStateName currentState;
  SuperStateName nextState;

  bool statusButton(ButtonCommand buttonCommand);

  WifiInitSm wifiInitSm = WifiInitSm();
  ShutdownSm shutdownSm = ShutdownSm();
  SsidSm ssidSm = SsidSm();
  PasswdSm passwdSm = PasswdSm();

  bool modeButton(ButtonCommand buttonCommand)
  {
    Serial.printf("modeButton button press %d\n", buttonCommand);
    if (SELECT == buttonCommand)
    {
      Serial.println("mode change button press");
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(0, 0, 2);
      M5.Lcd.println("MODE CHANGE");
      delay(2000);
      if (eepromConfig.configuredMode == LOCAL_MODE)
      {
        eepromConfig.configuredMode = REMOTE_MODE;
      }
      else
      {
        eepromConfig.configuredMode = LOCAL_MODE;
      }
      writeEepromConfig();  // does not return
    }
    return true;
  }

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
      eepromConfig.version = 1;
      eepromConfig.configuredMode = LOCAL_MODE;
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
FobSuperSm::statusButton(ButtonCommand buttonCommand)
{
  Serial.printf("statusButton button press %d\n", buttonCommand);
  if (SELECT == buttonCommand)
  {
    Serial.println("Toggle power button press");
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0, 2);
    M5.Lcd.println("TOGGLING POWER");
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

FobSuperSm fobSuperSm = FobSuperSm();

void printEeprom()
{
  Serial.printf("0x%x %d %d\n", eepromConfig.magic, eepromConfig.version, eepromConfig.configuredMode);
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

  // initialize time
  int64_t now = esp_timer_get_time();
  nextSecondTime = now + 1000000;  // time to increment the seconds counter

  // Set WiFi to station mode and disconnect from an AP if it was previously connected.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

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

  if (eepromConfig.magic != magicValue)
  {
    Serial.println("\nEEPROM not initialized!");
    M5.Display.println("EEPROM invalid\nFactory Reset needed");
    delay(30000);
    fobSuperSm.skipWifiInit();
    return;
  }

  configuredMode = eepromConfig.configuredMode;
  if (LOCAL_MODE == configuredMode)
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
    secondsUpdate();
    fobSuperSm.tick();
  }
}
