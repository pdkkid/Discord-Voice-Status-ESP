#include <Arduino.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#else
#include <WiFi.h>
#include <LittleFS.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#endif

#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// ================== USER DEFAULTS ==================
// If all are set, no portal is created for the user to enter these
static const char *DEFAULT_WS_URL = "";
static const char *DEFAULT_AUTH_TOKEN = "";
static const char *DEFAULT_WIFI_SSID = "";
static const char *DEFAULT_WIFI_PASS = "";
// ===================================================

// Firmware version string (bump this when you want devices to accept new versions)
static const char *FW_VERSION = "dev";

// LED pins (Active HIGH)
#if defined(ESP8266)
static const uint8_t LED_PIN = 5; // ESP8266 GPIO5
#else
static const uint8_t LED_PIN = 2; // ESP32 GPIO2
#endif

static const int FORCE_PORTAL_PIN = -1;

// WiFi retry behavior
static const uint8_t WIFI_CONNECT_TRIES = 4;
static const uint32_t WIFI_TRY_TIMEOUT_MS = 8000;

// Auth failure behavior
static const uint8_t MAX_AUTH_FAILURES = 3;
static uint8_t authFailureCount = 0;

// Config storage
static const char *CONFIG_PATH = "/config.json";

// WS reconnect pacing
static const uint32_t WS_RECONNECT_MS = 5000;
static bool wsWasConnected = false;

WebSocketsClient webSocket;

struct AppConfig
{
  String wsUrl;
  String authToken;
};
static AppConfig cfg;

struct WsParts
{
  bool secure;
  String host;
  uint16_t port;
  String path;
};

static uint32_t lastWsAttemptMs = 0;

// ---------- helpers ----------
static void setLed(bool on) { digitalWrite(LED_PIN, on ? HIGH : LOW); }

static bool ensureFS()
{
  static bool mounted = false;
  if (mounted)
    return true;
#if defined(ESP32)
  mounted = LittleFS.begin(true); // auto-format on first mount failure
#else
  mounted = LittleFS.begin();
#endif
  if (!mounted)
    Serial.println("❌ LittleFS mount failed");
  return mounted;
}

static bool loadConfig(AppConfig &out)
{
  if (!ensureFS())
    return false;
  if (!LittleFS.exists(CONFIG_PATH))
    return false;

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f)
    return false;

  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, f);
  f.close();
  if (err)
    return false;

  out.wsUrl = doc["wsUrl"] | DEFAULT_WS_URL;
  out.authToken = doc["authToken"] | DEFAULT_AUTH_TOKEN;
  return true;
}

static bool saveConfig(const AppConfig &in)
{
  if (!ensureFS())
    return false;

  StaticJsonDocument<512> doc;
  doc["wsUrl"] = in.wsUrl;
  doc["authToken"] = in.authToken;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f)
    return false;

  serializeJson(doc, f);
  f.close();
  return true;
}

static bool parseWsUrl(const String &url, WsParts &out)
{
  String u = url;
  u.trim();

  if (u.startsWith("wss://"))
  {
    out.secure = true;
    u = u.substring(6);
  }
  else if (u.startsWith("ws://"))
  {
    out.secure = false;
    u = u.substring(5);
  }
  else
    return false;

  int slash = u.indexOf('/');
  String hostPort = (slash >= 0) ? u.substring(0, slash) : u;
  out.path = (slash >= 0) ? u.substring(slash) : String("/");

  int colon = hostPort.indexOf(':');
  if (colon >= 0)
  {
    out.host = hostPort.substring(0, colon);
    long p = hostPort.substring(colon + 1).toInt();
    if (p <= 0 || p > 65535)
      return false;
    out.port = (uint16_t)p;
  }
  else
  {
    out.host = hostPort;
    out.port = out.secure ? 443 : 80;
  }

  if (out.host.length() == 0)
    return false;
  if (!out.path.startsWith("/"))
    out.path = "/" + out.path;
  return true;
}

static bool isBlank(const char *s)
{
  return (s == nullptr) || (s[0] == '\0');
}

static bool defaultsHaveWifi()
{
  return !isBlank(DEFAULT_WIFI_SSID); // allow open networks by leaving PASS empty
}

static bool defaultsHaveAppConfig()
{
  return !isBlank(DEFAULT_WS_URL) && !isBlank(DEFAULT_AUTH_TOKEN);
}

