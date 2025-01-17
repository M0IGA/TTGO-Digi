#include <list>
#include "taskWebServer.h"
#include "preference_storage.h"
#include "syslog_log.h"
#include "PSRAMJsonDocument.h"
#include <time.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

/**
 * @see board_build.embed_txtfiles in platformio.ini
 */
extern const char web_index_html[] asm("_binary_data_embed_index_html_out_start");
extern const char web_index_html_end[] asm("_binary_data_embed_index_html_out_end");
extern const char web_style_css[] asm("_binary_data_embed_style_css_out_start");
extern const char web_style_css_end[] asm("_binary_data_embed_style_css_out_end");
extern const char web_js_js[] asm("_binary_data_embed_js_js_out_start");
extern const char web_js_js_end[] asm("_binary_data_embed_js_js_out_end");

// Variable needed to send beacon from html page
extern bool manBeacon;

// Variable to show AP status
extern bool apEnabled;
extern bool apConnected;
extern String infoApName;
extern String infoApPass;
extern String infoApAddr;

extern int8_t wifi_txpwr_mode_AP;
extern int8_t wifi_txpwr_mode_STA;

extern bool tncServer_enabled;
extern bool gpsServer_enabled;

// For APRS-IS connection
extern String to_aprsis_data;
extern boolean aprsis_enabled;
extern String aprsis_host;
extern uint16_t aprsis_port;
extern String aprsis_filter;
extern String aprsis_callsign;
extern String aprsis_password;
extern uint8_t aprsis_data_allow_inet_to_rf;
extern String MY_APRS_DEST_IDENTIFYER;

extern double lora_freq_rx_curr;
extern boolean lora_tx_enabled;

extern char src_call_blacklist;

QueueHandle_t webListReceivedQueue = nullptr;
std::list <tReceivedPacketData*> receivedPackets;
const int MAX_RECEIVED_LIST_SIZE = 50;

String apSSID = "";
String apPassword;
String defApPassword = "xxxxxxxxxx";

// needed for aprsis igate functionality
String aprsis_status = "Disconnected";
// aprsis 3rd party traffic encoding
String generate_third_party_packet(String, String);
#ifdef KISS_PROTOCOL
extern void sendToTNC(const String &);
#endif
extern uint8_t txPower;
extern double lora_freq;
extern ulong lora_speed;
extern uint8_t txPower_cross_digi;
extern ulong lora_speed_cross_digi;
extern double lora_freq_cross_digi;
extern void loraSend(byte, float, ulong, const String &);

WebServer server(80);
#ifdef KISS_PROTOCOL
  WiFiServer tncServer(NETWORK_TNC_PORT);
#endif
WiFiServer gpsServer(NETWORK_GPS_PORT);

#ifdef ENABLE_SYSLOG
  // A UDP instance to let us send and receive packets over UDP
  WiFiUDP udpClient;

  // Create a new empty syslog instance
  Syslog syslog(udpClient, SYSLOG_PROTO_IETF);
#endif

#ifdef T_BEAM_V1_0
  #include <axp20x.h>
  extern AXP20X_Class axp;
#endif


void sendCacheHeader() { server.sendHeader("Cache-Control", "max-age=3600"); }
void sendGzipHeader() { server.sendHeader("Content-Encoding", "gzip"); }

String jsonEscape(String s){
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    s.replace("\x7f", "\\\x7f");
    for(char i = 0; i < 0x1f; i++){
        s.replace(String(i), "\\" + String((char)i));
    }
  return s;
}

String jsonLineFromPreferenceString(const char *preferenceName, bool last=false){
  return String("\"") + preferenceName + "\":\"" + jsonEscape(preferences.getString(preferenceName, "")) + (last ?  + R"(")" :  + R"(",)");
}
String jsonLineFromPreferenceBool(const char *preferenceName, bool last=false){
  return String("\"") + preferenceName + "\":" + (preferences.getBool(preferenceName) ? "true" : "false") + (last ?  + R"()" :  + R"(,)");
}
String jsonLineFromPreferenceInt(const char *preferenceName, bool last=false){
  return String("\"") + preferenceName + "\":" + (preferences.getInt(preferenceName)) + (last ?  + R"()" :  + R"(,)");
}
String jsonLineFromPreferenceDouble(const char *preferenceName, bool last=false){
    return String("\"") + preferenceName + "\":" + String(preferences.getDouble(preferenceName),4) + (last ?  + R"()" :  + R"(,)");
}
String jsonLineFromString(const char *name, const char *value, bool last=false){
  return String("\"") + name + "\":\"" + jsonEscape(value) + "\"" + (last ?  + R"()" :  + R"(,)");
}
String jsonLineFromInt(const char *name, const int value, bool last=false){
  return String("\"") + name + "\":" + String(value) + (last ?  + R"()" :  + R"(,)");
}
String jsonLineFromDouble(const char *name, const double value, bool last=false){
  return String("\"") + name + "\":" + String(value, 4) + (last ?  + R"()" :  + R"(,)");
}

void handle_NotFound(){
  sendCacheHeader();
  server.send(404, "text/plain", "Not found");
}

void handle_Index() {
  sendGzipHeader();
  server.send_P(200, "text/html", web_index_html, web_index_html_end - web_index_html);
}

void handle_Style() {
  sendCacheHeader();
  sendGzipHeader();
  server.send_P(200, "text/css", web_style_css, web_style_css_end - web_style_css);
}

void handle_Js() {
  sendCacheHeader();
  sendGzipHeader();
  server.send_P(200, "text/javascript", web_js_js, web_js_js_end-web_js_js);
}

void handle_ScanWifi() {
  String listResponse = R"(<label for="networks_found_list">Networks found:</label><select class="u-full-width" id="networks_found_list">)";
  int n = WiFi.scanNetworks();
  listResponse += "<option value=\"\">Select Network</option>";

  for (int i = 0; i < n; ++i) {
    listResponse += "<option value=\""+WiFi.SSID(i)+"\">" + WiFi.SSID(i) + "</option>";
  }
  listResponse += "</select>";
  server.send(200,"text/html", listResponse);
}

