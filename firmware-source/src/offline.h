#pragma once

#include "offline_page.h"

WebServer controlServer(80);
DNSServer captiveDns;
String accessPointName;
String accessPointPassword;
QueueHandle_t weatherResults = nullptr;
volatile bool weatherRefreshPending = false;
volatile bool weatherLoading = false;
String weatherLastError;
int weatherLastHttpCode = 0;
uint32_t weatherLastBodyBytes = 0;
bool wifiWasConnected = false;
volatile uint8_t wifiLastDisconnectReason = 0;
uint32_t wifiConnectAttempts = 0;
bool accessPointStarted = false;
bool wifiConnectPending = false;
bool calendarFallback = false;
uint32_t wifiReconnectAt = 0;
bool desktopHeartbeatSeen = false;
uint32_t desktopLastHeartbeatAt = 0;
uint8_t desktopAudioState = 0;

bool desktopIsConnected() {
  return desktopHeartbeatSeen && millis() - desktopLastHeartbeatAt < 6000;
}

void noteDesktopHeartbeat(uint8_t audioState) {
  desktopHeartbeatSeen = true;
  desktopLastHeartbeatAt = millis();
  desktopAudioState = audioState;
}

struct WeatherResult {
  bool success;
  WeatherReading reading;
  int httpCode;
  uint32_t bodyBytes;
  char error[160];
};

bool clockIsValid() {
  return time(nullptr) >= 1704067200LL;
}

bool weatherIsFresh() {
  const time_t now = time(nullptr);
  return clockIsValid() && savedWeather.updatedAt > 0 &&
         now >= savedWeather.updatedAt && now - savedWeather.updatedAt < 3600;
}

void markSettingsDirty() {
  settingsDirty = true;
  settingsChangedAt = millis();
}

void rememberMode(const String& mode) {
  if (preferredMode != mode) {
    preferredMode = mode;
    markSettingsDirty();
  }
}

bool validMode(const String& mode) {
  return mode == "calendar" || mode == "pet" || mode == "lamp" ||
         mode == "newyear" || mode == "dino" || mode == "waveform" ||
         mode == "spectrum" || mode == "status" || mode == "phrases";
}

void enterCalendar() {
  calendarFallback = true;
  if (clockIsValid()) {
    displayMode = DisplayMode::Calendar;
    lastCalendarSecond = 0;
  } else {
    displayMode = DisplayMode::Pet;
    setPetAction(PetAction::Awake, millis());
  }
}

bool chooseMode(const String& mode) {
  if (!validMode(mode)) {
    return false;
  }
  rememberMode(mode);
  calendarFallback = false;
  compensateSkippedWord = false;
  serpentineRows = false;
  memset(pixels, 0, sizeof(pixels));
  memset(musicPeaks, 0, sizeof(musicPeaks));
  if (mode == "calendar") {
    enterCalendar();
  } else if (mode == "phrases") {
    displayMode = DisplayMode::Phrases;
    phraseStartedAt = millis();
    phraseLastFrameAt = 0;
    showPhrases(millis());
  } else if (mode == "pet") {
    displayMode = DisplayMode::Pet;
    setPetAction(PetAction::Awake, millis());
  } else if (mode == "lamp") {
    showLamp(maxLampLevel);
  } else if (mode == "newyear") {
    displayMode = DisplayMode::NewYear;
    startedAt = millis();
  } else if (mode == "dino") {
    displayMode = DisplayMode::Dino;
    dinoState = DinoState::Ready;
    dinoScore = 0;
    cactusX = stripWidth - 1;
    cactusWidth = 2;
    reportDino("ready");
  } else if (controlOwner == "desktop") {
    displayMode = DisplayMode::Music;
    lastMusicFrameAt = millis();
    liveFrameTimeoutMs = 2600;
    musicLit = false;
    sendFrame();
  } else {
    enterCalendar();
  }
  return true;
}

