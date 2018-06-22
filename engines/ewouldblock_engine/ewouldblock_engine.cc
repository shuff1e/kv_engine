/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

/*
 *                "ewouldblock_engine"
 *
 * The "ewouldblock_engine" allows one to test how memcached responds when the
 * engine returns EWOULDBLOCK instead of the correct response.
 *
 * Motivation:
 *
 * The EWOULDBLOCK response code can be returned from a number of engine
 * functions, and is used to indicate that the request could not be immediately
 * fulfilled, and it "would block" if it tried to. The correct way for
 * memcached to handle this (in general) is to suspend that request until it
 * is later notified by the engine (via notify_io_complete()).
 *
 * However, engines typically return the correct response to requests
 * immediately, only rarely (and from memcached's POV non-deterministically)
 * returning EWOULDBLOCK. This makes testing of the code-paths handling
 * EWOULDBLOCK tricky.
 *
 *
 * Operation:
 * This engine, when loaded by memcached proxies requests to a "real" engine.
 * Depending on how it is configured, it can simply pass the request on to the
 * real engine, or artificially return EWOULDBLOCK back to memcached.
 *
 * See the 'Modes' enum below for the possible modes for a connection. The mode
 * can be selected by sending a `request_ewouldblock_ctl` command
 *  (opcode PROTOCOL_BINARY_CMD_EWOULDBLOCK_CTL).
 *
 * DCP:
 *    There is a special DCP stream named "ewb_internal" which is an
 *    endless stream of items. You may also add a number at the end
 *    e.g. "ewb_internal:10" and it'll create a stream with 10 entries.
 *    It will always send the same K-V pair.
 *    Note that we don't register for disconnect events so you might
 *    experience weirdness if you first try to use the internal dcp
 *    stream, and then later on want to use the one provided by the
 *    engine. The workaround for that is to delete the bucket
 *    in between ;-) (put them in separate test suites and it'll all
 *    be handled for you.
 *
 *    Any other stream name results in proxying the dcp request to
 *    the underlying engine's DCP implementation.
 *
 */

#include "ewouldblock_engine.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <gsl/gsl>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <utility>

#include <logger/logger.h>
#include <memcached/engine.h>
#include <memcached/extension.h>
#include <platform/cb_malloc.h>
#include <platform/dirutils.h>
#include <platform/thread.h>
#include <xattr/blob.h>

#include "utilities/engine_loader.h"

/* Public API declaration ****************************************************/

extern "C" {
    MEMCACHED_PUBLIC_API
    ENGINE_ERROR_CODE create_instance(GET_SERVER_API gsa,
                                      ENGINE_HANDLE** handle);

    MEMCACHED_PUBLIC_API
    void destroy_engine(void);
}


class EWB_Engine;

// Mapping from wrapped handle to EWB handles.
static std::map<ENGINE_HANDLE*, EWB_Engine*> engine_map;

class NotificationThread : public Couchbase::Thread {
public:
    NotificationThread(EWB_Engine& engine_)
        : Thread("ewb:pendingQ"),
          engine(engine_) {}

protected:
    void run() override;

protected:
    EWB_Engine& engine;
};

/**
 * The BlockMonitorThread represents the thread that is
 * monitoring the "lock" file. Once the file is no longer
 * there it will resume the client specified with the given
 * id.
 */
class BlockMonitorThread : public Couchbase::Thread {
public:
    BlockMonitorThread(EWB_Engine& engine_,
                       uint32_t id_,
                       const std::string file_)
        : Thread("ewb:BlockMon"),
          engine(engine_),
          id(id_),
          file(file_) {}

    /**
     * Wait for the underlying thread to reach the zombie state
     * (== terminated, but not reaped)
     */
    ~BlockMonitorThread() {
        waitForState(Couchbase::ThreadState::Zombie);
    }

protected:
    void run() override;

private:
    EWB_Engine& engine;
    const uint32_t id;
    const std::string file;
};

static void register_callback(ENGINE_HANDLE *, ENGINE_EVENT_TYPE,
                              EVENT_CALLBACK, const void *);

static SERVER_HANDLE_V1 wrapped_api;
static SERVER_HANDLE_V1 *real_api;
static void init_wrapped_api(GET_SERVER_API fn) {
    static bool init = false;
    if (init) {
        return;
    }

    init = true;
    real_api = fn();
    wrapped_api = *real_api;

    // Overrides
    static SERVER_CALLBACK_API callback = *wrapped_api.callback;
    callback.register_callback = register_callback;
    wrapped_api.callback = &callback;
}

static SERVER_HANDLE_V1 *get_wrapped_gsa() {
    return &wrapped_api;
}

/** ewouldblock_engine class */
class EWB_Engine : public EngineIface {
private:
    enum class Cmd {
        NONE,
        GET_INFO,
        ALLOCATE,
        REMOVE,
        GET,
        STORE,
        CAS,
        ARITHMETIC,
        LOCK,
        UNLOCK,
        FLUSH,
        GET_STATS,
        GET_META,
        UNKNOWN_COMMAND
    };

    const char* to_string(Cmd cmd);

    uint64_t (*get_connection_id)(gsl::not_null<const void*> cookie);

public:
    EWB_Engine(GET_SERVER_API gsa_);

    ~EWB_Engine() override;

    // Convert from a handle back to the read object.
    static EWB_Engine* to_engine(ENGINE_HANDLE* handle) {
        return reinterpret_cast<EWB_Engine*> (handle);
    }

    /* Returns true if the next command should have a fake error code injected.
     * @param func Address of the command function (get, store, etc).
     * @param cookie The cookie for the user's request.
     * @param[out] Error code to return.
     */
    bool should_inject_error(Cmd cmd, const void* cookie,
                             ENGINE_ERROR_CODE& err) {

        if (is_connection_suspended(cookie)) {
            err = ENGINE_EWOULDBLOCK;
            return true;
        }

        uint64_t id = get_connection_id(cookie);

        std::lock_guard<std::mutex> guard(cookie_map_mutex);

        auto iter = connection_map.find(id);
        if (iter == connection_map.end()) {
            return false;
        }

        if (iter->second.first != cookie) {
            // The cookie is different so it represents a different command
            connection_map.erase(iter);
            return false;
        }

        const bool inject = iter->second.second->should_inject_error(cmd, err);
        const bool add_to_pending_io_ops = iter->second.second->add_to_pending_io_ops();

        if (inject) {
            LOG_DEBUG("EWB_Engine: injecting error:{} for cmd:{}",
                      err,
                      to_string(cmd));

            if (err == ENGINE_EWOULDBLOCK && add_to_pending_io_ops) {
                // The server expects that if EWOULDBLOCK is returned then the
                // server should be notified in the future when the operation is
                // ready - so add this op to the pending IO queue.
                schedule_notification(iter->second.first);
            }
        }

        return inject;
    }

    /* Implementation of all the engine functions. ***************************/

    ENGINE_ERROR_CODE initialize(const char* config_str) override {
        // Extract the name of the real engine we will be proxying; then
        // create and initialize it.
        std::string config(config_str);
        auto seperator = config.find(";");
        std::string real_engine_name(config.substr(0, seperator));
        std::string real_engine_config;
        if (seperator != std::string::npos) {
            real_engine_config = config.substr(seperator + 1);
        }

        if ((real_engine_ref = load_engine(real_engine_name.c_str(), NULL)) ==
            NULL) {
            LOG_CRITICAL(
                    "ERROR: EWB_Engine::initialize(): Failed to load real "
                    "engine '{}'",
                    real_engine_name);
            abort();
        }

        if (!create_engine_instance(
                    real_engine_ref, get_wrapped_gsa, &real_handle)) {
            LOG_CRITICAL(
                    "ERROR: EWB_Engine::initialize(): Failed create "
                    "engine instance '{}'",
                    real_engine_name);
            abort();
        }

        real_engine = real_handle;

        engine_map[real_handle] = this;
        ENGINE_ERROR_CODE res =
                real_engine->initialize(real_engine_config.c_str());

        if (res == ENGINE_SUCCESS) {
            // For engine interface functions which cannot return EWOULDBLOCK,
            // and we otherwise don't want to interpose, we can simply use the
            // real_engine's functions directly.
            ENGINE_HANDLE_V1::item_set_cas = real_engine->item_set_cas;
            ENGINE_HANDLE_V1::set_item_info = real_engine->set_item_info;
            ENGINE_HANDLE_V1::item_set_datatype =
                    real_engine->item_set_datatype;
        }

        // Register a callback on DISCONNECT events, so we can delete
        // any stale elements from connection_map when a connection
        // DC's.
        real_api->callback->register_callback(
                this, ON_DISCONNECT, handle_disconnect, this);

        return res;
    }