// Try connecting to a specific SSID/pass (no saving). Returns true if connected.
static bool tryConnectWifiExplicit(const char *ssid, const char *pass, uint8_t tries, uint32_t perTryTimeoutMs)
{
  WiFi.mode(WIFI_STA);

  for (uint8_t i = 1; i <= tries; i++)
  {
    Serial.printf("📶 WiFi explicit connect attempt %u/%u to SSID '%s'...\n", i, tries, ssid);

    WiFi.begin(ssid, pass);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < perTryTimeoutMs)
    {
      delay(200);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.print("✅ WiFi connected. IP: ");
      Serial.println(WiFi.localIP());
      return true;
    }

    WiFi.disconnect(true);
    delay(250);
  }
  return false;
}

// Only attempt WiFi.begin() if creds exist
static bool hasSavedWiFiCreds()
{
  return WiFi.SSID().length() > 0;
}

static bool tryConnectWifiSaved(uint8_t tries, uint32_t perTryTimeoutMs)
{
  WiFi.mode(WIFI_STA);

  for (uint8_t i = 1; i <= tries; i++)
  {
    Serial.printf("📶 WiFi connect attempt %u/%u...\n", i, tries);
    WiFi.begin();

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < perTryTimeoutMs)
    {
      delay(200);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.print("✅ WiFi connected. IP: ");
      Serial.println(WiFi.localIP());
      return true;
    }

    WiFi.disconnect(true);
    delay(250);
  }

  return false;
}

