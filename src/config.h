#pragma once

// Pin connected to the LED data line.
#define KISLAP_DATA_PIN 5

// Number of LEDs in the strip.
#define KISLAP_NUM_LEDS 1521

// LED chipset, color order, and OTA hostname, selected by board profile.
// platformio.ini sets -DKISLAP_PROFILE_DEV or -DKISLAP_PROFILE_PROD per
// environment so the same source flashes either strand correctly.
#if defined(KISLAP_PROFILE_PROD)
    // Prod: house LEDs. WS2815 (800kHz, 12V, WS2812-style protocol) -- the same
    // chipset as the dev strand. The old WS2811_400 (400kHz) was the wrong timing
    // for this strand: it latched every bit as 1 and showed solid white. Color
    // order is RGB for now; the boot self-test reveals the true order (watch the
    // actual color of the "red" step), so change this to GRB if they come out
    // mislabeled.
    #define KISLAP_LED_TYPE WS2815
    #define KISLAP_COLOR_ORDER RGB
    // TODO: set once the prod board is on the network (see the serial
    // monitor at boot), and update upload_port in platformio.ini to match.
    #define KISLAP_HOSTNAME "192.168.68.180"
#elif defined(KISLAP_PROFILE_DEV)
    // Dev: WS2815 driver IC (12V, single-wire WS2812-style protocol), native
    // GRB color order.
    #define KISLAP_LED_TYPE WS2815
    #define KISLAP_COLOR_ORDER GRB
    #define KISLAP_HOSTNAME "192.168.68.179"
#else
    #error "Define KISLAP_PROFILE_DEV or KISLAP_PROFILE_PROD (see platformio.ini environments)"
#endif

// --- Runtime-tunable defaults ----------------------------------------------
// The values below seed the runtime Config (see runtime_config.h) at first
// boot. Once the device has run, live values come from NVS and can be changed
// over the REST API without recompiling; these macros are only the fallback
// defaults used before anything has been saved.

// Initial brightness (0-255).
#define KISLAP_BRIGHTNESS 128

// --- Schemes ---------------------------------------------------------------
// The strip shows one of several complete looks ("schemes"), selected live over
// the REST API ("scheme": "patriotic" | "halloween" | "autumn") and persisted
// to NVS.
// Switching schemes leaves every other scheme's settings untouched, so a scheme
// can be put away and brought back later exactly as it was.
//
//   SCHEME_PATRIOTIC -- per-zone blue/star intro + red/white stripes, pushed
//                       around by traveling wind ripples (everything under
//                       "Zones", "Intro section" and "Ripple effect" below).
//   SCHEME_HALLOWEEN -- alternating dark green / deep purple sections with
//                       faded ends (see "Halloween scheme" below).
//   SCHEME_AUTUMN    -- red/orange/yellow/brown leaves falling and blowing
//                       across the strip as if seen through a long slit,
//                       tumbling and flickering, with occasional gusts (see
//                       "Autumn scheme" below).
//
// Scheme used on a device that has never saved one.
#define KISLAP_DEFAULT_SCHEME SCHEME_HALLOWEEN

// --- Halloween scheme ------------------------------------------------------
// Static pattern of alternating colored sections running the length of the
// whole strip (zones don't apply). Each section is
// KISLAP_HALLOWEEN_SECTION_WIDTH pixels, and a single fully-black pixel sits
// between neighboring sections. The first and last KISLAP_HALLOWEEN_FADE_WIDTH
// pixels of each section ramp between near-black and the full section color, so
// each section fades in from, and out to, its black separator.
#define KISLAP_HALLOWEEN_COLOR_A 0x004400  // DarkGreen
#define KISLAP_HALLOWEEN_COLOR_B 0x110015  // DeepPurple
#define KISLAP_HALLOWEEN_SECTION_WIDTH 40
#define KISLAP_HALLOWEEN_FADE_WIDTH 5