    void destroy(bool force) override {
        real_engine->destroy(force);
        delete this;
    }

    cb::EngineErrorItemPair allocate(gsl::not_null<const void*> cookie,
                                     const DocKey& key,
                                     const size_t nbytes,
                                     const int flags,
                                     const rel_time_t exptime,
                                     uint8_t datatype,
                                     uint16_t vbucket) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::ALLOCATE, cookie, err)) {
            return cb::makeEngineErrorItemPair(cb::engine_errc(err));
        } else {
            return real_engine->allocate(
                    cookie, key, nbytes, flags, exptime, datatype, vbucket);
        }
    }

    std::pair<cb::unique_item_ptr, item_info> allocate_ex(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            size_t nbytes,
            size_t priv_nbytes,
            int flags,
            rel_time_t exptime,
            uint8_t datatype,
            uint16_t vbucket) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::ALLOCATE, cookie, err)) {
            throw cb::engine_error(cb::engine_errc(err), "ewb: injecting error");
        } else {
            return real_engine->allocate_ex(cookie,
                                            key,
                                            nbytes,
                                            priv_nbytes,
                                            flags,
                                            exptime,
                                            datatype,
                                            vbucket);
        }
    }

    ENGINE_ERROR_CODE remove(gsl::not_null<const void*> cookie,
                             const DocKey& key,
                             uint64_t& cas,
                             uint16_t vbucket,
                             mutation_descr_t& mut_info) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::REMOVE, cookie, err)) {
            return err;
        } else {
            return real_engine->remove(cookie, key, cas, vbucket, mut_info);
        }
    }

    void release(gsl::not_null<item*> item) override {
        LOG_DEBUG("EWB_Engine: release");

        if (item == &dcp_mutation_item) {
            // Ignore the DCP mutation, we own it (and don't track
            // refcounts on it).
        } else {
            return real_engine->release(item);
        }
    }

    cb::EngineErrorItemPair get(gsl::not_null<const void*> cookie,
                                const DocKey& key,
                                uint16_t vbucket,
                                DocStateFilter documentStateFilter) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::GET, cookie, err)) {
            return std::make_pair(
                    cb::engine_errc(err),
                    cb::unique_item_ptr{nullptr, cb::ItemDeleter{this}});
        } else {
            return real_engine->get(cookie, key, vbucket, documentStateFilter);
        }
    }

    cb::EngineErrorItemPair get_if(
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            uint16_t vbucket,
            std::function<bool(const item_info&)> filter) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::GET, cookie, err)) {
            return cb::makeEngineErrorItemPair(cb::engine_errc::would_block);
        } else {
            return real_engine->get_if(cookie, key, vbucket, filter);
        }
    }

    static cb::EngineErrorItemPair get_and_touch(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            const DocKey& key,
            uint16_t vbucket,
            uint32_t exptime) {
        EWB_Engine* ewb = to_engine(handle);
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (ewb->should_inject_error(Cmd::GET, cookie, err)) {
            return cb::makeEngineErrorItemPair(cb::engine_errc::would_block);
        } else {
            return ewb->real_engine->get_and_touch(
                    ewb->real_handle, cookie, key, vbucket, exptime);
        }
    }

    cb::EngineErrorItemPair get_locked(gsl::not_null<const void*> cookie,
                                       const DocKey& key,
                                       uint16_t vbucket,
                                       uint32_t lock_timeout) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::LOCK, cookie, err)) {
            return cb::makeEngineErrorItemPair(cb::engine_errc(err));
        } else {
            return real_engine->get_locked(cookie, key, vbucket, lock_timeout);
        }
    }

    cb::EngineErrorMetadataPair get_meta(gsl::not_null<const void*> cookie,
                                         const DocKey& key,
                                         uint16_t vbucket) override {
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (should_inject_error(Cmd::GET_META, cookie, err)) {
            return std::make_pair(cb::engine_errc(err), item_info());
        } else {
            return real_engine->get_meta(cookie, key, vbucket);
        }
    }

    static ENGINE_ERROR_CODE unlock(gsl::not_null<ENGINE_HANDLE*> handle,
                                    gsl::not_null<const void*> cookie,
                                    const DocKey& key,
                                    uint16_t vbucket,
                                    uint64_t cas) {
        EWB_Engine* ewb = to_engine(handle);
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (ewb->should_inject_error(Cmd::UNLOCK, cookie, err)) {
            return err;
        } else {
            return ewb->real_engine->unlock(ewb->real_handle, cookie, key,
                                            vbucket, cas);
        }
    }

    static ENGINE_ERROR_CODE store(gsl::not_null<ENGINE_HANDLE*> handle,
                                   gsl::not_null<const void*> cookie,
                                   gsl::not_null<item*> item,
                                   uint64_t& cas,
                                   ENGINE_STORE_OPERATION operation,
                                   DocumentState document_state) {
        EWB_Engine* ewb = to_engine(handle);
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        Cmd opcode = (operation == OPERATION_CAS) ? Cmd::CAS : Cmd::STORE;
        if (ewb->should_inject_error(opcode, cookie, err)) {
            return err;
        } else {
            return ewb->real_engine->store(ewb->real_handle, cookie, item, cas,
                                           operation, document_state);
        }
    }

    static cb::EngineErrorCasPair store_if(gsl::not_null<ENGINE_HANDLE*> handle,
                                           gsl::not_null<const void*> cookie,
                                           gsl::not_null<item*> item,
                                           uint64_t cas,
                                           ENGINE_STORE_OPERATION operation,
                                           cb::StoreIfPredicate predicate,
                                           DocumentState document_state) {
        EWB_Engine* ewb = to_engine(handle);
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        Cmd opcode = (operation == OPERATION_CAS) ? Cmd::CAS : Cmd::STORE;
        if (ewb->should_inject_error(opcode, cookie, err)) {
            return {cb::engine_errc(err), 0};
        } else {
            return ewb->real_engine->store_if(ewb->real_handle,
                                              cookie,
                                              item,
                                              cas,
                                              operation,
                                              predicate,
                                              document_state);
        }
    }

    static ENGINE_ERROR_CODE flush(gsl::not_null<ENGINE_HANDLE*> handle,
                                   gsl::not_null<const void*> cookie) {
        // Flush is a little different - it often returns EWOULDBLOCK, and
        // notify_io_complete() just tells the server it can issue it's *next*
        // command (i.e. no need to re-flush). Therefore just pass Flush
        // straight through for now.
        EWB_Engine* ewb = to_engine(handle);
        return ewb->real_engine->flush(ewb->real_handle, cookie);
    }

    static ENGINE_ERROR_CODE get_stats(gsl::not_null<ENGINE_HANDLE*> handle,
                                       gsl::not_null<const void*> cookie,
                                       cb::const_char_buffer key,
                                       ADD_STAT add_stat) {
        EWB_Engine* ewb = to_engine(handle);
        ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
        if (ewb->should_inject_error(Cmd::GET_STATS, cookie, err)) {
            return err;
        } else {
            return ewb->real_engine->get_stats(
                    ewb->real_handle, cookie, key, add_stat);
        }
    }

    static void reset_stats(gsl::not_null<ENGINE_HANDLE*> handle,
                            gsl::not_null<const void*> cookie) {
        EWB_Engine* ewb = to_engine(handle);
        return ewb->real_engine->reset_stats(ewb->real_handle, cookie);
    }

    /* Handle 'unknown_command'. In additional to wrapping calls to the
     * underlying real engine, this is also used to configure
     * ewouldblock_engine itself using he CMD_EWOULDBLOCK_CTL opcode.
     */
    static ENGINE_ERROR_CODE unknown_command(
            gsl::not_null<ENGINE_HANDLE*> handle,
            const void* cookie,
            gsl::not_null<protocol_binary_request_header*> request,
            ADD_RESPONSE response,
            DocNamespace doc_namespace) {
        EWB_Engine* ewb = to_engine(handle);

        if (request->request.opcode == PROTOCOL_BINARY_CMD_EWOULDBLOCK_CTL) {
            auto* req =
                    reinterpret_cast<request_ewouldblock_ctl*>(request.get());
            const EWBEngineMode mode = static_cast<EWBEngineMode>(ntohl(req->message.body.mode));
            const uint32_t value = ntohl(req->message.body.value);
            const ENGINE_ERROR_CODE injected_error =
                    static_cast<ENGINE_ERROR_CODE>(ntohl(req->message.body.inject_error));
            const std::string key((char*)req->bytes + sizeof(req->bytes),
                                  ntohs(req->message.header.request.keylen));

            std::shared_ptr<FaultInjectMode> new_mode = nullptr;

            // Validate mode, and construct new fault injector.
            switch (mode) {
                case EWBEngineMode::Next_N:
                    new_mode = std::make_shared<ErrOnNextN>(injected_error, value);
                    break;

                case EWBEngineMode::Random:
                    new_mode = std::make_shared<ErrRandom>(injected_error, value);
                    break;

                case EWBEngineMode::First:
                    new_mode = std::make_shared<ErrOnFirst>(injected_error);
                    break;

                case EWBEngineMode::Sequence:
                    new_mode = std::make_shared<ErrSequence>(injected_error, value);
                    break;

                case EWBEngineMode::No_Notify:
                    new_mode = std::make_shared<ErrOnNoNotify>(injected_error);
                    break;

                case EWBEngineMode::CasMismatch:
                    new_mode = std::make_shared<CASMismatch>(value);
                    break;

                case EWBEngineMode::IncrementClusterMapRevno:
                    ewb->clustermap_revno++;
                    response(nullptr, 0, nullptr, 0, nullptr, 0,
                             PROTOCOL_BINARY_RAW_BYTES,
                             PROTOCOL_BINARY_RESPONSE_SUCCESS, 0, cookie);
                    return ENGINE_SUCCESS;

                case EWBEngineMode::BlockMonitorFile:
                    return ewb->handleBlockMonitorFile(cookie, value, key,
                                                       response);

                case EWBEngineMode::Suspend:
                    return ewb->handleSuspend(cookie, value, response);

                case EWBEngineMode::Resume:
                    return ewb->handleResume(cookie, value, response);

                case EWBEngineMode::SetItemCas:
                    return ewb->setItemCas(cookie, key, value, response);
            }

            if (new_mode == nullptr) {
                LOG_WARNING(
                        "EWB_Engine::unknown_command(): "
                        "Got unexpected mode={} for EWOULDBLOCK_CTL, ",
                        (unsigned int)mode);
                response(nullptr, 0, nullptr, 0, nullptr, 0,
                         PROTOCOL_BINARY_RAW_BYTES,
                         PROTOCOL_BINARY_RESPONSE_EINVAL, /*cas*/0, cookie);
                return ENGINE_FAILED;
            } else {
                try {
                    LOG_DEBUG(
                            "EWB_Engine::unknown_command(): Setting EWB mode "
                            "to "
                            "{} for cookie {}",
                            new_mode->to_string(),
                            cookie);

                    uint64_t id = ewb->get_connection_id(cookie);

                    {
                        std::lock_guard<std::mutex> guard(ewb->cookie_map_mutex);
                        ewb->connection_map.erase(id);
                        ewb->connection_map.emplace(id, std::make_pair(cookie, new_mode));
                    }

                    response(nullptr, 0, nullptr, 0, nullptr, 0,
                             PROTOCOL_BINARY_RAW_BYTES,
                             PROTOCOL_BINARY_RESPONSE_SUCCESS, /*cas*/0, cookie);
                    return ENGINE_SUCCESS;
                } catch (std::bad_alloc&) {
                    return ENGINE_ENOMEM;
                }
            }
        } else {
            ENGINE_ERROR_CODE err = ENGINE_SUCCESS;
            if (ewb->should_inject_error(Cmd::UNKNOWN_COMMAND, cookie, err)) {
                return err;
            } else {
                return ewb->real_engine->unknown_command(ewb->real_handle, cookie,
                                                         request, response,
                                                         doc_namespace);
            }
        }
    }

    static void item_set_cas(gsl::not_null<ENGINE_HANDLE*> handle,
                             gsl::not_null<item*> item,
                             uint64_t cas) {
        // Should never be called as ENGINE_HANDLE_V1::item_set_cas is updated
        // to point to the real_engine once it is initialized. This function
        //only exists so there is a non-NULL value for
        // ENGINE_HANDLE_V1::item_set_cas initially to keep load_engine()
        // happy.
        abort();
    }

    static void item_set_datatype(gsl::not_null<ENGINE_HANDLE*>,
                                  gsl::not_null<item*> itm,
                                  protocol_binary_datatype_t datatype) {
        // Should never be called - set item_set_datatype().
        abort();
    }

    static bool get_item_info(gsl::not_null<ENGINE_HANDLE*> handle,
                              gsl::not_null<const item*> item,
                              gsl::not_null<item_info*> item_info) {
        EWB_Engine* ewb = to_engine(handle);
        LOG_DEBUG("EWB_Engine: get_item_info");

        // This function cannot return EWOULDBLOCK - just chain to the real
        // engine's function, unless it is a request for our special DCP item.
        if (item == &ewb->dcp_mutation_item) {
            item_info->cas = 0;
            item_info->vbucket_uuid = 0;
            item_info->seqno = 0;
            item_info->exptime = 0;
            item_info->nbytes =
                    gsl::narrow<uint32_t>(ewb->dcp_mutation_item.value.size());
            item_info->flags = 0;
            item_info->datatype = PROTOCOL_BINARY_DATATYPE_XATTR;
            item_info->nkey =
                    gsl::narrow<uint16_t>(ewb->dcp_mutation_item.key.size());
            item_info->key = ewb->dcp_mutation_item.key.c_str();
            item_info->value[0].iov_base = &ewb->dcp_mutation_item.value[0];
            item_info->value[0].iov_len = item_info->nbytes;
            return true;
        } else {
            return ewb->real_engine->get_item_info(
                    ewb->real_handle, item, item_info);
        }
    }

    static bool set_item_info(gsl::not_null<ENGINE_HANDLE*> handle,
                              gsl::not_null<item*> item,
                              gsl::not_null<const item_info*> item_info) {
        // Should never be called - set item_set_cas().
        abort();
    }

    static void handle_disconnect(const void* cookie,
                                  ENGINE_EVENT_TYPE type,
                                  const void* event_data,
                                  const void* cb_data) {
        cb_assert(event_data == NULL);
        EWB_Engine* ewb =
                reinterpret_cast<EWB_Engine*>(const_cast<void*>(cb_data));
        LOG_DEBUG("EWB_Engine::handle_disconnect");

        uint64_t id = ewb->get_connection_id(cookie);
        {
            std::lock_guard<std::mutex> guard(ewb->cookie_map_mutex);
            ewb->connection_map.erase(id);
        }
    }

    GET_SERVER_API gsa;

    // Actual engine we are proxying requests to.
    ENGINE_HANDLE*
            real_handle; // TODO: Remove real_handle as same as real_engine now.
    ENGINE_HANDLE_V1* real_engine;
    engine_reference* real_engine_ref;

    std::atomic_int clustermap_revno;

    /**
     * The method responsible for pushing all of the notify_io_complete
     * to the frontend. It is run by notify_io_thread and not intended to
     * be called by anyone else!.
     */
    void process_notifications();
    std::unique_ptr<Couchbase::Thread> notify_io_thread;

