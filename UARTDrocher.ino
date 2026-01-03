// NodeMCU V3 (ESP8266MOD) UART bridge for set-top box console.
// UART0 pins with swap: RX=GPIO13, TX=GPIO15 (recommended to free USB).
// USB serial remains for power only when swap is enabled.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>

// Configuration
static const uint32_t kDefaultBaud = 115200;
static const size_t kLineBufSize = 128;

// Set to 1 to swap UART0 pins to GPIO13 (RX) and GPIO15 (TX).
#define USE_UART_SWAP 1

// Wi-Fi station config for telnet bridge.
static const char *kStaSsid = ""; // SSID of the WiFi network
static const char *kStaPass = ""; // Password of the WiFi network
static const uint16_t kTelnetPort = 23; // Port for the telnet server

static WiFiServer telnetServer(kTelnetPort);
static WiFiClient telnetClient;

// Line buffer for telnet commands.
static char cmdBuf[kLineBufSize];
static size_t cmdLen = 0;

static bool bridgeMode = false;
static const uint8_t kBridgeExitChar = 0x0c;  // Ctrl+L to exit bridge mode
static bool sendCROnly = true;
static bool dropLocalLF = true;
static bool telnetIac = false;
static uint8_t telnetIacCount = 0;

static bool i2cInited = false;
static uint8_t i2cSda = 4;  // D2
static uint8_t i2cScl = 5;  // D1
static uint32_t i2cClock = 100000;
static bool i2cPassive = false;

enum I2CEventType : uint8_t {
  kI2CStart = 0,
  kI2CStop = 1,
  kI2CAddr = 2,
  kI2CData = 3,
};

struct I2CEvent {
  uint8_t type;
  uint8_t val;
  uint8_t meta;
};

static const uint8_t kI2CEventBufSize = 64;
static volatile I2CEvent i2cEvents[kI2CEventBufSize];
static volatile uint8_t i2cEventHead = 0;
static volatile uint8_t i2cEventTail = 0;

static volatile bool i2cSniffInFrame = false;
static volatile uint8_t i2cSniffBits = 0;
static volatile uint8_t i2cSniffByte = 0;
static volatile bool i2cSniffAddr = true;

static void IRAM_ATTR i2cSniffPush(uint8_t type, uint8_t val, uint8_t meta) {
  uint8_t next = static_cast<uint8_t>((i2cEventHead + 1) % kI2CEventBufSize);
  if (next == i2cEventTail) return;
  i2cEvents[i2cEventHead].type = type;
  i2cEvents[i2cEventHead].val = val;
  i2cEvents[i2cEventHead].meta = meta;
  i2cEventHead = next;
}

static void IRAM_ATTR i2cSniffOnSclRise() {
  if (!i2cPassive || !i2cSniffInFrame) return;
  uint8_t sda = digitalRead(i2cSda) ? 1 : 0;
  if (i2cSniffBits < 8) {
    i2cSniffByte = static_cast<uint8_t>((i2cSniffByte << 1) | sda);
    i2cSniffBits++;
  } else {
    uint8_t ack = sda ? 1 : 0;
    uint8_t meta = ack;
    if (i2cSniffAddr) {
      meta |= static_cast<uint8_t>((i2cSniffByte & 0x01) << 1);
      i2cSniffPush(kI2CAddr, static_cast<uint8_t>(i2cSniffByte >> 1), meta);
      i2cSniffAddr = false;
    } else {
      i2cSniffPush(kI2CData, i2cSniffByte, meta);
    }
    i2cSniffBits = 0;
    i2cSniffByte = 0;
  }
}