// --- Halloween lightning ---------------------------------------------------
// Sporadic lightning drawn over the Halloween sections. Each strike picks a
// random start pixel and a random end pixel KISLAP_LIGHTNING_MIN/MAX_LENGTH
// pixels away (either direction), then:
//   1. leader  -- a dim, crackling channel races from the start pixel to the
//                 end pixel over a few frames, with a bright tip;
//   2. strobe  -- the full bolt flashes on and off several times (the first,
//                 brightest flash being the "return stroke"), each flash also
//                 washing the surrounding pixels toward white as if lighting
//                 the sky -- out to half the bolt's length past each end;
//   3. afterglow -- the channel fades back down to the base pattern.
// The gap between strikes is wall-clock time (so "about every 5 seconds" holds
// regardless of frame rate), picked at random from the range below. The leader
// and strobe are paced in frames so every flash and gap lasts at least one
// rendered frame and is never skipped.
#define KISLAP_LIGHTNING_ENABLED true
#define KISLAP_LIGHTNING_MIN_INTERVAL_MS 3000
#define KISLAP_LIGHTNING_MAX_INTERVAL_MS 7000
#define KISLAP_LIGHTNING_MIN_LENGTH 60
#define KISLAP_LIGHTNING_MAX_LENGTH 200
// Bolt color: a cold blue-white.
#define KISLAP_LIGHTNING_COLOR 0xC0D0FF
// How far the pixels around the bolt are blended toward the bolt color during
// a full-strength flash (0-255). The glow covers the bolt plus half its length
// past each end, tapering to nothing at the edge. 0 lights only the bolt itself.
#define KISLAP_LIGHTNING_AMBIENT 48

// --- Autumn scheme ---------------------------------------------------------
// The strip is treated as a long horizontal slit that colorful leaves fall and
// blow across. Nothing slides along the strip: each leaf pops into view at a
// random spot, crosses the slit, and is gone. While it crosses, the slice of it
// the slit shows starts as a sliver (the leaf's tip), widens to the leaf's full
// width at its middle, and narrows back to nothing (its stem end). On the way
// through, the leaf tumbles: its visible width pulses as it turns edge-on and
// back, its color swings between its front and its paler back side, and it
// flickers as it catches the light. Every so often a gust rolls in: leaves come
// thicker and faster, and tumble and flicker harder, before it dies back down.
//
// Palette colors, picked to read correctly on LEDs (brown is a dim orange, and
// "light yellow" is a warm yellow so it doesn't look green or white). Each leaf
// gets a random color from (or between) these.
#define KISLAP_AUTUMN_RED    0xB00800
#define KISLAP_AUTUMN_ORANGE 0xFF3800
#define KISLAP_AUTUMN_YELLOW 0xFFA020
#define KISLAP_AUTUMN_BROWN  0x3A1404
// Color a leaf's back side is lightened toward (leaf undersides are paler).
#define KISLAP_AUTUMN_UNDERSIDE 0xFFC060
// Dim backdrop showing through the gaps between leaves. The color is fixed; its
// level (0-255, 0 = gaps are black) is KISLAP_AUTUMN_BACKGROUND.
#define KISLAP_AUTUMN_BACKGROUND_COLOR 0x401204
#define KISLAP_AUTUMN_BACKGROUND 60

// Leaf width range (pixels) at the leaf's widest point. Each leaf picks a random
// width in this range.
#define KISLAP_AUTUMN_LEAF_MIN_SIZE 5
#define KISLAP_AUTUMN_LEAF_MAX_SIZE 16

// How many new leaves enter the slit per frame, on average, in calm air
// (fractions allowed: 0.5 = one leaf every other frame). More = denser leaves.
#define KISLAP_AUTUMN_LEAF_RATE 0.8f

// How long one leaf takes to cross the slit, in rendered frames, in calm air.
// Each leaf picks a random time in this range. Smaller = faster-falling leaves.
#define KISLAP_AUTUMN_FALL_MIN_FRAMES 20
#define KISLAP_AUTUMN_FALL_MAX_FRAMES 50

// How much a full-strength gust ramps things up: leaf rate, crossing speed and
// tumble speed are all multiplied by up to (1 + this) at a gust's peak. Each
// gust picks a random strength, so most peak somewhat below this. 0 = no gusts.
#define KISLAP_AUTUMN_GUST_STRENGTH 2.0f

// Gap between gusts, in rendered frames (same clock as the ripples). A new gust
// starts at a random interval in this range.
#define KISLAP_AUTUMN_GUST_MIN_INTERVAL_FRAMES 100
#define KISLAP_AUTUMN_GUST_MAX_INTERVAL_FRAMES 300

// Flutter depth (0-255): how much a leaf dims at the bottom of its flicker
// during a full gust. Calm air flutters at about a third of this. 0 disables.
#define KISLAP_AUTUMN_FLUTTER 120

// Leaf pool capacity: the most leaves that can be in the slit at once. Sizes an
// array, so compile-time only.
#define KISLAP_AUTUMN_MAX_LEAVES 128

