#include <Arduino.h>
#include <esp_system.h>
#include <driver/spi_master.h>
#include <esp_attr.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <cJSON.h>
#include <time.h>
#include <sys/time.h>
#include "weather_ca.h"

namespace {

constexpr gpio_num_t dataPin = GPIO_NUM_4;
constexpr int matrixSize = 16;
constexpr size_t pixelCount = matrixSize * matrixSize;
// An extra black word here aligns the columns on this panel.
constexpr size_t panelPaddingWord = 12 * matrixSize + 2;
constexpr size_t maxChainWords = pixelCount + 1;
constexpr size_t bitsPerPixel = 24;
constexpr int spiClockHz = 8000000;
constexpr size_t clocksPerBit = 11;
constexpr size_t resetBytes = 400;
constexpr size_t payloadBytes = maxChainWords * bitsPerPixel * clocksPerBit / 8;
static_assert(bitsPerPixel * clocksPerBit % 8 == 0);
constexpr uint16_t driftStart = 650;
constexpr uint16_t driftEnd = 1960;
constexpr uint16_t exitEnd = 2850;
constexpr uint16_t cycleMs = 3500;
constexpr uint16_t framePowerLimit = 1150;
constexpr uint16_t maximumFramePower = 256 * 3 * 8;
constexpr uint16_t maximumBrightnessGain = 8;
uint8_t displayBrightness = 38;

struct Color {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

struct Position {
  float x;
  float y;
};

struct Pose {
  uint16_t time;
  Position center;
  float heading;
};

constexpr Pose trajectory[] = {
    {0, {4.4f, 18.3f}, 0.0f},
    {650, {4.4f, 11.6f}, 0.08f},
    {950, {4.6f, 8.5f}, 0.72f},
    {1300, {5.6f, 6.25f}, 1.31f},
    {1650, {7.5f, 4.65f}, 1.69f},
    {1960, {10.3f, 3.5f}, 1.57f},
    {2850, {19.3f, 3.5f}, 1.57f},
};

Color course[pixelCount];
Color pixels[pixelCount];
DMA_ATTR uint8_t frame[resetBytes + payloadBytes + resetBytes];
spi_device_handle_t panelBus = nullptr;
uint32_t startedAt = 0;
uint32_t frameCount = 0;
bool serpentineRows = false;
bool compensateSkippedWord = true;
enum class DisplayMode { Music, Calendar, Phrases, Dino, Pet, Lamp, NewYear, StripTest, Health, Boundary, Columns, RowScan, Drift };
DisplayMode displayMode = DisplayMode::Music;
String preferredMode = "calendar";
String controlOwner = "device";
Preferences settings;
bool settingsReady = false;
bool settingsDirty = false;
uint32_t settingsChangedAt = 0;
time_t lastCalendarSecond = 0;

struct WeatherReading {
  int temperature;
  uint8_t icon;
  time_t updatedAt;
  char condition[32];
};
WeatherReading savedWeather{};

bool clockIsValid();
void noteDesktopHeartbeat(uint8_t audioState);
bool validMode(const String& mode);
void rememberMode(const String& mode);
bool chooseMode(const String& mode);
void markSettingsDirty();
bool setClockEpoch(time_t epoch);
bool acceptCalendarPacket(const uint8_t* text, size_t length);
void printPanelState();
void releaseDesktopControl();
void startOfflineController();
void serviceOfflineController();

struct HealthPattern {
  const char* name;
  Color color;
};

constexpr HealthPattern healthPatterns[] = {
    {"red", {4, 0, 0}},
    {"green", {0, 4, 0}},
    {"blue", {0, 0, 4}},
    {"white", {1, 1, 1}},
    {"black", {0, 0, 0}},
};
constexpr uint32_t healthStepMs = 5000;
constexpr uint32_t boundaryStepMs = 1500;
constexpr size_t boundaryWords[] = {192, 193, 194, 195, 196, 197, 253, 254, 255, 256};
constexpr size_t boundaryMarkerFrame = maxChainWords;
constexpr size_t suspectPixelWord = 8 * matrixSize + 10;
int heldHealthPattern = -1;
size_t previousHealthStep = SIZE_MAX;
size_t previousBoundaryStep = SIZE_MAX;

constexpr size_t stripWidth = 32;
constexpr size_t stripHeight = 8;
static_assert(stripWidth * stripHeight == pixelCount);
constexpr uint8_t maxLampLevel = 8;
constexpr uint16_t lampPowerLimit = maximumFramePower;
constexpr size_t musicBands = stripWidth / 2;
constexpr uint8_t waveformBlank = stripHeight;
constexpr uint32_t musicTimeoutMs = 350;
uint8_t musicLevels[musicBands];
uint8_t musicPeaks[musicBands];
uint32_t musicPeakAt[musicBands];
uint32_t lastMusicFrameAt = 0;
uint32_t liveFrameTimeoutMs = musicTimeoutMs;
uint32_t musicFrames = 0;
uint32_t waveformFrames = 0;
bool audioHasSound = false;
bool musicLit = false;
constexpr uint32_t stripColorMs = 4500;
constexpr uint32_t stripRowMs = 800;
constexpr uint32_t stripPixelMs = 80;
constexpr uint32_t stripCheckerMs = 500;
constexpr size_t colorSteps = sizeof(healthPatterns) / sizeof(healthPatterns[0]);
constexpr size_t stripRowEnd = colorSteps + stripHeight;
constexpr size_t stripPixelEnd = stripRowEnd + pixelCount;
constexpr size_t stripCheckerSteps = 8;
constexpr uint32_t stripRowStartMs = colorSteps * stripColorMs;
constexpr uint32_t stripPixelStartMs = stripRowStartMs + stripHeight * stripRowMs;
constexpr uint32_t stripCheckerStartMs = stripPixelStartMs + pixelCount * stripPixelMs;
constexpr uint32_t stripCycleMs = stripCheckerStartMs + stripCheckerSteps * stripCheckerMs;
size_t previousStripStep = SIZE_MAX;
int heldStripColor = -1;
constexpr const char* greeting[stripHeight] = {
    ".#.....##.  .#.....  #...#...  .....#.",
    "#####.#...  .#####.  #..####.  .####..",
    ".#.#..#...  #..#...  ##..#.#.  .#.#...",
    "#####.####  .#####.  #.#####.  .#.#...",
    "..#...#.#.  .#.#...  #...#...  #######",
    "#####.#.#.  #######  #..#.#..  .#.#.#.",
    ".###..#.#.  ...#...  #.#...#.  #..#..#",
    "#.#.##..#.  ...#...  ##.....#  ..##...",
};
constexpr int greetingWidth = 38;
constexpr uint32_t lanternSceneMs = 2400;
constexpr uint32_t textStepMs = 112;
constexpr uint32_t textSceneMs = (stripWidth + greetingWidth) * textStepMs;
constexpr uint32_t fireworkSceneMs = 3800;
constexpr uint32_t newYearCycleMs = lanternSceneMs + textSceneMs + fireworkSceneMs;

constexpr Color columnColors[matrixSize] = {
    {4, 0, 0}, {0, 0, 4}, {3, 1, 0}, {0, 4, 0},
    {2, 0, 2}, {0, 2, 2}, {2, 2, 0}, {1, 0, 3},
    {1, 3, 0}, {3, 0, 1}, {0, 3, 1}, {2, 1, 1},
    {0, 1, 3}, {1, 2, 1}, {1, 1, 2}, {1, 1, 1},
};
bool columnsSent = false;

constexpr uint16_t rowStepMs = 400;
constexpr uint16_t pixelStepMs = 100;
constexpr uint16_t rowScanMs = matrixSize * rowStepMs;
constexpr uint16_t pixelScanMs = pixelCount * pixelStepMs;
constexpr uint16_t scanCycleMs = rowScanMs + pixelScanMs + 1000;
uint16_t previousScanStep = UINT16_MAX;

Pose carPose(uint16_t age) {
  for (size_t i = 1; i < sizeof(trajectory) / sizeof(trajectory[0]); ++i) {
    const Pose& next = trajectory[i];
    if (age <= next.time) {
      const Pose& previous = trajectory[i - 1];
      const float progress = static_cast<float>(age - previous.time) / (next.time - previous.time);
      return {age, {previous.center.x + (next.center.x - previous.center.x) * progress,
                    previous.center.y + (next.center.y - previous.center.y) * progress},
              previous.heading + (next.heading - previous.heading) * progress};
    }
  }
  return trajectory[sizeof(trajectory) / sizeof(trajectory[0]) - 1];
}

Position carPoint(const Pose& pose, float forward, float sideways) {
  const float sinHeading = sinf(pose.heading);
  const float cosHeading = cosf(pose.heading);
  return {pose.center.x + forward * sinHeading + sideways * cosHeading,
          pose.center.y - forward * cosHeading + sideways * sinHeading};
}

void glow(Position position, Color color, float strength = 1.0f) {
  const int left = static_cast<int>(floorf(position.x));
  const int top = static_cast<int>(floorf(position.y));
  for (int y = max(0, top); y <= min(matrixSize - 1, top + 1); ++y) {
    for (int x = max(0, left); x <= min(matrixSize - 1, left + 1); ++x) {
      const float weight = strength * (1.0f - fabsf(position.x - x)) * (1.0f - fabsf(position.y - y));
      Color& pixel = pixels[y * matrixSize + x];
      pixel.red = max(pixel.red, static_cast<uint8_t>(color.red * weight + 0.5f));
      pixel.green = max(pixel.green, static_cast<uint8_t>(color.green * weight + 0.5f));
      pixel.blue = max(pixel.blue, static_cast<uint8_t>(color.blue * weight + 0.5f));
    }
  }
}

void buildCourse() {
  constexpr float turnX = 10.0f;
  constexpr float turnY = 9.0f;
  constexpr float turnRadius = 5.5f;
  for (int y = 0; y < matrixSize; ++y) {
    for (int x = 0; x < matrixSize; ++x) {
      const float fromTurnX = max(0.0f, turnX - x);
      const float fromTurnY = max(0.0f, turnY - y);
      const float distance = fabsf(hypotf(fromTurnX, fromTurnY) - turnRadius);
      Color& pixel = course[y * matrixSize + x];
      if (distance >= 2.75f && distance < 3.65f) {
        const bool stripe = ((x + matrixSize - y) / 2) % 2 == 0;
        pixel = stripe ? Color{14, 2, 2} : Color{8, 8, 8};
      } else if (distance < 2.75f) {
        const bool dashed = distance < 0.55f && ((x + matrixSize - y) / 2) % 2 == 0;
        pixel = dashed ? Color{5, 5, 6} : Color{0, 0, 1};
      }
    }
  }
}

void drawDriftTrail(uint16_t age) {
  constexpr uint16_t emissionMs = 88;
  constexpr uint16_t smokeMs = 830;
  for (uint16_t born = driftStart; born < driftEnd && born <= age; born += emissionMs) {
    const uint16_t lifetime = age - born;
    if (lifetime >= smokeMs) {
      continue;
    }
    const Pose pose = carPose(born);
    const Position rear = carPoint(pose, -2.2f, -0.95f);
    const float time = static_cast<float>(lifetime) / smokeMs;
    const float heat = 1.0f - time;
    const int puff = (born - driftStart) / emissionMs;
    const Position cloud{rear.x - 1.6f * time, rear.y + 2.3f * time};
    glow(cloud, {27, 30, 34}, heat * heat);
    glow({cloud.x - 0.85f, cloud.y + 0.75f}, {16, 18, 22}, heat * heat);

    if (puff % 2 == 0 && lifetime < 285) {
      const float sparkTime = lifetime / 285.0f;
      const Position spark{rear.x - 2.3f * sparkTime, rear.y + 2.6f * sparkTime};
      glow(spark, {66, 35, 3}, 1.0f - sparkTime);
    }
  }
}

void drawCar(const Pose& pose) {
  const Color body{53, 3, 3};
  const Color nose{55, 14, 8};
  const Color glass{9, 22, 34};
  const Color tail{60, 14, 0};
  glow(carPoint(pose, 1.95f, 0.0f), nose);
  glow(carPoint(pose, 1.2f, 0.0f), body);
  glow(carPoint(pose, 0.65f, -1.0f), body);
  glow(carPoint(pose, 0.65f, 1.0f), body);
  glow(carPoint(pose, 0.3f, 0.0f), glass);
  glow(carPoint(pose, -0.7f, 0.0f), body);
  glow(carPoint(pose, -0.7f, -1.0f), body);
  glow(carPoint(pose, -0.7f, 1.0f), body);
  glow(carPoint(pose, -1.8f, -0.95f), tail);
  glow(carPoint(pose, -1.8f, 0.95f), tail);
}

void sendFrame(size_t rawWord = SIZE_MAX, Color rawColor = {16, 16, 16},
               uint16_t powerLimit = framePowerLimit) {
  uint32_t power = 0;
  for (const Color& pixel : pixels) {
    power += pixel.red + pixel.green + pixel.blue;
  }
  const uint32_t targetPower = min(static_cast<uint32_t>(powerLimit) * maximumBrightnessGain,
                                   static_cast<uint32_t>(maximumFramePower));
  uint32_t scale = maximumBrightnessGain * 65536UL;
  if (power > 0) {
    scale = min(scale, targetPower * 65536UL / power);
  }
  for (const Color& pixel : pixels) {
    const uint8_t peak = max(pixel.red, max(pixel.green, pixel.blue));
    if (peak > 0) {
      scale = min(scale, 255UL * 65536UL / peak);
    }
  }
  scale = scale * displayBrightness / 100;

  uint8_t* output = frame + resetBytes;
  uint32_t packed = 0;
  uint8_t pendingBits = 0;
  const bool rawProbe = rawWord != SIZE_MAX;
  const bool padded = compensateSkippedWord && !rawProbe;
  const size_t chainWords = rawProbe || padded ? maxChainWords : pixelCount;
  for (size_t word = 0; word < chainWords; ++word) {
    Color pixel{};
    if (rawProbe) {
      if (rawWord == boundaryMarkerFrame) {
        switch (word) {
          case panelPaddingWord - 1: pixel = {24, 0, 0}; break;
          case panelPaddingWord: pixel = {16, 16, 16}; break;
          case panelPaddingWord + 1: pixel = {0, 24, 0}; break;
          case panelPaddingWord + 2: pixel = {0, 0, 24}; break;
          case maxChainWords - 1: pixel = {20, 0, 20}; break;
        }
      } else if (word == rawWord) {
        pixel = rawColor;
      }
    } else if (!padded || word != panelPaddingWord) {
      const size_t physical = word - (padded && word > panelPaddingWord ? 1 : 0);
      const size_t y = physical / matrixSize;
      const size_t x = physical % matrixSize;
      const size_t column = (serpentineRows && y % 2 != 0) ? matrixSize - 1 - x : x;
      pixel = pixels[y * matrixSize + column];
    }
    const uint8_t grb[] = {pixel.green, pixel.red, pixel.blue};
    for (uint8_t component : grb) {
      const uint8_t value = (component * scale + 32768UL) / 65536UL;
      for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
        const bool one = (value & mask) != 0;
        // At 8 MHz these are 375/1000 ns for zero and 750/625 ns for one.
        constexpr uint16_t zeroPulse = 0b11100000000;
        constexpr uint16_t onePulse = 0b11111100000;
        packed = (packed << clocksPerBit) | (one ? onePulse : zeroPulse);
        pendingBits += clocksPerBit;
        while (pendingBits >= 8) {
          pendingBits -= 8;
          *output++ = static_cast<uint8_t>(packed >> pendingBits);
        }
      }
    }
  }
  spi_transaction_t transaction{};
  memset(output, 0, sizeof(frame) - static_cast<size_t>(output - frame));
  transaction.length = sizeof(frame) * 8;
  transaction.tx_buffer = frame;
  ESP_ERROR_CHECK(spi_device_transmit(panelBus, &transaction));
  ++frameCount;
}

void showHealth(uint32_t elapsed) {
  const size_t step = heldHealthPattern >= 0
                          ? static_cast<size_t>(heldHealthPattern)
                          : elapsed / healthStepMs % (sizeof(healthPatterns) / sizeof(healthPatterns[0]));
  if (step == previousHealthStep) {
    return;
  }
  previousHealthStep = step;
  const HealthPattern& pattern = healthPatterns[step];
  for (Color& pixel : pixels) {
    pixel = pattern.color;
  }
  sendFrame();
  Serial.printf("Panel health: %s, %u visible addresses, padding enabled\r\n",
                pattern.name, static_cast<unsigned>(pixelCount));
}

void setStripPixel(int x, int y, Color color) {
  if (x < 0 || x >= static_cast<int>(stripWidth) || y < 0 || y >= static_cast<int>(stripHeight)) {
    return;
  }
  // Viewed horizontally, the wire runs through eight LEDs per column.
  const size_t panelX = stripWidth - 1 - x;
  const size_t panelY = stripHeight - 1 - y;
  const size_t row = panelX % 2 == 0 ? panelY : stripHeight - 1 - panelY;
  Color& pixel = pixels[panelX * stripHeight + row];
  pixel = {max(pixel.red, color.red), max(pixel.green, color.green), max(pixel.blue, color.blue)};
}

enum class PetAction { Awake, Sleeping, Petted, Playing };
PetAction petAction = PetAction::Awake;
uint32_t petActionAt = 0;

void setPetAction(PetAction action, uint32_t now) {
  petAction = action;
  petActionAt = now;
  constexpr const char* events[] = {"awake", "sleeping", "petted", "playing"};
  Serial.printf("Pet 8x32 duck: %s\n", events[static_cast<size_t>(action)]);
}

void showPet(uint32_t now) {
  constexpr char duck[][14] = {
      ".......YYY...", "......YHEYY..", ".Y....YYYOOO.",
      ".YYYYYYYYYOO.", "..YYWWWWYYY..", "....SSSSS....",
  };
  constexpr Color yellow{25, 22, 2};
  constexpr Color wing{30, 27, 9};
  constexpr Color shade{18, 11, 1};
  constexpr Color beak{43, 12, 0};
  constexpr Color eye{0, 0, 1};
  uint32_t age = now - petActionAt;
  if (petAction == PetAction::Awake && age >= 16000) {
    setPetAction(PetAction::Sleeping, now);
    age = 0;
  } else if ((petAction == PetAction::Petted && age >= 1500) ||
             (petAction == PetAction::Playing && age >= 1900)) {
    setPetAction(PetAction::Awake, now);
    age = 0;
  }

  const bool sleeping = petAction == PetAction::Sleeping;
  const bool playing = petAction == PetAction::Playing;
  const bool happy = petAction == PetAction::Petted;
  const bool blink = sleeping || (petAction == PetAction::Awake && age % 3200 >= 3020);
  const bool flapping = (happy || playing) && age < 920;
  const int lift = flapping && age / 155 % 2 == 0;
  const int duckX = sleeping ? 9 : 8 + static_cast<int>(age / 620 % 3);
  const int duckY = 1 + (sleeping || age / 800 % 2 != 0) - lift;
  memset(pixels, 0, sizeof(pixels));

  for (int x = 0; x < static_cast<int>(stripWidth); ++x) {
    const int ripples = (x + age / 170) % 7;
    if (ripples < 4) {
      setStripPixel(x, 6, {0, 7, 15});
    }
    if (ripples >= 4) {
      setStripPixel(x, 7, {0, 5, 11});
    }
  }

  for (int y = 0; y < static_cast<int>(sizeof(duck) / sizeof(duck[0])); ++y) {
    for (int x = 0; x < 13; ++x) {
      const char part = duck[y][x];
      Color color = yellow;
      switch (part) {
        case '.': continue;
        case 'H': color = wing; break;
        case 'O': color = beak; break;
        case 'S': color = shade; break;
        case 'W': color = flapping && lift ? yellow : wing; break;
        case 'E': color = blink ? shade : eye; break;
      }
      if (sleeping) {
        color = {static_cast<uint8_t>(3 * color.red / 5),
                 static_cast<uint8_t>(3 * color.green / 5),
                 static_cast<uint8_t>(3 * color.blue / 5)};
      }
      setStripPixel(duckX + x, duckY + y, color);
    }
  }

  if (flapping && lift) {
    setStripPixel(duckX + 4, duckY + 2, wing);
    setStripPixel(duckX + 5, duckY + 3, wing);
    setStripPixel(duckX + 6, duckY + 4, wing);
  }

  if (happy) {
    const int y = 3 - min<uint32_t>(3, age / 260);
    constexpr Color heart{33, 2, 11};
    setStripPixel(24, y, heart);
    setStripPixel(26, y, heart);
    for (int x = 24; x <= 26; ++x) {
      setStripPixel(x, y + 1, heart);
    }
    setStripPixel(25, y + 2, heart);
  } else if (playing) {
    const int step = age % 620 / 85;
    constexpr int splashY[] = {6, 4, 2, 1, 1, 2, 4, 6};
    constexpr Color splash{2, 28, 36};
    const int distance = 2 + step / 3;
    setStripPixel(duckX - distance, splashY[step], splash);
    setStripPixel(duckX + 11 + distance, splashY[step] + 1, splash);
    setStripPixel(duckX - distance - 1, splashY[step] + 1, {0, 13, 20});
    for (int x = 1; x < static_cast<int>(stripWidth); x += 2) {
      setStripPixel(x, 7, {0, 12, 22});
    }
  } else if (sleeping) {
    const int y = 3 - static_cast<int>(age / 520 % 4);
    constexpr Color sleepMark{5, 11, 17};
    for (int x = 24; x <= 26; ++x) {
      setStripPixel(x, y, sleepMark);
      setStripPixel(x, y + 2, sleepMark);
    }
    setStripPixel(25, y + 1, sleepMark);
  }
  sendFrame();
}

enum class DinoState { Ready, Running, Crashed };
constexpr int dinoX = 3;
constexpr int dinoWidth = 5;
constexpr int dinoHeight = 4;
constexpr int groundY = stripHeight - 1;
constexpr uint32_t jumpMs = 1100;
constexpr uint32_t jumpRiseMs = 190;
constexpr uint32_t jumpFallMs = 260;
constexpr char dinoBody[][dinoWidth + 1] = {"..GeG", "..GGG", "GGGG.", ".G.G."};
DinoState dinoState = DinoState::Ready;
int cactusX = stripWidth - 1;
int cactusWidth = 2;
unsigned dinoScore = 0;
uint32_t dinoStepAt = 0;
uint32_t dinoJumpAt = 0;
uint32_t dinoFrameAt = 0;
bool dinoJumping = false;
bool dinoPassed = false;

void reportDino(const char* event) {
  Serial.printf("Dino 8x32: %s, score=%u\n", event, dinoScore);
}

void startDino(uint32_t now) {
  dinoState = DinoState::Running;
  dinoScore = 0;
  cactusX = stripWidth - 1;
  cactusWidth = 2;
  dinoStepAt = now;
  dinoJumping = false;
  dinoPassed = false;
  reportDino("running");
}

void handleDinoJump(uint32_t now) {
  if (dinoState != DinoState::Running) {
    startDino(now);
  } else if (!dinoJumping) {
    dinoJumpAt = now;
    dinoJumping = true;
    reportDino("jump");
  }
}

void showDino(uint32_t now) {
  if (now - dinoFrameAt < 28) {
    return;
  }
  dinoFrameAt = now;
  int lift = 0;
  if (dinoState == DinoState::Running) {
    if (dinoJumping) {
      const uint32_t airTime = now - dinoJumpAt;
      dinoJumping = airTime < jumpMs;
      if (dinoJumping) {
        const uint32_t rise = (3 * airTime + jumpRiseMs / 2) / jumpRiseMs;
        const uint32_t fall = (3 * (jumpMs - airTime) + jumpFallMs / 2) / jumpFallMs;
        lift = min<uint32_t>(3, min(rise, fall));
      }
    }
    const uint32_t stepMs = 115 - min(dinoScore, 10u) * 3;
    if (now - dinoStepAt >= stepMs) {
      dinoStepAt += stepMs;
      --cactusX;
      if (!dinoPassed && cactusX + cactusWidth <= dinoX) {
        dinoPassed = true;
        ++dinoScore;
        reportDino("passed");
      }
      if (cactusX + cactusWidth <= 0) {
        cactusX = stripWidth + 2 + dinoScore % 4;
        cactusWidth = dinoScore % 3 == 0 ? 2 : 1;
        dinoPassed = false;
      }
    }
    for (int y = 0; y < dinoHeight; ++y) {
      if (groundY - dinoHeight - lift + y < groundY - 2) {
        continue;
      }
      for (int x = 0; x < dinoWidth; ++x) {
        const int column = dinoX + x;
        if (dinoBody[y][x] != '.' && column >= cactusX && column < cactusX + cactusWidth) {
          dinoState = DinoState::Crashed;
        }
      }
    }
    if (dinoState == DinoState::Crashed) {
      reportDino("crashed");
    }
  }

  memset(pixels, 0, sizeof(pixels));
  for (int x = 0; x < static_cast<int>(stripWidth); ++x) {
    const bool dash = ((x + cactusX) & 3) == 0;
    setStripPixel(x, groundY, dash ? Color{15, 8, 2} : Color{5, 3, 1});
  }
  for (int x = 0; x < cactusWidth; ++x) {
    setStripPixel(cactusX + x, groundY - 2, {24, 9, 0});
    setStripPixel(cactusX + x, groundY - 1, {38, 14, 1});
  }
  const Color bodyColor = dinoState == DinoState::Crashed ? Color{39, 2, 2} : Color{2, 33, 13};
  for (int y = 0; y < dinoHeight; ++y) {
    for (int x = 0; x < dinoWidth; ++x) {
      const char pixel = dinoBody[y][x];
      if (pixel != '.') {
        setStripPixel(dinoX + x, groundY - dinoHeight - lift + y,
                      pixel == 'e' ? Color{22, 24, 15} : bodyColor);
      }
    }
  }
  if (dinoState == DinoState::Running && !dinoJumping && now / 120 % 2 != 0) {
    setStripPixel(dinoX + 2, groundY - 1, {2, 21, 8});
  }
  if (dinoState == DinoState::Crashed) {
    for (int x = 0; x < static_cast<int>(stripWidth); ++x) {
      setStripPixel(x, 0, {21, 0, 0});
    }
  }
  sendFrame();
}

void sendLiveFrame(uint32_t now, uint32_t timeoutMs = musicTimeoutMs) {
  displayMode = DisplayMode::Music;
  compensateSkippedWord = false;
  serpentineRows = false;
  sendFrame();
  lastMusicFrameAt = now;
  liveFrameTimeoutMs = timeoutMs;
  musicLit = true;
}

void showLamp(uint8_t level) {
  displayMode = DisplayMode::Lamp;
  compensateSkippedWord = false;
  serpentineRows = false;
  for (Color& pixel : pixels) {
    const uint8_t white = level * 255 / maxLampLevel;
    pixel = {white, white, white};
  }
  sendFrame(SIZE_MAX, {}, lampPowerLimit);
  Serial.printf("Desktop lamp 8x32: %u/%u\n", level, maxLampLevel);
}

Color waveformColor(size_t column, uint32_t now);

Color chaseGlow(Color color, unsigned numerator, unsigned denominator) {
  return {static_cast<uint8_t>(color.red * numerator / denominator),
          static_cast<uint8_t>(color.green * numerator / denominator),
          static_cast<uint8_t>(color.blue * numerator / denominator)};
}

void showIdleWaveform(uint32_t now) {
  const unsigned head = now / 45 % stripWidth;
  for (unsigned x = 0; x < stripWidth; ++x) {
    const unsigned distance = (head + stripWidth - x) % stripWidth;
    const unsigned brightness = distance < 9 ? 12 - distance : 3;
    setStripPixel(x, stripHeight / 2 - 1,
                  chaseGlow(waveformColor(x, now), brightness, 12));
  }
}

void showIdleSpectrum(uint32_t now) {
  constexpr unsigned perimeter = 2 * (stripWidth + stripHeight) - 4;
  constexpr unsigned trail = 16;
  constexpr unsigned bottomStart = stripWidth + stripHeight - 1;
  constexpr unsigned leftStart = bottomStart + stripWidth - 1;
  const unsigned head = now / 45 % perimeter;
  for (unsigned distance = 0; distance < trail; ++distance) {
    const unsigned position = (head + perimeter - distance) % perimeter;
    int x;
    int y;
    if (position < stripWidth) {
      x = position;
      y = 0;
    } else if (position < bottomStart) {
      x = stripWidth - 1;
      y = position - stripWidth + 1;
    } else if (position < leftStart) {
      x = stripWidth - 2 - (position - bottomStart);
      y = stripHeight - 1;
    } else {
      x = 0;
      y = stripHeight - 2 - (position - leftStart);
    }
    setStripPixel(x, y, chaseGlow(waveformColor(head + distance, now / 2), trail - distance, trail));
  }
}

void showMusicFrame(const uint8_t (&levels)[musicBands], uint32_t now) {
  constexpr Color gradients[] = {
      {2, 24, 27}, {2, 18, 30}, {4, 10, 34}, {10, 5, 36},
      {18, 3, 32}, {30, 2, 21}, {39, 5, 6}, {44, 15, 0},
  };
  static uint32_t lastSoundAt = 0;
  static bool wasIdle = false;
  const uint8_t* peak = std::max_element(levels, levels + musicBands);
  audioHasSound = *peak != 0;
  if (*peak != 0) {
    lastSoundAt = now;
  }
  const bool idle = *peak == 0 && (lastSoundAt == 0 || now - lastSoundAt >= 230);
  memset(pixels, 0, sizeof(pixels));
  if (idle) {
    memset(musicPeaks, 0, sizeof(musicPeaks));
    showIdleSpectrum(now);
  } else {
    for (size_t band = 0; band < musicBands; ++band) {
      const uint8_t level = levels[band];
      if (level >= musicPeaks[band]) {
        musicPeaks[band] = level;
        musicPeakAt[band] = now;
      } else if (now - musicPeakAt[band] >= 100) {
        --musicPeaks[band];
        musicPeakAt[band] = now;
      }
      for (uint8_t row = 0; row < level; ++row) {
        const Color core = gradients[row];
        setStripPixel(band * 2, stripHeight - 1 - row, core);
        setStripPixel(band * 2 + 1, stripHeight - 1 - row, chaseGlow(core, 1, 3));
      }
      if (musicPeaks[band] > 0) {
        setStripPixel(band * 2, stripHeight - musicPeaks[band], {32, 27, 14});
      }
    }
  }
  sendLiveFrame(now);
  if (++musicFrames % 32 == 1 || idle != wasIdle) {
    Serial.printf("Music 8x32: frames=%lu band=%u height=%u idle=%u\n",
                  static_cast<unsigned long>(musicFrames),
                  static_cast<unsigned>(peak - levels), static_cast<unsigned>(*peak), idle);
  }
  wasIdle = idle;
}

Color waveformColor(size_t column, uint32_t now) {
  constexpr Color rainbow[] = {
      {34, 0, 0}, {31, 11, 0}, {19, 20, 0}, {0, 33, 1},
      {0, 20, 22}, {2, 4, 34}, {17, 0, 23}, {30, 0, 10},
  };
  constexpr size_t colorCount = sizeof(rainbow) / sizeof(rainbow[0]);
  constexpr uint16_t cycle = 256;
  constexpr uint16_t segmentWidth = cycle / colorCount;
  const uint8_t hue = column * (cycle / stripWidth) - now / 24;
  const size_t segment = hue / segmentWidth;
  const uint16_t mix = hue % segmentWidth;
  const Color from = rainbow[segment];
  const Color to = rainbow[(segment + 1) % colorCount];
  const auto blend = [mix](uint8_t first, uint8_t second) {
    return static_cast<uint8_t>((first * (segmentWidth - mix) + second * mix) / segmentWidth);
  };
  return {blend(from.red, to.red), blend(from.green, to.green), blend(from.blue, to.blue)};
}

void showWaveform(const uint8_t* rows, uint32_t now) {
  static uint32_t lastSoundAt = 0;
  static bool wasIdle = false;
  memset(pixels, 0, sizeof(pixels));
  uint8_t top = waveformBlank;
  uint8_t bottom = 0;
  for (size_t x = 0; x < stripWidth; ++x) {
    const uint8_t row = rows[x];
    if (row == waveformBlank) {
      continue;
    }
    top = min(top, row);
    bottom = max(bottom, row);
    const Color color = waveformColor(x, now);
    if (x > 0 && rows[x - 1] < waveformBlank) {
      const uint8_t previous = rows[x - 1];
      const Color dim{static_cast<uint8_t>(color.red / 5),
                      static_cast<uint8_t>(color.green / 5),
                      static_cast<uint8_t>(color.blue / 5)};
      for (uint8_t y = min(row, previous) + 1; y < max(row, previous); ++y) {
        setStripPixel(x, y, dim);
      }
    }
    setStripPixel(x, row, color);
  }
  if (top != waveformBlank) {
    lastSoundAt = now;
  }
  const bool idle = top == waveformBlank && (lastSoundAt == 0 || now - lastSoundAt >= 230);
  audioHasSound = top != waveformBlank;
  if (idle) {
    showIdleWaveform(now);
  }
  sendLiveFrame(now);
  if (++waveformFrames % 32 == 1 || idle != wasIdle) {
    Serial.printf("Waveform 8x32: rainbow frames=%lu top=%u bottom=%u idle=%u\n",
                  static_cast<unsigned long>(waveformFrames),
                  static_cast<unsigned>(top), static_cast<unsigned>(bottom), idle);
  }
  wasIdle = idle;
}

struct PanelGlyph {
  char character;
  uint16_t bitmap;
};

constexpr PanelGlyph panelGlyphs[] = {
    {'0', 075557}, {'1', 026227}, {'2', 071747}, {'3', 071717}, {'4', 055711},
    {'5', 074717}, {'6', 074757}, {'7', 071111}, {'8', 075757}, {'9', 075717},
    {'A', 025755}, {'C', 074447}, {'D', 065556}, {'E', 074647}, {'F', 074644},
    {'H', 055755}, {'I', 072227}, {'M', 057755}, {'O', 025552}, {'R', 065655},
    {'S', 074717}, {'T', 072222}, {'U', 055557}, {'W', 055775},
    {'-', 000700},
};

void drawStripGlyph(char character, int x, int y, Color color) {
  if (character == ':') {
    setStripPixel(x, y + 1, color);
    setStripPixel(x, y + 3, color);
    return;
  }
  for (const PanelGlyph& glyph : panelGlyphs) {
    if (glyph.character != character) {
      continue;
    }
    for (int row = 0; row < 5; ++row) {
      for (int column = 0; column < 3; ++column) {
        if (glyph.bitmap & (1u << (14 - 3 * row - column))) {
          setStripPixel(x + column, y + row, color);
        }
      }
    }
    return;
  }
}

void drawCalendarDigit(uint8_t digit, int x, int y, Color color) {
  constexpr uint16_t digits[] = {
      07557, 02627, 07167, 07317, 05571,
      07437, 04757, 07111, 07257, 07571,
  };
  if (digit < '0' || digit > '9') {
    return;
  }
  const uint16_t bitmap = digits[digit - '0'];
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 3; ++column) {
      if (bitmap & (1u << (11 - 3 * row - column))) {
        setStripPixel(x + column, y + row, color);
      }
    }
  }
}