bool setClockEpoch(time_t epoch) {
  if (epoch < 1704067200LL || static_cast<int64_t>(epoch) >= 4102444800LL) {
    return false;
  }
  timeval value{epoch, 0};
  if (settimeofday(&value, nullptr) != 0) {
    return false;
  }
  lastCalendarSecond = 0;
  return true;
}

void storeWeather(const WeatherReading& reading) {
  savedWeather = reading;
  lastCalendarSecond = 0;
  if (settingsReady) {
    settings.putInt("weatherTemp", reading.temperature);
    settings.putUChar("weatherIcon", reading.icon);
    settings.putLong64("weatherAt", reading.updatedAt);
    settings.putString("weatherText", reading.condition);
  }
}

bool acceptCalendarPacket(const uint8_t* text, size_t length) {
  if (text[2] != ':' || text[5] != ':' || text[10] != '-') {
    return false;
  }
  constexpr size_t digits[] = {0, 1, 3, 4, 6, 7, 8, 9, 11, 12, 13};
  for (size_t index : digits) {
    if (text[index] < '0' || text[index] > '9') {
      return false;
    }
  }
  const unsigned hour = (text[0] - '0') * 10 + text[1] - '0';
  const unsigned minute = (text[3] - '0') * 10 + text[4] - '0';
  const unsigned second = (text[6] - '0') * 10 + text[7] - '0';
  const unsigned month = (text[8] - '0') * 10 + text[9] - '0';
  const unsigned day = (text[11] - '0') * 10 + text[12] - '0';
  if (hour > 23 || minute > 59 || second > 59 || month < 1 ||
      month > 12 || day < 1 || day > 31 || text[13] > '6') {
    return false;
  }
  tm local{};
  const time_t current = time(nullptr);
  if (clockIsValid()) {
    localtime_r(&current, &local);
  } else {
    local.tm_year = atoi(__DATE__ + 7) - 1900;
  }
  local.tm_mon = month - 1;
  local.tm_mday = day;
  local.tm_hour = hour;
  local.tm_min = minute;
  local.tm_sec = second;
  local.tm_isdst = -1;
  const time_t epoch = mktime(&local);
  if (local.tm_mon != static_cast<int>(month - 1) ||
      local.tm_mday != static_cast<int>(day) || !setClockEpoch(epoch)) {
    return false;
  }
  if (length == weatherCalendarLength && (text[14] == '+' || text[14] == '-') &&
      isdigit(text[15]) && isdigit(text[16]) && text[17] >= '0' && text[17] <= '7') {
    WeatherReading reading{};
    reading.temperature = ((text[15] - '0') * 10 + text[16] - '0') *
                          (text[14] == '-' ? -1 : 1);
    reading.icon = text[17] - '0';
    reading.updatedAt = epoch;
    constexpr const char* descriptions[] = {"晴", "晴", "多云/阴", "有雨", "雷雨", "雨雪", "雾霾", "未知天气"};
    snprintf(reading.condition, sizeof(reading.condition), "%s", descriptions[reading.icon]);
    // Desktop packets arrive twice per second. Persist the weather only periodically.
    if (savedWeather.updatedAt == 0 || epoch < savedWeather.updatedAt ||
        epoch - savedWeather.updatedAt >= 600 ||
        reading.temperature != savedWeather.temperature || reading.icon != savedWeather.icon) {
      storeWeather(reading);
    }
  }
  return true;
}

void renderOfflineCalendar() {
  const time_t now = time(nullptr);
  if (!clockIsValid() || now == lastCalendarSecond) {
    return;
  }
  tm local{};
  localtime_r(&now, &local);
  char packet[24]{};
  strftime(packet, sizeof(packet), "%H:%M:%S%m-%d", &local);
  packet[13] = '0' + (local.tm_wday + 6) % 7;
  size_t length = calendarLength;
  if (weatherIsFresh()) {
    snprintf(packet + calendarLength, sizeof(packet) - calendarLength,
             "%+03d%u", savedWeather.temperature, savedWeather.icon);
    length = weatherCalendarLength;
  }
  showCalendar(reinterpret_cast<const uint8_t*>(packet), length, millis(), false);
  lastCalendarSecond = now;
}

