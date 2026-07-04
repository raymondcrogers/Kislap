#include "runtime_config.h"

#include <FastLED.h>
#include <Preferences.h>

Config cfg;
Zone zones[KISLAP_NUM_ZONES];

void applyZones() {
    int32_t pos = 0;
    for (int z = 0; z < KISLAP_NUM_ZONES; z++) {
        int32_t length;
        if (z == KISLAP_NUM_ZONES - 1) {
            // Last zone absorbs whatever LEDs remain so the strip is always
            // exactly covered, regardless of what the earlier lengths summed to.
            length = KISLAP_NUM_LEDS - pos;
            if (length < 1) length = 1;
        } else {
            // Leave at least one LED for each of the zones that still follow, so
            // an oversized earlier zone can't starve the rest of the strip.
            int32_t zonesAfter = (KISLAP_NUM_ZONES - 1) - z;
            int32_t maxLen = KISLAP_NUM_LEDS - pos - zonesAfter;
            if (maxLen < 1) maxLen = 1;
            length = constrain(cfg.zoneLengths[z], (int32_t)1, maxLen);
        }

        zones[z].start = pos;
        pos += length;
        zones[z].end = pos;
        zones[z].introPixels = constrain(cfg.zoneIntros[z], (int32_t)0, length);

        // Write the effective values back so GET/NVS mirror what took effect.
        cfg.zoneLengths[z] = length;
        cfg.zoneIntros[z]  = zones[z].introPixels;
    }
}

void initZones() {
    applyZones();
}

// NVS namespace and short keys. NVS keys are limited to 15 characters, so these
// are abbreviated rather than matching the JSON field names one-to-one.
static Preferences prefs;
static const char* NVS_NAMESPACE = "kislap";

namespace keys {
    static const char* brightness = "bright";
    static const char* rippleSpeed = "rspeed";
    static const char* rippleWidth = "rwidth";
    static const char* rippleReturnRatio = "rretratio";
    static const char* rippleMinAmp = "rminamp";
    static const char* rippleMaxAmp = "rmaxamp";
    static const char* rippleMinInt = "rminintf";
    static const char* rippleMaxInt = "rmaxintf";
    static const char* rippleShade = "rshade";
    static const char* rippleBaseLevel = "rbase";
    static const char* maxRipples = "maxrip";
    static const char* stripeWidth = "stripew";
    static const char* introStarEvery = "introse";
    static const char* introStarMin = "introsm";
    static const char* frameDelayMs = "framems";
    // Zone geometry stored as packed blobs (one int32 per zone) rather than a key
    // per zone, to stay well under the 15-char NVS key limit as zone count grows.
    static const char* zoneLengths = "zonelen";
    static const char* zoneIntros = "zoneintro";
}

// Keep every derived value sane no matter what was loaded or posted. Runs after
// both load and update so a half-written NVS or a partial POST can't wedge the
// render loop (e.g. a zero divisor or min > max).
static void sanitize() {
    // Amplitudes: non-negative and min <= max.
    if (cfg.rippleMinAmp < 0.0f) cfg.rippleMinAmp = 0.0f;
    if (cfg.rippleMaxAmp < cfg.rippleMinAmp) cfg.rippleMaxAmp = cfg.rippleMinAmp;

    // Intervals: min <= max (both unsigned, so only ordering to fix).
    if (cfg.rippleMaxIntervalFrames < cfg.rippleMinIntervalFrames) {
        cfg.rippleMaxIntervalFrames = cfg.rippleMinIntervalFrames;
    }

    // Shading strength is non-negative (0 disables).
    if (cfg.rippleShade < 0.0f) cfg.rippleShade = 0.0f;

    // Resting brightness is a 0-1 fraction.
    cfg.rippleBaseLevel = constrain(cfg.rippleBaseLevel, 0.0f, 1.0f);

    // Divisors and spreads must stay positive.
    if (cfg.rippleWidth < 0.1f) cfg.rippleWidth = 0.1f;
    if (cfg.rippleReturnRatio < 0.1f) cfg.rippleReturnRatio = 0.1f;
    if (cfg.stripeWidth < 1) cfg.stripeWidth = 1;
    if (cfg.introStarEvery < 1) cfg.introStarEvery = 1;

    // Star twinkle floor: 0 = dim end is full background blue, 255 = always
    // white. Clamped into the blend range.
    cfg.introStarMin = constrain(cfg.introStarMin, 0, 255);

    // Active ripple cap can't exceed the fixed pool or drop below one.
    cfg.maxRipples = constrain(cfg.maxRipples, 1, KISLAP_MAX_RIPPLES);
}

void applyConfig() {
    FastLED.setBrightness(cfg.brightness);
}

