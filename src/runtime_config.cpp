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
    static const char* scheme = "scheme";
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
    static const char* halloweenSection = "hwsection";
    static const char* halloweenFade = "hwfade";
    static const char* lightningEnabled = "ltenable";
    static const char* lightningMinInt = "ltminms";
    static const char* lightningMaxInt = "ltmaxms";
    static const char* lightningMinLen = "ltminlen";
    static const char* lightningMaxLen = "ltmaxlen";
    static const char* lightningAmbient = "ltambient";
    static const char* autumnLeafMinSize = "atleafmin";
    static const char* autumnLeafMaxSize = "atleafmax";
    static const char* autumnLeafRate = "atrate";
    static const char* autumnFallMin = "atfallmin";
    static const char* autumnFallMax = "atfallmax";
    static const char* autumnGustStrength = "atguststr";
    static const char* autumnGustMinInt = "atgustmin";
    static const char* autumnGustMaxInt = "atgustmax";
    static const char* autumnFlutter = "atflutter";
    static const char* autumnBackground = "atbg";
}

// JSON names for each Scheme, indexed by its enum value.
static const char* const SCHEME_NAMES[SCHEME_COUNT] = { "patriotic", "halloween", "autumn" };

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

    // An unknown scheme (e.g. NVS written by a newer build) falls back to default.
    if (cfg.scheme >= SCHEME_COUNT) cfg.scheme = KISLAP_DEFAULT_SCHEME;

    // Halloween: at least one pixel per section, and the two end fades can't
    // run past the middle of a section.
    if (cfg.halloweenSectionWidth < 1) cfg.halloweenSectionWidth = 1;
    cfg.halloweenFadeWidth =
        constrain(cfg.halloweenFadeWidth, 0, cfg.halloweenSectionWidth / 2);

    // Lightning: min <= max interval, and a bolt spans at least two pixels and
    // fits on the strip (min <= max).
    if (cfg.lightningMaxIntervalMs < cfg.lightningMinIntervalMs) {
        cfg.lightningMaxIntervalMs = cfg.lightningMinIntervalMs;
    }
    cfg.lightningMinLength = constrain(cfg.lightningMinLength, 2, KISLAP_NUM_LEDS - 1);
    cfg.lightningMaxLength =
        constrain(cfg.lightningMaxLength, cfg.lightningMinLength, KISLAP_NUM_LEDS - 1);
    cfg.lightningAmbient = constrain(cfg.lightningAmbient, 0, 255);

    // Autumn: leaf sizes and fall times at least 1 with min <= max, rate and
    // gust strength non-negative, gust interval min <= max, 0-255 levels.
    cfg.autumnLeafMinSize = constrain(cfg.autumnLeafMinSize, 1, 255);
    cfg.autumnLeafMaxSize = constrain(cfg.autumnLeafMaxSize, cfg.autumnLeafMinSize, 255);
    cfg.autumnLeafRate = constrain(cfg.autumnLeafRate, 0.0f, (float)KISLAP_AUTUMN_MAX_LEAVES);
    if (cfg.autumnFallMinFrames < 1) cfg.autumnFallMinFrames = 1;
    if (cfg.autumnFallMaxFrames < cfg.autumnFallMinFrames) {
        cfg.autumnFallMaxFrames = cfg.autumnFallMinFrames;
    }
    if (cfg.autumnGustStrength < 0.0f) cfg.autumnGustStrength = 0.0f;
    if (cfg.autumnGustMinIntervalFrames < 1) cfg.autumnGustMinIntervalFrames = 1;
    if (cfg.autumnGustMaxIntervalFrames < cfg.autumnGustMinIntervalFrames) {
        cfg.autumnGustMaxIntervalFrames = cfg.autumnGustMinIntervalFrames;
    }
    cfg.autumnFlutter = constrain(cfg.autumnFlutter, 0, 255);
    cfg.autumnBackground = constrain(cfg.autumnBackground, 0, 255);
}

void applyConfig() {
    FastLED.setBrightness(cfg.brightness);
}

