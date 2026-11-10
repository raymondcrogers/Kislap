#include "runtime_config.h"

#include <stddef.h>
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

// NVS namespace. Each scalar setting's NVS key lives in its CONFIG_FIELDS row
// below. NVS keys are limited to 15 characters, so they are abbreviated rather
// than matching the JSON field names one-to-one. Never change an existing key:
// devices in the field would silently fall back to defaults.
static Preferences prefs;
static const char* NVS_NAMESPACE = "kislap";

// Zone geometry is stored as packed blobs (one int32 per zone) rather than a key
// per zone, to stay well under the 15-char NVS key limit as zone count grows.
namespace keys {
    constexpr const char* zoneLengths = "zonelen";
    constexpr const char* zoneIntros = "zoneintro";
    // Burnt LED list: burntLedCount followed by the whole burntLeds[] array.
    constexpr const char* burntLeds = "burnt";
}

// JSON names for each Scheme, indexed by its enum value. configFromJson
// validates "scheme" against this table and /api/schema lists it, so adding a
// scheme here (plus its enum value and label) updates both.
static const char* const SCHEME_NAMES[] = { "patriotic", "halloween", "autumn" };
// Human-readable labels for /api/schema, same order as SCHEME_NAMES.
static const char* const SCHEME_LABELS[] = { "Patriotic", "Halloween", "Autumn" };
static_assert(sizeof(SCHEME_NAMES) / sizeof(SCHEME_NAMES[0]) == SCHEME_COUNT,
              "SCHEME_NAMES needs exactly one name per Scheme enum value");
static_assert(sizeof(SCHEME_LABELS) / sizeof(SCHEME_LABELS[0]) == SCHEME_COUNT,
              "SCHEME_LABELS needs exactly one label per Scheme enum value");

