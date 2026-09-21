/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * The outermost envelope: the blob a peer is handed before it knows anything
 * about the peer. Everything else -- which transport, which region, which
 * version of either -- is negotiated inside it, so if this layer is read
 * wrongly the negotiation happens against the wrong bytes. */
#include <vector>

#include "core/factory.h"
#include "hux/engine.h"
#include "test_main.h"
#include "transport/local/local_provider.h"

using namespace hux;

namespace {

/* Where the magic sits: after the identity, which comes first because a peer
 * reads it to decide which path can reach this engine at all. */
constexpr size_t kMagicAt = 24;

/* Two engines, because an engine cannot add itself as a peer -- there is
 * nothing to transfer between -- and the checks under test come before that
 * would be noticed. */
struct Pair {
  std::shared_ptr<LocalProvider> pa, pb;
  std::unique_ptr<Engine> a, b;

  bool setup() {
    EngineConfig cfg;
    cfg.progress = ProgressMode::kExplicit;
    if (LocalProvider::create(&pa) != Status::kOk) return false;
    if (LocalProvider::create(&pb) != Status::kOk) return false;
    pa->pair_with(pb);
    pb->pair_with(pa);
    if (make_engine(cfg, nullptr, pa, &a) != Status::kOk) return false;
    return make_engine(cfg, nullptr, pb, &b) == Status::kOk;
  }
};

void put_u16_at(std::vector<uint8_t>& v, size_t at, uint16_t x) {
  v[at] = static_cast<uint8_t>(x & 0xff);
  v[at + 1] = static_cast<uint8_t>(x >> 8);
}

}  // namespace

HUX_TEST(metadata_is_named_not_inferred) {
  Pair p;
  CHECK(p.setup());
  std::vector<uint8_t> meta;
  CHECK_STATUS(p.b->local_metadata(&meta), Status::kOk);
  CHECK(meta.size() >= kMagicAt + 8);

  uint32_t const magic = uint32_t(meta[kMagicAt]) |
                         (uint32_t(meta[kMagicAt + 1]) << 8) |
                         (uint32_t(meta[kMagicAt + 2]) << 16) |
                         (uint32_t(meta[kMagicAt + 3]) << 24);
  CHECK(magic == Engine::kMetadataMagic);
  uint16_t const major =
      static_cast<uint16_t>(meta[kMagicAt + 4] | (meta[kMagicAt + 5] << 8));
  CHECK(major == Engine::kMetadataMajor);
}

HUX_TEST(metadata_refuses_a_different_major) {
  /* Refused outright rather than read as far as it goes. A major change moves
   * fields, so a best-effort parse reads one field out of another's bytes and
   * dials something plausible with them. */
  Pair p;
  CHECK(p.setup());
  std::vector<uint8_t> meta;
  CHECK_STATUS(p.b->local_metadata(&meta), Status::kOk);

  PeerPtr peer;
  std::vector<uint8_t> newer = meta;
  put_u16_at(newer, kMagicAt + 4, Engine::kMetadataMajor + 1);
  CHECK_STATUS(p.a->add_peer(newer, &peer), Status::kUnsupported);

  if (Engine::kMetadataMajor > 0) {
    std::vector<uint8_t> older = meta;
    put_u16_at(older, kMagicAt + 4, Engine::kMetadataMajor - 1);
    /* In both directions: being the newer end is no reason to accept a
     * layout the peer does not share. */
    CHECK_STATUS(p.a->add_peer(older, &peer), Status::kUnsupported);
  }
}

HUX_TEST(metadata_accepts_a_newer_minor) {
  /* Minor changes only append fields an older reader stops before, so a peer
   * ahead on minor stays usable. */
  Pair p;
  CHECK(p.setup());
  std::vector<uint8_t> meta;
  CHECK_STATUS(p.b->local_metadata(&meta), Status::kOk);
  put_u16_at(meta, kMagicAt + 6, Engine::kMetadataMinor + 7);

  PeerPtr peer;
  CHECK_STATUS(p.a->add_peer(meta, &peer), Status::kOk);
}

HUX_TEST(metadata_with_the_magic_must_parse_exactly) {
  /* Once the magic has claimed the layout, a blob that does not parse is
   * malformed. Truncation used to fall back to treating the whole thing as
   * one provider's dialling blob, which is a different transport's bytes. */
  Pair p;
  CHECK(p.setup());
  std::vector<uint8_t> meta;
  CHECK_STATUS(p.b->local_metadata(&meta), Status::kOk);

  PeerPtr peer;
  std::vector<uint8_t> truncated(meta.begin(), meta.end() - 1);
  CHECK_STATUS(p.a->add_peer(truncated, &peer), Status::kInvalidArgument);

  std::vector<uint8_t> trailing = meta;
  trailing.push_back(0);
  CHECK_STATUS(p.a->add_peer(trailing, &peer), Status::kInvalidArgument);
}
