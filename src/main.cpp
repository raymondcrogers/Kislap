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

// WiFi is brought up in the background so the lights never wait on it. The
// first connection attempt after power-on occasionally fails (router slow to
// answer, weak signal, supply dip as the strand powers up), and the ESP32 often
// doesn't recover from that on its own. The old blocking connect loop never
// retried, so such a boot sat dark forever. Now loop() calls maintainWiFi(),
// which re-issues WiFi.begin() every KISLAP_WIFI_RETRY_MS until connected, and
// does the same if the connection drops later.
static const uint32_t KISLAP_WIFI_RETRY_MS = 15000;
static uint32_t wifiLastAttemptMs = 0;
static bool wifiWasConnected = false;
// OTA and the REST API are started once, on the first successful connect.
static bool networkServicesStarted = false;

static void startWiFi() {
    Serial.printf("Connecting to WiFi '%s' (in the background)\n", KISLAP_WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(KISLAP_HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.begin(KISLAP_WIFI_SSID, KISLAP_WIFI_PASSWORD);
    wifiLastAttemptMs = millis();
}

static void setupOTA();

// Non-blocking: cheap to call every frame.
static void maintainWiFi() {
    const bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected) {
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            Serial.print("WiFi connected. IP address: ");
            Serial.println(WiFi.localIP());
            if (!networkServicesStarted) {
                setupOTA();
                setupWebApi();
                networkServicesStarted = true;
            }
        }
        return;
    }

    if (wifiWasConnected) {
        wifiWasConnected = false;
        wifiLastAttemptMs = millis();  // give auto-reconnect a full interval first
        Serial.println("WiFi connection lost; reconnecting in the background.");
        return;
    }

    if (millis() - wifiLastAttemptMs >= KISLAP_WIFI_RETRY_MS) {
        Serial.printf("WiFi still not connected (status %d); retrying.\n", (int)WiFi.status());
        WiFi.disconnect();
        WiFi.begin(KISLAP_WIFI_SSID, KISLAP_WIFI_PASSWORD);
        wifiLastAttemptMs = millis();
    }
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

    // Pull persisted tunables from NVS (falling back to config.h defaults) and
    // apply them, including FastLED brightness. Must run after addLeds().
    loadConfig();

    // Start connecting, but don't wait: the scheme starts rendering right away,
    // and loop() finishes the connection (and starts OTA + the REST API) whenever
    // the network comes up. See maintainWiFi().
    startWiFi();

    // Seed the RNG so ripple timing and strength differ run to run.
    randomSeed(esp_random());
    nextRippleFrame = random(cfg.rippleMinIntervalFrames,
                             cfg.rippleMaxIntervalFrames + 1);

    Serial.println("Kislap firmware starting with FastLED...");
}

// Patriotic scheme: per-zone blue/star intro and red/white stripes, pushed
// around by traveling wind ripples. Renders one frame into leds[].
static void renderPatriotic() {
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
}

// Halloween scheme: alternating green and purple sections running the whole
// strip, each pair separated by a single black pixel. The first and last
// cfg.halloweenFadeWidth pixels of a section ramp evenly between near-black and
// full color, so each section fades in from and out to its black separators.
// Static, so a frame is just a function of the pixel index.
static void renderHalloween() {
    const int32_t section = cfg.halloweenSectionWidth;
    const int32_t fade = cfg.halloweenFadeWidth;
    const int32_t period = section + 1;  // a section plus its leading black pixel
    for (int i = 0; i < KISLAP_NUM_LEDS; i++) {
        const int32_t p = i % period;
        if (p == 0) {
            leds[i] = CRGB::Black;
            continue;
        }
        const int32_t k = p - 1;  // position within the section, 0..section-1
        CRGB c = ((i / period) & 1) ? CRGB(KISLAP_HALLOWEEN_COLOR_B)
                                    : CRGB(KISLAP_HALLOWEEN_COLOR_A);
        // Pixels within `fade` of either end get (edge+1)/(fade+1) of full color,
        // stepping up evenly from the black separator. _video scaling keeps lit
        // channels from rounding down to zero, which matters for colors this dim.
        const int32_t edge = min(k, section - 1 - k);
        if (edge < fade) {
            c.nscale8_video((uint8_t)((edge + 1) * 255 / (fade + 1)));
        }
        leds[i] = c;
    }
}

