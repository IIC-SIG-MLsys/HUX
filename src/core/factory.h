/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#ifndef HUX_CORE_FACTORY_H
#define HUX_CORE_FACTORY_H

#include <memory>

#include "hux/engine.h"
#include "transport/provider.h"

namespace hux {

/* Construction with an explicitly injected provider: tests substitute the
 * mock, while the upper factory selects a real backend by
 * EngineConfig::preferred_provider. */
Status make_engine(EngineConfig const& cfg,
                   std::shared_ptr<DeviceBackend> device,
                   TransportProviderPtr provider, std::unique_ptr<Engine>* out);

}  // namespace hux
#endif  // HUX_CORE_FACTORY_H