static void IRAM_ATTR i2cSniffOnSdaChange() {
  if (!i2cPassive) return;
  uint8_t scl = digitalRead(i2cScl) ? 1 : 0;
  uint8_t sda = digitalRead(i2cSda) ? 1 : 0;
  if (!scl) return;
  if (!sda) {
    i2cSniffInFrame = true;
    i2cSniffBits = 0;
    i2cSniffByte = 0;
    i2cSniffAddr = true;
    i2cSniffPush(kI2CStart, 0, 0);
  } else {
    i2cSniffInFrame = false;
    i2cSniffPush(kI2CStop, 0, 0);
  }
}

static bool parseUint32(const char *s, uint32_t *out) {
  if (!s || !*s) return false;
  uint32_t v = 0;
  while (*s) {
    if (*s < '0' || *s > '9') return false;
    v = (v * 10u) + static_cast<uint32_t>(*s - '0');
    ++s;
  }
  *out = v;
  return true;
}

static bool parseHexOrDec(const char *s, uint32_t *out) {
  if (!s || !*s) return false;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
    if (!*s) return false;
    uint32_t v = 0;
    while (*s) {
      char c = *s++;
      uint8_t d = 0;
      if (c >= '0' && c <= '9') d = static_cast<uint8_t>(c - '0');
      else if (c >= 'a' && c <= 'f') d = static_cast<uint8_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = static_cast<uint8_t>(c - 'A' + 10);
      else return false;
      v = (v << 4) | d;
    }
    *out = v;
    return true;
  }
  return parseUint32(s, out);
}

static void i2cEnsureInit() {
  if (i2cInited) return;
  Wire.begin(i2cSda, i2cScl);
  Wire.setClock(i2cClock);
  i2cInited = true;
}

static void i2cScan() {
  i2cEnsureInit();
  telnetClient.println(F("I2C scan:"));
  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      telnetClient.printf("  0x%02X\r\n", addr);
      ++found;
    }
    yield();
  }
  telnetClient.printf("Done, found %u device(s).\r\n", found);
}

static void i2cReadReg(uint8_t addr, uint8_t reg, uint8_t len) {
  i2cEnsureInit();
  Wire.beginTransmission(addr);
  Wire.write(reg);
  uint8_t err = Wire.endTransmission(false);
  if (err != 0) {
    telnetClient.printf("ERR I2C WRITE %u\r\n", err);
    return;
  }
  uint8_t got = Wire.requestFrom(addr, len);
  telnetClient.printf("I2C 0x%02X reg 0x%02X: ", addr, reg);
  for (uint8_t i = 0; i < got; ++i) {
    int c = Wire.read();
    if (c < 0) break;
    telnetClient.printf("%02X ", static_cast<uint8_t>(c));
  }
  telnetClient.println();
}

static void i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t *data, uint8_t len) {
  i2cEnsureInit();
  Wire.beginTransmission(addr);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; ++i) {
    Wire.write(data[i]);
  }
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    telnetClient.printf("ERR I2C WRITE %u\r\n", err);
    return;
  }
  telnetClient.println(F("OK I2C WRITE"));
}

static void i2cReadRaw(uint8_t addr, uint8_t len) {
  i2cEnsureInit();
  uint8_t got = Wire.requestFrom(addr, len);
  telnetClient.printf("I2C 0x%02X raw: ", addr);
  for (uint8_t i = 0; i < got; ++i) {
    int c = Wire.read();
    if (c < 0) break;
    telnetClient.printf("%02X ", static_cast<uint8_t>(c));
  }
  telnetClient.println();
}