void handle_SaveWifiCfg() {

  if (!server.hasArg(PREF_WIFI_SSID) || !server.hasArg(PREF_WIFI_PASSWORD) || !server.hasArg(PREF_AP_PASSWORD)){
    server.send(500, "text/plain", "Invalid request, make sure all fields are set");
  }

  // Mode STA:
  if (!server.arg(PREF_WIFI_SSID).length()){
    server.send(403, "text/plain", "Empty SSID");
  } else {
    // Update SSID
    preferences.putString(PREF_WIFI_SSID, server.arg(PREF_WIFI_SSID));
    Serial.println("Updated SSID: " + server.arg(PREF_WIFI_SSID));
  }

  if (server.arg(PREF_WIFI_PASSWORD)!="*" && server.arg(PREF_WIFI_PASSWORD).length()>0 && server.arg(PREF_WIFI_PASSWORD).length()<8){
    server.send(403, "text/plain", "WiFi Password must be minimum 8 character");
  } else {
    if (server.arg(PREF_WIFI_PASSWORD)!="*") {
      // Update WiFi password
      preferences.putString(PREF_WIFI_PASSWORD, server.arg(PREF_WIFI_PASSWORD));
      Serial.println("Updated WiFi PASS: " + server.arg(PREF_WIFI_PASSWORD));
    }
  }
  if (server.hasArg(PREF_WIFI_TXPWR_MODE_STA)) {
    // Web chooser min, low, mid, high, max
    // We'll use "min", "low", "mid", "high", "max" -> 2dBm (1.5mW) -> 8, 11dBm (12mW) -> 44, 15dBm (32mW) -> 60, 18dBm (63mW) ->72, 20dBm (100mW) ->80
    int8_t choosed = server.arg(PREF_WIFI_TXPWR_MODE_STA).toInt();
    if (choosed < 0) choosed = 8;
    else if (choosed > 84) choosed = 84;
    preferences.putInt(PREF_WIFI_TXPWR_MODE_STA, choosed);
  }

  // Mode AP:
  if (server.arg(PREF_AP_PASSWORD)!="*" && server.arg(PREF_AP_PASSWORD).length()<8){
    server.send(403, "text/plain", "AP Password must be minimum 8 character");
  } else {
    if (server.arg(PREF_AP_PASSWORD)!="*") {
      // Update AP password
      preferences.putString(PREF_AP_PASSWORD, server.arg(PREF_AP_PASSWORD));
      Serial.println("Updated AP PASS: " + server.arg(PREF_AP_PASSWORD));
    }
  }
  if (server.hasArg(PREF_WIFI_TXPWR_MODE_AP)) {
    // Web chooser min, low, mid, high, max
    // We'll use "min", "low", "mid", "high", "max" -> 2dBm (1.5mW) -> 8, 11dBm (12mW) -> 44, 15dBm (32mW) -> 60, 18dBm (63mW) ->72, 20dBm (100mW) ->80
    int8_t choosed = server.arg(PREF_WIFI_TXPWR_MODE_AP).toInt();
    if (choosed < 0) choosed = 8;
    else if (choosed > 84) choosed = 84;
    preferences.putInt(PREF_WIFI_TXPWR_MODE_AP, choosed);
  }

  if (server.hasArg(PREF_WIFI_ENABLE))
    preferences.putInt(PREF_WIFI_ENABLE, server.arg(PREF_WIFI_ENABLE).toInt());

  preferences.putBool(PREF_TNCSERVER_ENABLE, server.hasArg(PREF_TNCSERVER_ENABLE));
  preferences.putBool(PREF_GPSSERVER_ENABLE, server.hasArg(PREF_GPSSERVER_ENABLE));
  String s = "";
  if (server.hasArg(PREF_NTP_SERVER) && server.arg(PREF_NTP_SERVER).length()) {
    s = server.arg(PREF_NTP_SERVER);
    s.trim();
  }
  preferences.putString(PREF_NTP_SERVER, s);

  server.sendHeader("Location", "/");
  server.send(302,"text/html", "");
}

void handle_Reboot() {
  server.sendHeader("Location", "/");
  server.send(302,"text/html", "");
  server.close();
  ESP.restart();
}

void handle_Beacon() {
  server.sendHeader("Location", "/");
  server.send(302,"text/html", "");
  manBeacon=true;
}

void handle_Shutdown() {
  #ifdef T_BEAM_V1_0
    server.send(200,"text/html", "Shutdown");
    axp.setChgLEDMode(AXP20X_LED_OFF);
    axp.shutdown();
  #else
    server.send(404,"text/html", "Not supported");
  #endif
}

void handle_Restore() {
  server.sendHeader("Location", "/");
  server.send(302,"text/html", "");
  preferences.clear();
  preferences.end();
  ESP.restart();
}

void handle_Cfg() {
  String jsonData = "{";
  jsonData += String("\"") + PREF_WIFI_PASSWORD + "\": \"" + jsonEscape((preferences.getString(PREF_WIFI_PASSWORD, "").isEmpty() ? String("") : "*")) + R"(",)";
  jsonData += String("\"") + PREF_AP_PASSWORD + "\": \"" + jsonEscape((preferences.getString(PREF_AP_PASSWORD, "").isEmpty() ? String("") : "*")) + R"(",)";
  jsonData += jsonLineFromPreferenceInt(PREF_WIFI_ENABLE);
  jsonData += jsonLineFromPreferenceString(PREF_WIFI_SSID);
  jsonData += jsonLineFromPreferenceInt(PREF_WIFI_TXPWR_MODE_AP);
  jsonData += jsonLineFromPreferenceInt(PREF_WIFI_TXPWR_MODE_STA);
  jsonData += jsonLineFromPreferenceBool(PREF_TNCSERVER_ENABLE);
  jsonData += jsonLineFromPreferenceBool(PREF_GPSSERVER_ENABLE);
  jsonData += jsonLineFromPreferenceString(PREF_NTP_SERVER);
  jsonData += jsonLineFromPreferenceDouble(PREF_LORA_FREQ_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_LORA_SPEED_PRESET);
  jsonData += jsonLineFromPreferenceBool(PREF_LORA_RX_ENABLE);
  jsonData += jsonLineFromPreferenceBool(PREF_LORA_TX_ENABLE);
  jsonData += jsonLineFromPreferenceInt(PREF_LORA_TX_POWER);
  jsonData += jsonLineFromPreferenceBool(PREF_LORA_AUTOMATIC_CR_ADAPTION_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_LORA_ADD_SNR_RSSI_TO_PATH_PRESET);
  jsonData += jsonLineFromPreferenceBool(PREF_LORA_ADD_SNR_RSSI_TO_PATH_END_AT_KISS_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_DIGIPEATING_MODE_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_CROSS_DIGIPEATING_MODE_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_LORA_TX_BEACON_AND_KISS_TO_FREQUENCIES_PRESET);
  jsonData += jsonLineFromPreferenceBool(PREF_LORA_TX_BEACON_AND_KISS_TO_APRSIS_PRESET);
  jsonData += jsonLineFromPreferenceDouble(PREF_LORA_FREQ_CROSSDIGI_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_LORA_SPEED_CROSSDIGI_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_LORA_TX_POWER_CROSSDIGI_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_LORA_RX_ON_FREQUENCIES_PRESET);
  jsonData += jsonLineFromPreferenceString(PREF_APRS_CALLSIGN);
  jsonData += jsonLineFromPreferenceString(PREF_APRS_RELAY_PATH);
  jsonData += jsonLineFromPreferenceString(PREF_APRS_SYMBOL_TABLE);
  jsonData += jsonLineFromPreferenceString(PREF_APRS_SYMBOL);
  jsonData += jsonLineFromPreferenceString(PREF_APRS_COMMENT);
  jsonData += jsonLineFromPreferenceString(PREF_APRS_LATITUDE_PRESET);
  jsonData += jsonLineFromPreferenceString(PREF_APRS_LONGITUDE_PRESET);
  jsonData += jsonLineFromPreferenceString(PREF_APRS_SENDER_BLACKLIST);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_FIXED_BEACON_INTERVAL_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_SB_MIN_INTERVAL_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_SB_MAX_INTERVAL_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_SB_MIN_SPEED_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_SB_MAX_SPEED_PRESET);
  jsonData += jsonLineFromPreferenceDouble(PREF_APRS_SB_ANGLE_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_SB_TURN_SLOPE_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_SB_TURN_TIME_PRESET);
  jsonData += jsonLineFromPreferenceBool(PREF_APRS_SHOW_BATTERY);
  jsonData += jsonLineFromPreferenceBool(PREF_APRS_FIXED_BEACON_PRESET);
  //jsonData += jsonLineFromPreferenceBool(PREF_APRS_SHOW_ALTITUDE);
  jsonData += jsonLineFromPreferenceBool(PREF_APRS_SHOW_ALTITUDE_INSIDE_COMPRESSED_POSITION);
  jsonData += jsonLineFromPreferenceInt(PREF_APRS_ALTITUDE_RATIO);
  jsonData += jsonLineFromPreferenceBool(PREF_APRS_GPS_EN);
  jsonData += jsonLineFromPreferenceBool(PREF_ACCEPT_OWN_POSITION_REPORTS_VIA_KISS);
  jsonData += jsonLineFromPreferenceBool(PREF_GPS_ALLOW_SLEEP_WHILE_KISS);
  jsonData += jsonLineFromPreferenceBool(PREF_ENABLE_TNC_SELF_TELEMETRY);
  jsonData += jsonLineFromPreferenceInt(PREF_TNC_SELF_TELEMETRY_INTERVAL);
  jsonData += jsonLineFromPreferenceInt(PREF_TNC_SELF_TELEMETRY_MIC);
  jsonData += jsonLineFromPreferenceString(PREF_TNC_SELF_TELEMETRY_PATH);
  jsonData += jsonLineFromPreferenceBool(PREF_DEV_OL_EN);
  jsonData += jsonLineFromPreferenceBool(PREF_APRS_SHOW_CMT);
  jsonData += jsonLineFromPreferenceBool(PREF_APRS_COMMENT_RATELIMIT_PRESET);
  jsonData += jsonLineFromPreferenceBool(PREF_DEV_BT_EN);
  jsonData += jsonLineFromPreferenceInt(PREF_DEV_SHOW_RX_TIME);
  jsonData += jsonLineFromPreferenceBool(PREF_DEV_AUTO_SHUT);
  jsonData += jsonLineFromPreferenceInt(PREF_DEV_AUTO_SHUT_PRESET);
  jsonData += jsonLineFromPreferenceInt(PREF_DEV_REBOOT_INTERVAL);
  jsonData += jsonLineFromPreferenceInt(PREF_DEV_SHOW_OLED_TIME);
  jsonData += jsonLineFromPreferenceInt(PREF_DEV_CPU_FREQ);
  jsonData += jsonLineFromPreferenceBool(PREF_APRSIS_EN);
  jsonData += jsonLineFromPreferenceString(PREF_APRSIS_SERVER_NAME);
  jsonData += jsonLineFromPreferenceInt(PREF_APRSIS_SERVER_PORT);
  jsonData += jsonLineFromPreferenceString(PREF_APRSIS_FILTER);
  jsonData += jsonLineFromPreferenceString(PREF_APRSIS_CALLSIGN);
  jsonData += jsonLineFromPreferenceString(PREF_APRSIS_PASSWORD);
  jsonData += jsonLineFromPreferenceInt(PREF_APRSIS_ALLOW_INET_TO_RF);
  jsonData += jsonLineFromDouble("lora_freq_rx_curr", lora_freq_rx_curr);
  jsonData += jsonLineFromString("aprsis_status", aprsis_status.c_str());
  jsonData += jsonLineFromInt("FreeHeap", ESP.getFreeHeap());
  jsonData += jsonLineFromInt("HeapSize", ESP.getHeapSize());
  jsonData += jsonLineFromInt("FreeSketchSpace", ESP.getFreeSketchSpace());
  jsonData += jsonLineFromInt("PSRAMSize", ESP.getPsramSize());
  jsonData += jsonLineFromInt("PSRAMFree", ESP.getFreePsram());
  jsonData += jsonLineFromInt("UptimeMinutes", millis()/1000/60, true);

  jsonData += "}";
  server.send(200,"application/json", jsonData);
}