void drawClockDigit(uint8_t digit, int x, Color color) {
  constexpr uint32_t digits[] = {
      07555557, 02622227, 07117447, 07117117, 05557111,
      07447117, 07447557, 07111111, 07557557, 07557117,
  };
  if (digit < '0' || digit > '9') {
    return;
  }
  const uint32_t bitmap = digits[digit - '0'];
  for (int row = 0; row < 7; ++row) {
    for (int column = 0; column < 3; ++column) {
      if (bitmap & (1UL << (20 - 3 * row - column))) {
        setStripPixel(x + column, row, color);
      }
    }
  }
}

void drawWeatherIcon(unsigned weather) {
  constexpr char icons[][4][5] = {
      {".SS.", "SSSS", "SSSS", ".SS."},
      {".MM.", "MM..", "MM..", ".MM."},
      {".CC.", "CCCC", "CCCC", "...."},
      {".CC.", "CCCC", ".R.R", "R.R."},
      {".CC.", "CCCC", "..SS", ".SS."},
      {".CC.", "CCCC", ".D.D", "D.D."},
      {"HHH.", "....", ".HHH", "...."},
      {"CCC.", "..C.", "....", ".C.."},
  };
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      switch (icons[weather][y][x]) {
        case 'S': setStripPixel(17 + x, 4 + y, {28, 20, 4}); break;
        case 'M': setStripPixel(17 + x, 4 + y, {22, 22, 28}); break;
        case 'C': setStripPixel(17 + x, 4 + y, {13, 16, 22}); break;
        case 'R': setStripPixel(17 + x, 4 + y, {0, 8, 26}); break;
        case 'D': setStripPixel(17 + x, 4 + y, {24, 24, 27}); break;
        case 'H': setStripPixel(17 + x, 4 + y, {24, 0, 26}); break;
      }
    }
  }
}

