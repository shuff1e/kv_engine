/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
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

#include "checkpoint_manager.h"
#include "collections/manifest.h"
#include "collections/vbucket_manifest.h"
#include "configuration.h"
#include "ep_vb.h"
#include "failover-table.h"
#include "item.h"
#include "stats.h"
#include "tests/module_tests/collections/test_manifest.h"
#include "tests/module_tests/test_helpers.h"

#include <folly/portability/GTest.h>

class MockVBManifest : public Collections::VB::Manifest {
public:
    MockVBManifest() {
    }

    MockVBManifest(const Collections::KVStore::Manifest& manifestData)
        : Collections::VB::Manifest(manifestData) {
    }

    bool exists(CollectionID identifier) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        return exists_UNLOCKED(identifier);
    }

    size_t size() const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        return map.size();
    }

    bool compareEntry(CollectionID id,
                      const Collections::VB::ManifestEntry& entry,
                      bool ignoreHighSeqno = false) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        if (exists_UNLOCKED(id)) {
            auto itr = map.find(id);
            const auto& myEntry = itr->second;
            if (myEntry == entry) {
                return true;
            } else if (ignoreHighSeqno) {
                // @todo JimW Collections metadata
                // We might not be able to set the high seqno correctly from
                // a persisted manifest pulled from a queued item as we don't
                // write the high seqno to the flatbuffers data. This is
                // changing soon, so for now take a copy of the manifest
                // entry and modify only the high seqno. If the equality check
                // now passes and we're on a path where this matters, we will
                // ignore the original check.
                auto copy = Collections::VB::ManifestEntry(myEntry);
                copy.setHighSeqno(myEntry.getHighSeqno());
                if (myEntry == copy) {
                    return true;
                }
            }
        }
        return false;
    }

    bool operator==(const MockVBManifest& rhs) const {
        std::lock_guard<cb::ReaderLock> readLock(rwlock.reader());
        if (rhs.size() != size()) {
            return false;
        }
        // Check all collections match
        for (const auto& e : map) {
            if (!rhs.compareEntry(e.first, e.second)) {
                return false;
            }
        }

        if (scopes.size() != rhs.scopes.size()) {
            return false;
        }
        // Check all scopes can be found
        for (const auto s : scopes) {
            if (std::find(rhs.scopes.begin(), rhs.scopes.end(), s) ==
                rhs.scopes.end()) {
                return false;
            }
        }
        if (rhs.manifestUid != manifestUid) {
            return false;
        }

        return true;
    }

    bool operator!=(const MockVBManifest& rhs) const {
        return !(*this == rhs);
    }

    // Wire through to private method
    boost::optional<Manifest::CollectionAddition> public_applyCreates(
            ::VBucket& vb, std::vector<CollectionAddition>& changes) {
        auto wHandle = wlock();
        return applyCreates(wHandle, vb, changes);
    }

    boost::optional<std::vector<CollectionID>> public_getCollectionsForScope(
            ScopeID identifier) {
        return getCollectionsForScope(identifier);
    }

protected:
    bool exists_UNLOCKED(CollectionID identifier) const {
        auto itr = map.find(identifier);
        return itr != map.end();
    }

    void expect_true(bool in) const {
        if (!in) {
            std::stringstream ss;
            ss << *this;
            throw std::logic_error("expect_true found false manifest:" +
                                   ss.str());
        }
    }
};

/**
 * Test class that owns an active and replica manifest.
 * Updates applied to the active are applied to the replica by processing
 * the active's checkpoint.
 */
class ActiveReplicaManifest {
public:
    /// Dummy callback to replace the flusher callback so we can create VBuckets
    class DummyCB : public Callback<Vbid> {
    public:
        DummyCB() {
        }

        void callback(Vbid& dummy) {
        }
    };

