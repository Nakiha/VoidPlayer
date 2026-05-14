#include "native_player_method_dispatcher.h"

#include <utility>

void NativePlayerMethodDispatcher::Register(std::string method, Handler handler) {
    handlers_[std::move(method)] = std::move(handler);
}

void NativePlayerMethodDispatcher::Dispatch(
    const MethodCall& method_call,
    MethodResultPtr result) const {
    const auto it = handlers_.find(method_call.method_name());
    if (it == handlers_.end()) {
        result->NotImplemented();
        return;
    }
    it->second(method_call, std::move(result));
}
