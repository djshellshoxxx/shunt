// FS-00 section 2 tests: Json::merge deep merge (6 cases) and Settings::fromJson using it.
// Original Shunt code.
#include "TestFramework.h"
#include "shunt/app/Settings.h"
#include <unistd.h>

using namespace shunt::app;

namespace {
Json parse(const char* s) { Json j; Json::parse(s, j); return j; }
std::string merged(const char* base, const char* patch) { Json b = parse(base); b.merge(parse(patch)); return b.dump(); }
}

TEST_CASE("FS00-T1 deep merge: objects merge, null deletes, arrays and scalars replace") {
    // 1. nested objects merge key by key, siblings survive
    CHECK(merged("{\"features\":{\"a\":{\"x\":1},\"b\":{\"y\":2}}}", "{\"features\":{\"a\":{\"z\":3}}}") ==
          "{\"features\":{\"a\":{\"x\":1,\"z\":3},\"b\":{\"y\":2}}}");
    // 2. null deletes a key (also nested); deleting a missing key is a no-op
    CHECK(merged("{\"a\":1,\"b\":{\"c\":2,\"d\":3}}", "{\"a\":null,\"b\":{\"d\":null},\"zz\":null}") == "{\"b\":{\"c\":2}}");
    // 3. arrays replace, never concatenate
    CHECK(merged("{\"a\":[1,2,3]}", "{\"a\":[4]}") == "{\"a\":[4]}");
    // 4. scalars replace objects and objects replace scalars (nulls inside a new object are dropped)
    CHECK(merged("{\"a\":{\"b\":1}}", "{\"a\":5}") == "{\"a\":5}");
    CHECK(merged("{\"a\":5}", "{\"a\":{\"b\":1,\"c\":null}}") == "{\"a\":{\"b\":1}}");
    // 5. a non-object patch replaces the whole value; merging into a non-object starts from {}
    CHECK(merged("{\"a\":1}", "[1,2]") == "[1,2]");
    CHECK(merged("7", "{\"a\":1}") == "{\"a\":1}");
    // 6. empty patch changes nothing and key order is kept (unknown keys survive a save)
    CHECK(merged("{\"z\":1,\"a\":{\"m\":2}}", "{}") == "{\"z\":1,\"a\":{\"m\":2}}");
}

TEST_CASE("FS00-T1b settings patch for one feature keeps the others and persists") {
    Settings s = Settings::fromJson(parse("{\"features\":{\"a\":{\"enabled\":true,\"n\":1},\"b\":{\"enabled\":false}},\"latencyMs\":2}"));
    Settings t = Settings::fromJson(parse("{\"features\":{\"a\":{}}}"), s);
    CHECK(t.raw.get("features").get("b").has("enabled"));
    CHECK_EQ(t.raw.get("features").get("a").get("n").asInt(), 1);
    t = Settings::fromJson(parse("{\"features\":{\"a\":{\"n\":null}}}"), t);
    CHECK(!t.raw.get("features").get("a").has("n"));
    CHECK(t.raw.get("features").get("a").get("enabled").asBool());
    CHECK_NEAR(t.latencyMs, 2.0, 1e-9);
    const std::string path = "/tmp/shunt_fs00_merge_" + std::to_string(::getpid()) + "/config.json";
    REQUIRE(t.save(path));
    Settings u = Settings::load(path);
    CHECK(u.toJson().get("features").get("b").has("enabled"));
    CHECK(u.toJson().get("features").get("a").get("enabled").asBool());
    ::unlink(path.c_str());
}
