/*
 * Copyright 2023 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Example command to run this (the grep is necessary to avoid noisy log
 * output):
 *
 * builders/tools/bazel-debian run \
 * //src/roma/sandbox/dispatcher:dispatcher_benchmark \
 * --test_output=all
 */

#include <gtest/gtest.h>

#include <benchmark/benchmark.h>

#include "absl/cleanup/cleanup.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/blocking_counter.h"
#include "src/roma/sandbox/dispatcher/dispatcher.h"

namespace {

using google::scp::roma::CodeObject;
using google::scp::roma::ResponseObject;
using google::scp::roma::sandbox::dispatcher::Dispatcher;
using google::scp::roma::sandbox::worker_api::WorkerSandboxApi;

std::vector<WorkerSandboxApi> Workers(int num_workers) {
  std::vector<WorkerSandboxApi> workers;
  workers.reserve(num_workers);
  for (int i = 0; i < num_workers; ++i) {
    workers.emplace_back(
        /*require_preload=*/true,
        /*native_js_function_comms_fd=*/-1,
        /*native_js_function_names=*/std::vector<std::string>(),
        /*server_address=*/"",
        /*max_worker_virtual_memory_mb=*/0,
        /*js_engine_initial_heap_size_mb=*/0,
        /*js_engine_maximum_heap_size_mb=*/0,
        /*js_engine_max_wasm_memory_number_of_pages=*/0,
        /*sandbox_request_response_shared_buffer_size_mb=*/0,
        /*enable_sandbox_sharing_request_response_with_buffer_only=*/false);
    CHECK(workers.back().Init().ok());
    CHECK(workers.back().Run().ok());
  }
  return workers;
}

void BM_Dispatch(benchmark::State& state) {
  const int number_of_calls = state.range(0);
  std::vector<WorkerSandboxApi> workers = Workers(/*num_workers=*/1);
  absl::Cleanup cleanup = [&] {
    for (WorkerSandboxApi& worker : workers) {
      CHECK(worker.Stop().ok());
    }
  };

  // Note: max_pending_requests must be large enough to hold all of the queued
  // tasks for the benchmark.
  Dispatcher dispatcher(absl::MakeSpan(workers),
                        /*max_pending_requests=*/100000);

  for (auto _ : state) {
    absl::BlockingCounter is_loading(number_of_calls);
    for (int i = 0; i < number_of_calls; ++i) {
      CodeObject load_request{
          .id = "id",
          .version_string = "v1",
          .js = R"(function test() { return 'Hello World'; })",
      };
      ASSERT_TRUE(
          dispatcher
              .Invoke(std::move(load_request),
                      [&is_loading](absl::StatusOr<ResponseObject> resp) {
                        ASSERT_TRUE(resp.ok());
                        is_loading.DecrementCount();
                      })
              .ok());
    }
    is_loading.Wait();
  }
}

}  // namespace

BENCHMARK(BM_Dispatch)->RangeMultiplier(10)->Range(1, 1000);

// Run the benchmark
BENCHMARK_MAIN();
