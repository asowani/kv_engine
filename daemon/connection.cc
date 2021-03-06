/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "config.h"
#include "mcaudit.h"
#include "memcached.h"
#include "runtime.h"
#include "server_event.h"
#include "statemachine_mcbp.h"

#include <mcbp/protocol/header.h>
#include <platform/checked_snprintf.h>
#include <platform/strerror.h>
#include <utilities/logtags.h>
#include <utilities/protocol2text.h>
#include <exception>

const char* to_string(const Connection::Priority& priority) {
    switch (priority) {
    case Connection::Priority::High:
        return "High";
    case Connection::Priority::Medium:
        return "Medium";
    case Connection::Priority::Low:
        return "Low";
    }
    throw std::invalid_argument("No such priority: " +
                                std::to_string(int(priority)));
}

static cbsasl_conn_t* create_new_cbsasl_server_t() {
    cbsasl_conn_t *conn;
    if (cbsasl_server_new("memcached", // service
                          nullptr, // Server DQDN
                          nullptr, // user realm
                          nullptr, // iplocalport
                          nullptr, // ipremoteport
                          nullptr, // callbacks
                          0, // flags
                          &conn) != CBSASL_OK) {
        throw std::bad_alloc();
    }
    return conn;
}

Connection::Connection(SOCKET sfd, event_base* b)
    : socketDescriptor(sfd),
      base(b),
      sasl_conn(create_new_cbsasl_server_t()),
      internal(false),
      authenticated(false),
      username("unknown"),
      domain(cb::sasl::Domain::Local),
      nodelay(false),
      refcount(0),
      engine_storage(nullptr),
      next(nullptr),
      thread(nullptr),
      parent_port(0),
      bucketEngine(nullptr),
      peername("unknown"),
      sockname("unknown"),
      priority(Priority::Medium),
      clustermap_revno(-2),
      trace_enabled(false),
      xerror_support(false),
      collections_support(false) {
    MEMCACHED_CONN_CREATE(this);
    bucketIndex.store(0);
    updateDescription();
}

Connection::Connection(SOCKET sock,
                       event_base* b,
                       const ListeningPort& interface)
    : Connection(sock, b) {
    parent_port = interface.port;
    resolveConnectionName(false);
    setTcpNoDelay(interface.tcp_nodelay);
    updateDescription();
}

Connection::~Connection() {
    MEMCACHED_CONN_DESTROY(this);
    if (socketDescriptor != INVALID_SOCKET) {
        LOG_DEBUG("{} - Closing socket descriptor", getId());
        safe_close(socketDescriptor);
    }
}

/**
 * Convert a sockaddr_storage to a textual string (no name lookup).
 *
 * @param addr the sockaddr_storage received from getsockname or
 *             getpeername
 * @param addr_len the current length used by the sockaddr_storage
 * @return a textual string representing the connection. or NULL
 *         if an error occurs (caller takes ownership of the buffer and
 *         must call free)
 */
static std::string sockaddr_to_string(const struct sockaddr_storage* addr,
                                      socklen_t addr_len) {
    char host[50];
    char port[50];

    int err = getnameinfo(reinterpret_cast<const struct sockaddr*>(addr),
                          addr_len,
                          host, sizeof(host),
                          port, sizeof(port),
                          NI_NUMERICHOST | NI_NUMERICSERV);
    if (err != 0) {
        LOG_WARNING("getnameinfo failed with error {}", err);
        return nullptr;
    }

    if (addr->ss_family == AF_INET6) {
        return "[" + std::string(host) + "]:" + std::string(port);
    } else {
        return std::string(host) + ":" + std::string(port);
    }
}

void Connection::resolveConnectionName(bool listening) {
    if (socketDescriptor == INVALID_SOCKET) {
        // Our unit tests run without a socket connected, and we don't
        // want them to flood the console with error messages
        peername = "[invalid]";
        sockname = "[invalid]";
        return;
    }

    int err;
    try {
        if (listening) {
            peername = "*";
        } else {
            struct sockaddr_storage peer;
            socklen_t peer_len = sizeof(peer);
            if ((err = getpeername(socketDescriptor,
                                   reinterpret_cast<struct sockaddr*>(&peer),
                                   &peer_len)) != 0) {
                LOG_WARNING("getpeername for socket {} with error {}",
                            socketDescriptor,
                            err);
            } else {
                peername = sockaddr_to_string(&peer, peer_len);
            }
        }

        struct sockaddr_storage sock;
        socklen_t sock_len = sizeof(sock);
        if ((err = getsockname(socketDescriptor,
                               reinterpret_cast<struct sockaddr*>(&sock),
                               &sock_len)) != 0) {
            LOG_WARNING("getsockname for socket {} with error {}",
                        socketDescriptor,
                        err);
        } else {
            sockname = sockaddr_to_string(&sock, sock_len);
        }
        updateDescription();
    } catch (const std::bad_alloc& e) {
        LOG_WARNING(
                "Connection::resolveConnectionName: failed to allocate memory: "
                "{}",
                e.what());
    }
}