static void startConfigPortalAndSave()
{
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setCaptivePortalEnable(true);

  static char wsUrlBuf[200];
  static char tokenBuf[140];

  memset(wsUrlBuf, 0, sizeof(wsUrlBuf));
  memset(tokenBuf, 0, sizeof(tokenBuf));

  strlcpy(wsUrlBuf, cfg.wsUrl.c_str(), sizeof(wsUrlBuf));
  strlcpy(tokenBuf, cfg.authToken.c_str(), sizeof(tokenBuf));

  WiFiManagerParameter p_wsurl("wsurl", "WebSocket URL (ws:// or wss://)", wsUrlBuf, sizeof(wsUrlBuf));
  WiFiManagerParameter p_token("authtok", "Auth Token", tokenBuf, sizeof(tokenBuf));

  wm.addParameter(&p_wsurl);
  wm.addParameter(&p_token);

  Serial.println("🛠 Starting config portal...");
  setLed(false);

  bool ok = wm.startConfigPortal("DiscordVoiceSetup");
  if (!ok)
  {
    Serial.println("⚠️ Config portal timed out/failed");
    return;
  }

  cfg.wsUrl = String(p_wsurl.getValue());
  cfg.wsUrl.trim();
  cfg.authToken = String(p_token.getValue());
  cfg.authToken.trim();

  saveConfig(cfg);

  Serial.print("✅ WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}

// ---------------- OTA ----------------

static void performOtaUpdate(const String &url, const String &md5Optional)
{
  Serial.println("🚀 OTA requested");
  Serial.print("   URL: ");
  Serial.println(url);

  // stop ws cleanly
  webSocket.disconnect();
  delay(100);

  // LED off during update start
  setLed(false);

#if defined(ESP8266)
  // Optional MD5
  if (md5Optional.length() > 0)
  {
    ESPhttpUpdate.setMD5sum(md5Optional);
  }

  // Follow redirects (GitHub pages/CDNs sometimes redirect)
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (url.startsWith("https://"))
  {
    BearSSL::WiFiClientSecure client;
    client.setInsecure(); // practical for ESP8266 remote OTA
    auto ret = ESPhttpUpdate.update(client, url, String(FW_VERSION));
    switch (ret)
    {
    case HTTP_UPDATE_OK:
      // Usually reboot occurs automatically; if not:
      Serial.println("✅ OTA OK (ESP8266) - rebooting");
      ESP.restart();
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("ℹ️ OTA: no updates");
      break;
    case HTTP_UPDATE_FAILED:
    default:
      Serial.printf("❌ OTA failed (ESP8266): (%d) %s\n",
                    ESPhttpUpdate.getLastError(),
                    ESPhttpUpdate.getLastErrorString().c_str());
      break;
    }
  }
  else
  {
    WiFiClient client;
    auto ret = ESPhttpUpdate.update(client, url, String(FW_VERSION));
    switch (ret)
    {
    case HTTP_UPDATE_OK:
      Serial.println("✅ OTA OK (ESP8266) - rebooting");
      ESP.restart();
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("ℹ️ OTA: no updates");
      break;
    case HTTP_UPDATE_FAILED:
    default:
      Serial.printf("❌ OTA failed (ESP8266): (%d) %s\n",
                    ESPhttpUpdate.getLastError(),
                    ESPhttpUpdate.getLastErrorString().c_str());
      break;
    }
  }

#else // ESP32
  HTTPUpdate httpUpdate;

  // NOTE: Some ESP32 cores don't expose setMD5() on HTTPUpdate.
  // We'll skip MD5 verification on ESP32 for compatibility.
  // (ESP8266 still supports ESPhttpUpdate.setMD5()).
  (void)md5Optional;

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);

  if (url.startsWith("https://"))
  {
    WiFiClientSecure client;
    client.setInsecure(); // easiest; if you want CA pinning later we can do it
    t_httpUpdate_return ret = httpUpdate.update(client, url, String(FW_VERSION));
    if (ret == HTTP_UPDATE_OK)
    {
      Serial.println("✅ OTA OK (ESP32) - rebooting");
      delay(200);
      ESP.restart();
    }
    else if (ret == HTTP_UPDATE_NO_UPDATES)
    {
      Serial.println("ℹ️ OTA: no updates");
    }
    else
    {
      Serial.printf("❌ OTA failed (ESP32): (%d) %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
    }
  }
  else
  {
    WiFiClient client;
    t_httpUpdate_return ret = httpUpdate.update(client, url, String(FW_VERSION));
    if (ret == HTTP_UPDATE_OK)
    {
      Serial.println("✅ OTA OK (ESP32) - rebooting");
      delay(200);
      ESP.restart();
    }
    else if (ret == HTTP_UPDATE_NO_UPDATES)
    {
      Serial.println("ℹ️ OTA: no updates");
    }
    else
    {
      Serial.printf("❌ OTA failed (ESP32): (%d) %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
    }
  }
#endif

  // If update failed, resume normal operation
  Serial.println("↩️ OTA did not complete; resuming WS");
}

static bool maybeHandleOtaMessage(const String &msg)
{
  // Text format: OTA:<url>
  if (msg.startsWith("OTA:"))
  {
    String url = msg.substring(4);
    url.trim();
    if (url.length() == 0)
      return false;
    performOtaUpdate(url, "");
    return true;
  }

  // JSON format: {"type":"ota","url":"...","md5":"...","chip":"esp8266|esp32"}
  if (msg.length() > 0 && msg[0] == '{')
  {
    StaticJsonDocument<768> doc;
    auto err = deserializeJson(doc, msg);
    if (err)
      return false;

    const char *type = doc["type"] | "";
    if (String(type) != "ota")
      return false;

    const char *url = doc["url"] | "";
    const char *md5 = doc["md5"] | "";
    const char *chip = doc["chip"] | "";

#if defined(ESP8266)
    if (String(chip).length() > 0 && String(chip) != "esp8266")
    {
      Serial.println("ℹ️ OTA ignored: chip mismatch (need esp8266)");
      return true;
    }
#else
    if (String(chip).length() > 0 && String(chip) != "esp32")
    {
      Serial.println("ℹ️ OTA ignored: chip mismatch (need esp32)");
      return true;
    }
#endif

    String sUrl(url);
    sUrl.trim();
    String sMd5(md5);
    sMd5.trim();

    if (sUrl.length() == 0)
    {
      Serial.println("❌ OTA JSON missing url");
      return true;
    }

    performOtaUpdate(sUrl, sMd5);
    return true;
  }

  return false;
}

// -------------- WS setup --------------

static void setupWebSocketFromConfig()
{
  WsParts parts;
  if (!parseWsUrl(cfg.wsUrl, parts))
  {
    Serial.println("❌ Bad WS URL -> portal");
    startConfigPortalAndSave();
    return;
  }

#if defined(ESP8266)
  if (parts.secure)
  {
    Serial.println("⚠️ ESP8266 auto-switching wss:// to ws://");
    cfg.wsUrl.replace("wss://", "ws://");
    saveConfig(cfg);
    if (!parseWsUrl(cfg.wsUrl, parts))
      return;
  }
#endif

  authFailureCount = 0;

  webSocket.disconnect();
  webSocket.setReconnectInterval(0); // manual pacing
  webSocket.enableHeartbeat(15000, 3000, 2);

  webSocket.onEvent([](WStype_t type, uint8_t *payload, size_t length)
                    {
    switch (type) {
      case WStype_CONNECTED: {
        wsWasConnected = true;
        Serial.println("🔌 WS connected -> AUTH");
        authFailureCount = 0;

        String authMsg = "AUTH:" + cfg.authToken;
        webSocket.sendTXT(authMsg);
      } break;

      case WStype_DISCONNECTED:
        if (wsWasConnected) {
          Serial.println("⚠️ WS disconnected");
          wsWasConnected = false;
        }
        break;

      case WStype_TEXT: {
        String s = String((char*)payload).substring(0, length);
        s.trim();

        // OTA first
        if (maybeHandleOtaMessage(s)) return;

        if (s == "OK") {
          Serial.println("✅ Auth OK");
          authFailureCount = 0;
          return;
        }

        if (s == "NOAUTH") {
          authFailureCount++;
          Serial.printf("❌ NOAUTH (%u/%u)\n", authFailureCount, MAX_AUTH_FAILURES);

          if (authFailureCount >= MAX_AUTH_FAILURES) {
            Serial.println("🛠 Too many auth failures -> portal");
            authFailureCount = 0;
            startConfigPortalAndSave();
            setupWebSocketFromConfig();
          }
          return;
        }

        if (s == "1") { setLed(true);  return; }
        if (s == "0") { setLed(false); return; }
      } break;

      default:
        break;
    } });

  Serial.print("🌐 Connecting to: ");
  Serial.println(cfg.wsUrl);

  wsWasConnected = false;

  if (parts.secure)
  {
#if defined(ESP8266)
    // ESP8266 should have been downgraded already
    Serial.println("❌ ESP8266: wss:// not supported reliably. Use ws:// in portal.");
    webSocket.disconnect();
    return;
#else
    webSocket.beginSSL(parts.host.c_str(), parts.port, parts.path.c_str());
#endif
  }
  else
  {
    webSocket.begin(parts.host.c_str(), parts.port, parts.path.c_str());
  }
}

void setup()
{
  Serial.begin(115200);
  delay(200);

  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  cfg.wsUrl = DEFAULT_WS_URL;
  cfg.authToken = DEFAULT_AUTH_TOKEN;
  loadConfig(cfg);

  if (FORCE_PORTAL_PIN >= 0)
  {
    pinMode(FORCE_PORTAL_PIN, INPUT_PULLUP);
    if (digitalRead(FORCE_PORTAL_PIN) == LOW)
    {
      startConfigPortalAndSave();
    }
  }

  // If defaults are missing, force portal
  if (!defaultsHaveAppConfig())
  {
    Serial.println("🛠 Missing DEFAULT_WS_URL and/or DEFAULT_AUTH_TOKEN -> portal");
    startConfigPortalAndSave();
  }
  else
  {
    // Prefer saved creds first (if present)
    if (hasSavedWiFiCreds())
    {
      if (!tryConnectWifiSaved(WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
      {
        Serial.println("🛠 Saved WiFi failed -> portal");
        startConfigPortalAndSave();
      }
    }
    else if (defaultsHaveWifi())
    {
      // No saved creds: try default WiFi
      if (!tryConnectWifiExplicit(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
      {
        Serial.println("🛠 Default WiFi failed -> portal");
        startConfigPortalAndSave();
      }
    }
    else
    {
      // No saved creds and no default WiFi
      Serial.println("🛠 No saved WiFi and no DEFAULT_WIFI_SSID -> portal");
      startConfigPortalAndSave();
    }
  }

  setupWebSocketFromConfig();
  lastWsAttemptMs = 0;
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("📶 WiFi lost");
    setLed(false);
    webSocket.disconnect();

    if (hasSavedWiFiCreds())
    {
      if (!tryConnectWifiSaved(WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
      {
        Serial.println("🛠 WiFi failed -> portal");
        startConfigPortalAndSave();
      }
    }
    else if (defaultsHaveWifi())
    {
      if (!tryConnectWifiExplicit(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
      {
        Serial.println("🛠 Default WiFi failed -> portal");
        startConfigPortalAndSave();
      }
    }
    else
    {
      Serial.println("📡 WiFi creds missing -> portal");
      startConfigPortalAndSave();
    }

    setupWebSocketFromConfig();
  }

  webSocket.loop();

  // Manual reconnect pacing
  uint32_t now = millis();
  if (!webSocket.isConnected() && (now - lastWsAttemptMs) >= WS_RECONNECT_MS)
  {
    lastWsAttemptMs = now;
    setupWebSocketFromConfig();
  }

  delay(5);
}
