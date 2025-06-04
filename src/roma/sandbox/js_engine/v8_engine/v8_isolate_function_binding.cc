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

#include "v8_isolate_function_binding.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/match.h"
#include "src/roma/config/type_converter.h"
#include "src/roma/interface/function_binding_io.pb.h"
#include "src/roma/logging/logging.h"
#include "src/roma/sandbox/constants/constants.h"
#include "src/roma/sandbox/native_function_binding/rpc_wrapper.pb.h"

using google::scp::roma::proto::FunctionBindingIoProto;
using google::scp::roma::proto::RpcWrapper;
using google::scp::roma::sandbox::constants::kRequestId;
using google::scp::roma::sandbox::constants::kRequestUuid;
using Callback = void (*)(const v8::FunctionCallbackInfo<v8::Value>& info);

namespace google::scp::roma::sandbox::js_engine::v8_js_engine {
namespace {
constexpr char kCouldNotRunFunctionBinding[] =
    "ROMA: Could not run C++ function binding.";
constexpr char kUnexpectedDataInBindingCallback[] =
    "ROMA: Unexpected data in global callback.";
constexpr char kCouldNotConvertJsFunctionInputToNative[] =
    "ROMA: Could not convert JS function input to native C++ type.";
constexpr char kErrorInFunctionBindingInvocation[] =
    "ROMA: Error while executing native function binding.";

bool V8TypesToProto(const v8::FunctionCallbackInfo<v8::Value>& info,
                    FunctionBindingIoProto& proto) {
  if (info.Length() == 0) {
    // No arguments were passed to function
    return true;
  }
  if (info.Length() > 1) {
    return false;
  }

  auto isolate = info.GetIsolate();
  const auto& function_parameter = info[0];

  using StrVec = std::vector<std::string>;
  using StrMap = absl::flat_hash_map<std::string, std::string>;

  // Try to convert to one of the supported types
  if (std::string string_native; TypeConverter<std::string>::FromV8(
          isolate, function_parameter, &string_native)) {
    proto.set_input_string(std::move(string_native));
  } else if (StrVec vector_of_string_native; TypeConverter<StrVec>::FromV8(
                 isolate, function_parameter, &vector_of_string_native)) {
    proto.mutable_input_list_of_string()->mutable_data()->Reserve(
        vector_of_string_native.size());
    for (auto&& native : vector_of_string_native) {
      proto.mutable_input_list_of_string()->mutable_data()->Add(
          std::move(native));
    }
  } else if (StrMap map_of_string_native; TypeConverter<StrMap>::FromV8(
                 isolate, function_parameter, &map_of_string_native)) {
    for (auto&& [key, value] : map_of_string_native) {
      (*proto.mutable_input_map_of_string()->mutable_data())[std::move(key)] =
          std::move(value);
    }
  } else if (function_parameter->IsUint8Array()) {
    const auto array = function_parameter.As<v8::Uint8Array>();
    const auto data_len = array->Length();
    std::string native_data;
    native_data.resize(data_len);
    if (!TypeConverter<uint8_t*>::FromV8(isolate, function_parameter,
                                         native_data)) {
      return false;
    }
    *proto.mutable_input_bytes() = std::move(native_data);
  } else {
    // Unknown type
    return false;
  }

  return true;
}

v8::Local<v8::Value> ProtoToV8Type(v8::Isolate* isolate,
                                   const FunctionBindingIoProto& proto) {
  if (proto.has_output_string()) {
    return TypeConverter<std::string>::ToV8(isolate, proto.output_string());
  } else if (proto.has_output_list_of_string()) {
    return TypeConverter<std::vector<std::string>>::ToV8(
        isolate, proto.output_list_of_string().data());
  } else if (proto.has_output_map_of_string()) {
    return TypeConverter<absl::flat_hash_map<std::string, std::string>>::ToV8(
        isolate, proto.output_map_of_string().data());
  } else if (proto.has_output_bytes()) {
    return TypeConverter<uint8_t*>::ToV8(isolate, proto.output_bytes());
  }

  // This function didn't return anything from C++
  ROMA_VLOG(1) << "Function binding did not set any return value from C++.";
  return v8::Undefined(isolate);
}

}  // namespace

V8IsolateFunctionBinding::V8IsolateFunctionBinding(
    const std::vector<std::string>& function_names,
    std::unique_ptr<native_function_binding::NativeFunctionInvoker>
        function_invoker,
    std::string_view server_address)
    : function_invoker_(std::move(function_invoker)) {
  for (const auto& function_name : function_names) {
    binding_references_.emplace_back(Binding{
        .function_name = function_name,
        .instance = this,
    });
  }

  if (!server_address.empty()) {
    grpc_channel_ = grpc::CreateChannel(std::string(server_address),
                                        grpc::InsecureChannelCredentials());
    stub_ = privacy_sandbox::server_common::TestService::NewStub(grpc_channel_);
  }
}

bool V8IsolateFunctionBinding::NativeFieldsToProto(
    const Binding& binding, FunctionBindingIoProto& function_proto,
    RpcWrapper& rpc_proto) {
  std::string_view native_function_name = binding.function_name;
  if (native_function_name.empty()) {
    return false;
  }

  rpc_proto.set_function_name(native_function_name);
  rpc_proto.set_request_id(binding.instance->invocation_req_id_);
  rpc_proto.set_request_uuid(binding.instance->invocation_req_uuid_);
  *rpc_proto.mutable_io_proto() = function_proto;
  return true;
}

void V8IsolateFunctionBinding::GlobalV8FunctionCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  ROMA_VLOG(9) << "Calling V8 function callback";
  auto isolate = info.GetIsolate();
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  auto data = info.Data();
  if (data.IsEmpty()) {
    isolate->ThrowError(kUnexpectedDataInBindingCallback);
    ROMA_VLOG(1) << kUnexpectedDataInBindingCallback;
    return;
  }
  auto binding_external = v8::Local<v8::External>::Cast(data);
  auto binding = reinterpret_cast<Binding*>(binding_external->Value());
  FunctionBindingIoProto function_invocation_proto;
  if (!V8TypesToProto(info, function_invocation_proto)) {
    isolate->ThrowError(kCouldNotConvertJsFunctionInputToNative);
    ROMA_VLOG(1) << kCouldNotConvertJsFunctionInputToNative;
    return;
  }