// ---------------------------------------------------------------------------
// Field table: the single description of every runtime setting.
//
// One row per Config member, in the same order as the members are declared in
// runtime_config.h. Each row drives:
//   * configToJson / configFromJson (the row's JSON key and value type),
//   * loadConfig / saveConfig       (its NVS key and storage type),
//   * sanitize()                    (its single-field min/max clamp),
//   * GET /api/schema               (label, group, limits, step, unit, help).
// Because the clamp and the schema read the same min/max, the limits a client
// sees are always the limits the firmware enforces. Rules that involve two
// settings (min <= max pairs, the Halloween fade cap) stay hand-written in
// sanitize() and are described in the rows' help text.
//
// The whole table is constexpr, so it and every string in it live in flash.
// ---------------------------------------------------------------------------
namespace {

// What a setting is, as a client sees it (the schema's "type").
enum class Kind : uint8_t { Int, Float, Bool, Enum, String, Array };

// How a setting is stored in Config (and NVS).
enum class Store : uint8_t { U8, I32, U32, F32, Bool, Zones, Burnt };

// Store tag for a Config member's C++ type, so a row can't claim the wrong one.
template <typename T> struct StoreOf;
template <> struct StoreOf<uint8_t>  { static constexpr Store value = Store::U8; };
template <> struct StoreOf<int32_t>  { static constexpr Store value = Store::I32; };
template <> struct StoreOf<uint32_t> { static constexpr Store value = Store::U32; };
template <> struct StoreOf<float>    { static constexpr Store value = Store::F32; };
template <> struct StoreOf<bool>     { static constexpr Store value = Store::Bool; };

// Settings groups shown by clients. A group tied to a scheme only matters while
// that scheme is selected; groups with no scheme always apply.
enum Group : uint8_t {
    G_GENERAL, G_PATRIOTIC, G_RIPPLES, G_HALLOWEEN, G_LIGHTNING, G_AUTUMN,
    G_BURNT, G_COUNT
};

struct GroupDesc {
    const char* id;
    const char* label;
    int8_t scheme;  // a Scheme value, or -1 for scheme-independent
};

constexpr GroupDesc GROUPS[] = {
    { "general",   "General",      -1 },
    { "patriotic", "Patriotic",    SCHEME_PATRIOTIC },
    { "ripples",   "Wind ripples", SCHEME_PATRIOTIC },
    { "halloween", "Halloween",    SCHEME_HALLOWEEN },
    { "lightning", "Lightning",    SCHEME_HALLOWEEN },
    { "autumn",    "Autumn",       SCHEME_AUTUMN },
    { "burnt",     "Burnt LEDs",   -1 },
};
static_assert(sizeof(GROUPS) / sizeof(GROUPS[0]) == G_COUNT,
              "GROUPS needs exactly one entry per Group value");

enum : uint8_t { HAS_MIN = 1, HAS_MAX = 2 };

struct FieldDesc {
    const char* key;      // JSON name in /api/config (= the Config member name)
    const char* nvsKey;   // NVS key (nullptr for the zones array, stored apart)
    const char* label;
    const char* unit;     // "" when none
    const char* help;
    const char* const* options;  // Kind::Enum only: allowed names, by value
    uint8_t  optionCount;
    uint16_t offset;      // offsetof(Config, member)
    uint16_t size;        // bytes the row covers in Config
    Kind     kind;
    Store    store;
    Group    group;
    uint8_t  limits;      // HAS_MIN | HAS_MAX
    double   min;
    double   max;
    double   step;        // UI step hint (0 = none)
};

// Limit helpers for the FIELD rows. Each expands to the row's
// limits, min, max members.
#define LIM_NONE           0, 0.0, 0.0
#define LIM_MIN(lo)        HAS_MIN, (double)(lo), 0.0
#define LIM_RANGE(lo, hi)  (HAS_MIN | HAS_MAX), (double)(lo), (double)(hi)

// A scalar setting. The JSON key is the member name itself, so the two can
// never drift apart.
#define FIELD(member, kind, nvs, group, label, limits, step, unit, help)      \
    { #member, nvs, label, unit, help, nullptr, 0,                            \
      (uint16_t)offsetof(Config, member), (uint16_t)sizeof(Config::member),   \
      Kind::kind, StoreOf<decltype(Config::member)>::value, group, limits,    \
      step }

// A setting chosen by name from a fixed list (stored as its index).
#define ENUM_FIELD(member, nvs, group, label, names, help)                    \
    { #member, nvs, label, "", help, names,                                   \
      (uint8_t)(sizeof(names) / sizeof(names[0])),                            \
      (uint16_t)offsetof(Config, member), (uint16_t)sizeof(Config::member),   \
      Kind::Enum, StoreOf<decltype(Config::member)>::value, group, LIM_NONE,  \
      0 }

// Widest a single zone (or its intro) can be: every other zone keeps >= 1 LED.
#define ZONE_MAX_PIXELS (KISLAP_NUM_LEDS - (KISLAP_NUM_ZONES - 1))

constexpr FieldDesc FIELDS[] = {
    ENUM_FIELD(scheme, "scheme", G_GENERAL, "Scheme", SCHEME_NAMES,
        "Which look the strip shows; each scheme keeps its own settings while "
        "another one is showing."),
    FIELD(brightness, Int, "bright", G_GENERAL, "Brightness",
        LIM_RANGE(0, 255), 1, "",
        "Global LED brightness."),

    FIELD(rippleSpeed, Float, "rspeed", G_RIPPLES, "Ripple speed",
        LIM_NONE, 0.1, "px/frame",
        "How far each wind ripple travels along the strip per frame."),
    FIELD(rippleWidth, Float, "rwidth", G_RIPPLES, "Ripple width",
        LIM_MIN(0.1), 0.5, "px",
        "Spread of a typical ripple; stronger ripples are drawn proportionally "
        "wider."),
    FIELD(rippleReturnRatio, Float, "rretratio", G_RIPPLES, "Return ratio",
        LIM_MIN(0.1), 0.1, "",
        "How much wider a ripple's trailing side is than its leading side "
        "(1 = symmetric, above 1 = slower, gentler return)."),
    FIELD(rippleMinAmp, Float, "rminamp", G_RIPPLES, "Min strength",
        LIM_MIN(0), 0.5, "px",
        "Smallest peak displacement a ripple can have; keep it at or below "
        "Max strength."),
    FIELD(rippleMaxAmp, Float, "rmaxamp", G_RIPPLES, "Max strength",
        LIM_MIN(0), 0.5, "px",
        "Largest peak displacement a ripple can have; raised to Min strength "
        "if set lower."),
    FIELD(rippleMinIntervalFrames, Int, "rminintf", G_RIPPLES, "Min gap",
        LIM_MIN(0), 1, "frames",
        "Fewest frames between ripple launches; keep it at or below Max gap."),
    FIELD(rippleMaxIntervalFrames, Int, "rmaxintf", G_RIPPLES, "Max gap",
        LIM_MIN(0), 1, "frames",
        "Most frames between ripple launches; raised to Min gap if set lower."),
    FIELD(rippleShade, Float, "rshade", G_RIPPLES, "Shading",
        LIM_MIN(0), 0.01, "",
        "How strongly a ripple's leading edge brightens and its trailing edge "
        "darkens (0 = off)."),
    FIELD(rippleBaseLevel, Float, "rbase", G_RIPPLES, "Resting level",
        LIM_RANGE(0, 1), 0.01, "",
        "Brightness of the pattern where no ripple is passing, as a fraction "
        "of full, leaving headroom for crests to brighten."),
    FIELD(maxRipples, Int, "maxrip", G_RIPPLES, "Max ripples",
        LIM_RANGE(1, KISLAP_MAX_RIPPLES), 1, "",
        "Most ripples allowed in flight at once."),

    FIELD(stripeWidth, Int, "stripew", G_PATRIOTIC, "Stripe width",
        LIM_MIN(1), 1, "px",
        "Width of each red or white stripe."),
    FIELD(introStarEvery, Int, "introse", G_PATRIOTIC, "Star spacing",
        LIM_MIN(1), 1, "px",
        "Every Nth pixel of each zone's blue intro is a twinkling white star."),
    FIELD(introStarMin, Int, "introsm", G_PATRIOTIC, "Star dim level",
        LIM_RANGE(0, 255), 1, "",
        "How bright a star stays at the dim end of its twinkle (0 = fades "
        "fully into the blue, 255 = always white)."),
    FIELD(frameDelayMs, Int, "framems", G_GENERAL, "Frame delay",
        LIM_MIN(0), 1, "ms",
        "Pause after each rendered frame; larger values slow every animation "
        "down evenly."),

    // zoneLengths + zoneIntros, shown to clients as one "zones" array of
    // objects. Its JSON, NVS and clamping are handled by hand (see
    // configToJson, configFromJson, loadConfig, applyZones); its items are
    // described by ZONE_ITEMS below.
    { "zones", nullptr, "Zones", "",
      "Each zone covers one side of the house: a blue intro with twinkling "
      "stars, then red and white stripes; the last zone's length is set "
      "automatically so the zones always cover the whole strip.",
      nullptr, 0,
      (uint16_t)offsetof(Config, zoneLengths),
      (uint16_t)(sizeof(Config::zoneLengths) + sizeof(Config::zoneIntros)),
      Kind::Array, Store::Zones, G_PATRIOTIC, LIM_NONE, 0 },

    FIELD(halloweenSectionWidth, Int, "hwsection", G_HALLOWEEN, "Section width",
        LIM_MIN(1), 1, "px",
        "Width of each green or purple section, not counting the black pixel "
        "between sections."),
    FIELD(halloweenFadeWidth, Int, "hwfade", G_HALLOWEEN, "Fade width",
        LIM_MIN(0), 1, "px",
        "Pixels at each end of a section that fade to black; capped at half "
        "the Section width."),

    FIELD(lightningEnabled, Bool, "ltenable", G_LIGHTNING, "Lightning",
        LIM_NONE, 0, "",
        "Draw occasional lightning strikes over the Halloween sections."),
    FIELD(lightningMinIntervalMs, Int, "ltminms", G_LIGHTNING, "Min gap",
        LIM_MIN(0), 100, "ms",
        "Shortest time between strikes; keep it at or below Max gap."),
    FIELD(lightningMaxIntervalMs, Int, "ltmaxms", G_LIGHTNING, "Max gap",
        LIM_MIN(0), 100, "ms",
        "Longest time between strikes; raised to Min gap if set lower."),
    FIELD(lightningMinLength, Int, "ltminlen", G_LIGHTNING, "Min bolt length",
        LIM_RANGE(2, KISLAP_NUM_LEDS - 1), 1, "px",
        "Shortest lightning bolt; keep it at or below Max bolt length."),
    FIELD(lightningMaxLength, Int, "ltmaxlen", G_LIGHTNING, "Max bolt length",
        LIM_RANGE(2, KISLAP_NUM_LEDS - 1), 1, "px",
        "Longest lightning bolt; raised to Min bolt length if set lower."),
    FIELD(lightningAmbient, Int, "ltambient", G_LIGHTNING, "Glow",
        LIM_RANGE(0, 255), 1, "",
        "How strongly each flash lights the pixels around the bolt, out to "
        "half the bolt's length past each end (0 = only the bolt lights up)."),

    FIELD(autumnLeafMinSize, Int, "atleafmin", G_AUTUMN, "Min leaf size",
        LIM_RANGE(1, 255), 1, "px",
        "Narrowest leaf; keep it at or below Max leaf size."),
    FIELD(autumnLeafMaxSize, Int, "atleafmax", G_AUTUMN, "Max leaf size",
        LIM_RANGE(1, 255), 1, "px",
        "Widest leaf; raised to Min leaf size if set lower."),
    FIELD(autumnLeafRate, Float, "atrate", G_AUTUMN, "Leaf rate",
        LIM_RANGE(0, KISLAP_AUTUMN_MAX_LEAVES), 0.05, "leaves/frame",
        "Average number of new leaves per frame in calm air (0.5 = one every "
        "other frame)."),
    FIELD(autumnFallMinFrames, Int, "atfallmin", G_AUTUMN, "Min fall time",
        LIM_MIN(1), 1, "frames",
        "Fewest frames a leaf takes to cross the strip in calm air; keep it "
        "at or below Max fall time."),
    FIELD(autumnFallMaxFrames, Int, "atfallmax", G_AUTUMN, "Max fall time",
        LIM_MIN(1), 1, "frames",
        "Most frames a leaf takes to cross the strip in calm air; raised to "
        "Min fall time if set lower."),
    FIELD(autumnGustStrength, Float, "atguststr", G_AUTUMN, "Gust strength",
        LIM_MIN(0), 0.1, "",
        "How much a full gust speeds up leaf rate, falling and tumbling "
        "(each up to 1 + this times; 0 = no gusts)."),
    FIELD(autumnGustMinIntervalFrames, Int, "atgustmin", G_AUTUMN, "Min gust gap",
        LIM_MIN(1), 1, "frames",
        "Fewest frames between gusts; keep it at or below Max gust gap."),
    FIELD(autumnGustMaxIntervalFrames, Int, "atgustmax", G_AUTUMN, "Max gust gap",
        LIM_MIN(1), 1, "frames",
        "Most frames between gusts; raised to Min gust gap if set lower."),
    FIELD(autumnFlutter, Int, "atflutter", G_AUTUMN, "Flutter",
        LIM_RANGE(0, 255), 1, "",
        "How much leaves dim at the bottom of their flicker in a full gust "
        "(calm air flutters about a third as much; 0 = off)."),
    FIELD(autumnBackground, Int, "atbg", G_AUTUMN, "Background",
        LIM_RANGE(0, 255), 1, "",
        "Brightness of the dim backdrop between leaves (0 = black)."),
    FIELD(autumnGreenPercent, Int, "atgreen", G_AUTUMN, "Green leaves",
        LIM_RANGE(0, 100), 1, "%",
        "Share of new leaves that are still green instead of a fall color "
        "(0 = none)."),

    FIELD(burntTolerance, Int, "burnttol", G_BURNT, "Color tolerance",
        LIM_RANGE(0, 100), 1, "%",
        "How much of a dead color a burnt LED may be asked to show, as a "
        "percent of the color's strongest part, and still light up (0 = any "
        "amount of a dead color turns it off)."),
    // burntLedCount + burntLeds, shown to clients as one "burntLeds" array of
    // objects. Its JSON, NVS and cleanup are handled by hand (see
    // configToJson, configFromJson, loadConfig, sanitize); its items are
    // described by BURNT_ITEMS below.
    { "burntLeds", nullptr, "Burnt LEDs", "",
      "Damaged LEDs and which of their colors still work; each one lights only "
      "when it can show the color it is given, and stays off otherwise. "
      "Sending this list replaces the whole list.",
      nullptr, 0,
      (uint16_t)offsetof(Config, burntLedCount),
      (uint16_t)(sizeof(Config::burntLedCount) + sizeof(Config::burntLeds)),
      Kind::Array, Store::Burnt, G_BURNT, LIM_NONE, 0 },
};

constexpr size_t FIELD_COUNT = sizeof(FIELDS) / sizeof(FIELDS[0]);

// Items of each element of an array setting, in /api/config order.
enum : uint8_t { ITEM_READ_ONLY = 1, ITEM_READ_ONLY_ON_LAST = 2 };
struct ItemDesc {
    const char* key;
    Kind kind;
    uint8_t flags;
    uint8_t limits;
    double min;
    double max;
    const char* unit;
    const char* help;
};
constexpr ItemDesc ZONE_ITEMS[] = {
    { "start", Kind::Int, ITEM_READ_ONLY, LIM_NONE, "px",
      "First LED of the zone (worked out from the zone lengths)." },
    { "end", Kind::Int, ITEM_READ_ONLY, LIM_NONE, "px",
      "One past the last LED of the zone (worked out from the zone lengths)." },
    { "length", Kind::Int, ITEM_READ_ONLY_ON_LAST, LIM_RANGE(1, ZONE_MAX_PIXELS), "px",
      "LEDs in the zone; each earlier zone leaves at least one LED for every "
      "zone after it, and the last zone gets whatever is left." },
    { "introPixels", Kind::Int, 0, LIM_RANGE(0, ZONE_MAX_PIXELS), "px",
      "LEDs at the start of the zone that form the blue/star intro; capped at "
      "the zone's length." },
};
constexpr ItemDesc BURNT_ITEMS[] = {
    { "pixel", Kind::Int, 0, LIM_RANGE(0, KISLAP_NUM_LEDS - 1), "",
      "Position of the LED on the strip, counting from 0." },
    { "red", Kind::Bool, 0, LIM_NONE, "",
      "Whether the LED's red still works (left out = dead)." },
    { "green", Kind::Bool, 0, LIM_NONE, "",
      "Whether the LED's green still works (left out = dead)." },
    { "blue", Kind::Bool, 0, LIM_NONE, "",
      "Whether the LED's blue still works (left out = dead)." },
};

// JSON item key for each color flag, in KISLAP_LED_R/G/B order.
constexpr uint8_t BURNT_COLOR_FLAGS[] = { KISLAP_LED_R, KISLAP_LED_G, KISLAP_LED_B };
constexpr const char* const BURNT_COLOR_KEYS[] = { "red", "green", "blue" };

#undef FIELD
#undef ENUM_FIELD
#undef LIM_NONE
#undef LIM_MIN
#undef LIM_RANGE

// --- Compile-time checks that keep the table in sync with Config -----------
// (Recursive one-liners because the toolchain builds as C++11.)

constexpr size_t storeAlign(Store s) {
    return (s == Store::U8 || s == Store::Bool) ? 1 : 4;
}
constexpr size_t alignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

// Row i starts exactly where the previous row ended (plus alignment padding),
// and the last row reaches the end of Config. A Config member with no row
// leaves a gap, so the build fails. (The one gap this can't see is a new 1-3
// byte member that lands entirely inside existing padding; step 2 of the
// checklist in runtime_config.h covers that.)
constexpr bool rowsCoverConfig(size_t i, size_t end) {
    return i == FIELD_COUNT
        ? alignUp(end, alignof(Config)) == sizeof(Config)
        : FIELDS[i].offset == alignUp(end, storeAlign(FIELDS[i].store)) &&
          rowsCoverConfig(i + 1, FIELDS[i].offset + FIELDS[i].size);
}
static_assert(offsetof(Config, zoneIntros) ==
                  offsetof(Config, zoneLengths) + sizeof(Config::zoneLengths),
              "zoneIntros must directly follow zoneLengths in Config");
static_assert(offsetof(Config, burntLeds) ==
                  offsetof(Config, burntLedCount) + sizeof(Config::burntLedCount),
              "burntLeds must directly follow burntLedCount in Config");
static_assert(rowsCoverConfig(0, 0),
              "FIELDS in runtime_config.cpp is out of sync with "
              "Config: every member needs exactly one row, in declaration order");

constexpr bool kindFitsStore(Kind k, Store s) {
    return k == Kind::Int   ? (s == Store::U8 || s == Store::I32 || s == Store::U32)
         : k == Kind::Float ? s == Store::F32
         : k == Kind::Bool  ? s == Store::Bool
         : k == Kind::Enum  ? s == Store::U8
         : k == Kind::Array ? (s == Store::Zones || s == Store::Burnt)
         : false;
}
constexpr bool kindsFit(size_t i) {
    return i == FIELD_COUNT ||
           (kindFitsStore(FIELDS[i].kind, FIELDS[i].store) && kindsFit(i + 1));
}
static_assert(kindsFit(0), "a FIELDS row's Kind doesn't match its member's type");

constexpr bool charsEq(const char* a, const char* b) {
    return *a == *b && (*a == '\0' || charsEq(a + 1, b + 1));
}
constexpr bool strEq(const char* a, const char* b) {
    return a != nullptr && b != nullptr && charsEq(a, b);
}
constexpr size_t strLen(const char* s) { return *s ? 1 + strLen(s + 1) : 0; }

// No two rows share a JSON key or NVS key.
constexpr bool noDupAfter(size_t i, size_t j) {
    return j == FIELD_COUNT ||
           (!strEq(FIELDS[i].key, FIELDS[j].key) &&
            !strEq(FIELDS[i].nvsKey, FIELDS[j].nvsKey) && noDupAfter(i, j + 1));
}
// Every NVS key fits the 15-char limit and doesn't collide with the array blobs.
constexpr bool nvsKeyOk(const char* k) {
    return k == nullptr ||
           (strLen(k) <= 15 && !strEq(k, keys::zoneLengths) &&
            !strEq(k, keys::zoneIntros) && !strEq(k, keys::burntLeds));
}
constexpr bool keysUnique(size_t i) {
    return i == FIELD_COUNT ||
           (nvsKeyOk(FIELDS[i].nvsKey) && noDupAfter(i, i + 1) && keysUnique(i + 1));
}
static_assert(keysUnique(0),
              "duplicate JSON/NVS key, or NVS key longer than 15 chars, in FIELDS");

// --- Typed access to a row's value inside a Config --------------------------

template <typename T>
T& member(Config& c, const FieldDesc& f) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(&c) + f.offset);
}
template <typename T>
const T& member(const Config& c, const FieldDesc& f) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(&c) + f.offset);
}