protected:
    /**
     * Handle the control message for block monitor file
     *
     * @param cookie The cookie executing the operation
     * @param id The identifier used to represent the cookie
     * @param file The file to monitor
     * @param response callback used to send a response to the client
     * @return The standard engine error codes
     */
    ENGINE_ERROR_CODE handleBlockMonitorFile(const void* cookie,
                                             uint32_t id,
                                             const std::string& file,
                                             ADD_RESPONSE response);

    /**
     * Handle the control message for suspend
     *
     * @param cookie The cookie executing the operation
     * @param id The identifier used to represent the cookie to resume
     *           (the use of a different id is to allow resume to
     *           be sent on a different connection)
     * @param response callback used to send a response to the client
     * @return The standard engine error codes
     */
    ENGINE_ERROR_CODE handleSuspend(const void* cookie,
                                    uint32_t id,
                                    ADD_RESPONSE response);

    /**
     * Handle the control message for resume
     *
     * @param cookie The cookie executing the operation
     * @param id The identifier representing the connection to resume
     * @param response callback used to send a response to the client
     * @return The standard engine error codes
     */
    ENGINE_ERROR_CODE handleResume(const void* cookie,
                                   uint32_t id,
                                   ADD_RESPONSE response);

    /**
     * @param cookie the cookie executing the operation
     * @param key ID of the item whose CAS should be changed
     * @param cas The new CAS
     * @param response Response callback used to send a response to the client
     * @return Standard engine error codes
     */
    ENGINE_ERROR_CODE setItemCas(const void *cookie,
                                 const std::string& key, uint32_t cas,
                                 ADD_RESPONSE response);

