#ifndef ELIOP2P_BASE_ERROR_CODE_H
#define ELIOP2P_BASE_ERROR_CODE_H

#include <string>
#include <system_error>

namespace eliop2p {

// Error code categories
enum class ErrorCode {
    Success = 0,

    // General errors (1000-1999)
    InvalidArgument = 1001,
    NotFound = 1002,
    AlreadyExists = 1003,
    Timeout = 1004,
    Cancelled = 1005,
    InternalError = 1006,
    NotImplemented = 1007,

    // Network errors (2000-2999)
    NetworkError = 2001,
    ConnectionFailed = 2002,
    ConnectionClosed = 2003,
    SendFailed = 2004,
    ReceiveFailed = 2005,

    // Cache errors (3000-3999)
    CacheMiss = 3001,
    CacheFull = 3002,
    EvictionFailed = 3003,

    // P2P errors (4000-4999)
    NodeNotFound = 4001,
    TransferFailed = 4002,
    ChunkNotAvailable = 4003,

    // Storage errors (5000-5999)
    StorageError = 5001,
    AuthenticationFailed = 5002,
    BucketNotFound = 5003,
    ObjectNotFound = 5004,

    // Control plane errors (6000-6999)
    ControlPlaneError = 6001,
    RegistrationFailed = 6002,
    MetadataError = 6003
};

std::error_code make_error_code(ErrorCode code);
std::string to_string(ErrorCode code);

class ElioP2PError : public std::exception {
public:
    ElioP2PError(ErrorCode code, const std::string& message);

    ErrorCode code() const { return code_; }
    const char* what() const noexcept override;

private:
    ErrorCode code_;
    std::string message_;
};

} // namespace eliop2p

#endif // ELIOP2P_BASE_ERROR_CODE_H
