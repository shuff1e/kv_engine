/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef EP_ENGINE_H
#define EP_ENGINE_H 1

#include "locks.hh"
#include "ep.hh"
#include "flusher.hh"
#include "sqlite-kvstore.hh"
#include "ep_extension.h"
#include <memcached/util.h>
#include "tapclient.hh"

#include <cstdio>
#include <map>
#include <list>
#include <errno.h>

#include "command_ids.h"

#define NUMBER_OF_SHARDS 4

extern "C" {
    EXPORT_FUNCTION
    ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                      GET_SERVER_API get_server_api,
                                      ENGINE_HANDLE **handle);
    void EvpHandleDisconnect(const void *cookie,
                             ENGINE_EVENT_TYPE type,
                             const void *event_data,
                             const void *cb_data);
    void *EvpNotifyTapIo(void*arg);
    void EvpHandleTapCallback(const void *cookie,
                              ENGINE_EVENT_TYPE type,
                              const void *event_data,
                              const void *cb_data);
    void EvpClockHandler(const void *cookie,
                         ENGINE_EVENT_TYPE type,
                         const void *event_data,
                         const void *cb_data);
}

#ifdef linux
/* /usr/include/netinet/in.h defines macros from ntohs() to _bswap_nn to
 * optimize the conversion functions, but the prototypes generate warnings
 * from gcc. The conversion methods isn't the bottleneck for my app, so
 * just remove the warnings by undef'ing the optimization ..
 */
#undef ntohs
#undef ntohl
#undef htons
#undef htonl
#endif

class BoolCallback : public Callback<bool> {
public:
    void callback(bool &theValue) {
        value = theValue;
    }

    bool getValue() {
        return value;
    }

private:
    bool value;
};

class BoolPairCallback : public Callback<std::pair<bool, int64_t> > {
public:
    void callback(std::pair<bool, int64_t> &theValue) {
        value = theValue.first;
    }

    bool getValue() {
        return value;
    }

private:
    bool value;
};

// Forward decl
class BinaryMessage;
class EventuallyPersistentEngine;

class LookupCallback : public Callback<GetValue> {
public:
    LookupCallback(EventuallyPersistentEngine *e, const void* c) :
       engine(e), cookie(c) {}

    virtual void callback(GetValue &value);
private:
    EventuallyPersistentEngine *engine;
    const void *cookie;
};

static void add_casted_stat(const char *k, const char *v,
                            ADD_STAT add_stat, const void *cookie) {
    add_stat(k, static_cast<uint16_t>(strlen(k)),
             v, static_cast<uint32_t>(strlen(v)), cookie);
}

static void add_casted_stat(const char *k, uint64_t v,
                            ADD_STAT add_stat, const void *cookie) {
    std::stringstream vals;
    vals << v;
    add_casted_stat(k, vals.str().c_str(), add_stat, cookie);
}

template <typename T>
static void add_casted_stat(const char *k, const Atomic<T> &v,
                            ADD_STAT add_stat, const void *cookie) {
    add_casted_stat(k, v.get(), add_stat, cookie);
}

class StatVBucketVisitor : public VBucketVisitor {
public:
    StatVBucketVisitor(const void *c, ADD_STAT a) : cookie(c), add_stat(a) {}

    bool visitBucket(uint16_t vbid, vbucket_state_t state) {
        char buf[16];
        snprintf(buf, sizeof(buf), "vb_%d", vbid);
        add_casted_stat(buf, VBucket::toString(state), add_stat, cookie);
        return false;
    }

    void visit(StoredValue* v) {
        (void)v;
        assert(false); // this does not happen
    }

private:
    const void *cookie;
    ADD_STAT add_stat;
};

/**
 * The tap stream may include other events than data mutation events,
 * but the data structures in the TapConnection does only store a key
 * for the item to store. We don't want to add more data to those elements,
 * because that could potentially consume a lot of memory (the tap queue
 * may have a lot of elements).
 */
class TapVBucketEvent {
public:
    /**
     * Create a new instance of the TapVBucketEvent and initialize
     * its members.
     * @param ev Type of event
     * @param b The bucket this event belongs to
     * @param s The state change for this event
     */
    TapVBucketEvent(tap_event_t ev, uint16_t b, vbucket_state_t s) :
        event(ev), vbucket(b), state(s) {}
    tap_event_t event;
    uint16_t vbucket;
    vbucket_state_t state;
};

/**
 * Class used by the EventuallyPersistentEngine to keep track of all
 * information needed per Tap connection.
 */
class TapConnection {
friend class EventuallyPersistentEngine;
friend class BackFillVisitor;
private:
    /**
     * Add a new item to the tap queue.
     * The item may be ignored if the TapConnection got a vbucket filter
     * associated and the item's vbucket isn't part of the filter.
     *
     * @return true if the the queue was empty
     */
    bool addEvent(const QueuedItem &it) {
        if (vbucketFilter.size() == 0 || find(vbucketFilter.begin(),
                                              vbucketFilter.end(),
                                              it.getVBucketId()) != vbucketFilter.end()) {
            bool wasEmpty = queue->empty();
            std::pair<std::set<QueuedItem>::iterator, bool> ret;
            ret = queue_set->insert(it);
            if (ret.second) {
                queue->push_back(it);
            }
            return wasEmpty;
        } else {
            return queue->empty();
        }
    }

    /**
     * Add a key to the tap queue.
     * @return true if the the queue was empty
     */
    bool addEvent(const std::string &key, uint16_t vbid) {
        return addEvent(QueuedItem(key, vbid));
    }

    QueuedItem next() {
        assert(!empty());
        QueuedItem qi = queue->front();
        queue->pop_front();
        queue_set->erase(qi);
        ++recordsFetched;
        return qi;
    }

    /**
     * Add a new high priority TapVBucketEvent to this TapConnection. A high
     * priority TapVBucketEvent will bypass the the normal queue of events to
     * be sent to the client, and be sent the next time it is possible to
     * send data over the tap connection.
     */
    void addVBucketHighPriority(TapVBucketEvent &ev) {
        vBucketHighPriority.push(ev);
    }

    /**
     * Get the next high priority TapVBucketEvent for this TapConnection.
     */
    TapVBucketEvent nextVBucketHighPriority() {
        TapVBucketEvent ret(TAP_PAUSE, 0, active);
        if (!vBucketHighPriority.empty()) {
            ret = vBucketHighPriority.front();
            vBucketHighPriority.pop();
            ++recordsFetched;
        }
        return ret;
    }

    /**
     * Add a new low priority TapVBucketEvent to this TapConnection. A low
     * priority TapVBucketEvent will only be sent when the tap connection
     * doesn't have any other events to send.
     */
    void addVBucketLowPriority(TapVBucketEvent &ev) {
        vBucketLowPriority.push(ev);
    }

    /**
     * Get the next low priority TapVBucketEvent for this TapConnection.
     */
    TapVBucketEvent nextVBucketLowPriority() {
        TapVBucketEvent ret(TAP_PAUSE, 0, active);
        if (!vBucketLowPriority.empty()) {
            ret = vBucketLowPriority.front();
            vBucketLowPriority.pop();
            ++recordsFetched;
        }
        return ret;
    }

