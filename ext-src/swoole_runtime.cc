/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author:   Tianfeng Han  <mikan.tenny@gmail.com>                      |
  +----------------------------------------------------------------------+
 */

#include "php_swoole_cxx.h"

#include "swoole_socket.h"
#include "swoole_util.h"

#include "thirdparty/php/standard/proc_open.h"

#ifdef SW_USE_CURL
#include "swoole_curl_interface.h"
#endif

#include <unordered_map>

BEGIN_EXTERN_C()
#include "stubs/php_swoole_runtime_arginfo.h"

#ifdef SW_USE_PGSQL
extern void swoole_pgsql_set_blocking(bool blocking);
#endif

#ifdef SW_USE_ODBC
extern void swoole_odbc_set_blocking(bool blocking);
#endif

#ifdef SW_USE_ORACLE
extern void swoole_oracle_set_blocking(bool blocking);
#endif

#ifdef SW_USE_SQLITE
extern void swoole_sqlite_set_blocking(bool blocking);
#endif
END_EXTERN_C()

/* openssl */
#ifndef OPENSSL_NO_ECDH
#define HAVE_ECDH 1
#endif
#ifndef OPENSSL_NO_TLSEXT
#define HAVE_TLS_SNI 1
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
#define HAVE_TLS_ALPN 1
#endif
#endif
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
#define HAVE_SEC_LEVEL 1
#endif

using swoole::Coroutine;
using swoole::PHPCoroutine;
using swoole::coroutine::PollSocket;
using swoole::coroutine::Socket;
using swoole::coroutine::System;

SW_EXTERN_C_BEGIN
static PHP_METHOD(swoole_runtime, enableCoroutine);
static PHP_METHOD(swoole_runtime, getHookFlags);
static PHP_METHOD(swoole_runtime, setHookFlags);
static PHP_FUNCTION(swoole_sleep);
static PHP_FUNCTION(swoole_usleep);
static PHP_FUNCTION(swoole_time_nanosleep);
static PHP_FUNCTION(swoole_time_sleep_until);
static PHP_FUNCTION(swoole_stream_select);
static PHP_FUNCTION(swoole_stream_socket_pair);
static PHP_FUNCTION(swoole_user_func_handler);
#if defined(HAVE_PUTENV) && defined(SW_THREAD)
static PHP_FUNCTION(swoole_putenv);
#endif
#if PHP_VERSION_ID >= 80400
extern PHP_FUNCTION(swoole_exit);
#endif
SW_EXTERN_C_END

static void inherit_class(const char *child_name, size_t child_length, const char *parent_name, size_t parent_length);
static void detach_parent_class(const char *child_name);
static void clear_class_entries();
static int socket_set_option(php_stream *stream, int option, int value, void *ptrparam);
static php_stream_size_t socket_read(php_stream *stream, char *buf, size_t count);
static php_stream_size_t socket_write(php_stream *stream, const char *buf, size_t count);
static int socket_flush(php_stream *stream);
static int socket_close(php_stream *stream, int close_handle);
static int socket_stat(php_stream *stream, php_stream_statbuf *ssb);
static int socket_cast(php_stream *stream, int castas, void **ret);
static bool socket_ssl_set_options(Socket *sock, php_stream_context *context);
// clang-format off
static zend_class_entry *swoole_runtime_ce;

static php_stream_ops socket_ops {
    socket_write,
    socket_read,
    socket_close,
    socket_flush,
    "socket/coroutine",
    nullptr, /* seek */
    socket_cast,
    socket_stat,
    socket_set_option,
};

struct NetStream {
    php_netstream_data_t stream;
    std::shared_ptr<Socket> socket;
    bool blocking;
};

static struct {
    php_stream_transport_factory tcp;
    php_stream_transport_factory udp;
    php_stream_transport_factory _unix;
    php_stream_transport_factory udg;
    php_stream_transport_factory ssl;
    php_stream_transport_factory tls;
} ori_factory = {
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

static std::vector<std::string> unsafe_functions {
    "pcntl_fork",
    "pcntl_rfork",
    "pcntl_wait",
    "pcntl_waitpid",
    "pcntl_sigtimedwait",
    "pcntl_sigwaitinfo",
};

#if defined(HAVE_PUTENV) && defined(SW_THREAD)
static std::unordered_map<std::string, std::string> swoole_runtime_environ;
#endif

static const zend_function_entry swoole_runtime_methods[] = {
    PHP_ME(swoole_runtime, enableCoroutine, arginfo_class_Swoole_Runtime_enableCoroutine, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_runtime, getHookFlags, arginfo_class_Swoole_Runtime_getHookFlags, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_runtime, setHookFlags, arginfo_class_Swoole_Runtime_setHookFlags, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};
// clang-format on

static php_stream_wrapper ori_php_plain_files_wrapper;
static php_stream_ops ori_php_stream_stdio_ops;

static void hook_func(const char *name,
                      size_t l_name,
                      zif_handler handler = nullptr,
                      zend_internal_arg_info *arg_info = nullptr);
static void unhook_func(const char *name, size_t l_name);

static zend_internal_arg_info *get_arginfo(const char *name, size_t l_name) {
    auto *zf = static_cast<zend_function *>(zend_hash_str_find_ptr(EG(function_table), name, l_name));
    if (zf == nullptr) {
        return nullptr;
    }
    return zf->internal_function.arg_info;
}

static zend_internal_arg_info *copy_arginfo(const zend_function *zf, zend_internal_arg_info *_arg_info) {
    uint32_t num_args = zf->internal_function.num_args + 1;
    zend_internal_arg_info *arg_info = _arg_info - 1;

    auto new_arg_info = static_cast<zend_internal_arg_info *>(pemalloc(sizeof(zend_internal_arg_info) * num_args, 1));
    memcpy(new_arg_info, arg_info, sizeof(zend_internal_arg_info) * num_args);

    if (zf->internal_function.fn_flags & ZEND_ACC_VARIADIC) {
        num_args++;
    }

    for (uint32_t i = 0; i < num_args; i++) {
        if (ZEND_TYPE_HAS_LIST(arg_info[i].type)) {
            auto old_list = ZEND_TYPE_LIST(arg_info[i].type);
            auto *new_list = static_cast<zend_type_list *>(pemalloc(ZEND_TYPE_LIST_SIZE(old_list->num_types), 1));
            memcpy(new_list, old_list, ZEND_TYPE_LIST_SIZE(old_list->num_types));
            ZEND_TYPE_SET_PTR(new_arg_info[i].type, new_list);

            zend_type *list_type;
            ZEND_TYPE_LIST_FOREACH(new_list, list_type) {
                zend_string *name = zend_string_dup(ZEND_TYPE_NAME(*list_type), true);
                ZEND_TYPE_SET_PTR(*list_type, name);
            }
            ZEND_TYPE_LIST_FOREACH_END();
        } else if (ZEND_TYPE_HAS_NAME(arg_info[i].type)) {
            zend_string *name = zend_string_dup(ZEND_TYPE_NAME(arg_info[i].type), true);
            ZEND_TYPE_SET_PTR(new_arg_info[i].type, name);
        }
    }

    return new_arg_info + 1;
}

static void free_arg_info(const zend_internal_function *function) {
    if ((function->fn_flags & (ZEND_ACC_HAS_RETURN_TYPE | ZEND_ACC_HAS_TYPE_HINTS)) && function->arg_info) {
        uint32_t num_args = function->num_args + 1;
        zend_internal_arg_info *arg_info = function->arg_info - 1;

        if (function->fn_flags & ZEND_ACC_VARIADIC) {
            num_args++;
        }
        for (uint32_t i = 0; i < num_args; i++) {
            zend_type_release(arg_info[i].type, true);
        }
        free(arg_info);
    }
}

#define SW_HOOK_FUNC(f) hook_func(ZEND_STRL(#f), PHP_FN(swoole_##f))
#define SW_UNHOOK_FUNC(f) unhook_func(ZEND_STRL(#f))
#define SW_HOOK_WITH_NATIVE_FUNC(f)                                                                                    \
    hook_func(ZEND_STRL(#f), PHP_FN(swoole_native_##f), get_arginfo(ZEND_STRL("swoole_native_" #f)))
#define SW_HOOK_WITH_PHP_FUNC(f) hook_func(ZEND_STRL(#f))

#define SW_HOOK_LIBRARY_FE(name, arg_info)                                                                             \
    ZEND_RAW_FENTRY("swoole_hook_" #name, PHP_FN(swoole_user_func_handler), arg_info, 0)

static SW_THREAD_LOCAL int runtime_hook_flags = 0;
static SW_THREAD_LOCAL zend_array *tmp_function_table = nullptr;
static SW_THREAD_LOCAL std::unordered_map<std::string, zend_class_entry *> child_class_entries;
static zend::ConcurrencyHashMap<std::string, zif_handler> ori_func_handlers(nullptr);
static zend::ConcurrencyHashMap<std::string, zend_internal_arg_info *> ori_func_arg_infos(nullptr);

SW_EXTERN_C_BEGIN
#include "ext/standard/file.h"
#include "thirdparty/php/streams/plain_wrapper.c"
#undef close
SW_EXTERN_C_END

void php_swoole_runtime_minit(int module_number) {
    SW_INIT_CLASS_ENTRY_BASE(swoole_runtime, "Swoole\\Runtime", nullptr, swoole_runtime_methods, nullptr);
    SW_SET_CLASS_CREATE(swoole_runtime, sw_zend_create_object_deny);

    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_TCP", PHPCoroutine::HOOK_TCP);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_UDP", PHPCoroutine::HOOK_UDP);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_UNIX", PHPCoroutine::HOOK_UNIX);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_UDG", PHPCoroutine::HOOK_UDG);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_SSL", PHPCoroutine::HOOK_SSL);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_TLS", PHPCoroutine::HOOK_TLS);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_STREAM_FUNCTION", PHPCoroutine::HOOK_STREAM_FUNCTION);
    // backward compatibility
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_STREAM_SELECT", PHPCoroutine::HOOK_STREAM_FUNCTION);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_FILE", PHPCoroutine::HOOK_FILE);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_STDIO", PHPCoroutine::HOOK_STDIO);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_SLEEP", PHPCoroutine::HOOK_SLEEP);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_PROC", PHPCoroutine::HOOK_PROC);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_CURL", PHPCoroutine::HOOK_CURL);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_NATIVE_CURL", PHPCoroutine::HOOK_NATIVE_CURL);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_BLOCKING_FUNCTION", PHPCoroutine::HOOK_BLOCKING_FUNCTION);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_SOCKETS", PHPCoroutine::HOOK_SOCKETS);
#ifdef SW_USE_PGSQL
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_PDO_PGSQL", PHPCoroutine::HOOK_PDO_PGSQL);
#endif
#ifdef SW_USE_ODBC
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_PDO_ODBC", PHPCoroutine::HOOK_PDO_ODBC);
#endif
#ifdef SW_USE_ORACLE
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_PDO_ORACLE", PHPCoroutine::HOOK_PDO_ORACLE);
#endif
#ifdef SW_USE_SQLITE
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_PDO_SQLITE", PHPCoroutine::HOOK_PDO_SQLITE);
#endif
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HOOK_ALL", PHPCoroutine::HOOK_ALL);
#ifdef SW_USE_CURL
    swoole_native_curl_minit(module_number);