static void i2cReadReg16(uint8_t addr, uint16_t reg, uint8_t len) {
  i2cEnsureInit();
  Wire.beginTransmission(addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  uint8_t err = Wire.endTransmission(false);
  if (err != 0) {
    telnetClient.printf("ERR I2C WRITE %u\r\n", err);
    return;
  }
  uint8_t got = Wire.requestFrom(addr, len);
  telnetClient.printf("I2C 0x%02X reg16 0x%04X: ", addr, reg);
  for (uint8_t i = 0; i < got; ++i) {
    int c = Wire.read();
    if (c < 0) break;
    telnetClient.printf("%02X ", static_cast<uint8_t>(c));
  }
  telnetClient.println();
}

static void i2cSetModePassive(bool passive) {
  if (i2cPassive == passive) return;
  i2cPassive = passive;
  if (i2cPassive) {
    i2cInited = false;
    pinMode(i2cSda, INPUT_PULLUP);
    pinMode(i2cScl, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(i2cScl), i2cSniffOnSclRise, RISING);
    attachInterrupt(digitalPinToInterrupt(i2cSda), i2cSniffOnSdaChange, CHANGE);
  } else {
    detachInterrupt(digitalPinToInterrupt(i2cScl));
    detachInterrupt(digitalPinToInterrupt(i2cSda));
  }
}

static void printTelnetHelp() {
  if (!telnetClient) return;
  telnetClient.println();
  telnetClient.println(F("Telnet commands:"));
  telnetClient.println(F("  /help            - this help"));
  telnetClient.println(F("  /baud <rate>     - change UART baud rate"));
  telnetClient.println(F("  /scan            - auto-detect baud (needs device output)"));
  telnetClient.println(F("  /go              - enter bridge mode"));
  telnetClient.println(F("  /cr              - send only CR for Enter (CFE safe)"));
  telnetClient.println(F("  /crlf            - send CR+LF for Enter"));
  telnetClient.println(F("  /lf              - send only LF for Enter"));
  telnetClient.println(F("  /i2c scan         - scan I2C bus"));
  telnetClient.println(F("  /i2c read <a> <r> <n>  - read n bytes from reg r"));
  telnetClient.println(F("  /i2c read16 <a> <r16> <n> - read n bytes from reg16"));
  telnetClient.println(F("  /i2c readraw <a> <n>  - read n bytes without reg"));
  telnetClient.println(F("  /i2c write <a> <r> <b..> - write bytes to reg r"));
  telnetClient.println(F("  /i2c pins <sda> <scl>  - set I2C pins (dec)"));
  telnetClient.println(F("  /i2c clock <hz>   - set I2C clock"));
  telnetClient.println(F("  /i2c mode passive|master - set I2C mode"));
  telnetClient.println(F("  /quit            - close connection"));
  telnetClient.println();
  telnetClient.println(F("Bridge mode: all bytes go to UART."));
  telnetClient.println(F("Exit bridge: Ctrl+L"));
  telnetClient.println();
}

static void handleTelnetCommand(char *line) {
  // Trim trailing CR/LF.
  size_t n = strlen(line);
  while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) {
    line[--n] = '\0';
  }
  if (n == 0) return;

  char *cmd = line;
  if (cmd[0] == '/') cmd++;

  if (strcasecmp(cmd, "help") == 0) {
    printTelnetHelp();
    return;
  }
  if (strcasecmp(cmd, "go") == 0) {
    bridgeMode = true;
    telnetClient.println(F("OK BRIDGE"));
    return;
  }
  if (strcasecmp(cmd, "quit") == 0) {
    telnetClient.println(F("BYE"));
    telnetClient.stop();
    return;
  }
  if (strncasecmp(cmd, "baud ", 5) == 0) {
    uint32_t rate = 0;
    if (parseUint32(cmd + 5, &rate)) {
      Serial.flush();
      Serial.begin(rate);
      telnetClient.printf("OK BAUD %lu\r\n", static_cast<unsigned long>(rate));
    } else {
      telnetClient.println(F("ERR BAUD"));
    }
    return;
  }
  if (strcasecmp(cmd, "scan") == 0) {
    static const uint32_t rates[] = {
        9600, 19200, 38400, 57600, 74880, 115200, 230400};
    const size_t rateCount = sizeof(rates) / sizeof(rates[0]);
    bool found = false;
    for (size_t i = 0; i < rateCount; ++i) {
      Serial.begin(rates[i]);
      const uint32_t start = millis();
      uint32_t total = 0;
      uint32_t printable = 0;
      while (millis() - start < 1500) {
        while (Serial.available() > 0) {
          int c = Serial.read();
          if (c < 0) break;
          ++total;
          if (c == '\r' || c == '\n' || c == '\t' ||
              (c >= 32 && c <= 126)) {
            ++printable;
          }
        }
        yield();
      }
      if (total >= 16 && printable * 10 >= total * 7) {
        telnetClient.printf("OK SCAN %lu\r\n",
                            static_cast<unsigned long>(rates[i]));
        found = true;
        break;
      }
    }
    if (!found) {
      telnetClient.println(F("ERR SCAN"));
    }
    return;
  }
  if (strcasecmp(cmd, "cr") == 0) {
    sendCROnly = true;
    dropLocalLF = true;
    telnetClient.println(F("OK CR"));
    return;
  }
  if (strncasecmp(cmd, "i2c ", 4) == 0) {
    char *args = cmd + 4;
    char *tok = strtok(args, " ");
    if (!tok) {
      telnetClient.println(F("ERR I2C"));
      return;
    }
    if (strcasecmp(tok, "scan") == 0) {
      i2cScan();
      return;
    }
    if (strcasecmp(tok, "read") == 0) {
      char *a = strtok(nullptr, " ");
      char *r = strtok(nullptr, " ");
      char *n = strtok(nullptr, " ");
      uint32_t addr = 0, reg = 0, len = 0;
      if (!a || !r || !n || !parseHexOrDec(a, &addr) ||
          !parseHexOrDec(r, &reg) || !parseHexOrDec(n, &len) ||
          addr > 0x7F || reg > 0xFF || len == 0 || len > 32) {
        telnetClient.println(F("ERR I2C READ"));
        return;
      }
      i2cReadReg(static_cast<uint8_t>(addr), static_cast<uint8_t>(reg),
                 static_cast<uint8_t>(len));
      return;
    }
    if (strcasecmp(tok, "readraw") == 0) {
      char *a = strtok(nullptr, " ");
      char *n = strtok(nullptr, " ");
      uint32_t addr = 0, len = 0;
      if (!a || !n || !parseHexOrDec(a, &addr) || !parseHexOrDec(n, &len) ||
          addr > 0x7F || len == 0 || len > 32) {
        telnetClient.println(F("ERR I2C READRAW"));
        return;
      }
      i2cReadRaw(static_cast<uint8_t>(addr), static_cast<uint8_t>(len));
      return;
    }
    if (strcasecmp(tok, "read16") == 0) {
      char *a = strtok(nullptr, " ");
      char *r = strtok(nullptr, " ");
      char *n = strtok(nullptr, " ");
      uint32_t addr = 0, reg = 0, len = 0;
      if (!a || !r || !n || !parseHexOrDec(a, &addr) ||
          !parseHexOrDec(r, &reg) || !parseHexOrDec(n, &len) ||
          addr > 0x7F || reg > 0xFFFF || len == 0 || len > 32) {
        telnetClient.println(F("ERR I2C READ16"));
        return;
      }
      i2cReadReg16(static_cast<uint8_t>(addr), static_cast<uint16_t>(reg),
                   static_cast<uint8_t>(len));
      return;
    }
    if (strcasecmp(tok, "write") == 0) {
      char *a = strtok(nullptr, " ");
      char *r = strtok(nullptr, " ");
      if (!a || !r) {
        telnetClient.println(F("ERR I2C WRITE"));
        return;
      }
      uint32_t addr = 0, reg = 0;
      if (!parseHexOrDec(a, &addr) || !parseHexOrDec(r, &reg) ||
          addr > 0x7F || reg > 0xFF) {
        telnetClient.println(F("ERR I2C WRITE"));
        return;
      }
      uint8_t data[32];
      uint8_t count = 0;
      char *b = nullptr;
      while ((b = strtok(nullptr, " ")) != nullptr && count < sizeof(data)) {
        uint32_t v = 0;
        if (!parseHexOrDec(b, &v) || v > 0xFF) {
          telnetClient.println(F("ERR I2C WRITE"));
          return;
        }
        data[count++] = static_cast<uint8_t>(v);
      }
      if (count == 0) {
        telnetClient.println(F("ERR I2C WRITE"));
        return;
      }
      i2cWriteReg(static_cast<uint8_t>(addr), static_cast<uint8_t>(reg), data,
                  count);
      return;
    }
    if (strcasecmp(tok, "pins") == 0) {
      char *sda = strtok(nullptr, " ");
      char *scl = strtok(nullptr, " ");
      uint32_t vsda = 0, vscl = 0;
      if (!sda || !scl || !parseUint32(sda, &vsda) ||
          !parseUint32(scl, &vscl)) {
        telnetClient.println(F("ERR I2C PINS"));
        return;
      }
      i2cSda = static_cast<uint8_t>(vsda);
      i2cScl = static_cast<uint8_t>(vscl);
      i2cInited = false;
      telnetClient.printf("OK I2C PINS %u %u\r\n", i2cSda, i2cScl);
      return;
    }
    if (strcasecmp(tok, "clock") == 0) {
      char *hz = strtok(nullptr, " ");
      uint32_t v = 0;
      if (!hz || !parseUint32(hz, &v) || v < 10000 || v > 400000) {
        telnetClient.println(F("ERR I2C CLOCK"));
        return;
      }
      i2cClock = v;
      i2cInited = false;
      telnetClient.printf("OK I2C CLOCK %lu\r\n",
                          static_cast<unsigned long>(i2cClock));
      return;
    }
    if (strcasecmp(tok, "mode") == 0) {
      char *m = strtok(nullptr, " ");
      if (!m) {
        telnetClient.println(F("ERR I2C MODE"));
        return;
      }
      if (strcasecmp(m, "passive") == 0) {
        i2cSetModePassive(true);
        telnetClient.println(F("OK I2C MODE PASSIVE"));
        return;
      }
      if (strcasecmp(m, "master") == 0) {
        i2cSetModePassive(false);
        telnetClient.println(F("OK I2C MODE MASTER"));
        return;
      }
      telnetClient.println(F("ERR I2C MODE"));
      return;
    }
    telnetClient.println(F("ERR I2C"));
    return;
  }
  if (strcasecmp(cmd, "crlf") == 0) {
    sendCROnly = false;
    dropLocalLF = false;
    telnetClient.println(F("OK CRLF"));
    return;
  }
  if (strcasecmp(cmd, "lf") == 0) {
    sendCROnly = false;
    dropLocalLF = false;
    telnetClient.println(F("OK LF"));
    return;
  }

  telnetClient.println(F("ERR UNKNOWN"));
}