// --- Halloween lightning ----------------------------------------------------
// One strike in flight at a time. A strike runs on a frame timeline measured
// from `startFrame`: `leaderFrames` of leader (the channel racing from `from`
// to `to`), then a burst of strobe flashes, then a short afterglow. Flash
// timings are offsets in frames from the end of the leader. The frame-paced
// timeline guarantees each flash and each dark gap between flashes lands on at
// least one rendered frame, so the strobe never blurs into a single blink.
#define LIGHTNING_MAX_FLASHES 6
#define LIGHTNING_AFTERGLOW_FRAMES 3
#define LIGHTNING_HALO 4  // pixels of soft glow past each end of the bolt

struct LightningStrike {
    bool active;
    int32_t from;          // pixel the strike starts at
    int32_t to;            // pixel it strikes to (may be below `from`)
    uint32_t startFrame;   // frameCount when the leader began
    uint8_t leaderFrames;
    uint8_t flashCount;
    uint8_t flashStart[LIGHTNING_MAX_FLASHES];  // frames after the leader ends
    uint8_t flashLen[LIGHTNING_MAX_FLASHES];
    uint8_t flashLevel[LIGHTNING_MAX_FLASHES];  // peak brightness, 0-255
    uint8_t seed;          // varies the bolt's brightness texture per strike
};

LightningStrike strike = {};
uint32_t nextLightningMs = 0;  // millis() at which the next strike may begin

static void scheduleNextLightning(uint32_t nowMs) {
    nextLightningMs = nowMs + random(cfg.lightningMinIntervalMs,
                                     cfg.lightningMaxIntervalMs + 1);
}

// Pick a random start pixel and a random end pixel a configured distance away,
// in whichever direction fits on the strip, and lay out the strobe pattern.
static void spawnLightning() {
    const int32_t span = random(cfg.lightningMinLength, cfg.lightningMaxLength + 1);
    strike.from = random(0, KISLAP_NUM_LEDS);
    int32_t dir = random(2) ? 1 : -1;
    // If the bolt would run off this end of the strip, send it the other way;
    // if it doesn't fit either way, clamp it to the end it reaches.
    if (strike.from + dir * span < 0 || strike.from + dir * span >= KISLAP_NUM_LEDS) {
        dir = -dir;
    }
    strike.to = constrain(strike.from + dir * span, (int32_t)0, (int32_t)KISLAP_NUM_LEDS - 1);

    strike.startFrame = frameCount;
    strike.leaderFrames = random(2, 4);
    strike.seed = random(0, 256);

    // First flash is the return stroke at full brightness; the re-strikes that
    // follow are weaker. Every flash and every dark gap is a single frame (the
    // shortest the strip can show; at ~21 fps that's ~47 ms), with an
    // occasional two-frame gap so the strobe rhythm isn't perfectly even.
    strike.flashCount = random(3, LIGHTNING_MAX_FLASHES + 1);
    uint8_t pos = 0;
    for (int f = 0; f < strike.flashCount; f++) {
        strike.flashStart[f] = pos;
        strike.flashLen[f] = 1;
        strike.flashLevel[f] = (f == 0) ? 255 : random(120, 256);
        pos += strike.flashLen[f] + (random(4) == 0 ? 2 : 1);
    }
    strike.active = true;
}

// Blend a bolt pixel toward the lightning color. `amount` is the pixel's
// brightness (0-255); the per-strike hash gives the channel an uneven,
// crackling texture rather than a flat bar of white.
static void lightBoltPixel(int32_t i, uint8_t amount) {
    if (i < 0 || i >= KISLAP_NUM_LEDS) return;
    const uint8_t texture = 150 + scale8(hash8((uint32_t)i * 7919u + strike.seed), 105);
    leds[i] = blend(leds[i], CRGB(KISLAP_LIGHTNING_COLOR), scale8(amount, texture));
}

