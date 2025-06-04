/*
 * Copyright 2023 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SERVICES_COMMON_TELEMETRY_TELEMETRY_FLAG_H_
#define SERVICES_COMMON_TELEMETRY_TELEMETRY_FLAG_H_

#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "src/metric/definition.h"
#include "src/telemetry/flag/config.pb.h"

namespace privacy_sandbox::server_common::telemetry {

struct TelemetryFlag {
  TelemetryConfig server_config;
};

bool AbslParseFlag(std::string_view text, TelemetryFlag* flag,
                   std::string* err);

std::string AbslUnparseFlag(const TelemetryFlag&);

// BuildDependentConfig wrap `TelemetryConfig`, provide methods that also
// depends on the build options `//:non_prod_build`
class BuildDependentConfig {
 public:
  explicit BuildDependentConfig(TelemetryConfig config);

  enum class BuildMode { kProd, kExperiment };

  // Get build mode. The implementation depend on build flag.
  BuildMode GetBuildMode() const;

  // Get the metric mode to use
  TelemetryConfig::TelemetryMode MetricMode() const;

  // Should metric be collected and exported;
  // Used to initialize Open Telemetry.
  bool MetricAllowed() const;

  // Should trace be collected and exported;
  // Used to initialize Open Telemetry.
  bool TraceAllowed() const { return IsDebug(); }

  // Should logs be collected and exported;
  // Used to initialize Open Telemetry.
  bool LogsAllowed() const {
    return server_config_.mode() != TelemetryConfig::OFF;
  }

  // Should metric collection run as debug mode(without noise)
  bool IsDebug() const;

  int metric_export_interval_ms() const {
    return server_config_.metric_export_interval_ms();
  }

  int dp_export_interval_ms() const {
    return server_config_.dp_export_interval_ms();
  }

  // If server_config_ has defined MetricConfig list, if found return the
  // MetricConfig for `metric_name`, otherwise return error; if server_config_
  // has empty MetricConfig list, always return default MetricConfig.
  absl::StatusOr<MetricConfig> GetMetricConfig(
      std::string_view metric_name) const;

  // return error if metric is not configured right.
  absl::Status CheckMetricConfig(
      absl::Span<const metrics::DefinitionName* const> server_metrics) const;

  // Override the public partition of a metric.
  void SetPartition(std::string_view name,
                    absl::Span<const std::string_view> partitions);

  // Return the public partition of a metric.
  template <typename MetricT>
  absl::Span<const std::string_view> GetPartition(
      const MetricT& definition) const {
    return GetPartition(definition, definition.name_);
  }

  // Return the public partition of a metric.
  absl::Span<const std::string_view> GetPartition(
      const metrics::internal::Partitioned& definition,
      const std::string_view name) const;

 private:
  TelemetryConfig server_config_;
  absl::flat_hash_map<std::string, MetricConfig> metric_config_;
  absl::flat_hash_map<std::string, MetricConfig> internal_config_;
  absl::flat_hash_map<std::string, std::vector<std::string_view>>
      partition_config_view_;
};

}  // namespace privacy_sandbox::server_common::telemetry

#endif  // SERVICES_COMMON_TELEMETRY_TELEMETRY_FLAG_H_