void loadConfig() {
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
    cfg.brightness          = prefs.getUChar(keys::brightness, cfg.brightness);
    cfg.rippleSpeed         = prefs.getFloat(keys::rippleSpeed, cfg.rippleSpeed);
    cfg.rippleWidth         = prefs.getFloat(keys::rippleWidth, cfg.rippleWidth);
    cfg.rippleReturnRatio   = prefs.getFloat(keys::rippleReturnRatio, cfg.rippleReturnRatio);
    cfg.rippleMinAmp        = prefs.getFloat(keys::rippleMinAmp, cfg.rippleMinAmp);
    cfg.rippleMaxAmp        = prefs.getFloat(keys::rippleMaxAmp, cfg.rippleMaxAmp);
    cfg.rippleMinIntervalFrames = prefs.getULong(keys::rippleMinInt, cfg.rippleMinIntervalFrames);
    cfg.rippleMaxIntervalFrames = prefs.getULong(keys::rippleMaxInt, cfg.rippleMaxIntervalFrames);
    cfg.rippleShade         = prefs.getFloat(keys::rippleShade, cfg.rippleShade);
    cfg.rippleBaseLevel     = prefs.getFloat(keys::rippleBaseLevel, cfg.rippleBaseLevel);
    cfg.maxRipples          = prefs.getLong(keys::maxRipples, cfg.maxRipples);
    cfg.stripeWidth         = prefs.getLong(keys::stripeWidth, cfg.stripeWidth);
    cfg.introStarEvery      = prefs.getLong(keys::introStarEvery, cfg.introStarEvery);
    cfg.introStarMin        = prefs.getLong(keys::introStarMin, cfg.introStarMin);
    cfg.frameDelayMs        = prefs.getULong(keys::frameDelayMs, cfg.frameDelayMs);

    // Zone geometry: packed int32 blobs. Only overwrite cfg's defaults if a blob
    // of the exact expected size was previously saved; a fresh, partially-written,
    // or wrong-length (e.g. after a KISLAP_NUM_ZONES change) NVS entry leaves the
    // compiled-in defaults in place.
    if (prefs.getBytesLength(keys::zoneLengths) == sizeof(cfg.zoneLengths))
        prefs.getBytes(keys::zoneLengths, cfg.zoneLengths, sizeof(cfg.zoneLengths));
    if (prefs.getBytesLength(keys::zoneIntros) == sizeof(cfg.zoneIntros))
        prefs.getBytes(keys::zoneIntros, cfg.zoneIntros, sizeof(cfg.zoneIntros));
    prefs.end();

    sanitize();
    applyZones();
    applyConfig();
}

void saveConfig() {
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    prefs.putUChar(keys::brightness, cfg.brightness);
    prefs.putFloat(keys::rippleSpeed, cfg.rippleSpeed);
    prefs.putFloat(keys::rippleWidth, cfg.rippleWidth);
    prefs.putFloat(keys::rippleReturnRatio, cfg.rippleReturnRatio);
    prefs.putFloat(keys::rippleMinAmp, cfg.rippleMinAmp);
    prefs.putFloat(keys::rippleMaxAmp, cfg.rippleMaxAmp);
    prefs.putULong(keys::rippleMinInt, cfg.rippleMinIntervalFrames);
    prefs.putULong(keys::rippleMaxInt, cfg.rippleMaxIntervalFrames);
    prefs.putFloat(keys::rippleShade, cfg.rippleShade);
    prefs.putFloat(keys::rippleBaseLevel, cfg.rippleBaseLevel);
    prefs.putLong(keys::maxRipples, cfg.maxRipples);
    prefs.putLong(keys::stripeWidth, cfg.stripeWidth);
    prefs.putLong(keys::introStarEvery, cfg.introStarEvery);
    prefs.putLong(keys::introStarMin, cfg.introStarMin);
    prefs.putULong(keys::frameDelayMs, cfg.frameDelayMs);
    prefs.putBytes(keys::zoneLengths, cfg.zoneLengths, sizeof(cfg.zoneLengths));
    prefs.putBytes(keys::zoneIntros,  cfg.zoneIntros,  sizeof(cfg.zoneIntros));
    prefs.end();
}

