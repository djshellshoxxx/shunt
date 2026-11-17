// dbserver client (RS-04 metadata path 1; research 01 sections 1 and 6).
#include "shunt/meta/Metadata.h"
#include "DbProtocol.h"
#include "Tcp.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <tuple>

namespace shunt::meta {

namespace {
using namespace db;

constexpr size_t kCacheLimit = 8192;
constexpr uint32_t kMaxMenuItems = 64;   // the metadata menu holds roughly a dozen rows

struct Job {
    uint32_t ip;
    uint8_t player, slot, trackType;
    uint32_t id;
    DbServerClient::Callback cb;
};

using CacheKey = std::tuple<uint32_t, uint8_t, uint32_t>;

class SocketSource : public ByteSource {
public:
    SocketSource(detail::TcpConn& c, int timeoutMs, const std::atomic<bool>* stop)
        : conn_(c), timeoutMs_(timeoutMs), stop_(stop) {}
    bool read(uint8_t* out, size_t n) override { return conn_.recvAll(out, n, timeoutMs_, stop_); }
private:
    detail::TcpConn& conn_;
    int timeoutMs_;
    const std::atomic<bool>* stop_;
};

void applyItem(const Message& item, TrackMetadata& md) {
    const auto& a = item.args;
    if (a.size() <= BenchTunables::kItemTypeArg) return;
    const Field& typeF = a[BenchTunables::kItemTypeArg];
    const Field& numF = a[BenchTunables::kItemNumericArg];
    const Field& labelF = a[BenchTunables::kItemLabelArg];
    if (!typeF.isNumber()) return;
    const uint8_t code = uint8_t(typeF.number & 0xff);
    const std::string label = labelF.tag == kTagString ? labelF.text : std::string();
    const uint32_t num = numF.isNumber() ? numF.number : 0;
    for (const auto& t : kItemTypeCodes) {
        if (t.code != code) continue;
        switch (t.field) {
        case ItemField::Title: md.title = label; break;
        case ItemField::Artist: md.artist = label; break;
        case ItemField::Album: md.album = label; break;
        case ItemField::Genre: md.genre = label; break;
        case ItemField::Key: md.key = label; break;
        case ItemField::Label: md.label = label; break;
        case ItemField::Bpm: md.bpm = num / 100.0; break;
        case ItemField::Duration: md.durationS = double(num); break;
        }
        return;
    }
}
} // namespace

struct DbServerClient::Impl {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> queue;
    std::thread worker;
    bool running = false;
    std::atomic<bool> stopping{false};
    std::atomic<uint8_t> ownNumber{0};
    std::atomic<uint16_t> discoveryPort{kDiscoveryPort};
    std::atomic<int> timeoutMs{10000};
    std::map<CacheKey, TrackMetadata> cache;   // worker thread only
    std::deque<CacheKey> cacheOrder;
    uint32_t nextTransaction = 1;

    void run();
    bool lookup(const Job& job, uint8_t own, TrackMetadata& out);
    bool exchange(detail::TcpConn& c, const Message& m, Message& reply, int to);
    void remember(const CacheKey& k, const TrackMetadata& md) {
        if (cache.emplace(k, md).second) {
            cacheOrder.push_back(k);
            if (cacheOrder.size() > kCacheLimit) { cache.erase(cacheOrder.front()); cacheOrder.pop_front(); }
        }
    }
};

DbServerClient::DbServerClient() : impl_(std::make_unique<Impl>()) {}
DbServerClient::~DbServerClient() { stop(); }

void DbServerClient::setOwnNumber(uint8_t number) { impl_->ownNumber = number; }
void DbServerClient::setDiscoveryPort(uint16_t port) { impl_->discoveryPort = port; }
void DbServerClient::setTimeoutMs(int ms) { impl_->timeoutMs = ms > 0 ? ms : 1; }

void DbServerClient::request(uint32_t playerIp, uint8_t player, uint8_t slot, uint8_t trackType,
                             uint32_t rekordboxId, Callback cb) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->queue.push_back({playerIp, player, slot, trackType, rekordboxId, std::move(cb)});
    if (!impl_->running) {
        impl_->running = true;
        impl_->stopping = false;
        impl_->worker = std::thread([this] { impl_->run(); });
    }
    impl_->cv.notify_one();
}

void DbServerClient::stop() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->stopping = true;
        t = std::move(impl_->worker);
        impl_->cv.notify_all();
    }
    if (t.joinable()) t.join();
    std::deque<Job> left;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->running = false;
        left.swap(impl_->queue);
    }
    for (auto& j : left) if (j.cb) j.cb(false, TrackMetadata{});
}