    bool empty() {
        return queue->empty();
    }

    void flush() {
        pendingFlush = true;
        /* No point of keeping the rep queue when someone wants to flush it */
        queue->clear();
        queue_set->clear();
    }

    bool shouldFlush() {
        bool ret = pendingFlush;
        pendingFlush = false;
        return ret;
    }

    void replaceQueues(std::list<QueuedItem> *q,
                       std::set<QueuedItem> *qs) {
        delete queue_set;
        queue_set = qs;

        std::list<QueuedItem> *old = queue;
        queue = q;

        while (!old->empty()) {
            addEvent(old->front());
            old->pop_front();
        }

        // replaceQueues is called when the backfill is complete.
        pendingBackfill = false;
    }

    bool complete(void) {
        return dumpQueue && empty() && !pendingBackfill;
    }

    TapConnection(const std::string &n, uint32_t f):
        client(n), queue(NULL), queue_set(NULL), flags(f),
        recordsFetched(0), pendingFlush(false), expiry_time((rel_time_t)-1),
        reconnects(0), connected(true), paused(false), backfillAge(0),
        doRunBackfill(false), pendingBackfill(true), vbucketFilter(),
        vBucketHighPriority(), vBucketLowPriority()
    {
        queue = new std::list<QueuedItem>;
        queue_set = new std::set<QueuedItem>;
    }

    ~TapConnection() {
        delete queue;
        delete queue_set;
    }

    /**
     * String used to identify the client.
     * @todo design the connect packet and fill inn som info here
     */
    std::string client;
    /**
     * The queue of keys that needs to be sent (this is the "live stream")
     */
    std::list<QueuedItem> *queue;
    /**
     * Set to prevent duplicate queue entries.
     *
     * Note that stl::set is O(log n) for ops we care about, so we'll
     * want to look out for this.
     */
    std::set<QueuedItem> *queue_set;
    /**
     * Flags passed by the client
     */
    uint32_t flags;
    /**
     * Counter of the number of records fetched from this stream since the
     * beginning
     */
    size_t recordsFetched;
    /**
     * Do we have a pending flush command?
     */
    bool pendingFlush;

    /**
     * when this tap conneciton expires.
     */
    rel_time_t expiry_time;

    /**
     * Number of times this client reconnected
     */
    uint32_t reconnects;

    /**
     * Is connected?
     */
    bool connected;

    /**
     * is his paused
     */
    bool paused;

    /**
     * Backfill age for the connection
     */
    uint64_t backfillAge;


    /**
     * Dump and disconnect?
     */
    bool dumpQueue;

    /**
     * We don't want to do the backfill in the thread used by the client,
     * because that would block all clients bound to the same thread.
     * Instead we run the backfill the first time we try to walk the
     * stream (that would be in the TAP thread). This would cause the other
     * tap streams to block, but allows all clients to use the cache.
     */
    bool doRunBackfill;

    // True until a backfill has dumped all the content.
    bool pendingBackfill;

    /**
     * Filter for the buckets we want (0 entries == all buckets)
     */
    std::vector<uint16_t> vbucketFilter;

    /**
     * VBucket status messages immediately (before userdata)
     */
    std::queue<TapVBucketEvent> vBucketHighPriority;
    /**
     * VBucket status messages sent when there is nothing else to send
     */
    std::queue<TapVBucketEvent> vBucketLowPriority;

    DISALLOW_COPY_AND_ASSIGN(TapConnection);
};

/**
 *
 */
class EventuallyPersistentEngine : public ENGINE_HANDLE_V1 {
friend class LookupCallback;
public:
    ENGINE_ERROR_CODE initialize(const char* config)
    {
        ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;
        char *master = NULL;

        if (config != NULL) {
            char *dbn = NULL, *initf = NULL;
            size_t htBuckets = 0;
            size_t htLocks = 0;

            const int max_items = 12;
            struct config_item items[max_items];
            int ii = 0;
            memset(items, 0, sizeof(items));

            items[ii].key = "dbname";
            items[ii].datatype = DT_STRING;
            items[ii].value.dt_string = &dbn;

            ++ii;
            items[ii].key = "initfile";
            items[ii].datatype = DT_STRING;
            items[ii].value.dt_string = &initf;

            ++ii;
            items[ii].key = "warmup";
            items[ii].datatype = DT_BOOL;
            items[ii].value.dt_bool = &warmup;

            ++ii;
            items[ii].key = "waitforwarmup";
            items[ii].datatype = DT_BOOL;
            items[ii].value.dt_bool = &wait_for_warmup;

            ++ii;
            items[ii].key = "vb0";
            items[ii].datatype = DT_BOOL;
            items[ii].value.dt_bool = &startVb0;

            ++ii;
            items[ii].key = "tap_keepalive";
            items[ii].datatype = DT_SIZE;
            items[ii].value.dt_size = &tapKeepAlive;

            ++ii;
            items[ii].key = "ht_size";
            items[ii].datatype = DT_SIZE;
            items[ii].value.dt_size = &htBuckets;

            ++ii;
            items[ii].key = "ht_locks";
            items[ii].datatype = DT_SIZE;
            items[ii].value.dt_size = &htLocks;

            ++ii;
            items[ii].key = "tap_peer";
            items[ii].datatype = DT_STRING;
            items[ii].value.dt_string = &master;

            ++ii;
            items[ii].key = "config_file";
            items[ii].datatype = DT_CONFIGFILE;

            ++ii;
            items[ii].key = "max_item_size";
            items[ii].datatype = DT_SIZE;
            items[ii].value.dt_size = &maxItemSize;

            ++ii;
            items[ii].key = NULL;

            if (serverApi->core->parse_config(config, items, stderr) != 0) {
                ret = ENGINE_FAILED;
            } else {
                if (dbn != NULL) {
                    dbname = dbn;
                }
                if (initf != NULL) {
                    initFile = initf;
                }
                HashTable::setDefaultNumBuckets(htBuckets);
                HashTable::setDefaultNumLocks(htLocks);
            }
        }

        if (ret == ENGINE_SUCCESS) {
            time_t start = time(NULL);
            try {
                MultiDBSqliteStrategy *strategy =
                    new MultiDBSqliteStrategy(dbname,
                                              initFile,
                                              NUMBER_OF_SHARDS);
                sqliteDb = new StrategicSqlite3(strategy);
            } catch (std::exception& e) {
                std::stringstream ss;
                ss << "Failed to create database: " << e.what() << std::endl;
                if (!dbAccess()) {
                    ss << "No access to \"" << dbname << "\"."
                              << std::endl;
                }

                getLogger()->log(EXTENSION_LOG_WARNING, NULL, "%s",
                                 ss.str().c_str());
                return ENGINE_FAILED;
            }

            databaseInitTime = time(NULL) - start;
            epstore = new EventuallyPersistentStore(sqliteDb, startVb0);

            if (epstore == NULL) {
                ret = ENGINE_ENOMEM;
            } else {
                if (!warmup) {
                    epstore->reset();
                }

                SERVER_CALLBACK_API *sapi;
                sapi = getServerApi()->callback;
                sapi->register_callback(ON_DISCONNECT, EvpHandleDisconnect, this);
            }
            startEngineThreads();

            // If requested, don't complete the initialization until the
            // flusher transitions out of the initializing state (i.e
            // warmup is finished).
            const Flusher *flusher = epstore->getFlusher();
            if (wait_for_warmup && flusher) {
                while (flusher->state() == initializing) {
                    sleep(1);
                }
            }

            if (master != NULL) {
                setTapPeer(master);
                free(master);
            }
        }

        if (ret == ENGINE_SUCCESS) {
            getlExtension = new GetlExtension(epstore, getServerApi);
            getlExtension->initialize();
        }

        getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Engine init complete.\n");

        return ret;
    }

