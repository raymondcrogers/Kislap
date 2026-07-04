#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <FastLED.h>
#include "config.h"
#include "runtime_config.h"
#include "web_api.h"

CRGB leds[KISLAP_NUM_LEDS];

// A gust of wind that emanates from the origin seam (zone 1's blue/stripe
// border, where its intro ends and its red/white stripes begin) and travels
// outward in both directions at once, the length of the whole strip -- a
// single continuous wave that isn't clipped at zone boundaries (only the
// zone-local color lookup in loop() is). `distance` is how far the wave's
// crests have traveled from the origin (in pixel units, advancing each
// frame): one crest rides toward the far end at origin+distance, its mirror
// image rides toward the near end at origin-distance. Because zone 1 is wired
// back on itself at the first corner, that near-going crest already traces
// the same physical direction along the house as the far-going one -- no
// extra reversal needed. `amplitude` is how far it nudges a pixel at its
// peak; the displacement falls off with distance from each crest, so a pixel
// is pushed out of place as a crest arrives and settles back as it leaves.
// `active` marks an in-use slot in the pool below.
struct Ripple {
    float distance;
    float amplitude;
    float width;      // spatial spread, scaled to amplitude (stronger = wider)
    bool active;
};

Ripple ripples[KISLAP_MAX_RIPPLES] = {};
uint32_t frameCount = 0;      // frames rendered; drives all frame-paced animation
uint32_t nextRippleFrame = 0; // frameCount at which to launch the next ripple

// Rolling measured frame rate, recomputed about once a second from wall-clock
// time. Read by the web API's /api/status endpoint (declared extern there).
float measuredFps = 0.0f;

// Base pattern color at a (possibly displaced, possibly negative) position
// measured from the zone's own seam (where that zone's intro ends): alternating
// red and white stripes, each KISLAP_STRIPE_WIDTH LEDs wide, so the first
// stripe starts at the seam at full width and begins red.
static CRGB stripeColor(float posFromSeam) {
    const long stripe = (long)floorf(posFromSeam / cfg.stripeWidth);
    // Even stripes red, odd stripes white. '& 1' on a negative long still gives
    // a stable parity here since floorf rounds toward negative infinity.
    return (stripe & 1L) ? CRGB::White : CRGB::Red;
}

// Scramble an integer into a well-mixed 8-bit value. Used to give each star an
// uncorrelated twinkle, so neighbors don't dim in lockstep.
static uint8_t hash8(uint32_t x) {
    x = (x ^ 61u) ^ (x >> 16);
    x += x << 3;
    x ^= x >> 4;
    x *= 0x27d4eb2du;
    x ^= x >> 15;
    return (uint8_t)x;
}

// Intro-section color at a (possibly displaced, possibly negative) position: a
// dark-blue field with a twinkling white star every KISLAP_INTRO_STAR_EVERY
// pixels. The twinkle is keyed to the star's own position, so a star keeps its
// identity (and shimmer) as the ripple slides it along the strand. Each star's
// phase and rate come from a hash of its index rather than a linear ramp, so
// they twinkle independently instead of in a wave marching down the line.
// `tick` is the frame counter (not wall-clock time), so the shimmer advances
// per rendered frame -- adding frame delay slows it down smoothly rather than
// stepping it forward in jumps, keeping it in the same clock as the ripples.
static CRGB introColor(float pos, uint32_t tick) {
    const long p = (long)floorf(pos);
    const long starEvery = cfg.introStarEvery;
    // Positive modulo so stars land consistently even at negative positions.
    if (((p % starEvery) + starEvery) % starEvery == 0) {
        const uint8_t h = hash8((uint32_t)(p / starEvery));
        // Per-star rate (frames per phase step) and a random starting phase. The
        // rate spread keeps stars from sharing a common beat frequency.
        const uint8_t rate = 6 + (h & 0x07);     // 6..13
        const uint8_t phase = (uint8_t)(tick / rate) + h;
        // Twinkle blends the star from the background blue (dim end) up to white
        // (bright end), so at the bottom of its pulse it melts into the field
        // rather than winking to black. The configured floor raises the dim end
        // off the background: 0 = full background blue, 255 = never leaves white.
        const uint8_t floor = (uint8_t)cfg.introStarMin;
        const uint8_t mix = floor + scale8(sin8(phase), 255 - floor);
        return blend(CRGB(CRGB::DarkBlue), CRGB(CRGB::White), mix);
    }
    return CRGB::DarkBlue;
}