#endif
    swoole_proc_open_init(module_number);
}

struct PhpFunc {
    zend_function *function;
    zif_handler ori_handler;
    zend_internal_arg_info *ori_arg_info;
    zend_internal_arg_info *arg_info_copy;
    uint32_t ori_fn_flags;
    uint32_t ori_num_args;
    zend::Callable *fci_cache;
    zval name;
};

void php_swoole_runtime_rinit() {
    tmp_function_table = static_cast<zend_array *>(emalloc(sizeof(zend_array)));
    zend_hash_init(tmp_function_table, 8, nullptr, nullptr, 0);

#if defined(HAVE_PUTENV) && defined(SW_THREAD)
    /**
     * There are issues with the implementation of putenv in PHP,
     * which can lead to memory invalid read in multi-thread environment.
     */
    SW_HOOK_FUNC(putenv);
#endif

    if (!sw_is_main_thread()) {
        return;
    }

#if PHP_VERSION_ID >= 80400
    SW_HOOK_FUNC(exit);
#endif

    HashTable *xport_hash = php_stream_xport_get_hash();
    ori_factory.tcp = (php_stream_transport_factory) zend_hash_str_find_ptr(xport_hash, ZEND_STRL("tcp"));
    ori_factory.udp = (php_stream_transport_factory) zend_hash_str_find_ptr(xport_hash, ZEND_STRL("udp"));
    ori_factory._unix = (php_stream_transport_factory) zend_hash_str_find_ptr(xport_hash, ZEND_STRL("unix"));
    ori_factory.udg = (php_stream_transport_factory) zend_hash_str_find_ptr(xport_hash, ZEND_STRL("udg"));
    ori_factory.ssl = (php_stream_transport_factory) zend_hash_str_find_ptr(xport_hash, ZEND_STRL("ssl"));
    ori_factory.tls = (php_stream_transport_factory) zend_hash_str_find_ptr(xport_hash, ZEND_STRL("tls"));

    memcpy(&ori_php_plain_files_wrapper, &php_plain_files_wrapper, sizeof(php_plain_files_wrapper));
    memcpy(&ori_php_stream_stdio_ops, &php_stream_stdio_ops, sizeof(php_stream_stdio_ops));
}

void php_swoole_runtime_rshutdown() {
    if (sw_is_main_thread()) {
        PHPCoroutine::disable_hook();
        ori_func_handlers.clear();
        ori_func_arg_infos.clear();
    }

    void *ptr;
    ZEND_HASH_FOREACH_PTR(tmp_function_table, ptr) {
        auto *rf = static_cast<PhpFunc *>(ptr);
        /**
         * php library function
         */
        if (rf->fci_cache) {
            zval_dtor(&rf->name);
            sw_callable_free(rf->fci_cache);
        }
        if (sw_is_main_thread()) {
            rf->function->internal_function.handler = rf->ori_handler;
            rf->function->internal_function.arg_info = rf->ori_arg_info;
        }
        efree(rf);
    }
    ZEND_HASH_FOREACH_END();

    zend_hash_destroy(tmp_function_table);
    efree(tmp_function_table);
    tmp_function_table = nullptr;

    clear_class_entries();
}

void php_swoole_runtime_mshutdown() {
#ifdef SW_USE_CURL
    swoole_native_curl_mshutdown();
#endif
}

static inline char *parse_ip_address_ex(const char *str, size_t str_len, int *portno, int get_err, zend_string **err) {
    char *colon;
    char *host = nullptr;

    if (*(str) == '[' && str_len > 1) {
        /* IPV6 notation to specify raw address with port (i.e. [fe80::1]:80) */
        char *p = (char *) memchr(str + 1, ']', str_len - 2);
        if (!p || *(p + 1) != ':') {
            if (get_err) {
                *err = strpprintf(0, "Failed to parse IPv6 address \"%s\"", str);
            }
            return nullptr;
        }
        *portno = atoi(p + 2);
        return estrndup(str + 1, p - str - 1);
    }
    if (str_len) {
        colon = (char *) memchr(str, ':', str_len - 1);
    } else {
        colon = nullptr;
    }
    if (colon) {
        *portno = atoi(colon + 1);
        host = estrndup(str, colon - str);
    } else {
        if (get_err) {
            *err = strpprintf(0, "Failed to parse address \"%s\"", str);
        }
        return nullptr;
    }

    return host;
}

static php_stream_size_t socket_write(php_stream *stream, const char *buf, size_t count) {
    ssize_t didwrite = -1;
    std::shared_ptr<Socket> sock;

    auto *abstract = static_cast<NetStream *>(stream->abstract);
    if (UNEXPECTED(!abstract || !abstract->socket)) {
        goto _exit;
    }
    sock = abstract->socket;

    if (abstract->blocking) {
        didwrite = sock->send_all(buf, count);
    } else {
        didwrite = sock->get_socket()->send(buf, count, 0);
        sock->set_err(errno);
    }

    if (didwrite < 0 || (size_t) didwrite != count) {
        /* we do not expect the outer layer to continue to call the `send` syscall in a loop
         * and `didwrite` is meaningless if it failed */
        didwrite = -1;
        abstract->stream.timeout_event = (sock->errCode == ETIMEDOUT);
        php_error_docref(nullptr,
                         E_NOTICE,
                         "Send of " ZEND_LONG_FMT " bytes failed with errno=%d %s",
                         (zend_long) count,
                         sock->errCode,
                         sock->errMsg);
    } else {
        php_stream_notify_progress_increment(PHP_STREAM_CONTEXT(stream), didwrite, 0);
    }

    if (didwrite < 0) {
        if (sock->errCode == ETIMEDOUT || sock->get_socket()->catch_write_error(sock->errCode) == SW_WAIT) {
            didwrite = 0;
        } else {
            stream->eof = 1;
        }
    } else if (didwrite == 0) {
        stream->eof = 1;
    }

_exit:
    return didwrite;
}

static php_stream_size_t socket_read(php_stream *stream, char *buf, size_t count) {
    std::shared_ptr<Socket> sock;
    ssize_t nr_bytes = -1;

    auto *abstract = static_cast<NetStream *>(stream->abstract);
    if (UNEXPECTED(!abstract || !abstract->socket)) {
        goto _exit;
    }
    sock = abstract->socket;

    if (abstract->blocking) {
        nr_bytes = sock->recv(buf, count);
    } else {
        nr_bytes = sock->get_socket()->recv(buf, count, 0);
        sock->set_err(errno);
    }

    if (nr_bytes > 0) {
        php_stream_notify_progress_increment(PHP_STREAM_CONTEXT(stream), nr_bytes, 0);
    }

    if (nr_bytes < 0) {
        if (sock->errCode == ETIMEDOUT || sock->get_socket()->catch_read_error(sock->errCode) == SW_WAIT) {
            nr_bytes = 0;
        } else {
            stream->eof = 1;
        }
    } else if (nr_bytes == 0) {
        stream->eof = 1;
    }

_exit:
    return nr_bytes;
}

static int socket_flush(php_stream *stream) {
    return 0;
}

static int socket_close(php_stream *stream, int close_handle) {
    const auto *abstract = static_cast<NetStream *>(stream->abstract);
    if (UNEXPECTED(!abstract)) {
        return FAILURE;
    }
    /** set it null immediately */
    stream->abstract = nullptr;
    /**
     * it's always successful (even if the destructor rule is violated)
     * every calls passes through the hook function in PHP
     * so there is unnecessary to worry about the null pointer.
     */
    if (abstract->socket) {
        abstract->socket->close();
    }
    delete abstract;
    return SUCCESS;
}

enum {
    STREAM_XPORT_OP_BIND,
    STREAM_XPORT_OP_CONNECT,
    STREAM_XPORT_OP_LISTEN,
    STREAM_XPORT_OP_ACCEPT,
    STREAM_XPORT_OP_CONNECT_ASYNC,
    STREAM_XPORT_OP_GET_NAME,
    STREAM_XPORT_OP_GET_PEER_NAME,
    STREAM_XPORT_OP_RECV,
    STREAM_XPORT_OP_SEND,
    STREAM_XPORT_OP_SHUTDOWN,
};

enum { STREAM_XPORT_CRYPTO_OP_SETUP, STREAM_XPORT_CRYPTO_OP_ENABLE };

static int socket_cast(php_stream *stream, int castas, void **ret) {
    const auto *abstract = static_cast<NetStream *>(stream->abstract);
    if (UNEXPECTED(!abstract || !abstract->socket)) {
        return FAILURE;
    }
    const std::shared_ptr<Socket> sock = abstract->socket;
    switch (castas) {
    case PHP_STREAM_AS_STDIO:
        if (ret) {
            *reinterpret_cast<FILE **>(ret) = fdopen(sock->get_fd(), stream->mode);
            if (*ret) {
                return SUCCESS;
            }
            return FAILURE;
        }
        return SUCCESS;
    case PHP_STREAM_AS_FD_FOR_SELECT:
    case PHP_STREAM_AS_FD:
    case PHP_STREAM_AS_SOCKETD:
        if (ret) *reinterpret_cast<php_socket_t *>(ret) = sock->get_fd();
        return SUCCESS;
    default:
        return FAILURE;
    }
}

static int socket_stat(php_stream *stream, php_stream_statbuf *ssb) {
    const auto *abstract = static_cast<NetStream *>(stream->abstract);
    if (UNEXPECTED(!abstract)) {
        return FAILURE;
    }
    if (UNEXPECTED(!abstract->socket)) {
        return FAILURE;
    }
    return zend_fstat(abstract->socket->get_fd(), &ssb->sb);
}

static inline int socket_connect(php_stream *stream, Socket *sock, php_stream_xport_param *xparam) {
    char *host = nullptr, *bindto = nullptr;
    int portno = 0, bindport = 0;
    int ret = 0;
    zval *tmpzval = nullptr;
    char *ip_address = nullptr;

    if (UNEXPECTED(sock->get_fd() < 0)) {
        return FAILURE;
    }

    if (sock->get_socket()->is_inet()) {
        ip_address = parse_ip_address_ex(
            xparam->inputs.name, xparam->inputs.namelen, &portno, xparam->want_errortext, &xparam->outputs.error_text);
        host = ip_address;
        if (sock->get_sock_type() == SOCK_STREAM) {
            sock->get_socket()->set_tcp_nodelay();
        }
    } else {
        host = xparam->inputs.name;
    }
    if (host == nullptr) {
        return FAILURE;
    }
    ON_SCOPE_EXIT {
        if (ip_address) {
            efree(ip_address);
        }
    };
    if (PHP_STREAM_CONTEXT(stream) &&
        (tmpzval = php_stream_context_get_option(PHP_STREAM_CONTEXT(stream), "socket", "bindto")) != nullptr) {
        if (Z_TYPE_P(tmpzval) != IS_STRING) {
            if (xparam->want_errortext) {
                xparam->outputs.error_text = strpprintf(0, "local_addr context option is not a string.");
            }
            return FAILURE;
        }
        bindto = parse_ip_address_ex(
            Z_STRVAL_P(tmpzval), Z_STRLEN_P(tmpzval), &bindport, xparam->want_errortext, &xparam->outputs.error_text);
        if (bindto == nullptr) {
            return FAILURE;
        }
        if (strcmp(bindto, "0") == 0 || bindto[0] == '\0') {
            efree(bindto);
            bindto = nullptr;
        }
        ON_SCOPE_EXIT {
            if (bindto) {
                efree(bindto);
            }
        };
        if (!sock->bind(bindto ? bindto : "0.0.0.0", bindport)) {
            return FAILURE;
        }
    }

    if (xparam->inputs.timeout) {
        sock->set_timeout(xparam->inputs.timeout, SW_TIMEOUT_CONNECT);
    }
    if (sock->connect(host, portno) == false) {
        xparam->outputs.error_code = sock->errCode;
        if (sock->errMsg) {
            xparam->outputs.error_text = zend_string_init(sock->errMsg, strlen(sock->errMsg), false);
        }
        ret = -1;
    }
    return ret;
}