void DbServerClient::Impl::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [&] { return stopping.load() || !queue.empty(); });
            if (queue.empty()) return;
            job = std::move(queue.front());
            queue.pop_front();
        }
        TrackMetadata md;
        bool ok = false;
        if (!stopping) {
            const uint8_t own = ownNumber;
            const CacheKey key{job.ip, job.slot, job.id};
            auto it = cache.find(key);
            if (it != cache.end()) { md = it->second; ok = true; }
            else if (own >= 1 && own <= 6 && job.player != own && job.id != 0) {
                ok = lookup(job, own, md);
                if (ok) remember(key, md);
            }
        }
        if (!ok) md = TrackMetadata{};
        if (job.cb) job.cb(ok, md);
    }
}

bool DbServerClient::Impl::exchange(detail::TcpConn& c, const Message& m, Message& reply, int to) {
    const auto bytes = encodeMessage(m);
    if (!c.sendAll(bytes.data(), bytes.size(), to, &stopping)) return false;
    SocketSource src(c, to, &stopping);
    return decodeMessage(src, reply) == DecodeError::None && reply.transaction == m.transaction;
}

bool DbServerClient::Impl::lookup(const Job& job, uint8_t own, TrackMetadata& out) {
    const int to = timeoutMs;
    detail::TcpConn c;

    // Port discovery: 19-byte query to TCP 12523, 2-byte big-endian port back.
    uint16_t port = 0;
    {
        static const char kName[] = "RemoteDBServer";
        uint8_t q[19] = {0, 0, 0, 0x0f};
        for (size_t i = 0; i < 14; ++i) q[4 + i] = uint8_t(kName[i]);
        q[18] = 0;
        uint8_t p[2];
        if (!c.connect(job.ip, discoveryPort, to, &stopping) || !c.sendAll(q, sizeof q, to, &stopping) ||
            !c.recvAll(p, 2, to, &stopping))
            return false;
        c.close();
        port = uint16_t(p[0] << 8 | p[1]);
        if (port == 0) return false;
    }
    if (!c.connect(job.ip, port, to, &stopping)) return false;
    SocketSource src(c, to, &stopping);

    if (BenchTunables::kSendPreamble) {
        std::vector<uint8_t> pre;
        encodeField(Field::u32(BenchTunables::kPreambleValue), pre);
        Field echo;
        if (!c.sendAll(pre.data(), pre.size(), to, &stopping) || decodeField(src, echo) != DecodeError::None ||
            !echo.isNumber())
            return false;
    }

    Message reply;
    if (!exchange(c, {kSetupTransaction, kMsgGreeting, {Field::u32(own)}}, reply, to) || reply.type != kMsgSuccess)
        return false;

    const uint32_t dmst = uint32_t(own) << 24 | uint32_t(kMenuMain) << 16 | uint32_t(job.slot) << 8 | job.trackType;
    const uint32_t tx = nextTransaction++;
    if (nextTransaction >= kSetupTransaction) nextTransaction = 1;
    bool ok = exchange(c, {tx, kMsgMetadataRequest, {Field::u32(dmst), Field::u32(job.id)}}, reply, to) &&
              reply.type == kMsgSuccess && reply.args.size() > BenchTunables::kAvailableCountArg &&
              reply.args[BenchTunables::kAvailableCountArg].isNumber();
    uint32_t available = ok ? reply.args[BenchTunables::kAvailableCountArg].number : 0;
    if (available == 0 || available == 0xffffffffu) ok = false;
    if (available > kMaxMenuItems) available = kMaxMenuItems;

    if (ok) {
        const uint32_t rtx = nextTransaction++;
        if (nextTransaction >= kSetupTransaction) nextTransaction = 1;
        const Message render{rtx, kMsgRenderRequest,
            {Field::u32(dmst), Field::u32(0), Field::u32(available), Field::u32(0), Field::u32(available), Field::u32(0)}};
        const auto bytes = encodeMessage(render);
        ok = c.sendAll(bytes.data(), bytes.size(), to, &stopping);
        TrackMetadata md;
        size_t items = 0;
        bool footer = false;
        for (uint32_t n = 0; ok && n < available + 2; ++n) {
            Message m;
            if (decodeMessage(src, m) != DecodeError::None || m.transaction != rtx) { ok = false; break; }
            if (m.type == kMsgMenuHeader) continue;
            if (m.type == kMsgMenuFooter) { footer = true; break; }
            if (m.type != kMsgMenuItem) { ok = false; break; }
            applyItem(m, md);
            ++items;
        }
        ok = ok && footer && items > 0;
        if (ok) {
            md.rekordboxId = job.id;
            md.source = "dbserver";
            out = std::move(md);
        }
    }

    // Teardown is best effort; the result is already decided.
    if (c.isOpen() && !stopping) {
        const auto bye = encodeMessage({kSetupTransaction, kMsgTeardown, {}});
        c.sendAll(bye.data(), bye.size(), to, &stopping);
    }
    return ok;
}

} // namespace shunt::meta
