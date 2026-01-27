#include <Arduino.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
extern "C" {
  #include "user_interface.h"
  #include "wpa2_enterprise.h"
}
#else
#include <WiFi.h>
#include <LittleFS.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "esp_wpa2.h"
#include "esp_wifi.h"
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
// 802.1X (WPA Enterprise) credentials - leave empty if not using enterprise WiFi
static const char *DEFAULT_EAP_IDENTITY = "";
static const char *DEFAULT_EAP_PASSWORD = "";
// ===================================================

// Firmware version string (bump this when you want devices to accept new versions)
#ifndef FW_VERSION
  #define FW_VERSION "dev"
#endif
static const char *FW_VERSION_STR = FW_VERSION;

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
  // 802.1X WPA Enterprise credentials
  String eapIdentity;
  String eapPassword;
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
  out.eapIdentity = doc["eapIdentity"] | DEFAULT_EAP_IDENTITY;
  out.eapPassword = doc["eapPassword"] | DEFAULT_EAP_PASSWORD;
  return true;
}

static bool saveConfig(const AppConfig &in)
{
  if (!ensureFS())
    return false;

  StaticJsonDocument<768> doc;
  doc["wsUrl"] = in.wsUrl;
  doc["authToken"] = in.authToken;
  doc["eapIdentity"] = in.eapIdentity;
  doc["eapPassword"] = in.eapPassword;

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

// Check if we have a valid app config (from defaults OR loaded from flash)
static bool hasAppConfig()
{
  return cfg.wsUrl.length() > 0 && cfg.authToken.length() > 0;
}

// Check if 802.1X enterprise authentication is configured
static bool hasEapCredentials()
{
  return cfg.eapIdentity.length() > 0 && cfg.eapPassword.length() > 0;
}

// Try connecting using 802.1X WPA Enterprise
static bool tryConnectWifiEnterprise(const char *ssid, const char *identity, const char *password, uint8_t tries, uint32_t perTryTimeoutMs)
{
  Serial.println("🔐 Configuring 802.1X WPA Enterprise...");
  Serial.printf("   SSID: %s\n", ssid);
  Serial.printf("   Identity: %s\n", identity);
  Serial.printf("   Password: %s\n", password[0] ? "****" : "(empty)");
  
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_STA);
  delay(100);

#if defined(ESP8266)
  // Configure WPA2 Enterprise for ESP8266
  Serial.println("🔐 Setting up ESP8266 WPA2 Enterprise...");
  wifi_station_clear_enterprise_identity();
  wifi_station_set_enterprise_identity((uint8_t *)identity, strlen(identity));
  wifi_station_set_enterprise_username((uint8_t *)identity, strlen(identity));
  wifi_station_set_enterprise_password((uint8_t *)password, strlen(password));
  int ret = wifi_station_set_wpa2_enterprise_auth(1);
  Serial.printf("🔐 wifi_station_set_wpa2_enterprise_auth returned: %d\n", ret);
#else
  // Configure WPA2 Enterprise for ESP32
  // Must initialize WiFi first before setting enterprise config
  Serial.println("🔐 Setting up ESP32 WPA2 Enterprise...");
  
  // Initialize WiFi to ensure it's ready
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  delay(100);
  
  esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)identity, strlen(identity));
  esp_wifi_sta_wpa2_ent_set_username((uint8_t *)identity, strlen(identity));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t *)password, strlen(password));
  esp_err_t err = esp_wifi_sta_wpa2_ent_enable();
  Serial.printf("🔐 esp_wifi_sta_wpa2_ent_enable returned: %d (0=OK)\n", err);
  if (err != ESP_OK) {
    Serial.printf("❌ WPA2 Enterprise setup failed with error: 0x%x\n", err);
  }
#endif

  for (uint8_t i = 1; i <= tries; i++)
  {
    Serial.printf("📶 WiFi 802.1X connect attempt %u/%u to SSID '%s' as '%s'...\n", i, tries, ssid, identity);

    WiFi.begin(ssid); // No password for WPA Enterprise - uses EAP credentials

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < perTryTimeoutMs)
    {
      delay(200);
      Serial.print(".");
    }
    Serial.println();

    wl_status_t status = WiFi.status();
    Serial.printf("📶 WiFi status after attempt: %d\n", status);
    
    if (status == WL_CONNECTED)
    {
      Serial.print("✅ WiFi 802.1X connected. IP: ");
      Serial.println(WiFi.localIP());
      return true;
    }
    
    // Log specific failure reasons
    switch (status) {
      case WL_NO_SSID_AVAIL:
        Serial.println("❌ SSID not found");
        break;
      case WL_CONNECT_FAILED:
        Serial.println("❌ Connection failed (wrong password or auth rejected)");
        break;
      case WL_DISCONNECTED:
        Serial.println("❌ Disconnected");
        break;
      default:
        Serial.printf("❌ Connection failed with status: %d\n", status);
        break;
    }

    WiFi.disconnect(true);
    delay(250);
  }

  Serial.println("❌ All 802.1X connection attempts failed");
  
  // Disable WPA2 Enterprise on failure to allow normal connection attempts