constexpr size_t calendarLength = 14;
constexpr size_t weatherCalendarLength = calendarLength + 4;
constexpr size_t statusLength = 9;

void showCalendar(const uint8_t* text, size_t length, uint32_t now, bool report = true) {
  const unsigned seconds = (text[6] - '0') * 10 + text[7] - '0';
  const unsigned weekday = text[13] - '0';
  if (text[2] != ':' || text[5] != ':' || text[10] != '-' || seconds > 59 || weekday > 6) {
    return;
  }
  constexpr int digitX[] = {0, 4, 9, 13};
  constexpr size_t digitIndex[] = {0, 1, 3, 4};
  constexpr Color clockInk{18, 18, 18};
  constexpr Color calendarInk{0, 25, 1};
  memset(pixels, 0, sizeof(pixels));
  for (size_t digit = 0; digit < 4; ++digit) {
    drawClockDigit(text[digitIndex[digit]], digitX[digit], clockInk);
    const int dateX = 17 + static_cast<int>(digit) * 4;
    drawCalendarDigit(text[8 + digitIndex[digit]], dateX, 0, calendarInk);
  }
  setStripPixel(7, 2, clockInk);
  setStripPixel(7, 4, clockInk);
  setStripPixel(24, 1, {0, 12, 0});

  const bool weatherReady = length == weatherCalendarLength &&
      (text[14] == '+' || text[14] == '-') &&
      text[15] >= '0' && text[15] <= '9' && text[16] >= '0' && text[16] <= '9' &&
      text[17] >= '0' && text[17] <= '7';
  drawWeatherIcon(weatherReady ? text[17] - '0' : 7);
  if (weatherReady) {
    constexpr Color temperatureInk{0, 20, 26};
    const bool twoDigits = text[15] != '0';
    const bool negative = text[14] == '-';
    if (twoDigits) {
      drawCalendarDigit(text[15], 22, 4, temperatureInk);
      drawCalendarDigit(text[16], 26, 4, temperatureInk);
    } else {
      drawCalendarDigit(text[16], 26, 4, temperatureInk);
    }
    if (negative) {
      setStripPixel(twoDigits ? 21 : 24, 6, temperatureInk);
    }
    for (int row = 4; row < 8; ++row) {
      setStripPixel(30, row, temperatureInk);
    }
    setStripPixel(31, 4, temperatureInk);
    setStripPixel(31, 7, temperatureInk);
  } else {
    for (int x = 0; x < 3; ++x) {
      setStripPixel(22 + x, 6, {7, 7, 7});
      setStripPixel(26 + x, 6, {7, 7, 7});
    }
  }

  displayMode = DisplayMode::Calendar;
  compensateSkippedWord = false;
  serpentineRows = false;
  sendFrame();
  if (report) {
    Serial.printf("Desktop calendar 8x32: %.*s\n", static_cast<int>(length),
                  reinterpret_cast<const char*>(text));
  }
}