static inline int socket_bind(php_stream *stream, Socket *sock, php_stream_xport_param *xparam STREAMS_DC) {
    char *host = nullptr;
    int portno = 0;
    char *ip_address = nullptr;

    if (sock->get_socket()->is_inet()) {
        ip_address = parse_ip_address_ex(
            xparam->inputs.name, xparam->inputs.namelen, &portno, xparam->want_errortext, &xparam->outputs.error_text);
        host = ip_address;
    } else {
        host = xparam->inputs.name;
    }
    if (host == nullptr) {
        sock->set_err(EINVAL);
        return -1;
    }
    int ret = sock->bind(host, portno) ? 0 : -1;
    if (ip_address) {
        efree(ip_address);
    }
    return ret;
}

static inline int socket_accept(php_stream *stream, Socket *sock, php_stream_xport_param *xparam STREAMS_DC) {
    int tcp_nodelay = 0;
    zval *tmpzval = nullptr;

    xparam->outputs.client = nullptr;

    if ((nullptr != PHP_STREAM_CONTEXT(stream)) &&
        (tmpzval = php_stream_context_get_option(PHP_STREAM_CONTEXT(stream), "socket", "tcp_nodelay")) != nullptr &&
        zval_is_true(tmpzval)) {
        tcp_nodelay = 1;
    }

    zend_string **textaddr = xparam->want_textaddr ? &xparam->outputs.textaddr : nullptr;
    sockaddr **addr = xparam->want_addr ? &xparam->outputs.addr : nullptr;
    socklen_t *addrlen = xparam->want_addr ? &xparam->outputs.addrlen : nullptr;

    timeval *timeout = xparam->inputs.timeout;
    zend_string **error_string = xparam->want_errortext ? &xparam->outputs.error_text : nullptr;
    int *error_code = &xparam->outputs.error_code;

    int error = 0;
    php_sockaddr_storage sa;
    socklen_t sl = sizeof(sa);

    if (timeout) {
        sock->set_timeout(timeout, SW_TIMEOUT_READ);
    }

    std::shared_ptr<Socket> clisock(sock->accept());

#ifdef SW_USE_OPENSSL
    if (clisock != nullptr && clisock->ssl_is_enable()) {
        if (!clisock->ssl_handshake()) {
            sock->errCode = clisock->errCode;
            clisock.reset();
        }
    }
#endif

    if (clisock == nullptr) {
        error = sock->errCode;
        if (error_code) {
            *error_code = error;
        }
        if (error_string) {
            *error_string = php_socket_error_str(error);
        }
        return FAILURE;
    } else {
        php_network_populate_name_from_sockaddr((sockaddr *) &sa, sl, textaddr, addr, addrlen);
#ifdef TCP_NODELAY
        if (tcp_nodelay) {
            clisock->get_socket()->set_tcp_nodelay(tcp_nodelay);
        }
#endif
        auto abstract = new NetStream();
        abstract->socket = clisock;
        abstract->blocking = true;

        xparam->outputs.client = php_stream_alloc_rel(stream->ops, abstract, nullptr, "r+");
        if (xparam->outputs.client) {
            xparam->outputs.client->ctx = stream->ctx;
            if (stream->ctx) {
                GC_ADDREF(stream->ctx);
            }
        }
        return SUCCESS;
    }
}

static inline int socket_recvfrom(
    Socket *sock, char *buf, size_t buflen, zend_string **textaddr, struct sockaddr **addr, socklen_t *addrlen) {
    int ret;
    int want_addr = textaddr || addr;

    if (want_addr) {
        php_sockaddr_storage sa;
        socklen_t sl = sizeof(sa);
        ret = sock->recvfrom(buf, buflen, (struct sockaddr *) &sa, &sl);
        if (sl) {
            php_network_populate_name_from_sockaddr((struct sockaddr *) &sa, sl, textaddr, addr, addrlen);
        } else {
            if (textaddr) {
                *textaddr = ZSTR_EMPTY_ALLOC();
            }
            if (addr) {
                *addr = nullptr;
                *addrlen = 0;
            }
        }
    } else {
        ret = sock->recv(buf, buflen);
    }

    return ret;
}

static inline int socket_sendto(
    Socket *sock, const char *buf, size_t buflen, struct sockaddr *addr, socklen_t addrlen) {
    if (addr) {
        return sendto(sock->get_fd(), buf, buflen, 0, addr, addrlen);
    } else {
        return sock->send(buf, buflen);
    }
}

#ifdef SW_USE_OPENSSL

#define GET_VER_OPT(name)                                                                                              \
    (PHP_STREAM_CONTEXT(stream) &&                                                                                     \
     (val = php_stream_context_get_option(PHP_STREAM_CONTEXT(stream), "ssl", name)) != nullptr)
#define GET_VER_OPT_STRING(name, str)                                                                                  \
    if (GET_VER_OPT(name)) {                                                                                           \
        convert_to_string_ex(val);                                                                                     \
        str = Z_STRVAL_P(val);                                                                                         \
    }
#define GET_VER_OPT_LONG(name, num)                                                                                    \
    if (GET_VER_OPT(name)) {                                                                                           \
        convert_to_long_ex(val);                                                                                       \
        num = Z_LVAL_P(val);                                                                                           \
    }

static int socket_setup_crypto(php_stream *stream, Socket *sock, php_stream_xport_crypto_param *cparam STREAMS_DC) {
    return 0;
}

static int socket_xport_crypto_setup(php_stream *stream) {
    php_stream_xport_crypto_param param = {};

    param.op = (decltype(param.op)) STREAM_XPORT_CRYPTO_OP_SETUP;
    param.inputs.method = (php_stream_xport_crypt_method_t) 0;
    param.inputs.session = nullptr;

    int ret = php_stream_set_option(stream, PHP_STREAM_OPTION_CRYPTO_API, 0, &param);

    if (ret == PHP_STREAM_OPTION_RETURN_OK) {
        return param.outputs.returncode;
    }

    php_error_docref("streams.crypto", E_WARNING, "this stream does not support SSL/crypto");

    return ret;
}

static int socket_xport_crypto_enable(php_stream *stream, int activate) {
    php_stream_xport_crypto_param param = {};

    param.op = (decltype(param.op)) STREAM_XPORT_CRYPTO_OP_ENABLE;
    param.inputs.activate = activate;

    int ret = php_stream_set_option(stream, PHP_STREAM_OPTION_CRYPTO_API, 0, &param);

    if (ret == PHP_STREAM_OPTION_RETURN_OK) {
        return param.outputs.returncode;
    }

    php_error_docref("streams.crypto", E_WARNING, "this stream does not support SSL/crypto");

    return ret;
}

static bool php_openssl_capture_peer_certs(php_stream *stream, Socket *sslsock) {
    zval *val;

    std::string peer_cert = sslsock->ssl_get_peer_cert();
    if (peer_cert.empty()) {
        return false;
    }

    zval argv[1];
    ZVAL_STRINGL(&argv[0], peer_cert.c_str(), peer_cert.length());
    auto retval = zend::function::call("openssl_x509_read", 1, argv);
    php_stream_context_set_option(PHP_STREAM_CONTEXT(stream), "ssl", "peer_certificate", &retval.value);
    zval_dtor(&argv[0]);

    if (nullptr !=
            (val = php_stream_context_get_option(PHP_STREAM_CONTEXT(stream), "ssl", "capture_peer_cert_chain")) &&
        zend_is_true(val)) {
        zval arr;
        auto chain = sslsock->get_socket()->ssl_get_peer_cert_chain(INT_MAX);

        if (!chain.empty()) {
            array_init(&arr);
            for (auto &cert : chain) {
                zval _argv[1];
                ZVAL_STRINGL(&_argv[0], cert.c_str(), cert.length());
                auto _retval = zend::function::call("openssl_x509_read", 1, _argv);
                zval_add_ref(&_retval.value);
                add_next_index_zval(&arr, &_retval.value);
                zval_dtor(&_argv[0]);
            }
        } else {
            ZVAL_NULL(&arr);
        }

        php_stream_context_set_option(PHP_STREAM_CONTEXT(stream), "ssl", "peer_certificate_chain", &arr);
        zval_ptr_dtor(&arr);
    }

    return true;
}

static int socket_enable_crypto(php_stream *stream, Socket *sock, php_stream_xport_crypto_param *cparam STREAMS_DC) {
    php_stream_context *context = PHP_STREAM_CONTEXT(stream);
    if (cparam->inputs.activate && !sock->ssl_is_available()) {
        sock->enable_ssl_encrypt();
        if (!socket_ssl_set_options(sock, context)) {
            return -1;
        }
        if (!sock->ssl_handshake()) {
            return -1;
        }
    } else if (!cparam->inputs.activate && sock->ssl_is_available()) {
        sock->ssl_shutdown();
        return -1;
    }

    if (context && sock->ssl_is_available()) {
        zval *val = php_stream_context_get_option(context, "ssl", "capture_peer_cert");
        if (val && zend_is_true(val) && !php_openssl_capture_peer_certs(stream, sock)) {
            return -1;
        }
    }

    return 1;
}
#endif