void releaseDesktopControl() {
  desktopHeartbeatSeen = false;
  desktopAudioState = 0;
  if (controlOwner == "phone") {
    if (preferredMode == "waveform" || preferredMode == "spectrum") {
      enterCalendar();
    }
    printPanelState();
    return;
  }
  controlOwner = "device";
  chooseMode(preferredMode);
  printPanelState();
}

const char* jsonText(cJSON* object, const char* name) {
  cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
  return cJSON_IsString(value) ? value->valuestring : "";
}

String panelStateJson() {
  cJSON* state = cJSON_CreateObject();
  const time_t now = time(nullptr);
  char clockText[24]{};
  if (clockIsValid()) {
    tm local{};
    localtime_r(&now, &local);
    strftime(clockText, sizeof(clockText), "%Y-%m-%d %H:%M:%S", &local);
  }
  cJSON_AddStringToObject(state, "mode", preferredMode.c_str());
  cJSON_AddStringToObject(state, "owner", controlOwner.c_str());
  cJSON_AddNumberToObject(state, "brightness", displayBrightness);
  const bool desktopConnected = desktopIsConnected();
  cJSON_AddBoolToObject(state, "webAudioControl", true);
  cJSON_AddBoolToObject(state, "desktopConnected", desktopConnected);
  const char* audioStatus = !desktopConnected ? "offline" :
      desktopAudioState == 2 ? "unavailable" : desktopAudioState == 1 ? "ready" : "idle";
  cJSON_AddStringToObject(state, "desktopAudioStatus", audioStatus);
  cJSON_AddBoolToObject(state, "audioStreaming", desktopConnected && musicLit &&
      displayMode == DisplayMode::Music &&
      (preferredMode == "waveform" || preferredMode == "spectrum") &&
      millis() - lastMusicFrameAt <= musicTimeoutMs);
  cJSON_AddBoolToObject(state, "audioActive", desktopConnected && musicLit &&
      displayMode == DisplayMode::Music && audioHasSound &&
      millis() - lastMusicFrameAt <= musicTimeoutMs);
  cJSON_AddBoolToObject(state, "timeValid", clockIsValid());
  cJSON_AddStringToObject(state, "time", clockText);
  cJSON_AddBoolToObject(state, "wifiConnected", WiFi.status() == WL_CONNECTED);
  cJSON_AddNumberToObject(state, "wifiStatus", WiFi.status());
  cJSON_AddNumberToObject(state, "wifiDisconnectReason", wifiLastDisconnectReason);
  cJSON_AddNumberToObject(state, "wifiConnectAttempts", wifiConnectAttempts);
  cJSON_AddNumberToObject(state, "wifiSignal", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  cJSON_AddBoolToObject(state, "wifiSaved", settingsReady && !settings.getString("ssid").isEmpty());
  cJSON_AddStringToObject(state, "stationIp", WiFi.localIP().toString().c_str());
  cJSON_AddStringToObject(state, "apSsid", accessPointName.c_str());
  cJSON_AddStringToObject(state, "apIp", WiFi.softAPIP().toString().c_str());
  cJSON_AddBoolToObject(state, "apStarted", accessPointStarted);
  cJSON_AddStringToObject(state, "apActualSsid", WiFi.softAPSSID().c_str());
  cJSON_AddNumberToObject(state, "wifiMode", WiFi.getMode());
  cJSON_AddNumberToObject(state, "apClients", WiFi.softAPgetStationNum());
  cJSON_AddNumberToObject(state, "freeHeap", ESP.getFreeHeap());
  cJSON_AddBoolToObject(state, "settingsPending", settingsDirty);
  cJSON_AddNumberToObject(state, "uptime", millis() / 1000);
  cJSON_AddNumberToObject(state, "frames", frameCount);
  cJSON_AddNumberToObject(state, "phraseIndex", currentPhraseIndex(millis()));
  cJSON_AddStringToObject(state, "phraseText", phraseLines[currentPhraseIndex(millis())].c_str());
  cJSON* weather = cJSON_AddObjectToObject(state, "weather");
  cJSON_AddBoolToObject(weather, "valid", weatherIsFresh());
  cJSON_AddNumberToObject(weather, "temperature", savedWeather.temperature);
  cJSON_AddNumberToObject(weather, "icon", savedWeather.icon);
  cJSON_AddStringToObject(weather, "condition", savedWeather.condition);
  cJSON_AddNumberToObject(weather, "updatedAt", savedWeather.updatedAt);
  cJSON_AddNumberToObject(weather, "ageSeconds", weatherIsFresh() ? now - savedWeather.updatedAt : -1);
  cJSON_AddBoolToObject(weather, "loading", weatherLoading);
  cJSON_AddStringToObject(weather, "error", weatherLastError.c_str());
  cJSON_AddNumberToObject(weather, "httpCode", weatherLastHttpCode);
  cJSON_AddNumberToObject(weather, "bodyBytes", weatherLastBodyBytes);
  char* encoded = cJSON_PrintUnformatted(state);
  const String result = encoded ? encoded : "{}";
  cJSON_free(encoded);
  cJSON_Delete(state);
  return result;
}

void printPanelState() {
  Serial.printf("Panel state: %s\n", panelStateJson().c_str());
}

void sendWebState() {
  controlServer.sendHeader("Cache-Control", "no-store");
  controlServer.send(200, "application/json; charset=utf-8", panelStateJson());
}

bool unsignedArgument(const String& text, uint64_t& value, uint64_t maximum) {
  if (text.isEmpty() || text.length() > 12) {
    return false;
  }
  value = 0;
  for (size_t index = 0; index < text.length(); ++index) {
    if (text[index] < '0' || text[index] > '9') {
      return false;
    }
    value = value * 10 + text[index] - '0';
    if (value > maximum) {
      return false;
    }
  }
  return true;
}

bool performPetOrGameAction(const String& action) {
  if (preferredMode == "pet" && (action == "pet" || action == "play")) {
    setPetAction(action == "pet" ? PetAction::Petted : PetAction::Playing, millis());
    return true;
  }
  if (preferredMode == "dino" && action == "jump") {
    handleDinoJump(millis());
    return true;
  }
  return false;
}

void configureWebRoutes() {
  controlServer.on("/", HTTP_GET, []() {
    controlServer.sendHeader("Cache-Control", "no-store");
    controlServer.send_P(200, "text/html; charset=utf-8", controlPage);
  });
  controlServer.on("/api/state", HTTP_GET, sendWebState);
  controlServer.on("/api/text", HTTP_GET, []() {
    controlServer.sendHeader("Cache-Control", "no-store");
    controlServer.send(200, "application/json; charset=utf-8", phraseConfigurationJson());
  });
  controlServer.on("/api/text", HTTP_POST, []() {
    const String body = controlServer.arg("config");
    if (body.length() > 5000) {
      controlServer.send(400, "application/json", R"({"error":"轮播内容太长"})"); return;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    String lines[maximumPhrases], error;
    unsigned count = 0;
    cJSON* seconds = cJSON_GetObjectItemCaseSensitive(root, "seconds");
    const bool valid = cJSON_IsNumber(seconds) && seconds->valuedouble == seconds->valueint &&
        seconds->valueint >= 3 && seconds->valueint <= 120 &&
        parsePhraseArray(cJSON_GetObjectItemCaseSensitive(root, "lines"), lines, maximumPhrases, count, false, error);
    if (!valid) {
      if (error.isEmpty()) error = "每句显示时间需要在 3 到 120 秒之间";
      cJSON* response = cJSON_CreateObject();
      cJSON_AddStringToObject(response, "error", error.c_str());
      char* encoded = cJSON_PrintUnformatted(response);
      controlServer.send(400, "application/json; charset=utf-8", encoded ? encoded : "{}");
      cJSON_free(encoded); cJSON_Delete(response); cJSON_Delete(root); return;
    }
    // Roll back in-memory state if storage is unavailable.
    String previous[maximumPhrases];
    const unsigned previousCount = phraseCount, previousSeconds = phraseSeconds;
    for (unsigned i = 0; i < phraseCount; ++i) previous[i] = phraseLines[i];
    phraseCount = count; phraseSeconds = seconds->valueint;
    for (unsigned i = 0; i < count; ++i) phraseLines[i] = lines[i];
    cJSON_Delete(root);
    if (!persistPhrases()) {
      phraseCount = previousCount; phraseSeconds = previousSeconds;
      for (unsigned i = 0; i < phraseCount; ++i) phraseLines[i] = previous[i];
      controlServer.send(500, "application/json", R"({"error":"未能保存，请重试"})"); return;
    }
    phraseStartedAt = millis(); phraseLastFrameAt = 0;
    controlServer.send(200, "application/json; charset=utf-8", phraseConfigurationJson());
  });
  controlServer.on("/api/text/favorites", HTTP_POST, []() {
    const String body = controlServer.arg("lines");
    if (body.length() > 10000) {
      controlServer.send(400, "application/json", R"({"error":"常用词句内容太长"})"); return;
    }
    cJSON* array = cJSON_Parse(body.c_str());
    String lines[maximumFavorites], error;
    unsigned count = 0;
    const bool valid = parsePhraseArray(array, lines, maximumFavorites, count, true, error);
    cJSON_Delete(array);
    if (!valid) {
      cJSON* response = cJSON_CreateObject();
      cJSON_AddStringToObject(response, "error", error.c_str());
      char* encoded = cJSON_PrintUnformatted(response);
      controlServer.send(400, "application/json; charset=utf-8", encoded ? encoded : "{}");
      cJSON_free(encoded); cJSON_Delete(response); return;
    }
    String previous[maximumFavorites];
    const unsigned previousCount = favoriteCount;
    for (unsigned i = 0; i < favoriteCount; ++i) previous[i] = favoriteLines[i];
    favoriteCount = 0;
    for (unsigned i = 0; i < count; ++i) {
      bool duplicate = false;
      for (unsigned j = 0; j < favoriteCount; ++j) duplicate |= favoriteLines[j] == lines[i];
      if (!duplicate) favoriteLines[favoriteCount++] = lines[i];
    }
    if (!persistPhrases()) {
      favoriteCount = previousCount;
      for (unsigned i = 0; i < favoriteCount; ++i) favoriteLines[i] = previous[i];
      controlServer.send(500, "application/json", R"({"error":"未能保存，请重试"})"); return;
    }
    controlServer.send(200, "application/json; charset=utf-8", phraseConfigurationJson());
  });
  controlServer.on("/api/weather/refresh", HTTP_POST, []() {
    if (WiFi.status() != WL_CONNECTED) {
      controlServer.send(400, "application/json", R"({"error":"请先连接家里的 Wi-Fi"})");
      return;
    }
    weatherRefreshPending = true;
    sendWebState();
  });
  controlServer.on("/api/mode", HTTP_POST, []() {
    const String mode = controlServer.arg("mode");
    if (!validMode(mode)) {
      controlServer.send(400, "application/json", R"({"error":"无效的显示模式"})");
      return;
    }
    if ((mode == "waveform" || mode == "spectrum") && !desktopIsConnected()) {
      controlServer.send(409, "application/json",
                         R"({"error":"电脑音频后台未连接，请检查电脑和 USB 数据线连接"})");
      return;
    }
    controlOwner = "phone";
    chooseMode(mode);
    sendWebState();
  });
  controlServer.on("/api/brightness", HTTP_POST, []() {
    uint64_t value;
    if (!unsignedArgument(controlServer.arg("value"), value, 100)) {
      controlServer.send(400, "application/json", R"({"error":"亮度需要在 0 到 100 之间"})");
      return;
    }
    if (displayBrightness != value) {
      displayBrightness = value;
      markSettingsDirty();
    }
    sendFrame(SIZE_MAX, {}, displayMode == DisplayMode::Lamp ? lampPowerLimit : framePowerLimit);
    sendWebState();
  });
  controlServer.on("/api/time", HTTP_POST, []() {
    uint64_t epoch;
    if (!unsignedArgument(controlServer.arg("epoch"), epoch, 4102444799ULL) ||
        !setClockEpoch(static_cast<time_t>(epoch))) {
      controlServer.send(400, "application/json", R"({"error":"手机时间无效，请检查手机日期"})");
      return;
    }
    sendWebState();
  });
  controlServer.on("/api/action", HTTP_POST, []() {
    if (!performPetOrGameAction(controlServer.arg("action"))) {
      controlServer.send(400, "application/json", R"({"error":"请先切换到对应模式"})");
      return;
    }
    controlOwner = "phone";
    sendWebState();
  });
  controlServer.on("/api/wifi", HTTP_POST, []() {
    const String ssid = controlServer.arg("ssid");
    const String password = controlServer.arg("password");
    if (!settingsReady || ssid.isEmpty() || ssid.length() > 32 ||
        (password.length() > 0 && password.length() < 8) || password.length() > 63) {
      controlServer.send(400, "application/json", R"({"error":"请检查 Wi-Fi 名称和密码长度"})");
      return;
    }
    settings.putString("ssid", ssid);
    settings.putString("password", password);
    sendWebState();
    wifiConnectPending = true;
  });
  controlServer.onNotFound([]() {
    if (controlServer.uri().startsWith("/api/")) {
      controlServer.send(404, "application/json", R"({"error":"找不到此操作"})");
      return;
    }
    controlServer.sendHeader("Location", "http://192.168.4.1/", true);
    controlServer.send(302, "text/plain", "");
  });
  controlServer.begin();
}

WeatherResult fetchFuzhouWeather() {
  WeatherResult result{};
  WiFiClientSecure client;
  client.setCACert(weatherRootCA);
  client.setHandshakeTimeout(8);
  HTTPClient request;
  request.setConnectTimeout(5000);
  request.setTimeout(6000);
  request.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!request.begin(client, "https://wttr.in/Fuzhou,Fujian?format=j2")) {
    snprintf(result.error, sizeof(result.error), "HTTPS initialization failed");
    return result;
  }
  request.setUserAgent("ESP32DesktopPanel/2.0");
  result.httpCode = request.GET();
  if (result.httpCode != HTTP_CODE_OK || request.getSize() > 65536) {
    char tlsMessage[96]{};
    const int tlsCode = client.lastError(tlsMessage, sizeof(tlsMessage));
    snprintf(result.error, sizeof(result.error), "HTTP %d; TLS %d %.96s",
             result.httpCode, tlsCode, tlsMessage);
    request.end();
    return result;
  }
  const String body = request.getString();
  result.bodyBytes = body.length();
  request.end();
  client.stop();
  cJSON* data = cJSON_Parse(body.c_str());
  if (!data) {
    snprintf(result.error, sizeof(result.error), "Invalid JSON, %u bytes", result.bodyBytes);
    return result;
  }
  cJSON* area = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(data, "nearest_area"), 0);
  cJSON* areaName = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(area, "areaName"), 0);
  cJSON* region = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(area, "region"), 0);
  cJSON* current = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(data, "current_condition"), 0);
  cJSON* text = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(current, "weatherDesc"), 0);
  const String city = jsonText(areaName, "value");
  const String province = jsonText(region, "value");
  const String temperatureText = jsonText(current, "temp_C");
  char* end = nullptr;
  const double temperature = strtod(temperatureText.c_str(), &end);
  if (!city.equalsIgnoreCase("Fuzhou") || !province.equalsIgnoreCase("Fujian") ||
      temperatureText.isEmpty() || end == temperatureText.c_str() || *end != 0 ||
      !isfinite(temperature) || temperature < -99 || temperature > 99) {
    snprintf(result.error, sizeof(result.error), "Invalid Fuzhou weather response");
    cJSON_Delete(data);
    return result;
  }
  String description = jsonText(text, "value");
  description.toLowerCase();
  result.reading.temperature = lround(temperature);
  result.reading.updatedAt = time(nullptr);
  const char* condition = "未知天气";
  uint8_t icon = 7;
  if (description.indexOf("thunder") >= 0) {
    icon = 4;
    condition = "雷雨";
  } else if (description.indexOf("snow") >= 0 || description.indexOf("sleet") >= 0 ||
             description.indexOf("ice pellets") >= 0) {
    icon = 5;
    condition = "雨雪";
  } else if (description.indexOf("rain") >= 0 || description.indexOf("drizzle") >= 0) {
    icon = 3;
    condition = "有雨";
  } else if (description.indexOf("haze") >= 0 || description.indexOf("smok") >= 0 ||
             description.indexOf("smog") >= 0) {
    icon = 6;
    condition = "霾";
  } else if (description.indexOf("dust") >= 0 || description.indexOf("sand") >= 0) {
    icon = 6;
    condition = "沙尘";
  } else if (description.indexOf("mist") >= 0 || description.indexOf("fog") >= 0) {
    icon = 6;
    condition = "雾";
  } else if (description.indexOf("cloud") >= 0 || description.indexOf("overcast") >= 0) {
    icon = 2;
    condition = description.indexOf("partly") >= 0 ? "多云" : "阴";
  } else if (description.indexOf("sunny") >= 0 || description.indexOf("clear") >= 0) {
    tm local{};
    const time_t now = time(nullptr);
    localtime_r(&now, &local);
    icon = local.tm_hour >= 6 && local.tm_hour < 18 ? 0 : 1;
    condition = "晴";
  }
  result.reading.icon = icon;
  snprintf(result.reading.condition, sizeof(result.reading.condition), "%s", condition);
  result.success = true;
  cJSON_Delete(data);
  return result;
}