  RpcWrapper rpc_proto;
  if (!NativeFieldsToProto(*binding, function_invocation_proto, rpc_proto)) {
    isolate->ThrowError(kCouldNotRunFunctionBinding);
    ROMA_VLOG(1) << kCouldNotRunFunctionBinding;
  }

  const auto result = binding->instance->function_invoker_->Invoke(rpc_proto);
  if (!result.ok()) {
    isolate->ThrowError(kCouldNotRunFunctionBinding);
    ROMA_VLOG(1) << kCouldNotRunFunctionBinding;
    return;
  }
  if (!rpc_proto.io_proto().errors().empty()) {
    isolate->ThrowError(kErrorInFunctionBindingInvocation);
    ROMA_VLOG(1) << kErrorInFunctionBindingInvocation;
    return;
  }
  const auto& returned_value = ProtoToV8Type(isolate, rpc_proto.io_proto());
  info.GetReturnValue().Set(returned_value);
}

void V8IsolateFunctionBinding::BindFunction(
    v8::Isolate* isolate, v8::Local<v8::ObjectTemplate>& global_object_template,
    void* binding, void (*callback)(const v8::FunctionCallbackInfo<v8::Value>&),
    std::string_view function_name) {
  const v8::Local<v8::External>& binding_context =
      v8::External::New(isolate, binding);
  // Create the function template to register in the global object
  const auto function_template =
      v8::FunctionTemplate::New(isolate, callback, binding_context);
  // Convert the function binding name to a v8 type
  const auto& binding_name =
      TypeConverter<std::string>::ToV8(isolate, function_name).As<v8::String>();
  global_object_template->Set(binding_name, function_template);
}

bool V8IsolateFunctionBinding::BindFunctions(
    v8::Isolate* isolate,
    v8::Local<v8::ObjectTemplate>& global_object_template) {
  if (!isolate) {
    return false;
  }
  for (auto& binding : binding_references_) {
    BindFunction(isolate, global_object_template,
                 reinterpret_cast<void*>(&binding), &GlobalV8FunctionCallback,
                 binding.function_name);
  }

  return true;
}

void V8IsolateFunctionBinding::AddIds(std::string_view uuid,
                                      std::string_view id) {
  if (!binding_references_.empty()) {
    // All binding_references_ share a single V8IsolateFunctionBinding instance,
    // so setting the members on binding_references[0].instance changes all
    // elements of binding_references_.
    binding_references_[0].instance->invocation_req_uuid_ = uuid;
    binding_references_[0].instance->invocation_req_id_ = id;
  }
}

void V8IsolateFunctionBinding::AddExternalReferences(
    std::vector<intptr_t>& external_references) {
  // Must add pointers that are not within the v8 heap to external_references_
  // so that the snapshot serialization works.
  for (const auto& binding : binding_references_) {
    external_references.push_back(reinterpret_cast<intptr_t>(&binding));
  }
  external_references.push_back(
      reinterpret_cast<intptr_t>(&GlobalV8FunctionCallback));
}

absl::Status V8IsolateFunctionBinding::InvokeRpc(RpcWrapper& rpc_proto) {
  return function_invoker_->Invoke(rpc_proto);
}
}  // namespace google::scp::roma::sandbox::js_engine::v8_js_engine
