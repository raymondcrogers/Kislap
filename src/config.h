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