void handle_ReceivedList() {
  //PSRAMJsonDocument doc(MAX_RECEIVED_LIST_SIZE * 1000);
  DynamicJsonDocument doc(MAX_RECEIVED_LIST_SIZE * 500);
  JsonObject root = doc.to<JsonObject>();
  auto received = root.createNestedArray("received");
  for (auto element: receivedPackets){
    char buf[64];
    strftime(buf, 64, "%Y-%m-%d %H:%M:%S", &element->rxTime);
    auto packet_data = received.createNestedObject();
    packet_data["time"] = String(buf);
    packet_data["packet"] = element->packet->c_str();
    packet_data["rssi"] = element->RSSI;
    packet_data["snr"] = element->SNR;
  }

  server.send(200,"application/json", doc.as<String>());
}


void store_lat_long(float f_lat, float f_long) {
  char buf[13];

  int i_deg = (int ) (f_lat < 0 ? (f_lat*-1) : f_lat);
  float f_min = ((f_lat < 0 ? f_lat*-1 : f_lat) -((int ) i_deg))*60.0;
  if (f_min > 59.99995) { f_min = 0.0; i_deg++; };
  if (i_deg >= 180) { i_deg = 179; f_min = 59.9999; };
  sprintf(buf, "%2.2d-%07.4f%c", i_deg, f_min, f_lat < 0 ? 'S' :  'N');
  preferences.putString(PREF_APRS_LATITUDE_PRESET, String(buf));

  i_deg = (int ) (f_long < 0 ? (f_long*-1) : f_long);
  f_min = ((f_long < 0 ? f_long*-1 : f_long) -((int ) i_deg))*60.0;
  if (f_min > 59.99995) { f_min = 0.0; i_deg++; };
  if (i_deg >= 90) { i_deg = 89; f_min = 59.9999; };
  sprintf(buf, "%3.3d-%07.4f%c", i_deg, f_min, f_long < 0 ? 'W' :  'E');
  preferences.putString(PREF_APRS_LONGITUDE_PRESET, String(buf));
}


boolean is_locator(String s) {
  int i = s.length();
  if ((i % 2) || i < 6)
    return false;
  const char *p = s.c_str();
  if (*p < 'A' || p[1] > 'R')
    return false;
  p += 2;
  while (*p) {
    if (*p < '0' || p[1] > '9')
      return false;
    p += 2;
    if (!*p) return true;
    if (*p < 'A' || p[1] > 'X')
      return false;
    p += 2;
  }
  return true;
}


void compute_locator(String s)
{
  float f_long, f_lat;
  const char *p = s.c_str();
  int s_len = s.length();
  f_long = (-9 + p[0]-'A') * 10 + p[2]-'0';
  p = p+4;
  uint32_t divisor = 1;
  while (p-s.c_str() < s_len) {
    if (*p >= 'A' && *p <= 'Z') {
      divisor *= 24;
      f_long = f_long + (*p-'A')/(float ) divisor;
    } else {
      divisor *= 10;
      f_long = f_long + (p[0]-'0')/(float ) divisor;
    }
    p = p+2;
  }
  f_long = f_long *2.0;

  p = s.c_str() + 1;
  f_lat = (-9 + p[0]-'A') * 10 + p[2]-'0';
  p = p+4;
  divisor = 1;
  while (p-s.c_str() < s_len) {
    if (*p >= 'A' && *p <= 'Z') {
      divisor *= 24;
      f_lat = f_lat + (*p-'A')/(float ) divisor;
    } else {
      divisor *= 10;
      f_lat = f_lat + (*p-'0')/(float ) divisor;
    }
    p = p+2;
  }
  store_lat_long(f_lat, f_long);
}