// Which zone LED index `i` physically belongs to. Zones are contiguous and in
// strip order, so a linear scan over the (small, fixed) zone count is plenty.
static int zoneIndexForLed(int i) {
    for (int z = 0; z < KISLAP_NUM_ZONES - 1; z++) {
        if (i < zones[z].end) {
            return z;
        }
    }
    return KISLAP_NUM_ZONES - 1;
}

// Launch a ripple at the origin seam with a random strength, if a slot is
// free. Its two mirror-image crests start together at the seam (distance 0) and
// spread outward from there. Strength varies so gusts range from gentle nudges
// to strong shoves.
static void spawnRipple() {
    // Only consider the first cfg.maxRipples slots of the pool, so lowering the
    // cap at runtime limits how many ripples can be in flight at once. Slots
    // beyond the cap that are still finishing their travel are advanced and
    // retired by the loop below; they just won't be reused until the cap rises.
    for (int r = 0; r < cfg.maxRipples; r++) {
        if (!ripples[r].active) {
            // random() is integer-only; scale to get a fractional amplitude.
            const float t = random(0, 1001) / 1000.0f;  // 0.0 .. 1.0
            const float amp =
                cfg.rippleMinAmp + t * (cfg.rippleMaxAmp - cfg.rippleMinAmp);
            ripples[r].amplitude = amp;
            // Width scales with amplitude so a stronger gust is also a wider
            // swell. cfg.rippleWidth is the width at the typical (mean) gust, so
            // gentle gusts are narrower and strong ones broader; the config knob
            // stays meaningful as a reference length.
            const float refAmp = 0.5f * (cfg.rippleMinAmp + cfg.rippleMaxAmp);
            const float scale = (refAmp > 0.01f) ? (amp / refAmp) : 1.0f;
            ripples[r].width = cfg.rippleWidth * scale;
            // Both crests start at the origin seam and travel outward from it.
            ripples[r].distance = 0.0f;
            ripples[r].active = true;
            return;
        }
    }
}

// Set while an OTA update is in flight. FastLED.show() disables interrupts to
// bit-bang LED timing, which drops WiFi packets and breaks OTA; pause rendering
// while updating.
volatile bool otaInProgress = false;