    void destroy()
    {
        stopReplication();
        stopEngineThreads();
    }

    ENGINE_ERROR_CODE itemAllocate(const void* cookie,
                                   item** item,
                                   const void* key,
                                   const size_t nkey,
                                   const size_t nbytes,
                                   const int flags,
                                   const rel_time_t exptime)
    {
        (void)cookie;
        if (nbytes > maxItemSize) {
            return ENGINE_E2BIG;
        }

        *item = new Item(key, nkey, nbytes, flags, exptime);
        if (*item == NULL) {
            return ENGINE_ENOMEM;
        } else {
            return ENGINE_SUCCESS;
        }
    }

    ENGINE_ERROR_CODE itemDelete(const void* cookie,
                                 const void* key,
                                 const size_t nkey,
                                 uint64_t cas,
                                 uint16_t vbucket)
    {
        (void)cas;
        (void)vbucket;
        std::string k(static_cast<const char*>(key), nkey);
        return itemDelete(cookie, k, vbucket);
    }

    ENGINE_ERROR_CODE itemDelete(const void* cookie,
                                 const std::string &key,
                                 uint16_t vbucket)
    {
        (void)cookie;
        RememberingCallback<bool> delCb;

        epstore->del(key, vbucket, delCb);
        delCb.waitForValue();
        if (delCb.val) {
            addDeleteEvent(key, vbucket);
        }
        return static_cast<ENGINE_ERROR_CODE>(delCb.getStatus());
    }


    void itemRelease(const void* cookie, item *item)
    {
        (void)cookie;
        delete (Item*)item;
    }

    ENGINE_ERROR_CODE get(const void* cookie,
                          item** item,
                          const void* key,
                          const int nkey,
                          uint16_t vbucket)
    {
        (void)cookie;
        (void)vbucket;
        std::string k(static_cast<const char*>(key), nkey);
        RememberingCallback<GetValue> getCb;
        epstore->get(k, vbucket, getCb);
        getCb.waitForValue();
        if (getCb.val.getStatus() == ENGINE_SUCCESS) {
            *item = getCb.val.getValue();
            return getCb.val.getStatus();
        } else {
            return getCb.val.getStatus();
        }

    }

    ENGINE_ERROR_CODE getStats(const void* cookie,
                               const char* stat_key,
                               int nkey,
                               ADD_STAT add_stat)
    {
        ENGINE_ERROR_CODE rv = ENGINE_KEY_ENOENT;
        if (stat_key == NULL) {
            rv = doEngineStats(cookie, add_stat);
        } else if (nkey == 3 && strncmp(stat_key, "tap", 3) == 0) {
            rv = doTapStats(cookie, add_stat);
        } else if (nkey == 4 && strncmp(stat_key, "hash", 3) == 0) {
            rv = doHashStats(cookie, add_stat);
        } else if (nkey == 7 && strncmp(stat_key, "vbucket", 7) == 0) {
            rv = doVBucketStats(cookie, add_stat);
        } else if (nkey > 4 && strncmp(stat_key, "key ", 4) == 0) {
            // Non-validating, non-blocking version
            rv = doKeyStats(cookie, add_stat, &stat_key[4], nkey-4, false);
        } else if (nkey > 5 && strncmp(stat_key, "vkey ", 5) == 0) {
            // Validating version; blocks
            rv = doKeyStats(cookie, add_stat, &stat_key[5], nkey-5, true);
        }

        // The server doesn't terminate stats other than null
        if (stat_key != NULL) {
            add_stat(NULL, 0, NULL, 0, cookie);
        }
        return rv;
    }

    ENGINE_ERROR_CODE store(const void *cookie,
                            item* itm,
                            uint64_t *cas,
                            ENGINE_STORE_OPERATION operation,
                            uint16_t vbucket)
    {
        ENGINE_ERROR_CODE ret;
        BoolPairCallback callback;
        Item *it = static_cast<Item*>(itm);
        item *i;

        it->setVBucketId(vbucket);

        switch (operation) {
            case OPERATION_CAS:
                if (it->getCas() == 0) {
                    // Using a cas command with a cas wildcard doesn't make sense
                    ret = ENGINE_NOT_STORED;
                    break;
                }
                // FALLTHROUGH
            case OPERATION_SET:
                epstore->set(*it, callback);
                if (callback.getValue() &&
                    ((mutation_type_t)callback.getStatus() != IS_LOCKED)) {
                    *cas = it->getCas();
                    addMutationEvent(it, vbucket);
                    ret = ENGINE_SUCCESS;
                } else if (callback.getStatus() == INVALID_VBUCKET) {
                    ret = ENGINE_NOT_MY_VBUCKET;
                } else {
                    if (operation == OPERATION_CAS &&
                        ((mutation_type_t)callback.getStatus() == NOT_FOUND)) {
                        ret = ENGINE_KEY_ENOENT;
                    } else {
                        ret = ENGINE_KEY_EEXISTS;;
                    }
                }
                break;

            case OPERATION_ADD:
                // @todo this isn't atomic!
                if (get(cookie, &i, it->getKey().c_str(), it->getNKey(),
                        vbucket) == ENGINE_SUCCESS) {
                    itemRelease(cookie, i);
                    ret = ENGINE_NOT_STORED;
                } else {
                    epstore->set(*it, callback);
                    // unable to set if the key is locked
                    if ((mutation_type_t)callback.getStatus() == IS_LOCKED) {
                        return ENGINE_NOT_STORED;
                    } else if (callback.getStatus() == INVALID_VBUCKET) {
                        return ENGINE_NOT_MY_VBUCKET;
                    }
                    *cas = it->getCas();
                    addMutationEvent(it, vbucket);
                    ret = ENGINE_SUCCESS;
                }
                break;

            case OPERATION_REPLACE:
                // @todo this isn't atomic!
                if (get(cookie, &i, it->getKey().c_str(), it->getNKey(),
                        vbucket) == ENGINE_SUCCESS) {
                    itemRelease(cookie, i);
                    epstore->set(*it, callback);
                    // unable to set if the key is locked
                    if ((mutation_type_t)callback.getStatus() == IS_LOCKED) {
                        return ENGINE_KEY_EEXISTS;
                    }
                    *cas = it->getCas();
                    addMutationEvent(it, vbucket);
                    ret = ENGINE_SUCCESS;
                } else {
                    ret = ENGINE_NOT_STORED;
                }
                break;

            case OPERATION_APPEND:
            case OPERATION_PREPEND:
                do {
                    if ((ret = get(cookie, &i, it->getKey().c_str(),
                                   it->getNKey(), vbucket)) == ENGINE_SUCCESS) {
                        Item *old = reinterpret_cast<Item*>(i);

                        if (operation == OPERATION_APPEND) {
                            if (!old->append(*it)) {
                                itemRelease(cookie, i);
                                return ENGINE_ENOMEM;
                            }
                        } else {
                            if (!old->prepend(*it)) {
                                itemRelease(cookie, i);
                                return ENGINE_ENOMEM;
                            }
                        }

                        ret = store(cookie, old, cas, OPERATION_CAS, vbucket);
                        if (ret == ENGINE_SUCCESS) {
                            addMutationEvent(static_cast<Item*>(i), vbucket);
                        }
                        itemRelease(cookie, i);
                    }
                } while (ret == ENGINE_KEY_EEXISTS);

                // Map the error code back to what memcacpable expects
                if (ret == ENGINE_KEY_ENOENT) {
                    ret = ENGINE_NOT_STORED;
                }
                break;

            default:
                ret = ENGINE_ENOTSUP;
        }

        return ret;
    }