static void handleTelnet() {
  if (!telnetClient || !telnetClient.connected()) {
    WiFiClient newClient = telnetServer.available();
    if (newClient) {
      if (telnetClient) {
        telnetClient.stop();
      }
      telnetClient = newClient;
      bridgeMode = false;
      cmdLen = 0;
      telnetClient.println();
      telnetClient.println(F("SML-Bridge ready."));
      telnetClient.println(F("Type /help for commands."));
    }
  }

  if (!telnetClient || !telnetClient.connected()) return;

  // UART -> telnet
  while (Serial.available() > 0 && telnetClient.connected()) {
    int c = Serial.read();
    if (c < 0) break;
    telnetClient.write(static_cast<uint8_t>(c));
  }

  // Telnet -> UART or command mode.
  while (telnetClient.available() > 0 && telnetClient.connected()) {
    int c = telnetClient.read();
    if (c < 0) break;

    // Drop telnet IAC negotiation bytes.
    if (telnetIac) {
      if (telnetIacCount > 0) {
        telnetIacCount--;
        if (telnetIacCount == 0) telnetIac = false;
      }
      continue;
    }
    if (static_cast<uint8_t>(c) == 255) {
      telnetIac = true;
      telnetIacCount = 2;
      continue;
    }

    if (!bridgeMode) {
      if (c == '\n' || c == '\r') {
        if (cmdLen < kLineBufSize) {
          cmdBuf[cmdLen++] = static_cast<char>(c);
        }
        cmdBuf[cmdLen] = '\0';
        handleTelnetCommand(cmdBuf);
        cmdLen = 0;
      } else if (cmdLen + 1 < kLineBufSize) {
        cmdBuf[cmdLen++] = static_cast<char>(c);
      } else {
        cmdLen = 0;
        telnetClient.println(F("ERR LINE TOO LONG"));
      }
      continue;
    }

    if (static_cast<uint8_t>(c) == kBridgeExitChar) {
      bridgeMode = false;
      telnetClient.println(F("OK CMD"));
      continue;
    }

    if (sendCROnly) {
      if (c == '\r') {
        Serial.write('\r');
        continue;
      }
      if (c == '\n') {
        Serial.write('\r');
        continue;
      }
    } else if (c == '\n' && dropLocalLF) {
      continue;
    }
    if (c == '\r' && !sendCROnly) {
      Serial.write('\r');
      Serial.write('\n');
      continue;
    }

    // Forward every byte immediately to allow boot interruption.
    Serial.write(static_cast<uint8_t>(c));
  }
}