// Draw the current strike (if any) over the already-rendered Halloween frame,
// and launch a new one when the random wall-clock gap has elapsed.
static void renderLightning() {
    const uint32_t nowMs = millis();
    // Hold off the first strike for one full interval after the scheme first
    // renders, rather than striking the instant the device boots.
    static bool firstScheduled = false;
    if (!firstScheduled) {
        scheduleNextLightning(nowMs);
        firstScheduled = true;
    }
    if (!strike.active) {
        // Wrap-safe "now >= next" check on the millis() clock.
        if (!cfg.lightningEnabled || (int32_t)(nowMs - nextLightningMs) < 0) {
            return;
        }
        spawnLightning();
        scheduleNextLightning(nowMs);
    }

    const int32_t dir = (strike.to >= strike.from) ? 1 : -1;
    const int32_t span = abs(strike.to - strike.from);
    const uint32_t t = frameCount - strike.startFrame;

    // Leader: a dim, flickering channel grows from `from` toward `to`, one step
    // per frame, with a bright tip at its leading edge.
    if (t < strike.leaderFrames) {
        const int32_t reach = span * (int32_t)(t + 1) / strike.leaderFrames;
        for (int32_t k = 0; k <= reach; k++) {
            const uint8_t amount = (reach - k < 3) ? 255 : random8(40, 140);
            lightBoltPixel(strike.from + dir * k, amount);
        }
        return;
    }

    // Strobe: find which flash (if any) covers this frame. Between flashes the
    // bolt goes fully dark so each flash reads as a distinct strobe.
    const uint32_t u = t - strike.leaderFrames;
    uint8_t level = 0;
    bool flashing = false;
    for (int f = 0; f < strike.flashCount; f++) {
        if (u >= strike.flashStart[f] && u < (uint32_t)(strike.flashStart[f] + strike.flashLen[f])) {
            level = strike.flashLevel[f];
            flashing = true;
            break;
        }
    }

    // Afterglow: once the last flash ends, the channel fades out over a few
    // frames, then the strike is done.
    const int last = strike.flashCount - 1;
    const uint32_t flashesEnd = strike.flashStart[last] + strike.flashLen[last];
    if (u >= flashesEnd) {
        const uint32_t k = u - flashesEnd;
        if (k >= LIGHTNING_AFTERGLOW_FRAMES) {
            strike.active = false;
            return;
        }
        level = 110 * (LIGHTNING_AFTERGLOW_FRAMES - k) / LIGHTNING_AFTERGLOW_FRAMES;
    }
    if (level == 0) {
        return;
    }

    // During a flash the sky around the bolt briefly lights up. The glow covers
    // the bolt and extends half the bolt's length past each end, tapering
    // linearly to nothing at the edge of that reach, so it stays local.
    if (flashing && cfg.lightningAmbient > 0) {
        const uint8_t ambient = scale8(level, (uint8_t)cfg.lightningAmbient);
        const int32_t lo = min(strike.from, strike.to);
        const int32_t hi = max(strike.from, strike.to);
        const int32_t reach = span / 2;
        const int32_t first = max(lo - reach, (int32_t)0);
        const int32_t lastLed = min(hi + reach, (int32_t)KISLAP_NUM_LEDS - 1);
        for (int32_t i = first; i <= lastLed; i++) {
            const int32_t outside = (i < lo) ? lo - i : (i > hi ? i - hi : 0);
            const uint8_t amount = (outside == 0)
                ? ambient
                : (uint8_t)(ambient * (reach + 1 - outside) / (reach + 1));
            leds[i] = blend(leds[i], CRGB(KISLAP_LIGHTNING_COLOR), amount);
        }
    }

    // The bolt itself, with a slight per-frame flicker while flashing and a
    // soft halo tapering off past each end.
    for (int32_t k = -LIGHTNING_HALO; k <= span + LIGHTNING_HALO; k++) {
        uint8_t amount = flashing ? scale8(level, random8(190, 255)) : level;
        const int32_t outside = (k < 0) ? -k : (k > span ? k - span : 0);
        if (outside > 0) {
            amount = scale8(amount, 255 * (LIGHTNING_HALO + 1 - outside) / (LIGHTNING_HALO + 1));
        }
        lightBoltPixel(strike.from + dir * k, amount);
    }
}

// --- Autumn -----------------------------------------------------------------
// The strip is a long horizontal slit that leaves fall and blow across. Nothing
// moves along the strip: each leaf appears at a random spot, crosses the slit,
// and is gone. The slice of the leaf the slit shows grows from a sliver (its
// tip) to its full width (its middle) and back to nothing (its stem end). While
// crossing, the leaf tumbles -- its visible width pulses as it turns edge-on,
// its color swings between its front and its paler back, and it flickers --
// and gusts bring more leaves, faster, tumbling and flickering harder.