// --- Zones -------------------------------------------------------------
// The strip runs around several sides of the house; each side is a "zone"
// that starts with a blue/star intro and ends with red/white stripes. Zone
// boundaries fall at the physical corners of the house. Colors never cross a
// zone boundary -- each zone's blue/star and red/white sections are confined
// to that zone -- but the ripple/wave motion is a single continuous effect
// that runs the length of the whole strip, unaffected by zone edges.
//
// Zones are listed in physical strip order. KISLAP_ZONE_LENGTHS gives each
// zone's total pixel count; KISLAP_ZONE_INTRO_LENGTHS gives how many of those
// pixels (from the start of the zone) are the blue/star intro, with the rest
// of the zone being red/white stripes. The lengths must sum to KISLAP_NUM_LEDS.
//
// Zone 1's intro also doubles as the origin seam that ripples spawn from (see
// KISLAP_MAX_RIPPLES below); because zone 1 is wired back on itself at the
// first corner, the near-going and far-going halves of a ripple already trace
// the same physical direction along the house without any extra reversal.
#define KISLAP_NUM_ZONES 4
#define KISLAP_ZONE_LENGTHS { 639, 233, 391, 258 }
#define KISLAP_ZONE_INTRO_LENGTHS { 110, 60, 100, 60 }

// --- Intro section ---------------------------------------------------------
// Within each zone's blue/star intro, every KISLAP_INTRO_STAR_EVERY-th pixel
// (counted from the start of the zone) is a white "star" that twinkles.
#define KISLAP_INTRO_STAR_EVERY 4

// Dim end of a star's twinkle as a blend from background blue (0) toward white
// (255). At 0 the star melts fully into the background field at the bottom of
// its pulse; raise it so stars stay partly lit and only pulse toward white.
#define KISLAP_INTRO_STAR_MIN 0

// Base pattern: alternating red and white stripes. Each stripe is this many
// LEDs wide (15 LEDs = 5 pixels at 3 LEDs per pixel). Hard stripe edges make
// the ripple's displacement far easier to see than a gradual rainbow fade.
#define KISLAP_STRIPE_WIDTH 20

// Delay between rendered frames (ms). Smaller = smoother ripple motion.
#define KISLAP_FRAME_DELAY_MS 0

// --- Ripple effect ---------------------------------------------------------
// The rainbow sits still. A "ripple" is a localized bump of wind that travels
// down the strand: as it passes a pixel it nudges that pixel's color out of
// place, then the pixel settles back while the bump keeps moving.

// How fast a ripple travels, in pixels per frame.
#define KISLAP_RIPPLE_SPEED 2.0f

// Spatial spread of a ripple (pixels). Larger = a wider, gentler swell.
#define KISLAP_RIPPLE_WIDTH 16.0f

// How much wider the trailing (return) side of a gust is than the leading
// (push) side. A single-signed bump naturally compresses behind the crest,
// making the return look faster than the push; widening the trailing side
// stretches the return back out. 1.0 = symmetric, >1 = gentler/slower return.
#define KISLAP_RIPPLE_RETURN_RATIO 2.0f

// Peak displacement range (pixels). Each ripple picks a random strength in
// this range, so some are gentle nudges and some are strong gusts.
#define KISLAP_RIPPLE_MIN_AMP 4.0f
#define KISLAP_RIPPLE_MAX_AMP 10.0f

// Gap between ripples, measured in rendered frames (not milliseconds). A new
// ripple launches at a random interval in this range. Frame-based so the spawn
// cadence rides the same clock as ripple travel and the star twinkle: adding
// frame delay slows the whole scene uniformly instead of letting gusts pile up.
#define KISLAP_RIPPLE_MIN_INTERVAL_FRAMES 150
#define KISLAP_RIPPLE_MAX_INTERVAL_FRAMES 400

// Front/back shading: the leading edge of a gust brightens (a lit crest) and
// the trailing edge darkens, giving the wave a sense of depth and direction.
// Larger = more pronounced light/dark contrast; 0 disables shading.
#define KISLAP_RIPPLE_SHADE 0.16f

// Nominal brightness of the resting pattern (0-1), i.e. the level with no gust
// over a pixel. Below 1.0 so a crest has headroom to brighten toward full;
// lower = more highlight range but a dimmer resting strip.
#define KISLAP_RIPPLE_BASE_LEVEL 0.78f

// Ripple pool capacity: the size of the fixed ripples[] array and the hard
// ceiling for how many can ever be in flight at once. This is compile-time
// because it sizes an array. The runtime Config.maxRipples caps active ripples
// at or below this value and can be changed live over the REST API.
#define KISLAP_MAX_RIPPLES 75

// Serial baud rate for debugging.
#define KISLAP_SERIAL_BAUD 115200

// WiFi credentials and OTA password live in secrets.h (gitignored).
// Copy secrets.example.h to secrets.h and fill in your own values.
#include "secrets.h"