void set_lat_long(String s_lat, String s_long) {
  /*  Valid notations:
    53 14 59 N
    53 14 59.1 N
   -53 14 59
    53 15
    53 14.983
    53-14.983
    53-14.983 N
    53-14,983 N
    53° 14.983
    53° 14.983'
    53° 14' 59"
    53° 14' 59.999"
    53.2500
    5314.98 // aprs notation. pos value 2-3 < 60.
    JO62QN10

    Invalid notations:
    -53 14 59 N
    5375 // -> No, not 53° 45".
    53 -14.0 N
    53.25.01 N
    53,25.01 N
    91 N
    537 N (-> is it 53° 7min or 5° 37min ?)
    1301 E (deg in aprs notation is always len 3 (and 0<=x<180))
    3194.98 // deg*60 + minutes == (53 *60 + 14.983)
    5399.99 N
    185 E (instead of writing 175 W)
    11h55min ;)
  */

  float f_lat = 0.0f;
  float f_long = 0.0f;
  int curr_long = 0;

  // If notation min ' sec "" is used, then sec has to follow min
  if (s_lat.indexOf("\"") > -1 && (s_lat.indexOf("\"") == -1 || s_lat.indexOf("\"") < s_lat.indexOf("'"))) goto out;
  if (s_long.indexOf("\"") > -1 && (s_long.indexOf("\"") == -1 || s_long.indexOf("\"") < s_long.indexOf("'"))) goto out;
  s_lat.toUpperCase(); s_lat.trim(); s_lat.replace("DEG", "-"); s_lat.replace("'", ""); s_lat.replace("\"", "");
  s_long.toUpperCase(); s_long.trim(); s_long.replace("DEG", "-"); s_long.replace("'", ""); s_long.replace("\"", "");

  if (s_long.isEmpty()) {
    if (s_lat.isEmpty())
      goto out;
    if (is_locator(s_lat)) {
      compute_locator(s_lat);
      return;
    }
  } else {
    if (is_locator(s_long)) {
      compute_locator(s_long);
      return;
    }
  }

  char buf[64];
  for (curr_long = 0; curr_long < 2; curr_long++) {
    float f_degree = 0.0f;
    boolean is_negative = false;
    strncpy(buf, (curr_long == 0) ? s_lat.c_str() : s_long.c_str(), sizeof(buf)-1);
    buf[sizeof(buf)-1] = 0;
    char *p = buf;
    char c = p[strlen(p)-1];
    char *q;

    if (c >= '0' && c <= '9') {
      if (*p == '-') {
        is_negative = 1;
        p = p+1;
        while (*p && *p == ' ') p++;
      }
    } else if ((curr_long == 0 && (c == 'N' || c == 'S')) || (curr_long == 1 && (c == 'E' || c == 'W'))) {
      if (c == 'S' || c == 'W')
        is_negative = 1;
      buf[strlen(buf)-1] = 0;
      q = buf + strlen(buf)-1;
      while(q > p && *q == ' ')
        q--;
    } else goto out;

    if (!*p) goto out;
    // Notations like position 53-02.13 N
    if ((q = strchr(p, '-')))
      *q = ' ';

    if (!isdigit(*p))
      goto out;

    if ((q = strchr(p, '.')) || (q = strchr(p, ','))) {
      *q++ = '.';
      while (*q && isdigit(*q))
        q++;
      *q = 0;
    }

    if ((q = strchr(p, ' '))) {
      float f_minute = 0.0f;
      while (*q && *q == ' ')
        *q++ = 0;
      char *r = strchr(q, ' ');
      if (r) {
        while (*r && *r == ' ')
          *r++ = 0;
        if (*r) {
          char *s = strchr(r, ' ');
          if (s) *s = 0;
          float f_seconds = atof(r);
          if (f_seconds < 0 || f_seconds >= 60)
            goto out;
          f_minute = f_seconds / 60.0;
        }
      }
      f_minute = atof(q) + f_minute;
      if (f_minute > 60.0)
        goto out;
      f_degree = f_minute / 60.0;
    } else {
      // dd.nnnn, or aprs notation ddmm.nn (latt) / dddmm.nn (long)
      if (strlen(p) == (curr_long ? 8 : 7)) {
        q = p + (curr_long ? 3 : 2);
        if (q[2] == '.') {
          // atof of mm.nn. dd part comes later
          f_degree = atof(q) / 60.0;
          if (f_degree >= 1.0)
            goto out;
          // cut after dd
          *q = 0;
        }
      }
    }
    f_degree = atof(p) + f_degree;

    if (f_degree < 0 || f_degree > (curr_long == 1 ? 180 : 90))
      goto out;

    if (is_negative) f_degree *= -1;
    if (curr_long == 0)
      f_lat = f_degree;
    else
      f_long = f_degree;
  }

out:
  store_lat_long(f_lat, f_long);
}