static inline int socket_xport_api(php_stream *stream, Socket *sock, php_stream_xport_param *xparam STREAMS_DC) {
    static const int shutdown_how[] = {SHUT_RD, SHUT_WR, SHUT_RDWR};

    switch (xparam->op) {
    case STREAM_XPORT_OP_LISTEN: {
        xparam->outputs.returncode = sock->listen(xparam->inputs.backlog) ? 0 : -1;
        break;
    }
    case STREAM_XPORT_OP_CONNECT:
    case STREAM_XPORT_OP_CONNECT_ASYNC:
        xparam->outputs.returncode = socket_connect(stream, sock, xparam);
#ifdef SW_USE_OPENSSL
        if (sock->ssl_is_enable() &&
            (socket_xport_crypto_setup(stream) < 0 || socket_xport_crypto_enable(stream, 1) < 0)) {
            xparam->outputs.returncode = -1;
        }
#endif
        break;
    case STREAM_XPORT_OP_BIND: {
        if (sock->get_sock_domain() != AF_UNIX) {
            zval *tmpzval = nullptr;
            php_stream_context *ctx = PHP_STREAM_CONTEXT(stream);
            if (!ctx) {
                break;
            }
#ifdef SO_REUSEADDR
            sock->get_socket()->set_reuse_addr();
#endif

#ifdef IPV6_V6ONLY
            if ((tmpzval = php_stream_context_get_option(ctx, "socket", "ipv6_v6only")) != nullptr &&
                zval_is_true(tmpzval)) {
                sock->get_socket()->set_option(IPPROTO_IPV6, IPV6_V6ONLY, 1);
            }
#endif

#ifdef SO_REUSEPORT
            if ((tmpzval = php_stream_context_get_option(ctx, "socket", "so_reuseport")) != nullptr &&
                zval_is_true(tmpzval)) {
                sock->get_socket()->set_reuse_port();
            }
#endif

#ifdef SO_BROADCAST
            if ((tmpzval = php_stream_context_get_option(ctx, "socket", "so_broadcast")) != nullptr &&
                zval_is_true(tmpzval)) {
                sock->set_option(SOL_SOCKET, SO_BROADCAST, 1);
            }
#endif
        }
        xparam->outputs.returncode = socket_bind(stream, sock, xparam STREAMS_CC);
        break;
    }
    case STREAM_XPORT_OP_ACCEPT:
        xparam->outputs.returncode = socket_accept(stream, sock, xparam STREAMS_CC);
        break;
    case STREAM_XPORT_OP_GET_NAME:
        xparam->outputs.returncode =
            php_network_get_sock_name(sock->get_fd(),
                                      xparam->want_textaddr ? &xparam->outputs.textaddr : nullptr,
                                      xparam->want_addr ? &xparam->outputs.addr : nullptr,
                                      xparam->want_addr ? &xparam->outputs.addrlen : nullptr);
        break;
    case STREAM_XPORT_OP_GET_PEER_NAME:
        xparam->outputs.returncode =
            php_network_get_peer_name(sock->get_fd(),
                                      xparam->want_textaddr ? &xparam->outputs.textaddr : nullptr,
                                      xparam->want_addr ? &xparam->outputs.addr : nullptr,
                                      xparam->want_addr ? &xparam->outputs.addrlen : nullptr);
        break;

    case STREAM_XPORT_OP_SEND:
        if ((xparam->inputs.flags & STREAM_OOB) == STREAM_OOB) {
            php_swoole_error(E_WARNING, "STREAM_OOB flags is not supports");
            xparam->outputs.returncode = -1;
            break;
        }
        xparam->outputs.returncode =
            socket_sendto(sock, xparam->inputs.buf, xparam->inputs.buflen, xparam->inputs.addr, xparam->inputs.addrlen);
        if (xparam->outputs.returncode == -1) {
            char *err = php_socket_strerror(php_socket_errno(), nullptr, 0);
            php_error_docref(nullptr, E_WARNING, "%s\n", err);
            efree(err);
        }
        break;

    case STREAM_XPORT_OP_RECV:
        if ((xparam->inputs.flags & STREAM_OOB) == STREAM_OOB) {
            php_swoole_error(E_WARNING, "STREAM_OOB flags is not supports");
            xparam->outputs.returncode = -1;
            break;
        }
        if ((xparam->inputs.flags & STREAM_PEEK) == STREAM_PEEK) {
            xparam->outputs.returncode = sock->peek(xparam->inputs.buf, xparam->inputs.buflen);
        } else {
            xparam->outputs.returncode = socket_recvfrom(sock,
                                                         xparam->inputs.buf,
                                                         xparam->inputs.buflen,
                                                         xparam->want_textaddr ? &xparam->outputs.textaddr : nullptr,
                                                         xparam->want_addr ? &xparam->outputs.addr : nullptr,
                                                         xparam->want_addr ? &xparam->outputs.addrlen : nullptr);
        }
        break;
    case STREAM_XPORT_OP_SHUTDOWN:
        xparam->outputs.returncode = sock->shutdown(shutdown_how[xparam->how]);
        break;
    default:
#ifdef SW_DEBUG
        php_swoole_fatal_error(E_WARNING, "socket_xport_api: unsupported option %d", xparam->op);
#endif
        break;
    }
    return PHP_STREAM_OPTION_RETURN_OK;
}

static int socket_set_option(php_stream *stream, int option, int value, void *ptrparam) {
    auto *abstract = static_cast<NetStream *>(stream->abstract);
    if (UNEXPECTED(!abstract || !abstract->socket)) {
        return PHP_STREAM_OPTION_RETURN_ERR;
    }
    std::shared_ptr<Socket> sock_wrapped = abstract->socket;
    auto sock = sock_wrapped.get();
    switch (option) {
    case PHP_STREAM_OPTION_BLOCKING:
        if (abstract->blocking == (bool) value) {
            break;
        }
        abstract->blocking = (bool) value;
        break;
    case PHP_STREAM_OPTION_XPORT_API: {
        return socket_xport_api(stream, sock, (php_stream_xport_param *) ptrparam STREAMS_CC);
    }
    case PHP_STREAM_OPTION_META_DATA_API: {
#ifdef SW_USE_OPENSSL
        SSL *ssl = sock->get_socket() ? sock->get_socket()->ssl : nullptr;
        if (ssl) {
            zval tmp;
            const char *proto_str;

            array_init(&tmp);
            switch (SSL_version(ssl)) {
#ifdef TLS1_3_VERSION
            case TLS1_3_VERSION:
                proto_str = "TLSv1.3";
                break;
#endif
#ifdef TLS1_2_VERSION
            case TLS1_2_VERSION:
                proto_str = "TLSv1.2";
                break;
#endif
#ifdef TLS1_1_VERSION
            case TLS1_1_VERSION:
                proto_str = "TLSv1.1";
                break;
#endif
            case TLS1_VERSION:
                proto_str = "TLSv1";
                break;
#ifdef SSL3_VERSION
            case SSL3_VERSION:
                proto_str = "SSLv3";
                break;
#endif
            default:
                proto_str = "UNKNOWN";
                break;
            }

            const auto cipher = SSL_get_current_cipher(ssl);
            add_assoc_string(&tmp, "protocol", proto_str);
            add_assoc_string(&tmp, "cipher_name", SSL_CIPHER_get_name(cipher));
            add_assoc_long(&tmp, "cipher_bits", SSL_CIPHER_get_bits(cipher, nullptr));
            add_assoc_string(&tmp, "cipher_version", SSL_CIPHER_get_version(cipher));
            add_assoc_zval((zval *) ptrparam, "crypto", &tmp);
        }
#endif
        add_assoc_bool((zval *) ptrparam, "timed_out", sock->errCode == ETIMEDOUT);
        add_assoc_bool((zval *) ptrparam, "eof", stream->eof);
        add_assoc_bool((zval *) ptrparam, "blocked", true);
        break;
    }
    case PHP_STREAM_OPTION_READ_TIMEOUT: {
        abstract->socket->set_timeout(static_cast<timeval *>(ptrparam), SW_TIMEOUT_READ);
        break;
    }
#ifdef SW_USE_OPENSSL
    case PHP_STREAM_OPTION_CRYPTO_API: {
        auto *cparam = static_cast<php_stream_xport_crypto_param *>(ptrparam);
        switch (cparam->op) {
        case STREAM_XPORT_CRYPTO_OP_SETUP:
            cparam->outputs.returncode = socket_setup_crypto(stream, sock, cparam STREAMS_CC);
            return PHP_STREAM_OPTION_RETURN_OK;
        case STREAM_XPORT_CRYPTO_OP_ENABLE:
            cparam->outputs.returncode = socket_enable_crypto(stream, sock, cparam STREAMS_CC);
            return PHP_STREAM_OPTION_RETURN_OK;
        default:
            /* never here */
            SW_ASSERT(0);
            break;
        }
        break;
    }
#endif
    case PHP_STREAM_OPTION_CHECK_LIVENESS: {
        return sock->check_liveness() ? PHP_STREAM_OPTION_RETURN_OK : PHP_STREAM_OPTION_RETURN_ERR;
    }
    case PHP_STREAM_OPTION_READ_BUFFER:
    case PHP_STREAM_OPTION_WRITE_BUFFER: {
        // TODO: read/write buffer
        break;
    }
    default:
#ifdef SW_DEBUG
        php_swoole_fatal_error(E_WARNING, "socket_set_option: unsupported option %d with value %d", option, value);
#endif
        break;
    }
    return PHP_STREAM_OPTION_RETURN_OK;
}

static bool socket_ssl_set_options(Socket *sock, php_stream_context *context) {
    if (context && ZVAL_IS_ARRAY(&context->options)) {
#ifdef SW_USE_OPENSSL
        zval *ztmp;

        if (sock->ssl_is_enable() && php_swoole_array_get_value(Z_ARRVAL_P(&context->options), "ssl", ztmp) &&
            ZVAL_IS_ARRAY(ztmp)) {
            zval zalias;
            array_init(&zalias);
            zend_array *options = Z_ARRVAL_P(ztmp);

            auto add_alias = [&zalias, options](const char *name, const char *alias) {
                zval *ztmp;
                if (php_swoole_array_get_value_ex(options, name, ztmp)) {
                    zend::array_set(&zalias, alias, strlen(alias), ztmp);
                }
            };

            add_alias("peer_name", "ssl_host_name");
            add_alias("verify_peer", "ssl_verify_peer");
            add_alias("allow_self_signed", "ssl_allow_self_signed");
            add_alias("cafile", "ssl_cafile");
            add_alias("capath", "ssl_capath");
            add_alias("local_cert", "ssl_cert_file");
            add_alias("local_pk", "ssl_key_file");
            add_alias("passphrase", "ssl_passphrase");
            add_alias("verify_depth", "ssl_verify_depth");
            add_alias("disable_compression", "ssl_disable_compression");

            bool ret = php_swoole_socket_set_ssl(sock, &zalias);
            zval_dtor(&zalias);
            return ret;
        }
#endif
    }

    return true;
}

static php_stream *socket_create_original(const char *proto,
                                          size_t protolen,
                                          const char *resourcename,
                                          size_t resourcenamelen,
                                          const char *persistent_id,
                                          int options,
                                          int flags,
                                          struct timeval *timeout,
                                          php_stream_context *context STREAMS_DC) {
    php_stream_transport_factory factory = nullptr;
    if (SW_STREQ(proto, protolen, "tcp")) {
        factory = ori_factory.tcp;
    } else if (SW_STREQ(proto, protolen, "ssl")) {
        factory = ori_factory.ssl;
    } else if (SW_STREQ(proto, protolen, "tls")) {
        factory = ori_factory.tls;
    } else if (SW_STREQ(proto, protolen, "unix")) {
        factory = ori_factory._unix;
    } else if (SW_STREQ(proto, protolen, "udp")) {
        factory = ori_factory.udp;
    } else if (SW_STREQ(proto, protolen, "udg")) {
        factory = ori_factory.udg;
    }

    if (factory) {
        return factory(
            proto, protolen, resourcename, resourcenamelen, persistent_id, options, flags, timeout, context STREAMS_CC);
    } else {
        php_swoole_fatal_error(E_WARNING, "unknown protocol '%s'", proto);
        return nullptr;
    }
}

