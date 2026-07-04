#pragma once

// Minimal REST API for live configuration:
//   GET  /api/config  -> current config as JSON
//   POST /api/config  -> partial JSON update; persists and echoes new config
//   GET  /api/status  -> live telemetry: measured fps, uptimeMs, numLeds
//
// Uses the arduino-esp32 built-in synchronous WebServer, so setupWebApi() must
// be called once from setup() (after WiFi is connected) and handleWebApi()
// pumped from loop() alongside ArduinoOTA.handle().
void setupWebApi();
void handleWebApi();
