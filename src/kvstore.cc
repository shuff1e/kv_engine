/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
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

#include <map>
#include <string>
#include <fcntl.h>

#include "common.h"
#include "couch-kvstore/couch-kvstore.h"
#include "forest-kvstore/forest-kvstore.h"
#include "kvstore.h"
#include "vbucket.h"
#include <platform/dirutils.h>
#include <sys/types.h>
#include <sys/stat.h>

using namespace CouchbaseDirectoryUtilities;

KVStoreConfig::KVStoreConfig(Configuration& config, uint16_t shardid)
    : maxVBuckets(config.getMaxVbuckets()), maxShards(config.getMaxNumShards()),
      dbname(config.getDbname()), backend(config.getBackend()), shardId(shardid) {

}
KVStoreConfig::KVStoreConfig(uint16_t _maxVBuckets, uint16_t _maxShards,
                             std::string& _dbname, std::string& _backend,
                             uint16_t _shardId)
    : maxVBuckets(_maxVBuckets), maxShards(_maxShards), dbname(_dbname),
      backend(_backend), shardId(_shardId) {

}

KVStore *KVStoreFactory::create(KVStoreConfig &config, bool read_only) {
    KVStore *ret = NULL;
    std::string backend = config.getBackend();
    if (backend.compare("couchdb") == 0) {
        ret = new CouchKVStore(config, read_only);
    } else if (backend.compare("forestdb") == 0) {
        ret = new ForestKVStore(config);
    } else {
        LOG(EXTENSION_LOG_WARNING, "Unknown backend: [%s]", backend.c_str());
    }

    return ret;
}

void KVStore::createDataDir(const std::string& dbname) {
    if (!mkdirp(dbname.c_str())) {
        if (errno != EEXIST) {
            std::stringstream ss;
            ss << "Failed to create data directory ["
               << dbname << "]: " << strerror(errno);
            throw std::runtime_error(ss.str());
        }
    }
}

std::string KVStore::updateCachedVBState(uint16_t vbid, uint64_t maxDeletedRevSeqno,
                                         uint64_t snapStartSeqno, uint64_t snapEndSeqno,
                                         uint64_t maxCas, uint64_t driftCounter) {

    std::string output;
    vbucket_state *vbState = cachedVBStates[vbid];

    if (vbState) {
        if (maxDeletedRevSeqno > 0 &&
                vbState->maxDeletedSeqno < maxDeletedRevSeqno) {
            vbState->maxDeletedSeqno = maxDeletedRevSeqno;
        }

        vbState->lastSnapStart = snapStartSeqno;
        vbState->lastSnapEnd = snapEndSeqno;

        if (maxCas > vbState->maxCas) {
            vbState->maxCas = maxCas;
        }

        vbState->driftCounter = driftCounter;

        std::stringstream jsonState;
        jsonState << "{\"state\": \"" << VBucket::toString(vbState->state) << "\""
                  << ",\"checkpoint_id\": \"" << vbState->checkpointId << "\""
                  << ",\"max_deleted_seqno\": \"" << vbState->maxDeletedSeqno << "\""
                  << ",\"failover_table\": " << vbState->failovers
                  << ",\"snap_start\": \"" << vbState->lastSnapStart << "\""
                  << ",\"snap_end\": \"" << vbState->lastSnapEnd << "\""
                  << ",\"max_cas\": \"" << vbState->maxCas << "\""
                  << ",\"drift_counter\": \"" << vbState->driftCounter << "\""
                  << "}";

        output = jsonState.str();
    }

    return output;
}

IORequest::IORequest(uint16_t vbId, MutationRequestCallback &cb , bool del,
                     const std::string &itmKey)
    : vbucketId(vbId), deleteItem(del), key(itmKey) {

    if (del) {
        callback.delCb = cb.delCb;
    } else {
        callback.setCb = cb.setCb;
    }

    start = gethrtime();
}