// Compiled-in defaults (every member initialized from its KISLAP_* macro).
const Config DEFAULTS = Config();

template <typename T>
T clampToRow(T v, const FieldDesc& f) {
    if ((f.limits & HAS_MIN) && v < (T)f.min) v = (T)f.min;
    if ((f.limits & HAS_MAX) && v > (T)f.max) v = (T)f.max;
    return v;
}

// Apply a row's own min/max (or, for an enum, its option range) to cfg.
void clampField(const FieldDesc& f) {
    switch (f.store) {
        case Store::U8:
            if (f.kind == Kind::Enum) {
                // Unknown value (e.g. NVS written by a newer build) -> default.
                uint8_t& v = member<uint8_t>(cfg, f);
                if (v >= f.optionCount) v = member<uint8_t>(DEFAULTS, f);
            } else {
                member<uint8_t>(cfg, f) = clampToRow(member<uint8_t>(cfg, f), f);
            }
            break;
        case Store::I32: member<int32_t>(cfg, f)  = clampToRow(member<int32_t>(cfg, f), f); break;
        case Store::U32: member<uint32_t>(cfg, f) = clampToRow(member<uint32_t>(cfg, f), f); break;
        case Store::F32: member<float>(cfg, f)    = clampToRow(member<float>(cfg, f), f); break;
        case Store::Bool:
        case Store::Zones:
        case Store::Burnt:
            break;
    }
}

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Int:    return "int";
        case Kind::Float:  return "float";
        case Kind::Bool:   return "bool";
        case Kind::Enum:   return "enum";
        case Kind::String: return "string";
        case Kind::Array:  return "array";
    }
    return "string";
}