private:
    // Shared state between the main thread of execution and the background
    // thread processing pending io ops.
    std::mutex mutex;
    std::condition_variable condvar;
    std::queue<const void*> pending_io_ops;

    std::atomic<bool> stop_notification_thread;

    ///////////////////////////////////////////////////////////////////////////
    //             All of the methods used in the DCP interface              //
    //                                                                       //
    // We don't support mocking with the DCP interface yet, so all access to //
    // the DCP interface will be proxied down to the underlying engine.      //
    ///////////////////////////////////////////////////////////////////////////
    static ENGINE_ERROR_CODE dcp_step(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            gsl::not_null<struct dcp_message_producers*> producers);

    static ENGINE_ERROR_CODE dcp_open(gsl::not_null<ENGINE_HANDLE*> handle,
                                      gsl::not_null<const void*> cookie,
                                      uint32_t opaque,
                                      uint32_t seqno,
                                      uint32_t flags,
                                      cb::const_char_buffer name,
                                      cb::const_byte_buffer json);

    static ENGINE_ERROR_CODE dcp_add_stream(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            uint16_t vbucket,
            uint32_t flags);

    static ENGINE_ERROR_CODE dcp_close_stream(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            uint16_t vbucket);

    static ENGINE_ERROR_CODE dcp_stream_req(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t flags,
            uint32_t opaque,
            uint16_t vbucket,
            uint64_t start_seqno,
            uint64_t end_seqno,
            uint64_t vbucket_uuid,
            uint64_t snap_start_seqno,
            uint64_t snap_end_seqno,
            uint64_t* rollback_seqno,
            dcp_add_failover_log callback);

    static ENGINE_ERROR_CODE dcp_get_failover_log(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            uint16_t vbucket,
            dcp_add_failover_log callback);

    static ENGINE_ERROR_CODE dcp_stream_end(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            uint16_t vbucket,
            uint32_t flags);

    static ENGINE_ERROR_CODE dcp_snapshot_marker(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            uint16_t vbucket,
            uint64_t start_seqno,
            uint64_t end_seqno,
            uint32_t flags);

    static ENGINE_ERROR_CODE dcp_mutation(gsl::not_null<ENGINE_HANDLE*> handle,
                                          gsl::not_null<const void*> cookie,
                                          uint32_t opaque,
                                          const DocKey& key,
                                          cb::const_byte_buffer value,
                                          size_t priv_bytes,
                                          uint8_t datatype,
                                          uint64_t cas,
                                          uint16_t vbucket,
                                          uint32_t flags,
                                          uint64_t by_seqno,
                                          uint64_t rev_seqno,
                                          uint32_t expiration,
                                          uint32_t lock_time,
                                          cb::const_byte_buffer meta,
                                          uint8_t nru);

    static ENGINE_ERROR_CODE dcp_deletion(gsl::not_null<ENGINE_HANDLE*> handle,
                                          gsl::not_null<const void*> cookie,
                                          uint32_t opaque,
                                          const DocKey& key,
                                          cb::const_byte_buffer value,
                                          size_t priv_bytes,
                                          uint8_t datatype,
                                          uint64_t cas,
                                          uint16_t vbucket,
                                          uint64_t by_seqno,
                                          uint64_t rev_seqno,
                                          cb::const_byte_buffer meta);

    static ENGINE_ERROR_CODE dcp_deletion_v2(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            const DocKey& key,
            cb::const_byte_buffer value,
            size_t priv_bytes,
            uint8_t datatype,
            uint64_t cas,
            uint16_t vbucket,
            uint64_t by_seqno,
            uint64_t rev_seqno,
            uint32_t delete_time);

    static ENGINE_ERROR_CODE dcp_expiration(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            const DocKey& key,
            cb::const_byte_buffer value,
            size_t priv_bytes,
            uint8_t datatype,
            uint64_t cas,
            uint16_t vbucket,
            uint64_t by_seqno,
            uint64_t rev_seqno,
            cb::const_byte_buffer meta);

    static ENGINE_ERROR_CODE dcp_flush(gsl::not_null<ENGINE_HANDLE*> handle,
                                       gsl::not_null<const void*> cookie,
                                       uint32_t opaque,
                                       uint16_t vbucket);

    static ENGINE_ERROR_CODE dcp_set_vbucket_state(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            uint16_t vbucket,
            vbucket_state_t state);

    static ENGINE_ERROR_CODE dcp_noop(gsl::not_null<ENGINE_HANDLE*> handle,
                                      gsl::not_null<const void*> cookie,
                                      uint32_t opaque);

    static ENGINE_ERROR_CODE dcp_buffer_acknowledgement(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            uint16_t vbucket,
            uint32_t buffer_bytes);

    static ENGINE_ERROR_CODE dcp_control(gsl::not_null<ENGINE_HANDLE*> handle,
                                         gsl::not_null<const void*> cookie,
                                         uint32_t opaque,
                                         const void* key,
                                         uint16_t nkey,
                                         const void* value,
                                         uint32_t nvalue);

    static ENGINE_ERROR_CODE dcp_response_handler(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            const protocol_binary_response_header* response);

    static ENGINE_ERROR_CODE dcp_system_event(
            gsl::not_null<ENGINE_HANDLE*> handle,
            gsl::not_null<const void*> cookie,
            uint32_t opaque,
            uint16_t vbucket,
            mcbp::systemevent::id event,
            uint64_t bySeqno,
            cb::const_byte_buffer key,
            cb::const_byte_buffer eventData);

    static cb::engine_error collections_set_manifest(
            gsl::not_null<ENGINE_HANDLE*> handle, cb::const_char_buffer json);

    static cb::EngineErrorStringPair collections_get_manifest(
            gsl::not_null<ENGINE_HANDLE*> handle);

    static bool isXattrEnabled(gsl::not_null<ENGINE_HANDLE*> handle);

    static BucketCompressionMode getCompressionMode(gsl::not_null<ENGINE_HANDLE*> handle);

    static size_t getMaxItemSize(gsl::not_null<ENGINE_HANDLE*> handle);

    static float getMinCompressionRatio(gsl::not_null<ENGINE_HANDLE*> handle);

    // Base class for all fault injection modes.
    struct FaultInjectMode {
        FaultInjectMode(ENGINE_ERROR_CODE injected_error_)
          : injected_error(injected_error_) {}

        virtual bool add_to_pending_io_ops() {
            return true;
        }
        virtual bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) = 0;

        virtual std::string to_string() const = 0;

    protected:
        ENGINE_ERROR_CODE injected_error;
    };

    // Subclasses for each fault inject mode: /////////////////////////////////

    class ErrOnFirst : public FaultInjectMode {
    public:
        ErrOnFirst(ENGINE_ERROR_CODE injected_error_)
          : FaultInjectMode(injected_error_),
            prev_cmd(Cmd::NONE) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            // Block unless the previous command from this cookie
            // was the same - i.e. all of a connections' commands
            // will EWOULDBLOCK the first time they are called.
            bool inject = (prev_cmd != cmd);
            prev_cmd = cmd;
            if (inject) {
                err = injected_error;
            }
            return inject;
        }

        std::string to_string() const {
            return "ErrOnFirst inject_error=" + std::to_string(injected_error);
        }

    private:
        // Last command issued by this cookie.
        Cmd prev_cmd;
    };

    class ErrOnNextN : public FaultInjectMode {
    public:
        ErrOnNextN(ENGINE_ERROR_CODE injected_error_, uint32_t count_)
          : FaultInjectMode(injected_error_),
            count(count_) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            if (count > 0) {
                --count;
                err = injected_error;
                return true;
            } else {
                return false;
            }
        }

        std::string to_string() const {
            return std::string("ErrOnNextN") +
                   " inject_error=" + std::to_string(injected_error) +
                   " count=" + std::to_string(count);
        }

    private:
        // The count of commands issued that should return error.
        uint32_t count;
    };

    class ErrRandom : public FaultInjectMode {
    public:
        ErrRandom(ENGINE_ERROR_CODE injected_error_, uint32_t percentage_)
          : FaultInjectMode(injected_error_),
            percentage_to_err(percentage_) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<uint32_t> dis(1, 100);
            if (dis(gen) < percentage_to_err) {
                err = injected_error;
                return true;
            } else {
                return false;
            }
        }

        std::string to_string() const {
            return std::string("ErrRandom") +
                   " inject_error=" + std::to_string(injected_error) +
                   " percentage=" + std::to_string(percentage_to_err);
        }

    private:
        // Percentage chance that the specified error should be injected.
        uint32_t percentage_to_err;
    };

    class ErrSequence : public FaultInjectMode {
    public:
        ErrSequence(ENGINE_ERROR_CODE injected_error_, uint32_t sequence_)
            : FaultInjectMode(injected_error_),
              sequence(sequence_),
              pos(0) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            bool inject = false;
            if (pos < 32) {
                inject = (sequence & (1 << pos)) != 0;
                pos++;
            }
            if (inject) {
                err = injected_error;
            }
            return inject;
        }

        std::string to_string() const {
            std::stringstream ss;
            ss << "ErrSequence inject_error=" << injected_error
               << " sequence=0x" << std::hex << sequence
               << " pos=" << pos;
            return ss.str();
        }

    private:
        uint32_t sequence;
        uint32_t pos;
    };

    class ErrOnNoNotify : public FaultInjectMode {
        public:
            ErrOnNoNotify(ENGINE_ERROR_CODE injected_error_)
              : FaultInjectMode(injected_error_),
                issued_return_error(false) {}

            bool add_to_pending_io_ops() {return false;}
            bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
                if (!issued_return_error) {
                    issued_return_error = true;
                    err = injected_error;
                    return true;
                } else {
                    return false;
                }
            }

            std::string to_string() const {
                return std::string("ErrOnNoNotify") +
                       " inject_error=" + std::to_string(injected_error) +
                       " issued_return_error=" +
                       std::to_string(issued_return_error);
            }

        private:
            // Record of whether have yet issued return error.
            bool issued_return_error;
        };

    class CASMismatch : public FaultInjectMode {
    public:
        CASMismatch(uint32_t count_)
          : FaultInjectMode(ENGINE_KEY_EEXISTS),
            count(count_) {}

        bool should_inject_error(Cmd cmd, ENGINE_ERROR_CODE& err) {
            if (cmd == Cmd::CAS && (count > 0)) {
                --count;
                err = injected_error;
                return true;
            } else {
                return false;
            }
        }

        std::string to_string() const {
            return std::string("CASMismatch") +
                   " count=" + std::to_string(count);
        }

    private:
        uint32_t count;
    };

    // Map of connections (aka cookies) to their current mode.
    std::map<uint64_t, std::pair<const void*, std::shared_ptr<FaultInjectMode> > > connection_map;
    // Mutex for above map.
    std::mutex cookie_map_mutex;

    // Current DCP mutation `item`. We return the address of this
    // (in the dcp step() function) back to the server, and then in
    // get_item_info we check if the requested item is this one.
    class EwbDcpKey {
    public:
        EwbDcpKey()
            : key("k") {
            cb::xattr::Blob builder;
            builder.set("_ewb", "{\"internal\":true}");
            builder.set("meta", "{\"author\":\"jack\"}");
            const auto blob = builder.finalize();
            std::copy(blob.buf, blob.buf + blob.len, std::back_inserter(value));
            // MB24971 - the body is large as it increases the probability of
            // transit returning TransmitResult::SoftError
            const std::string body(1000, 'x');
            std::copy(body.begin(), body.end(), std::back_inserter(value));
        }

        std::string key;
        std::vector<uint8_t> value;
    } dcp_mutation_item;

    /**
     * The dcp_stream map is used to map a cookie to the count of objects
     * it should send on the stream.
     *
     * Each entry in here constists of a pair containing a boolean specifying
     * if the stream is opened or not, and a count of how many times we should
     * return data
     */
    std::map<const void*, std::pair<bool, uint64_t>> dcp_stream;

    friend class BlockMonitorThread;
    std::map<uint32_t, const void*> suspended_map;
    std::mutex suspended_map_mutex;

    bool suspend(const void* cookie, uint32_t id) {
        {
            std::lock_guard<std::mutex> guard(suspended_map_mutex);
            auto iter = suspended_map.find(id);
            if (iter == suspended_map.cend()) {
                suspended_map[id] = cookie;
                return true;
            }
        }

        return false;
    }

    bool resume(uint32_t id) {
        const void* cookie = nullptr;
        {
            std::lock_guard<std::mutex> guard(suspended_map_mutex);
            auto iter = suspended_map.find(id);
            if (iter == suspended_map.cend()) {
                return false;
            }
            cookie = iter->second;
            suspended_map.erase(iter);
        }


        schedule_notification(cookie);
        return true;
    }

    bool is_connection_suspended(const void* cookie) {
        std::lock_guard<std::mutex> guard(suspended_map_mutex);
        for (const auto c : suspended_map) {
            if (c.second == cookie) {
                LOG_DEBUG(
                        "Connection {} with id {} should be suspended for "
                        "engine {}",
                        c.second,
                        c.first,
                        (void*)this);

                return true;
            }
        }
        return false;
    }

    void schedule_notification(const void* cookie) {
        {
            std::lock_guard<std::mutex> guard(mutex);
            pending_io_ops.push(cookie);
        }
        LOG_DEBUG("EWB_Engine: connection {} should be resumed for engine {}",
                  (void*)cookie,
                  (void*)this);

        condvar.notify_one();
    }

    // Vector to keep track of the threads we've started to ensure
    // we don't leak memory ;-)
    std::mutex threads_mutex;
    std::vector<std::unique_ptr<Couchbase::Thread> > threads;
};

