#include "WAVMWasmModule.h"
#include "syscalls.h"

#include <faabric/util/bytes.h>
#include <faabric/util/logging.h>

#include <netdb.h>

#include <WAVM/Runtime/Intrinsics.h>
#include <WAVM/Runtime/Runtime.h>

using namespace WAVM;

namespace wasm {
/** Writes changes to a native sockaddr back to a wasm sockaddr. This is
 * important in several networking syscalls that receive responses and modify
 * arguments in place */
void setSockAddr(sockaddr nativeSockAddr, I32 addrPtr)
{
    // Get pointer to wasm address
    wasm_sockaddr* wasmAddrPtr = &Runtime::memoryRef<wasm_sockaddr>(
      getExecutingWAVMModule()->defaultMemory, (Uptr)addrPtr);

    // Modify in place
    wasmAddrPtr->sa_family = nativeSockAddr.sa_family;
    std::copy(nativeSockAddr.sa_data,
              nativeSockAddr.sa_data + 14,
              wasmAddrPtr->sa_data);
}

void setSockLen(socklen_t nativeValue, I32 wasmPtr)
{
    // Get pointer to wasm address
    I32* wasmAddrPtr = &Runtime::memoryRef<I32>(
      getExecutingWAVMModule()->defaultMemory, (Uptr)wasmPtr);
    std::copy(&nativeValue, &nativeValue + 1, wasmAddrPtr);
}

/**
 * When properly isolated, functions will run in their own network namespace,
 * therefore we can be relatively comfortable passing some of the syscalls
 * straight through.
 */
I32 s__socketcall(I32 call, I32 argsPtr)
{
    WAVMWasmModule* module = getExecutingWAVMModule();
    Runtime::Memory* memoryPtr = module->defaultMemory;

    // NOTE
    // We don't want to support server-side socket syscalls as we expect
    // functions only to be clients

    switch (call) {
            // ----------------------------
            // Suppported
            // ----------------------------

        case (SocketCalls::sc_socket): {
            // Socket constants can be found in musl at include/sys/socket.h and
            // include/netinet/in.h

            U32* subCallArgs =
              Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, 3);
            U32 domain = subCallArgs[0];
            U32 type = subCallArgs[1];
            U32 protocol = subCallArgs[2];

            switch (domain) {
                case (2): {
                    domain = AF_INET;
                    break;
                }
                case (10): {
                    domain = AF_INET6;
                    break;
                }
                default: {
                    printf("Unrecognised domain (%u)\n", domain);
                    break;
                }
            }

            switch (type) {
                case (1): {
                    type = SOCK_STREAM;
                    break;
                }
                case (2): {
                    type = SOCK_DGRAM;
                    break;
                }
                default: {
                    printf("Unrecognised socket type (%u)\n", type);
                    break;
                }
            }

            switch (protocol) {
                case (0): {
                    protocol = IPPROTO_IP;
                    break;
                }
                case (6): {
                    protocol = IPPROTO_TCP;
                    break;
                }
                default: {
                    printf("Unrecognised protocol (%u)\n", protocol);
                    break;
                }
            }

            SPDLOG_DEBUG("S - socket - {} {} {}", domain, type, protocol);
            I32 sock = (int)syscall(SYS_socket, domain, type, protocol);

            if (sock < 0) {
                printf("Socket error: %i\n", sock);
            }

            return sock;
        }

        case (SocketCalls::sc_connect): {
            U32* subCallArgs =
              Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, 3);

            I32 sockfd = subCallArgs[0];
            I32 addrPtr = subCallArgs[1];

            SPDLOG_DEBUG(
              "S - connect - {} {} {}", sockfd, addrPtr, subCallArgs[2]);

            sockaddr addr = getSockAddr(addrPtr);
            int result = connect(sockfd, &addr, sizeof(sockaddr));

            return result;
        }