// Wrap a flash string so ArduinoJson stores the pointer instead of copying it.
inline JsonString flashStr(const char* s) { return JsonString(s, true); }

// Write one row's current value from `c` into `dst`.
// (Templated so `dst` can be a doc["key"] proxy: converting that to a
// JsonVariant first would not create the member.)
template <typename TDst>
void valueToJson(TDst&& dst, const FieldDesc& f, const Config& c) {
    switch (f.store) {
        case Store::U8:
            if (f.kind == Kind::Enum) {
                const uint8_t v = member<uint8_t>(c, f);
                dst.set(flashStr(f.options[v < f.optionCount ? v : 0]));
            } else {
                dst.set(member<uint8_t>(c, f));
            }
            break;
        case Store::I32:  dst.set(member<int32_t>(c, f)); break;
        case Store::U32:  dst.set(member<uint32_t>(c, f)); break;
        case Store::F32:  dst.set(member<float>(c, f)); break;
        case Store::Bool: dst.set(member<bool>(c, f)); break;
        case Store::Zones: break;
        case Store::Burnt: break;
    }
}

// Write a burnt LED list as [{ "pixel", "red", "green", "blue" }, ...].
void burntLedsToJson(JsonArray arr, const BurntLed* list, int32_t count) {
    for (int32_t k = 0; k < count; k++) {
        JsonObject o = arr.add<JsonObject>();
        o["pixel"] = list[k].pixel;
        for (int c = 0; c < 3; c++) {
            o[flashStr(BURNT_COLOR_KEYS[c])] = (list[k].works & BURNT_COLOR_FLAGS[c]) != 0;
        }
    }
}

