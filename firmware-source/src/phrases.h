#pragma once

#include "chinese_font.h"

constexpr unsigned maximumPhrases = 16;
constexpr unsigned maximumFavorites = 32;
constexpr unsigned maximumPhraseCharacters = 32;
String phraseLines[maximumPhrases] = {"物来顺应", "未来不迎", "当时不杂", "既往不恋"};
String favoriteLines[maximumFavorites] = {"物来顺应", "未来不迎", "当时不杂", "既往不恋"};
unsigned phraseCount = 4;
unsigned favoriteCount = 4;
unsigned phraseSeconds = 10;
uint32_t phraseStartedAt = 0;
uint32_t phraseLastFrameAt = 0;

// Strict UTF-8 decoding avoids truncating a Chinese character at a byte boundary.
bool nextPhraseCharacter(const String& text, size_t& offset, uint16_t& character) {
  if (offset >= text.length()) return false;
  const uint8_t first = text[offset++];
  if (first < 0x80) { character = first; return first >= 0x20 && first != 0x7f; }
  const unsigned count = first >= 0xc2 && first <= 0xdf ? 1 :
                         first >= 0xe0 && first <= 0xef ? 2 : 0;
  if (!count || offset + count > text.length()) return false;
  uint32_t value = first & (count == 1 ? 0x1f : 0x0f);
  for (unsigned index = 0; index < count; ++index) {
    const uint8_t byte = text[offset++];
    if ((byte & 0xc0) != 0x80) return false;
    value = (value << 6) | (byte & 0x3f);
  }
  if (value < (count == 1 ? 0x80u : 0x800u) || (value >= 0xd800 && value <= 0xdfff)) return false;
  character = value;
  return true;
}

const ChineseGlyph* findPhraseGlyph(uint16_t character) {
  size_t left = 0, right = sizeof(chineseGlyphs) / sizeof(chineseGlyphs[0]);
  while (left < right) {
    const size_t middle = left + (right - left) / 2;
    if (chineseGlyphs[middle].character < character) left = middle + 1;
    else right = middle;
  }
  return left < sizeof(chineseGlyphs) / sizeof(chineseGlyphs[0]) &&
         chineseGlyphs[left].character == character ? &chineseGlyphs[left] : nullptr;
}

bool validatePhrase(const String& text, String& error) {
  if (text.isEmpty() || text.length() > maximumPhraseCharacters * 3) {
    error = "每句请输入 1 到 32 个字"; return false;
  }
  size_t offset = 0;
  unsigned count = 0;
  while (offset < text.length()) {
    const size_t before = offset;
    uint16_t character;
    if (!nextPhraseCharacter(text, offset, character)) {
      error = "请使用常用中文、英文或标点，不支持表情符号"; return false;
    }
    if (!findPhraseGlyph(character)) {
      error = "字库暂不支持这个字：" + text.substring(before, offset); return false;
    }
    if (++count > maximumPhraseCharacters) { error = "每句最多 32 个字"; return false; }
  }
  return true;
}

bool parsePhraseArray(cJSON* array, String* result, unsigned maximum, unsigned& count,
                      bool allowEmpty, String& error) {
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > static_cast<int>(maximum) ||
      (!allowEmpty && cJSON_GetArraySize(array) == 0)) {
    error = allowEmpty ? "常用词句最多保存 32 条" : "轮播需要 1 到 16 句"; return false;
  }
  count = 0;
  cJSON* item;
  cJSON_ArrayForEach(item, array) {
    if (!cJSON_IsString(item)) { error = "词句格式无效"; return false; }
    String line = item->valuestring;
    line.trim();
    if (!validatePhrase(line, error)) return false;
    result[count++] = line;
  }
  return true;
}

String phraseConfigurationJson() {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "seconds", phraseSeconds);
  cJSON* lines = cJSON_AddArrayToObject(root, "lines");
  cJSON* favorites = cJSON_AddArrayToObject(root, "favorites");
  for (unsigned i = 0; i < phraseCount; ++i) cJSON_AddItemToArray(lines, cJSON_CreateString(phraseLines[i].c_str()));
  for (unsigned i = 0; i < favoriteCount; ++i) cJSON_AddItemToArray(favorites, cJSON_CreateString(favoriteLines[i].c_str()));
  char* encoded = cJSON_PrintUnformatted(root);
  const String result = encoded ? encoded : "{}";
  cJSON_free(encoded); cJSON_Delete(root);
  return result;
}

bool persistPhrases() {
  const String encoded = phraseConfigurationJson();
  return settingsReady && settings.putString("textConfig", encoded) == encoded.length();
}

void loadPhrases() {
  if (!settingsReady) return;
  const String encoded = settings.getString("textConfig");
  if (encoded.isEmpty()) return;
  cJSON* root = cJSON_Parse(encoded.c_str());
  String lines[maximumPhrases], favorites[maximumFavorites], error;
  unsigned count = 0, favoritesSize = 0;
  cJSON* seconds = cJSON_GetObjectItemCaseSensitive(root, "seconds");
  if (cJSON_IsNumber(seconds) && seconds->valuedouble == seconds->valueint &&
      seconds->valueint >= 3 && seconds->valueint <= 120 &&
      parsePhraseArray(cJSON_GetObjectItemCaseSensitive(root, "lines"), lines, maximumPhrases, count, false, error) &&
      parsePhraseArray(cJSON_GetObjectItemCaseSensitive(root, "favorites"), favorites, maximumFavorites, favoritesSize, true, error)) {
    phraseCount = count; favoriteCount = favoritesSize; phraseSeconds = seconds->valueint;
    for (unsigned i = 0; i < count; ++i) phraseLines[i] = lines[i];
    for (unsigned i = 0; i < favoritesSize; ++i) favoriteLines[i] = favorites[i];
  }
  cJSON_Delete(root);
}

unsigned currentPhraseIndex(uint32_t now) {
  return ((now - phraseStartedAt) / (phraseSeconds * 1000u)) % phraseCount;
}

void showPhrases(uint32_t now) {
  if (now - phraseLastFrameAt < 35) return;
  phraseLastFrameAt = now;
  const unsigned index = currentPhraseIndex(now);
  const String& line = phraseLines[index];
  unsigned width = 0;
  size_t offset = 0;
  uint16_t character;
  while (nextPhraseCharacter(line, offset, character)) width += character < 0x80 ? 4 : 8;
  const uint32_t age = (now - phraseStartedAt) % (phraseSeconds * 1000u);
  int x = (static_cast<int>(stripWidth) - static_cast<int>(width)) / 2;
  if (width > stripWidth) {
    const unsigned travel = width - stripWidth;
    const uint32_t scrollingTime = phraseSeconds * 1000u - 1600;
    const uint32_t progress = age <= 800 ? 0 : min(age - 800, scrollingTime);
    x = -static_cast<int>(travel * progress / scrollingTime);
  }
  constexpr Color inks[] = {{0, 26, 24}, {30, 20, 6}, {9, 20, 32}, {27, 8, 23}};
  const Color ink = inks[index % 4];
  memset(pixels, 0, sizeof(pixels));
  offset = 0;
  while (nextPhraseCharacter(line, offset, character)) {
    const ChineseGlyph* glyph = findPhraseGlyph(character);
    const unsigned glyphWidth = character < 0x80 ? 4 : 8;
    if (glyph) {
      for (unsigned y = 0; y < 8; ++y)
        for (unsigned column = 0; column < glyphWidth; ++column)
          if (glyph->rows[y] & (0x80 >> column)) setStripPixel(x + static_cast<int>(column), y, ink);
    }
    x += glyphWidth;
  }
  sendFrame();
}