void configToJson(JsonDocument& doc) {
    doc["brightness"]          = cfg.brightness;
    doc["rippleSpeed"]         = cfg.rippleSpeed;
    doc["rippleWidth"]         = cfg.rippleWidth;
    doc["rippleReturnRatio"]   = cfg.rippleReturnRatio;
    doc["rippleMinAmp"]        = cfg.rippleMinAmp;
    doc["rippleMaxAmp"]        = cfg.rippleMaxAmp;
    doc["rippleMinIntervalFrames"] = cfg.rippleMinIntervalFrames;
    doc["rippleMaxIntervalFrames"] = cfg.rippleMaxIntervalFrames;
    doc["rippleShade"]         = cfg.rippleShade;
    doc["rippleBaseLevel"]     = cfg.rippleBaseLevel;
    doc["maxRipples"]          = cfg.maxRipples;
    doc["stripeWidth"]         = cfg.stripeWidth;
    doc["introStarEvery"]      = cfg.introStarEvery;
    doc["introStarMin"]        = cfg.introStarMin;
    doc["frameDelayMs"]        = cfg.frameDelayMs;

    // Zone geometry. `length` and `introPixels` are the settable inputs (see
    // configFromJson); `start`/`end` are the derived absolute bounds, echoed for
    // visibility. The zone count is fixed at KISLAP_NUM_ZONES.
    JsonArray zonesArr = doc["zones"].to<JsonArray>();
    for (int z = 0; z < KISLAP_NUM_ZONES; z++) {
        JsonObject zo = zonesArr.add<JsonObject>();
        zo["start"] = zones[z].start;
        zo["end"] = zones[z].end;
        zo["length"] = zones[z].end - zones[z].start;
        zo["introPixels"] = zones[z].introPixels;
    }
}

bool configFromJson(const JsonDocument& doc, String& err) {
    if (!doc.is<JsonObjectConst>()) {
        err = "body must be a JSON object";
        return false;
    }

    // Brightness is a hard 0-255 range; clamp explicitly since it's a uint8_t.
    if (doc["brightness"].is<int>()) {
        int b = doc["brightness"].as<int>();
        cfg.brightness = (uint8_t)constrain(b, 0, 255);
    }

    if (doc["rippleSpeed"].is<float>())  cfg.rippleSpeed  = doc["rippleSpeed"].as<float>();
    if (doc["rippleWidth"].is<float>())  cfg.rippleWidth  = doc["rippleWidth"].as<float>();
    if (doc["rippleReturnRatio"].is<float>()) cfg.rippleReturnRatio = doc["rippleReturnRatio"].as<float>();
    if (doc["rippleMinAmp"].is<float>()) cfg.rippleMinAmp = doc["rippleMinAmp"].as<float>();
    if (doc["rippleMaxAmp"].is<float>()) cfg.rippleMaxAmp = doc["rippleMaxAmp"].as<float>();
    if (doc["rippleShade"].is<float>())  cfg.rippleShade  = doc["rippleShade"].as<float>();
    if (doc["rippleBaseLevel"].is<float>()) cfg.rippleBaseLevel = doc["rippleBaseLevel"].as<float>();

    if (doc["rippleMinIntervalFrames"].is<uint32_t>())
        cfg.rippleMinIntervalFrames = doc["rippleMinIntervalFrames"].as<uint32_t>();
    if (doc["rippleMaxIntervalFrames"].is<uint32_t>())
        cfg.rippleMaxIntervalFrames = doc["rippleMaxIntervalFrames"].as<uint32_t>();

    if (doc["maxRipples"].is<int>())     cfg.maxRipples     = doc["maxRipples"].as<int>();
    if (doc["stripeWidth"].is<int>())    cfg.stripeWidth    = doc["stripeWidth"].as<int>();
    if (doc["introStarEvery"].is<int>()) cfg.introStarEvery = doc["introStarEvery"].as<int>();
    if (doc["introStarMin"].is<int>())   cfg.introStarMin   = doc["introStarMin"].as<int>();
    if (doc["frameDelayMs"].is<uint32_t>())
        cfg.frameDelayMs = doc["frameDelayMs"].as<uint32_t>();

    // Zone geometry. Accept an array positionally aligned with the zones (index 0
    // = zone 0, ...). Only the elements present are touched, and within each only
    // the fields present, so a partial update leaves the rest alone. Elements past
    // KISLAP_NUM_ZONES are ignored; the count itself is not settable. The raw
    // values are stored as-is here -- applyZones() below auto-fits and clamps them.
    if (doc["zones"].is<JsonArrayConst>()) {
        JsonArrayConst zonesArr = doc["zones"].as<JsonArrayConst>();
        int z = 0;
        for (JsonVariantConst zv : zonesArr) {
            if (z >= KISLAP_NUM_ZONES) break;
            if (zv.is<JsonObjectConst>()) {
                if (zv["length"].is<int>())      cfg.zoneLengths[z] = zv["length"].as<int>();
                if (zv["introPixels"].is<int>()) cfg.zoneIntros[z]  = zv["introPixels"].as<int>();
            }
            z++;
        }
    }

    // Enforce cross-field invariants and clamp anything out of range.
    sanitize();
    applyZones();
    applyConfig();
    return true;
}