// Find `name` (case-insensitive) among an enum row's options; -1 if absent.
int enumIndex(const FieldDesc& f, const char* name) {
    for (int i = 0; name && i < f.optionCount; i++) {
        if (strcasecmp(name, f.options[i]) == 0) return i;
    }
    return -1;
}

// e.g.  scheme must be "patriotic", "halloween" or "autumn"
String enumError(const FieldDesc& f) {
    String e = f.key;
    e += " must be ";
    for (int i = 0; i < f.optionCount; i++) {
        if (i > 0) e += (i == f.optionCount - 1) ? " or " : ", ";
        e += '"';
        e += f.options[i];
        e += '"';
    }
    return e;
}

}  // namespace

// Keep every derived value sane no matter what was loaded or posted. Runs after
// both load and update so a half-written NVS or a partial POST can't wedge the
// render loop (e.g. a zero divisor or min > max).
static void sanitize() {
    // Single-field limits come straight from the FIELDS table (the same min/max
    // /api/schema reports), e.g. maxRipples to [1, KISLAP_MAX_RIPPLES], every
    // 0-255 level, positive divisors and widths, unknown scheme -> default.
    for (const FieldDesc& f : FIELDS) clampField(f);

    // Cross-field rules. Each is also described in the affected rows' help text.

    // Pairs: max is raised to min when set below it.
    if (cfg.rippleMaxAmp < cfg.rippleMinAmp) cfg.rippleMaxAmp = cfg.rippleMinAmp;
    if (cfg.rippleMaxIntervalFrames < cfg.rippleMinIntervalFrames) {
        cfg.rippleMaxIntervalFrames = cfg.rippleMinIntervalFrames;
    }
    if (cfg.lightningMaxIntervalMs < cfg.lightningMinIntervalMs) {
        cfg.lightningMaxIntervalMs = cfg.lightningMinIntervalMs;
    }
    if (cfg.lightningMaxLength < cfg.lightningMinLength) {
        cfg.lightningMaxLength = cfg.lightningMinLength;
    }
    if (cfg.autumnLeafMaxSize < cfg.autumnLeafMinSize) {
        cfg.autumnLeafMaxSize = cfg.autumnLeafMinSize;
    }
    if (cfg.autumnFallMaxFrames < cfg.autumnFallMinFrames) {
        cfg.autumnFallMaxFrames = cfg.autumnFallMinFrames;
    }
    if (cfg.autumnGustMaxIntervalFrames < cfg.autumnGustMinIntervalFrames) {
        cfg.autumnGustMaxIntervalFrames = cfg.autumnGustMinIntervalFrames;
    }

    // Halloween: the two end fades can't run past the middle of a section.
    if (cfg.halloweenFadeWidth > cfg.halloweenSectionWidth / 2) {
        cfg.halloweenFadeWidth = cfg.halloweenSectionWidth / 2;
    }

    // Burnt LEDs: drop pixels past the end of the strip, sort by pixel, and keep
    // only the last entry given for a repeated pixel.
    cfg.burntLedCount = constrain(cfg.burntLedCount, (int32_t)0, (int32_t)KISLAP_MAX_BURNT_LEDS);
    int32_t n = 0;
    for (int32_t k = 0; k < cfg.burntLedCount; k++) {
        BurntLed b = cfg.burntLeds[k];
        if (b.pixel >= KISLAP_NUM_LEDS) continue;
        b.works &= (KISLAP_LED_R | KISLAP_LED_G | KISLAP_LED_B);
        // Insertion sort into the kept prefix [0, n).
        int32_t j = n;
        while (j > 0 && cfg.burntLeds[j - 1].pixel > b.pixel) {
            cfg.burntLeds[j] = cfg.burntLeds[j - 1];
            j--;
        }
        if (j > 0 && cfg.burntLeds[j - 1].pixel == b.pixel) {
            // Repeat: replace the earlier entry and undo the shift.
            cfg.burntLeds[j - 1] = b;
            for (int32_t m = j; m < n; m++) cfg.burntLeds[m] = cfg.burntLeds[m + 1];
            continue;
        }
        cfg.burntLeds[j] = b;
        n++;
    }
    cfg.burntLedCount = n;
    for (int32_t k = n; k < KISLAP_MAX_BURNT_LEDS; k++) cfg.burntLeds[k] = BurntLed{0, 0};
}

