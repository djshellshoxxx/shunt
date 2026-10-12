// dbserver message codec (research 01 section 6). Every value on the wire is a
// tagged field; a message is a fixed run of header fields followed by its arguments.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shunt::meta::db {

constexpr uint32_t kMagic = 0x872349ae;
constexpr uint32_t kSetupTransaction = 0xfffffffe;
constexpr uint16_t kDiscoveryPort = 12523;
constexpr size_t kMaxArgs = 12;
constexpr uint32_t kMaxBlobBytes = 1u << 20;     // robustness bound, artwork blobs stay well below
constexpr uint32_t kMaxStringUnits = 1u << 15;   // robustness bound on UTF-16 units

enum FieldTag : uint8_t {
    kTagU8 = 0x0f,
    kTagU16 = 0x10,
    kTagU32 = 0x11,
    kTagBlob = 0x14,
    kTagString = 0x26,
};

enum MessageType : uint16_t {
    kMsgGreeting = 0x0000,
    kMsgTeardown = 0x0100,
    kMsgMetadataRequest = 0x2002,
    kMsgRenderRequest = 0x3000,
    kMsgSuccess = 0x4000,
    kMsgMenuHeader = 0x4001,
    kMsgMenuItem = 0x4101,
    kMsgMenuFooter = 0x4201,
};

constexpr uint8_t kMenuMain = 0x01;

// ---------------------------------------------------------------------------
// Values the research notes leave open. Kept together so the hardware bench
// test can correct them in one place.
struct BenchTunables {
    // Connection preamble: one u32 number field (value 1) sent before the
    // greeting; the player answers with a u32 number field.
    static constexpr bool kSendPreamble = true;
    static constexpr uint32_t kPreambleValue = 1;
    // Codes written into the 12-byte argument-type blob. The parser never
    // relies on them; fields are self-describing.
    static constexpr uint8_t kArgTypeNumber = 0x06;
    static constexpr uint8_t kArgTypeBlob = 0x03;
    static constexpr uint8_t kArgTypeString = 0x02;
    // Menu item (0x4101) argument positions.
    static constexpr size_t kItemNumericArg = 1;   // u32: bpm * 100, duration in s, ids
    static constexpr size_t kItemLabelArg = 3;     // UTF-16 string: primary text
    static constexpr size_t kItemTypeArg = 6;      // u32: item type code (low byte used)
    // 0x4000 reply argument holding the number of available menu items.
    static constexpr size_t kAvailableCountArg = 1;
};

// Menu item type code -> metadata field.
enum class ItemField { Title, Artist, Album, Genre, Key, Bpm, Duration, Label };
struct ItemTypeCode { uint8_t code; ItemField field; };
constexpr ItemTypeCode kItemTypeCodes[] = {
    {0x04, ItemField::Title},
    {0x07, ItemField::Artist},
    {0x02, ItemField::Album},
    {0x06, ItemField::Genre},
    {0x0f, ItemField::Key},
    {0x0d, ItemField::Bpm},
    {0x0b, ItemField::Duration},
    {0x0e, ItemField::Label},
};
// ---------------------------------------------------------------------------

struct Field {
    uint8_t tag = 0;
    uint32_t number = 0;           // numeric tags
    std::vector<uint8_t> blob;     // kTagBlob
    std::string text;              // kTagString, decoded to UTF-8 without the trailing NUL

    static Field u8(uint8_t v) { Field f; f.tag = kTagU8; f.number = v; return f; }
    static Field u16(uint16_t v) { Field f; f.tag = kTagU16; f.number = v; return f; }
    static Field u32(uint32_t v) { Field f; f.tag = kTagU32; f.number = v; return f; }
    static Field bytes(std::vector<uint8_t> b) { Field f; f.tag = kTagBlob; f.blob = std::move(b); return f; }
    static Field string(const std::string& utf8) { Field f; f.tag = kTagString; f.text = utf8; return f; }
    bool isNumber() const { return tag == kTagU8 || tag == kTagU16 || tag == kTagU32; }
};

struct Message {
    uint32_t transaction = 0;
    uint16_t type = 0;
    std::vector<Field> args;
};

void encodeField(const Field& f, std::vector<uint8_t>& out);
std::vector<uint8_t> encodeMessage(const Message& m);

// Byte source for the decoder: returns false when the bytes cannot be read.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual bool read(uint8_t* out, size_t n) = 0;
};

enum class DecodeError { None, Io, BadTag, TooLarge, BadMagic, BadHeader };
DecodeError decodeField(ByteSource& in, Field& out);
DecodeError decodeMessage(ByteSource& in, Message& out);

// Buffer-backed source (used by tests and for parsing captured bytes).
class BufferSource : public ByteSource {
public:
    BufferSource(const uint8_t* d, size_t n) : data_(d), size_(n) {}
    bool read(uint8_t* out, size_t n) override;
    size_t remaining() const { return size_ - pos_; }
private:
    const uint8_t* data_;
    size_t size_, pos_ = 0;
};

std::string utf16beToUtf8(const uint8_t* data, size_t units);
std::vector<uint8_t> utf8ToUtf16be(const std::string& s);   // includes trailing NUL

} // namespace shunt::meta::db
