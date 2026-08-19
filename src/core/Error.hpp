#pragma once

#include <string>
#include <variant>

namespace ifs {

enum class ErrorCode {
    None = 0,
    InvalidConfig,
    ServerStartFailed,
    ServerBindFailed,
    ResourceNotFound,
    InternalError,
    ShutdownRequested
};

struct Error {
    ErrorCode   code;
    std::string message;

    Error(ErrorCode c, std::string msg)
        : code(c), message(std::move(msg)) {}
};

template <typename T>
using Result = std::variant<T, Error>;

template <typename T>
bool isOk(const Result<T>& result) {
    return std::holds_alternative<T>(result);
}

template <typename T>
const T& getValue(const Result<T>& result) {
    return std::get<T>(result);
}

template <typename T>
const Error& getError(const Result<T>& result) {
    return std::get<Error>(result);
}

} // namespace ifs