void applyConfig() {
    FastLED.setBrightness(cfg.brightness);
}

static const size_t BURNT_BLOB_SIZE = sizeof(Config::burntLedCount) + sizeof(Config::burntLeds);

void loadConfig() {
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
    for (const FieldDesc& f : FIELDS) {
        // Any key never saved keeps the value already in cfg (the default).
        switch (f.store) {
            case Store::U8:
                member<uint8_t>(cfg, f) = prefs.getUChar(f.nvsKey, member<uint8_t>(cfg, f));
                break;
            case Store::I32:
                member<int32_t>(cfg, f) = prefs.getLong(f.nvsKey, member<int32_t>(cfg, f));
                break;
            case Store::U32:
                member<uint32_t>(cfg, f) = prefs.getULong(f.nvsKey, member<uint32_t>(cfg, f));
                break;
            case Store::F32:
                member<float>(cfg, f) = prefs.getFloat(f.nvsKey, member<float>(cfg, f));
                break;
            case Store::Bool:
                member<bool>(cfg, f) = prefs.getBool(f.nvsKey, member<bool>(cfg, f));
                break;
            case Store::Zones:
            case Store::Burnt:
                break;
        }
    }

    // Zone geometry: packed int32 blobs. Only overwrite cfg's defaults if a blob
    // of the exact expected size was previously saved; a fresh, partially-written,
    // or wrong-length (e.g. after a KISLAP_NUM_ZONES change) NVS entry leaves the
    // compiled-in defaults in place.
    if (prefs.getBytesLength(keys::zoneLengths) == sizeof(cfg.zoneLengths))
        prefs.getBytes(keys::zoneLengths, cfg.zoneLengths, sizeof(cfg.zoneLengths));
    if (prefs.getBytesLength(keys::zoneIntros) == sizeof(cfg.zoneIntros))
        prefs.getBytes(keys::zoneIntros, cfg.zoneIntros, sizeof(cfg.zoneIntros));
    // Burnt LEDs: one blob holding burntLedCount and the whole burntLeds[] array,
    // so an empty list is still saved. Same exact-size rule as the zones (a
    // KISLAP_MAX_BURNT_LEDS change falls back to the compiled-in list).
    if (prefs.getBytesLength(keys::burntLeds) == BURNT_BLOB_SIZE)
        prefs.getBytes(keys::burntLeds, &cfg.burntLedCount, BURNT_BLOB_SIZE);
    prefs.end();

    sanitize();
    applyZones();
    applyConfig();
}