EWB_Engine::EWB_Engine(GET_SERVER_API gsa_)
  : gsa(gsa_),
    real_engine(NULL),
    real_engine_ref(nullptr),
    notify_io_thread(new NotificationThread(*this))
{
    init_wrapped_api(gsa);

    ENGINE_HANDLE_V1::get_and_touch = get_and_touch;
    ENGINE_HANDLE_V1::unlock = unlock;
    ENGINE_HANDLE_V1::store = store;
    ENGINE_HANDLE_V1::store_if = store_if;
    ENGINE_HANDLE_V1::flush = flush;
    ENGINE_HANDLE_V1::get_stats = get_stats;
    ENGINE_HANDLE_V1::reset_stats = reset_stats;
    ENGINE_HANDLE_V1::unknown_command = unknown_command;
    ENGINE_HANDLE_V1::item_set_cas = item_set_cas;
    ENGINE_HANDLE_V1::item_set_datatype = item_set_datatype;
    ENGINE_HANDLE_V1::get_item_info = get_item_info;
    ENGINE_HANDLE_V1::set_item_info = set_item_info;
    ENGINE_HANDLE_V1::set_log_level = NULL;

    ENGINE_HANDLE_V1::dcp = {};
    ENGINE_HANDLE_V1::dcp.step = dcp_step;
    ENGINE_HANDLE_V1::dcp.open = dcp_open;
    ENGINE_HANDLE_V1::dcp.stream_req = dcp_stream_req;
    ENGINE_HANDLE_V1::dcp.add_stream = dcp_add_stream;
    ENGINE_HANDLE_V1::dcp.close_stream = dcp_close_stream;
    ENGINE_HANDLE_V1::dcp.buffer_acknowledgement = dcp_buffer_acknowledgement;
    ENGINE_HANDLE_V1::dcp.control = dcp_control;
    ENGINE_HANDLE_V1::dcp.get_failover_log = dcp_get_failover_log;
    ENGINE_HANDLE_V1::dcp.stream_end = dcp_stream_end;
    ENGINE_HANDLE_V1::dcp.snapshot_marker = dcp_snapshot_marker;
    ENGINE_HANDLE_V1::dcp.mutation = dcp_mutation;
    ENGINE_HANDLE_V1::dcp.deletion = dcp_deletion;
    ENGINE_HANDLE_V1::dcp.deletion_v2 = dcp_deletion_v2;
    ENGINE_HANDLE_V1::dcp.expiration = dcp_expiration;
    ENGINE_HANDLE_V1::dcp.flush = dcp_flush;
    ENGINE_HANDLE_V1::dcp.set_vbucket_state = dcp_set_vbucket_state;
    ENGINE_HANDLE_V1::dcp.noop = dcp_noop;
    ENGINE_HANDLE_V1::dcp.response_handler = dcp_response_handler;
    ENGINE_HANDLE_V1::dcp.system_event = dcp_system_event;

    ENGINE_HANDLE_V1::collections = {};
    ENGINE_HANDLE_V1::collections.set_manifest = collections_set_manifest;
    ENGINE_HANDLE_V1::collections.get_manifest = collections_get_manifest;

    ENGINE_HANDLE_V1::isXattrEnabled = isXattrEnabled;
    ENGINE_HANDLE_V1::getCompressionMode = getCompressionMode;
    ENGINE_HANDLE_V1::getMaxItemSize = getMaxItemSize;
    ENGINE_HANDLE_V1::getMinCompressionRatio = getMinCompressionRatio;

    clustermap_revno = 1;

    get_connection_id = gsa()->cookie->get_connection_id;

    stop_notification_thread.store(false);
    notify_io_thread->start();
}