    ENGINE_ERROR_CODE arithmetic(const void* cookie,
                                 const void* key,
                                 const int nkey,
                                 const bool increment,
                                 const bool create,
                                 const uint64_t delta,
                                 const uint64_t initial,
                                 const rel_time_t exptime,
                                 uint64_t *cas,
                                 uint64_t *result,
                                 uint16_t vbucket)
    {
        item *it = NULL;

        ENGINE_ERROR_CODE ret;
        ret = get(cookie, &it, key, nkey, vbucket);
        if (ret == ENGINE_SUCCESS) {
            Item *item = static_cast<Item*>(it);
            char *endptr;
            uint64_t val = strtoull(item->getData(), &endptr, 10);
            if ((errno != ERANGE) && (isspace(*endptr) || (*endptr == '\0' && endptr != item->getData()))) {
                if (increment) {
                    val += delta;
                } else {
                    if (delta > val) {
                        val = 0;
                    } else {
                        val -= delta;
                    }
                }

                std::stringstream vals;
                vals << val << "\r\n";
                size_t nb = vals.str().length();
                *result = val;
                Item *nit = new Item(key, (uint16_t)nkey, item->getFlags(),
                                     exptime, vals.str().c_str(), nb);
                nit->setCas(item->getCas());
                ret = store(cookie, nit, cas, OPERATION_CAS, vbucket);
                delete nit;
            } else {
                ret = ENGINE_EINVAL;
            }

            delete item;
        } else if (ret == ENGINE_NOT_MY_VBUCKET) {
            return ret;
        } else if (ret == ENGINE_KEY_ENOENT && create) {
            std::stringstream vals;

            vals << initial << "\r\n";
            size_t nb = vals.str().length();

            *result = initial;
            Item *item = new Item(key, (uint16_t)nkey, 0, exptime,
                                  vals.str().c_str(), nb);
            ret = store(cookie, item, cas, OPERATION_ADD, vbucket);
            delete item;
        }

        /* We had a race condition.. just call ourself recursively to retry */
        if (ret == ENGINE_KEY_EEXISTS) {
            return arithmetic(cookie, key, nkey, increment, create, delta,
                              initial, exptime, cas, result, vbucket);
        }

        return ret;
    }



    ENGINE_ERROR_CODE flush(const void *cookie, time_t when)
    {
        (void)cookie;
        ENGINE_ERROR_CODE ret= ENGINE_ENOTSUP;

        if (when == 0) {
            epstore->reset();
            addFlushEvent();
            ret = ENGINE_SUCCESS;
        }

        return ret;
    }

    tap_event_t walkTapQueue(const void *cookie, item **itm, void **es,
                             uint16_t *nes, uint8_t *ttl, uint16_t *flags,
                             uint32_t *seqno, uint16_t *vbucket) {
        LockHolder lh(tapNotifySync);
        TapConnection *connection = tapConnectionMap[cookie];
        assert(connection);

        if (connection->doRunBackfill) {
            queueBackfill(connection);
        }

        tap_event_t ret = TAP_PAUSE;
        connection->paused = false;

        *es = NULL;
        *nes = 0;
        *ttl = (uint8_t)-1;
        *seqno = 0;
        *flags = 0;

        TapVBucketEvent ev = connection->nextVBucketHighPriority();
        if (ev.event != TAP_PAUSE) {
            *vbucket = ev.vbucket;
            *flags = static_cast<uint16_t>(ev.state);
            return ev.event;
        }

        if (!connection->empty()) {
            QueuedItem qi = connection->next();
            lh.unlock();

            ENGINE_ERROR_CODE r;
            *vbucket = qi.getVBucketId();
            std::string key = qi.getKey();
            r = get(cookie, itm, key.c_str(), (int)key.length(), qi.getVBucketId());
            if (r == ENGINE_SUCCESS) {
                ret = TAP_MUTATION;
            } else if (r == ENGINE_KEY_ENOENT) {
                ret = TAP_DELETION;
                r = itemAllocate(cookie, itm,
                                 key.c_str(), key.length(), 0, 0, 0);
                if (r != ENGINE_SUCCESS) {
                    EXTENSION_LOGGER_DESCRIPTOR *logger;
                    logger = (EXTENSION_LOGGER_DESCRIPTOR*)serverApi->extension->get_extension(EXTENSION_LOGGER);

                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                     "Failed to allocate memory for deletion of: %s\n", key.c_str());
                    ret = TAP_PAUSE;
                }
            }
        } else if (connection->shouldFlush()) {
            ret = TAP_FLUSH;
        } else {
            connection->paused = true;
        }

        if (ret == TAP_PAUSE && connection->complete()) {
            ev = connection->nextVBucketLowPriority();
            if (ev.event != TAP_PAUSE) {
                *vbucket = ev.vbucket;
                *flags = static_cast<uint16_t>(ev.state);
                if (ev.state == active) {
                    epstore->setVBucketState(ev.vbucket, dead);
                }
                return ev.event;
            }

            ret = TAP_DISCONNECT;
        }

