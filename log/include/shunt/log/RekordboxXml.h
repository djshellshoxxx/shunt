// rekordbox XML export import (RS-07 addendum): id -> metadata.
#pragma once
#include "shunt/log/Tracklist.h"
#include <map>
#include <string>

namespace shunt::log {

struct TrackMeta {
    std::string title, artist, album, genre, key;
    double bpm = 0, durationS = 0;
};
using MetaMap = std::map<uint32_t, TrackMeta>;

MetaMap parseRekordboxXml(const std::string& xml);
// Fills title/artist/... on rows whose id is in the map and marks source "rekordbox-xml".
void applyMeta(std::vector<Row>& rows, const MetaMap& meta);

} // namespace shunt::log