bool Connection::setTcpNoDelay(bool enable) {
    if (socketDescriptor == INVALID_SOCKET) {
        // Our unit test run without a connected socket (and there is
        // no point of running setsockopt on an invalid socket and
        // get the error message from there).. But we don't want them
        // (the unit tests) to flood the console with error messages
        // that setsockopt failed
        return false;
    }

    int flags = enable ? 1 : 0;

#if defined(WIN32)
    char* flags_ptr = reinterpret_cast<char*>(&flags);
#else
    void* flags_ptr = reinterpret_cast<void*>(&flags);
#endif
    int error = setsockopt(socketDescriptor, IPPROTO_TCP, TCP_NODELAY,
                           flags_ptr,
                           sizeof(flags));

    if (error != 0) {
        std::string errmsg = cb_strerror(GetLastNetworkError());
        LOG_WARNING("setsockopt(TCP_NODELAY): {}", errmsg);
        nodelay = false;
        return false;
    } else {
        nodelay = enable;
    }

    return true;
}

unique_cJSON_ptr Connection::toJSON() const {
    unique_cJSON_ptr ret(cJSON_CreateObject());
    cJSON* obj = ret.get();
    cJSON_AddUintPtrToObject(obj, "connection", (uintptr_t)this);
    if (socketDescriptor == INVALID_SOCKET) {
        cJSON_AddStringToObject(obj, "socket", "disconnected");
    } else {
        cJSON_AddNumberToObject(obj, "socket", socketDescriptor);
        cJSON_AddStringToObject(obj, "protocol", "memcached");
        cJSON_AddStringToObject(obj, "peername", getPeername().c_str());
        cJSON_AddStringToObject(obj, "sockname", getSockname().c_str());
        cJSON_AddNumberToObject(obj, "parent_port", parent_port);
        cJSON_AddNumberToObject(obj, "bucket_index", getBucketIndex());
        cJSON_AddBoolToObject(obj, "internal", isInternal());
        if (authenticated) {
            cJSON_AddStringToObject(obj, "username", username.c_str());
        }
        if (sasl_conn != NULL) {
            cJSON_AddUintPtrToObject(obj, "sasl_conn",
                                       (uintptr_t)sasl_conn.get());
        }
        cJSON_AddBoolToObject(obj, "nodelay", nodelay);
        cJSON_AddNumberToObject(obj, "refcount", refcount);

        cJSON* features = cJSON_CreateObject();
        cJSON_AddBoolToObject(features, "mutation_extras",
                                isSupportsMutationExtras());
        cJSON_AddBoolToObject(features, "xerror", isXerrorSupport());

        cJSON_AddItemToObject(obj, "features", features);

        cJSON_AddUintPtrToObject(obj, "engine_storage",
                                   (uintptr_t)engine_storage);
        cJSON_AddUintPtrToObject(obj, "next", (uintptr_t)next);
        cJSON_AddUintPtrToObject(obj, "thread", (uintptr_t)thread.load(
            std::memory_order::memory_order_relaxed));
        cJSON_AddStringToObject(obj, "priority", to_string(priority));

        if (clustermap_revno == -2) {
            cJSON_AddStringToObject(obj, "clustermap_revno", "unknown");
        } else {
            cJSON_AddNumberToObject(obj, "clustermap_revno", clustermap_revno);
        }

        cJSON_AddStringToObject(obj,
                                "total_cpu_time",
                                std::to_string(total_cpu_time.count()).c_str());
        cJSON_AddStringToObject(obj,
                                "min_sched_time",
                                std::to_string(min_sched_time.count()).c_str());
        cJSON_AddStringToObject(obj,
                                "max_sched_time",
                                std::to_string(max_sched_time.count()).c_str());
    }
    return ret;
}

void Connection::restartAuthentication() {
    sasl_conn.reset(create_new_cbsasl_server_t());
    internal = false;
    authenticated = false;
    username = "";
}

cb::engine_errc Connection::dropPrivilege(cb::rbac::Privilege privilege) {
    if (privilegeContext.dropPrivilege(privilege)) {
        return cb::engine_errc::success;
    }

    return cb::engine_errc::no_access;
}