void showComputerStatus(const uint8_t* encoded, uint32_t now) {
  unsigned percentages[3];
  for (size_t metric = 0; metric < 3; ++metric) {
    const uint8_t* digits = encoded + metric * 3;
    percentages[metric] = (digits[0] - '0') * 100 + (digits[1] - '0') * 10 + digits[2] - '0';
    if (percentages[metric] > 100) {
      return;
    }
  }
  memset(pixels, 0, sizeof(pixels));
  constexpr char labels[] = "CMD";  // CPU, memory, and system drive.
  for (int metric = 0; metric < 3; ++metric) {
    const unsigned percentage = percentages[metric];
    const int startX = metric * 11;
    drawStripGlyph(labels[metric], startX + 3, 0, {10, 18, 22});
    const Color color = percentage >= 85 ? Color{36, 3, 1}
                        : percentage >= 65 ? Color{29, 14, 1} : Color{1, 26, 10};
    const unsigned length = (percentage + 9) / 10;
    for (unsigned column = 0; column < 10; ++column) {
      const Color level = column < length ? color : Color{1, 2, 3};
      for (int row = 6; row <= 7; ++row) {
        setStripPixel(startX + column, row, level);
      }
    }
  }
  sendLiveFrame(now, 2600);
  Serial.printf("Desktop status: CPU=%u memory=%u drive=%u\n",
                percentages[0], percentages[1], percentages[2]);
}