// Fall palette that each new leaf picks its color from. Entries are ordered so
// blends between neighbors stay autumnal (no muddy jumps), and the last entry
// leads back into the first so a random pick anywhere is a fall color.
static const CRGBPalette16 autumnPalette(
    CRGB(KISLAP_AUTUMN_RED),    CRGB(KISLAP_AUTUMN_RED),
    CRGB(KISLAP_AUTUMN_ORANGE), CRGB(KISLAP_AUTUMN_ORANGE),
    CRGB(KISLAP_AUTUMN_YELLOW), CRGB(KISLAP_AUTUMN_YELLOW),
    CRGB(KISLAP_AUTUMN_ORANGE), CRGB(KISLAP_AUTUMN_BROWN),
    CRGB(KISLAP_AUTUMN_BROWN),  CRGB(KISLAP_AUTUMN_RED),
    CRGB(KISLAP_AUTUMN_ORANGE), CRGB(KISLAP_AUTUMN_YELLOW),
    CRGB(KISLAP_AUTUMN_ORANGE), CRGB(KISLAP_AUTUMN_BROWN),
    CRGB(KISLAP_AUTUMN_BROWN),  CRGB(KISLAP_AUTUMN_RED));

// One leaf crossing the slit. It stays at `center` the whole time; only how
// much of it shows (and how it looks) changes as `progress` runs 0 -> 1.
struct Leaf {
    bool active;
    float center;      // pixel position where the leaf crosses the slit
    float halfWidth;   // half the leaf's width at its widest
    float progress;    // 0 = tip just entering, 1 = stem just leaving
    float step;        // progress per frame in calm air (1 / crossing frames)
    float tumble;      // current tumble angle (radians)
    float tumbleRate;  // radians per frame in calm air (sign = spin direction)
    CRGB front;        // leaf's front color
    CRGB back;         // its paler underside
    uint8_t seed;      // per-leaf flicker phase and rate
};

static Leaf leaves[KISLAP_AUTUMN_MAX_LEAVES] = {};
static float autumnGust = 0.0f;         // current gust strength, 0..1
static float autumnGustTarget = 0.0f;   // strength the gust is rising toward / decaying
static float autumnFlutterClock = 0.0f; // flicker time; runs faster in a gust
static uint32_t autumnNextGustFrame = 0;
static bool autumnStarted = false;

// Start a new leaf at a random spot, if a slot is free (if the pool is full the
// leaf is simply skipped).
static void spawnLeaf() {
    for (int k = 0; k < KISLAP_AUTUMN_MAX_LEAVES; k++) {
        Leaf& l = leaves[k];
        if (l.active) {
            continue;
        }
        l.center = random(0, KISLAP_NUM_LEDS * 100) / 100.0f;
        l.halfWidth = 0.5f * random(cfg.autumnLeafMinSize, cfg.autumnLeafMaxSize + 1);
        l.progress = 0.0f;
        l.step = 1.0f / random(cfg.autumnFallMinFrames, cfg.autumnFallMaxFrames + 1);
        l.tumble = random(0, 628) / 100.0f;
        // 0.10 .. 0.30 radians per frame, spinning either way.
        l.tumbleRate = (0.10f + random(0, 201) / 1000.0f) * (random(2) ? 1.0f : -1.0f);
        l.front = ColorFromPalette(autumnPalette, random8(), 255, LINEARBLEND);
        l.back = blend(l.front, CRGB(KISLAP_AUTUMN_UNDERSIDE), 110);
        l.seed = random8();
        l.active = true;
        return;
    }
}