void handle_SaveAPRSCfg() {
  // LoRa settings
  if (server.hasArg(PREF_LORA_FREQ_PRESET)){
    preferences.putDouble(PREF_LORA_FREQ_PRESET, server.arg(PREF_LORA_FREQ_PRESET).toDouble());
    Serial.printf("FREQ saved:\t%f\n", server.arg(PREF_LORA_FREQ_PRESET).toDouble());
  }
  if (server.hasArg(PREF_LORA_SPEED_PRESET)){
    preferences.putInt(PREF_LORA_SPEED_PRESET, server.arg(PREF_LORA_SPEED_PRESET).toInt());
  }
  preferences.putBool(PREF_LORA_RX_ENABLE, server.hasArg(PREF_LORA_RX_ENABLE));
  preferences.putBool(PREF_LORA_TX_ENABLE, server.hasArg(PREF_LORA_TX_ENABLE));
  if (server.hasArg(PREF_LORA_TX_POWER)) {
    preferences.putInt(PREF_LORA_TX_POWER, server.arg(PREF_LORA_TX_POWER).toInt());
  }
  preferences.putBool(PREF_LORA_AUTOMATIC_CR_ADAPTION_PRESET, server.hasArg(PREF_LORA_AUTOMATIC_CR_ADAPTION_PRESET));
  if (server.hasArg(PREF_LORA_ADD_SNR_RSSI_TO_PATH_PRESET)){
    preferences.putInt(PREF_LORA_ADD_SNR_RSSI_TO_PATH_PRESET, server.arg(PREF_LORA_ADD_SNR_RSSI_TO_PATH_PRESET).toInt());
  }
  preferences.putBool(PREF_LORA_ADD_SNR_RSSI_TO_PATH_END_AT_KISS_PRESET, server.hasArg(PREF_LORA_ADD_SNR_RSSI_TO_PATH_END_AT_KISS_PRESET));
  if (server.hasArg(PREF_APRS_DIGIPEATING_MODE_PRESET)){
    preferences.putInt(PREF_APRS_DIGIPEATING_MODE_PRESET, server.arg(PREF_APRS_DIGIPEATING_MODE_PRESET).toInt());
  }
  if (server.hasArg(PREF_APRS_CROSS_DIGIPEATING_MODE_PRESET)){
    preferences.putInt(PREF_APRS_CROSS_DIGIPEATING_MODE_PRESET, server.arg(PREF_APRS_CROSS_DIGIPEATING_MODE_PRESET).toInt());
  }
  if (server.hasArg(PREF_LORA_TX_BEACON_AND_KISS_TO_FREQUENCIES_PRESET)) {
    preferences.putInt(PREF_LORA_TX_BEACON_AND_KISS_TO_FREQUENCIES_PRESET, server.arg(PREF_LORA_TX_BEACON_AND_KISS_TO_FREQUENCIES_PRESET).toInt());
  }
  preferences.putBool(PREF_LORA_TX_BEACON_AND_KISS_TO_APRSIS_PRESET, server.hasArg(PREF_LORA_TX_BEACON_AND_KISS_TO_APRSIS_PRESET));
  if (server.hasArg(PREF_LORA_FREQ_CROSSDIGI_PRESET)){
    preferences.putDouble(PREF_LORA_FREQ_CROSSDIGI_PRESET, server.arg(PREF_LORA_FREQ_CROSSDIGI_PRESET).toDouble());
    Serial.printf("FREQ crossdigi saved:\t%f\n", server.arg(PREF_LORA_FREQ_CROSSDIGI_PRESET).toDouble());
  }
  if (server.hasArg(PREF_LORA_SPEED_CROSSDIGI_PRESET)){
    preferences.putInt(PREF_LORA_SPEED_CROSSDIGI_PRESET, server.arg(PREF_LORA_SPEED_CROSSDIGI_PRESET).toInt());
  }
  if (server.hasArg(PREF_LORA_TX_POWER_CROSSDIGI_PRESET)) {
    preferences.putInt(PREF_LORA_TX_POWER_CROSSDIGI_PRESET, server.arg(PREF_LORA_TX_POWER_CROSSDIGI_PRESET).toInt());
  }
  if (server.hasArg(PREF_LORA_RX_ON_FREQUENCIES_PRESET)) {
    preferences.putInt(PREF_LORA_RX_ON_FREQUENCIES_PRESET, server.arg(PREF_LORA_RX_ON_FREQUENCIES_PRESET).toInt());
  }
  // APRS station settings
  if (server.hasArg(PREF_APRS_CALLSIGN) && !server.arg(PREF_APRS_CALLSIGN).isEmpty()){
    String s = server.arg(PREF_APRS_CALLSIGN); s.trim();
    preferences.putString(PREF_APRS_CALLSIGN, s);
  }
  if (server.hasArg(PREF_APRS_SYMBOL_TABLE) && !server.arg(PREF_APRS_SYMBOL_TABLE).isEmpty()){
    preferences.putString(PREF_APRS_SYMBOL_TABLE, server.arg(PREF_APRS_SYMBOL_TABLE));
  }
  if (server.hasArg(PREF_APRS_SYMBOL) && !server.arg(PREF_APRS_SYMBOL).isEmpty()){
    preferences.putString(PREF_APRS_SYMBOL, server.arg(PREF_APRS_SYMBOL));
  }
  if (server.hasArg(PREF_APRS_RELAY_PATH)){
    String s = server.arg(PREF_APRS_RELAY_PATH);
    s.toUpperCase(); s.trim(); s.replace(" ", ","); s.replace(",,", ",");
    if (s.endsWith("WIDE1") || s.endsWith("WIDE2")) s = s + "-1";
    if (s.indexOf("WIDE1,") > -1) s.replace("WIDE1,", "WIDE1-1,");
    if (s.indexOf("WIDE2,") > -1) s.replace("WIDE2,", "WIDE2-1,");
    // Avoid wrong paths like WIDE2-1,WIDE1-1
    if (! ( s.indexOf("WIDE1") > s.indexOf("WIDE") || s.indexOf("WIDE-") > -1 || s.startsWith("RFONLY,WIDE") || s.startsWith("NOGATE,WIDE") || s.indexOf("*") > -1 ))
      preferences.putString(PREF_APRS_RELAY_PATH, s);
  }
  if (server.hasArg(PREF_APRS_COMMENT)){
    preferences.putString(PREF_APRS_COMMENT, server.arg(PREF_APRS_COMMENT));
  }
  set_lat_long(server.hasArg(PREF_APRS_LATITUDE_PRESET) ? server.arg(PREF_APRS_LATITUDE_PRESET) : String(""), server.hasArg(PREF_APRS_LONGITUDE_PRESET) ? server.arg(PREF_APRS_LONGITUDE_PRESET) : String(""));
  //if (server.hasArg(PREF_APRS_LATITUDE_PRESET)){
    ////preferences.putString(PREF_APRS_LATITUDE_PRESET, server.arg(PREF_APRS_LATITUDE_PRESET));
    //String s_lat = latORlon(server.arg(PREF_APRS_LATITUDE_PRESET), 0);
    //if (s_lat.length() == 8)
      //preferences.putString(PREF_APRS_LATITUDE_PRESET, s_lat);
  //}
  //if (server.hasArg(PREF_APRS_LONGITUDE_PRESET)){
    ////preferences.putString(PREF_APRS_LONGITUDE_PRESET, server.arg(PREF_APRS_LONGITUDE_PRESET));
    //String s_lon = latORlon(server.arg(PREF_APRS_LONGITUDE_PRESET), 1);
    //if (s_lon.length() == 9)
      //preferences.putString(PREF_APRS_LONGITUDE_PRESET, s_lon);
  //}
  if (server.hasArg(PREF_APRS_SENDER_BLACKLIST)){
    String s = server.arg(PREF_APRS_SENDER_BLACKLIST);
    s.toUpperCase(); s.trim(); s.replace(" ", ","); s.replace(",,", ",");
    preferences.putString(PREF_APRS_SENDER_BLACKLIST, (s.isEmpty() || s == ",") ? "" : s);
  }
  if (server.hasArg(PREF_TNC_SELF_TELEMETRY_INTERVAL)){
    preferences.putInt(PREF_TNC_SELF_TELEMETRY_INTERVAL, server.arg(PREF_TNC_SELF_TELEMETRY_INTERVAL).toInt());
  }
  if (server.hasArg(PREF_TNC_SELF_TELEMETRY_MIC)){
    preferences.putInt(PREF_TNC_SELF_TELEMETRY_MIC, server.arg(PREF_TNC_SELF_TELEMETRY_MIC).toInt());
  }
  if (server.hasArg(PREF_TNC_SELF_TELEMETRY_PATH)){
    String s = server.arg(PREF_TNC_SELF_TELEMETRY_PATH);
    s.toUpperCase(); s.trim(); s.replace(" ", ","); s.replace(",,", ",");
    preferences.putString(PREF_TNC_SELF_TELEMETRY_PATH, s);
  }

  // Smart Beaconing settings 
  if (server.hasArg(PREF_APRS_FIXED_BEACON_INTERVAL_PRESET)){
    preferences.putInt(PREF_APRS_FIXED_BEACON_INTERVAL_PRESET, server.arg(PREF_APRS_FIXED_BEACON_INTERVAL_PRESET).toInt());
  }
  if (server.hasArg(PREF_APRS_SB_MIN_INTERVAL_PRESET)){
    preferences.putInt(PREF_APRS_SB_MIN_INTERVAL_PRESET, server.arg(PREF_APRS_SB_MIN_INTERVAL_PRESET).toInt());
  }
  if (server.hasArg(PREF_APRS_SB_MAX_INTERVAL_PRESET)){
    //preferences.putInt(PREF_APRS_SB_MAX_INTERVAL_PRESET, server.arg(PREF_APRS_SB_MAX_INTERVAL_PRESET).toInt());
    int i = server.arg(PREF_APRS_SB_MAX_INTERVAL_PRESET).toInt();
    if (i <= preferences.getInt(PREF_APRS_SB_MIN_INTERVAL_PRESET)) i = preferences.getInt(PREF_APRS_SB_MIN_INTERVAL_PRESET) +1;
    preferences.putInt(PREF_APRS_SB_MAX_INTERVAL_PRESET, i);
  }
  if (server.hasArg(PREF_APRS_SB_MIN_SPEED_PRESET)){
    preferences.putInt(PREF_APRS_SB_MIN_SPEED_PRESET, server.arg(PREF_APRS_SB_MIN_SPEED_PRESET).toInt());
  }
  if (server.hasArg(PREF_APRS_SB_MAX_SPEED_PRESET)){
    //preferences.putInt(PREF_APRS_SB_MAX_SPEED_PRESET, server.arg(PREF_APRS_SB_MAX_SPEED_PRESET).toInt());
    int i = server.arg(PREF_APRS_SB_MAX_SPEED_PRESET).toInt();
    if (i <= preferences.getInt(PREF_APRS_SB_MIN_SPEED_PRESET)) i = preferences.getInt(PREF_APRS_SB_MIN_SPEED_PRESET) +1;
    preferences.putInt(PREF_APRS_SB_MAX_SPEED_PRESET, i);
  }
  if (server.hasArg(PREF_APRS_SB_ANGLE_PRESET)){
    preferences.putDouble(PREF_APRS_SB_ANGLE_PRESET, server.arg(PREF_APRS_SB_ANGLE_PRESET).toDouble());
  }
  if (server.hasArg(PREF_APRS_SB_TURN_SLOPE_PRESET)){
    preferences.putInt(PREF_APRS_SB_TURN_SLOPE_PRESET, server.arg(PREF_APRS_SB_TURN_SLOPE_PRESET).toInt());
  }
  if (server.hasArg(PREF_APRS_SB_TURN_TIME_PRESET)){
    preferences.putInt(PREF_APRS_SB_TURN_TIME_PRESET, server.arg(PREF_APRS_SB_TURN_TIME_PRESET).toInt());
  }
 
  preferences.putBool(PREF_APRSIS_EN, server.hasArg(PREF_APRSIS_EN));
  if (server.hasArg(PREF_APRSIS_SERVER_NAME)){
    String s = server.arg(PREF_APRSIS_SERVER_NAME);
    preferences.putString(PREF_APRSIS_SERVER_NAME, s);
  }
  if (server.hasArg(PREF_APRSIS_SERVER_PORT)){
    preferences.putInt(PREF_APRSIS_SERVER_PORT, server.arg(PREF_APRSIS_SERVER_PORT).toInt());
  }
  if (server.hasArg(PREF_APRSIS_FILTER)){
    String s = server.arg(PREF_APRSIS_FILTER);
    s.trim();
    preferences.putString(PREF_APRSIS_FILTER, s);
  }
  if (server.hasArg(PREF_APRSIS_CALLSIGN)){
    String s = server.arg(PREF_APRSIS_CALLSIGN);
    s.toUpperCase(); s.trim();
    preferences.putString(PREF_APRSIS_CALLSIGN, s);
  }
  if (server.hasArg(PREF_APRSIS_PASSWORD)){
    String s = server.arg(PREF_APRSIS_PASSWORD);
    s.trim();
    preferences.putString(PREF_APRSIS_PASSWORD, s);
  }
  if (server.hasArg(PREF_APRSIS_ALLOW_INET_TO_RF)){
    preferences.putInt(PREF_APRSIS_ALLOW_INET_TO_RF, server.arg(PREF_APRSIS_ALLOW_INET_TO_RF).toInt());
  }

  preferences.putBool(PREF_APRS_SHOW_BATTERY, server.hasArg(PREF_APRS_SHOW_BATTERY));
  preferences.putBool(PREF_ENABLE_TNC_SELF_TELEMETRY, server.hasArg(PREF_ENABLE_TNC_SELF_TELEMETRY));
  //preferences.putBool(PREF_APRS_SHOW_ALTITUDE, server.hasArg(PREF_APRS_SHOW_ALTITUDE));
  preferences.putBool(PREF_APRS_SHOW_ALTITUDE_INSIDE_COMPRESSED_POSITION, server.hasArg(PREF_APRS_SHOW_ALTITUDE_INSIDE_COMPRESSED_POSITION));
  if (server.hasArg(PREF_APRS_ALTITUDE_RATIO)){
    preferences.putInt(PREF_APRS_ALTITUDE_RATIO, server.arg(PREF_APRS_ALTITUDE_RATIO).toInt());
  }
  preferences.putBool(PREF_APRS_FIXED_BEACON_PRESET, server.hasArg(PREF_APRS_FIXED_BEACON_PRESET));
  preferences.putBool(PREF_APRS_GPS_EN, server.hasArg(PREF_APRS_GPS_EN));
  preferences.putBool(PREF_ACCEPT_OWN_POSITION_REPORTS_VIA_KISS, server.hasArg(PREF_ACCEPT_OWN_POSITION_REPORTS_VIA_KISS));
  preferences.putBool(PREF_GPS_ALLOW_SLEEP_WHILE_KISS, server.hasArg(PREF_GPS_ALLOW_SLEEP_WHILE_KISS));
  preferences.putBool(PREF_APRS_SHOW_CMT, server.hasArg(PREF_APRS_SHOW_CMT));
  preferences.putBool(PREF_APRS_COMMENT_RATELIMIT_PRESET, server.hasArg(PREF_APRS_COMMENT_RATELIMIT_PRESET));

  server.sendHeader("Location", "/");
  server.send(302,"text/html", "");
  
}