static php_stream *socket_create(const char *proto,
                                 size_t protolen,
                                 const char *resourcename,
                                 size_t resourcenamelen,
                                 const char *persistent_id,
                                 int options,
                                 int flags,
                                 struct timeval *timeout,
                                 php_stream_context *context STREAMS_DC) {
    php_stream *stream = nullptr;
    Socket *sock = nullptr;

    auto co = Coroutine::get_current();
    if (sw_unlikely(co == nullptr)) {
        return socket_create_original(
            proto, protolen, resourcename, resourcenamelen, persistent_id, options, flags, timeout, context STREAMS_CC);
    }

    if (SW_STREQ(proto, protolen, "tcp")) {
        sock = new Socket(resourcename[0] == '[' ? SW_SOCK_TCP6 : SW_SOCK_TCP);
    } else if (SW_STREQ(proto, protolen, "ssl") || SW_STREQ(proto, protolen, "tls")) {
#ifdef SW_USE_OPENSSL
        sock = new Socket(resourcename[0] == '[' ? SW_SOCK_TCP6 : SW_SOCK_TCP);
        sock->enable_ssl_encrypt();
#else
        php_swoole_error(E_WARNING,
                         "you must configure with `--enable-openssl` to support ssl connection when compiling Swoole");
        return nullptr;
#endif
    } else if (SW_STREQ(proto, protolen, "unix")) {
        sock = new Socket(SW_SOCK_UNIX_STREAM);
    } else if (SW_STREQ(proto, protolen, "udp")) {
        sock = new Socket(SW_SOCK_UDP);
    } else if (SW_STREQ(proto, protolen, "udg")) {
        sock = new Socket(SW_SOCK_UNIX_DGRAM);
    } else {
        php_swoole_fatal_error(E_WARNING, "unknown protocol '%s'", proto);
        return nullptr;
    }

    if (UNEXPECTED(sock->get_fd() < 0)) {
    _failed:
        if (!stream) {
            delete sock;
        } else {
            php_stream_close(stream);
        }
        return nullptr;
    }

    sock->set_zero_copy(true);

    auto abstract = new NetStream();
    abstract->socket.reset(sock);
    abstract->stream.socket = sock->get_fd();
    abstract->blocking = true;

    stream = php_stream_alloc_rel(&socket_ops, abstract, persistent_id, "r+");
    if (stream == nullptr) {
        delete abstract;
        goto _failed;
    }

    if (!socket_ssl_set_options(sock, context)) {
        goto _failed;
    }

    return stream;
}

static ZEND_FUNCTION(swoole_display_disabled_function) {
    zend_error(E_WARNING, "%s() has been disabled for security reasons", get_active_function_name());
}

static bool disable_func(const char *name, size_t l_name) {
    auto *rf = static_cast<PhpFunc *>(zend_hash_str_find_ptr(tmp_function_table, name, l_name));
    if (rf) {
        rf->function->internal_function.handler = ZEND_FN(swoole_display_disabled_function);
        return true;
    }

    auto *zf = static_cast<zend_function *>(zend_hash_str_find_ptr(EG(function_table), name, l_name));
    if (zf == nullptr) {
        return false;
    }

    rf = static_cast<PhpFunc *>(emalloc(sizeof(PhpFunc)));
    sw_memset_zero(rf, sizeof(*rf));
    rf->function = zf;
    rf->ori_handler = zf->internal_function.handler;
    rf->ori_arg_info = zf->internal_function.arg_info;
    rf->ori_fn_flags = zf->internal_function.fn_flags;
    rf->ori_num_args = zf->internal_function.num_args;

    zf->internal_function.handler = ZEND_FN(swoole_display_disabled_function);
    zf->internal_function.arg_info = nullptr;
    zf->internal_function.fn_flags &= ~(ZEND_ACC_VARIADIC | ZEND_ACC_HAS_TYPE_HINTS | ZEND_ACC_HAS_RETURN_TYPE);
    zf->internal_function.num_args = 0;

    zend_hash_add_ptr(tmp_function_table, zf->common.function_name, rf);
    return true;
}

static bool enable_func(const char *name, size_t l_name) {
    auto *rf = static_cast<PhpFunc *>(zend_hash_str_find_ptr(tmp_function_table, name, l_name));
    if (!rf) {
        return false;
    }

    rf->function->internal_function.handler = rf->ori_handler;
    rf->function->internal_function.arg_info = rf->ori_arg_info;
    rf->function->internal_function.fn_flags = rf->ori_fn_flags;
    rf->function->internal_function.num_args = rf->ori_num_args;

    return true;
}

bool php_swoole_call_original_handler(const char *name, size_t len, INTERNAL_FUNCTION_PARAMETERS) {
    auto ori_handler = php_swoole_get_original_handler(name, len);
    if (!ori_handler) {
        return false;
    }
    ori_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);

    return true;
}

void PHPCoroutine::disable_unsafe_function() {
    for (auto &f : unsafe_functions) {
        disable_func(f.c_str(), f.length());
    }
}

void PHPCoroutine::enable_unsafe_function() {
    for (auto &f : unsafe_functions) {
        enable_func(f.c_str(), f.length());
    }
}

static void hook_stream_throw_exception(const char *type) {
    zend_throw_exception_ex(
        swoole_exception_ce, SW_ERROR_PHP_FATAL_ERROR, "failed to register `%s` stream transport factory", type);
}

static void hook_remove_stream_flags(uint32_t *flags_ptr) {
    uint32_t flags = *flags_ptr;
    // stream factory
    if (flags & PHPCoroutine::HOOK_TCP) {
        flags ^= PHPCoroutine::HOOK_TCP;
    }
    if (flags & PHPCoroutine::HOOK_UDP) {
        flags ^= PHPCoroutine::HOOK_UDP;
    }
    if (flags & PHPCoroutine::HOOK_UNIX) {
        flags ^= PHPCoroutine::HOOK_UNIX;
    }
    if (flags & PHPCoroutine::HOOK_UDG) {
        flags ^= PHPCoroutine::HOOK_UDG;
    }
    if (flags & PHPCoroutine::HOOK_SSL) {
        flags ^= PHPCoroutine::HOOK_SSL;
    }
    if (flags & PHPCoroutine::HOOK_TLS) {
        flags ^= PHPCoroutine::HOOK_TLS;
    }
    // stream ops
    if (flags & PHPCoroutine::HOOK_FILE) {
        flags ^= PHPCoroutine::HOOK_FILE;
    }
    if (flags & PHPCoroutine::HOOK_STDIO) {
        flags ^= PHPCoroutine::HOOK_STDIO;
    }
    *flags_ptr = flags;
}

static void hook_stream_factory(uint32_t *flags_ptr) {
    uint32_t flags = *flags_ptr;

    if (flags & PHPCoroutine::HOOK_TCP) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_TCP)) {
            if (php_stream_xport_register("tcp", socket_create) != SUCCESS) {
                flags ^= PHPCoroutine::HOOK_TCP;
                hook_stream_throw_exception("tcp");
            }
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_TCP) {
            php_stream_xport_register("tcp", ori_factory.tcp);
        }
    }
    if (flags & PHPCoroutine::HOOK_UDP) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_UDP)) {
            if (php_stream_xport_register("udp", socket_create) != SUCCESS) {
                flags ^= PHPCoroutine::HOOK_UDP;
                hook_stream_throw_exception("udp");
            }
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_UDP) {
            php_stream_xport_register("udp", ori_factory.udp);
        }
    }
    if (flags & PHPCoroutine::HOOK_UNIX) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_UNIX)) {
            if (php_stream_xport_register("unix", socket_create) != SUCCESS) {
                flags ^= PHPCoroutine::HOOK_UNIX;
                hook_stream_throw_exception("unix");
            }
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_UNIX) {
            php_stream_xport_register("unix", ori_factory._unix);
        }
    }
    if (flags & PHPCoroutine::HOOK_UDG) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_UDG)) {
            if (php_stream_xport_register("udg", socket_create) != SUCCESS) {
                flags ^= PHPCoroutine::HOOK_UDG;
                hook_stream_throw_exception("udg");
            }
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_UDG) {
            php_stream_xport_register("udg", ori_factory.udg);
        }
    }
    if (flags & PHPCoroutine::HOOK_SSL) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_SSL)) {
            if (php_stream_xport_register("ssl", socket_create) != SUCCESS) {
                flags ^= PHPCoroutine::HOOK_SSL;
                hook_stream_throw_exception("ssl");
            }
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_SSL) {
            if (ori_factory.ssl != nullptr) {
                php_stream_xport_register("ssl", ori_factory.ssl);
            } else {
                php_stream_xport_unregister("ssl");
            }
        }
    }
    if (flags & PHPCoroutine::HOOK_TLS) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_TLS)) {
            if (php_stream_xport_register("tls", socket_create) != SUCCESS) {
                flags ^= PHPCoroutine::HOOK_TLS;
                hook_stream_throw_exception("tls");
            }
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_TLS) {
            if (ori_factory.tls != nullptr) {
                php_stream_xport_register("tls", ori_factory.tls);
            } else {
                php_stream_xport_unregister("tls");
            }
        }
    }
    *flags_ptr = flags;
}

static void hook_stream_ops(uint32_t flags) {
    // file
    if (flags & PHPCoroutine::HOOK_FILE) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_FILE)) {
            memcpy(&php_plain_files_wrapper, &sw_php_plain_files_wrapper, sizeof(php_plain_files_wrapper));
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_FILE) {
            memcpy(&php_plain_files_wrapper, &ori_php_plain_files_wrapper, sizeof(php_plain_files_wrapper));
        }
    }
    // stdio
    if (flags & PHPCoroutine::HOOK_STDIO) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_STDIO)) {
            memcpy(&php_stream_stdio_ops, &sw_php_stream_stdio_ops, sizeof(php_stream_stdio_ops));
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_STDIO) {
            memcpy(&php_stream_stdio_ops, &ori_php_stream_stdio_ops, sizeof(php_stream_stdio_ops));
        }
    }
}

static void hook_pdo_driver(uint32_t flags) {
#ifdef SW_USE_PGSQL
    if (flags & PHPCoroutine::HOOK_PDO_PGSQL) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_PDO_PGSQL)) {
            swoole_pgsql_set_blocking(false);
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_PDO_PGSQL) {
            swoole_pgsql_set_blocking(true);
        }
    }
#endif
#ifdef SW_USE_ODBC
    if (flags & PHPCoroutine::HOOK_PDO_ODBC) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_PDO_ODBC)) {
            swoole_odbc_set_blocking(false);
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_PDO_ODBC) {
            swoole_odbc_set_blocking(true);
        }
    }
#endif
#ifdef SW_USE_ORACLE
    if (flags & PHPCoroutine::HOOK_PDO_ORACLE) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_PDO_ORACLE)) {
            swoole_oracle_set_blocking(0);
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_PDO_ORACLE) {
            swoole_oracle_set_blocking(1);
        }
    }
#endif
#ifdef SW_USE_SQLITE
    if (flags & PHPCoroutine::HOOK_PDO_SQLITE) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_PDO_SQLITE)) {
            swoole_sqlite_set_blocking(false);
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_PDO_SQLITE) {
            swoole_sqlite_set_blocking(true);
        }
    }