    ActiveReplicaManifest()
        : vbA(Vbid(0),
              vbucket_state_active,
              global_stats,
              checkpoint_config,
              /*kvshard*/ nullptr,
              /*lastSeqno*/ 0,
              /*lastSnapStart*/ 0,
              /*lastSnapEnd*/ 0,
              /*table*/ nullptr,
              std::make_shared<DummyCB>(),
              /*newSeqnoCb*/ nullptr,
              NoopSyncWriteCompleteCb,
              NoopSeqnoAckCb,
              config,
              EvictionPolicy::Value,
              std::make_unique<Collections::VB::Manifest>()),
          vbR(Vbid(1),
              vbucket_state_replica,
              global_stats,
              checkpoint_config,
              /*kvshard*/ nullptr,
              /*lastSeqno*/ 0,
              /*lastSnapStart*/ 0,
              /*lastSnapEnd*/ snapEnd,
              /*table*/ nullptr,
              std::make_shared<DummyCB>(),
              /*newSeqnoCb*/ nullptr,
              NoopSyncWriteCompleteCb,
              NoopSeqnoAckCb,
              config,
              EvictionPolicy::Value,
              std::make_unique<Collections::VB::Manifest>()),
          lastCompleteDeletionArgs(0) {
    }

    ::testing::AssertionResult update(const std::string& json) {
        try {
            active.wlock().update(vbA, {json});
        } catch (std::exception& e) {
            return ::testing::AssertionFailure()
                   << "Exception thrown for update with " << json
                   << ", e.what:" << e.what();
        }
        try {
            applyCheckpointEventsToReplica();
        } catch (std::exception& e) {
            return ::testing::AssertionFailure()
                   << "Exception thrown for replica update, e.what:"
                   << e.what();
        }
        if (active != replica) {
            return ::testing::AssertionFailure()
                   << "active doesn't match replica active:\n"
                   << active << " replica:\n"
                   << replica;
        }
        return ::testing::AssertionSuccess();
    }

    ::testing::AssertionResult doesKeyContainValidCollection(DocKey key) {
        if (!active.lock().doesKeyContainValidCollection(key)) {
            return ::testing::AssertionFailure() << "active failed the key";
        } else if (!replica.lock().doesKeyContainValidCollection(key)) {
            return ::testing::AssertionFailure() << "replica failed the key";
        }
        return ::testing::AssertionSuccess();
    }

    ::testing::AssertionResult isLogicallyDeleted(DocKey key, int64_t seqno) {
        if (!active.lock().isLogicallyDeleted(key, seqno)) {
            return ::testing::AssertionFailure()
                   << "active failed the key seqno:" << seqno << "\n"
                   << active;
        } else if (!replica.lock().isLogicallyDeleted(key, seqno)) {
            return ::testing::AssertionFailure()
                   << "replica failed the key seqno:" << seqno << "\n"
                   << replica;
        }
        return ::testing::AssertionSuccess();
    }

    bool exists(CollectionID identifier) {
        return active.exists(identifier) && replica.exists(identifier);
    }

    bool checkSize(size_t s) {
        return active.size() == s && replica.size() == s;
    }

    VBucket& getActiveVB() {
        return vbA;
    }

    MockVBManifest& getActiveManifest() {
        return active;
    }

    int64_t getLastSeqno() const {
        return lastSeqno;
    }

    static void getEventsFromCheckpoint(VBucket& vb,
                                        std::vector<queued_item>& events) {
        std::vector<queued_item> items;
        vb.checkpointManager->getAllItemsForPersistence(items);
        for (const auto& qi : items) {
            if (qi->getOperation() == queue_op::system_event) {
                events.push_back(qi);
            }
        }

        if (events.empty()) {
            throw std::logic_error("getEventsFromCheckpoint: no events in " +
                                   vb.getId().to_string());
        }
    }

