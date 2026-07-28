#pragma once

#include "signal_studio/visualization/visualization.hpp"
#include "signal_studio/workbench/workbench.hpp"

#include <cstdint>
#include <memory>

namespace signal_studio_consumer {

inline bool verify_ms03_public_api() {
  const auto exact = signal::visualization::parse_frequency_hz("2.450000001 GHz");
  const auto loaded = signal::data::SampleRange::make(1'000, 101'000);
  if (!exact || exact.value() != 2'450'000'001 || !loaded)
    return false;

  signal::visualization::ViewportController viewport{"installed-ms03-consumer"};
  const auto request = viewport.bind_source("consumer-source-v1", loaded.value(), {-25'000'000, 25'000'000});
  const auto current = signal::data::SampleRange::make(20'000, 40'000);
  if (!request || !current || !viewport.set_time(current.value()) || !viewport.set_frequency({-5'000'000, 5'000'000}))
    return false;

  signal::visualization::OverlayModel overlays{loaded.value(), {-25'000'000, 25'000'000}};
  const auto selection =
      overlays.create({"", "installed-consumer-selection", signal::visualization::SelectionKind::time_frequency,
                       current.value(), signal::visualization::FrequencyRange{-1'000'000, 1'000'000}});
  if (!selection || selection.value().id.empty())
    return false;

  signal::workbench::ServiceRegistry services;
  auto instance = std::make_shared<std::uint64_t>(42U);
  const auto service_status = services.add({"consumer.service", "consumer.contract", instance});
  signal::workbench::CommandRegistry commands;
  std::uint32_t executions{};
  const auto command_status =
      commands.add({"consumer.command", "消费者命令", "验证已安装命令注册表", "Ctrl+Alt+C", "{}", [] { return true; },
                    [&executions] {
                      ++executions;
                      return signal::core::Status::success();
                    }});
  return service_status && services.resolve("consumer.service") == instance && command_status &&
         commands.execute("consumer.command") && executions == 1U;
}

} // namespace signal_studio_consumer