void loadConfig() {
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
    cfg.scheme              = prefs.getUChar(keys::scheme, cfg.scheme);
    cfg.brightness         = prefs.getUChar(keys::brightness, cfg.brightness);
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
    cfg.halloweenSectionWidth = prefs.getLong(keys::halloweenSection, cfg.halloweenSectionWidth);
    cfg.halloweenFadeWidth  = prefs.getLong(keys::halloweenFade, cfg.halloweenFadeWidth);
    cfg.lightningEnabled    = prefs.getBool(keys::lightningEnabled, cfg.lightningEnabled);
    cfg.lightningMinIntervalMs = prefs.getULong(keys::lightningMinInt, cfg.lightningMinIntervalMs);
    cfg.lightningMaxIntervalMs = prefs.getULong(keys::lightningMaxInt, cfg.lightningMaxIntervalMs);
    cfg.lightningMinLength  = prefs.getLong(keys::lightningMinLen, cfg.lightningMinLength);
    cfg.lightningMaxLength  = prefs.getLong(keys::lightningMaxLen, cfg.lightningMaxLength);
    cfg.lightningAmbient    = prefs.getLong(keys::lightningAmbient, cfg.lightningAmbient);
    cfg.autumnLeafMinSize   = prefs.getLong(keys::autumnLeafMinSize, cfg.autumnLeafMinSize);
    cfg.autumnLeafMaxSize   = prefs.getLong(keys::autumnLeafMaxSize, cfg.autumnLeafMaxSize);
    cfg.autumnLeafRate      = prefs.getFloat(keys::autumnLeafRate, cfg.autumnLeafRate);
    cfg.autumnFallMinFrames = prefs.getULong(keys::autumnFallMin, cfg.autumnFallMinFrames);
    cfg.autumnFallMaxFrames = prefs.getULong(keys::autumnFallMax, cfg.autumnFallMaxFrames);
    cfg.autumnGustStrength  = prefs.getFloat(keys::autumnGustStrength, cfg.autumnGustStrength);
    cfg.autumnGustMinIntervalFrames = prefs.getULong(keys::autumnGustMinInt, cfg.autumnGustMinIntervalFrames);
    cfg.autumnGustMaxIntervalFrames = prefs.getULong(keys::autumnGustMaxInt, cfg.autumnGustMaxIntervalFrames);
    cfg.autumnFlutter       = prefs.getLong(keys::autumnFlutter, cfg.autumnFlutter);
    cfg.autumnBackground    = prefs.getLong(keys::autumnBackground, cfg.autumnBackground);

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
    prefs.putUChar(keys::scheme, cfg.scheme);
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
    prefs.putLong(keys::halloweenSection, cfg.halloweenSectionWidth);
    prefs.putLong(keys::halloweenFade, cfg.halloweenFadeWidth);
    prefs.putBool(keys::lightningEnabled, cfg.lightningEnabled);
    prefs.putULong(keys::lightningMinInt, cfg.lightningMinIntervalMs);
    prefs.putULong(keys::lightningMaxInt, cfg.lightningMaxIntervalMs);
    prefs.putLong(keys::lightningMinLen, cfg.lightningMinLength);
    prefs.putLong(keys::lightningMaxLen, cfg.lightningMaxLength);
    prefs.putLong(keys::lightningAmbient, cfg.lightningAmbient);
    prefs.putLong(keys::autumnLeafMinSize, cfg.autumnLeafMinSize);
    prefs.putLong(keys::autumnLeafMaxSize, cfg.autumnLeafMaxSize);
    prefs.putFloat(keys::autumnLeafRate, cfg.autumnLeafRate);
    prefs.putULong(keys::autumnFallMin, cfg.autumnFallMinFrames);
    prefs.putULong(keys::autumnFallMax, cfg.autumnFallMaxFrames);
    prefs.putFloat(keys::autumnGustStrength, cfg.autumnGustStrength);
    prefs.putULong(keys::autumnGustMinInt, cfg.autumnGustMinIntervalFrames);
    prefs.putULong(keys::autumnGustMaxInt, cfg.autumnGustMaxIntervalFrames);
    prefs.putLong(keys::autumnFlutter, cfg.autumnFlutter);
    prefs.putLong(keys::autumnBackground, cfg.autumnBackground);
    prefs.putBytes(keys::zoneLengths, cfg.zoneLengths, sizeof(cfg.zoneLengths));
    prefs.putBytes(keys::zoneIntros,  cfg.zoneIntros,  sizeof(cfg.zoneIntros));
    prefs.end();
}