    /**
     * 1. scan the VBucketManifestTestVBucket's checkpoint for all system
     * events.
     * 2. for all system-events, pretend to be the DcpConsumer and call
     *    the VBucket's manifest's replica functions on.
     * @param replicaVB A vbucket acting as the replica, we will create/delete
     *        collections against this VB.
     * @param replicaManfiest The replica VB's manifest, we will create/delete
     *         collections against this manifest.
     *
     */
    void applyCheckpointEventsToReplica() {
        std::vector<queued_item> events;
        getEventsFromCheckpoint(vbA, events);
        for (const auto& qi : events) {
            lastSeqno = qi->getBySeqno();
            if (qi->getOperation() == queue_op::system_event) {
                switch (SystemEvent(qi->getFlags())) {
                case SystemEvent::Collection: {
                    if (qi->isDeleted()) {
                        auto dcpData =
                                Collections::VB::Manifest::getDropEventData(
                                        {qi->getData(), qi->getNBytes()});
                        // A deleted create means beginDelete collection
                        replica.wlock().replicaDrop(vbR,
                                                    dcpData.manifestUid,
                                                    dcpData.cid,
                                                    qi->getBySeqno());
                    } else {
                        auto dcpData =
                                Collections::VB::Manifest::getCreateEventData(
                                        {qi->getData(), qi->getNBytes()});
                        replica.wlock().replicaAdd(
                                vbR,
                                dcpData.manifestUid,
                                {dcpData.metaData.sid, dcpData.metaData.cid},
                                dcpData.metaData.name,
                                dcpData.metaData.maxTtl,
                                qi->getBySeqno());
                    }
                    break;
                }
                case SystemEvent::Scope: {
                    if (qi->isDeleted()) {
                        auto dcpData = Collections::VB::Manifest::
                                getDropScopeEventData(
                                        {qi->getData(), qi->getNBytes()});
                        // A deleted create means beginDelete collection
                        replica.wlock().replicaDropScope(vbR,
                                                         dcpData.manifestUid,
                                                         dcpData.sid,
                                                         qi->getBySeqno());
                    } else {
                        auto dcpData = Collections::VB::Manifest::
                                getCreateScopeEventData(
                                        {qi->getData(), qi->getNBytes()});
                        replica.wlock().replicaAddScope(vbR,
                                                        dcpData.manifestUid,
                                                        dcpData.sid,
                                                        dcpData.name,
                                                        qi->getBySeqno());
                    }
                    break;
                }
                default:
                    throw std::invalid_argument("Unknown event " +
                                                std::to_string(qi->getFlags()));
                }
            }
        }
    }

    MockVBManifest active;
    MockVBManifest replica;
    EPStats global_stats;
    CheckpointConfig checkpoint_config;
    Configuration config;
    EPVBucket vbA;
    EPVBucket vbR;
    int64_t lastSeqno;
    CollectionID lastCompleteDeletionArgs;

    static const int64_t snapEnd{200};
};

class VBucketManifestTest : public ::testing::Test {
public:
    ActiveReplicaManifest manifest;
    CollectionsManifest cm{CollectionEntry::vegetable};
};

TEST_F(VBucketManifestTest, collectionExists) {
    EXPECT_TRUE(manifest.update(cm));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable));
}

TEST_F(VBucketManifestTest, defaultCollectionExists) {
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"anykey", CollectionEntry::defaultC}));
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::defaultC)));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"anykey", CollectionEntry::defaultC}));
}

TEST_F(VBucketManifestTest, add_to_scope) {
    EXPECT_TRUE(manifest.update(
            cm.add(CollectionEntry::vegetable, ScopeEntry::shop1)));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:cucumber", CollectionEntry::vegetable}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable));
}

TEST_F(VBucketManifestTest, add_delete_different_scopes) {
    // Add dairy to default scope
    EXPECT_TRUE(manifest.update(cm.add(CollectionEntry::dairy)));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::dairy));

    // Add dairy to shop1 scope - we don't create scope creation/deletion events
    cm.add(ScopeEntry::shop1);
    EXPECT_TRUE(manifest.update(
            cm.add(CollectionEntry::dairy2, ScopeEntry::shop1)));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy2}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::dairy2));

    // Remove dairy from default scope
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::dairy)));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy}));
    EXPECT_FALSE(manifest.exists(CollectionEntry::dairy));

    // We can still use dairy in shop1 scope
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy2}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::dairy2));
}

