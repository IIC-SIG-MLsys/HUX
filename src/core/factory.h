/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_CORE_FACTORY_H
#define HUX_CORE_FACTORY_H

#include <memory>
#include <vector>

#include "hux/engine.h"
#include "transport/provider.h"

namespace hux {

/* Construction with an explicitly injected provider: tests substitute the
 * mock, while the upper factory selects a real backend by
 * EngineConfig::preferred_provider. */
Status make_engine(EngineConfig const& cfg,
                   std::shared_ptr<DeviceBackend> device,
                   TransportProviderPtr provider, std::unique_ptr<Engine>* out);

/* Several providers, in preference order. A peer is then reached over the one
 * that suits where it is -- the same host or another machine -- rather than
 * over whichever single transport the engine was built with. Memory is
 * registered with all of them, because which path a peer will arrive on is not
 * known when it is registered, and a provider that refuses a particular
 * allocation simply has no key for it. */
Status make_engine(EngineConfig const& cfg,
                   std::shared_ptr<DeviceBackend> device,
                   std::vector<TransportProviderPtr> providers,
                   std::unique_ptr<Engine>* out);

}  // namespace hux
#endif  // HUX_CORE_FACTORY_H
