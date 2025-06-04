/*
 * Copyright 2022 Google LLC
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
 * //src/roma/benchmark:kv_server_udf_benchmark_test \
 * --test_output=all 2>&1 | fgrep -v sandbox2.cc
 */

#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include "src/roma/benchmark/fake_kv_server.h"
#include "src/roma/benchmark/test_code.h"
#include "src/roma/config/config.h"

namespace {

using google::scp::roma::Config;
using google::scp::roma::FunctionBindingObjectV2;
using google::scp::roma::FunctionBindingPayload;
using google::scp::roma::benchmark::CodeConfig;
using google::scp::roma::benchmark::FakeKvServer;

void LoadCodeBenchmark(std::string_view code, std::string_view handler_name,
                       benchmark::State& state) {
  Config config;
  config.number_of_workers = 10;
  FakeKvServer server(std::move(config));

  CodeConfig code_config;
  code_config.js = code;
  code_config.udf_handler_name = handler_name;

  // If the code is being padded with extra bytes then add a comment at the end
  // and fill it with extra zeroes.
  if (const int extra_padding_bytes = state.range(1); extra_padding_bytes > 0) {
    constexpr std::string_view extra_prefix = " // ";
    const int total_size =
        code_config.js.size() + extra_prefix.size() + extra_padding_bytes;
    code_config.js.reserve(total_size);
    code_config.js.append(extra_prefix);
    code_config.js.resize(total_size, '0');
  }

  // Each benchmark routine has exactly one `for (auto s : state)` loop, this
  // is what's timed.
  const int number_of_loads = state.range(0);
  for (auto _ : state) {
    for (int i = 0; i < number_of_loads; ++i) {
      server.SetCodeObject(code_config);
    }
  }
  state.SetItemsProcessed(number_of_loads);
  state.SetBytesProcessed(number_of_loads * code_config.js.length());
}

void ExecuteCodeBenchmark(std::string_view code, std::string_view handler_name,
                          benchmark::State& state) {
  const int number_of_calls = state.range(0);
  Config config;
  FakeKvServer server(std::move(config));

  CodeConfig code_config;
  code_config.js = code;
  code_config.udf_handler_name = handler_name;
  server.SetCodeObject(code_config);

  // Each benchmark routine has exactly one `for (auto s : state)` loop, this
  // is what's timed.
  for (auto _ : state) {
    for (int i = 0; i < number_of_calls; ++i) {
      benchmark::DoNotOptimize(server.ExecuteCode({}));
    }
  }
  state.SetItemsProcessed(number_of_calls);
}

// This C++ callback function is called in the benchmark below:
static void HelloWorldCallback(FunctionBindingPayload<>& wrapper) {
  wrapper.io_proto.set_output_string("I am a callback");
}

void BM_ExecuteHelloWorldCallback(benchmark::State& state) {
  const int number_of_calls = state.range(0);
  Config config;
  {
    auto function_object = std::make_unique<FunctionBindingObjectV2<>>();
    function_object->function_name = "callback";
    function_object->function = HelloWorldCallback;
    config.RegisterFunctionBinding(std::move(function_object));
  }
  FakeKvServer server(std::move(config));

  CodeConfig code_config{
      .js = "hello = () => 'Hello world! ' + callback();",
      .udf_handler_name = "hello",
  };
  server.SetCodeObject(code_config);

  // Each benchmark routine has exactly one `for (auto s : state)` loop, this
  // is what's timed.
  for (auto _ : state) {
    for (int i = 0; i < number_of_calls; ++i) {
      benchmark::DoNotOptimize(server.ExecuteCode({}));
    }
  }
  state.SetItemsProcessed(number_of_calls);
}

void BM_LoadHelloWorld(benchmark::State& state) {
  LoadCodeBenchmark(google::scp::roma::benchmark::kCodeHelloWorld,
                    google::scp::roma::benchmark::kHandlerNameHelloWorld,
                    state);
}

void BM_LoadGoogleAdManagerGenerateBid(benchmark::State& state) {
  LoadCodeBenchmark(
      google::scp::roma::benchmark::kCodeGoogleAdManagerGenerateBid,
      google::scp::roma::benchmark::kHandlerNameGoogleAdManagerGenerateBid,
      state);
}

void BM_ExecuteHelloWorld(benchmark::State& state) {
  ExecuteCodeBenchmark(google::scp::roma::benchmark::kCodeHelloWorld,
                       google::scp::roma::benchmark::kHandlerNameHelloWorld,
                       state);
}

void BM_ExecutePrimeSieve(benchmark::State& state) {
  ExecuteCodeBenchmark(google::scp::roma::benchmark::kCodePrimeSieve,
                       google::scp::roma::benchmark::kHandlerNamePrimeSieve,
                       state);
}

}  // namespace

// Register the function as a benchmark
BENCHMARK(BM_LoadHelloWorld)
    ->ArgsProduct({
        // Run this many loads of the code.
        {1, 10, 100},
        // Pad with this many extra bytes.
        {0, 128, 512, 1024, 10'000, 20'000, 50'000, 100'000, 200'000, 500'000},
    });
BENCHMARK(BM_LoadGoogleAdManagerGenerateBid)
    ->ArgsProduct({
        {1, 10, 100},  // Run this many loads of the code.
        {0},           // No need to pad this code with extra bytes.
    });
BENCHMARK(BM_ExecuteHelloWorld)->RangeMultiplier(10)->Range(1, 100);
BENCHMARK(BM_ExecuteHelloWorldCallback)->RangeMultiplier(10)->Range(1, 100);
BENCHMARK(BM_ExecutePrimeSieve)->RangeMultiplier(10)->Range(1, 100);

// Run the benchmark
BENCHMARK_MAIN();
