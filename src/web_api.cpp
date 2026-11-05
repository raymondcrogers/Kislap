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

// Counts bytes without storing them, to size the schema response up front.
class CountingPrint : public Print {
public:
    size_t count = 0;
    size_t write(uint8_t) override { count++; return 1; }
    size_t write(const uint8_t*, size_t n) override { count += n; return n; }
};

// Buffers output and forwards it to the client in ~512-byte pieces, so the
// schema streams out without ever being held in RAM as one String. Also tracks
// the lowest free heap seen while sending, for the log line below.
class ClientPrint : public Print {
public:
    size_t minFreeHeap = SIZE_MAX;
    size_t write(uint8_t c) override {
        if (len_ == sizeof(buf_)) flush();
        buf_[len_++] = (char)c;
        return 1;
    }
    size_t write(const uint8_t* data, size_t n) override {
        for (size_t i = 0; i < n; i++) write(data[i]);
        return n;
    }
    void flush() override {
        const size_t freeNow = ESP.getFreeHeap();
        if (freeNow < minFreeHeap) minFreeHeap = freeNow;
        if (len_ > 0) server.sendContent(buf_, len_);
        len_ = 0;
    }
private:
    char buf_[512];
    size_t len_ = 0;
};

// The schema depends only on compile-time data, so its length never changes;
// measure it once.
static size_t schemaLength() {
    static size_t len = 0;
    if (len == 0) {
        CountingPrint counter;
        writeConfigSchema(counter);
        len = counter.count;
    }
    return len;
}

// GET /api/schema -- the self-describing settings schema, streamed with a known
// Content-Length (a few KB; see README.md).
static void handleGetSchema() {
    const size_t heapBefore = ESP.getFreeHeap();
    server.setContentLength(schemaLength());
    server.send(200, "application/json", "");
    ClientPrint out;
    writeConfigSchema(out);
    out.flush();
    log_i("/api/schema sent %u bytes; free heap %u before, %u lowest while sending",
          (unsigned)schemaLength(), (unsigned)heapBefore, (unsigned)out.minFreeHeap);
}

static void handleNotFound() {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
}

void setupWebApi() {
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.on("/api/schema", HTTP_GET, handleGetSchema);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("REST API ready on http://<device-ip>/api/config");
    Serial.println("Status/FPS at http://<device-ip>/api/status");
    Serial.printf("Settings schema at http://<device-ip>/api/schema (%u bytes)\n",
                  (unsigned)schemaLength());
}

void handleWebApi() {
    server.handleClient();
}