#if defined(ESP8266)
  Serial.println("🔐 Disabling WPA2 Enterprise mode");
  wifi_station_set_wpa2_enterprise_auth(0);
#else
  Serial.println("🔐 Disabling WPA2 Enterprise mode");
  esp_wifi_sta_wpa2_ent_disable();
#endif
  return false;
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
  static char eapIdentityBuf[100];
  static char eapPasswordBuf[100];

  memset(wsUrlBuf, 0, sizeof(wsUrlBuf));
  memset(tokenBuf, 0, sizeof(tokenBuf));
  memset(eapIdentityBuf, 0, sizeof(eapIdentityBuf));
  memset(eapPasswordBuf, 0, sizeof(eapPasswordBuf));

  strlcpy(wsUrlBuf, cfg.wsUrl.c_str(), sizeof(wsUrlBuf));
  strlcpy(tokenBuf, cfg.authToken.c_str(), sizeof(tokenBuf));
  strlcpy(eapIdentityBuf, cfg.eapIdentity.c_str(), sizeof(eapIdentityBuf));
  strlcpy(eapPasswordBuf, cfg.eapPassword.c_str(), sizeof(eapPasswordBuf));

  WiFiManagerParameter p_wsurl("wsurl", "WebSocket URL (ws:// or wss://)", wsUrlBuf, sizeof(wsUrlBuf));
  WiFiManagerParameter p_token("authtok", "Auth Token", tokenBuf, sizeof(tokenBuf));
  
  // 802.1X Enterprise WiFi parameters
  WiFiManagerParameter p_eap_header("<hr><h3>802.1X Enterprise WiFi (optional)</h3><p style='font-size:0.9em;color:#666;'>For corporate/university networks using WPA2-Enterprise authentication. Leave blank for standard home WiFi.</p>");
  WiFiManagerParameter p_eap_identity("eapid", "802.1X Username/Identity", eapIdentityBuf, sizeof(eapIdentityBuf), "autocapitalize='off' autocorrect='off' autocomplete='username'");
  WiFiManagerParameter p_eap_password("eappwd", "802.1X Password", eapPasswordBuf, sizeof(eapPasswordBuf), "type='password' autocapitalize='off' autocomplete='current-password'");

  wm.addParameter(&p_wsurl);
  wm.addParameter(&p_token);
  wm.addParameter(&p_eap_header);
  wm.addParameter(&p_eap_identity);
  wm.addParameter(&p_eap_password);
  
  // Don't let WiFiManager try to connect - we'll handle it ourselves for 802.1X support
  wm.setBreakAfterConfig(true);
  // Set a very short connect timeout so WiFiManager's connection attempt fails fast
  wm.setConnectTimeout(1);

  Serial.println("🛠 Starting config portal...");
  setLed(false);

  wm.startConfigPortal("DiscordVoiceSetup");
  
  // Get the SSID/password that was entered in the portal
  String portalSSID = wm.getWiFiSSID();
  String portalPass = wm.getWiFiPass();
  
  // Check if user actually submitted config (SSID will be set)
  if (portalSSID.length() == 0)
  {
    Serial.println("⚠️ Config portal closed without submitting config");
    return;
  }

  cfg.wsUrl = String(p_wsurl.getValue());
  cfg.wsUrl.trim();
  cfg.authToken = String(p_token.getValue());
  cfg.authToken.trim();
  cfg.eapIdentity = String(p_eap_identity.getValue());
  cfg.eapIdentity.trim();
  cfg.eapPassword = String(p_eap_password.getValue());
  cfg.eapPassword.trim();

  saveConfig(cfg);
  
  Serial.printf("🔐 Portal closed. SSID: %s\n", portalSSID.c_str());
  Serial.printf("🔐 802.1X Identity: %s\n", cfg.eapIdentity.length() > 0 ? cfg.eapIdentity.c_str() : "(not set)");
  
  // Now connect with 802.1X if credentials are provided
  if (cfg.eapIdentity.length() > 0 && cfg.eapPassword.length() > 0)
  {
    Serial.println("🔐 Using 802.1X Enterprise authentication...");
    if (tryConnectWifiEnterprise(portalSSID.c_str(), cfg.eapIdentity.c_str(), cfg.eapPassword.c_str(), WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
    {
      Serial.print("✅ WiFi 802.1X connected. IP: ");
      Serial.println(WiFi.localIP());
      return;
    }
    Serial.println("❌ 802.1X connection failed, trying standard connection...");
  }
  
  // Standard connection (or fallback)
  if (tryConnectWifiExplicit(portalSSID.c_str(), portalPass.c_str(), WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
  {
    Serial.print("✅ WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("❌ WiFi connection failed");
  }
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
    auto ret = ESPhttpUpdate.update(client, url, String(FW_VERSION_STR));
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
    auto ret = ESPhttpUpdate.update(client, url, String(FW_VERSION_STR));
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
    t_httpUpdate_return ret = httpUpdate.update(client, url, String(FW_VERSION_STR));
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
    t_httpUpdate_return ret = httpUpdate.update(client, url, String(FW_VERSION_STR));
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

// -------------- Serial Command Handler --------------
static void handleSerialCommand(const String &cmd)
{
  // CONFIG:{"wsUrl":"...","authToken":"..."}
  if (cmd.startsWith("CONFIG:"))
  {
    String json = cmd.substring(7);
    StaticJsonDocument<512> doc;
    auto err = deserializeJson(doc, json);
    if (!err)
    {
      bool changed = false;
      
      if (doc.containsKey("wsUrl"))
      {
        cfg.wsUrl = doc["wsUrl"].as<String>();
        changed = true;
      }
      if (doc.containsKey("authToken"))
      {
        cfg.authToken = doc["authToken"].as<String>();
        changed = true;
      }
      if (doc.containsKey("eapIdentity"))
      {
        cfg.eapIdentity = doc["eapIdentity"].as<String>();
        changed = true;
      }
      if (doc.containsKey("eapPassword"))
      {
        cfg.eapPassword = doc["eapPassword"].as<String>();
        changed = true;
      }
      
      if (changed)
      {
        saveConfig(cfg);
        Serial.println("OK:CONFIG_SAVED");
        Serial.println("OK:REBOOTING");
        Serial.flush();
        delay(100);
        ESP.restart();
      }
      else
      {
        Serial.println("OK:NO_CHANGES");
      }
    }
    else
    {
      Serial.println("ERR:INVALID_JSON");
    }
  }
  else if (cmd == "GET_CONFIG")
  {
    StaticJsonDocument<512> doc;
    doc["wsUrl"] = cfg.wsUrl;
    doc["authToken"] = cfg.authToken.length() > 0 ? "****" : "";
    doc["eapIdentity"] = cfg.eapIdentity;
    doc["hasEapPassword"] = cfg.eapPassword.length() > 0;
    doc["version"] = FW_VERSION_STR;
    Serial.print("CONFIG:");
    serializeJson(doc, Serial);
    Serial.println();
  }
  else if (cmd == "REBOOT")
  {
    Serial.println("OK:REBOOTING");
    Serial.flush();
    delay(100);
    ESP.restart();
  }
  else if (cmd == "PORTAL")
  {
    Serial.println("OK:STARTING_PORTAL");
    startConfigPortalAndSave();
    setupWebSocketFromConfig();
  }
  else if (cmd == "PING")
  {
    Serial.println("PONG");
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
  cfg.eapIdentity = DEFAULT_EAP_IDENTITY;
  cfg.eapPassword = DEFAULT_EAP_PASSWORD;
  loadConfig(cfg);

  // Brief window to catch WEB_CONFIG command from the web UI
  // If WEB_CONFIG is received, enter extended configuration mode
  Serial.println("⏳ Send WEB_CONFIG within 5s for serial configuration...");
  uint32_t waitStart = millis();
  bool webConfigMode = false;
  
  while (millis() - waitStart < 5000)
  {
    if (Serial.available())
    {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd.length() > 0)
      {
        if (cmd == "WEB_CONFIG")
        {
          webConfigMode = true;
          Serial.println("OK:WEB_CONFIG_MODE");
          Serial.println("🔧 Web config mode active - waiting for configuration...");
          Serial.println("💡 Send CONFIG:{\"wsUrl\":\"...\",\"authToken\":\"...\"} to configure");
          break;
        }
        else
        {
          handleSerialCommand(cmd);
          loadConfig(cfg);
        }
      }
    }
    delay(10);
  }
  
  // Extended wait if web config mode was activated
  if (webConfigMode)
  {
    waitStart = millis();
    while (millis() - waitStart < 300000)  // 5 minute window
    {
      if (Serial.available())
      {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd.length() > 0)
        {
          if (cmd == "WEB_CONFIG")
          {
            // Refresh the timeout
            waitStart = millis();
            Serial.println("OK:WEB_CONFIG_MODE");
          }
          else
          {
            handleSerialCommand(cmd);
            loadConfig(cfg);
            // If config now exists, we're done
            if (hasAppConfig())
            {
              Serial.println("✅ Configuration complete!");
              break;
            }
          }
        }
      }
      delay(10);
    }
  }

  if (FORCE_PORTAL_PIN >= 0)
  {
    pinMode(FORCE_PORTAL_PIN, INPUT_PULLUP);
    if (digitalRead(FORCE_PORTAL_PIN) == LOW)
    {
      startConfigPortalAndSave();
    }
  }

  // Check if we have app config (from defaults, flash, or just received via serial)
  if (!hasAppConfig())
  {
    Serial.println("🛠 No WS_URL/AUTH_TOKEN configured -> portal");
    Serial.println("💡 Tip: Send CONFIG:{\"wsUrl\":\"...\",\"authToken\":\"...\"} via serial to skip portal");
    startConfigPortalAndSave();
  }
  
  // Now try to connect to WiFi
  if (WiFi.status() != WL_CONNECTED)
  {
    // Prefer saved creds first (if present)
    if (hasSavedWiFiCreds())
    {
      // Try 802.1X first if credentials are available
      if (hasEapCredentials())
      {
        if (!tryConnectWifiEnterprise(WiFi.SSID().c_str(), cfg.eapIdentity.c_str(), cfg.eapPassword.c_str(), WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
        {
          Serial.println("🛠 802.1X WiFi failed -> trying standard connection");
          if (!tryConnectWifiSaved(WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
          {
            Serial.println("🛠 Saved WiFi failed -> portal");
            startConfigPortalAndSave();
          }
        }
      }
      else if (!tryConnectWifiSaved(WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
      {
        Serial.println("🛠 Saved WiFi failed -> portal");
        startConfigPortalAndSave();
      }
    }
    else if (defaultsHaveWifi())
    {
      // Try 802.1X first if credentials are available
      if (hasEapCredentials())
      {
        if (!tryConnectWifiEnterprise(DEFAULT_WIFI_SSID, cfg.eapIdentity.c_str(), cfg.eapPassword.c_str(), WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
        {
          Serial.println("🛠 802.1X WiFi failed -> trying standard connection");
          if (!tryConnectWifiExplicit(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
          {
            Serial.println("🛠 Default WiFi failed -> portal");
            startConfigPortalAndSave();
          }
        }
      }
      // No saved creds: try default WiFi
      else if (!tryConnectWifiExplicit(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
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
  // Handle serial commands FIRST - before WiFi checks so config works even without WiFi
  if (Serial.available())
  {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();  // Removes \r and whitespace
    
    if (cmd.length() > 0)
    {
      handleSerialCommand(cmd);
    }
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("📶 WiFi lost");
    setLed(false);
    webSocket.disconnect();

    if (hasSavedWiFiCreds())
    {
      // Try 802.1X first if credentials are available
      if (hasEapCredentials())
      {
        if (!tryConnectWifiEnterprise(WiFi.SSID().c_str(), cfg.eapIdentity.c_str(), cfg.eapPassword.c_str(), WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
        {
          Serial.println("🛠 802.1X WiFi failed -> trying standard connection");
          if (!tryConnectWifiSaved(WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
          {
            Serial.println("🛠 WiFi failed -> portal");
            startConfigPortalAndSave();
          }
        }
      }
      else if (!tryConnectWifiSaved(WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
      {
        Serial.println("🛠 WiFi failed -> portal");
        startConfigPortalAndSave();
      }
    }
    else if (defaultsHaveWifi())
    {
      // Try 802.1X first if credentials are available
      if (hasEapCredentials())
      {
        if (!tryConnectWifiEnterprise(DEFAULT_WIFI_SSID, cfg.eapIdentity.c_str(), cfg.eapPassword.c_str(), WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
        {
          Serial.println("🛠 802.1X WiFi failed -> trying standard connection");
          if (!tryConnectWifiExplicit(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
          {
            Serial.println("🛠 Default WiFi failed -> portal");
            startConfigPortalAndSave();
          }
        }
      }
      else if (!tryConnectWifiExplicit(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, WIFI_CONNECT_TRIES, WIFI_TRY_TIMEOUT_MS))
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