static void register_callback(ENGINE_HANDLE *eh, ENGINE_EVENT_TYPE type,
                              EVENT_CALLBACK cb, const void *cb_data) {
    const auto& p = engine_map.find(eh);
    if (p == engine_map.end()) {
        std::cerr << "Can't find EWB corresponding to " << std::hex << eh << std::endl;
        for (const auto& pair : engine_map) {
            std::cerr << "EH: " << std::hex << pair.first << " = EWB: " << std::hex << pair.second << std::endl;
        }
        abort();
    }
    cb_assert(p != engine_map.end());
    auto wrapped_eh = reinterpret_cast<ENGINE_HANDLE*>(p->second);
    real_api->callback->register_callback(wrapped_eh, type, cb, cb_data);
}

EWB_Engine::~EWB_Engine() {
    engine_map.erase(real_handle);
    cb_free(real_engine_ref);
    stop_notification_thread = true;
    condvar.notify_all();
    notify_io_thread->waitForState(Couchbase::ThreadState::Zombie);
}

ENGINE_ERROR_CODE EWB_Engine::dcp_step(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        gsl::not_null<struct dcp_message_producers*> producers) {
    EWB_Engine* ewb = to_engine(handle);
    auto stream = ewb->dcp_stream.find(cookie);
    if (stream != ewb->dcp_stream.end()) {
        auto& count = stream->second.second;
        // If the stream is enabled and we have data to send..
        if (stream->second.first && count > 0) {
            // This is using the internal dcp implementation which always
            // send the same item back
            auto ret = producers->mutation(cookie,
                                           0xdeadbeef /*opqaue*/,
                                           &ewb->dcp_mutation_item,
                                           0 /*vb*/,
                                           0 /*by_seqno*/,
                                           0 /*rev_seqno*/,
                                           0 /*lock_time*/,
                                           nullptr /*meta*/,
                                           0 /*nmeta*/,
                                           0 /*nru*/,
                                           0 /*collection_len*/);
            --count;
            if (ret == ENGINE_SUCCESS) {
                return ENGINE_WANT_MORE;
            }
            return ret;
        }
        return ENGINE_SUCCESS;
    }

    if (ewb->real_engine->dcp.step == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.step(ewb->real_handle, cookie, producers);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_open(gsl::not_null<ENGINE_HANDLE*> handle,
                                       gsl::not_null<const void*> cookie,
                                       uint32_t opaque,
                                       uint32_t seqno,
                                       uint32_t flags,
                                       cb::const_char_buffer name,
                                       cb::const_byte_buffer json) {
    EWB_Engine* ewb = to_engine(handle);
    std::string nm = cb::to_string(name);
    if (nm.find("ewb_internal") == 0) {
        // Yeah, this is a request for the internal "magic" DCP stream
        // The user could specify the iteration count by adding a colon
        // at the end...
        auto idx = nm.rfind(":");

        if (idx != nm.npos) {
            ewb->dcp_stream[cookie] =
                    std::make_pair(false, std::stoull(nm.substr(idx + 1)));
        } else {
            ewb->dcp_stream[cookie] =
                    std::make_pair(false, std::numeric_limits<uint64_t>::max());
        }
        return ENGINE_SUCCESS;
    }

    if (ewb->real_engine->dcp.open == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.open(ewb->real_handle,
                                          cookie,
                                          opaque,
                                          seqno,
                                          flags,
                                          name,
                                          json);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_stream_req(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t flags,
        uint32_t opaque,
        uint16_t vbucket,
        uint64_t start_seqno,
        uint64_t end_seqno,
        uint64_t vbucket_uuid,
        uint64_t snap_start_seqno,
        uint64_t snap_end_seqno,
        uint64_t* rollback_seqno,
        dcp_add_failover_log callback) {
    EWB_Engine* ewb = to_engine(handle);
    auto stream = ewb->dcp_stream.find(cookie.get());
    if (stream != ewb->dcp_stream.end()) {
        // This is a client of our internal streams.. just let it pass
        if (start_seqno == 1) {
            *rollback_seqno = 0;
            return ENGINE_ROLLBACK;
        }
        // Start the stream
        stream->second.first = true;
        return ENGINE_SUCCESS;
    }

    if (ewb->real_engine->dcp.stream_req == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.stream_req(ewb->real_handle, cookie,
                                                flags, opaque, vbucket,
                                                start_seqno, end_seqno,
                                                vbucket_uuid, snap_start_seqno,
                                                snap_end_seqno, rollback_seqno,
                                                callback);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_add_stream(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        uint16_t vbucket,
        uint32_t flags) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.add_stream == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.add_stream(ewb->real_handle, cookie,
                                                opaque, vbucket, flags);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_close_stream(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        uint16_t vbucket) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.close_stream == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.close_stream(ewb->real_handle, cookie,
                                                opaque, vbucket);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_get_failover_log(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        uint16_t vbucket,
        dcp_add_failover_log callback) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.get_failover_log == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.get_failover_log(ewb->real_handle,
                                                      cookie,
                                                      opaque,
                                                      vbucket,
                                                      callback);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_stream_end(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        uint16_t vbucket,
        uint32_t flags) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.stream_end == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.stream_end(ewb->real_handle, cookie,
                                                opaque, vbucket, flags);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_snapshot_marker(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        uint16_t vbucket,
        uint64_t start_seqno,
        uint64_t end_seqno,
        uint32_t flags) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.snapshot_marker == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.snapshot_marker(ewb->real_handle, cookie,
                                                     opaque, vbucket,
                                                     start_seqno, end_seqno,
                                                     flags);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_mutation(gsl::not_null<ENGINE_HANDLE*> handle,
                                           gsl::not_null<const void*> cookie,
                                           uint32_t opaque,
                                           const DocKey& key,
                                           cb::const_byte_buffer value,
                                           size_t priv_bytes,
                                           uint8_t datatype,
                                           uint64_t cas,
                                           uint16_t vbucket,
                                           uint32_t flags,
                                           uint64_t by_seqno,
                                           uint64_t rev_seqno,
                                           uint32_t expiration,
                                           uint32_t lock_time,
                                           cb::const_byte_buffer meta,
                                           uint8_t nru) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.mutation == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.mutation(ewb->real_handle, cookie, opaque,
                                              key, value, priv_bytes, datatype,
                                              cas, vbucket, flags, by_seqno,
                                              rev_seqno, expiration, lock_time,
                                              meta, nru);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_deletion(gsl::not_null<ENGINE_HANDLE*> handle,
                                           gsl::not_null<const void*> cookie,
                                           uint32_t opaque,
                                           const DocKey& key,
                                           cb::const_byte_buffer value,
                                           size_t priv_bytes,
                                           uint8_t datatype,
                                           uint64_t cas,
                                           uint16_t vbucket,
                                           uint64_t by_seqno,
                                           uint64_t rev_seqno,
                                           cb::const_byte_buffer meta) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.deletion == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.deletion(ewb->real_handle, cookie, opaque,
                                              key, value, priv_bytes, datatype,
                                              cas, vbucket, by_seqno, rev_seqno,
                                              meta);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_deletion_v2(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        const DocKey& key,
        cb::const_byte_buffer value,
        size_t priv_bytes,
        uint8_t datatype,
        uint64_t cas,
        uint16_t vbucket,
        uint64_t by_seqno,
        uint64_t rev_seqno,
        uint32_t delete_time) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.deletion_v2 == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.deletion_v2(ewb->real_handle,
                                                 cookie,
                                                 opaque,
                                                 key,
                                                 value,
                                                 priv_bytes,
                                                 datatype,
                                                 cas,
                                                 vbucket,
                                                 by_seqno,
                                                 rev_seqno,
                                                 delete_time);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_expiration(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        const DocKey& key,
        cb::const_byte_buffer value,
        size_t priv_bytes,
        uint8_t datatype,
        uint64_t cas,
        uint16_t vbucket,
        uint64_t by_seqno,
        uint64_t rev_seqno,
        cb::const_byte_buffer meta) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.expiration == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.expiration(ewb->real_handle, cookie, opaque,
                                                key, value, priv_bytes, datatype,
                                                cas, vbucket, by_seqno, rev_seqno,
                                                meta);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_flush(gsl::not_null<ENGINE_HANDLE*> handle,
                                        gsl::not_null<const void*> cookie,
                                        uint32_t opaque,
                                        uint16_t vbucket) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.flush == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.flush(ewb->real_handle,
                                                cookie,
                                                opaque,
                                                vbucket);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_set_vbucket_state(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        uint16_t vbucket,
        vbucket_state_t state) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.set_vbucket_state == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.set_vbucket_state(ewb->real_handle,
                                                cookie,
                                                opaque,
                                                vbucket,
                                                state);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_noop(gsl::not_null<ENGINE_HANDLE*> handle,
                                       gsl::not_null<const void*> cookie,
                                       uint32_t opaque) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.noop == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.noop(ewb->real_handle, cookie, opaque);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_buffer_acknowledgement(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        uint16_t vbucket,
        uint32_t buffer_bytes) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.buffer_acknowledgement == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.buffer_acknowledgement(ewb->real_handle,
                                                            cookie,
                                                            opaque,
                                                            vbucket,
                                                            buffer_bytes);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_control(gsl::not_null<ENGINE_HANDLE*> handle,
                                          gsl::not_null<const void*> cookie,
                                          uint32_t opaque,
                                          const void* key,
                                          uint16_t nkey,
                                          const void* value,
                                          uint32_t nvalue) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.control == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.control(ewb->real_handle, cookie,
                                             opaque, key, nkey, value, nvalue);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_response_handler(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const protocol_binary_response_header* response) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.response_handler == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.response_handler(ewb->real_handle,
                                                      cookie,
                                                      response);
    }
}

ENGINE_ERROR_CODE EWB_Engine::dcp_system_event(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        uint32_t opaque,
        uint16_t vbucket,
        mcbp::systemevent::id event,
        uint64_t bySeqno,
        cb::const_byte_buffer key,
        cb::const_byte_buffer eventData) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->dcp.response_handler == nullptr) {
        return ENGINE_ENOTSUP;
    } else {
        return ewb->real_engine->dcp.system_event(ewb->real_handle,
                                                  cookie,
                                                  opaque,
                                                  vbucket,
                                                  event,
                                                  bySeqno,
                                                  key,
                                                  eventData);
    }
}

cb::engine_error EWB_Engine::collections_set_manifest(
        gsl::not_null<ENGINE_HANDLE*> handle, cb::const_char_buffer json) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->collections.set_manifest == nullptr) {
        return {cb::engine_errc::not_supported,
                "EWB_Engine::collections_set_manifest"};
    } else {
        return ewb->real_engine->collections.set_manifest(ewb->real_handle,
                                                          json);
    }
}

