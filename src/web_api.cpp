#include "web_api.h"

#include <WebServer.h>
#include <ArduinoJson.h>
#include "runtime_config.h"

static WebServer server(80);

// Live render rate, measured and updated in the main loop (main.cpp).
extern float measuredFps;

// GET /api/config -- serialize and return the full current config.
static void handleGetConfig() {
    JsonDocument doc;
    configToJson(doc);
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// POST /api/config -- parse a (partial) JSON body, apply + persist it, then
// echo the resulting full config so the client sees exactly what took effect
// (including any clamping).
static void handlePostConfig() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json",
                    "{\"error\":\"empty body; expected JSON object\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError parseErr = deserializeJson(doc, server.arg("plain"));
    if (parseErr) {
        JsonDocument e;
        e["error"] = "invalid json";
        e["detail"] = parseErr.c_str();
        String out;
        serializeJson(e, out);
        server.send(400, "application/json", out);
        return;
    }

    String applyErr;
    if (!configFromJson(doc, applyErr)) {
        JsonDocument e;
        e["error"] = applyErr;
        String out;
        serializeJson(e, out);
        server.send(400, "application/json", out);
        return;
    }

    saveConfig();
    handleGetConfig();  // echo the persisted state (with clamps applied)
}

// GET /api/status -- live runtime telemetry (measured FPS, uptime, LED count).
static void handleGetStatus() {
    JsonDocument doc;
    doc["fps"] = measuredFps;
    doc["uptimeMs"] = millis();
    doc["numLeds"] = KISLAP_NUM_LEDS;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleNotFound() {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
}

void setupWebApi() {
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("REST API ready on http://<device-ip>/api/config");
    Serial.println("Status/FPS at http://<device-ip>/api/status");
}

void handleWebApi() {
    server.handleClient();
}