TEST_F(VBucketManifestTest, add_delete_same_scopes) {
    // Add dairy to default scope
    EXPECT_TRUE(manifest.update(cm.add(CollectionEntry::dairy)));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::dairy));

    // Remove dairy from default scope
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::dairy)));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy}));
    EXPECT_FALSE(manifest.exists(CollectionEntry::dairy));

    // Add dairy to shop1 scope - we don't create scope creation/deletion events
    cm.add(ScopeEntry::shop1);
    cm.add(CollectionEntry::dairy, ScopeEntry::shop1);

    EXPECT_TRUE(manifest.update(cm));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::dairy));

    // Remove dairy from shop1 scope
    EXPECT_TRUE(manifest.update(
            cm.remove(CollectionEntry::dairy, ScopeEntry::shop1)));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy}));
    EXPECT_FALSE(manifest.exists(CollectionEntry::dairy));
}

/**
 * Test that we can add a collection to a scope that was previously empty
 */
TEST_F(VBucketManifestTest, add_to_empty_scope) {
    EXPECT_TRUE(manifest.update(cm.add(ScopeEntry::shop1)));

    // We have no collections, but the scope does exist
    EXPECT_TRUE(
            manifest.active.public_getCollectionsForScope(ScopeEntry::shop1));
    EXPECT_EQ(manifest.active.public_getCollectionsForScope(ScopeEntry::shop1)
                      ->size(),
              0);

    // Add meat to the shop 1 scope
    EXPECT_TRUE(
            manifest.update(cm.add(CollectionEntry::meat, ScopeEntry::shop1)));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"meat:beef", CollectionEntry::meat}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::meat));
}

/**
 * Test that all collections in a scope get deleted just by the scope drop
 */
TEST_F(VBucketManifestTest, drop_scope) {
    EXPECT_TRUE(manifest.update(cm.add(ScopeEntry::shop1)));
    EXPECT_TRUE(manifest.update(
            cm.add(CollectionEntry::fruit, ScopeEntry::shop1)
                    .add(CollectionEntry::dairy, ScopeEntry::shop1)
                    .add(CollectionEntry::meat, ScopeEntry::shop1)));
    EXPECT_TRUE(manifest.exists(CollectionEntry::fruit));
    EXPECT_TRUE(manifest.exists(CollectionEntry::dairy));
    EXPECT_TRUE(manifest.exists(CollectionEntry::meat));
    EXPECT_TRUE(manifest.update(cm.remove(ScopeEntry::shop1)));
    EXPECT_FALSE(manifest.exists(CollectionEntry::fruit));
    EXPECT_FALSE(manifest.exists(CollectionEntry::dairy));
    EXPECT_FALSE(manifest.exists(CollectionEntry::meat));
}
/**
 * Test that we can drop a scope (simulate this by dropping all the
 * collections within it) then add it back.
 */
TEST_F(VBucketManifestTest, drop_scope_then_add) {
    // Add meat to the shop 1 scope
    EXPECT_TRUE(manifest.update(
            cm.add(ScopeEntry::shop1)
                    .add(CollectionEntry::meat, ScopeEntry::shop1)));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"meat:beef", CollectionEntry::meat}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::meat));

    // Now remove the meat collection and shop1 scope
    EXPECT_TRUE(
            manifest.update(cm.remove(CollectionEntry::meat, ScopeEntry::shop1)
                                    .remove(ScopeEntry::shop1)));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"meat:beef", CollectionEntry::meat}));
    EXPECT_FALSE(manifest.exists(CollectionEntry::meat));

    // We have no collections, and the scope does not exist
    EXPECT_FALSE(
            manifest.active.public_getCollectionsForScope(ScopeEntry::shop1));

    // And add the meat collection back to the shop 1 scope
    EXPECT_TRUE(manifest.update(
            cm.add(ScopeEntry::shop1)
                    .add(CollectionEntry::meat, ScopeEntry::shop1)));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"meat:beef", CollectionEntry::meat}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::meat));

    // We have only 1 collection
    EXPECT_TRUE(
            manifest.active.public_getCollectionsForScope(ScopeEntry::shop1));
    EXPECT_EQ(manifest.active.public_getCollectionsForScope(ScopeEntry::shop1)
                      ->size(),
              1);
}