cb::EngineErrorStringPair EWB_Engine::collections_get_manifest(
        gsl::not_null<ENGINE_HANDLE*> handle) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->collections.get_manifest == nullptr) {
        return {cb::engine_errc::not_supported,
                "EWB_Engine::collections_get_manifest"};
    } else {
        return ewb->real_engine->collections.get_manifest(ewb->real_handle);
    }
}

bool EWB_Engine::isXattrEnabled(gsl::not_null<ENGINE_HANDLE*> handle) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->isXattrEnabled == nullptr) {
        return false;
    } else {
        return ewb->real_engine->isXattrEnabled(ewb->real_handle);
    }
}

BucketCompressionMode EWB_Engine::getCompressionMode(gsl::not_null<ENGINE_HANDLE*> handle) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->getCompressionMode == nullptr) {
        return BucketCompressionMode::Off;
    } else {
        return ewb->real_engine->getCompressionMode(ewb->real_handle);
    }
}

size_t EWB_Engine::getMaxItemSize(gsl::not_null<ENGINE_HANDLE*> handle) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->getMaxItemSize == nullptr) {
        return default_max_item_size;
    } else {
        return ewb->real_engine->getMaxItemSize(ewb->real_handle);
    }
}

float EWB_Engine::getMinCompressionRatio(gsl::not_null<ENGINE_HANDLE*> handle) {
    EWB_Engine* ewb = to_engine(handle);
    if (ewb->real_engine->getMinCompressionRatio == nullptr) {
        return default_min_compression_ratio;
    } else {
        return ewb->real_engine->getMinCompressionRatio(ewb->real_handle);
    }
}