int readPanelCommand() {
  static size_t count = 0;
  static int packetType = 0;
  static uint8_t packet[stripWidth];
  static uint32_t packetAt = 0;
  const uint32_t now = millis();
  if (packetType && now - packetAt > 120) {
    packetType = 0;
  }
  while (Serial.available()) {
    const int byte = Serial.read();
    if (!packetType && (byte == 'F' || byte == 'W' || byte == 'Q' || byte == 'K' ||
                        byte == 'I' || byte == 'V' || byte == 'E' || byte == 'M' || byte == 'O')) {
      count = 0;
      packetAt = now;
      packetType = byte;
    } else if (!packetType) {
      return byte;
    } else if (byte == '\n') {
      const bool fromDesktop = controlOwner != "phone";
      if (packetType == 'O' && count == 1 && packet[0] >= '0' && packet[0] <= '2') {
        noteDesktopHeartbeat(packet[0] - '0');
      } else if (packetType == 'M' && count > 0) {
        packet[count] = 0;
        const String mode(reinterpret_cast<const char*>(packet));
        if (validMode(mode)) {
          const bool continuing = controlOwner == "desktop" && preferredMode == mode;
          controlOwner = "desktop";
          if (!continuing) {
            chooseMode(mode);
          }
          printPanelState();
        }
      } else if (packetType == 'E' && count == 10) {
        char timestamp[11]{};
        memcpy(timestamp, packet, count);
        if (setClockEpoch(strtoll(timestamp, nullptr, 10))) {
          Serial.printf("Panel time: %lld\n", static_cast<long long>(time(nullptr)));
        }
      } else if (packetType == 'W' && count == stripWidth &&
                 (fromDesktop || preferredMode == "waveform")) {
        rememberMode("waveform");
        if (fromDesktop) {
          controlOwner = "desktop";
        }
        showWaveform(packet, now);
      } else if (packetType == 'F' && count == musicBands &&
                 (fromDesktop || preferredMode == "spectrum")) {
        rememberMode("spectrum");
        if (fromDesktop) {
          controlOwner = "desktop";
        }
        memcpy(musicLevels, packet, musicBands);
        showMusicFrame(musicLevels, now);
      } else if (packetType == 'Q' && (count == calendarLength || count == weatherCalendarLength)) {
        if (acceptCalendarPacket(packet, count) && fromDesktop) {
          rememberMode("calendar");
          controlOwner = "desktop";
          showCalendar(packet, count, now);
        }
      } else if (packetType == 'K' && count == statusLength && fromDesktop) {
        rememberMode("status");
        controlOwner = "desktop";
        showComputerStatus(packet, now);
      } else if (packetType == 'I' && count == 1 && packet[0] <= '0' + maxLampLevel && fromDesktop) {
        rememberMode("lamp");
        controlOwner = "desktop";
        showLamp(packet[0] - '0');
      } else if (packetType == 'V' && count == 3 && fromDesktop) {
        const unsigned brightness = (packet[0] - '0') * 100 +
                                    (packet[1] - '0') * 10 + packet[2] - '0';
        if (brightness <= 100) {
          if (displayBrightness != brightness) {
            displayBrightness = brightness;
            markSettingsDirty();
          }
          sendFrame(SIZE_MAX, {}, displayMode == DisplayMode::Lamp ? lampPowerLimit : framePowerLimit);
          Serial.printf("Desktop brightness: %u%%\n", displayBrightness);
        }
      }
      packetType = 0;
    } else {
      const bool modeLetter = packetType == 'M' && byte >= 'a' && byte <= 'z';
      const bool waveRow = packetType == 'W' && byte >= 'a' && byte <= 'a' + waveformBlank;
      const bool digit = packetType != 'W' && byte >= '0' && byte <= (packetType == 'F' ? '8' : '9');
      const bool calendarSeparator = packetType == 'Q' && (byte == ':' || byte == '-' || byte == '+');
      const size_t capacity = packetType == 'M' ? sizeof(packet) - 1 : sizeof(packet);
      if (count < capacity && (modeLetter || waveRow || digit || calendarSeparator)) {
        packet[count++] = packetType == 'W' ? byte - 'a' : packetType == 'F' ? byte - '0' : byte;
      } else {
        packetType = 0;
      }
    }
  }
  return -1;
}