        return ret;
    }

    void createTapQueue(const void *cookie, std::string &client, uint32_t flags,
                        const void *userdata,
                        size_t nuserdata) {
        // map is set-assocative, so this will create an instance here..
        LockHolder lh(tapNotifySync);
        purgeExpiredTapConnections_UNLOCKED();

        std::string name = "eq_tapq:";
        if (client.length() == 0) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%p", cookie);
            name.append(buffer);
        } else {
            name.append(client);
        }

        TapConnection *tap = NULL;

        std::list<TapConnection*>::iterator iter;
        for (iter = allTaps.begin(); iter != allTaps.end(); ++iter) {
            tap = *iter;
            if (tap->client == name) {
                tap->expiry_time = (rel_time_t)-1;
                ++tap->reconnects;
                break;
            } else {
                tap = NULL;
            }
        }

        // Disconnects aren't quite immediate yet, so if we see a
        // connection request for a client *and* expiry_time is 0, we
        // should kill this guy off.
        if (tap != NULL && tapKeepAlive == 0) {
            getLogger()->log(EXTENSION_LOG_INFO, NULL,
                             "Forcing close of tap client [%s]\n",
                             name.c_str());
            std::map<const void*, TapConnection*>::iterator miter;
            for (miter = tapConnectionMap.begin();
                 miter != tapConnectionMap.end();
                 ++miter) {
                if (miter->second == tap) {
                    break;
                }
            }
            assert(miter != tapConnectionMap.end());
            tapConnectionMap.erase(miter);
            purgeSingleExpiredTapConnection(tap);
            tap = NULL;
        }

        // Start decoding the userdata section of the packet...
        const char *ptr = static_cast<const char*>(userdata);
        uint64_t backfillAge = 0;
        std::vector<uint16_t> vbuckets;

        if (flags & TAP_CONNECT_FLAG_BACKFILL) { /* */
            assert(nuserdata >= sizeof(backfillAge));
            // use memcpy to avoid alignemt issues
            memcpy(&backfillAge, ptr, sizeof(backfillAge));
            backfillAge = ntohll(backfillAge);
            nuserdata -= sizeof(backfillAge);
            ptr += sizeof(backfillAge);
        }

        if (flags & TAP_CONNECT_FLAG_LIST_VBUCKETS) {
            uint16_t nvbuckets;
            assert(nuserdata >= sizeof(nvbuckets));
            memcpy(&nvbuckets, ptr, sizeof(nvbuckets));
            nuserdata -= sizeof(nvbuckets);
            ptr += sizeof(nvbuckets);
            nvbuckets = ntohs(nvbuckets);
            if (nvbuckets > 0) {
                assert(nuserdata >= (sizeof(uint16_t) * nvbuckets));
                for (uint16_t ii = 0; ii < nvbuckets; ++ii) {
                    uint16_t val;
                    memcpy(&val, ptr, sizeof(nvbuckets));
                    ptr += sizeof(uint16_t);
                    vbuckets.push_back(ntohs(val));
                }
            }
        }

        // @todo ensure that we don't have this client alredy
        // if so this should be a reconnect...
        if (tap == NULL) {
            TapConnection *tc = new TapConnection(name, flags);
            allTaps.push_back(tc);
            tapConnectionMap[cookie] = tc;

            if (flags & TAP_CONNECT_FLAG_BACKFILL) {
                tc->backfillAge = backfillAge;
            }

            if (tc->backfillAge < (uint64_t)time(NULL)) {
                tc->doRunBackfill = true;
                tc->pendingBackfill = true;
            }

            if (flags & TAP_CONNECT_FLAG_LIST_VBUCKETS) {
                tc->vbucketFilter = vbuckets;
            }

            tc->dumpQueue = flags & TAP_CONNECT_FLAG_DUMP;
           if (flags & TAP_CONNECT_FLAG_TAKEOVER_VBUCKETS) {
               for (std::vector<uint16_t>::iterator it = vbuckets.begin();
                    it != vbuckets.end(); ++it) {
                   TapVBucketEvent hi(TAP_VBUCKET_SET, *it, pending);
                   TapVBucketEvent lo(TAP_VBUCKET_SET, *it, active);
                   tc->addVBucketHighPriority(hi);
                   tc->addVBucketLowPriority(lo);
               }
               tc->dumpQueue = true;
           }
        } else {
            tapConnectionMap[cookie] = tap;
            tap->connected = true;
        }
    }

    ENGINE_ERROR_CODE tapNotify(const void *cookie,
                                void *engine_specific,
                                uint16_t nengine,
                                uint8_t ttl,
                                uint16_t tap_flags,
                                tap_event_t tap_event,
                                uint32_t tap_seqno,
                                const void *key,
                                size_t nkey,
                                uint32_t flags,
                                uint32_t exptime,
                                uint64_t cas,
                                const void *data,
                                size_t ndata,
                                uint16_t vbucket)
    {
        (void)engine_specific;
        (void)nengine;
        (void)ttl;
        (void)tap_flags;
        (void)tap_seqno;

        // If cookie is null, this is the internal tap client, so we
        // should disconnect it if tap isn't enabled.
        if (!cookie && !tapEnabled) {
            return ENGINE_DISCONNECT;
        }

        switch (tap_event) {
        case TAP_FLUSH:
            return flush(cookie, 0);
        case TAP_DELETION:
        {
            std::string k(static_cast<const char*>(key), nkey);
            return itemDelete(cookie, k, vbucket);
        }

        case TAP_MUTATION:
        {
            // We don't get the trailing CRLF in tap mutation but should store it
            // to satisfy memcached expectations.
            //
            // We do this by manually constructing the item using its
            // value_t constructor to reduce memory copies as much as
            // possible.

            std::string k(static_cast<const char *>(key), nkey);
            shared_ptr<std::string> s(new std::string);
            s->reserve(ndata+2);
            s->append(static_cast<const char*>(data), ndata);
            s->append("\r\n");

            Item *item = new Item(k, flags, exptime, s);

            /* @TODO we don't have CAS now.. we might in the future.. */
            (void)cas;
            uint64_t ncas;
            ENGINE_ERROR_CODE ret = store(cookie, item, &ncas, OPERATION_SET,
                                          vbucket);
            delete item;
            return ret;
        }

        case TAP_OPAQUE:
            break;

        case TAP_VBUCKET_SET:
            {
                vbucket_state_t state = static_cast<vbucket_state_t>(tap_flags);
                epstore->setVBucketState(vbucket, state);
                if (state == active) {
                    return ENGINE_DISCONNECT;
                }
            }
            break;

        default:
            abort();
        }

        return ENGINE_SUCCESS;
    }


    /**
     * Visit the objects and add them to the tap connecitons queue.
     * @todo this code should honor the backfill time!
     */
    void queueBackfill(TapConnection *tc);

    void handleDisconnect(const void *cookie) {
        LockHolder lh(tapNotifySync);
        std::map<const void*, TapConnection*>::iterator iter;
        iter = tapConnectionMap.find(cookie);
        if (iter != tapConnectionMap.end()) {
            iter->second->expiry_time = serverApi->core->get_current_time()
                + (int)tapKeepAlive;
            iter->second->connected = false;
            tapConnectionMap.erase(iter);
        }
        purgeExpiredTapConnections_UNLOCKED();
    }

    protocol_binary_response_status stopFlusher(const char **msg) {
        protocol_binary_response_status rv = PROTOCOL_BINARY_RESPONSE_SUCCESS;
        *msg = NULL;
        if (!epstore->pauseFlusher()) {
            getLogger()->log(EXTENSION_LOG_INFO, NULL,
                             "Attempted to stop flusher in state [%s]\n",
                             epstore->getFlusher()->stateName());
            *msg = "Flusher not running.";
            rv = PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
        return rv;
    }

    protocol_binary_response_status startFlusher(const char **msg) {
        protocol_binary_response_status rv = PROTOCOL_BINARY_RESPONSE_SUCCESS;
        *msg = NULL;
        if (!epstore->resumeFlusher()) {
            getLogger()->log(EXTENSION_LOG_INFO, NULL,
                             "Attempted to start flusher in state [%s]\n",
                             epstore->getFlusher()->stateName());
            *msg = "Flusher not shut down.";
            rv = PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
        return rv;
    }

    void resetStats()
    {
        if (epstore) {
            epstore->resetStats();
        }

        // @todo reset statistics
    }

    void setMinDataAge(int to) {
        epstore->setMinDataAge(to);
    }

    void setQueueAgeCap(int to) {
        epstore->setQueueAgeCap(to);
    }

    void setTxnSize(int to) {
        epstore->setTxnSize(to);
    }

    bool startReplication() {
        LockHolder lh(tapMutex);
        tapEnabled = true;
        if (clientTap != NULL) {
            try {
                clientTap->start();
                return true;
            } catch (std::runtime_error &e) {
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "Failed to start replication: %s\n",
                                 e.what());
            }
        }
        return false;
    }

    bool stopReplication() {
        LockHolder ltm(tapMutex);
        tapEnabled = false;

        if (clientTap != NULL) {
            try {
                clientTap->stop();
                return true;
            } catch (std::runtime_error &e) {
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "Failed to start replication: %s\n",
                                 e.what());
            }
        }
        return false;
    }

    void setTapPeer(std::string peer) {
        tapConnect(peer);
    }

    RCPtr<VBucket> getVBucket(uint16_t vbucket) {
        return epstore->getVBucket(vbucket);
    }

    void setVBucketState(uint16_t vbid, vbucket_state_t to) {
        epstore->setVBucketState(vbid, to);
    }

    ~EventuallyPersistentEngine() {
        delete epstore;
        delete sqliteDb;
        delete getlExtension;
    }

    engine_info *getInfo() {
        return &info.info;
    }