void setup() {
  Serial.begin(kDefaultBaud);
#if USE_UART_SWAP
  Serial.swap();
#endif

  Serial.println();
  Serial.println(F("NodeMCU UART bridge ready."));
  Serial.printf("Default baud: %lu\r\n", static_cast<unsigned long>(kDefaultBaud));

  WiFi.mode(WIFI_STA);
  IPAddress ip(192, 168, 0, 156); // IP address of the device
  IPAddress gw(192, 168, 0, 1); // Gateway address
  IPAddress mask(255, 255, 255, 0); // Network mask
  WiFi.config(ip, gw, mask);
  WiFi.begin(kStaSsid, kStaPass);

  uint32_t waitStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - waitStart < 15000) {
    delay(200);
    yield();
  }

  telnetServer.begin();
  telnetServer.setNoDelay(true);
  IPAddress curIp = WiFi.localIP();
  Serial.printf("STA connected, IP: %s\r\n", curIp.toString().c_str());
}

void loop() {
  handleTelnet();

  if (i2cPassive && telnetClient && telnetClient.connected()) {
    while (i2cEventTail != i2cEventHead) {
      I2CEvent ev;
      ev.type = i2cEvents[i2cEventTail].type;
      ev.val = i2cEvents[i2cEventTail].val;
      ev.meta = i2cEvents[i2cEventTail].meta;
      i2cEventTail =
          static_cast<uint8_t>((i2cEventTail + 1) % kI2CEventBufSize);
      if (ev.type == kI2CStart) {
        telnetClient.println(F("I2C START"));
      } else if (ev.type == kI2CStop) {
        telnetClient.println(F("I2C STOP"));
      } else if (ev.type == kI2CAddr) {
        uint8_t rw = static_cast<uint8_t>((ev.meta >> 1) & 0x01);
        uint8_t ack = static_cast<uint8_t>(ev.meta & 0x01);
        telnetClient.printf("I2C ADDR 0x%02X %c %s\r\n", ev.val,
                            rw ? 'R' : 'W', ack ? "NACK" : "ACK");
      } else if (ev.type == kI2CData) {
        uint8_t ack = static_cast<uint8_t>(ev.meta & 0x01);
        telnetClient.printf("I2C DATA 0x%02X %s\r\n", ev.val,
                            ack ? "NACK" : "ACK");
      }
    }
  }

  yield();
}