static void connectWiFi() {
    Serial.printf("Connecting to WiFi '%s'", KISLAP_WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(KISLAP_HOSTNAME);
    WiFi.begin(KISLAP_WIFI_SSID, KISLAP_WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("Connected. IP address: ");
    Serial.println(WiFi.localIP());
}

static void setupOTA() {
    ArduinoOTA.setHostname(KISLAP_HOSTNAME);
    if (strlen(KISLAP_OTA_PASSWORD) > 0) {
        ArduinoOTA.setPassword(KISLAP_OTA_PASSWORD);
    }

    ArduinoOTA.onStart([]() {
        otaInProgress = true;
        FastLED.clear(true);  // blank the strip so show() isn't fighting the upload
        Serial.println("OTA update starting...");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA update complete.");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA progress: %u%%\r", (progress * 100) / total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        otaInProgress = false;  // resume rendering if the update aborts
        Serial.printf("OTA error [%u]\n", error);
    });

    ArduinoOTA.begin();
    Serial.println("OTA ready.");
}

void setup() {
    Serial.begin(KISLAP_SERIAL_BAUD);
    // Bounded wait: give a USB host a moment to attach, but never block boot on
    // it. On a wall-powered board with no serial host, `while (!Serial)` would
    // hang here forever -- and because the strip isn't initialized yet, it would
    // sit in its power-on all-white state the whole time.
    for (uint32_t t0 = millis(); !Serial && millis() - t0 < 2000;) {
        delay(10);
    }

    initZones();

    // Bring up and blank the strip FIRST, before the blocking WiFi connect
    // below. WS281x LEDs power on to full white; if we init them only after
    // connectWiFi() (which blocks until the network is up), any WiFi trouble
    // leaves the whole strand stuck pure white. Clearing here means a boot that
    // stalls on WiFi shows a dark strip, not a white one.
    //
    // Stringify the compile-time chipset/order/pin so the serial log shows
    // exactly what this build is driving (# turns a macro's value into a string
    // literal; the extra level forces the macro to expand first).
#define KISLAP_STR2(x) #x
#define KISLAP_STR(x) KISLAP_STR2(x)
#ifdef KISLAP_COLOR_TEST
    // Identity order so what we send is exactly what the strip receives.
    Serial.printf("FastLED init: type=%s order=%s (COLOR_TEST) pin=%d leds=%d\n",
                  KISLAP_STR(KISLAP_LED_TYPE), "RGB",
                  KISLAP_DATA_PIN, KISLAP_NUM_LEDS);
    FastLED.addLeds<KISLAP_LED_TYPE, KISLAP_DATA_PIN, RGB>(leds, KISLAP_NUM_LEDS).setCorrection(TypicalLEDStrip);
#else
    Serial.printf("FastLED init: type=%s order=%s pin=%d leds=%d\n",
                  KISLAP_STR(KISLAP_LED_TYPE), KISLAP_STR(KISLAP_COLOR_ORDER),
                  KISLAP_DATA_PIN, KISLAP_NUM_LEDS);
    FastLED.addLeds<KISLAP_LED_TYPE, KISLAP_DATA_PIN, KISLAP_COLOR_ORDER>(leds, KISLAP_NUM_LEDS).setCorrection(TypicalLEDStrip);
#endif
#undef KISLAP_STR
#undef KISLAP_STR2
    FastLED.clear(true);  // blank the strip immediately (writes out all-off once)

    connectWiFi();
    setupOTA();
    setupWebApi();

    // Pull persisted tunables from NVS (falling back to config.h defaults) and
    // apply them, including FastLED brightness. Must run after addLeds().
    loadConfig();

    // Seed the RNG so ripple timing and strength differ run to run.
    randomSeed(esp_random());
    nextRippleFrame = random(cfg.rippleMinIntervalFrames,
                             cfg.rippleMaxIntervalFrames + 1);

    Serial.println("Kislap firmware starting with FastLED...");
}

void loop() {
    ArduinoOTA.handle();
    handleWebApi();

    // While updating, do nothing but service OTA so the upload isn't disrupted.
    if (otaInProgress) {
        return;
    }

    // Launch a new ripple once the random frame gap has elapsed, then schedule
    // the next one. Frame-based so the spawn cadence slows with frame delay in
    // lockstep with ripple travel and the twinkle. Strength is randomized in
    // spawnRipple().
    if (frameCount >= nextRippleFrame) {
        spawnRipple();
        nextRippleFrame = frameCount + random(cfg.rippleMinIntervalFrames,
                                              cfg.rippleMaxIntervalFrames + 1);
    }

    // Advance every active ripple's crests outward from the origin and retire
    // those whose crests have both traveled off the ends (plus a margin so the
    // swell fully exits). The near-side crest reaches index 0 first, but the
    // far-side crest still has the rest of the strand to clear, so both must be
    // gone before the slot is freed.
    const float origin = (float)(zones[0].start + zones[0].introPixels);
    for (int r = 0; r < KISLAP_MAX_RIPPLES; r++) {
        if (!ripples[r].active) {
            continue;
        }
        ripples[r].distance += cfg.rippleSpeed;
        // The trailing side lags behind each crest and is the wider one, so the
        // retire margin has to cover its reach or the tail would pop off abruptly.
        const float trailW =
            ripples[r].width * (cfg.rippleReturnRatio > 1.0f ? cfg.rippleReturnRatio : 1.0f);
        const bool farGone =
            (origin + ripples[r].distance) > KISLAP_NUM_LEDS + 4.0f * trailW;
        const bool nearGone = (origin - ripples[r].distance) < -4.0f * trailW;
        if (farGone && nearGone) {
            ripples[r].active = false;
        }
    }

    // Each zone is its own fixed base image: a star field over its intro
    // pixels, red/white stripes after. A ripple shifts which part of that
    // image a pixel shows by displacing its sample position. Each ripple
    // contributes two bell-curve crests: one traveling toward the far end
    // (center origin+distance) and its mirror image traveling toward the near
    // end (center origin-distance) -- the same continuous wave for the whole
    // strip, regardless of zone edges. A pixel is nudged most as a crest
    // passes over it and returns to rest as the crest moves on. Contributions
    // from overlapping crests add up. Each ripple carries its own width
    // (scaled to its amplitude), so the bell curve is computed per-ripple
    // rather than from a single sigma.
    for (int i = 0; i < KISLAP_NUM_LEDS; i++) {
        float displacement = 0.0f;
        // Signed shading: positive on the leading edge of a crest (brighter) and
        // negative behind it (the trailing edge, darker). d*g is the shape of the
        // bump's slope, so the light rides the front of each wave. The amplitude
        // factor keeps a stronger gust's light/shadow more pronounced even though
        // its wider width would otherwise flatten the slope.
        float shade = 0.0f;
        for (int r = 0; r < KISLAP_MAX_RIPPLES; r++) {
            if (!ripples[r].active) {
                continue;
            }
            const float amp = ripples[r].amplitude;
            const float baseW = ripples[r].width;
            // Two crests per ripple. `sign` orients each: +1 for the far-going
            // crest (travels toward higher indices, so its leading/push side is
            // ahead of it at d>0) and -1 for the near-going mirror image
            // (travels toward lower indices, leading side at d<0). Folding the
            // coordinate by `sign` reuses the same asymmetric shape and shading
            // for both, so the near-going wave is a true mirror of the far one.
            for (int s = 0; s < 2; s++) {
                const float sign = (s == 0) ? 1.0f : -1.0f;
                const float center = origin + sign * ripples[r].distance;
                // Fold into the crest's own frame: dd>0 is always its leading
                // (push) side, dd<0 its trailing (return) side, regardless of
                // travel direction.
                const float dd = sign * (i - center);
                // Give the return side a wider spread so pixels ease back to rest
                // more gradually than they were shoved out.
                const float w = (dd < 0.0f) ? baseW * cfg.rippleReturnRatio : baseW;
                // Skip pixels too far from this crest to feel it (keeps the inner
                // loop cheap; beyond ~4 sigma the bump is effectively zero).
                if (fabsf(dd) > 4.0f * w) {
                    continue;
                }
                const float g = expf(-(dd * dd) / (2.0f * w * w));
                // Displace in the crest's travel direction so the near-going wave
                // drags the pattern the opposite way from the far-going one.
                displacement += sign * amp * g;
                shade += amp * (dd / w) * g;
            }
        }
        const float sample = i - displacement;

        // Keep the sample from reading into a neighboring zone, but only clamp
        // the upper (stripe) end. A forward displacement past zone.end would
        // reveal the next zone, so pin it to the last index. The lower (intro)
        // end is left unclamped on purpose: introColor() is periodic and defined
        // for negative positions, so letting localPos go negative scrolls more of
        // THIS zone's own blue/star field into frame (still never a neighbor's
        // pattern). Clamping it to zone.start instead pinned every displaced
        // pixel to the star at localPos 0, which twinkles white -- that was the
        // "white pulled into frame at the start of the intro" bug.
        const Zone& zone = zones[zoneIndexForLed(i)];
        float clampedSample = sample;
        if (clampedSample > (float)(zone.end - 1)) clampedSample = (float)(zone.end - 1);
        const float localPos = clampedSample - zone.start;
        leds[i] = (localPos < zone.introPixels)
                      ? introColor(localPos, frameCount)
                      : stripeColor(localPos - zone.introPixels);

        // Apply the crest's light/shadow as a single symmetric brightness
        // scale, so brightening and darkening behave the same across every base
        // color. The pattern is rendered at cfg.rippleBaseLevel (below full) to
        // leave headroom above it; a positive shade scales the pixel's value up
        // toward full, a negative shade down toward black. Because it multiplies
        // the existing RGB, hue is preserved (no white-adding), so red stays red
        // instead of washing out to pink, and blue/white respond in kind.
        float shadeF = shade * cfg.rippleShade;
        if (shadeF > 1.0f) shadeF = 1.0f;
        if (shadeF < -1.0f) shadeF = -1.0f;
        float mul = cfg.rippleBaseLevel * (1.0f + shadeF);
        if (mul > 1.0f) mul = 1.0f;
        if (mul < 0.0f) mul = 0.0f;
        leds[i].nscale8((uint8_t)(mul * 255.0f));
    }

    FastLED.show();
    frameCount++;

    // Recompute the rolling FPS roughly once a second from real elapsed time, so
    // it reflects the true render rate (transmission + compute + frame delay).
    static uint32_t fpsWindowStartMs = 0;
    static uint32_t fpsWindowStartFrame = 0;
    const uint32_t nowMs = millis();
    const uint32_t elapsedMs = nowMs - fpsWindowStartMs;
    if (elapsedMs >= 1000) {
        measuredFps = (frameCount - fpsWindowStartFrame) * 1000.0f / elapsedMs;
        fpsWindowStartMs = nowMs;
        fpsWindowStartFrame = frameCount;
    }

    delay(cfg.frameDelayMs);
}