cb::rbac::PrivilegeAccess Connection::checkPrivilege(
        cb::rbac::Privilege privilege, Cookie& cookie) {
    cb::rbac::PrivilegeAccess ret;
    unsigned int retries = 0;
    const unsigned int max_retries = 100;

    while ((ret = privilegeContext.check(privilege)) ==
                   cb::rbac::PrivilegeAccess::Stale &&
           retries < max_retries) {
        ++retries;
        const auto opcode = cookie.getHeader().getOpcode();
        const std::string command(memcached_opcode_2_text(opcode));

        // The privilege context we had could have been a dummy entry
        // (created when the client connected, and used until the
        // connection authenticates). Let's try to automatically update it,
        // but let the client deal with whatever happens after
        // a single update.
        try {
            privilegeContext = cb::rbac::createContext(getUsername(),
                                                       all_buckets[bucketIndex].name);
        } catch (const cb::rbac::NoSuchBucketException&) {
            // Remove all access to the bucket
            privilegeContext = cb::rbac::createContext(getUsername(), "");
            LOG_INFO(
                    "{}: RBAC: Connection::checkPrivilege({}) {} No access to "
                    "bucket [{}]. command: [{}] new privilege set: {}",
                    getId(),
                    to_string(privilege),
                    getDescription(),
                    all_buckets[bucketIndex].name,
                    command,
                    privilegeContext.to_string());
        } catch (const cb::rbac::Exception& error) {
            LOG_WARNING(
                    "{}: RBAC: Connection::checkPrivilege({}) {}: An "
                    "exception occurred. command: [{}] bucket: [{}] UUID:"
                    "[{}] message: {}",
                    getId(),
                    to_string(privilege),
                    getDescription(),
                    command,
                    all_buckets[bucketIndex].name,
                    cookie.getEventId(),
                    error.what());
            // Add a textual error as well
            cookie.setErrorContext("An exception occurred. command: [" +
                                   command + "]");
            return cb::rbac::PrivilegeAccess::Fail;
        }
    }

    if (retries == max_retries) {
        LOG_INFO(
                "{}: RBAC: Gave up rebuilding privilege context after {} "
                "times. Let the client handle the stale authentication "
                "context",
                getId(),
                retries);

    } else if (retries > 1) {
        LOG_INFO("{}: RBAC: Had to rebuild privilege context {} times",
                 getId(),
                 retries);
    }

    if (ret == cb::rbac::PrivilegeAccess::Fail) {
        const auto opcode = cookie.getHeader().getOpcode();
        const std::string command(memcached_opcode_2_text(opcode));
        const std::string privilege_string = cb::rbac::to_string(privilege);
        const std::string context = privilegeContext.to_string();

        if (settings.isPrivilegeDebug()) {
            audit_privilege_debug(this,
                                  command,
                                  all_buckets[bucketIndex].name,
                                  privilege_string,
                                  context);

            LOG_INFO(
                    "{}: RBAC privilege debug:{} command:[{}] bucket:[{}] "
                    "privilege:[{}] context:{}",
                    getId(),
                    getDescription(),
                    command,
                    all_buckets[bucketIndex].name,
                    privilege_string,
                    context);

            return cb::rbac::PrivilegeAccess::Ok;
        } else {
            LOG_INFO(
                    "{} RBAC {} missing privilege {} for {} in bucket:[{}] "
                    "with context: "
                    "%s UUID:[{}]",
                    getId(),
                    getDescription(),
                    privilege_string,
                    command,
                    all_buckets[bucketIndex].name,
                    context,
                    cookie.getEventId());
            // Add a textual error as well
            cookie.setErrorContext("Authorization failure: can't execute " +
                                   command + " operation without the " +
                                   privilege_string + " privilege");
        }
    }

    return ret;
}

Bucket& Connection::getBucket() const {
    return all_buckets[getBucketIndex()];
}