        case (SocketCalls::sc_recv):
        case (SocketCalls::sc_recvfrom):
        case (SocketCalls::sc_sendto):
        case (SocketCalls::sc_send): {
            int argCount = 4;

            if (call == SocketCalls::sc_sendto ||
                call == SocketCalls::sc_recvfrom) {
                argCount = 6;
            }

            // Pull out arguments
            U32* subCallArgs = Runtime::memoryArrayPtr<U32>(
              memoryPtr, (Uptr)argsPtr, (Uptr)argCount);
            I32 sockfd = subCallArgs[0];
            Uptr bufPtr = subCallArgs[1];
            size_t bufLen = subCallArgs[2];
            I32 flags = subCallArgs[3];

            // Set up buffer
            U8* buf = Runtime::memoryArrayPtr<U8>(memoryPtr, bufPtr, bufLen);

            ssize_t result = 0;
            if (call == SocketCalls::sc_send) {
                SPDLOG_DEBUG(
                  "S - send - {} {} {} {}", sockfd, bufPtr, bufLen, flags);

                result = send(sockfd, buf, bufLen, flags);

            } else if (call == SocketCalls::sc_recv) {
                SPDLOG_DEBUG(
                  "S - recv - {} {} {} {}", sockfd, bufPtr, bufLen, flags);

                result = recv(sockfd, buf, bufLen, flags);

            } else {
                I32 sockAddrPtr = subCallArgs[4];
                sockaddr sockAddr = getSockAddr(sockAddrPtr);
                socklen_t nativeAddrLen = sizeof(sockAddr);
                socklen_t addrLen = subCallArgs[5];

                if (call == SocketCalls::sc_sendto) {
                    SPDLOG_DEBUG("S - sendto - {} {} {} {} {} {}",
                                 sockfd,
                                 bufPtr,
                                 bufLen,
                                 flags,
                                 sockAddrPtr,
                                 addrLen);

                    result = sendto(
                      sockfd, buf, bufLen, flags, &sockAddr, nativeAddrLen);

                } else {
                    // Note, addrLen here is actually a pointer
                    SPDLOG_DEBUG("S - recvfrom - {} {} {} {} {} {}",
                                 sockfd,
                                 bufPtr,
                                 bufLen,
                                 flags,
                                 sockAddrPtr,
                                 addrLen);

                    // Make the call
                    result = recvfrom(
                      sockfd, buf, bufLen, flags, &sockAddr, &nativeAddrLen);

                    // Note, recvfrom will modify the sockaddr and addrlen in
                    // place with the details returned from the host, therefore
                    // we must also modify the original wasm object
                    setSockAddr(sockAddr, sockAddrPtr);
                    setSockLen(nativeAddrLen, addrLen);
                }
            }

            return (I32)result;
        }

        case (SocketCalls::sc_bind): {
            U32* subCallArgs =
              Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, 3);
            I32 sockfd = subCallArgs[0];

            I32 addrPtr = subCallArgs[1];
            sockaddr addr = getSockAddr(addrPtr);

            SPDLOG_DEBUG(
              "S - bind - {} {} {}", sockfd, addrPtr, subCallArgs[2]);

            int bindResult = bind(sockfd, &addr, sizeof(addr));

            return (I32)bindResult;
        }

        case (SocketCalls::sc_getsockname): {
            U32* subCallArgs =
              Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, 3);
            I32 sockfd = subCallArgs[0];
            I32 addrPtr = subCallArgs[1];
            I32 addrLenPtr = subCallArgs[2];

            SPDLOG_DEBUG(
              "S - getsockname - {} {} {}", sockfd, addrPtr, addrLenPtr);

            sockaddr nativeAddr = getSockAddr(addrPtr);
            socklen_t nativeAddrLen = sizeof(nativeAddr);

            int result = getsockname(sockfd, &nativeAddr, &nativeAddrLen);

            // Make sure we write any results back to the wasm objects
            setSockAddr(nativeAddr, addrPtr);
            setSockLen(nativeAddrLen, addrLenPtr);

            return result;
        }

            // ----------------------------
            // Unfinished
            // ----------------------------

        case (SocketCalls::sc_getpeername): {
            SPDLOG_DEBUG("S - getpeername - {} {}", call, argsPtr);
            return 0;
        }

        case (SocketCalls::sc_socketpair): {
            SPDLOG_DEBUG("S - socketpair - {} {}", call, argsPtr);
            return 0;
        }

        case (SocketCalls::sc_shutdown): {
            SPDLOG_DEBUG("S - shutdown - {} {}", call, argsPtr);
            return 0;
        }

        case (SocketCalls::sc_setsockopt): {
            SPDLOG_DEBUG("S - setsockopt - {} {}", call, argsPtr);
            return 0;
        }
        case (SocketCalls::sc_getsockopt): {
            SPDLOG_DEBUG("S - getsockopt - {} {}", call, argsPtr);
            return 0;
        }

        case (SocketCalls::sc_sendmsg): {
            SPDLOG_DEBUG("S - sendmsg - {} {}", call, argsPtr);
            return 0;
        }

        case (SocketCalls::sc_recvmsg): {
            SPDLOG_DEBUG("S - recvmsg - {} {}", call, argsPtr);
            return 0;
        }

        case (SocketCalls::sc_accept4): {
            SPDLOG_DEBUG("S - accept4 - {} {}", call, argsPtr);
            return 0;
        }

        case (SocketCalls::sc_recvmmsg): {
            SPDLOG_DEBUG("S - recvmmsg - {} {}", call, argsPtr);
            return 0;
        }

        case (SocketCalls::sc_sendmmsg): {
            SPDLOG_DEBUG("S - sendmmsg - {} {}", call, argsPtr);
            return 0;
        }

            // ----------------------------
            // Not supported
            // ----------------------------

        case (SocketCalls::sc_accept):
            // Server-side
            SPDLOG_DEBUG("S - accept - {} {}", call, argsPtr);

            throwException(
              Runtime::ExceptionTypes::calledUnimplementedIntrinsic);

        case (SocketCalls::sc_listen): {
            // Server-side
            SPDLOG_DEBUG("S - listen - {} {}", call, argsPtr);

            throwException(
              Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
        }

        default: {
            printf("Unrecognised socketcall %i\n", call);
            return 0;
        }
    }

    return 0;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "gethostbyname",
                               I32,
                               _gethostbyname,
                               I32 hostnamePtr)
{
    const std::string hostname = getStringFromWasm(hostnamePtr);
    SPDLOG_DEBUG("S - gethostbyname {}", hostname);

    return 0;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "gethostname",
                               I32,
                               gethostname,
                               I32 buffer,
                               I32 bufferLen)
{
    SPDLOG_DEBUG("S - gethostname {} {}", buffer, bufferLen);

    Runtime::Memory* memoryPtr = getExecutingWAVMModule()->defaultMemory;
    char* key = &Runtime::memoryRef<char>(memoryPtr, (Uptr)buffer);
    std::strcpy(key, FAKE_HOSTNAME);

    return 0;
}