void weatherWorker(void*) {
  uint32_t lastAttempt = 0;
  uint32_t retryAfter = 0;
  bool attempted = false;
  while (true) {
    const uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED && clockIsValid() &&
        (weatherRefreshPending || !attempted || now - lastAttempt >= retryAfter)) {
      weatherRefreshPending = false;
      weatherLoading = true;
      Serial.println("Panel weather: fetching");
      attempted = true;
      lastAttempt = now;
      const WeatherResult result = fetchFuzhouWeather();
      retryAfter = result.success ? 600000 : 30000;
      xQueueOverwrite(weatherResults, &result);
      weatherLoading = false;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void connectSavedWifi() {
  if (!settingsReady) {
    return;
  }
  const String ssid = settings.getString("ssid");
  if (ssid.isEmpty()) {
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  const String password = settings.getString("password");
  WiFi.begin(ssid.c_str(), password.c_str(), 0, nullptr, false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.reconnect();
  ++wifiConnectAttempts;
  wifiReconnectAt = millis();
}

void startOfflineController() {
  setenv("TZ", "CST-8", 1);
  tzset();
  settingsReady = settings.begin("deskpanel", false);
  loadPhrases();
  if (settingsReady) {
    const String remembered = settings.getString("mode", "calendar");
    preferredMode = validMode(remembered) ? remembered : "calendar";
    displayBrightness = min(static_cast<unsigned>(settings.getUChar("brightness", 38)), 100u);
    savedWeather.temperature = settings.getInt("weatherTemp", 0);
    savedWeather.icon = settings.getUChar("weatherIcon", 7);
    savedWeather.updatedAt = settings.getLong64("weatherAt", 0);
    const String condition = settings.getString("weatherText");
    snprintf(savedWeather.condition, sizeof(savedWeather.condition), "%s", condition.c_str());
    if (savedWeather.icon > 7 || savedWeather.temperature < -99 || savedWeather.temperature > 99) {
      savedWeather.updatedAt = 0;
    }
  }
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      wifiLastDisconnectReason = info.wifi_sta_disconnected.reason;
      Serial.printf("Panel WiFi: disconnected, reason=%u\n", wifiLastDisconnectReason);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      wifiLastDisconnectReason = 0;
      Serial.println("Panel WiFi: connected");
    }
  });
  const bool wifiConfigured = settingsReady && !settings.getString("ssid").isEmpty();
  WiFi.mode(wifiConfigured ? WIFI_AP_STA : WIFI_AP);
  WiFi.setSleep(false);
  accessPointPassword = settingsReady ? settings.getString("apPassword") : "";
  if (accessPointPassword.length() < 12) {
    char generated[17];
    snprintf(generated, sizeof(generated), "%08lx%08lx",
             static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    accessPointPassword = generated;
    if (settingsReady) settings.putString("apPassword", accessPointPassword);
  }
  const String mac = WiFi.macAddress();
  accessPointName = "LED-Clock-" + mac.substring(12);
  accessPointName.replace(":", "");
  accessPointStarted = WiFi.softAP(accessPointName.c_str(), accessPointPassword.c_str(), 6, false, 4);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  captiveDns.start(53, "*", WiFi.softAPIP());
  configureWebRoutes();
  connectSavedWifi();
  weatherResults = xQueueCreate(1, sizeof(WeatherResult));
  if (weatherResults) {
    if (xTaskCreate(weatherWorker, "panel-weather", 8192, nullptr, 1, nullptr) != pdPASS) {
      Serial.println("Panel weather: worker unavailable");
    }
  }
  chooseMode(preferredMode);
  Serial.printf("Panel boot: mode=%s brightness=%u clock=%s\n",
                preferredMode.c_str(), displayBrightness, clockIsValid() ? "valid" : "waiting");
  Serial.printf("Panel WiFi: %s http://%s/\n",
                accessPointName.c_str(), WiFi.softAPIP().toString().c_str());
  Serial.printf("Panel setup: AP password: %s\n", accessPointPassword.c_str());
  printPanelState();
}

void serviceOfflineController() {
  captiveDns.processNextRequest();
  controlServer.handleClient();
  if (wifiConnectPending) {
    wifiConnectPending = false;
    connectSavedWifi();
  }
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    configTime(0, 0, "ntp.aliyun.com", "time.cloudflare.com", "pool.ntp.org");
    // configTime changes TZ. Restore the display's China Standard Time.
    setenv("TZ", "CST-8", 1);
    tzset();
    weatherRefreshPending = true;
  }
  wifiWasConnected = connected;
  if (!connected && millis() - wifiReconnectAt >= 15000) {
    connectSavedWifi();
    wifiReconnectAt = millis();
  }
  WeatherResult weather{};
  if (weatherResults && xQueueReceive(weatherResults, &weather, 0) == pdTRUE) {
    weatherLastHttpCode = weather.httpCode;
    weatherLastBodyBytes = weather.bodyBytes;
    weatherLastError = weather.error;
    if (weather.success) {
      storeWeather(weather.reading);
      Serial.printf("Panel weather: %s %d C\n", savedWeather.condition, savedWeather.temperature);
    } else {
      Serial.printf("Panel weather: retry pending, %s\n", weather.error);
    }
  }
  if (settingsReady && settingsDirty && millis() - settingsChangedAt >= 1500) {
    if (settings.getString("mode") != preferredMode) {
      settings.putString("mode", preferredMode);
    }
    if (settings.getUChar("brightness", 255) != displayBrightness) {
      settings.putUChar("brightness", displayBrightness);
    }
    settingsDirty = false;
  }
  if (displayMode == DisplayMode::Pet && calendarFallback && clockIsValid()) {
    enterCalendar();
  }
  if (displayMode == DisplayMode::Calendar) {
    renderOfflineCalendar();
  }
}