TEST_F(VBucketManifestTest, duplicate_cid_different_scope) {
    // Add dairy to default scope
    EXPECT_TRUE(manifest.update(cm.add(CollectionEntry::dairy)));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::dairy));

    // Add dairy to shop1 scope
    EXPECT_FALSE(
            manifest.update(cm.add(CollectionEntry::dairy, ScopeEntry::shop1)));
}

TEST_F(VBucketManifestTest, add_delete_in_one_update) {
    EXPECT_TRUE(manifest.update(cm));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:cucumber", CollectionEntry::vegetable}));
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::vegetable)
                                        .add(CollectionEntry::vegetable2)));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:cucumber", CollectionEntry::vegetable2}));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable2));
}

TEST_F(VBucketManifestTest, updates) {
    EXPECT_TRUE(manifest.checkSize(1));
    EXPECT_TRUE(manifest.exists(CollectionEntry::defaultC));

    EXPECT_TRUE(manifest.update(cm));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable));

    EXPECT_TRUE(manifest.update(cm.add(CollectionEntry::fruit)));
    EXPECT_TRUE(manifest.checkSize(3));
    EXPECT_TRUE(manifest.exists(CollectionEntry::fruit));

    EXPECT_TRUE(manifest.update(
            cm.add(CollectionEntry::meat).add(CollectionEntry::dairy)));
    EXPECT_TRUE(manifest.checkSize(5));
    EXPECT_TRUE(manifest.exists(CollectionEntry::meat));
}

TEST_F(VBucketManifestTest, updates2) {
    EXPECT_TRUE(manifest.update(cm.add(CollectionEntry::fruit)
                                        .add(CollectionEntry::meat)
                                        .add(CollectionEntry::dairy)));
    EXPECT_TRUE(manifest.checkSize(5));

    // Remove meat and dairy
    EXPECT_TRUE(manifest.update(
            cm.remove(CollectionEntry::meat).remove(CollectionEntry::dairy)));
    EXPECT_TRUE(manifest.checkSize(3));
    EXPECT_FALSE(manifest.exists(CollectionEntry::meat));
    EXPECT_FALSE(manifest.exists(CollectionEntry::dairy));

    // But vegetable is accessible, the others are locked out
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"anykey", CollectionEntry::defaultC}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"meat:chicken", CollectionEntry::meat}));
}

TEST_F(VBucketManifestTest, updates3) {
    EXPECT_TRUE(manifest.update(cm.add(CollectionEntry::fruit)
                                        .add(CollectionEntry::meat)
                                        .add(CollectionEntry::dairy)));
    EXPECT_TRUE(manifest.checkSize(5));

    // Remove everything
    CollectionsManifest cm2(NoDefault{});
    EXPECT_TRUE(manifest.update(cm2));
    EXPECT_TRUE(manifest.checkSize(0));
    EXPECT_FALSE(manifest.exists(CollectionEntry::defaultC));
    EXPECT_FALSE(manifest.exists(CollectionEntry::vegetable));
    EXPECT_FALSE(manifest.exists(CollectionEntry::fruit));
    EXPECT_FALSE(manifest.exists(CollectionEntry::meat));
    EXPECT_FALSE(manifest.exists(CollectionEntry::dairy));

    // But vegetable is accessible, the others are 'locked' out
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:milk", CollectionEntry::dairy}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"meat:chicken", CollectionEntry::meat}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"fruit:apple", CollectionEntry::fruit}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"anykey", CollectionEntry::defaultC}));
}