void handle_saveDeviceCfg(){
  preferences.putBool(PREF_DEV_BT_EN, server.hasArg(PREF_DEV_BT_EN));
  preferences.putBool(PREF_DEV_OL_EN, server.hasArg(PREF_DEV_OL_EN));
  if (server.hasArg(PREF_DEV_SHOW_RX_TIME)){
    preferences.putInt(PREF_DEV_SHOW_RX_TIME, server.arg(PREF_DEV_SHOW_RX_TIME).toInt());
  }
  // Manage OLED Timeout
  if (server.hasArg(PREF_DEV_SHOW_OLED_TIME)){
    preferences.putInt(PREF_DEV_SHOW_OLED_TIME, server.arg(PREF_DEV_SHOW_OLED_TIME).toInt());
  }
  preferences.putBool(PREF_DEV_AUTO_SHUT, server.hasArg(PREF_DEV_AUTO_SHUT));
  if (server.hasArg(PREF_DEV_AUTO_SHUT_PRESET)){
    preferences.putInt(PREF_DEV_AUTO_SHUT_PRESET, server.arg(PREF_DEV_AUTO_SHUT_PRESET).toInt());
  } 
  if (server.hasArg(PREF_DEV_REBOOT_INTERVAL)){
    preferences.putInt(PREF_DEV_REBOOT_INTERVAL, server.arg(PREF_DEV_REBOOT_INTERVAL).toInt());
  }
  if (server.hasArg(PREF_DEV_CPU_FREQ)){
    uint8_t cpufreq = server.arg(PREF_DEV_CPU_FREQ).toInt();
    if (cpufreq != 0 && cpufreq < 10)
      cpufreq = 10;
    preferences.putInt(PREF_DEV_CPU_FREQ, cpufreq);
  }
  server.sendHeader("Location", "/");
  server.send(302,"text/html", "");
}