void drawLantern(int x, int swing) {
  constexpr Color gold{35, 15, 0};
  constexpr Color red{36, 2, 0};
  constexpr Color warm{32, 8, 0};
  setStripPixel(x, 0, gold);
  x += swing;
  for (int dx = -1; dx <= 1; ++dx) {
    setStripPixel(x + dx, 1, gold);
    setStripPixel(x + dx, 5, gold);
  }
  for (int y = 2; y <= 4; ++y) {
    for (int dx = -2; dx <= 2; ++dx) {
      setStripPixel(x + dx, y, dx == 0 ? warm : red);
    }
  }
  setStripPixel(x, 6, gold);
  setStripPixel(x + swing, 7, {23, 5, 0});
}

void drawGreeting(uint32_t age) {
  const int left = static_cast<int>(stripWidth) - 1 - static_cast<int>(age / textStepMs);
  for (int y = 0; y < static_cast<int>(stripHeight); ++y) {
    const Color ink = y < 3 ? Color{32, 16, 0} : Color{35, 7, 0};
    for (int x = 0; greeting[y][x] != '\0'; ++x) {
      if (greeting[y][x] == '#') {
        setStripPixel(left + x, y, ink);
      }
    }
  }
}

void drawFirework(int x, int y, int age, Color outer) {
  if (age < 0 || age >= 1850) {
    return;
  }
  if (age < 500) {
    const int head = 7 - age * (8 - y) / 500;
    setStripPixel(x, head, {50, 28, 3});
    setStripPixel(x, head + 1, {30, 9, 0});
    setStripPixel(x, head + 2, {14, 2, 0});
    return;
  }
  const float flight = (age - 500) / 1350.0f;
  const float spread = 1.0f + flight * 6.0f;
  const float gravity = flight * flight * 2.7f;
  const float fadeLevel = 1.0f - flight;
  if (age < 630) {
    setStripPixel(x, y, {55, 42, 18});
    setStripPixel(x - 1, y, {35, 20, 3});
    setStripPixel(x + 1, y, {35, 20, 3});
  }
  for (int spark = 0; spark < 12; ++spark) {
    const float angle = spark * TWO_PI / 12;
    const float radius = spread * (spark % 2 == 0 ? 1.0f : 0.73f);
    const int px = x + lroundf(cosf(angle) * radius);
    const int py = y + lroundf(sinf(angle) * radius * 0.65f + gravity);
    const Color color = spark % 2 == 0 ? outer : Color{48, 24, 2};
    setStripPixel(px, py, {static_cast<uint8_t>(color.red * fadeLevel),
                           static_cast<uint8_t>(color.green * fadeLevel),
                           static_cast<uint8_t>(color.blue * fadeLevel)});
  }
}

