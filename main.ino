/**
 * Breathing LED + RoboEyes (SSD1306) with Personality
 * ---------------------------------------------------
 * - Drives a RED LED with a Gaussian breathing pulse.
 * - Renders animated "robo eyes" on an SSD1306 OLED.
 * - PersonalityController randomly changes mood & curiosity,
 *   keeping DEFAULT the majority mood over time.
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// Teensy core macro name collisions with some enums (defensive)
#ifdef DEFAULT
#undef DEFAULT
#endif
#ifdef SE
#undef SE
#endif

// ──────────────────────────────────────────────────────────────────────────────
// 1) CONFIG
// ──────────────────────────────────────────────────────────────────────────────

// Hardware
constexpr uint8_t RED_LED_PIN = 10;       // Must support PWM on your board
constexpr uint32_t I2C_FREQ_HZ = 400000;  // Fast mode for SSD1306
constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int OLED_RESET_PIN = -1;
constexpr uint8_t I2C_ADDR_PRI = 0x3C;
constexpr uint8_t I2C_ADDR_ALT = 0x3D;

// LED breathing timing
constexpr unsigned long BREATH_PERIOD_MS = 3000;  // Full inhale+exhale
constexpr unsigned long LED_UPDATE_MS = 10;       // LED update cadence

// Gaussian shape controls
constexpr float GAMMA = 0.14f;  // Width of brightness peak
constexpr float BETA = 0.50f;   // Center of peak (0.5 = symmetric)

// Brightness scaling
constexpr float MAX_PWM_F = 255.0f;
constexpr int MAX_PWM_I = 255;

// Personality weights & durations (tweak to taste)
// Weights are percentages that sum to ~100; DEFAULT should be largest.
constexpr uint8_t WEIGHT_DEFAULT = 80;
constexpr uint8_t WEIGHT_HAPPY = 7;
constexpr uint8_t WEIGHT_ANGRY = 7;
constexpr uint8_t WEIGHT_TIRED = 6;

// Mood durations (ms). DEFAULT lasts longer; others are brief “bursts”.
constexpr unsigned long DEFAULT_MIN_MS = 20000;  // 20–60s
constexpr unsigned long DEFAULT_MAX_MS = 60000;
constexpr unsigned long OTHER_MIN_MS = 5000;  // 5–15s
constexpr unsigned long OTHER_MAX_MS = 15000;

// Curiosity ON ratio (0–100). 50 = coin flip; increase for more “quirk”.
constexpr uint8_t CURIOSITY_ON_PERCENT = 50;

// ──────────────────────────────────────────────────────────────────────────────
// 2) FORWARD DECLS & LIBRARIES THAT NEED `display`
// ──────────────────────────────────────────────────────────────────────────────
extern Adafruit_SSD1306 display;  // RoboEyes expects this global
#include <FluxGarage_RoboEyes.h>  // depends on global `display`

// ──────────────────────────────────────────────────────────────────────────────
// 3) GLOBALS
// ──────────────────────────────────────────────────────────────────────────────
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);
roboEyes eyes;

static unsigned long gLastLedUpdateMs = 0;

// ──────────────────────────────────────────────────────────────────────────────
// 4) HELPERS
// ──────────────────────────────────────────────────────────────────────────────
inline float phaseFromTimeMs(unsigned long tMs, unsigned long periodMs) {
  const unsigned long m = (periodMs == 0) ? 0 : (tMs % periodMs);
  return static_cast<float>(m) / static_cast<float>(periodMs == 0 ? 1 : periodMs);
}

inline uint8_t gaussianBrightness(float x) {
  const float z = (x - BETA) / GAMMA;
  const float y = expf(-(z * z) * 0.5f);
  float pwm = y * MAX_PWM_F;
  if (pwm < 1.0f) pwm = 0.0f;  // deadband near zero
  if (pwm > MAX_PWM_F) pwm = MAX_PWM_F;
  return static_cast<uint8_t>(pwm + 0.5f);
}

bool initDisplayOrFallback() {
  if (display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDR_PRI)) return true;
  if (display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDR_ALT)) return true;
  return false;
}

void showInitErrorOnLed() {
  for (int i = 0; i < 3; i++) {
    analogWrite(RED_LED_PIN, MAX_PWM_I);
    delay(200);
    analogWrite(RED_LED_PIN, 0);
    delay(400);
  }
}

// ──────────────────────────────────────────────────────────────────────────────
// 5) PERSONALITY CONTROLLER
//    A tiny state machine that:
//    - Picks a weighted random mood (DEFAULT most of the time)
//    - Assigns a random duration for the chosen mood
//    - Randomly toggles Curiosity ON/OFF
// ──────────────────────────────────────────────────────────────────────────────
namespace Personality {

enum class Mood { Default,
                  Happy,
                  Angry,
                  Tired };

struct Controller {
  Mood currentMood = Mood::Default;
  bool curiosityOn = true;
  unsigned long nextChangeMs = 0;

  void begin() {
    // Try to seed RNG with something non-deterministic
#if defined(ARDUINO)
    randomSeed((uint32_t)micros());
#endif
    // Start with a sane baseline
    applyToEyes();
    scheduleNext();
  }

  void tick() {
    const unsigned long now = millis();
    if ((long)(now - nextChangeMs) >= 0) {
      // Time to change personality
      currentMood = pickWeightedMood();
      curiosityOn = rollCuriosity();
      applyToEyes();
      scheduleNext();
    }
  }

private:
  static Mood pickWeightedMood() {
    const uint8_t total =
      WEIGHT_DEFAULT + WEIGHT_HAPPY + WEIGHT_ANGRY + WEIGHT_TIRED;
    const uint8_t r = (uint8_t)random(0, total);  // [0, total)

    uint8_t acc = WEIGHT_DEFAULT;
    if (r < acc) return Mood::Default;
    acc += WEIGHT_HAPPY;
    if (r < acc) return Mood::Happy;
    acc += WEIGHT_ANGRY;
    if (r < acc) return Mood::Angry;
    return Mood::Tired;
  }

  static bool rollCuriosity() {
    // Return true with CURIOSITY_ON_PERCENT probability
    const uint8_t r = (uint8_t)random(0, 100);  // [0,100)
    return r < CURIOSITY_ON_PERCENT;
  }

  void applyToEyes() const {
    // Map our enum to library moods. The library exposes TIRED/ANGRY/HAPPY/DEFAULT.
    switch (currentMood) {
      case Mood::Default: eyes.setMood(DEFAULT); break;
      case Mood::Happy: eyes.setMood(HAPPY); break;
      case Mood::Angry: eyes.setMood(ANGRY); break;
      case Mood::Tired: eyes.setMood(TIRED); break;
    }
    eyes.setCuriosity(curiosityOn ? ON : OFF);
  }

  void scheduleNext() {
    const bool isDefault = (currentMood == Mood::Default);
    const unsigned long minMs = isDefault ? DEFAULT_MIN_MS : OTHER_MIN_MS;
    const unsigned long maxMs = isDefault ? DEFAULT_MAX_MS : OTHER_MAX_MS;
    const unsigned long span = (maxMs > minMs) ? (maxMs - minMs + 1UL) : 1UL;
    const unsigned long jitter = (unsigned long)random(0, span);
    nextChangeMs = millis() + minMs + jitter;
  }
};

}  // namespace Personality

Personality::Controller personality;

// ──────────────────────────────────────────────────────────────────────────────
// 6) SETUP
// ──────────────────────────────────────────────────────────────────────────────
void setup() {
  Wire.begin();
  Wire.setClock(I2C_FREQ_HZ);

  pinMode(RED_LED_PIN, OUTPUT);
  analogWrite(RED_LED_PIN, 0);

  if (!initDisplayOrFallback()) {
    showInitErrorOnLed();
    while (true) { /* hang */
    }
  }

  display.clearDisplay();
  display.display();

  eyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  // Baseline animations (personality will override mood/curiosity over time)
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);

  // Start personality engine (sets initial mood/curiosity & schedules changes)
  personality.begin();
}

// ──────────────────────────────────────────────────────────────────────────────
// 7) LOOP
// ──────────────────────────────────────────────────────────────────────────────
void loop() {
  // Keep eyes animation fresh every frame
  eyes.update();

  // Let the personality controller decide when to flip mood/curiosity
  personality.tick();

  // Update the breathing LED on its own cadence
  const unsigned long now = millis();
  if (now - gLastLedUpdateMs >= LED_UPDATE_MS) {
    gLastLedUpdateMs = now;

    const float phase = phaseFromTimeMs(now, BREATH_PERIOD_MS);
    const uint8_t brightness = gaussianBrightness(phase);
    analogWrite(RED_LED_PIN, brightness);
  }
}