TEST_F(VBucketManifestTest, add_delete_add) {
    // remove default and add vegetable
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::defaultC)));
    auto seqno = manifest.getLastSeqno(); // seqno of the vegetable addition
    EXPECT_TRUE(manifest.checkSize(1));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));

    // The first manifest.update has dropped default collection and added
    // vegetable - test $default key with a seqno it could of existed with
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            StoredDocKey{"anykey", CollectionEntry::defaultC}, seqno - 1));
    // But vegetable is still good
    EXPECT_FALSE(manifest.isLogicallyDeleted(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable},
            seqno + 1));

    // remove vegetable
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::vegetable)));
    seqno = manifest.getLastSeqno();
    EXPECT_TRUE(manifest.checkSize(0));
    EXPECT_FALSE(manifest.exists(CollectionEntry::vegetable));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));

    // vegetable is now a deleted collection
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable},
            seqno));

    // add vegetable2
    EXPECT_TRUE(manifest.update(cm.add(CollectionEntry::vegetable2)));
    auto oldSeqno = seqno;
    auto newSeqno = manifest.getLastSeqno();
    EXPECT_TRUE(manifest.checkSize(1));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable2));
    EXPECT_FALSE(manifest.exists(CollectionEntry::vegetable));

    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable2}));

    // Now we expect older vegetables to be deleting and newer not to be.
    EXPECT_FALSE(manifest.isLogicallyDeleted(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable2},
            newSeqno + 1));
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable},
            oldSeqno));
}

TEST_F(VBucketManifestTest, add_beginDelete_delete) {
    EXPECT_TRUE(manifest.update(cm));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));

    // remove vegetable
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::vegetable)));
    auto seqno = manifest.getLastSeqno();
    EXPECT_TRUE(manifest.checkSize(1));
    EXPECT_FALSE(manifest.exists(CollectionEntry::vegetable));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));
    EXPECT_TRUE(manifest.isLogicallyDeleted(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable},
            seqno));
}

TEST_F(VBucketManifestTest, add_beginDelete_add_delete) {
    EXPECT_TRUE(manifest.update(cm));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));

    // remove vegetable
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::vegetable)));
    EXPECT_TRUE(manifest.checkSize(1));
    EXPECT_FALSE(manifest.exists(CollectionEntry::vegetable));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));

    // add vegetable:2
    EXPECT_TRUE(manifest.update(cm.add(CollectionEntry::vegetable2)));
    EXPECT_TRUE(manifest.checkSize(2));
    EXPECT_TRUE(manifest.exists(CollectionEntry::vegetable2));
    EXPECT_FALSE(manifest.exists(CollectionEntry::vegetable));

    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable2}));
}

// Check that a deleting collection doesn't keep adding system events
TEST_F(VBucketManifestTest, doubleDelete) {
    auto seqno = manifest.getActiveVB().getHighSeqno();
    // add vegetable
    EXPECT_TRUE(manifest.update(cm));
    EXPECT_LT(seqno, manifest.getActiveVB().getHighSeqno());
    seqno = manifest.getActiveVB().getHighSeqno();

    // Apply same manifest (different revision). Nothing will be created or
    // deleted. Apply direct to vbm, not via manifest.update as that would
    // complain about the lack of events
    manifest.getActiveManifest().wlock().update(manifest.getActiveVB(), {cm});

    EXPECT_EQ(seqno, manifest.getActiveVB().getHighSeqno());
    seqno = manifest.getActiveVB().getHighSeqno();

    // Now delete vegetable
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::vegetable)));

    EXPECT_LT(seqno, manifest.getActiveVB().getHighSeqno());
    seqno = manifest.getActiveVB().getHighSeqno();

    // same again, should have nothing created or deleted
    manifest.getActiveManifest().wlock().update(manifest.getActiveVB(), {cm});

    EXPECT_EQ(seqno, manifest.getActiveVB().getHighSeqno());
}