void showNewYear(uint32_t elapsed) {
  const uint32_t age = elapsed % newYearCycleMs;
  const char* scene;
  memset(pixels, 0, sizeof(pixels));
  if (age < lanternSceneMs) {
    constexpr int sway[] = {0, 1, 1, 0, -1, -1};
    const int swing = sway[age / 200 % 6];
    drawLantern(4, swing);
    drawLantern(12, -swing);
    drawLantern(20, swing);
    drawLantern(27, -swing);
    scene = "lanterns";
  } else if (age < lanternSceneMs + textSceneMs) {
    drawGreeting(age - lanternSceneMs);
    scene = "greeting";
  } else {
    const int burstAge = age - lanternSceneMs - textSceneMs;
    drawFirework(6, 3, burstAge, {47, 8, 0});
    drawFirework(24, 2, burstAge - 590, {50, 19, 1});
    drawFirework(15, 3, burstAge - 1270, {48, 3, 0});
    scene = "fireworks";
  }
  sendFrame();
  static const char* previousScene = nullptr;
  if (scene != previousScene) {
    Serial.printf("New Year 8x32: %s, 256 words, column serpentine\r\n", scene);
    previousScene = scene;
  }
}

void showStripTest(uint32_t elapsed) {
  const uint32_t age = elapsed % stripCycleMs;
  size_t step;
  if (heldStripColor >= 0) {
    step = static_cast<size_t>(heldStripColor);
  } else if (age < stripRowStartMs) {
    step = age / stripColorMs;
  } else if (age < stripPixelStartMs) {
    step = colorSteps + (age - stripRowStartMs) / stripRowMs;
  } else if (age < stripCheckerStartMs) {
    step = stripRowEnd + (age - stripPixelStartMs) / stripPixelMs;
  } else {
    step = stripPixelEnd + (age - stripCheckerStartMs) / stripCheckerMs;
  }

  if (step == previousStripStep) {
    return;
  }
  previousStripStep = step;
  memset(pixels, 0, sizeof(pixels));
  if (step < colorSteps) {
    const HealthPattern& pattern = healthPatterns[step];
    for (Color& pixel : pixels) {
      pixel = pattern.color;
    }
    sendFrame();
    Serial.printf("Strip 8x32: color %s, 256 words\r\n", pattern.name);
  } else if (step < stripRowEnd) {
    const size_t row = step - colorSteps;
    Color* line = pixels + row * stripWidth;
    for (size_t column = 0; column < stripWidth; ++column) {
      line[column] = {0, 6, 0};
    }
    line[0] = {18, 0, 0};
    line[stripWidth - 1] = {0, 0, 18};
    sendFrame();
    Serial.printf("Strip 8x32: row %u, words %u..%u\r\n",
                  static_cast<unsigned>(row + 1), static_cast<unsigned>(row * stripWidth),
                  static_cast<unsigned>((row + 1) * stripWidth - 1));
  } else if (step < stripPixelEnd) {
    const size_t word = step - stripRowEnd;
    pixels[word] = {8, 8, 8};
    sendFrame();
    Serial.printf("Strip 8x32: pixel %u\r\n", static_cast<unsigned>(word));
  } else {
    const size_t phase = step - stripPixelEnd;
    for (size_t word = 0; word < pixelCount; ++word) {
      const size_t row = word / stripWidth;
      const size_t column = word % stripWidth;
      if ((row + column + phase) % 2 == 0) {
        pixels[word] = {2, 2, 2};
      }
    }
    sendFrame();
    Serial.printf("Strip 8x32: alternating pixels %u\r\n", static_cast<unsigned>(phase + 1));
  }
}

void showBoundary(uint32_t elapsed) {
  const size_t step = elapsed / boundaryStepMs % (sizeof(boundaryWords) / sizeof(boundaryWords[0]));
  if (step == previousBoundaryStep) {
    return;
  }
  previousBoundaryStep = step;
  memset(pixels, 0, sizeof(pixels));
  const size_t word = boundaryWords[step];
  sendFrame(word);
  Serial.printf("Panel boundary: raw word %u\r\n", static_cast<unsigned>(word));
}

void scanRows(uint32_t elapsed) {
  const uint16_t age = elapsed % scanCycleMs;
  uint16_t step = matrixSize + pixelCount;
  if (age < rowScanMs) {
    step = age / rowStepMs;
  } else if (age < rowScanMs + pixelScanMs) {
    step = matrixSize + (age - rowScanMs) / pixelStepMs;
  }

  if (step == previousScanStep) {
    return;
  }
  previousScanStep = step;
  memset(pixels, 0, sizeof(pixels));
  if (step < matrixSize) {
    const size_t first = step * matrixSize;
    for (size_t column = 1; column < matrixSize - 1; ++column) {
      pixels[first + column] = {0, 12, 0};
    }
    pixels[first] = {24, 0, 0};
    pixels[first + matrixSize - 1] = {0, 0, 24};
    sendFrame();
    Serial.printf("Row scan: row %u, indices %u..%u\r\n",
                  static_cast<unsigned>(step + 1), static_cast<unsigned>(first),
                  static_cast<unsigned>(first + matrixSize - 1));
  } else if (step < matrixSize + pixelCount) {
    const size_t index = step - matrixSize;
    const size_t column = index % matrixSize;
    pixels[index] = column == 0 ? Color{24, 0, 0}
                               : column == matrixSize - 1 ? Color{0, 0, 24}
                                                          : Color{12, 12, 12};
    sendFrame();
    if (column == 0 || column == matrixSize - 1) {
      Serial.printf("Pixel scan: row %u, index %u\r\n",
                    static_cast<unsigned>(index / matrixSize + 1), static_cast<unsigned>(index));
    }
  } else {
    sendFrame();
  }
}

void showColumns() {
  if (columnsSent) {
    return;
  }
  columnsSent = true;
  serpentineRows = false;
  for (size_t row = 0; row < matrixSize; ++row) {
    memcpy(pixels + row * matrixSize, columnColors, sizeof(columnColors));
  }
  sendFrame();
  Serial.printf("16 color columns: linear, %u words, %s\r\n",
                static_cast<unsigned>(pixelCount + (compensateSkippedWord ? 1 : 0)),
                compensateSkippedWord ? "pad at row 13 column 3" : "no padding");
}

