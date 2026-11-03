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

// Selectable display schemes (see KISLAP_DEFAULT_SCHEME in config.h). The
// numeric value is persisted to NVS, so append new schemes rather than
// renumbering existing ones.
enum Scheme : uint8_t {
    SCHEME_PATRIOTIC = 0,
    SCHEME_HALLOWEEN = 1,
    SCHEME_AUTUMN = 2,
    SCHEME_COUNT
};

// Runtime-tunable configuration. Every field here can be changed on the fly
// over the REST API without recompiling; changes are persisted to NVS and
// reloaded at boot. Hardware/sizing constants (pin, LED count/type, max ripple
// pool, zone layout) stay compile-time in config.h because they size arrays
// or feed FastLED template parameters and cannot change at runtime.
//
// The member defaults come from the KISLAP_* macros in config.h, so a fresh
// device (empty NVS) behaves exactly as the compiled-in defaults describe.
struct Config {
    // Which scheme the render loop draws (a Scheme value).
    uint8_t  scheme              = KISLAP_DEFAULT_SCHEME;
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

    // Halloween scheme geometry (see config.h). Section width excludes the black
    // separator pixel; fade width is clamped to at most half a section.
    int32_t  halloweenSectionWidth = KISLAP_HALLOWEEN_SECTION_WIDTH;
    int32_t  halloweenFadeWidth    = KISLAP_HALLOWEEN_FADE_WIDTH;

    // Halloween lightning (see config.h). Interval is wall-clock ms between
    // strikes; length is the bolt's pixel span; ambient is how strongly a flash
    // washes the whole strip (0-255).
    bool     lightningEnabled        = KISLAP_LIGHTNING_ENABLED;
    uint32_t lightningMinIntervalMs  = KISLAP_LIGHTNING_MIN_INTERVAL_MS;
    uint32_t lightningMaxIntervalMs  = KISLAP_LIGHTNING_MAX_INTERVAL_MS;
    int32_t  lightningMinLength      = KISLAP_LIGHTNING_MIN_LENGTH;
    int32_t  lightningMaxLength      = KISLAP_LIGHTNING_MAX_LENGTH;
    int32_t  lightningAmbient        = KISLAP_LIGHTNING_AMBIENT;

    // Autumn scheme (see config.h). Sizes are pixels; rate is new leaves per
    // frame; fall times and gust intervals are frames; flutter and background
    // are 0-255 levels.
    int32_t  autumnLeafMinSize       = KISLAP_AUTUMN_LEAF_MIN_SIZE;
    int32_t  autumnLeafMaxSize       = KISLAP_AUTUMN_LEAF_MAX_SIZE;
    float    autumnLeafRate          = KISLAP_AUTUMN_LEAF_RATE;
    uint32_t autumnFallMinFrames     = KISLAP_AUTUMN_FALL_MIN_FRAMES;
    uint32_t autumnFallMaxFrames     = KISLAP_AUTUMN_FALL_MAX_FRAMES;
    float    autumnGustStrength      = KISLAP_AUTUMN_GUST_STRENGTH;
    uint32_t autumnGustMinIntervalFrames = KISLAP_AUTUMN_GUST_MIN_INTERVAL_FRAMES;
    uint32_t autumnGustMaxIntervalFrames = KISLAP_AUTUMN_GUST_MAX_INTERVAL_FRAMES;
    int32_t  autumnFlutter           = KISLAP_AUTUMN_FLUTTER;
    int32_t  autumnBackground        = KISLAP_AUTUMN_BACKGROUND;
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