[[noreturn]] void taskWebServer(void *parameter) {
  auto *webServerCfg = (tWebServerCfg*)parameter;
  apSSID = webServerCfg->callsign + " AP";

  server.on("/", handle_Index);
  server.on("/favicon.ico", handle_NotFound);
  server.on("/style.css", handle_Style);
  server.on("/js.js", handle_Js);
  server.on("/scan_wifi", handle_ScanWifi);
  server.on("/save_wifi_cfg", handle_SaveWifiCfg);
  server.on("/reboot", handle_Reboot);
  server.on("/beacon", handle_Beacon);
  server.on("/shutdown", handle_Shutdown);
  server.on("/cfg", handle_Cfg);
  server.on("/received_list", handle_ReceivedList);
  server.on("/save_aprs_cfg", handle_SaveAPRSCfg);
  server.on("/save_device_cfg", handle_saveDeviceCfg);
  server.on("/restore", handle_Restore);
  server.on("/update", HTTP_POST, []() {
    syslog_log(LOG_WARNING, String("Update finished. Status: ") + (!Update.hasError() ? "Ok" : "Error"));
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    delay(500);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      rf95.sleep(); // disable rf95 before update
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        syslog_log(LOG_ERR, String("Update begin error: ") + Update.errorString());
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        syslog_log(LOG_ERR, String("Update error: ") + Update.errorString());
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
        syslog_log(LOG_WARNING, String("Update Success: ") + String((int)upload.totalSize));
      } else {
        syslog_log(LOG_ERR, String("Update error: ") + Update.errorString());
        Update.printError(Serial);
      }
    }
  });
  server.onNotFound(handle_NotFound);

  String wifi_password = preferences.getString(PREF_WIFI_PASSWORD, "");
  String wifi_ssid = preferences.getString(PREF_WIFI_SSID, "");
  apPassword = preferences.getString(PREF_AP_PASSWORD, "");
  // 8 characters is requirements for WPA2
  if (apPassword.length() < 8) {
    apPassword = defApPassword;
  }
  if (!wifi_ssid.length()){
    WiFi.softAP(apSSID.c_str(), apPassword.c_str());
    esp_wifi_set_max_tx_power(wifi_txpwr_mode_AP);
  } else {
    int retryWifi = 0;
    WiFi.begin(wifi_ssid.c_str(), wifi_password.length() ? wifi_password.c_str() : nullptr);
    Serial.println("Connecting to " + wifi_ssid);
    // Set power:  minimum 8 (2dBm) (max 80 (20dBm))
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html
    // Mapping Table {Power, max_tx_power} = {{8, 2}, {20, 5}, {28, 7}, {34, 8}, {44, 11}, {52, 13}, {56, 14}, {60, 15}, {66, 16}, {72, 18}, {80, 20}}.
    // We'll use "min", "low", "mid", "high", "max" -> 2dBm (1.5mW) -> 8, 11dBm (12mW) -> 44, 15dBm (32mW) -> 60, 18dBm (63mW) ->72, 20dBm (100mW) ->80
    esp_wifi_set_max_tx_power(wifi_txpwr_mode_STA);
    while (WiFi.status() != WL_CONNECTED) {
      Serial.print("Not connected: ");
      Serial.println((int)WiFi.status());
      Serial.print("Retry: ");
      Serial.println(retryWifi);
      vTaskDelay(500/portTICK_PERIOD_MS);
      retryWifi += 1;
      if (retryWifi > 60) {
        WiFi.softAP(apSSID.c_str(), apPassword.c_str());
        //WiFi.softAP(apSSID.c_str(), "password");
        Serial.println("Unable to connect to to wifi. Starting AP");
        Serial.print("SSID: ");
        Serial.print(apSSID.c_str());
        Serial.print(" Password: ");
        Serial.println(apPassword.c_str());
        esp_wifi_set_max_tx_power(wifi_txpwr_mode_AP);
        break;
      }
    }

    //Serial.print("WiFi Mode: ");
    //Serial.println(WiFi.getMode());
    if (WiFi.getMode() == 3){
      Serial.println("Running AP. IP: " + WiFi.softAPIP().toString());
      apEnabled=true;
      infoApName = apSSID.c_str();
      infoApPass = apSSID.c_str();
      infoApAddr = WiFi.softAPIP().toString();
    } else if (WiFi.getMode() == 1) {
      // Save some battery
      //WiFi.setSleep(true);
      esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
      Serial.println("Connected. IP: " + WiFi.localIP().toString());
      apConnected=true;
      infoApName = wifi_ssid.c_str();
      infoApPass = wifi_password.c_str();
      infoApAddr = WiFi.localIP().toString();
    } else {
      Serial.println("WiFi Mode: " + WiFi.getMode());
    }

    #ifdef ENABLE_SYSLOG
      syslog.server(SYSLOG_IP, 514);
      syslog.deviceHostname(webServerCfg->callsign.c_str());
      syslog.appName("TTGO");
      syslog.defaultPriority(LOG_KERN);
      syslog_log(LOG_INFO, "Connected. IP: " + WiFi.localIP().toString());
    #endif
    String ntp_server = preferences.getString(PREF_NTP_SERVER, "");
    if (ntp_server.isEmpty()) {
      if (infoApAddr.startsWith("44."))
        ntp_server = "ntp.hc.r1.ampr.org";
      else
        ntp_server = "pool.ntp.org";
    }
    configTime(0, 0, ntp_server.c_str());
    #ifdef ENABLE_SYSLOG
      struct tm timeinfo{};
      if(!getLocalTime(&timeinfo)){
        syslog_log(LOG_WARNING, "Failed to obtain time");
      } else {
        char buf[64];
        strftime(buf, 64, "%A, %B %d %Y %H:%M:%S", &timeinfo);
        syslog_log(LOG_INFO, String("Time: ") + String(buf));
      }
    #endif
  }

  server.begin();
  #ifdef KISS_PROTOCOL
    if (tncServer_enabled)
      tncServer.begin();
  #endif
  if (gpsServer_enabled)
    gpsServer.begin();
  if (MDNS.begin(webServerCfg->callsign.c_str())) {
    MDNS.setInstanceName(webServerCfg->callsign + " TTGO LoRa APRS TNC " + TXFREQ + "MHz");
    MDNS.addService("http", "tcp", 80);
    #ifdef KISS_PROTOCOL
      MDNS.addService("kiss-tnc", "tcp", NETWORK_TNC_PORT);
    #endif
  }

  webListReceivedQueue = xQueueCreate(4,sizeof(tReceivedPacketData *));


  tReceivedPacketData *receivedPacketData = nullptr;

  WiFiClient aprs_is_client;

  uint32_t t_connect_apprsis_again = 0L;
  String aprs_callsign = webServerCfg->callsign;
  aprsis_host.trim();
  aprsis_filter.trim();
  aprsis_password.trim();
  if (aprsis_callsign.length() < 3 || aprsis_callsign.length() > 9)
    aprsis_callsign = "";
  if (aprsis_callsign.isEmpty()) aprsis_callsign = aprs_callsign;
  if (aprsis_callsign.length() < 3 || aprsis_callsign.length() > 9)
    aprsis_callsign = "";
  aprsis_callsign.toUpperCase(); aprsis_callsign.trim();

  // sanity check
  if (aprsis_enabled) {
    if (aprsis_callsign.isEmpty() || aprsis_host.isEmpty() || aprsis_port == 0) {
      aprsis_enabled = false;
   } else {
      const char *p = aprsis_callsign.c_str();
      for (; *p && aprsis_enabled; p++) {
        if (*p == '-') {
          int len = strlen(p+1);
          if (len < 1 || len > 2)
            aprsis_enabled = false;
        } else if ( !((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) )
            aprsis_enabled = false;
      }
    }
  }

  esp_task_wdt_init(120, true); //enable panic so ESP32 restarts
  esp_task_wdt_add(NULL); //add current thread to WDT watch

  while (true){
    esp_task_wdt_reset();

    // Mode STA and connection lost? -> reconnect
    if (WiFi.getMode() == 1 && WiFi.status() != WL_CONNECTED) {
      static uint32_t last_connection_attempt = millis();
      if (millis() - last_connection_attempt > 20000L) {
        WiFi.disconnect();
        esp_task_wdt_reset();
        WiFi.reconnect();
        esp_task_wdt_reset();
        last_connection_attempt = millis();
      }
      if (WiFi.status() != WL_CONNECTED) {
         // 500ms for reconnect should be enough, ant not too often (power consumption).. Or, if we did not try to reconnect, this value is also fine
         delay(500);
         continue;
      }
    }

    server.handleClient();
    if (xQueueReceive(webListReceivedQueue, &receivedPacketData, (1 / portTICK_PERIOD_MS)) == pdPASS) {
      auto *receivedPacketToQueue = new tReceivedPacketData();
      receivedPacketToQueue->packet = new String();
      receivedPacketToQueue->packet->concat(*receivedPacketData->packet);
      receivedPacketToQueue->RSSI = receivedPacketData->RSSI;
      receivedPacketToQueue->SNR = receivedPacketData->SNR;
      receivedPacketToQueue->rxTime = receivedPacketData->rxTime;
      receivedPackets.push_back(receivedPacketToQueue);
      if (receivedPackets.size() > MAX_RECEIVED_LIST_SIZE){
        auto *packetDataToDelete = receivedPackets.front();
        delete packetDataToDelete->packet;
        delete packetDataToDelete;
        receivedPackets.pop_front();
      }
      delete receivedPacketData->packet;
      delete receivedPacketData;
    }


    if (aprsis_enabled) {
      boolean err = true;
      if (WiFi.getMode() == 1) {
        if (WiFi.status() != WL_CONNECTED) { aprsis_status = "Error: no internet"; goto on_err; } else { if (aprsis_status == "Error: no internet") aprsis_status = "Internet available"; }
        if (!aprs_is_client.connected() && t_connect_apprsis_again < millis()) {
          aprsis_status = "Connecting";
          aprs_is_client.connect(aprsis_host.c_str(), aprsis_port);
          if (!aprs_is_client.connected()) { aprsis_status = "Error: connect failed"; goto on_err; }
          aprsis_status = "Connected. Waiting for greeting.";
          uint32_t t_start = millis();
          while (!aprs_is_client.available() && (millis()-t_start) < 25000) delay(100);
          if (aprs_is_client.available()) {
	    // check 
	    String s = aprs_is_client.readStringUntil('\n');
	    if (s.isEmpty() || !s.startsWith("#")) { aprsis_status = "Error: unexpected greeting"; goto on_err; }
	  } else { aprsis_status = "Error: No response"; goto on_err; }
          aprsis_status = "Login";
	  char buffer[1024];
	  sprintf(buffer, "user %s pass %s TTGO-T-Beam-LoRa-APRS 0.1%s%s\r\n", aprsis_callsign.c_str(), aprsis_password.c_str(), aprsis_filter.isEmpty() ? "" : " filter ", aprsis_filter.isEmpty() ? "" :  aprsis_filter.c_str());
          aprs_is_client.print(String(buffer));
          t_start = millis();
          while (!aprs_is_client.available() && (millis()-t_start) < 25000) delay(100);
          aprsis_status = "Logged in";
          if (aprs_is_client.available()) {
	    // check 
	    String s = aprs_is_client.readStringUntil('\n');
	    if (s.isEmpty() || !s.startsWith("#")) { aprsis_status = "Error: unexpected reponse on login"; goto on_err; }
	    if (s.indexOf(" logresp") == -1) { aprsis_status = "Error: Login denied: " + s; aprsis_status.trim(); goto on_err; }
	    if (s.indexOf(" verified") == -1) { aprsis_status = "Notice: server responsed not verified: " + s; aprsis_status.trim(); }
	  } else { aprsis_status = "Error: No response"; goto on_err; }
        }
        if (!aprs_is_client.connected())
          goto on_err;
        //aprsis_status = "OK";
        if (aprs_is_client.available()) {
          //aprsis_status = "OK, reading";
          String s = aprs_is_client.readStringUntil('\n');
          if (!s) goto on_err;
          //aprsis_status = "OK";
          s.trim();
          if (s.isEmpty()) goto on_err;
	  if (*(s.c_str()) == '#' || !isalnum(*(s.c_str()))) goto do_not_send;
	  char *header_end = strchr(s.c_str(), ':');
	  if (!header_end) goto do_not_send;
	  char *src_call_end = strchr(s.c_str(), '>');
	  if (!src_call_end) goto do_not_send;
	  if (src_call_end > header_end-2) goto do_not_send;
	  char *q = strchr(s.c_str(), '-');
	  if (q && q < src_call_end) {
	    // len callsign > 6?
	    if (q-s.c_str() > 6) goto do_not_send;
	    // SSID optional, only 0..15
	    if (q[2] == '>') {
	      if (q[1] < '0' || q[1] > '9') goto do_not_send;
	    } else if (q[3] == '>') {
	      if (q[1] != '1' || q[2] < '0' || q[2] > '5') goto do_not_send;
	    } else goto do_not_send;
	  } else {
	    if (src_call_end-s.c_str() > 6) goto do_not_send;
	  }

	  // do not interprete packets coming back from aprs-is net (either our source call, or if we have repeated it with one of our calls
	  // sender is our call?
          if (s.startsWith(aprs_callsign + '>') || s.startsWith(aprsis_callsign + '>')) goto do_not_send;

	  // packet has our call in i.E. ...,qAR,OURCALL:...
	  q = strstr(s.c_str(), (',' + aprsis_callsign + ':').c_str());
	  if (q && q < header_end) goto do_not_send;
	  for (int i = 0; i < 2; i++) {
	    String call = (i == 0 ? aprs_callsign : aprsis_callsign);
		// digipeated frames look like "..,DL9SAU,DL1AAA,DL1BBB*,...", or "..,DL9SAU*,DL1AAA,DL1BBB,.."
	    if (((q = strstr(s.c_str(), (',' + call + '*').c_str())) || (q = strstr(s.c_str(), (',' + call + ',').c_str()))) && q < header_end) {
              char *digipeatedflag = strchr(q, '*');
	      if (digipeatedflag && digipeatedflag < header_end && digipeatedflag > q)
	        goto do_not_send;
	    }
	  }
	  // generate third party packet. Use aprs_callsign (deriving from webServerCfg->callsign), because aprsis_callsign may have a non-aprs (but only aprsis-compatible) ssid like '-L4'
          String third_party_packet = generate_third_party_packet(aprs_callsign, s);
          if (!third_party_packet.isEmpty()) {
            aprsis_status = "OK, fromAPRSIS: " + s + " => " + third_party_packet; aprsis_status.trim();
#ifdef KISS_PROTOCOL
            sendToTNC(third_party_packet);
#endif
            if (lora_tx_enabled && aprsis_data_allow_inet_to_rf) {
	      // not query or aprs-message addressed to our call (check both, aprs_callsign and aprsis_callsign)=
	      // Format: "..::DL9SAU-15:..."
	      // check is in this code part, because we may like to see those packets via kiss (sent above)
	      q = header_end + 1;
	      if (*q == ':' && strlen(q) > 10 && q[10] == ':' &&
                  ((!strncmp(q+1, aprs_callsign.c_str(), aprs_callsign.length()) && (aprs_callsign.length() == 9 || q[9] == ' ')) ||
                  (!strncmp(q+1, aprsis_callsign.c_str(), aprsis_callsign.length()) && (aprsis_callsign.length() == 9 || q[9] == ' ')) ))
	        goto do_not_send;
              if (aprsis_data_allow_inet_to_rf % 2)
                loraSend(txPower, lora_freq, lora_speed, third_party_packet);
              if (aprsis_data_allow_inet_to_rf > 1 && lora_freq_cross_digi > 1.0 && lora_freq_cross_digi != lora_freq)
                loraSend(txPower_cross_digi, lora_freq_cross_digi, lora_speed_cross_digi, third_party_packet);
            }
          }
        }
do_not_send:
        if (to_aprsis_data) {
	  // copy(). We are threaded..
	  String data = String(to_aprsis_data);
	  // clear queue
          to_aprsis_data = "";
          data.trim();
          char *p = strchr(data.c_str(), '>');
          char *q;
          // some plausibility checks.
          if (p && p > data.c_str() && (q = strchr(p+1, ':'))) {
            // Due to http://www.aprs-is.net/IGateDetails.aspx , never gate third-party traffic contining TCPIP or TCPXX
            // IGATECALL>APRS,GATEPATH:}FROMCALL>TOCALL,TCPIP,IGATECALL*:original packet data
            char *r; char *s;
            if (!(q[1] == '}' && (r = strchr(q+2, '>')) && ((s = strstr(r+1, ",TCPIP,")) || (s = strstr(r+1, ",TCPXX,"))) && strstr(s+6, "*:"))) {
	      char buf[256];
	      int len = (q-data.c_str());
	      if (len > 0 && len < sizeof(buf)) {
	        strncpy(buf, data.c_str(), len);
                buf[len] = 0;
		String s_data = String(buf) + (lora_tx_enabled ? ",qAR," : ",qAO,") + aprsis_callsign + q + "\r\n";
                aprsis_status = "OK, toAPRSIS: " + s_data; aprsis_status.trim();
                aprs_is_client.print(s_data);
	      }
            }
          }
          //aprsis_status = "OK";
        }
        err = false;

        if (err) {
on_err:
	  aprs_is_client.stop();
	  if (!aprsis_status.startsWith("Error: "))
            aprsis_status = "Disconnected";
	  if (t_connect_apprsis_again <=  millis())
	    t_connect_apprsis_again = millis() + 60000;
          to_aprsis_data = "";
        }
      }
    }

    vTaskDelay(5/portTICK_PERIOD_MS);
  }
}


String generate_third_party_packet(String callsign, String packet_in)
{
  String packet_out = "";
  const char *s = packet_in.c_str();
  char *p = strchr(s, '>');
  char *q = strchr(s, ',');
  char *r = strchr(s, ':');
  char fromtodest[20]; // room for max (due to spec) 'DL9SAU-15>APRSXX-NN' + \0
  if (p > s && p < q && q < r && (q-s) < sizeof(fromtodest)) {
    r++;
    strncpy(fromtodest, s, q-s);
    fromtodest[(q-s)] = 0;
    packet_out = callsign + ">" + MY_APRS_DEST_IDENTIFYER + ":}" + fromtodest + ",TCPIP," + callsign + "*:" + r;
                             // ^ 3rd party traffic should be addressed directly (-> not to WIDE2-1 or so)
  }
  return packet_out;
}