void showStripLayout() {
  memset(pixels, 0, sizeof(pixels));
  pixels[0] = {34, 0, 0};
  pixels[1] = {0, 34, 0};
  pixels[7] = {0, 0, 34};
  pixels[8] = {25, 22, 0};
  pixels[16] = {26, 0, 13};
  pixels[31] = {12, 12, 12};
  pixels[32] = {33, 8, 0};
  pixels[63] = {16, 0, 25};
  pixels[64] = {0, 23, 20};
  pixels[128] = {12, 28, 0};
  pixels[255] = {11, 11, 14};
  compensateSkippedWord = false;
  serpentineRows = false;
  sendFrame();
  displayMode = DisplayMode::Columns;
  columnsSent = true;
  Serial.println("Strip layout: fixed raw markers, 256 words");
}

#include "phrases.h"
#include "offline.h"

}  // namespace

void setup() {
  Serial.begin(115200);
  buildCourse();
  spi_bus_config_t bus{};
  bus.mosi_io_num = dataPin;
  bus.miso_io_num = -1;
  bus.sclk_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = sizeof(frame);
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t device{};
  device.clock_speed_hz = spiClockHz;
  device.spics_io_num = -1;
  device.flags = SPI_DEVICE_HALFDUPLEX;
  device.queue_size = 1;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &panelBus));
  compensateSkippedWord = false;
  sendFrame();
  startedAt = millis();
  Serial.println("8x32 desktop display ready: GPIO4");
  startOfflineController();
}

void loop() {
  serviceOfflineController();
  const int command = readPanelCommand();
  constexpr char stripColors[] = "rgbwk";
  const char* stripColor = command > 0 ? strchr(stripColors, command) : nullptr;
  if (command == '?') {
    printPanelState();
  } else if (command == '~') {
    releaseDesktopControl();
  } else if (controlOwner == "phone" && command >= 0) {
    // Phone control holds the display until an explicit desktop M packet arrives.
  } else if (command == '!') {
    memset(pixels, 0, sizeof(pixels));
    memset(musicPeaks, 0, sizeof(musicPeaks));
    sendLiveFrame(millis());
    musicLit = false;
  } else if (command == 'G') {
    controlOwner = "desktop";
    chooseMode("dino");
  } else if (command == 'X') {
    controlOwner = "desktop";
    chooseMode("pet");
  } else if ((command == 'Y' || command == 'Z') && displayMode == DisplayMode::Pet) {
    setPetAction(command == 'Y' ? PetAction::Petted : PetAction::Playing, millis());
  } else if (command == 'J' && displayMode == DisplayMode::Dino) {
    handleDinoJump(millis());
  } else if (command == 'P') {
    showStripLayout();
  } else if (command == 'N') {
    controlOwner = "desktop";
    chooseMode("newyear");
  } else if (command == 'T' || stripColor != nullptr) {
    displayMode = DisplayMode::StripTest;
    compensateSkippedWord = false;
    serpentineRows = false;
    heldStripColor = stripColor == nullptr ? -1 : stripColor - stripColors;
    previousStripStep = SIZE_MAX;
    startedAt = millis();
  } else if (command == 'H' || (command >= '1' && command <= '5')) {
    displayMode = DisplayMode::Health;
    heldHealthPattern = command == 'H' ? -1 : command - '1';
    compensateSkippedWord = true;
    serpentineRows = false;
    previousHealthStep = SIZE_MAX;
    startedAt = millis();
  } else if (command >= '6' && command <= '9') {
    constexpr Color isolatedColors[] = {{36, 0, 0}, {0, 36, 0}, {0, 0, 36}, {18, 18, 18}};
    const size_t step = command - '6';
    memset(pixels, 0, sizeof(pixels));
    sendFrame(suspectPixelWord, isolatedColors[step]);
    // Hold the isolated result until the next serial command.
    displayMode = DisplayMode::Columns;
    columnsSent = true;
    Serial.printf("Isolated pixel: row 9 column 11, %s\r\n", healthPatterns[step].name);
  } else if (command == 'A' || command == 'D') {
    memset(pixels, 0, sizeof(pixels));
    sendFrame(command == 'A' ? boundaryMarkerFrame : panelPaddingWord);
    displayMode = DisplayMode::Columns;
    columnsSent = true;
    Serial.println(command == 'A' ? "Raw markers: 193 red, 194 white, 195 green, 196 blue, 256 violet"
                                  : "Raw isolated: 194 white");
  } else if (command == 'B') {
    displayMode = DisplayMode::Boundary;
    previousBoundaryStep = SIZE_MAX;
    startedAt = millis();
  } else if (command == 'S' || command == 'L') {
    displayMode = DisplayMode::Drift;
    serpentineRows = command == 'S';
    startedAt = millis();
    Serial.printf("Matrix layout: %s\r\n", serpentineRows ? "serpentine" : "linear");
  } else if (command == 'R') {
    displayMode = DisplayMode::RowScan;
    serpentineRows = false;
    previousScanStep = UINT16_MAX;
    startedAt = millis();
  } else if (command == 'C' || command == 'U') {
    displayMode = DisplayMode::Columns;
    compensateSkippedWord = command == 'C';
    columnsSent = false;
    startedAt = millis();
  }
  if (displayMode == DisplayMode::Phrases) {
    showPhrases(millis());
    delay(2);
    return;
  }
  if (displayMode == DisplayMode::Dino) {
    showDino(millis());
    delay(2);
    return;
  }
  if (displayMode == DisplayMode::Pet) {
    showPet(millis());
    delay(32);
    return;
  }
  if (displayMode == DisplayMode::Lamp) {
    delay(2);
    return;
  }
  if (displayMode == DisplayMode::Calendar) {
    delay(2);
    return;
  }
  if (displayMode == DisplayMode::Music) {
    if (millis() - lastMusicFrameAt > liveFrameTimeoutMs) {
      if (controlOwner != "phone") {
        controlOwner = "device";
      }
      musicLit = false;
      enterCalendar();
    }
    delay(2);
    return;
  }
  if (displayMode == DisplayMode::NewYear) {
    showNewYear(millis() - startedAt);
    delay(22);
    return;
  }
  if (displayMode == DisplayMode::StripTest) {
    showStripTest(millis() - startedAt);
    delay(5);
    return;
  }
  if (displayMode == DisplayMode::Health || displayMode == DisplayMode::Boundary) {
    const uint32_t elapsed = millis() - startedAt;
    if (displayMode == DisplayMode::Health) {
      showHealth(elapsed);
    } else {
      showBoundary(elapsed);
    }
    delay(5);
    return;
  }
  if (displayMode == DisplayMode::Columns) {
    showColumns();
    delay(5);
    return;
  }
  if (displayMode == DisplayMode::RowScan) {
    scanRows(millis() - startedAt);
    delay(5);
    return;
  }
  const uint16_t age = (millis() - startedAt) % cycleMs;
  memcpy(pixels, course, sizeof(pixels));
  drawDriftTrail(age);
  if (age < exitEnd) {
    drawCar(carPose(age));
  }
  sendFrame();

  const char* stage = age < driftStart ? "entry" : age < driftEnd ? "drift" : age < exitEnd ? "exit" : "clear";
  static const char* previousStage = nullptr;
  if (previousStage != stage) {
    Serial.printf("Pixel drift 16x16: GPIO4, %s, %s, %s, %lu frames sent\r\n",
                  stage, serpentineRows ? "serpentine" : "linear",
                  compensateSkippedWord ? "padded" : "unpadded",
                  static_cast<unsigned long>(frameCount));
    previousStage = stage;
  }
  delay(18);
}
