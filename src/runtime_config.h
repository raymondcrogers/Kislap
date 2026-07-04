#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

// A single zone's geometry: a contiguous run of LEDs, in absolute strip
// indices, whose first `introPixels` LEDs are the blue/star intro and whose
// remaining LEDs (up to `end`) are red/white stripes. See KISLAP_ZONE_LENGTHS
// and KISLAP_ZONE_INTRO_LENGTHS in config.h for the physical layout; this
// struct holds the derived absolute bounds computed by initZones().
struct Zone {
    int32_t start;       // first LED index (inclusive)
    int32_t end;         // one past the last LED index (exclusive)
    int32_t introPixels; // length of the blue/star intro at the start of the zone
};

// Fixed zone table, populated once by initZones(). Compile-time geometry
// (see config.h) since zone boundaries are tied to the physical corners of
// the house, not something to tune live.
extern Zone zones[KISLAP_NUM_ZONES];

// Derive zones[] from the current cfg zone geometry. Call once during setup(),
// before the render loop runs. (At first boot cfg still holds the compiled-in
// KISLAP_ZONE_* defaults, so this reproduces the original layout.)
void initZones();

// Re-derive the absolute zones[] bounds from cfg.zoneLengths/cfg.zoneIntros,
// auto-fitting the total to KISLAP_NUM_LEDS (the last zone absorbs any
// surplus/shortfall) and clamping each intro into [0, zone length]. The
// effective (fitted/clamped) values are written back into cfg so a subsequent
// GET or saveConfig() reflects exactly what the render loop is using. Call after
// loading NVS and after every config update that can touch zone geometry.
void applyZones();

// Runtime-tunable configuration. Every field here can be changed on the fly
// over the REST API without recompiling; changes are persisted to NVS and
// reloaded at boot. Hardware/sizing constants (pin, LED count/type, max ripple
// pool, zone layout) stay compile-time in config.h because they size arrays
// or feed FastLED template parameters and cannot change at runtime.
//
// The member defaults come from the KISLAP_* macros in config.h, so a fresh
// device (empty NVS) behaves exactly as the compiled-in defaults describe.
struct Config {
    uint8_t  brightness          = KISLAP_BRIGHTNESS;

    float    rippleSpeed         = KISLAP_RIPPLE_SPEED;
    float    rippleWidth         = KISLAP_RIPPLE_WIDTH;
    // Trailing-side width multiplier: >1 slows the return relative to the push.
    // See KISLAP_RIPPLE_RETURN_RATIO.
    float    rippleReturnRatio   = KISLAP_RIPPLE_RETURN_RATIO;
    float    rippleMinAmp        = KISLAP_RIPPLE_MIN_AMP;
    float    rippleMaxAmp        = KISLAP_RIPPLE_MAX_AMP;
    // Gap between ripple launches, in rendered frames (see config.h).
    uint32_t rippleMinIntervalFrames = KISLAP_RIPPLE_MIN_INTERVAL_FRAMES;
    uint32_t rippleMaxIntervalFrames = KISLAP_RIPPLE_MAX_INTERVAL_FRAMES;
    // Front/back shading strength: leading edge brightens, trailing edge
    // darkens. 0 disables. See KISLAP_RIPPLE_SHADE.
    float    rippleShade         = KISLAP_RIPPLE_SHADE;
    // Resting brightness of the pattern (0-1); crests brighten above it,
    // troughs darken below it. See KISLAP_RIPPLE_BASE_LEVEL.
    float    rippleBaseLevel     = KISLAP_RIPPLE_BASE_LEVEL;
    // Max ripples active at once. Clamped to [1, KISLAP_MAX_RIPPLES] (the
    // compile-time pool capacity) since it can't exceed the array size.
    int32_t  maxRipples          = KISLAP_MAX_RIPPLES;

    int32_t  stripeWidth         = KISLAP_STRIPE_WIDTH;
    int32_t  introStarEvery      = KISLAP_INTRO_STAR_EVERY;
    // Dimmest a star gets at the bottom of its twinkle. See KISLAP_INTRO_STAR_MIN.
    int32_t  introStarMin        = KISLAP_INTRO_STAR_MIN;
    uint32_t frameDelayMs        = KISLAP_FRAME_DELAY_MS;

    // Per-zone geometry, tunable at runtime. zoneLengths[z] is the zone's total
    // pixel count; zoneIntros[z] is how many of those (counted from the zone
    // start) are the blue/star intro. Defaults come from config.h. The zone
    // *count* (KISLAP_NUM_ZONES) stays compile-time because it sizes this array.
    // On apply the lengths are auto-fit to KISLAP_NUM_LEDS -- the last zone
    // absorbs any surplus/shortfall so the strip is always exactly covered -- and
    // the fitted values are written back here so GET/NVS reflect what took effect
    // (see applyZones()).
    int32_t  zoneLengths[KISLAP_NUM_ZONES] = KISLAP_ZONE_LENGTHS;
    int32_t  zoneIntros[KISLAP_NUM_ZONES]  = KISLAP_ZONE_INTRO_LENGTHS;
};

// The single live config instance, defined in runtime_config.cpp.
extern Config cfg;

// Load persisted values from NVS into cfg. Any field never saved keeps its
// compiled-in default. Applies side effects (e.g. FastLED brightness) via
// applyConfig(). Safe to call once during setup(), after FastLED is added.
void loadConfig();

// Persist the current cfg to NVS. Called after a successful update.
void saveConfig();

// Push cfg values into the systems that need them applied immediately
// (currently just FastLED brightness). Called after load and after each update.
void applyConfig();

// Serialize the full current config into `doc` for a GET response.
void configToJson(JsonDocument& doc);

// Apply a partial update from a parsed JSON object. Only fields present in
// `doc` are touched; each is range-checked and clamped. Cross-field invariants
// (min <= max pairs, positive divisors) are enforced afterward. Returns false
// with `err` set if the payload is structurally unusable; individual
// out-of-range fields are clamped rather than rejected.
bool configFromJson(const JsonDocument& doc, String& err);