void saveConfig() {
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    for (const FieldDesc& f : FIELDS) {
        switch (f.store) {
            case Store::U8:   prefs.putUChar(f.nvsKey, member<uint8_t>(cfg, f)); break;
            case Store::I32:  prefs.putLong(f.nvsKey, member<int32_t>(cfg, f)); break;
            case Store::U32:  prefs.putULong(f.nvsKey, member<uint32_t>(cfg, f)); break;
            case Store::F32:  prefs.putFloat(f.nvsKey, member<float>(cfg, f)); break;
            case Store::Bool: prefs.putBool(f.nvsKey, member<bool>(cfg, f)); break;
            case Store::Zones: break;
            case Store::Burnt: break;
        }
    }
    prefs.putBytes(keys::zoneLengths, cfg.zoneLengths, sizeof(cfg.zoneLengths));
    prefs.putBytes(keys::zoneIntros,  cfg.zoneIntros,  sizeof(cfg.zoneIntros));
    prefs.putBytes(keys::burntLeds, &cfg.burntLedCount, BURNT_BLOB_SIZE);
    prefs.end();
}

void configToJson(JsonDocument& doc) {
    for (const FieldDesc& f : FIELDS) {
        if (f.kind != Kind::Array) valueToJson(doc[flashStr(f.key)], f, cfg);
    }

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

    burntLedsToJson(doc["burntLeds"].to<JsonArray>(), cfg.burntLeds, cfg.burntLedCount);
}

