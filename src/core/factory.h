// Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
#ifndef HUX_CORE_FACTORY_H
#define HUX_CORE_FACTORY_H

#include <memory>

#include "hux/engine.h"
#include "transport/provider.h"

namespace hux {

// 显式注入 provider 的构造入口。测试用 mock 替换后端，
// 上层工厂按 EngineConfig::preferred_provider 选择真实后端。
Status make_engine(EngineConfig const& cfg,
                   std::shared_ptr<DeviceBackend> device,
                   TransportProviderPtr provider,
                   std::unique_ptr<Engine>* out);

}  // namespace hux
#endif  // HUX_CORE_FACTORY_H
