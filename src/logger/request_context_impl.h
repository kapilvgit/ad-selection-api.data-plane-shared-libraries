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

#ifndef LOGGER_REQUEST_CONTEXT_IMPL_H_
#define LOGGER_REQUEST_CONTEXT_IMPL_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "opentelemetry/logs/logger_provider.h"
#include "opentelemetry/logs/provider.h"
#include "src/logger/logger.pb.h"
#include "src/logger/request_context_logger.h"

namespace privacy_sandbox::server_common::log {

inline std::string_view ServerToken(
    std::optional<std::string_view> token = std::nullopt) {
  static std::string* server_token = [token]() {
    if (token == std::nullopt || token->empty()) {
      fprintf(stderr,
              "Warning: server token is not set, consent log is turned off.\n");
      return new std::string("");
    }
    constexpr int kTokenMinLength = 6;
    CHECK(token->length() >= kTokenMinLength)
        << "server token length must be at least " << kTokenMinLength;
    return new std::string(token->data());
  }();
  return *server_token;
}

ABSL_CONST_INIT inline opentelemetry::logs::Logger* logger_private = nullptr;

// Utility method to format the context provided as key/value pair into a
// string. Function excludes any empty values from the output string.
std::string FormatContext(
    const absl::btree_map<std::string, std::string>& context_map);

class ContextImpl : public RequestContext {
 public:
  ContextImpl(
      const absl::btree_map<std::string, std::string>& context_map,
      const ConsentedDebugConfiguration& debug_config,
      absl::AnyInvocable<DebugInfo*()> debug_info = []() { return nullptr; })
      : consented_sink(ServerToken()),
        debug_response_sink_(std::move(debug_info)) {
    Update(context_map, debug_config);
  }

  virtual ~ContextImpl() = default;

  std::string_view ContextStr() const override { return context_; }

  bool is_consented() const override { return consented_sink.is_consented(); }

  absl::LogSink* ConsentedSink() override { return &consented_sink; }

  bool is_debug_response() const override {
    return debug_response_sink_.should_log();
  };

  absl::LogSink* DebugResponseSink() override { return &debug_response_sink_; };

  void Update(const absl::btree_map<std::string, std::string>& new_context,
              const ConsentedDebugConfiguration& debug_config) {
    context_ = FormatContext(new_context);
    consented_sink.client_debug_token_ =
        debug_config.is_consented() ? debug_config.token() : "";
    debug_response_sink_.should_log_ = debug_config.is_debug_info_in_response();
  }

 private:
  friend class ConsentedLogTest;

  class ConsentedSinkImpl : public absl::LogSink {
   public:
    explicit ConsentedSinkImpl(std::string_view server_token)
        : server_token_(server_token) {}

    void Send(const absl::LogEntry& entry) override {
      logger_private->EmitLogRecord(
          entry.text_message_with_prefix_and_newline_c_str());
    }

    void Flush() override {}

    bool is_consented() const {
      return !server_token_.empty() && server_token_ == client_debug_token_;
    }

    // Debug token given by a consented client request.
    std::string client_debug_token_;
    // Debug token owned by the server.
    std::string_view server_token_;
  };

  class DebugResponseSinkImpl : public absl::LogSink {
   public:
    explicit DebugResponseSinkImpl(absl::AnyInvocable<DebugInfo*()> debug_info)
        : debug_info_(std::move(debug_info)) {}

    void Send(const absl::LogEntry& entry) override {
      debug_info_()->add_logs(entry.text_message_with_prefix());
    }

    void Flush() override {}

    bool should_log() const { return should_log_; }

    absl::AnyInvocable<DebugInfo*()> debug_info_;
    bool should_log_ = false;
  };

  void SetServerTokenForTestOnly(std::string_view token) {
    consented_sink.server_token_ = token;
  }

  std::string context_;
  ConsentedSinkImpl consented_sink;
  DebugResponseSinkImpl debug_response_sink_;
};

}  // namespace privacy_sandbox::server_common::log

#endif  // LOGGER_REQUEST_CONTEXT_IMPL_H_