static void renderAutumn() {
    if (!autumnStarted) {
        autumnNextGustFrame = frameCount + random(cfg.autumnGustMinIntervalFrames,
                                                  cfg.autumnGustMaxIntervalFrames + 1);
        autumnStarted = true;
    }

    // Gusts: at each launch pick a random strength. The gust eases up toward it
    // while the target itself fades away, so wind swells in over a second or so
    // and then dies back down over a few seconds.
    if ((int32_t)(frameCount - autumnNextGustFrame) >= 0) {
        autumnGustTarget = 0.4f + 0.6f * (random(0, 1001) / 1000.0f);
        autumnNextGustFrame = frameCount + random(cfg.autumnGustMinIntervalFrames,
                                                  cfg.autumnGustMaxIntervalFrames + 1);
    }
    autumnGust += (autumnGustTarget - autumnGust) * 0.06f;
    autumnGustTarget *= 0.985f;

    // A gust multiplies leaf rate, crossing speed and tumble speed.
    const float speedMult = 1.0f + cfg.autumnGustStrength * autumnGust;

    // New leaves this frame: the whole part of the expected count, plus one more
    // by chance for the fractional part, so e.g. 0.8/frame averages out right.
    const float expected = cfg.autumnLeafRate * speedMult;
    int newLeaves = (int)expected;
    if (random(0, 1000) < (long)((expected - newLeaves) * 1000.0f)) {
        newLeaves++;
    }
    for (int n = 0; n < newLeaves; n++) {
        spawnLeaf();
    }

    // Flicker runs up to 4x faster at the peak of a gust. Wrapping at 512 keeps
    // every leaf's phase continuous (see the rate math below).
    autumnFlutterClock += 1.0f + 3.0f * autumnGust;
    if (autumnFlutterClock >= 512.0f) autumnFlutterClock -= 512.0f;

    // Flicker depth: about a third of the configured depth in calm air, the
    // full depth at the peak of a gust.
    const uint8_t depth = (uint8_t)(cfg.autumnFlutter * (0.35f + 0.65f * autumnGust));

    // Backdrop showing through the gaps between leaves.
    CRGB bg = CRGB(KISLAP_AUTUMN_BACKGROUND_COLOR);
    bg.nscale8_video((uint8_t)cfg.autumnBackground);
    if (cfg.autumnBackground == 0) bg = CRGB::Black;
    fill_solid(leds, KISLAP_NUM_LEDS, bg);

    for (int k = 0; k < KISLAP_AUTUMN_MAX_LEAVES; k++) {
        Leaf& l = leaves[k];
        if (!l.active) {
            continue;
        }
        l.progress += l.step * speedMult;
        if (l.progress >= 1.0f) {
            l.active = false;
            continue;
        }
        l.tumble += l.tumbleRate * speedMult;
        if (l.tumble > TWO_PI) l.tumble -= TWO_PI;
        if (l.tumble < 0.0f) l.tumble += TWO_PI;

        // Leaf outline: the slice in the slit swells from tip to middle and
        // tapers to the stem. Tumbling narrows it as the leaf turns edge-on
        // (never fully to zero, so it doesn't blink out mid-flight).
        const float shape = sinf(PI * l.progress);
        const float ct = cosf(l.tumble);
        const float hw = l.halfWidth * shape * (0.3f + 0.7f * fabsf(ct));

        // Color swings from front (ct = +1) to underside (ct = -1) as it turns.
        CRGB c = blend(l.back, l.front, (uint8_t)((ct + 1.0f) * 127.5f));

        // Flicker: two sine waves at per-leaf rates (in half-steps, so 512 clock
        // ticks is a whole number of cycles), so each leaf flickers irregularly
        // and independently. _video keeps dim browns from losing a channel.
        const uint8_t phaseA =
            (uint8_t)(autumnFlutterClock * (6 + (l.seed & 7)) * 0.5f) + l.seed;
        const uint8_t phaseB =
            (uint8_t)(autumnFlutterClock * (9 + ((l.seed >> 3) & 7)) * 0.5f) + (l.seed << 2);
        const uint8_t wave = (uint8_t)(((uint16_t)sin8(phaseA) + sin8(phaseB)) >> 1);
        c.nscale8_video(255 - scale8(255 - wave, depth));

        // Draw with soft (anti-aliased) edges so a leaf grows and shrinks
        // smoothly rather than stepping a whole pixel at a time. Later leaves
        // cover earlier ones where they overlap.
        const int first = max((int)floorf(l.center - hw - 1.0f), 0);
        const int last = min((int)ceilf(l.center + hw + 1.0f), KISLAP_NUM_LEDS - 1);
        for (int i = first; i <= last; i++) {
            float cov = hw + 0.5f - fabsf((float)i - l.center);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            leds[i] = blend(leds[i], c, (uint8_t)(cov * 255.0f));
        }
    }
}

void loop() {
    maintainWiFi();
    if (networkServicesStarted) {
        ArduinoOTA.handle();
        handleWebApi();
    }

    // While updating, do nothing but service OTA so the upload isn't disrupted.
    if (otaInProgress) {
        return;
    }

    switch (cfg.scheme) {
        case SCHEME_HALLOWEEN:
            renderHalloween();
            renderLightning();
            break;
        case SCHEME_AUTUMN:
            renderAutumn();
            break;
        case SCHEME_PATRIOTIC:
        default:
            renderPatriotic();
            break;
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