#endif
}

static void hook_all_func(uint32_t flags) {
    // stream func
    if (flags & PHPCoroutine::HOOK_STREAM_FUNCTION) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_STREAM_FUNCTION)) {
            SW_HOOK_FUNC(stream_select);
            SW_HOOK_FUNC(stream_socket_pair);
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_STREAM_FUNCTION) {
            SW_UNHOOK_FUNC(stream_select);
            SW_UNHOOK_FUNC(stream_socket_pair);
        }
    }
    // sleep
    if (flags & PHPCoroutine::HOOK_SLEEP) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_SLEEP)) {
            SW_HOOK_FUNC(sleep);
            SW_HOOK_FUNC(usleep);
            SW_HOOK_FUNC(time_nanosleep);
            SW_HOOK_FUNC(time_sleep_until);
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_SLEEP) {
            SW_UNHOOK_FUNC(sleep);
            SW_UNHOOK_FUNC(usleep);
            SW_UNHOOK_FUNC(time_nanosleep);
            SW_UNHOOK_FUNC(time_sleep_until);
        }
    }
    // proc_open
    if (flags & PHPCoroutine::HOOK_PROC) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_PROC)) {
            SW_HOOK_FUNC(proc_open);
            SW_HOOK_FUNC(proc_close);
            SW_HOOK_FUNC(proc_get_status);
            SW_HOOK_FUNC(proc_terminate);
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_PROC) {
            SW_UNHOOK_FUNC(proc_open);
            SW_UNHOOK_FUNC(proc_close);
            SW_UNHOOK_FUNC(proc_get_status);
            SW_UNHOOK_FUNC(proc_terminate);
        }
    }
    // blocking function
    if (flags & PHPCoroutine::HOOK_BLOCKING_FUNCTION) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_BLOCKING_FUNCTION)) {
            hook_func(ZEND_STRL("gethostbyname"), PHP_FN(swoole_coroutine_gethostbyname));
            SW_HOOK_WITH_PHP_FUNC(exec);
            SW_HOOK_WITH_PHP_FUNC(shell_exec);
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_BLOCKING_FUNCTION) {
            SW_UNHOOK_FUNC(gethostbyname);
            SW_UNHOOK_FUNC(exec);
            SW_UNHOOK_FUNC(shell_exec);
        }
    }
    // ext-sockets
    if (flags & PHPCoroutine::HOOK_SOCKETS) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_SOCKETS)) {
            SW_HOOK_WITH_PHP_FUNC(socket_create);
            SW_HOOK_WITH_PHP_FUNC(socket_create_listen);
            SW_HOOK_WITH_PHP_FUNC(socket_create_pair);
            SW_HOOK_WITH_PHP_FUNC(socket_connect);
            SW_HOOK_WITH_PHP_FUNC(socket_write);
            SW_HOOK_WITH_PHP_FUNC(socket_read);
            SW_HOOK_WITH_PHP_FUNC(socket_send);
            SW_HOOK_WITH_PHP_FUNC(socket_recv);
            SW_HOOK_WITH_PHP_FUNC(socket_sendto);
            SW_HOOK_WITH_PHP_FUNC(socket_recvfrom);
            SW_HOOK_WITH_PHP_FUNC(socket_bind);
            SW_HOOK_WITH_PHP_FUNC(socket_listen);
            SW_HOOK_WITH_PHP_FUNC(socket_accept);
            SW_HOOK_WITH_PHP_FUNC(socket_getpeername);
            SW_HOOK_WITH_PHP_FUNC(socket_getsockname);
            SW_HOOK_WITH_PHP_FUNC(socket_getopt);
            SW_HOOK_WITH_PHP_FUNC(socket_get_option);
            SW_HOOK_WITH_PHP_FUNC(socket_setopt);
            SW_HOOK_WITH_PHP_FUNC(socket_set_option);
            SW_HOOK_WITH_PHP_FUNC(socket_set_block);
            SW_HOOK_WITH_PHP_FUNC(socket_set_nonblock);
            SW_HOOK_WITH_PHP_FUNC(socket_shutdown);
            SW_HOOK_WITH_PHP_FUNC(socket_close);
            SW_HOOK_WITH_PHP_FUNC(socket_clear_error);
            SW_HOOK_WITH_PHP_FUNC(socket_last_error);
            SW_HOOK_WITH_PHP_FUNC(socket_import_stream);

            inherit_class(ZEND_STRL("Swoole\\Coroutine\\Socket"), ZEND_STRL("Socket"));
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_BLOCKING_FUNCTION) {
            SW_UNHOOK_FUNC(socket_create);
            SW_UNHOOK_FUNC(socket_create_listen);
            SW_UNHOOK_FUNC(socket_create_pair);
            SW_UNHOOK_FUNC(socket_connect);
            SW_UNHOOK_FUNC(socket_write);
            SW_UNHOOK_FUNC(socket_read);
            SW_UNHOOK_FUNC(socket_send);
            SW_UNHOOK_FUNC(socket_recv);
            SW_UNHOOK_FUNC(socket_sendto);
            SW_UNHOOK_FUNC(socket_recvfrom);
            SW_UNHOOK_FUNC(socket_bind);
            SW_UNHOOK_FUNC(socket_listen);
            SW_UNHOOK_FUNC(socket_accept);
            SW_UNHOOK_FUNC(socket_getpeername);
            SW_UNHOOK_FUNC(socket_getsockname);
            SW_UNHOOK_FUNC(socket_getopt);
            SW_UNHOOK_FUNC(socket_get_option);
            SW_UNHOOK_FUNC(socket_setopt);
            SW_UNHOOK_FUNC(socket_set_option);
            SW_UNHOOK_FUNC(socket_set_block);
            SW_UNHOOK_FUNC(socket_set_nonblock);
            SW_UNHOOK_FUNC(socket_shutdown);
            SW_UNHOOK_FUNC(socket_close);
            SW_UNHOOK_FUNC(socket_clear_error);
            SW_UNHOOK_FUNC(socket_last_error);
            SW_UNHOOK_FUNC(socket_import_stream);

            detach_parent_class("Swoole\\Coroutine\\Socket");
        }
    }

#ifdef SW_USE_CURL
    // curl native
    if (flags & PHPCoroutine::HOOK_NATIVE_CURL) {
        if (flags & PHPCoroutine::HOOK_CURL) {
            php_swoole_fatal_error(E_WARNING, "cannot enable both hooks HOOK_NATIVE_CURL and HOOK_CURL at same time");
            flags ^= PHPCoroutine::HOOK_CURL;
        }
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_NATIVE_CURL)) {
            SW_HOOK_WITH_NATIVE_FUNC(curl_close);
            SW_HOOK_WITH_NATIVE_FUNC(curl_copy_handle);
            SW_HOOK_WITH_NATIVE_FUNC(curl_errno);
            SW_HOOK_WITH_NATIVE_FUNC(curl_error);
            SW_HOOK_WITH_NATIVE_FUNC(curl_exec);
            SW_HOOK_WITH_NATIVE_FUNC(curl_getinfo);
            SW_HOOK_WITH_NATIVE_FUNC(curl_init);
            SW_HOOK_WITH_NATIVE_FUNC(curl_setopt);
            SW_HOOK_WITH_NATIVE_FUNC(curl_setopt_array);
            SW_HOOK_WITH_NATIVE_FUNC(curl_reset);
            SW_HOOK_WITH_NATIVE_FUNC(curl_pause);
            SW_HOOK_WITH_NATIVE_FUNC(curl_escape);
            SW_HOOK_WITH_NATIVE_FUNC(curl_unescape);

            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_init);
            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_add_handle);
            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_exec);
            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_errno);
            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_select);
            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_setopt);
            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_getcontent);
            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_info_read);
            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_remove_handle);
            SW_HOOK_WITH_NATIVE_FUNC(curl_multi_close);
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_NATIVE_CURL) {
            SW_UNHOOK_FUNC(curl_close);
            SW_UNHOOK_FUNC(curl_copy_handle);
            SW_UNHOOK_FUNC(curl_errno);
            SW_UNHOOK_FUNC(curl_error);
            SW_UNHOOK_FUNC(curl_exec);
            SW_UNHOOK_FUNC(curl_getinfo);
            SW_UNHOOK_FUNC(curl_init);
            SW_UNHOOK_FUNC(curl_setopt);
            SW_UNHOOK_FUNC(curl_setopt_array);
            SW_UNHOOK_FUNC(curl_reset);
            SW_UNHOOK_FUNC(curl_pause);
            SW_UNHOOK_FUNC(curl_escape);
            SW_UNHOOK_FUNC(curl_unescape);

            SW_UNHOOK_FUNC(curl_multi_init);
            SW_UNHOOK_FUNC(curl_multi_add_handle);
            SW_UNHOOK_FUNC(curl_multi_exec);
            SW_UNHOOK_FUNC(curl_multi_errno);
            SW_UNHOOK_FUNC(curl_multi_select);
            SW_UNHOOK_FUNC(curl_multi_setopt);
            SW_UNHOOK_FUNC(curl_multi_getcontent);
            SW_UNHOOK_FUNC(curl_multi_info_read);
            SW_UNHOOK_FUNC(curl_multi_remove_handle);
            SW_UNHOOK_FUNC(curl_multi_close);
        }
    }
#endif
    // curl
    if (flags & PHPCoroutine::HOOK_CURL) {
        if (!(runtime_hook_flags & PHPCoroutine::HOOK_CURL)) {
            SW_HOOK_WITH_PHP_FUNC(curl_init);
            SW_HOOK_WITH_PHP_FUNC(curl_setopt);
            SW_HOOK_WITH_PHP_FUNC(curl_setopt_array);
            SW_HOOK_WITH_PHP_FUNC(curl_exec);
            SW_HOOK_WITH_PHP_FUNC(curl_getinfo);
            SW_HOOK_WITH_PHP_FUNC(curl_errno);
            SW_HOOK_WITH_PHP_FUNC(curl_error);
            SW_HOOK_WITH_PHP_FUNC(curl_reset);
            SW_HOOK_WITH_PHP_FUNC(curl_close);
            SW_HOOK_WITH_PHP_FUNC(curl_multi_getcontent);

            inherit_class(ZEND_STRL("Swoole\\Curl\\Handler"), ZEND_STRL("CurlHandle"));
        }
    } else {
        if (runtime_hook_flags & PHPCoroutine::HOOK_CURL) {
            SW_UNHOOK_FUNC(curl_init);
            SW_UNHOOK_FUNC(curl_setopt);
            SW_UNHOOK_FUNC(curl_setopt_array);
            SW_UNHOOK_FUNC(curl_exec);
            SW_UNHOOK_FUNC(curl_getinfo);
            SW_UNHOOK_FUNC(curl_errno);
            SW_UNHOOK_FUNC(curl_error);
            SW_UNHOOK_FUNC(curl_reset);
            SW_UNHOOK_FUNC(curl_close);
            SW_UNHOOK_FUNC(curl_multi_getcontent);

            detach_parent_class("Swoole\\Curl\\Handler");
        }
    }
}