// ------------------------------------------------------
// NOT SUPPORTED
// ------------------------------------------------------

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "socket", I32, socket, I32 a, I32 b, I32 c)
{
    SPDLOG_DEBUG("S - socket - {} {} {}", a, b, c);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "sock_accept",
                               I32,
                               sock_accept,
                               I32 a,
                               I32 b,
                               I32 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "sock_send",
                               I32,
                               wasi_sock_send,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d,
                               I32 e)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "sock_recv",
                               I32,
                               wasi_sock_recv,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d,
                               I32 e,
                               I32 f)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "sock_shutdown",
                               I32,
                               wasi_sock_shutdown,
                               I32 a,
                               I32 b)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "bind", I32, bind, I32 a, I32 b, I32 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "listen", I32, listen, I32 a, I32 b)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "setsockopt",
                               I32,
                               setsockopt,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d,
                               I32 e)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "accept", I32, accept, I32 a, I32 b, I32 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "inet_addr", I32, inet_addr, I32 a)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "connect",
                               I32,
                               connect,
                               I32 a,
                               I32 b,
                               I32 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "recvfrom",
                               I32,
                               recvfrom,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d,
                               I32 e,
                               I32 f)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "sendto",
                               I32,
                               sendto,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d,
                               I32 e,
                               I32 f)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "inet_ntoa", I32, inet_ntoa, I32 a)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "getprotobyname",
                               I32,
                               getprotobyname,
                               I32 a)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "getservbyname",
                               I32,
                               s__getservbyname,
                               I32 a,
                               I32 b)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "gethostbyaddr",
                               I32,
                               s__gethostbyaddr,
                               I32 a,
                               I32 b,
                               I32 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "getservbyport",
                               I32,
                               s__getservbyport,
                               I32 a,
                               I32 b)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "getsockname",
                               I32,
                               s__getsockname,
                               I32 a,
                               I32 b,
                               I32 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "atoi", I32, atoi, I32 a)
{
    SPDLOG_DEBUG("S - atoi - {}", a);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "htons", I32, _htons, I32 a)
{
    SPDLOG_DEBUG("S - htons - {}", a);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "ntohl", I32, _ntohl, I32 a)
{
    SPDLOG_DEBUG("S - ntohl - {}", a);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "ntohs", I32, _ntohs, I32 a)
{
    SPDLOG_DEBUG("S - ntohs - {}", a);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "htonl", I32, _htonl, I32 a)
{
    SPDLOG_DEBUG("S - htonl - {}", a);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "inet_aton", I32, _inet_aton, I32 a, I32 b)
{
    SPDLOG_DEBUG("S - inet_aton - {} {}", a, b);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "shutdown", I32, _shutdown, I32 a, I32 b)
{
    SPDLOG_DEBUG("S - shutdown - {} {}", a, b);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "inet_pton",
                               I32,
                               _inet_pton,
                               I32 a,
                               I32 b,
                               I32 c)
{
    SPDLOG_DEBUG("S - inet_pton - {} {} {}", a, b, c);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "inet_ntop",
                               I32,
                               _inet_ntop,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d)
{
    SPDLOG_DEBUG("S - inet_ntop - {} {} {} {}", a, b, c, d);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "recv",
                               I32,
                               _recv,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d)
{
    SPDLOG_DEBUG("S - recv - {} {} {} {}", a, b, c, d);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "send",
                               I32,
                               _send,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d)
{
    SPDLOG_DEBUG("S - send - {} {} {} {}", a, b, c, d);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "getsockopt",
                               I32,
                               getsockopt,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d,
                               I32 e)
{
    SPDLOG_DEBUG("S - getsockopt - {} {} {} {} {}", a, b, c, d, e);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

void networkLink() {}
}