private:
    EventuallyPersistentEngine(GET_SERVER_API get_server_api);
    friend ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                             GET_SERVER_API get_server_api,
                                             ENGINE_HANDLE **handle);

    void updateTapStats(void) {
        LockHolder lh(tapNotifySync);
        size_t depth = 0, totalSent = 0;
        std::map<const void*, TapConnection*>::iterator iter;
        for (iter = tapConnectionMap.begin(); iter != tapConnectionMap.end(); iter++) {
            depth += iter->second->queue->size();
            totalSent += iter->second->recordsFetched;
        }
        lh.unlock();

        // Use the depth and number of records fetched we calculated above.
        epstore->setTapStats(depth, totalSent);
    }

    void notifyTapIoThreadMain(void) {
        LockHolder lh(tapNotifySync);
        // We should pause unless we purged some connections or
        // all queues have items.
        bool shouldPause = purgeExpiredTapConnections_UNLOCKED() == 0;
        // see if I have some channels that I have to signal..
        std::map<const void*, TapConnection*>::iterator iter;
        for (iter = tapConnectionMap.begin(); iter != tapConnectionMap.end(); iter++) {
            if (!iter->second->queue->empty()) {
                shouldPause = false;
            }
        }

        if (shouldPause) {
            tapNotifySync.wait();
            purgeExpiredTapConnections_UNLOCKED();
        }

        // Create a copy of the list so that we can release the lock
        std::map<const void*, TapConnection*> tcm = tapConnectionMap;

        lh.unlock();

        for (iter = tcm.begin(); iter != tcm.end(); iter++) {
            if (iter->second->paused) {
                serverApi->core->notify_io_complete(iter->first, ENGINE_SUCCESS);
            }
        }
    }

    friend void *EvpNotifyTapIo(void*arg);
    void notifyTapIoThread(void) {
        // Fix clean shutdown!!!
        while (!shutdown) {

            updateTapStats();
            notifyTapIoThreadMain();

            // Prevent the notify thread from busy-looping while
            // holding locks when there's work to do.
            sleep(1);
        }
    }

    void purgeSingleExpiredTapConnection(TapConnection *tc) {
        allTaps.remove(tc);
        /* Assert that the connection doesn't live in the map.. */
        /* TROND: Remove this when we're sure we don't have a bug here */
        assert(!mapped(tc));
        delete tc;
    }

    bool mapped(TapConnection *tc) {
        bool rv = false;
        std::map<const void*, TapConnection*>::iterator it;
        for (it = tapConnectionMap.begin();
             it != tapConnectionMap.end();
             ++it) {
            if (it->second == tc) {
                rv = true;
            }
        }
        return rv;
    }

    int purgeExpiredTapConnections_UNLOCKED() {
        rel_time_t now = serverApi->core->get_current_time();
        std::list<TapConnection*> deadClients;

        std::list<TapConnection*>::iterator iter;
        for (iter = allTaps.begin(); iter != allTaps.end(); iter++) {
            TapConnection *tc = *iter;
            if (tc->expiry_time <= now && !mapped(tc) && !tc->connected) {
                deadClients.push_back(tc);
            }
        }

        for (iter = deadClients.begin(); iter != deadClients.end(); iter++) {
            TapConnection *tc = *iter;
            purgeSingleExpiredTapConnection(tc);
        }

        return static_cast<int>(deadClients.size());
    }

    friend class BackFillVisitor;
    bool setEvents(const std::string &name,
                   std::list<QueuedItem> *q,
                   std::set<QueuedItem> *qs)
    {
        bool notify = true;
        bool found = false;
        LockHolder lh(tapNotifySync);

        std::list<TapConnection*>::iterator iter;
        for (iter = allTaps.begin(); iter != allTaps.end(); iter++) {
            TapConnection *tc = *iter;
            if (tc->client == name) {
                found = true;
                tc->replaceQueues(q, qs);
                notify = tc->paused; // notify if paused
            }
        }

        if (notify) {
            tapNotifySync.notify();
        }

        return found;
    }

    void addEvent(const std::string &str, uint16_t vbid)
    {
        bool notify = false;
        LockHolder lh(tapNotifySync);

        std::list<TapConnection*>::iterator iter;
        for (iter = allTaps.begin(); iter != allTaps.end(); iter++) {
            TapConnection *tc = *iter;
            if (!tc->dumpQueue && tc->addEvent(str, vbid) && tc->paused) {
                notify = true;
            }
        }

        if (notify) {
            tapNotifySync.notify();
        }
    }

    void addMutationEvent(Item *it, uint16_t vbid) {
        // Currently we use the same queue for all kinds of events..
        addEvent(it->getKey(), vbid);
    }

    void addDeleteEvent(const std::string &key, uint16_t vbid) {
        // Currently we use the same queue for all kinds of events..
        addEvent(key, vbid);
    }

    void addFlushEvent() {
        LockHolder lh(tapNotifySync);
        bool notify = false;
        std::list<TapConnection*>::iterator iter;
        for (iter = allTaps.begin(); iter != allTaps.end(); iter++) {
            TapConnection *tc = *iter;
            if (!tc->dumpQueue) {
                tc->flush();
                notify = true;
            }
        }
        if (notify) {
            tapNotifySync.notify();
        }
    }

    void startEngineThreads(void);
    void stopEngineThreads(void) {
        LockHolder lh(tapNotifySync);
        shutdown = true;
        tapNotifySync.notify();
        lh.unlock();
        pthread_join(notifyThreadId, NULL);
    }


    bool dbAccess(void) {
        bool ret = true;
        if (access(dbname, F_OK) == -1) {
            // file does not exist.. let's try to create it..
            FILE *fp = fopen(dbname, "w");
            if (fp == NULL) {
                ret= false;
            } else {
                fclose(fp);
                std::remove(dbname);
            }
        } else if (access(dbname, R_OK) == -1 || access(dbname, W_OK) == -1) {
            ret = false;
        }

        return ret;
    }

    ENGINE_ERROR_CODE doEngineStats(const void *cookie, ADD_STAT add_stat)
    {
        if (epstore) {
            EPStats &epstats = epstore->getStats();

            add_casted_stat("ep_version", VERSION, add_stat, cookie);
            add_casted_stat("ep_storage_age",
                            epstats.dirtyAge, add_stat, cookie);
            add_casted_stat("ep_storage_age_highwat",
                            epstats.dirtyAgeHighWat, add_stat, cookie);
            add_casted_stat("ep_min_data_age",
                            epstats.min_data_age, add_stat, cookie);
            add_casted_stat("ep_queue_age_cap",
                            epstats.queue_age_cap, add_stat, cookie);
            add_casted_stat("ep_max_txn_size",
                            epstore->getTxnSize(), add_stat, cookie);
            add_casted_stat("ep_data_age",
                            epstats.dataAge, add_stat, cookie);
            add_casted_stat("ep_data_age_highwat",
                            epstats.dataAgeHighWat, add_stat, cookie);
            add_casted_stat("ep_too_young",
                            epstats.tooYoung, add_stat, cookie);
            add_casted_stat("ep_too_old",
                            epstats.tooOld, add_stat, cookie);
            add_casted_stat("ep_total_enqueued",
                            epstats.totalEnqueued, add_stat, cookie);
            add_casted_stat("ep_total_persisted",
                            epstats.totalPersisted, add_stat, cookie);
            add_casted_stat("ep_item_flush_failed",
                            epstats.flushFailed, add_stat, cookie);
            add_casted_stat("ep_item_commit_failed",
                            epstats.commitFailed, add_stat, cookie);
            add_casted_stat("ep_queue_size",
                            epstats.queue_size, add_stat, cookie);
            add_casted_stat("ep_flusher_todo",
                            epstats.flusher_todo, add_stat, cookie);
            add_casted_stat("ep_flusher_state",
                            epstore->getFlusher()->stateName(),
                            add_stat, cookie);
            add_casted_stat("ep_commit_time",
                            epstats.commit_time, add_stat, cookie);
            add_casted_stat("ep_flush_duration",
                            epstats.flushDuration, add_stat, cookie);
            add_casted_stat("ep_flush_duration_highwat",
                            epstats.flushDurationHighWat, add_stat, cookie);
            add_casted_stat("curr_items", epstats.curr_items, add_stat,
                            cookie);

            if (warmup) {
                add_casted_stat("ep_warmup_thread",
                                epstats.warmupComplete.get() ? "complete" : "running",
                                add_stat, cookie);
                add_casted_stat("ep_warmed_up", epstats.warmedUp, add_stat, cookie);
                if (epstats.warmupComplete.get()) {
                    add_casted_stat("ep_warmup_time", epstats.warmupTime,
                                    add_stat, cookie);
                }
            }

            add_casted_stat("ep_tap_total_queue", epstats.tap_queue,
                            add_stat, cookie);
            add_casted_stat("ep_tap_total_fetched", epstats.tap_fetched,
                            add_stat, cookie);
            add_casted_stat("ep_tap_keepalive", tapKeepAlive,
                            add_stat, cookie);
        }

        add_casted_stat("ep_dbname", dbname, add_stat, cookie);
        add_casted_stat("ep_dbinit", databaseInitTime, add_stat, cookie);
        add_casted_stat("ep_warmup", warmup ? "true" : "false",
                        add_stat, cookie);
        return ENGINE_SUCCESS;
    }

    ENGINE_ERROR_CODE doVBucketStats(const void *cookie, ADD_STAT add_stat) {
        if (epstore) {
            StatVBucketVisitor svbv(cookie, add_stat);
            epstore->visit(svbv);
        }
        return ENGINE_SUCCESS;
    }

    ENGINE_ERROR_CODE doHashStats(const void *cookie, ADD_STAT add_stat) {
        HashTableDepthStatVisitor depthVisitor;
        if (epstore) {
            epstore->visitDepth(depthVisitor);
        }
        add_casted_stat("ep_hash_bucket_size", epstore->getHashSize(), add_stat, cookie);
        add_casted_stat("ep_hash_num_locks", epstore->getHashLocks(), add_stat, cookie);
        add_casted_stat("ep_hash_min_depth", depthVisitor.min, add_stat, cookie);
        add_casted_stat("ep_hash_max_depth", depthVisitor.max, add_stat, cookie);
        return ENGINE_SUCCESS;
    }

    ENGINE_ERROR_CODE doTapStats(const void *cookie, ADD_STAT add_stat) {
        std::list<TapConnection*>::iterator iter;
        if (epstore) {
            EPStats &epstats = epstore->getStats();
            add_casted_stat("ep_tap_total_queue", epstats.tap_queue, add_stat, cookie);
            add_casted_stat("ep_tap_total_fetched", epstats.tap_fetched, add_stat, cookie);
            add_casted_stat("ep_tap_keepalive", tapKeepAlive, add_stat, cookie);
        }
        int totalTaps = 0;
        LockHolder lh(tapNotifySync);
        for (iter = allTaps.begin(); iter != allTaps.end(); iter++) {
            char tap[80];
            totalTaps++;
            TapConnection *tc = *iter;
            sprintf(tap, "%s:qlen", tc->client.c_str());
            add_casted_stat(tap, tc->queue->size(), add_stat, cookie);
            sprintf(tap, "%s:rec_fetched", tc->client.c_str());
            add_casted_stat(tap, tc->recordsFetched, add_stat, cookie);
            if (tc->reconnects > 0) {
                sprintf(tap, "%s:reconnects", tc->client.c_str());
                add_casted_stat(tap, tc->reconnects, add_stat, cookie);
            }
            if (tc->backfillAge != 0) {
                sprintf(tap, "%s:backfill_age", tc->client.c_str());
                add_casted_stat(tap, (size_t)tc->backfillAge, add_stat, cookie);
            }
        }
        add_casted_stat("ep_tap_count", totalTaps, add_stat, cookie);

        add_casted_stat("ep_replication_state",
                        tapEnabled? "enabled": "disabled", add_stat, cookie);

        const char *repStatus = "stopped";
        std::string tapPeer;

        {
            LockHolder ltm(tapMutex);
            if (clientTap != NULL) {
                tapPeer.assign(clientTap->peer);
                if (clientTap->connected) {
                    if (!tapEnabled) {
                        repStatus = "stopping";
                    } else {
                        repStatus = "running";
                    }
                } else if (clientTap->running && tapEnabled) {
                    repStatus = "connecting";
                }
            }

        }
        add_casted_stat("ep_replication_peer",
                        tapPeer.empty()? "none":
                        tapPeer.c_str(), add_stat, cookie);
        add_casted_stat("ep_replication_status", repStatus, add_stat, cookie);

        return ENGINE_SUCCESS;
    }

    void addLookupResult(const void *cookie, Item *result) {
        LockHolder lh(lookupMutex);
        std::map<const void*, Item*>::iterator it = lookups.find(cookie);
        if (it != lookups.end()) {
            if (it->second != NULL) {
                getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                    "Cleaning up old lookup result for '%s'\n",
                    it->second->getKey().c_str());
                delete it->second;
            } else {
                getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                    "Cleaning up old null lookup result\n");
            }
            lookups.erase(it);
        }
        lookups[cookie] = result;
    }

    bool fetchLookupResult(const void *cookie, Item **item) {
        // This will return *and erase* the lookup result for a connection.
        // You look it up, you own it.
        LockHolder lh(lookupMutex);
        std::map<const void*, Item*>::iterator it = lookups.find(cookie);
        if (it != lookups.end()) {
            *item = it->second;
            lookups.erase(it);
            return true;
        } else {
            return false;
        }
    }

    ENGINE_ERROR_CODE doKeyStats(const void *cookie, ADD_STAT add_stat,
                                 const char *k, int nkey, bool validate=false)
    {
        std::string key(k, nkey);
        ENGINE_ERROR_CODE rv = ENGINE_FAILED;

        if (epstore) {
            Item *it = NULL;
            shared_ptr<Item> diskItem;
            struct key_stats kstats;
            rel_time_t now = ep_current_time();
            if (fetchLookupResult(cookie, &it)) {
                diskItem.reset(it); // Will be null if the key was not found
                if (!validate) {
                    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                        "Found lookup results for non-validating key stat call. Would have leaked\n");
                    diskItem.reset();
                }
            } else if (validate) {
                shared_ptr<LookupCallback> cb(new LookupCallback(this, cookie));
                // TODO:  Need a proper vbucket ID here.
                epstore->getFromUnderlying(key, 0, cb);
                return ENGINE_EWOULDBLOCK;
            }

            // TODO:  Need a proper vbucket ID here.
            if (epstore->getKeyStats(k, 0, kstats)) {
                std::string valid("this_is_a_bug");
                if (validate) {
                    if (kstats.dirty) {
                        valid.assign("dirty");
                    } else {
                        RememberingCallback<GetValue> cb;
                        // TODO:  Need a proper vbucket ID here.
                        epstore->get(key, 0, cb);
                        cb.waitForValue();
                        if (cb.getStatus() == ENGINE_SUCCESS) {
                            shared_ptr<Item> item(cb.val.getValue());
                            if (diskItem.get()) {
                                // Both items exist
                                if (diskItem->getNBytes() != item->getNBytes()) {
                                    valid.assign("length_mismatch");
                                } else if (memcmp(diskItem->getData(), item->getData(),
                                           diskItem->getNBytes()) != 0) {
                                    valid.assign("data_mismatch");
                                } else if (diskItem->getFlags() != item->getFlags()) {
                                    valid.assign("flags_mismatch");
                                } else {
                                    valid.assign("valid");
                                }
                            } else {
                                // Since we do the disk lookup first, this could
                                // be transient
                                valid.assign("ram_but_not_disk");
                            }
                        } else {
                            valid.assign("item_deleted");
                        }
                    }
                    getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Key '%s' is %s\n",
                                     key.c_str(), valid.c_str());
                }
                add_casted_stat("key_is_dirty", kstats.dirty, add_stat, cookie);
                add_casted_stat("key_exptime", kstats.exptime, add_stat, cookie);
                add_casted_stat("key_flags", kstats.flags, add_stat, cookie);
                add_casted_stat("key_cas", kstats.cas, add_stat, cookie);
                add_casted_stat("key_dirtied", kstats.dirty ? now -
                                kstats.dirtied : 0, add_stat, cookie);
                add_casted_stat("key_data_age", kstats.dirty ? now -
                                kstats.data_age : 0, add_stat, cookie);
                if (validate) {
                    add_casted_stat("key_valid", valid.c_str(), add_stat, cookie);
                }
                rv = ENGINE_SUCCESS;
            } else {
                rv = ENGINE_KEY_ENOENT;
            }
        }
        return rv;
    }

    void tapConnect(std::string &peer) {
        uint32_t flags = 0;

        LockHolder lh(tapMutex);
        bool found = false;
        if (clientTap != NULL) {
            if (clientTap->peer != peer) {
                delete clientTap;
            } else {
                found = true;
            }
        }

        if (!found) {
            clientTap = new TapClientConnection(peer, flags, this);
            tapConnect();
        }
    }

    void tapConnect() {
        try {
            tapEnabled = true;
            clientTap->start();
        } catch (std::runtime_error &e) {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Failed to create TAP connection: %s\n",
                             e.what());
        }
    }

    const char *dbname;
    const char *initFile;
    bool warmup;
    bool wait_for_warmup;
    bool startVb0;
    SERVER_HANDLE_V1 *serverApi;
    StrategicSqlite3 *sqliteDb;
    EventuallyPersistentStore *epstore;
    std::map<const void*, TapConnection*> tapConnectionMap;
    std::list<TapConnection*> allTaps;
    std::map<const void*, Item*> lookups;
    Mutex lookupMutex;
    time_t databaseInitTime;
    size_t tapKeepAlive;
    pthread_t notifyThreadId;
    SyncObject tapNotifySync;
    volatile bool shutdown;
    GET_SERVER_API getServerApi;
    union {
        engine_info info;
        char buffer[sizeof(engine_info) + 10 * sizeof(feature_info) ];
    } info;
    GetlExtension *getlExtension;

    Mutex tapMutex;
    TapClientConnection* clientTap;
    bool tapEnabled;
    size_t maxItemSize;
};

class BackFillVisitor : public VBucketVisitor {
public:
    BackFillVisitor(EventuallyPersistentEngine *e, TapConnection *tc):
        VBucketVisitor(), engine(e), name(tc->client), queue(NULL), queue_set(NULL)
    {
        queue_set = new std::set<QueuedItem>;
    }

    ~BackFillVisitor() {
        delete queue;
        delete queue_set;
    }

    void visit(StoredValue *v) {
        std::string key(v->getKey());
        queue_set->insert(QueuedItem(key, currentBucket));
    }

    void apply(void) {
        queue = new std::list<QueuedItem>(queue_set->begin(),
                                          queue_set->end());
        if (engine->setEvents(name, queue, queue_set)) {
            queue = NULL;
            queue_set = NULL;
        }
    }

private:
    EventuallyPersistentEngine *engine;
    std::string name;
    std::list<QueuedItem> *queue;
    std::set<QueuedItem> *queue_set;
};

#endif
