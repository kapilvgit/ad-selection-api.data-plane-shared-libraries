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
 */

#ifndef ROMA_SANDBOX_NATIVE_FUNCTION_BINDING_NATIVE_FUNCTION_HANDLER_SAPI_IPC_H_
#define ROMA_SANDBOX_NATIVE_FUNCTION_BINDING_NATIVE_FUNCTION_HANDLER_SAPI_IPC_H_

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "sandboxed_api/sandbox2/comms.h"
#include "src/roma/logging/logging.h"
#include "src/roma/metadata_storage/metadata_storage.h"
#include "src/roma/sandbox/constants/constants.h"
#include "src/roma/sandbox/native_function_binding/rpc_wrapper.pb.h"

#include "native_function_table.h"

using google::scp::roma::sandbox::constants::kRequestId;
using google::scp::roma::sandbox::constants::kRequestUuid;

inline constexpr std::string_view kFailedNativeHandlerExecution =
    "ROMA: Failed to execute the C++ function.";
inline constexpr std::string_view kCouldNotFindFunctionName =
    "ROMA: Could not find C++ function by name.";
inline constexpr std::string_view kCouldNotFindMetadata =
    "ROMA: Could not find metadata associated with C++ function.";
inline constexpr std::string_view kCouldNotFindMutex =
    "ROMA: Could not find mutex for metadata associated with C++ function.";

namespace google::scp::roma::sandbox::native_function_binding {

template <typename TMetadata = google::scp::roma::DefaultMetadata>
class NativeFunctionHandlerSapiIpc {
 public:
  /**
   * @brief Construct a new Native Function Handler Sapi Ipc object
   *
   * @param function_table The function table object to look up function
   * bindings in.
   * @param local_fds The local file descriptors that we will use to listen for
   * function calls.
   * @param remote_fds The remote file descriptors. These are what the remote
   * process uses to send requests to this process.
   */
  NativeFunctionHandlerSapiIpc(NativeFunctionTable<TMetadata>* function_table,
                               MetadataStorage<TMetadata>* metadata_storage,
                               const std::vector<int>& local_fds,
                               std::vector<int> remote_fds)
      : stop_(false),
        function_table_(function_table),
        metadata_storage_(metadata_storage),
        remote_fds_(std::move(remote_fds)) {
    ipc_comms_.reserve(local_fds.size());
    for (const int local_fd : local_fds) {
      ipc_comms_.emplace_back(local_fd);
    }
  }

  void Run() {
    ROMA_VLOG(9) << "Calling native function handler";
    for (int i = 0; i < ipc_comms_.size(); i++) {
      function_handler_threads_.emplace_back([this, i] {
        while (true) {
          sandbox2::Comms& comms = ipc_comms_.at(i);
          proto::RpcWrapper wrapper_proto;

          // This unblocks once a call is issued from the other side
          const bool received = comms.RecvProtoBuf(&wrapper_proto);
          if (absl::MutexLock lock(&stop_mutex_); stop_) {
            break;
          }
          if (!received) {
            continue;
          }

          auto io_proto = wrapper_proto.mutable_io_proto();
          const auto& invocation_req_uuid = wrapper_proto.request_uuid();

          // Get function name
          if (const auto function_name = wrapper_proto.function_name();
              !function_name.empty()) {
            if (auto reader = ScopedValueReader<TMetadata>::Create(
                    metadata_storage_->GetMetadataMap(), invocation_req_uuid);
                !reader.ok()) {
              // If mutex can't be found, add errors to the proto to return
              io_proto->mutable_errors()->Add(std::string(kCouldNotFindMutex));
              ROMA_VLOG(1) << kCouldNotFindMutex;
            } else if (auto value = reader->Get(); !value.ok()) {
              // If metadata can't be found, add errors to the proto to return
              io_proto->mutable_errors()->Add(
                  std::string(kCouldNotFindMetadata));
              ROMA_VLOG(1) << kCouldNotFindMetadata;
            } else if (FunctionBindingPayload<TMetadata> wrapper{
                           *io_proto,
                           **value,
                       };
                       !function_table_->Call(function_name, wrapper).ok()) {
              // If execution failed, add errors to the proto to return
              io_proto->mutable_errors()->Add(
                  std::string(kFailedNativeHandlerExecution));
              ROMA_VLOG(1) << kFailedNativeHandlerExecution;
            }
          } else {
            // If we can't find the function, add errors to the proto to return
            io_proto->mutable_errors()->Add(
                std::string(kCouldNotFindFunctionName));
            ROMA_VLOG(1) << kCouldNotFindFunctionName;
          }
          if (!comms.SendProtoBuf(wrapper_proto)) {
            continue;
          }
        }
      });
    }
  }

  void Stop() {
    {
      absl::MutexLock lock(&stop_mutex_);
      stop_ = true;
    }

    // We write to the comms object so that we can unblock the function binding
    // threads waiting on it.
    for (const int fd : remote_fds_) {
      sandbox2::Comms remote_comms(fd);
      proto::FunctionBindingIoProto io_proto;
      remote_comms.SendProtoBuf(io_proto);
    }
    // Wait for the function binding threads to stop before terminating the
    // comms object below.
    for (auto& t : function_handler_threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
    for (sandbox2::Comms& c : ipc_comms_) {
      c.Terminate();
    }
  }

 private:
  bool stop_ ABSL_GUARDED_BY(stop_mutex_);
  absl::Mutex stop_mutex_;

  NativeFunctionTable<TMetadata>* function_table_;
  // Map of invocation request uuid to associated metadata.
  MetadataStorage<TMetadata>* metadata_storage_;
  std::vector<std::thread> function_handler_threads_;
  std::vector<sandbox2::Comms> ipc_comms_;
  // We need the remote file descriptors to unblock the local ones when stopping
  std::vector<int> remote_fds_;
};

}  // namespace google::scp::roma::sandbox::native_function_binding

#endif  // ROMA_SANDBOX_NATIVE_FUNCTION_BINDING_NATIVE_FUNCTION_HANDLER_SAPI_IPC_H_
