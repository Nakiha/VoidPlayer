#pragma once

#include <flutter/method_call.h>
#include <flutter/method_result.h>
#include <flutter/standard_method_codec.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class NativePlayerMethodDispatcher {
public:
    using MethodCall = flutter::MethodCall<flutter::EncodableValue>;
    using MethodResult = flutter::MethodResult<flutter::EncodableValue>;
    using MethodResultPtr = std::unique_ptr<MethodResult>;
    using Handler = std::function<void(const MethodCall&, MethodResultPtr)>;

    void Register(std::string method, Handler handler);
    void Dispatch(const MethodCall& method_call, MethodResultPtr result) const;

private:
    std::unordered_map<std::string, Handler> handlers_;
};