TEST_F(VBucketManifestTest, replica_add_remove) {
    // add vegetable
    EXPECT_TRUE(manifest.update(cm));

    // add meat & dairy
    EXPECT_TRUE(manifest.update(
            cm.add(CollectionEntry::meat).add(CollectionEntry::dairy)));

    // remove vegetable
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::vegetable)));

    // remove $default
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::defaultC)));

    // Check we can access the remaining collections
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"vegetable:carrot", CollectionEntry::vegetable}));
    EXPECT_FALSE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"anykey", CollectionEntry::defaultC}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"meat:sausage", CollectionEntry::meat}));
    EXPECT_TRUE(manifest.doesKeyContainValidCollection(
            StoredDocKey{"dairy:butter", CollectionEntry::dairy}));
}

TEST_F(VBucketManifestTest, check_applyChanges) {
    std::vector<Collections::VB::Manifest::CollectionAddition>
            changes; // start out empty
    auto value = manifest.getActiveManifest().public_applyCreates(
            manifest.getActiveVB(), changes);
    EXPECT_FALSE(value.is_initialized());
    changes.push_back({std::make_pair(0, 8), "name1"});
    value = manifest.getActiveManifest().public_applyCreates(
            manifest.getActiveVB(), changes);
    ASSERT_TRUE(value.is_initialized());
    EXPECT_EQ(0, value.get().identifiers.first);
    EXPECT_EQ(8, value.get().identifiers.second);
    EXPECT_EQ("name1", value.get().name);
    EXPECT_TRUE(changes.empty());

    changes.push_back({std::make_pair(0, 8), "name2"});
    changes.push_back({std::make_pair(0, 9), "name3"});
    EXPECT_EQ(1, manifest.getActiveManifest().size());
    value = manifest.getActiveManifest().public_applyCreates(
            manifest.getActiveVB(), changes);
    ASSERT_TRUE(value.is_initialized());
    EXPECT_EQ(0, value.get().identifiers.first);
    EXPECT_EQ(9, value.get().identifiers.second);
    EXPECT_EQ("name3", value.get().name);
    EXPECT_EQ(2, manifest.getActiveManifest().size());
}

class VBucketManifestCachingReadHandle : public VBucketManifestTest {};

TEST_F(VBucketManifestCachingReadHandle, basic) {
    // Add
    EXPECT_TRUE(manifest.update(cm));

    StoredDocKey key1{"vegetable:v1", CollectionEntry::vegetable};
    StoredDocKey key2{"fruit:v1", CollectionEntry::fruit};

    {
        auto rh = manifest.active.lock(key1);
        EXPECT_TRUE(rh.valid());
        EXPECT_EQ(CollectionEntry::vegetable.getId(),
                  rh.getKey().getCollectionID());
    }

    {
        auto rh = manifest.active.lock(key2);
        EXPECT_FALSE(rh.valid());

        // cached the key
        EXPECT_EQ(CollectionEntry::fruit.getId(),
                  rh.getKey().getCollectionID());
    }
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::vegetable)));

    {
        auto rh = manifest.active.lock(key1);
        EXPECT_FALSE(rh.valid());

        EXPECT_EQ(CollectionEntry::vegetable.getId(),
                  rh.getKey().getCollectionID());
    }
    {
        auto rh = manifest.active.lock(key2);
        EXPECT_FALSE(rh.valid());
        EXPECT_EQ(CollectionEntry::fruit.getId(),
                  rh.getKey().getCollectionID());
    }
}

TEST_F(VBucketManifestCachingReadHandle, deleted_default) {
    // Check we can still get an iterator into the map when the default
    // collection is logically deleted only (i.e marked deleted, but in the map)
    EXPECT_TRUE(manifest.update(cm.remove(CollectionEntry::defaultC)));
    StoredDocKey key{"fruit:v1", CollectionEntry::defaultC};
    auto rh = manifest.active.lock(key);
    EXPECT_TRUE(rh.isLogicallyDeleted(0));
}