bool configFromJson(const JsonDocument& doc, String& err) {
    if (!doc.is<JsonObjectConst>()) {
        err = "body must be a JSON object";
        return false;
    }

    // Enums (scheme), by name (case-insensitive). Validated before any field is
    // touched so a bad name rejects the whole update rather than half-applying it.
    for (const FieldDesc& f : FIELDS) {
        if (f.kind != Kind::Enum) continue;
        JsonVariantConst v = doc[flashStr(f.key)];
        if (!v.isNull() && enumIndex(f, v.as<const char*>()) < 0) {
            err = enumError(f);
            return false;
        }
    }

    // Burnt LEDs, also checked up front: the list is replaced as a whole, so a
    // bad entry rejects the update instead of silently dropping that LED.
    JsonVariantConst burntV = doc["burntLeds"];
    if (!burntV.isNull()) {
        if (!burntV.is<JsonArrayConst>()) {
            err = "burntLeds must be an array";
            return false;
        }
        JsonArrayConst arr = burntV.as<JsonArrayConst>();
        if (arr.size() > KISLAP_MAX_BURNT_LEDS) {
            err = "burntLeds can list at most " + String(KISLAP_MAX_BURNT_LEDS) + " LEDs";
            return false;
        }
        for (JsonVariantConst e : arr) {
            const bool ok = e.is<JsonObjectConst>() && e["pixel"].is<int>() &&
                            e["pixel"].as<int>() >= 0 &&
                            e["pixel"].as<int>() < KISLAP_NUM_LEDS;
            if (!ok) {
                err = "each burntLeds entry needs a \"pixel\" from 0 to " +
                      String(KISLAP_NUM_LEDS - 1);
                return false;
            }
        }
    }

    // Scalars: only fields present with a usable type are touched; the raw
    // values are clamped by sanitize() below. Each type check mirrors what the
    // field accepts (e.g. a negative number is ignored for an unsigned field).
    for (const FieldDesc& f : FIELDS) {
        JsonVariantConst v = doc[flashStr(f.key)];
        if (v.isNull()) continue;
        switch (f.store) {
            case Store::U8:
                if (f.kind == Kind::Enum) {
                    member<uint8_t>(cfg, f) = (uint8_t)enumIndex(f, v.as<const char*>());
                } else if (v.is<int>()) {
                    // Clamp before narrowing so e.g. 300 becomes 255, not 44.
                    member<uint8_t>(cfg, f) = (uint8_t)constrain(v.as<int>(), 0, 255);
                }
                break;
            case Store::I32:
                if (v.is<int>()) member<int32_t>(cfg, f) = v.as<int>();
                break;
            case Store::U32:
                if (v.is<uint32_t>()) member<uint32_t>(cfg, f) = v.as<uint32_t>();
                break;
            case Store::F32:
                if (v.is<float>()) member<float>(cfg, f) = v.as<float>();
                break;
            case Store::Bool:
                if (v.is<bool>()) member<bool>(cfg, f) = v.as<bool>();
                break;
            case Store::Zones:
            case Store::Burnt:
                break;
        }
    }

    // Burnt LEDs (validated above): replace the whole list. A color left out
    // counts as dead, so {"pixel": 12} marks a completely dead LED.
    if (!burntV.isNull()) {
        int32_t n = 0;
        for (JsonVariantConst e : burntV.as<JsonArrayConst>()) {
            BurntLed b{ (uint16_t)e["pixel"].as<int>(), 0 };
            for (int c = 0; c < 3; c++) {
                if (e[flashStr(BURNT_COLOR_KEYS[c])].as<bool>()) b.works |= BURNT_COLOR_FLAGS[c];
            }
            cfg.burntLeds[n++] = b;
        }
        cfg.burntLedCount = n;
    }

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

// ---------------------------------------------------------------------------
// GET /api/schema
// ---------------------------------------------------------------------------

// Write a numeric limit/step as an int for int fields, as a number otherwise.
template <typename TDst>
static void setNumber(TDst&& dst, Kind kind, double v) {
    if (kind == Kind::Int) dst.set((long)v);
    else dst.set(v);
}

static void setLimits(JsonObject o, Kind kind, uint8_t limits, double mn, double mx) {
    if (limits & HAS_MIN) setNumber(o["min"], kind, mn);
    if (limits & HAS_MAX) setNumber(o["max"], kind, mx);
}

// One "fields" entry. Each entry gets its own small, short-lived document so the
// whole schema never has to sit in RAM at once.
static void writeFieldSchema(Print& out, const FieldDesc& f) {
    JsonDocument d;
    d["key"] = flashStr(f.key);
    d["label"] = flashStr(f.label);
    d["group"] = flashStr(GROUPS[f.group].id);
    d["type"] = flashStr(kindName(f.kind));

    if (f.kind == Kind::Enum) {
        JsonArray opts = d["options"].to<JsonArray>();
        for (int i = 0; i < f.optionCount; i++) opts.add(flashStr(f.options[i]));
        valueToJson(d["default"], f, DEFAULTS);
    } else if (f.kind == Kind::Array) {
        const bool zonesArr = (f.store == Store::Zones);
        if (zonesArr) {
            d["count"] = KISLAP_NUM_ZONES;
            d["fixedCount"] = true;
        } else {
            d["maxCount"] = KISLAP_MAX_BURNT_LEDS;
            d["fixedCount"] = false;
        }
        JsonArray items = d["items"].to<JsonArray>();
        const ItemDesc* itemTable = zonesArr ? ZONE_ITEMS : BURNT_ITEMS;
        const size_t itemCount = zonesArr ? sizeof(ZONE_ITEMS) / sizeof(ZONE_ITEMS[0])
                                          : sizeof(BURNT_ITEMS) / sizeof(BURNT_ITEMS[0]);
        for (size_t i = 0; i < itemCount; i++) {
            const ItemDesc& it = itemTable[i];
            JsonObject io = items.add<JsonObject>();
            io["key"] = flashStr(it.key);
            io["type"] = flashStr(kindName(it.kind));
            if (it.flags & ITEM_READ_ONLY) io["readOnly"] = true;
            if (it.flags & ITEM_READ_ONLY_ON_LAST) io["readOnlyOnLast"] = true;
            setLimits(io, it.kind, it.limits, it.min, it.max);
            if (it.kind == Kind::Int && !(it.flags & ITEM_READ_ONLY)) io["step"] = 1;
            io["unit"] = flashStr(it.unit);
            io["help"] = flashStr(it.help);
        }
        JsonArray def = d["default"].to<JsonArray>();
        if (zonesArr) {
            // Compiled-in zone layout (the settable parts).
            for (int z = 0; z < KISLAP_NUM_ZONES; z++) {
                JsonObject zo = def.add<JsonObject>();
                zo["length"] = DEFAULTS.zoneLengths[z];
                zo["introPixels"] = DEFAULTS.zoneIntros[z];
            }
        } else {
            burntLedsToJson(def, DEFAULTS.burntLeds, DEFAULTS.burntLedCount);
        }
    } else {
        setLimits(d.as<JsonObject>(), f.kind, f.limits, f.min, f.max);
        if (f.step > 0) setNumber(d["step"], f.kind, f.step);
        valueToJson(d["default"], f, DEFAULTS);
    }

    d["unit"] = flashStr(f.unit);
    d["help"] = flashStr(f.help);
    serializeJson(d, out);
}

void writeConfigSchema(Print& out) {
    out.print("{\"schemaVersion\":");
    out.print(KISLAP_SCHEMA_VERSION);

    out.print(",\"schemes\":[");
    for (int s = 0; s < SCHEME_COUNT; s++) {
        JsonDocument d;
        d["name"] = flashStr(SCHEME_NAMES[s]);
        d["label"] = flashStr(SCHEME_LABELS[s]);
        if (s > 0) out.print(',');
        serializeJson(d, out);
    }

    out.print("],\"groups\":[");
    for (int g = 0; g < G_COUNT; g++) {
        JsonDocument d;
        d["id"] = flashStr(GROUPS[g].id);
        d["label"] = flashStr(GROUPS[g].label);
        if (GROUPS[g].scheme >= 0) d["scheme"] = flashStr(SCHEME_NAMES[GROUPS[g].scheme]);
        if (g > 0) out.print(',');
        serializeJson(d, out);
    }

    out.print("],\"fields\":[");
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (i > 0) out.print(',');
        writeFieldSchema(out, FIELDS[i]);
    }
    out.print("]}");
}