ENGINE_ERROR_CODE create_instance(GET_SERVER_API gsa, ENGINE_HANDLE** handle) {
    try {
        EWB_Engine* engine = new EWB_Engine(gsa);
        *handle = reinterpret_cast<ENGINE_HANDLE*> (engine);
        return ENGINE_SUCCESS;

    } catch (std::exception& e) {
        auto logger = gsa()->log->get_logger();
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "EWB_Engine: failed to create engine: %s", e.what());
        return ENGINE_FAILED;
    }

}

void destroy_engine(void) {
    // nothing todo.
}

const char* EWB_Engine::to_string(const Cmd cmd) {
    switch (cmd) {
    case Cmd::NONE:
        return "NONE";
    case Cmd::GET_INFO:
        return "GET_INFO";
    case Cmd::GET_META:
        return "GET_META";
    case Cmd::ALLOCATE:
        return "ALLOCATE";
    case Cmd::REMOVE:
        return "REMOVE";
    case Cmd::GET:
        return "GET";
    case Cmd::STORE:
        return "STORE";
    case Cmd::CAS:
        return "CAS";
    case Cmd::ARITHMETIC:
        return "ARITHMETIC";
    case Cmd::FLUSH:
        return "FLUSH";
    case Cmd::GET_STATS:
        return "GET_STATS";
    case Cmd::UNKNOWN_COMMAND:
        return "UNKNOWN_COMMAND";
    case Cmd::LOCK:
        return "LOCK";
    case Cmd::UNLOCK:
        return "UNLOCK";
    }
    throw std::invalid_argument("EWB_Engine::to_string() Unknown command");
}

void EWB_Engine::process_notifications() {
    SERVER_HANDLE_V1* server = gsa();
    LOG_DEBUG("EWB_Engine: notification thread running for engine {}",
              (void*)this);
    std::unique_lock<std::mutex> lk(mutex);
    while (!stop_notification_thread) {
        condvar.wait(lk);
        while (!pending_io_ops.empty()) {
            const void* cookie = pending_io_ops.front();
            pending_io_ops.pop();
            lk.unlock();
            LOG_DEBUG("EWB_Engine: notify {}", cookie);
            server->cookie->notify_io_complete(cookie, ENGINE_SUCCESS);
            lk.lock();
        }
    }

    LOG_DEBUG("EWB_Engine: notification thread stopping for engine {}",
              (void*)this);
}

void NotificationThread::run() {
    setRunning();
    engine.process_notifications();
}

ENGINE_ERROR_CODE EWB_Engine::handleBlockMonitorFile(const void* cookie,
                                                     uint32_t id,
                                                     const std::string& file,
                                                     ADD_RESPONSE response) {
    if (file.empty()) {
        return ENGINE_EINVAL;
    }

    if (!cb::io::isFile(file)) {
        return ENGINE_KEY_ENOENT;
    }

    if (!suspend(cookie, id)) {
        LOG_WARNING(
                "EWB_Engine::handleBlockMonitorFile(): "
                "Id {} already registered",
                id);
        return ENGINE_KEY_EEXISTS;
    }

    try {
        std::unique_ptr<Couchbase::Thread> thread(
                new BlockMonitorThread(*this, id, file));
        thread->start();
        std::lock_guard<std::mutex> guard(threads_mutex);
        threads.emplace_back(thread.release());
    } catch (std::exception& e) {
        LOG_WARNING(
                "EWB_Engine::handleBlockMonitorFile(): Failed to create "
                "block monitor thread: {}",
                e.what());
        return ENGINE_FAILED;
    }

    LOG_DEBUG(
            "Registered connection {} (engine {}) as {} to be"
            " suspended. Monitor file {}",
            cookie,
            (void*)this,
            id,
            file.c_str());

    response(nullptr, 0, nullptr, 0, nullptr, 0,
             PROTOCOL_BINARY_RAW_BYTES,
             PROTOCOL_BINARY_RESPONSE_SUCCESS, /*cas*/0, cookie);
    return ENGINE_SUCCESS;
}

ENGINE_ERROR_CODE EWB_Engine::handleSuspend(const void* cookie,
                                            uint32_t id,
                                            ADD_RESPONSE response) {
    if (suspend(cookie, id)) {
        LOG_DEBUG("Registered connection {} as {} to be suspended", cookie, id);
        response(nullptr, 0, nullptr, 0, nullptr, 0,
                 PROTOCOL_BINARY_RAW_BYTES,
                 PROTOCOL_BINARY_RESPONSE_SUCCESS, /*cas*/0, cookie);
        return ENGINE_SUCCESS;
    } else {
        LOG_WARNING("EWB_Engine::handleSuspend(): Id {} already registered",
                    id);
        return ENGINE_KEY_EEXISTS;
    }
}

ENGINE_ERROR_CODE EWB_Engine::handleResume(const void* cookie, uint32_t id,
                                           ADD_RESPONSE response) {
    if (resume(id)) {
        LOG_DEBUG("Connection with id {} will be resumed", id);
        response(nullptr, 0, nullptr, 0, nullptr, 0,
                 PROTOCOL_BINARY_RAW_BYTES,
                 PROTOCOL_BINARY_RESPONSE_SUCCESS, /*cas*/0, cookie);
        return ENGINE_SUCCESS;
    } else {
        LOG_WARNING(
                "EWB_Engine::unknown_command(): No "
                "connection registered with id {}",
                id);
        return ENGINE_EINVAL;
    }
}

ENGINE_ERROR_CODE EWB_Engine::setItemCas(const void *cookie,
                                         const std::string& key,
                                         uint32_t cas,
                                         ADD_RESPONSE response) {
    uint64_t cas64 = cas;
    if (cas == static_cast<uint32_t>(-1)) {
        cas64 = LOCKED_CAS;
    }

    auto rv = real_engine->get(cookie,
                               DocKey{key, DocNamespace::DefaultCollection},
                               0,
                               DocStateFilter::Alive);
    if (rv.first != cb::engine_errc::success) {
        return ENGINE_ERROR_CODE(rv.first);
    }

    // item_set_cas has no return value!
    real_engine->item_set_cas(real_handle, rv.second.get(), cas64);
    response(nullptr, 0, nullptr, 0, nullptr, 0,
             PROTOCOL_BINARY_RAW_BYTES,
             PROTOCOL_BINARY_RESPONSE_SUCCESS, 0, cookie);
    return ENGINE_SUCCESS;
}

void BlockMonitorThread::run() {
    setRunning();

    LOG_DEBUG("Block monitor for file {} started", file);

    // @todo Use the file monitoring API's to avoid this "busy" loop
    while (cb::io::isFile(file)) {
        usleep(100);
    }

    LOG_DEBUG("Block monitor for file {} stopping (file is gone)", file);
    engine.resume(id);
}
