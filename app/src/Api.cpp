#include "shunt/app/Api.h"
#include <string>

extern const unsigned char kIndexHtml[];
extern const size_t kIndexHtml_size;
extern const unsigned char kOverlayHtml[];
extern const size_t kOverlayHtml_size;

namespace shunt::app {

namespace {
HttpResponse json(const Json& j, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.body = j.dump();
    return r;
}
HttpResponse error(int status, const std::string& msg) {
    Json j = Json::object();
    j["ok"] = false;
    j["error"] = msg;
    return json(j, status);
}
HttpResponse ok() {
    Json j = Json::object();
    j["ok"] = true;
    return json(j);
}
HttpResponse html(const unsigned char* data, size_t n) {
    HttpResponse r;
    r.contentType = "text/html; charset=utf-8";
    r.body.assign(reinterpret_cast<const char*>(data), n);
    r.headers.push_back({"Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self' ws: wss:; img-src 'self' data:"});
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    return r;
}
}

HttpResponse handleRequest(Core& core, const HttpRequest& req) {
    const std::string& p = req.path;
    const bool post = req.method == "POST";
    if (req.method != "GET" && req.method != "HEAD" && !post) return error(405, "method not allowed");

    if (!post) {
        if (p == "/" || p == "/index.html") return html(kIndexHtml, kIndexHtml_size);
        if (p == "/favicon.ico") { HttpResponse r; r.status = 200; r.contentType = "image/svg+xml"; r.body = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><circle cx='16' cy='16' r='14' fill='#07090c' stroke='#2ee6c4' stroke-width='2.4'/><path d='M6 16h6l3-7 3 14 3-7h5' fill='none' stroke='#2ee6c4' stroke-width='2.4'/></svg>"; return r; }
        if (p == "/overlay") return html(kOverlayHtml, kOverlayHtml_size);
        if (p == "/api/status") return json(core.status());
        if (p == "/api/config") return json(core.config());
        if (p == "/api/interfaces") return json(core.interfaces());
        if (p == "/api/midi-ports") return json(core.midiPorts());
        if (p == "/api/tracklist") return json(core.tracklist());
        if (p == "/api/compat") return json(core.compat());
        if (p == "/api/export") {
            HttpResponse r;
            std::string name;
            if (!core.exportTracklist(req.queryParam("format"), r.body, r.contentType, name)) return error(400, "unknown format");
            r.contentType += "; charset=utf-8";
            r.headers.push_back({"Content-Disposition", "attachment; filename=\"" + name + "\""});
            return r;
        }
        if (p.rfind("/api/", 0) == 0) return error(404, "not found");
        return error(404, "not found");
    }

    if (p == "/api/rekordbox-xml") {
        const int n = core.importRekordboxXml(req.body);
        if (n < 0) return error(400, "no tracks with TrackID and Name found: is this a rekordbox XML export?");
        Json j = Json::object();
        j["ok"] = true;
        j["tracks"] = n;
        return json(j);
    }
    Json body;
    if (!req.body.empty()) {
        std::string err;
        if (!Json::parse(req.body, body, &err)) return error(400, "invalid JSON: " + err);
    }
    if (p == "/api/config") {
        std::string err;
        if (!core.applyConfig(body, err)) return error(400, err);
        return json(core.config());
    }
    if (p == "/api/baroffset") {
        if (body.get("cycle").asBool()) core.cycleBarOffset();
        else if (body.get("value").type() == Json::Number) core.setBarOffset(body.get("value").asInt());
        else return error(400, "need value or cycle");
        return ok();
    }
    if (p == "/api/beatonly") { core.setBeatOnly(body.get("value").asBool()); return ok(); }
    if (p == "/api/latency") {
        if (body.get("fromScope").asBool()) { if (!core.latencyFromScope()) return error(400, "not enough scope data yet"); }
        else if (body.get("ms").type() == Json::Number) core.setLatencyMs(body.get("ms").asNumber());
        else return error(400, "need ms or fromScope");
        return ok();
    }
    if (p == "/api/setstart") { core.markSetStart(); return ok(); }
    if (p == "/api/session/clear") { core.clearSession(); return ok(); }
    if (p == "/api/record") { core.setRecording(body.get("on").asBool()); return ok(); }
    return error(404, "not found");
}

} // namespace shunt::app