ENGINE_ERROR_CODE Connection::remapErrorCode(ENGINE_ERROR_CODE code) const {
    if (xerror_support) {
        return code;
    }

    // Check our whitelist
    switch (code) {
    case ENGINE_SUCCESS: // FALLTHROUGH
    case ENGINE_KEY_ENOENT: // FALLTHROUGH
    case ENGINE_KEY_EEXISTS: // FALLTHROUGH
    case ENGINE_ENOMEM: // FALLTHROUGH
    case ENGINE_NOT_STORED: // FALLTHROUGH
    case ENGINE_EINVAL: // FALLTHROUGH
    case ENGINE_ENOTSUP: // FALLTHROUGH
    case ENGINE_EWOULDBLOCK: // FALLTHROUGH
    case ENGINE_E2BIG: // FALLTHROUGH
    case ENGINE_WANT_MORE: // FALLTHROUGH
    case ENGINE_DISCONNECT: // FALLTHROUGH
    case ENGINE_NOT_MY_VBUCKET: // FALLTHROUGH
    case ENGINE_TMPFAIL: // FALLTHROUGH
    case ENGINE_ERANGE: // FALLTHROUGH
    case ENGINE_ROLLBACK: // FALLTHROUGH
    case ENGINE_EBUSY: // FALLTHROUGH
    case ENGINE_DELTA_BADVAL: // FALLTHROUGH
    case ENGINE_PREDICATE_FAILED:
    case ENGINE_FAILED:
        return code;

    case ENGINE_LOCKED:
        return ENGINE_KEY_EEXISTS;
    case ENGINE_LOCKED_TMPFAIL:
        return ENGINE_TMPFAIL;
    case ENGINE_UNKNOWN_COLLECTION:
        return isCollectionsSupported() ? code : ENGINE_EINVAL;

    case ENGINE_EACCESS:break;
    case ENGINE_NO_BUCKET:break;
    case ENGINE_AUTH_STALE:break;
    }

    // Seems like the rest of the components in our system isn't
    // prepared to receive access denied or authentincation stale.
    // For now we should just disconnect them
    auto errc = cb::make_error_condition(cb::engine_errc(code));
    LOG_INFO(
            "{} - Client {} not aware of extended error code ({}). "
            "Disconnecting",
            getId(),
            getDescription().c_str(),
            errc.message().c_str());

    return ENGINE_DISCONNECT;
}

void Connection::resetUsernameCache() {
    static const char unknown[] = "unknown";
    const void* unm = unknown;

    if (cbsasl_getprop(sasl_conn.get(),
                       CBSASL_USERNAME, &unm) != CBSASL_OK) {
        unm = unknown;
    }

    username.assign(reinterpret_cast<const char*>(unm));

    domain = cb::sasl::get_domain(sasl_conn.get());

    updateDescription();
}

void Connection::updateDescription() {
    description.assign("[ " + getPeername() + " - " + getSockname());
    if (authenticated) {
        description += " (";
        if (isInternal()) {
            description += "System, ";
        }
        description += cb::logtags::tagUserData(getUsername());

        if (domain == cb::sasl::Domain::External) {
            description += " (LDAP)";
        }
        description += ")";
    } else {
        description += " (not authenticated)";
    }
    description += " ]";
}

void Connection::setBucketIndex(int bucketIndex) {
    Connection::bucketIndex.store(bucketIndex, std::memory_order_relaxed);

    if (bucketIndex < 0) {
        // The connection objects which listens to the ports to accept
        // use a bucketIndex of -1. Those connection objects should
        // don't need an entry
        return;
    }

    // Update the privilege context. If a problem occurs within the RBAC
    // module we'll assign an empty privilege context to the connection.
    try {
        if (authenticated) {
            // The user have logged in, so we should create a context
            // representing the users context in the desired bucket.
            privilegeContext = cb::rbac::createContext(username,
                                                       all_buckets[bucketIndex].name);
        } else if (is_default_bucket_enabled() &&
                   strcmp("default", all_buckets[bucketIndex].name) == 0) {
            // We've just connected to the _default_ bucket, _AND_ the client
            // is unknown.
            // Personally I think the "default bucket" concept is a really
            // really bad idea, but we need to be backwards compatible for
            // a while... lets look up a profile named "default" and
            // assign that. It should only contain access to the default
            // bucket.
            privilegeContext = cb::rbac::createContext("default",
                                                       all_buckets[bucketIndex].name);
        } else {
            // The user has not authenticated, and this isn't for the
            // "default bucket". Assign an empty profile which won't give
            // you any privileges.
            privilegeContext = cb::rbac::PrivilegeContext{};
        }
    } catch (const cb::rbac::Exception&) {
        privilegeContext = cb::rbac::PrivilegeContext{};
    }

    if (bucketIndex == 0) {
        // If we're connected to the no bucket we should return
        // no bucket instead of EACCESS. Lets give the connection all
        // possible bucket privileges
        privilegeContext.setBucketPrivileges();
    }
}

void Connection::addCpuTime(std::chrono::nanoseconds ns) {
    total_cpu_time += ns;
    min_sched_time = std::min(min_sched_time, ns);
    max_sched_time = std::max(min_sched_time, ns);
}

void Connection::enqueueServerEvent(std::unique_ptr<ServerEvent> event) {
    server_events.push(std::move(event));
}