bool PHPCoroutine::enable_hook(uint32_t flags) {
    /**
     * Stream-related settings are global variables, not thread-local resources.
     * The child threads must not modify stream settings;
     * the main thread can only make changes when there are no active worker threads.
     */
    if (sw_is_main_thread()) {
        if (sw_active_thread_count() > 1) {
            swoole_warning(
                "The stream runtime hook must be enabled or disabled only when there are no active threads.");
            hook_remove_stream_flags(&flags);
        }
    } else {
        hook_remove_stream_flags(&flags);
    }

    if (swoole_isset_hook((swGlobalHookType) PHP_SWOOLE_HOOK_BEFORE_ENABLE_HOOK)) {
        swoole_call_hook((swGlobalHookType) PHP_SWOOLE_HOOK_BEFORE_ENABLE_HOOK, &flags);
    }

    hook_stream_factory(&flags);
    hook_stream_ops(flags);
    hook_pdo_driver(flags);
    hook_all_func(flags);

    if (swoole_isset_hook((swGlobalHookType) PHP_SWOOLE_HOOK_AFTER_ENABLE_HOOK)) {
        swoole_call_hook((swGlobalHookType) PHP_SWOOLE_HOOK_AFTER_ENABLE_HOOK, &flags);
    }

    runtime_hook_flags = flags;

    return true;
}

bool PHPCoroutine::disable_hook() {
    return enable_hook(0);
}

static PHP_METHOD(swoole_runtime, enableCoroutine) {
    if (!SWOOLE_G(cli)) {
        php_swoole_fatal_error(E_ERROR, "must be used in PHP CLI mode");
        RETURN_FALSE;
    }
    zend_long flags = PHPCoroutine::HOOK_ALL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    PHPCoroutine::set_hook_flags(flags);
    RETURN_BOOL(PHPCoroutine::enable_hook(flags));
}

static PHP_METHOD(swoole_runtime, getHookFlags) {
    RETURN_LONG(PHPCoroutine::get_hook_flags());
}

static PHP_METHOD(swoole_runtime, setHookFlags) {
    if (!SWOOLE_G(cli)) {
        php_swoole_fatal_error(E_ERROR, "must be used in PHP CLI mode");
        RETURN_FALSE;
    }
    zend_long flags = PHPCoroutine::HOOK_ALL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    PHPCoroutine::set_hook_flags(flags);
    RETURN_BOOL(PHPCoroutine::enable_hook(flags));
}

static PHP_FUNCTION(swoole_sleep) {
    zend_long num;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &num) == FAILURE) {
        RETURN_FALSE;
    }
    if (num < 0) {
        php_error_docref(nullptr, E_WARNING, "Number of seconds must be greater than or equal to 0");
        RETURN_FALSE;
    }

    if (Coroutine::get_current()) {
        RETURN_LONG(System::sleep((double) num) < 0 ? num : 0);
    } else {
        RETURN_LONG(php_sleep((unsigned int) num));
    }
}

static PHP_FUNCTION(swoole_usleep) {
    zend_long num;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &num) == FAILURE) {
        RETURN_FALSE;
    }
    if (num < 0) {
        php_error_docref(nullptr, E_WARNING, "Number of seconds must be greater than or equal to 0");
        RETURN_FALSE;
    }
    double sec = (double) num / 1000000;
    if (Coroutine::get_current()) {
        System::sleep(sec);
    } else {
        usleep((unsigned int) num);
    }
}

static PHP_FUNCTION(swoole_time_nanosleep) {
    zend_long tv_sec, tv_nsec;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "ll", &tv_sec, &tv_nsec) == FAILURE) {
        RETURN_FALSE;
    }

    if (tv_sec < 0) {
        php_error_docref(nullptr, E_WARNING, "The seconds value must be greater than 0");
        RETURN_FALSE;
    }
    if (tv_nsec < 0) {
        php_error_docref(nullptr, E_WARNING, "The nanoseconds value must be greater than 0");
        RETURN_FALSE;
    }
    double _time = (double) tv_sec + (double) tv_nsec / 1000000000.00;
    if (Coroutine::get_current()) {
        System::sleep(_time);
    } else {
        struct timespec php_req, php_rem;
        php_req.tv_sec = (time_t) tv_sec;
        php_req.tv_nsec = (long) tv_nsec;

        if (nanosleep(&php_req, &php_rem) == 0) {
            RETURN_TRUE;
        } else if (errno == EINTR) {
            array_init(return_value);
            add_assoc_long_ex(return_value, "seconds", sizeof("seconds") - 1, php_rem.tv_sec);
            add_assoc_long_ex(return_value, "nanoseconds", sizeof("nanoseconds") - 1, php_rem.tv_nsec);
        } else if (errno == EINVAL) {
            php_swoole_error(E_WARNING, "nanoseconds was not in the range 0 to 999 999 999 or seconds was negative");
        }
    }
    RETURN_TRUE;
}

static PHP_FUNCTION(swoole_time_sleep_until) {
    double d_ts;
    timeval tm;
    timespec php_req, php_rem;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "d", &d_ts) == FAILURE) {
        RETURN_FALSE;
    }

    if (gettimeofday(&tm, nullptr) != 0) {
        RETURN_FALSE;
    }

    double c_ts = d_ts - tm.tv_sec - tm.tv_usec / 1000000.00;
    if (c_ts < 0) {
        php_error_docref(nullptr, E_WARNING, "Sleep until to time is less than current time");
        RETURN_FALSE;
    }

    php_req.tv_sec = (time_t) c_ts;
    if (php_req.tv_sec > c_ts) {
        php_req.tv_sec--;
    }
    php_req.tv_nsec = (long) ((c_ts - php_req.tv_sec) * 1000000000.00);

    double _time = (double) php_req.tv_sec + (double) php_req.tv_nsec / 1000000000.00;
    if (Coroutine::get_current()) {
        System::sleep(_time);
    } else {
        while (nanosleep(&php_req, &php_rem)) {
            if (errno == EINTR) {
                php_req.tv_sec = php_rem.tv_sec;
                php_req.tv_nsec = php_rem.tv_nsec;
            } else {
                RETURN_FALSE;
            }
        }
    }
    RETURN_TRUE;
}

static void stream_array_to_fd_set(zval *stream_array, std::unordered_map<int, PollSocket> &fds, int event) {
    zval *elem;
    zend_ulong index;
    zend_string *key;

    if (!ZVAL_IS_ARRAY(stream_array)) {
        return;
    }

    ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(stream_array), index, key, elem) {
        ZVAL_DEREF(elem);
        php_socket_t sock = php_swoole_convert_to_fd(elem);
        if (sock < 0) {
            continue;
        }
        auto i = fds.find(sock);
        if (i == fds.end()) {
            fds.emplace(sock, PollSocket(event, new zend::KeyValue(index, key, elem)));
        } else {
            i->second.events |= event;
        }
    }
    ZEND_HASH_FOREACH_END();
}

static int stream_array_emulate_read_fd_set(zval *stream_array) {
    zval *elem, new_array;
    php_stream *stream;
    int ret = 0;
    zend_ulong num_ind;
    zend_string *key;

    if (!ZVAL_IS_ARRAY(stream_array)) {
        return 0;
    }

    array_init_size(&new_array, zend_hash_num_elements(Z_ARRVAL_P(stream_array)));
    HashTable *ht = Z_ARRVAL(new_array);

    ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(stream_array), num_ind, key, elem) {
        ZVAL_DEREF(elem);
        php_stream_from_zval_no_verify(stream, elem);
        if (stream == nullptr) {
            continue;
        }
        if ((stream->writepos - stream->readpos) > 0) {
            /* allow readable non-descriptor based streams to participate in stream_select.
             * Non-descriptor streams will only "work" if they have previously buffered the
             * data.  Not ideal, but better than nothing.
             * This branch of code also allows blocking streams with buffered data to
             * operate correctly in stream_select.
             * */
            zval *dest_elem = !key ? zend_hash_index_update(ht, num_ind, elem) : zend_hash_update(ht, key, elem);
            zval_add_ref(dest_elem);
            ret++;
            continue;
        }
    }
    ZEND_HASH_FOREACH_END();

    if (ret > 0) {
        /* destroy old array and add new one */
        zend_array_destroy(Z_ARR_P(stream_array));
        ZVAL_ARR(stream_array, ht);
    } else {
        zend_array_destroy(ht);
    }

    return ret;
}

static PHP_FUNCTION(swoole_stream_select) {
    Coroutine::get_current_safe();

    zval *r_array, *w_array, *e_array;
    zend_long sec, usec = 0;
    zend_bool secnull;
#if PHP_VERSION_ID >= 80100
    bool usecnull = true;
#endif
    int retval = 0;

    ZEND_PARSE_PARAMETERS_START(4, 5)
    Z_PARAM_ARRAY_EX2(r_array, 1, 1, 1)
    Z_PARAM_ARRAY_EX2(w_array, 1, 1, 1)
    Z_PARAM_ARRAY_EX2(e_array, 1, 1, 1)
    Z_PARAM_LONG_OR_NULL(sec, secnull)
    Z_PARAM_OPTIONAL
#if PHP_VERSION_ID >= 80100
    Z_PARAM_LONG_OR_NULL(usec, usecnull)
#else
    Z_PARAM_LONG(usec)
#endif
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

#if PHP_VERSION_ID >= 80100
    if (secnull && !usecnull) {
        if (usec != 0) {
            zend_argument_value_error(5, "must be null when argument #4 ($seconds) is null");
            RETURN_THROWS();
        }
    }
#endif

    double timeout = -1;
    if (!secnull) {
        if (sec < 0) {
            php_error_docref(nullptr, E_WARNING, "The seconds parameter must be greater than 0");
            RETURN_FALSE;
        } else if (usec < 0) {
            php_error_docref(nullptr, E_WARNING, "The microseconds parameter must be greater than 0");
            RETURN_FALSE;
        }
        timeout = (double) sec + ((double) usec / 1000000);
    }

    std::unordered_map<int, PollSocket> fds;

    if (r_array != nullptr) {
        stream_array_to_fd_set(r_array, fds, SW_EVENT_READ);
    }

    if (w_array != nullptr) {
        stream_array_to_fd_set(w_array, fds, SW_EVENT_WRITE);
    }

    if (e_array != nullptr) {
        stream_array_to_fd_set(e_array, fds, SW_EVENT_ERROR);
    }

    if (fds.size() == 0) {
        php_error_docref(nullptr, E_WARNING, "No stream arrays were passed");
        RETURN_FALSE;
    }

    ON_SCOPE_EXIT {
        for (auto &i : fds) {
            const auto *kv = static_cast<zend::KeyValue *>(i.second.ptr);
            delete kv;
        }
    };

    /* slight hack to support buffered data; if there is data sitting in the
     * read buffer of the streams in the read array, let's pretend
     * that we selected, but return only the readable sockets */
    if (r_array != nullptr) {
        retval = stream_array_emulate_read_fd_set(r_array);
        if (retval > 0) {
            if (w_array != nullptr) {
                zend_hash_clean(Z_ARRVAL_P(w_array));
            }
            if (e_array != nullptr) {
                zend_hash_clean(Z_ARRVAL_P(e_array));
            }
            RETURN_LONG(retval);
        }
    }

    if (r_array != nullptr) {
        zend_hash_clean(Z_ARRVAL_P(r_array));
    }
    if (w_array != nullptr) {
        zend_hash_clean(Z_ARRVAL_P(w_array));
    }
    if (e_array != nullptr) {
        zend_hash_clean(Z_ARRVAL_P(e_array));
    }

    /**
     * timeout or add failed
     */
    if (!System::socket_poll(fds, timeout)) {
        RETURN_LONG(0);
    }

    for (auto &i : fds) {
        auto *kv = static_cast<zend::KeyValue *>(i.second.ptr);
        int revents = i.second.revents;
        SW_ASSERT((revents & (~(SW_EVENT_READ | SW_EVENT_WRITE | SW_EVENT_ERROR))) == 0);
        if (revents > 0) {
            if ((revents & SW_EVENT_READ) && r_array) {
                kv->add_to(r_array);
            }
            if ((revents & SW_EVENT_WRITE) && w_array) {
                kv->add_to(w_array);
            }
            if ((revents & SW_EVENT_ERROR) && e_array) {
                kv->add_to(e_array);
            }
            retval++;
        }
    }

    RETURN_LONG(retval);
}