void configToJson(JsonDocument& doc) {
    doc["scheme"]              = SCHEME_NAMES[cfg.scheme];
    doc["brightness"]         = cfg.brightness;
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
    doc["halloweenSectionWidth"] = cfg.halloweenSectionWidth;
    doc["halloweenFadeWidth"]  = cfg.halloweenFadeWidth;
    doc["lightningEnabled"]    = cfg.lightningEnabled;
    doc["lightningMinIntervalMs"] = cfg.lightningMinIntervalMs;
    doc["lightningMaxIntervalMs"] = cfg.lightningMaxIntervalMs;
    doc["lightningMinLength"]  = cfg.lightningMinLength;
    doc["lightningMaxLength"]  = cfg.lightningMaxLength;
    doc["lightningAmbient"]    = cfg.lightningAmbient;
    doc["autumnLeafMinSize"]   = cfg.autumnLeafMinSize;
    doc["autumnLeafMaxSize"]   = cfg.autumnLeafMaxSize;
    doc["autumnLeafRate"]      = cfg.autumnLeafRate;
    doc["autumnFallMinFrames"] = cfg.autumnFallMinFrames;
    doc["autumnFallMaxFrames"] = cfg.autumnFallMaxFrames;
    doc["autumnGustStrength"]  = cfg.autumnGustStrength;
    doc["autumnGustMinIntervalFrames"] = cfg.autumnGustMinIntervalFrames;
    doc["autumnGustMaxIntervalFrames"] = cfg.autumnGustMaxIntervalFrames;
    doc["autumnFlutter"]       = cfg.autumnFlutter;
    doc["autumnBackground"]    = cfg.autumnBackground;

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

    // Scheme, by name (case-insensitive). Validated before any other field is
    // touched so a bad name rejects the whole update rather than half-applying it.
    if (!doc["scheme"].isNull()) {
        const char* name = doc["scheme"].as<const char*>();
        int found = -1;
        for (int s = 0; name && s < SCHEME_COUNT; s++) {
            if (strcasecmp(name, SCHEME_NAMES[s]) == 0) found = s;
        }
        if (found < 0) {
            err = "scheme must be \"patriotic\", \"halloween\" or \"autumn\"";
            return false;
        }
        cfg.scheme = (uint8_t)found;
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
    if (doc["halloweenSectionWidth"].is<int>())
        cfg.halloweenSectionWidth = doc["halloweenSectionWidth"].as<int>();
    if (doc["halloweenFadeWidth"].is<int>())
        cfg.halloweenFadeWidth = doc["halloweenFadeWidth"].as<int>();
    if (doc["lightningEnabled"].is<bool>())
        cfg.lightningEnabled = doc["lightningEnabled"].as<bool>();
    if (doc["lightningMinIntervalMs"].is<uint32_t>())
        cfg.lightningMinIntervalMs = doc["lightningMinIntervalMs"].as<uint32_t>();
    if (doc["lightningMaxIntervalMs"].is<uint32_t>())
        cfg.lightningMaxIntervalMs = doc["lightningMaxIntervalMs"].as<uint32_t>();
    if (doc["lightningMinLength"].is<int>())
        cfg.lightningMinLength = doc["lightningMinLength"].as<int>();
    if (doc["lightningMaxLength"].is<int>())
        cfg.lightningMaxLength = doc["lightningMaxLength"].as<int>();
    if (doc["lightningAmbient"].is<int>())
        cfg.lightningAmbient = doc["lightningAmbient"].as<int>();
    if (doc["autumnLeafMinSize"].is<int>())
        cfg.autumnLeafMinSize = doc["autumnLeafMinSize"].as<int>();
    if (doc["autumnLeafMaxSize"].is<int>())
        cfg.autumnLeafMaxSize = doc["autumnLeafMaxSize"].as<int>();
    if (doc["autumnLeafRate"].is<float>())
        cfg.autumnLeafRate = doc["autumnLeafRate"].as<float>();
    if (doc["autumnFallMinFrames"].is<uint32_t>())
        cfg.autumnFallMinFrames = doc["autumnFallMinFrames"].as<uint32_t>();
    if (doc["autumnFallMaxFrames"].is<uint32_t>())
        cfg.autumnFallMaxFrames = doc["autumnFallMaxFrames"].as<uint32_t>();
    if (doc["autumnGustStrength"].is<float>())
        cfg.autumnGustStrength = doc["autumnGustStrength"].as<float>();
    if (doc["autumnGustMinIntervalFrames"].is<uint32_t>())
        cfg.autumnGustMinIntervalFrames = doc["autumnGustMinIntervalFrames"].as<uint32_t>();
    if (doc["autumnGustMaxIntervalFrames"].is<uint32_t>())
        cfg.autumnGustMaxIntervalFrames = doc["autumnGustMaxIntervalFrames"].as<uint32_t>();
    if (doc["autumnFlutter"].is<int>())
        cfg.autumnFlutter = doc["autumnFlutter"].as<int>();
    if (doc["autumnBackground"].is<int>())
        cfg.autumnBackground = doc["autumnBackground"].as<int>();

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
