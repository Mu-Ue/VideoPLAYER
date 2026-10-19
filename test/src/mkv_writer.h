#pragma once

// Writes Matroska files byte by byte, laid out as the tests ask, to drive every path of the player's reader: with
// an index of all blocks, of the video's only, or none; elements of unknown size; CRC-32s; header stripping.

#include <windows.h>

#include <string>
#include <vector>

namespace mkv {

enum : UINT32 {
    kEbml = 0x1A45DFA3, kDocType = 0x4282, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB,
    kSeekId = 0x53AB, kSeekPosition = 0x53AC, kInfo = 0x1549A966, kTimestampScale = 0x2AD7B1, kTracks = 0x1654AE6B,
    kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kCodecId = 0x86, kLanguage = 0x22B59C,
    kLanguageBcp47 = 0x22B59D, kName = 0x536E, kDefaultDuration = 0x23E383, kCodecDelay = 0x56AA,
    kContentEncodings = 0x6D80, kContentEncoding = 0x6240, kContentEncodingType = 0x5033,
    kContentCompression = 0x5034, kContentCompAlgo = 0x4254, kContentCompSettings = 0x4255, kCues = 0x1C53BB6B,
    kCuePoint = 0xBB, kCueTime = 0xB3, kCueTrackPositions = 0xB7, kCueTrack = 0xF7, kCueClusterPosition = 0xF1,
    kCueRelativePosition = 0xF0, kCueDuration = 0xB2, kCluster = 0x1F43B675, kTimestamp = 0xE7, kBlockGroup = 0xA0,
    kBlock = 0xA1, kSimpleBlock = 0xA3, kBlockDuration = 0x9B, kCrc32 = 0xBF, kVoid = 0xEC,
};

inline std::string Be(UINT64 v, int bytes) {
    std::string s(size_t(bytes), '\0');
    for (int i = bytes - 1; i >= 0; --i, v >>= 8) s[size_t(i)] = char(v & 0xFF);
    return s;
}

inline std::string Id(UINT32 id) { return Be(id, id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1); }

// A size as a variable-length integer of `length` bytes, or the fewest that hold it.
inline std::string Size(UINT64 n, int length = 0) {
    if (!length) {
        length = 1;
        while (length < 8 && n >= (UINT64(1) << (7 * length)) - 1) ++length;
    }
    return Be(n | (UINT64(1) << (7 * length)), length);
}
inline std::string UnknownSize() { return std::string("\x01", 1) + std::string(7, '\xFF'); }

inline std::string El(UINT32 id, const std::string& data) { return Id(id) + Size(data.size()) + data; }
inline std::string UInt(UINT32 id, UINT64 v, int bytes = 0) {
    if (!bytes) {
        bytes = 1;
        while (bytes < 8 && (v >> (8 * bytes))) ++bytes;
    }
    return El(id, Be(v, bytes));
}

struct Track {
    UINT64 number = 1;
    UINT64 type = 0x11;  // 1 video, 2 audio, 0x11 subtitles
    std::string codec = "S_TEXT/UTF8";
    std::string language, bcp47, name;  // empty: not written
    UINT64 defaultDurationNs = 0, codecDelayNs = 0;
    std::string strip;          // header stripping: bytes taken off the front of each block
    bool compressed = false;    // zlib, which the reader can't undo
    bool encrypted = false;
};

struct Block {
    UINT64 track = 1;
    INT64 ms = 0;               // its time
    std::string data;
    INT64 durationMs = -1;      // in a BlockGroup with a BlockDuration, if not negative
    bool group = false;         // in a BlockGroup even without a duration
    bool laced = false;
    bool key = true;
};

struct File {
    std::vector<Track> tracks;
    std::vector<Block> blocks;  // in time order
    UINT64 timestampScale = 1000000;  // ns a timestamp unit: 1 ms
    INT64 clusterMs = 1000;           // a cluster starts each time this many ms have passed
    enum { kNoIndex, kVideoIndex, kFullIndex } index = kFullIndex;
    bool cueDurations = false, cueRelative = true;
    bool cuesFirst = false;  // the index before the clusters, else after them
    bool seekHead = true;    // else Info, Tracks and Cues are found walking the segment
    bool unknownSegment = false, unknownClusters = false, crc = false;
    std::string docType = "matroska";
};

inline UINT64 Units(const File& f, INT64 ms) { return UINT64(ms * 1000000 / INT64(f.timestampScale)); }

inline std::string TrackEntry(const Track& t) {
    std::string e = UInt(kTrackNumber, t.number) + UInt(kTrackType, t.type) + El(kCodecId, t.codec);
    if (!t.language.empty()) e += El(kLanguage, t.language);
    if (!t.bcp47.empty()) e += El(kLanguageBcp47, t.bcp47);
    if (!t.name.empty()) e += El(kName, t.name);
    if (t.defaultDurationNs) e += UInt(kDefaultDuration, t.defaultDurationNs);
    if (t.codecDelayNs) e += UInt(kCodecDelay, t.codecDelayNs);
    if (!t.strip.empty() || t.compressed || t.encrypted) {
        std::string encoding = UInt(kContentEncodingType, t.encrypted ? 1 : 0);
        if (!t.encrypted) {
            encoding += El(kContentCompression, UInt(kContentCompAlgo, t.compressed ? 0 : 3) +
                                                     (t.compressed ? "" : El(kContentCompSettings, t.strip)));
        }
        e += El(kContentEncodings, El(kContentEncoding, encoding));
    }
    return El(kTrackEntry, e);
}

// The file, with the index listing where each block really is.
inline std::string Build(const File& f) {
    std::string header = El(kEbml, UInt(0x4286, 1) + UInt(0x42F7, 1) + UInt(0x42F2, 4) + UInt(0x42F3, 8) +
                                       El(kDocType, f.docType) + UInt(0x4287, 4) + UInt(0x4285, 2));
    const std::string info = El(kInfo, UInt(kTimestampScale, f.timestampScale) + El(0x4D80, "tests"));
    std::string entries;
    for (const Track& t : f.tracks) entries += TrackEntry(t);
    const std::string tracks = El(kTracks, entries);
    const auto isText = [&](UINT64 n) {
        for (const Track& t : f.tracks) {
            if (t.number == n) return t.type == 0x11;
        }
        return false;
    };
    const auto isVideo = [&](UINT64 n) {
        for (const Track& t : f.tracks) {
            if (t.number == n) return t.type == 1;
        }
        return false;
    };

    // Clusters, with where each block lands; positions are from the segment's data.
    struct Placed {
        const Block* block;
        UINT64 cluster, relative;
    };
    const auto clusters = [&](UINT64 at, std::vector<Placed>& placed) {
        std::string out;
        placed.clear();
        for (size_t i = 0; i < f.blocks.size();) {
            const INT64 start = f.blocks[i].ms / f.clusterMs * f.clusterMs;
            std::string body = f.crc ? El(kCrc32, std::string(4, '\x5A')) : "";
            body += UInt(kTimestamp, Units(f, start));
            std::vector<std::pair<const Block*, UINT64>> inCluster;
            for (; i < f.blocks.size() && f.blocks[i].ms / f.clusterMs * f.clusterMs == start; ++i) {
                const Block& b = f.blocks[i];
                const INT64 rel = INT64(Units(f, b.ms)) - INT64(Units(f, start));
                const std::string head = Size(b.track) + Be(UINT64(UINT16(INT16(rel))), 2);
                inCluster.push_back({&b, body.size()});
                if (b.durationMs >= 0 || b.group) {
                    std::string group = El(kBlock, head + char(b.laced ? 0x02 : 0) + b.data);
                    if (b.durationMs >= 0) group += UInt(kBlockDuration, Units(f, b.durationMs));
                    body += El(kBlockGroup, group);
                } else {
                    body += El(kSimpleBlock, head + char((b.key ? 0x80 : 0) | (b.laced ? 0x02 : 0)) + b.data);
                }
            }
            const std::string clusterHead = Id(kCluster) + (f.unknownClusters ? UnknownSize() : Size(body.size(), 8));
            for (const auto& [b, rel] : inCluster) placed.push_back({b, at + out.size(), rel});
            out += clusterHead + body;
        }
        return out;
    };
    // The index; its size doesn't depend on the positions, which are written 8 bytes long.
    const auto cues = [&](const std::vector<Placed>& placed) {
        std::string points;
        for (const Placed& p : placed) {
            const Block& b = *p.block;
            const bool listed = f.index == File::kFullIndex ? isText(b.track) || (isVideo(b.track) && b.key)
                                : f.index == File::kVideoIndex ? isVideo(b.track) && b.key
                                                                : false;
            if (!listed) continue;
            std::string positions = UInt(kCueTrack, b.track) + UInt(kCueClusterPosition, p.cluster, 8);
            if (f.cueRelative) positions += UInt(kCueRelativePosition, p.relative, 8);
            if (f.cueDurations && b.durationMs >= 0) positions += UInt(kCueDuration, Units(f, b.durationMs), 8);
            points += El(kCuePoint, UInt(kCueTime, Units(f, b.ms), 8) + El(kCueTrackPositions, positions));
        }
        return f.index == File::kNoIndex ? std::string() : El(kCues, points);
    };
    const auto seekHead = [&](UINT64 infoAt, UINT64 tracksAt, UINT64 cuesAt, bool withCues) {
        std::string seeks = El(kSeek, El(kSeekId, Id(kInfo)) + UInt(kSeekPosition, infoAt, 8)) +
                            El(kSeek, El(kSeekId, Id(kTracks)) + UInt(kSeekPosition, tracksAt, 8));
        if (withCues) seeks += El(kSeek, El(kSeekId, Id(kCues)) + UInt(kSeekPosition, cuesAt, 8));
        return El(kSeekHead, seeks);
    };

    std::vector<Placed> placed;
    const bool hasCues = f.index != File::kNoIndex;
    const size_t seekSize = f.seekHead ? seekHead(0, 0, 0, hasCues).size() : 0;
    const UINT64 infoAt = seekSize, tracksAt = infoAt + info.size(), afterTracks = tracksAt + tracks.size();
    std::string body;
    if (f.cuesFirst && hasCues) {
        clusters(0, placed);
        const size_t cuesSize = cues(placed).size();
        const std::string cl = clusters(afterTracks + cuesSize, placed);
        body = (f.seekHead ? seekHead(infoAt, tracksAt, afterTracks, true) : "") + info + tracks + cues(placed) + cl;
    } else {
        const std::string cl = clusters(afterTracks, placed);
        body = (f.seekHead ? seekHead(infoAt, tracksAt, afterTracks + cl.size(), hasCues) : "") + info + tracks + cl +
               cues(placed);
    }
    return header + Id(kSegment) + (f.unknownSegment ? UnknownSize() : Size(body.size(), 8)) + body;
}

}  // namespace mkv