static void hook_func(const char *name, size_t l_name, zif_handler handler, zend_internal_arg_info *arg_info) {
    auto *rf = static_cast<PhpFunc *>(zend_hash_str_find_ptr(tmp_function_table, name, l_name));
    bool use_php_func = false;
    /**
     * use php library function
     */
    if (handler == nullptr) {
        handler = PHP_FN(swoole_user_func_handler);
        use_php_func = true;
    }
    if (rf) {
        rf->function->internal_function.handler = handler;
        if (arg_info) {
            rf->function->internal_function.arg_info = arg_info;
        }
        return;
    }

    auto *zf = static_cast<zend_function *>(zend_hash_str_find_ptr(EG(function_table), name, l_name));
    if (zf == nullptr) {
        return;
    }

    auto fn_str = zf->common.function_name;
    rf = static_cast<PhpFunc *>(emalloc(sizeof(PhpFunc)));
    sw_memset_zero(rf, sizeof(*rf));
    rf->function = zf;

    auto fn_name = std::string(fn_str->val, fn_str->len);

    rf->ori_handler = zf->internal_function.handler;
    rf->ori_arg_info = zf->internal_function.arg_info;

    if (sw_is_main_thread()) {
        ori_func_handlers.set(fn_name, rf->ori_handler);
        ori_func_arg_infos.set(fn_name, rf->ori_arg_info);
    }

    zf->internal_function.handler = handler;
    if (arg_info) {
        zf->internal_function.arg_info = copy_arginfo(zf, arg_info);
        rf->arg_info_copy = zf->internal_function.arg_info;
    }

    if (use_php_func) {
        char func[128];
        memcpy(func, ZEND_STRL("swoole_"));
        memcpy(func + 7, fn_str->val, fn_str->len);

        ZVAL_STRINGL(&rf->name, func, fn_str->len + 7);
        auto fci_cache = sw_callable_create(&rf->name);
        if (!fci_cache) {
            return;
        }
        rf->fci_cache = fci_cache;
    }

    zend_hash_add_ptr(tmp_function_table, fn_str, rf);
}

static void unhook_func(const char *name, size_t l_name) {
    auto *rf = static_cast<PhpFunc *>(zend_hash_str_find_ptr(tmp_function_table, name, l_name));
    if (rf == nullptr) {
        return;
    }
    if (rf->arg_info_copy) {
        free_arg_info(&rf->function->internal_function);
        rf->arg_info_copy = nullptr;
    }
    rf->function->internal_function.handler = rf->ori_handler;
    rf->function->internal_function.arg_info = rf->ori_arg_info;
}

php_stream *php_swoole_create_stream_from_socket(php_socket_t _fd, int domain, int type, int protocol STREAMS_DC) {
    auto *abstract = new NetStream();
    abstract->socket = std::make_shared<Socket>(_fd, domain, type, protocol);
    if (FG(default_socket_timeout) > 0) {
        abstract->socket->set_timeout((double) FG(default_socket_timeout));
    }
    abstract->stream.timeout.tv_sec = FG(default_socket_timeout);
    abstract->stream.socket = abstract->socket->get_fd();
    abstract->blocking = true;

    php_stream *stream = php_stream_alloc_rel(&socket_ops, abstract, nullptr, "r+");

    if (stream == nullptr) {
        delete abstract;
    } else {
        stream->flags |= PHP_STREAM_FLAG_AVOID_BLOCKING;
    }

    return stream;
}

php_stream *php_swoole_create_stream_from_pipe(int fd, const char *mode, const char *persistent_id STREAMS_DC) {
#if PHP_VERSION_ID >= 80200
    return _sw_php_stream_fopen_from_fd(fd, mode, persistent_id, false STREAMS_CC);
#else
    return _sw_php_stream_fopen_from_fd(fd, mode, persistent_id STREAMS_CC);
#endif
}

php_stream_ops *php_swoole_get_ori_php_stream_stdio_ops() {
    return &ori_php_stream_stdio_ops;
}

zif_handler php_swoole_get_original_handler(const char *name, size_t len) {
    if (sw_is_main_thread()) {
        auto *rf = static_cast<PhpFunc *>(zend_hash_str_find_ptr(tmp_function_table, name, len));
        if (rf) {
            return rf->ori_handler;
        }
    } else {
        zif_handler handler = ori_func_handlers.get(std::string(name, len));
        if (handler) {
            return handler;
        }
        auto *zf = static_cast<zend_function *>(zend_hash_str_find_ptr(EG(function_table), name, len));
        if (zf && zf->type == ZEND_INTERNAL_FUNCTION && zf->internal_function.handler) {
            return zf->internal_function.handler;
        }
    }

    return nullptr;
}

static PHP_FUNCTION(swoole_stream_socket_pair) {
    zend_long domain, type, protocol;
    php_socket_t pair[2];

    ZEND_PARSE_PARAMETERS_START(3, 3)
    Z_PARAM_LONG(domain)
    Z_PARAM_LONG(type)
    Z_PARAM_LONG(protocol)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    if (0 != socketpair((int) domain, (int) type, (int) protocol, pair)) {
        php_swoole_error(E_WARNING, "failed to create sockets: [%d]: %s", errno, strerror(errno));
        RETURN_FALSE;
    }

    array_init(return_value);

    php_swoole_check_reactor();

    php_stream *s1 = php_swoole_create_stream_from_socket(pair[0], domain, type, protocol STREAMS_CC);
    php_stream *s2 = php_swoole_create_stream_from_socket(pair[1], domain, type, protocol STREAMS_CC);

    /* set the __exposed flag.
     * php_stream_to_zval() does, add_next_index_resource() does not */
    php_stream_auto_cleanup(s1);
    php_stream_auto_cleanup(s2);

    add_next_index_resource(return_value, s1->res);
    add_next_index_resource(return_value, s2->res);
}

static PHP_FUNCTION(swoole_user_func_handler) {
    auto fn_str = execute_data->func->common.function_name;
    if (!swoole_coroutine_is_in()) {
        auto ori_handler = ori_func_handlers.get(std::string(fn_str->val, fn_str->len));
        ori_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);
        return;
    }

    auto *rf = static_cast<PhpFunc *>(zend_hash_find_ptr(tmp_function_table, fn_str));
    if (!rf) {
        zend_throw_exception_ex(swoole_exception_ce, SW_ERROR_UNDEFINED_BEHAVIOR, "%s func not exists", fn_str->val);
        return;
    }

    zend_fcall_info fci;
    fci.size = sizeof(fci);
    fci.object = nullptr;
    fci.retval = return_value;
    fci.param_count = ZEND_NUM_ARGS();
    fci.params = ZEND_CALL_ARG(execute_data, 1);
    fci.named_params = nullptr;
    ZVAL_UNDEF(&fci.function_name);
    zend_call_function(&fci, rf->fci_cache->ptr());
}

zend_class_entry *find_class_entry(const char *name, size_t length) {
    zend_string *search_key = zend_string_init(name, length, false);
    zend_class_entry *class_ce = zend_lookup_class(search_key);
    zend_string_release(search_key);
    return class_ce ? class_ce : nullptr;
}

static void inherit_class(const char *child_name, size_t child_length, const char *parent_name, size_t parent_length) {
    zend_class_entry *temp_ce = nullptr;
    zend_class_entry *child_ce = find_class_entry(child_name, child_length);
    zend_class_entry *parent_ce = find_class_entry(parent_name, parent_length);

    if (!child_ce || !parent_ce || instanceof_function(child_ce, parent_ce)) {
        return;
    }

    temp_ce = child_ce;
    while (temp_ce->parent) {
        temp_ce = temp_ce->parent;
    }
    temp_ce->parent = parent_ce;

    std::string key(ZSTR_VAL(child_ce->name));
    child_class_entries.insert({key, child_ce});
}

void start_detach_parent_class(zend_class_entry *class_ce) {
    zend_class_entry *p1 = nullptr;
    zend_class_entry *p2 = nullptr;

    p1 = class_ce;
    p2 = class_ce->parent;
    while (p2->parent) {
        p1 = p1->parent;
        p2 = p2->parent;
    }

    p1->parent = nullptr;
}

static void detach_parent_class(const char *child_name) {
    std::string search_key(child_name);
    auto iter = child_class_entries.find(search_key);
    if (iter == child_class_entries.end()) {
        return;
    }
    start_detach_parent_class(iter->second);
    child_class_entries.erase(search_key);
}

static void clear_class_entries() {
    for (const auto &child_class_entry : child_class_entries) {
        start_detach_parent_class(child_class_entry.second);
    }
    child_class_entries.clear();
}

#if defined(HAVE_PUTENV) && defined(SW_THREAD)
/* {{{ Set the value of an environment variable */
static PHP_FUNCTION(swoole_putenv) {
    char *setting;
    size_t setting_len;
    char *p;
    bool result;
    std::string key;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(setting, setting_len)
    ZEND_PARSE_PARAMETERS_END();

    if (setting_len == 0 || setting[0] == '=') {
        zend_argument_value_error(1, "must have a valid syntax");
        RETURN_THROWS();
    }

    if ((p = strchr(setting, '='))) {
        key = std::string(setting, p - setting);
    } else {
        key = std::string(setting, setting_len);
    }

    tsrm_env_lock();
    swoole_runtime_environ[key] = std::string(setting, setting_len);
    auto iter = swoole_runtime_environ.find(key);

#ifdef HAVE_UNSETENV
    if (!p) { /* no '=' means we want to unset it */
        unsetenv(iter->second.c_str());
    }
    if (!p || putenv((char *) iter->second.c_str()) == 0) { /* success */
#else
    if (putenv((char *) iter->second.c_str()) == 0) { /* success */
#endif

#ifdef HAVE_TZSET
        if (zend_binary_strcasecmp(key.c_str(), key.length(), ZEND_STRL("TZ")) == 0) {
            tzset();
        }
#endif
        result = true;
    } else {
        result = false;
    }

    tsrm_env_unlock();
    RETURN_BOOL(result);
}
/* }}} */
#endif
