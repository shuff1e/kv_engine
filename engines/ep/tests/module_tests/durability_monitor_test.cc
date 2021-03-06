/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
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

#include "durability_monitor_test.h"

#include "checkpoint_manager.h"
#include "test_helpers.h"
#include "vbucket_utils.h"

#include "durability/active_durability_monitor.h"
#include "durability/passive_durability_monitor.h"

#include "../mock/mock_synchronous_ep_engine.h"

void ActiveDurabilityMonitorTest::SetUp() {
    STParameterizedBucketTest::SetUp();
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);
    vb = store->getVBuckets().getBucket(vbid).get();
    ASSERT_TRUE(vb);

    monitor = &VBucketTestIntrospector::public_getActiveDM(*vb);
    ASSERT_TRUE(monitor);

    // The ActiveDM must not track anything before the Replication Chain is set.
    auto& adm = getActiveDM();
    ASSERT_EQ(0, adm.getFirstChainSize());
    ASSERT_EQ(0, adm.getFirstChainMajority());

    adm.setReplicationTopology(nlohmann::json::array({{active, replica1}}));
    ASSERT_EQ(2, adm.getFirstChainSize());
    ASSERT_EQ(2, adm.getFirstChainMajority());
}

ActiveDurabilityMonitor& ActiveDurabilityMonitorTest::getActiveDM() const {
    return dynamic_cast<ActiveDurabilityMonitor&>(*monitor);
}

void DurabilityMonitorTest::addSyncWrite(int64_t seqno,
                                         cb::durability::Requirements req) {
    auto item = Item(makeStoredDocKey("key" + std::to_string(seqno)),
                     0 /*flags*/,
                     0 /*exp*/,
                     "value",
                     5 /*valueSize*/,
                     PROTOCOL_BINARY_RAW_BYTES,
                     0 /*cas*/,
                     seqno);
    using namespace cb::durability;
    item.setPendingSyncWrite(req);
    // Note: necessary for non-auto-generated seqno
    vb->checkpointManager->createSnapshot(seqno, seqno);
    // Note: need to go through VBucket::processSet to set the given bySeqno
    ASSERT_EQ(MutationStatus::WasClean, processSet(item));
}

size_t ActiveDurabilityMonitorTest::addSyncWrites(
        int64_t seqnoStart,
        int64_t seqnoEnd,
        cb::durability::Requirements req) {
    size_t expectedNumTracked = monitor->getNumTracked();
    size_t added = 0;
    for (auto seqno = seqnoStart; seqno <= seqnoEnd; seqno++) {
        addSyncWrite(seqno, req);
        added++;
        expectedNumTracked++;
        EXPECT_EQ(expectedNumTracked, monitor->getNumTracked());
    }
    return added;
}

void DurabilityMonitorTest::addSyncWrites(const std::vector<int64_t>& seqnos,
                                          cb::durability::Requirements req) {
    for (auto seqno : seqnos) {
        addSyncWrite(seqno, req);
    }
}

MutationStatus DurabilityMonitorTest::processSet(Item& item) {
    auto htRes = vb->ht.findForWrite(item.getKey());
    VBQueueItemCtx ctx;
    ctx.genBySeqno = GenerateBySeqno::No;
    ctx.durability =
            DurabilityItemCtx{item.getDurabilityReqs(), /*cookie*/ nullptr};
    return vb
            ->processSet(htRes.lock,
                         htRes.storedValue,
                         item,
                         item.getCas(),
                         true /*allow_existing*/,
                         false /*has_metadata*/,
                         ctx,
                         {/*no predicate*/})
            .first;
}

void ActiveDurabilityMonitorTest::assertNodeTracking(
        const std::string& node,
        uint64_t lastWriteSeqno,
        uint64_t lastAckSeqno) const {
    auto& adm = getActiveDM();
    ASSERT_EQ(lastWriteSeqno, adm.getNodeWriteSeqno(node));
    ASSERT_EQ(lastAckSeqno, adm.getNodeAckSeqno(node));
}

void ActiveDurabilityMonitorTest::testLocalAck(int64_t ackSeqno,
                                               size_t expectedNumTracked,
                                               int64_t expectedLastWriteSeqno,
                                               int64_t expectedLastAckSeqno) {
    vb->setPersistenceSeqno(ackSeqno);
    monitor->notifyLocalPersistence();
    EXPECT_EQ(expectedNumTracked, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(
                active, expectedLastWriteSeqno, expectedLastAckSeqno);
    }
}

void ActiveDurabilityMonitorTest::testSeqnoAckReceived(
        const std::string& replica,
        int64_t ackSeqno,
        size_t expectedNumTracked,
        int64_t expectedLastWriteSeqno,
        int64_t expectedLastAckSeqno) const {
    EXPECT_NO_THROW(getActiveDM().seqnoAckReceived(replica, ackSeqno));
    EXPECT_EQ(expectedNumTracked, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(
                replica, expectedLastWriteSeqno, expectedLastAckSeqno);
    }
}

bool ActiveDurabilityMonitorTest::testDurabilityPossible(
        const nlohmann::json::array_t& topology,
        queued_item& item,
        uint8_t expectedFirstChainSize,
        uint8_t expectedFirstChainMajority,
        uint8_t expectedSecondChainSize,
        uint8_t expectedSecondChainMajority) {
    auto& adm = getActiveDM();
    auto expectAddSyncWriteImpossible = [&adm, &item]() -> void {
        try {
            adm.addSyncWrite(nullptr /*cookie*/, item);
        } catch (const std::logic_error& e) {
            EXPECT_TRUE(std::string(e.what()).find("Impossible") !=
                        std::string::npos);
            return;
        }
        FAIL();
    };

    adm.setReplicationTopology(topology);
    EXPECT_EQ(expectedFirstChainSize, adm.getFirstChainSize());
    EXPECT_EQ(expectedFirstChainMajority, adm.getFirstChainMajority());
    EXPECT_EQ(expectedSecondChainSize, adm.getSecondChainSize());
    EXPECT_EQ(expectedSecondChainMajority, adm.getSecondChainMajority());

    if (expectedFirstChainSize < expectedFirstChainMajority ||
        expectedSecondChainSize < expectedSecondChainMajority) {
        expectAddSyncWriteImpossible();
        EXPECT_EQ(0, monitor->getNumTracked());
        return false;
    }

    adm.addSyncWrite(nullptr /*cookie*/, item);
    // @todo: There is an open issue (MB-33395) that requires that we wipe
    //     out the tracked container before resetting the replication chain
    //     (note some tests call this function multiple times).
    //     We can remove this step as soon as MB-33395 is fixed.
    // Note: This is also an implicit check on the number of tracked items.
    if (const auto numTracked = monitor->getNumTracked()) {
        EXPECT_EQ(numTracked /*numRemoved*/, adm.wipeTracked());
    }
    return true;
}

void ActiveDurabilityMonitorTest::testChainEmpty(
        const nlohmann::json::array_t& topology,
        DurabilityMonitor::ReplicationChainName chainName) {
    try {
        getActiveDM().setReplicationTopology(topology);
    } catch (const std::invalid_argument& e) {
        EXPECT_TRUE(std::string(e.what()).find(to_string(chainName) +
                                               " chain cannot be empty") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

void ActiveDurabilityMonitorTest::testChainUndefinedActive(
        const nlohmann::json::array_t& topology,
        DurabilityMonitor::ReplicationChainName chainName) {
    try {
        getActiveDM().setReplicationTopology(topology);
    } catch (const std::invalid_argument& e) {
        EXPECT_TRUE(std::string(e.what()).find(
                            "first node in " + to_string(chainName) +
                            " chain (active) "
                            "cannot be undefined") != std::string::npos);
        return;
    }
    FAIL();
}

void ActiveDurabilityMonitorTest::testChainTooManyNodes(
        const nlohmann::json::array_t& topology,
        DurabilityMonitor::ReplicationChainName chainName) {
    try {
        getActiveDM().setReplicationTopology(topology);
    } catch (const std::invalid_argument& e) {
        EXPECT_TRUE(std::string(e.what()).find("Too many nodes in " +
                                               to_string(chainName) +
                                               " chain") != std::string::npos);
        return;
    }
    FAIL();
}

void ActiveDurabilityMonitorTest::testChainDuplicateNode(
        const nlohmann::json::array_t& topology) {
    try {
        getActiveDM().setReplicationTopology(topology);
    } catch (const std::invalid_argument& e) {
        EXPECT_TRUE(std::string(e.what()).find("Duplicate node") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

void ActiveDurabilityMonitorTest::testSeqnoAckSmallerThanLastAck() {
    DurabilityMonitorTest::addSyncWrites({1, 2} /*seqnos*/);
    ASSERT_EQ(2, monitor->getNumTracked());

    SCOPED_TRACE("");
    assertNodeTracking(
            active, 2 /*expectedLastWriteSeqno*/, 0 /*expectedLastAckSeqno*/);

    // This call removes s:1
    testSeqnoAckReceived(replica1,
                         1 /*ackSeqno*/,
                         1 /*expectedNumTracked*/,
                         1 /*expectedLastWriteSeqno*/,
                         1 /*expectedLastAckSeqno*/);

    try {
        getActiveDM().seqnoAckReceived(replica1, 0 /*preparedSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("Monotonic") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

void ActiveDurabilityMonitorTest::testSeqnoAckEqualToPending(
        const std::vector<std::string>& nodesToAck) {
    int64_t seqnoStart = 1;
    int64_t seqnoEnd = 3;

    auto numItems = addSyncWrites(seqnoStart, seqnoEnd);
    ASSERT_EQ(3, numItems);
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 3 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    {
        SCOPED_TRACE("");
        for (const auto& node : nodesToAck) {
            assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        }
    }

    for (int64_t ackSeqno = seqnoStart; ackSeqno <= seqnoEnd; ackSeqno++) {
        SCOPED_TRACE("");
        for (const auto& node : nodesToAck) {
            // We must have a last element if we're iterating on nodesToAck.
            if (node == nodesToAck.back()) {
                // We should only commit the SyncWrite for the last node that we
                // ack
                --numItems;
            }
            testSeqnoAckReceived(node,
                                 ackSeqno,
                                 numItems /*expectedNumTracked*/,
                                 ackSeqno /*expectedLastWriteSeqno*/,
                                 ackSeqno /*expectedLastAckSeqno*/);
        }
    }
}

void ActiveDurabilityMonitorTest::
        testSeqnoAckGreaterThanPendingContinuousSeqnos(
                const std::vector<std::string>& nodesToAck) {
    auto numTracked = addSyncWrites(1 /*seqnoStart*/, 3 /*seqnoEnd*/);
    ASSERT_EQ(3, numTracked);
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 3 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    {
        SCOPED_TRACE("");
        for (const auto& node : nodesToAck) {
            assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        }
    }

    for (const auto& node : nodesToAck) {
        SCOPED_TRACE("");
        // We must have a last element if we're iterating on nodesToAck.
        if (node == nodesToAck.back()) {
            // We should only commit the SyncWrite for the last node that we
            // ack
            numTracked = 1;
        }
        testSeqnoAckReceived(node,
                             2 /*ackSeqno*/,
                             numTracked /*expectedNumTracked*/,
                             2 /*expectedLastWriteSeqno*/,
                             2 /*expectedLastAckSeqno*/);
    }
}

void ActiveDurabilityMonitorTest::testSeqnoAckGreaterThanPendingSparseSeqnos(
        const std::vector<std::string>& nodesToAck) {
    DurabilityMonitorTest::addSyncWrites({1, 3, 5} /*seqnos*/);
    auto numTracked = monitor->getNumTracked();
    ASSERT_EQ(3, numTracked);
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 5 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    {
        SCOPED_TRACE("");
        for (const auto& node : nodesToAck) {
            assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        }
    }

    for (const auto& node : nodesToAck) {
        SCOPED_TRACE("");
        // We must have a last element if we're iterating on nodesToAck.
        if (node == nodesToAck.back()) {
            // We should only commit the SyncWrite for the last node that we
            // ack
            numTracked = 1;
        }
        testSeqnoAckReceived(node,
                             4 /*ackSeqno*/,
                             numTracked /*expectedNumTracked*/,
                             3 /*expectedLastWriteSeqno*/,
                             4 /*expectedLastAckSeqno*/);
    }
}

void ActiveDurabilityMonitorTest::
        testSeqnoAckGreaterThanLastTrackedContinuousSeqnos(
                const std::vector<std::string>& nodesToAck) {
    auto numTracked = addSyncWrites(1 /*seqnoStart*/, 3 /*seqnoEnd*/);
    ASSERT_EQ(3, numTracked);
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 3 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    {
        SCOPED_TRACE("");
        for (const auto& node : nodesToAck) {
            assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        }
    }

    for (const auto& node : nodesToAck) {
        SCOPED_TRACE("");
        // We must have a last element if we're iterating on nodesToAck.
        if (node == nodesToAck.back()) {
            // We should only commit the SyncWrite for the last node that we
            // ack
            numTracked = 0;
        }
        testSeqnoAckReceived(node,
                             4 /*ackSeqno*/,
                             numTracked /*expectedNumTracked*/,
                             3 /*expectedLastWriteSeqno*/,
                             4 /*expectedLastAckSeqno*/);
    }
}

void ActiveDurabilityMonitorTest::
        testSeqnoAckGreaterThanLastTrackedSparseSeqnos(
                const std::vector<std::string>& nodesToAck) {
    DurabilityMonitorTest::addSyncWrites({1, 3, 5} /*seqnos*/);
    auto numTracked = monitor->getNumTracked();
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 5 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    {
        SCOPED_TRACE("");
        for (const auto& node : nodesToAck) {
            assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        }
    }

    for (const auto& node : nodesToAck) {
        SCOPED_TRACE("");
        // We must have a last element if we're iterating on nodesToAck.
        if (node == nodesToAck.back()) {
            // We should only commit the SyncWrite for the last node that we
            // ack
            numTracked = 0;
        }
        testSeqnoAckReceived(node,
                             10 /*ackSeqno*/,
                             numTracked /*expectedNumTracked*/,
                             5 /*expectedLastWriteSeqno*/,
                             10 /*expectedLastAckSeqno*/);
    }
}

void ActiveDurabilityMonitorTest::testSeqnoAckPersistToMajority(
        const std::vector<std::string>& nodesToAck) {
    DurabilityMonitorTest::addSyncWrites(
            {1, 3, 5} /*seqnos*/,
            {cb::durability::Level::PersistToMajority, {}});
    EXPECT_EQ(3, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    {
        SCOPED_TRACE("");
        for (const auto& node : nodesToAck) {
            assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        }
    }

    const int64_t ackSeqno = 10;

    for (const auto& node : nodesToAck) {
        SCOPED_TRACE("");
        testSeqnoAckReceived(node,
                             ackSeqno /*ackSeqno*/,
                             3 /*expectedNumTracked*/,
                             5 /*expectedLastWriteSeqno*/,
                             ackSeqno /*expectedLastAckSeqno*/);
    }

    testLocalAck(ackSeqno,
                 0 /*expectedNumTracked*/,
                 5 /*expectedLastWriteSeqno*/,
                 0 /*expectedLastAckSeqno*/);
}

void ActiveDurabilityMonitorTest::testSeqnoAckMultipleReplicas(
        const std::vector<std::string>& nodesToAck,
        const std::vector<std::string>& unchangedNodes) {
    int64_t seqno = 1;
    addSyncWrite(seqno);
    ASSERT_EQ(1, monitor->getNumTracked());

    // Active has implicitly ack'ed
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 1 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    // Nothing ack'ed yet by Replicas
    for (const auto& node : nodesToAck) {
        SCOPED_TRACE("");
        assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }
    // Nothing ack'ed yet by Replicas
    for (const auto& node : unchangedNodes) {
        SCOPED_TRACE("");
        assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    // Nothing committed
    EXPECT_EQ(1, monitor->getNumTracked());

    {
        SCOPED_TRACE("");

        auto expectedNumTracked = seqno;
        for (const auto& node : nodesToAck) {
            SCOPED_TRACE("");
            // We must have a last element if we're iterating on nodesToAck.
            if (node == nodesToAck.back()) {
                // We should only commit the SyncWrite for the last node that we
                // ack
                expectedNumTracked = 0;
            }
            testSeqnoAckReceived(node,
                                 seqno /*ackSeqno*/,
                                 expectedNumTracked /*expectedNumTracked*/,
                                 seqno /*expectedLastWriteSeqno*/,
                                 seqno /*expectedLastAckSeqno*/);
        }

        for (const auto& node : unchangedNodes) {
            assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        }
    }
}

void ActiveDurabilityMonitorTest::testSeqnoAckMajorityAndPersistOnMaster(
        const std::vector<std::string>& nodesToAck) {
    DurabilityMonitorTest::addSyncWrites(
            {1, 3, 5} /*seqnos*/,
            {cb::durability::Level::MajorityAndPersistOnMaster, {}});
    ASSERT_EQ(3, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }
    // Nothing ack'ed yet by Replicas
    for (const auto& node : nodesToAck) {
        SCOPED_TRACE("");
        assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    const int64_t ackSeqno = 10;

    // Ack all replicas, nothing should be committed
    for (const auto& node : nodesToAck) {
        SCOPED_TRACE("");
        testSeqnoAckReceived(node,
                             ackSeqno,
                             3 /*expectedNumTracked*/,
                             5 /*expectedLastWriteSeqno*/,
                             ackSeqno /*expectedLastAckSeqno*/);
    }

    SCOPED_TRACE("");

    // Simulate the Flusher that notifies the local DurabilityMonitor after
    // persistence. Then: all satisfied, all committed.
    testLocalAck(ackSeqno /*ackSeqno*/,
                 0 /*expectedNumTracked*/,
                 5 /*expectedLastWriteSeqno*/,
                 0 /*expectedLastAckSeqno*/);
}

void ActiveDurabilityMonitorTest::testSeqnoAckUnknownNode(
        const std::string& nodeToAck,
        const std::vector<std::string>& unchangedNodes) {
    DurabilityMonitorTest::addSyncWrite(1);
    ASSERT_EQ(1, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 1 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }
    // Nothing ack'ed yet
    for (const auto& node : unchangedNodes) {
        SCOPED_TRACE("");
        assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    EXPECT_NO_THROW(getActiveDM().seqnoAckReceived(nodeToAck, 1));

    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 1 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }
    // Nothing ack'ed yet
    for (const auto& node : unchangedNodes) {
        SCOPED_TRACE("");
        assertNodeTracking(node, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }
    EXPECT_EQ(1, monitor->getNumTracked());
}

TEST_P(ActiveDurabilityMonitorTest, AddSyncWrite) {
    EXPECT_EQ(3, addSyncWrites(1 /*seqnoStart*/, 3 /*seqnoEnd*/));
}

TEST_P(ActiveDurabilityMonitorTest, SeqnoAckReceivedUnknownNode) {
    testSeqnoAckUnknownNode(replica2, {replica1});
}

TEST_P(ActiveDurabilityMonitorTest, SeqnoAckReceivedUnknownNodeTwoChains) {
    getActiveDM().setReplicationTopology(
            nlohmann::json::array({{active, replica1}, {active, replica2}}));
    testSeqnoAckUnknownNode(replica3, {replica1, replica2});
}

TEST_P(ActiveDurabilityMonitorTest, SeqnoAckReceivedSmallerThanLastAcked) {
    testSeqnoAckSmallerThanLastAck();
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceivedSmallerThanLastAckedSecondChain) {
    getActiveDM().setReplicationTopology({{active}, {active, replica1}});
    testSeqnoAckSmallerThanLastAck();
}

TEST_P(ActiveDurabilityMonitorTest, SeqnoAckReceivedEqualPending) {
    testSeqnoAckEqualToPending(std::vector<std::string>{replica1});
}

TEST_P(ActiveDurabilityMonitorTest, SeqnoAckReceivedEqualPendingTwoChains) {
    auto& adm = getActiveDM();
    adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}, {active, replica2}}));
    ASSERT_EQ(2, adm.getFirstChainSize());
    ASSERT_EQ(2, adm.getSecondChainSize());

    testSeqnoAckEqualToPending(std::vector<std::string>{replica1, replica2});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanPending_ContinuousSeqnos) {
    testSeqnoAckGreaterThanPendingContinuousSeqnos({replica1});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanPending_ContinuousSeqnosTwoChains) {
    auto& adm = getActiveDM();
    adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}, {active, replica2}}));
    ASSERT_EQ(2, adm.getFirstChainSize());
    ASSERT_EQ(2, adm.getSecondChainSize());

    testSeqnoAckGreaterThanPendingContinuousSeqnos({replica1, replica2});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanPending_SparseSeqnos) {
    testSeqnoAckGreaterThanPendingSparseSeqnos({replica1});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanPending_SparseSeqnosTwoChains) {
    auto& adm = getActiveDM();
    adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}, {active, replica2}}));
    ASSERT_EQ(2, adm.getFirstChainSize());
    ASSERT_EQ(2, adm.getSecondChainSize());

    testSeqnoAckGreaterThanPendingSparseSeqnos({replica1, replica2});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanLastTracked_ContinuousSeqnos) {
    testSeqnoAckGreaterThanLastTrackedContinuousSeqnos({replica1});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanLastTracked_ContinuousSeqnosTwoChains) {
    auto& adm = getActiveDM();
    adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}, {active, replica2}}));
    ASSERT_EQ(2, adm.getFirstChainSize());
    ASSERT_EQ(2, adm.getSecondChainSize());

    testSeqnoAckGreaterThanLastTrackedContinuousSeqnos({replica1, replica2});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanLastTracked_SparseSeqnos) {
    testSeqnoAckGreaterThanLastTrackedSparseSeqnos({replica1});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanLastTracked_SparseSeqnosTwoChains) {
    auto& adm = getActiveDM();
    adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}, {active, replica2}}));
    ASSERT_EQ(2, adm.getFirstChainSize());
    ASSERT_EQ(2, adm.getSecondChainSize());

    testSeqnoAckGreaterThanLastTrackedSparseSeqnos({replica1, replica2});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceived_PersistToSecondChainNewActive) {
    auto& adm = getActiveDM();
    adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}, {replica2, replica1}}));
    ASSERT_EQ(2, adm.getFirstChainSize());
    ASSERT_EQ(2, adm.getSecondChainSize());

    DurabilityMonitorTest::addSyncWrites({1, 3, 5} /*seqnos*/);
    EXPECT_EQ(3, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 5 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        assertNodeTracking(replica1, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        assertNodeTracking(replica2, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    const int64_t ackSeqno = 10;

    SCOPED_TRACE("");
    // active has already acked, replica 1 now acks which gives us majority on
    // both fist and second chain. However, we still need the ack from our new
    // active (replica2)
    testSeqnoAckReceived(replica1,
                         ackSeqno /*ackSeqno*/,
                         3 /*expectedNumTracked*/,
                         5 /*expectedLastWriteSeqno*/,
                         ackSeqno /*expectedLastAckSeqno*/);
    testSeqnoAckReceived(replica2,
                         ackSeqno /*ackSeqno*/,
                         0 /*expectedNumTracked*/,
                         5 /*expectedLastWriteSeqno*/,
                         ackSeqno /*expectedLastAckSeqno*/);
}

// @todo: Refactor test suite and expand test cases
TEST_P(ActiveDurabilityMonitorTest, SeqnoAckReceived_PersistToMajority) {
    testSeqnoAckPersistToMajority({replica1});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceived_PersistToMajorityTwoChains) {
    auto& adm = getActiveDM();
    adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}, {active, replica2}}));
    ASSERT_EQ(2, adm.getFirstChainSize());
    ASSERT_EQ(2, adm.getSecondChainSize());

    testSeqnoAckPersistToMajority({replica1, replica2});
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_NotAnArray) {
    try {
        getActiveDM().setReplicationTopology(nlohmann::json::object());
    } catch (const std::invalid_argument& e) {
        EXPECT_TRUE(std::string(e.what()).find("Topology is not an array") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_Empty) {
    try {
        getActiveDM().setReplicationTopology(nlohmann::json::array());
    } catch (const std::invalid_argument& e) {
        EXPECT_TRUE(std::string(e.what()).find("Topology is empty") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_FirstChainEmpty) {
    testChainEmpty(nlohmann::json::array({{}}),
                   DurabilityMonitor::ReplicationChainName::First);
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_SecondChainEmpty) {
    // It's valid to not have a second chain, but we shouldn't have an empty
    // second chain
    testChainEmpty(nlohmann::json::array(
                           {{active, replica1}, nlohmann::json::array()}),
                   DurabilityMonitor::ReplicationChainName::Second);
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_TooManyChains) {
    try {
        getActiveDM().setReplicationTopology(nlohmann::json::array(
                {{active, replica1}, {active, replica2}, {active, replica3}}));
    } catch (const std::invalid_argument& e) {
        EXPECT_TRUE(std::string(e.what()).find("Too many chains specified") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_FirstChainUndefinedActive) {
    testChainUndefinedActive(nlohmann::json::array({{nullptr}}),
                             DurabilityMonitor::ReplicationChainName::First);
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_SecondChainUndefinedActive) {
    testChainUndefinedActive(
            nlohmann::json::array({{active, replica1}, {nullptr}}),
            DurabilityMonitor::ReplicationChainName::Second);
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_TooManyNodesInFirstChain) {
    testChainTooManyNodes(nlohmann::json::array({{"active",
                                                  "replica1",
                                                  "replica2",
                                                  "replica3",
                                                  "replica4"}}),
                          DurabilityMonitor::ReplicationChainName::First);
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_TooManyNodesInSecondChain) {
    testChainTooManyNodes(
            nlohmann::json::array(
                    {{active, replica1},
                     {active, replica1, replica2, replica3, "replica4"}}),
            DurabilityMonitor::ReplicationChainName::Second);
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_NodeDuplicateInFirstChain) {
    testChainDuplicateNode(nlohmann::json::array({{"node1", "node1"}}));
}

TEST_P(ActiveDurabilityMonitorTest, SetTopology_NodeDuplicateInSecondChain) {
    testChainDuplicateNode(
            nlohmann::json::array({{active, replica1}, {active, active}}));
}

TEST_P(ActiveDurabilityMonitorTest, SeqnoAckReceived_MultipleReplica) {
    auto& adm = getActiveDM();
    ASSERT_NO_THROW(adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1, replica2, replica3}})));
    ASSERT_EQ(4, adm.getFirstChainSize());

    testSeqnoAckMultipleReplicas({replica2, replica3}, {replica1});
}

TEST_P(ActiveDurabilityMonitorTest, SeqnoAckReceived_MultipleReplicaTwoChains) {
    auto& adm = getActiveDM();
    ASSERT_NO_THROW(adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1, replica2, replica3},
                                   {active, replica1, replica2, replica4}})));
    ASSERT_EQ(4, adm.getFirstChainSize());
    ASSERT_EQ(4, adm.getSecondChainSize());

    testSeqnoAckMultipleReplicas({replica2, replica3, replica4}, {replica1});
}

TEST_P(ActiveDurabilityMonitorTest,
       SeqnoAckReceived_MultipleReplicaTwoChainsDisjoint) {
    auto& adm = getActiveDM();
    ASSERT_NO_THROW(adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1, replica2, replica3},
                                   {active, replica4, replica5, replica6}})));
    ASSERT_EQ(4, adm.getFirstChainSize());
    ASSERT_EQ(4, adm.getSecondChainSize());

    testSeqnoAckMultipleReplicas({replica2, replica3, replica4, replica5},
                                 {replica1, replica6});
}

TEST_P(ActiveDurabilityMonitorTest, NeverExpireIfTimeoutNotSet) {
    auto& adm = getActiveDM();
    ASSERT_NO_THROW(adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}})));
    ASSERT_EQ(2, adm.getFirstChainSize());

    using namespace cb::durability;
    addSyncWrite(1 /*seqno*/,
                 Requirements(Level::Majority, Timeout::Infinity()));
    EXPECT_EQ(1, monitor->getNumTracked());

    // Never expire, neither after 1 year !
    const auto year = std::chrono::hours(24 * 365);
    adm.processTimeout(std::chrono::steady_clock::now() + year);

    // Not expired, still tracked
    EXPECT_EQ(1, monitor->getNumTracked());
}

TEST_P(ActiveDurabilityMonitorTest, ProcessTimeout) {
    auto& adm = getActiveDM();
    ASSERT_NO_THROW(adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}})));
    ASSERT_EQ(2, adm.getFirstChainSize());

    /*
     * 1 SyncWrite
     */

    const auto level = cb::durability::Level::Majority;

    addSyncWrite(1 /*seqno*/, {level, 1 /*timeout*/});
    EXPECT_EQ(1, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 1 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        assertNodeTracking(replica1, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    adm.processTimeout(std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(1000));

    EXPECT_EQ(0, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        // @todo: Here and below, the next check means that after abort the
        //     tracking for a node (the Active in this case) is like:
        //     - Position::it -> points to Container::end()
        //     - Position::lastWriteSeqno -> stays set to the seqno of the
        //         aborted Prepare
        // Is that what we want?
        assertNodeTracking(active, 1 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        assertNodeTracking(replica1, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    /*
     * Multiple SyncWrites, ordered by timeout
     */

    addSyncWrite(101 /*seqno*/, {level, 1 /*timeout*/});
    addSyncWrite(102 /*seqno*/, {level, 10});
    addSyncWrite(103 /*seqno*/, {level, 20});
    ASSERT_EQ(3, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 103 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        assertNodeTracking(replica1, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    adm.processTimeout(std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(10000));

    EXPECT_EQ(0, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 103 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        assertNodeTracking(replica1, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    /*
     * Multiple SyncWrites, not ordered by timeout
     */

    addSyncWrite(201 /*seqno*/, {level, 20 /*timeout*/});
    addSyncWrite(202 /*seqno*/, {level, 1});
    addSyncWrite(203 /*seqno*/, {level, 50000});
    ASSERT_EQ(3, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 203 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        assertNodeTracking(replica1, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    adm.processTimeout(std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(10000));

    EXPECT_EQ(1, monitor->getNumTracked());
    const auto tracked = adm.getTrackedSeqnos();
    EXPECT_TRUE(tracked.find(201) == tracked.end());
    EXPECT_TRUE(tracked.find(202) == tracked.end());
    EXPECT_TRUE(tracked.find(203) != tracked.end());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 203 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        assertNodeTracking(replica1, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }

    adm.processTimeout(std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(100000));

    EXPECT_EQ(0, monitor->getNumTracked());
    {
        SCOPED_TRACE("");
        assertNodeTracking(active, 203 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
        assertNodeTracking(replica1, 0 /*lastWriteSeqno*/, 0 /*lastAckSeqno*/);
    }
}

TEST_P(ActiveDurabilityMonitorTest, MajorityAndPersistOnMaster) {
    testSeqnoAckMajorityAndPersistOnMaster({replica1});
}

TEST_P(ActiveDurabilityMonitorTest, MajorityAndPersistOnMasterTwoChains) {
    auto& adm = getActiveDM();
    adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1}, {active, replica2}}));
    ASSERT_EQ(2, adm.getFirstChainSize());
    ASSERT_EQ(2, adm.getSecondChainSize());

    testSeqnoAckMajorityAndPersistOnMaster({replica1, replica2});
}

TEST_P(ActiveDurabilityMonitorTest, PersistToMajority_EnsurePersistAtActive) {
    auto& adm = getActiveDM();
    ASSERT_NO_THROW(adm.setReplicationTopology(
            nlohmann::json::array({{active, replica1, replica2}})));
    ASSERT_EQ(3, adm.getFirstChainSize());

    testSeqnoAckPersistToMajority({replica1, replica2});
}

TEST_P(ActiveDurabilityMonitorTest,
       PersistToMajority_EnsurePersistAtActiveTwoChains) {
    auto& adm = getActiveDM();
    ASSERT_NO_THROW(adm.setReplicationTopology(nlohmann::json::array(
            {{active, replica1, replica2}, {active, replica3, replica4}})));
    ASSERT_EQ(3, adm.getFirstChainSize());
    ASSERT_EQ(3, adm.getSecondChainSize());

    testSeqnoAckPersistToMajority({replica1, replica2, replica3, replica4});
}

/*
 * The following tests check that the DurabilityMonitor enforces the
 * durability-impossible semantic. I.e., DM enforces durability-impossible when
 * the caller tries to enqueue a SyncWrite but the topology state prevents
 * Requirements from being satisfied.
 *
 * Note: Not covered in the following as already covered in dedicated tests:
 * - FirstChain cannot be empty
 * - Active node in chain cannot be undefined
 *
 * @todo: Extend durability-impossible tests to SecondChain when supported
 */

TEST_P(ActiveDurabilityMonitorTest, DurabilityImpossible_NoReplica) {
    auto item = makePendingItem(
            makeStoredDocKey("key"),
            "value",
            cb::durability::Requirements(
                    cb::durability::Level::PersistToMajority, {}));
    item->setBySeqno(1);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible({{"active"}},
                                           item,
                                           1 /*expectedFirstChainSize*/,
                                           1 /*expectedFirstChainMajority*/));
    }
}

TEST_P(ActiveDurabilityMonitorTest, DurabilityImpossible_TwoChains_NoReplica) {
    auto item = makePendingItem(
            makeStoredDocKey("key"),
            "value",
            cb::durability::Requirements(
                    cb::durability::Level::PersistToMajority, {}));
    item->setBySeqno(1);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible({{active}, {replica1}},
                                           item,
                                           1 /*expectedFirstChainSize*/,
                                           1 /*expectedFirstChainMajority*/,
                                           1 /*expectedSecondChainSize*/,
                                           1 /*expectedSecondChainMajority*/));
    }
}

TEST_P(ActiveDurabilityMonitorTest, DurabilityImpossible_1Replica) {
    auto item = makePendingItem(makeStoredDocKey("key"), "value");

    item->setBySeqno(1);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible({{"active", "replica"}},
                                           item,
                                           2 /*expectedFirstChainSize*/,
                                           2 /*expectedFirstChainMajority*/));
    }

    item->setBySeqno(2);
    {
        SCOPED_TRACE("");
        EXPECT_FALSE(testDurabilityPossible({{"active", nullptr}},
                                            item,
                                            1 /*expectedFirstChainSize*/,
                                            2 /*expectedFirstChainMajority*/));
    }
}

TEST_P(ActiveDurabilityMonitorTest, DurabilityImpossible_TwoChains_1Replica) {
    auto item = makePendingItem(makeStoredDocKey("key"), "value");

    item->setBySeqno(1);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(
                testDurabilityPossible({{active, replica1}, {active, replica2}},
                                       item,
                                       2 /*expectedFirstChainSize*/,
                                       2 /*expectedFirstChainMajority*/,
                                       2 /*expectedSecondChainSize*/,
                                       2 /*expectedSecondChainMajority*/));
    }

    item->setBySeqno(2);
    {
        SCOPED_TRACE("");
        EXPECT_FALSE(
                testDurabilityPossible({{active, nullptr}, {active, replica2}},
                                       item,
                                       1 /*expectedFirstChainSize*/,
                                       2 /*expectedFirstChainMajority*/,
                                       2 /*expectedSecondChainSize*/,
                                       2 /*expectedSecondChainMajority*/));
    }

    {
        SCOPED_TRACE("");
        EXPECT_FALSE(
                testDurabilityPossible({{active, replica1}, {active, nullptr}},
                                       item,
                                       2 /*expectedFirstChainSize*/,
                                       2 /*expectedFirstChainMajority*/,
                                       1 /*expectedSecondChainSize*/,
                                       2 /*expectedSecondChainMajority*/));
    }
}

TEST_P(ActiveDurabilityMonitorTest, DurabilityImpossible_2Replicas) {
    auto item = makePendingItem(makeStoredDocKey("key"), "value");

    item->setBySeqno(1);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible({{"active", "replica1", "replica2"}},
                                           item,
                                           3 /*expectedFirstChainSize*/,
                                           2 /*expectedFirstChainMajority*/));
    }

    item->setBySeqno(2);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible({{"active", "replica1", nullptr}},
                                           item,
                                           2 /*expectedFirstChainSize*/,
                                           2 /*expectedFirstChainMajority*/));
    }

    item->setBySeqno(3);
    {
        SCOPED_TRACE("");
        EXPECT_FALSE(testDurabilityPossible({{"active", nullptr, nullptr}},
                                            item,
                                            1 /*expectedFirstChainSize*/,
                                            2 /*expectedFirstChainMajority*/));
    }
}

TEST_P(ActiveDurabilityMonitorTest, DurabilityImpossible_TwoChains_2Replicas) {
    auto item = makePendingItem(makeStoredDocKey("key"), "value");

    item->setBySeqno(1);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible(
                {{active, replica1, replica2}, {active, replica3, replica4}},
                item,
                3 /*expectedFirstChainSize*/,
                2 /*expectedFirstChainMajority*/,
                3 /*expectedSecondChainSize*/,
                2 /*expectedSecondChainMajority*/));
    }

    item->setBySeqno(2);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible(
                {{active, replica1, nullptr}, {active, replica3, replica4}},
                item,
                2 /*expectedFirstChainSize*/,
                2 /*expectedFirstChainMajority*/,
                3 /*expectedSecondChainSize*/,
                2 /*expectedSecondChainMajority*/));
    }

    {
        SCOPED_TRACE("");
        EXPECT_FALSE(testDurabilityPossible(
                {{active, nullptr, nullptr}, {active, replica3, replica4}},
                item,
                1 /*expectedFirstChainSize*/,
                2 /*expectedFirstChainMajority*/,
                3 /*expectedSecondChainSize*/,
                2 /*expectedSecondChainMajority*/));
    }

    item->setBySeqno(4);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible(
                {{active, replica1, replica2}, {active, replica3, nullptr}},
                item,
                3 /*expectedFirstChainSize*/,
                2 /*expectedFirstChainMajority*/,
                2 /*expectedSecondChainSize*/,
                2 /*expectedSecondChainMajority*/));
    }

    item->setBySeqno(5);
    {
        SCOPED_TRACE("");
        EXPECT_FALSE(testDurabilityPossible(
                {{active, replica1, replica2}, {active, nullptr, nullptr}},
                item,
                3 /*expectedFirstChainSize*/,
                2 /*expectedFirstChainMajority*/,
                1 /*expectedSecondChainSize*/,
                2 /*expectedSecondChainMajority*/));
    }
}

TEST_P(ActiveDurabilityMonitorTest, DurabilityImpossible_3Replicas) {
    // Note: In this test, playing with Undefined nodes in different and
    // non-consecutive positions.
    // Not sure if ns_server can set something like that (e.g. {A, u, R2, u}),
    // but better that we cover the most general case.
    auto item = makePendingItem(makeStoredDocKey("key"), "value");

    item->setBySeqno(1);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible(
                {{"active", "replica1", "replica2", "replica3"}},
                item,
                4 /*expectedFirstChainSize*/,
                3 /*expectedFirstChainMajority*/));
    }

    item->setBySeqno(2);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(testDurabilityPossible(
                {{"active", "replica1", nullptr, "replica3"}},
                item,
                3 /*expectedFirstChainSize*/,
                3 /*expectedFirstChainMajority*/));
    }

    item->setBySeqno(3);
    {
        SCOPED_TRACE("");
        EXPECT_FALSE(testDurabilityPossible(
                {{"active", nullptr, nullptr, "replica3"}},
                item,
                2 /*expectedFirstChainSize*/,
                3 /*expectedFirstChainMajority*/));
    }

    // No need to increase bySeqno here, as at the previous call s:3 must be
    // rejected (durability is impossible when chainSize<chainMajority).
    // Also, if for some reason the durability-impossible logic is broken
    // (i.e., s:3 is successfully queued at the previous step rather than being
    // rejected) then the following step fails as well, as trying to
    // re-queueing s:3 will break seqno-invariant.
    {
        SCOPED_TRACE("");
        EXPECT_FALSE(
                testDurabilityPossible({{"active", nullptr, nullptr, nullptr}},
                                       item,
                                       1 /*expectedFirstChainSize*/,
                                       3 /*expectedFirstChainMajority*/));
    }
}

TEST_P(ActiveDurabilityMonitorTest, DurabilityImpossible_TwoChains_3Replicas) {
    // Note: In this test, playing with Undefined nodes in different and
    // non-consecutive positions.
    // Not sure if ns_server can set something like that (e.g. {A, u, R2, u}),
    // but better that we cover the most general case.
    auto item = makePendingItem(makeStoredDocKey("key"), "value");

    item->setBySeqno(1);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(
                testDurabilityPossible({{active, replica1, replica2, replica3},
                                        {active, replica4, replica5, replica6}},
                                       item,
                                       4 /*expectedFirstChainSize*/,
                                       3 /*expectedFirstChainMajority*/,
                                       4 /*expectedSecondChainSize*/,
                                       3 /*expectedSecondChainMajority*/));
    }

    item->setBySeqno(2);
    {
        SCOPED_TRACE("");
        EXPECT_TRUE(
                testDurabilityPossible({{active, replica1, nullptr, replica3},
                                        {active, replica4, replica5, replica6}},
                                       item,
                                       3 /*expectedFirstChainSize*/,
                                       3 /*expectedFirstChainMajority*/,
                                       4 /*expectedSecondChainSize*/,
                                       3 /*expectedSecondChainMajority*/));
    }

    item->setBySeqno(3);
    {
        SCOPED_TRACE("");
        EXPECT_FALSE(
                testDurabilityPossible({{active, nullptr, nullptr, replica3},
                                        {active, replica3, replica5, replica6}},
                                       item,
                                       2 /*expectedFirstChainSize*/,
                                       3 /*expectedFirstChainMajority*/,
                                       4 /*expectedSecondChainSize*/,
                                       3 /*expectedSecondChainMajority*/));
    }

    {
        SCOPED_TRACE("");
        EXPECT_FALSE(
                testDurabilityPossible({{active, nullptr, nullptr, nullptr},
                                        {active, replica4, replica5, replica6}},
                                       item,
                                       1 /*expectedFirstChainSize*/,
                                       3 /*expectedFirstChainMajority*/,
                                       4 /*expectedSecondChainSize*/,
                                       3 /*expectedSecondChainMajority*/));
    }

    {
        SCOPED_TRACE("");
        EXPECT_TRUE(
                testDurabilityPossible({{active, replica1, replica2, replica3},
                                        {active, replica4, nullptr, replica6}},
                                       item,
                                       4 /*expectedFirstChainSize*/,
                                       3 /*expectedFirstChainMajority*/,
                                       3 /*expectedSecondChainSize*/,
                                       3 /*expectedSecondChainMajority*/));
    }

    item->setBySeqno(4);
    {
        SCOPED_TRACE("");
        EXPECT_FALSE(
                testDurabilityPossible({{active, replica1, replica2, replica3},
                                        {active, nullptr, nullptr, replica6}},
                                       item,
                                       4 /*expectedFirstChainSize*/,
                                       3 /*expectedFirstChainMajority*/,
                                       2 /*expectedSecondChainSize*/,
                                       3 /*expectedSecondChainMajority*/));
    }

    {
        SCOPED_TRACE("");
        EXPECT_FALSE(
                testDurabilityPossible({{active, replica1, replica2, replica3},
                                        {active, nullptr, nullptr, nullptr}},
                                       item,
                                       4 /*expectedFirstChainSize*/,
                                       3 /*expectedFirstChainMajority*/,
                                       1 /*expectedSecondChainSize*/,
                                       3 /*expectedSecondChainMajority*/));
    }
}

void PassiveDurabilityMonitorTest::SetUp() {
    STParameterizedBucketTest::SetUp();
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_replica);
    vb = store->getVBuckets().getBucket(vbid).get();
    ASSERT_TRUE(vb);
    monitor = &VBucketTestIntrospector::public_getPassiveDM(*vb);
    ASSERT_TRUE(monitor);
}

void PassiveDurabilityMonitorTest::TearDown() {
    STParameterizedBucketTest::TearDown();
}

TEST_P(PassiveDurabilityMonitorTest, AddSyncWrite) {
    ASSERT_EQ(0, monitor->getNumTracked());
    using namespace cb::durability;
    addSyncWrite(1 /*seqno*/,
                 Requirements{Level::Majority, Timeout::Infinity()});
    EXPECT_EQ(1, monitor->getNumTracked());
}

/// Check that attempting to add a SyncWrite to PDM with default timeout
/// fails (active should have set an explicit timeout).
TEST_P(PassiveDurabilityMonitorTest, AddSyncWriteDefaultTimeoutInvalid) {
    using namespace cb::durability;
    auto item = makePendingItem(makeStoredDocKey("key"),
                                "value",
                                Requirements{Level::Majority, Timeout()});
    EXPECT_THROW(VBucketTestIntrospector::public_getPassiveDM(*vb).addSyncWrite(
                         item),
                 std::invalid_argument);
}

void DurabilityMonitorTest::addSyncWriteAndCheckHPS(
        const std::vector<int64_t>& seqnos,
        cb::durability::Level level,
        int64_t expectedHPS) {
    const size_t expectedNumTracked = monitor->getNumTracked() + seqnos.size();
    // Use non-default timeout as this function is used by both active and
    // passive DM tests (and passive DM doesn't accept Timeout::BucketDefault
    // as active should have already set it to an explicit value).
    cb::durability::Timeout timeout(10);
    addSyncWrites(seqnos, cb::durability::Requirements{level, timeout});
    EXPECT_EQ(expectedNumTracked, monitor->getNumTracked());
    EXPECT_EQ(expectedHPS, monitor->getHighPreparedSeqno());
}

void DurabilityMonitorTest::notifyPersistenceAndCheckHPS(int64_t persistedSeqno,
                                                         int64_t expectedHPS) {
    vb->setPersistenceSeqno(persistedSeqno);
    monitor->notifyLocalPersistence();
    EXPECT_EQ(expectedHPS, monitor->getHighPreparedSeqno());
}

void DurabilityMonitorTest::testHPS_Majority() {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 2, 3} /*seqnos*/,
                            cb::durability::Level::Majority,
                            3 /*expectedHPS*/);

    notifyPersistenceAndCheckHPS(1000 /*persistedSeqno*/, 3 /*expectedHPS*/);
}

TEST_P(PassiveDurabilityMonitorTest, HPS_Majority) {
    testHPS_Majority();
}

TEST_P(ActiveDurabilityMonitorTest, HPS_Majority) {
    testHPS_Majority();
}

TEST_P(PassiveDurabilityMonitorTest, HPS_MajorityAndPersistOnMaster) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 2, 3} /*seqnos*/,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            3 /*expectedHPS*/);

    notifyPersistenceAndCheckHPS(1000 /*persistedSeqno*/, 3 /*expectedHPS*/);
}

TEST_P(ActiveDurabilityMonitorTest, HPS_MajorityAndPersistOnMaster) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    const std::vector<int64_t> seqnos{1, 2, 3};
    addSyncWriteAndCheckHPS(seqnos,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            0 /*expectedHPS*/);

    for (const auto s : seqnos) {
        notifyPersistenceAndCheckHPS(s /*persistedSeqno*/, s /*expectedHPS*/);
    }
}

void DurabilityMonitorTest::testHPS_PersistToMajority() {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    const std::vector<int64_t> seqnos{1, 2, 3};
    addSyncWriteAndCheckHPS(seqnos,
                            cb::durability::Level::PersistToMajority,
                            0 /*expectHPS*/);

    for (const auto s : seqnos) {
        notifyPersistenceAndCheckHPS(s /*persistedSeqno*/, s /*expectedHPS*/);
    }
}

TEST_P(PassiveDurabilityMonitorTest, HPS_PersistToMajority) {
    testHPS_PersistToMajority();
}

TEST_P(ActiveDurabilityMonitorTest, HPS_PersistToMajority) {
    testHPS_PersistToMajority();
}

TEST_P(PassiveDurabilityMonitorTest,
       HPS_MajorityAndPersistOnMajority_Majority) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 2, 3} /*seqnos*/,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            3 /*expectHPS*/);

    addSyncWriteAndCheckHPS({4, 10, 21} /*seqnos*/,
                            cb::durability::Level::Majority,
                            21 /*expectHPS*/);
}

TEST_P(ActiveDurabilityMonitorTest, HPS_MajorityAndPersistOnMajority_Majority) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 2, 3} /*seqnos*/,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            0 /*expectHPS*/);

    addSyncWriteAndCheckHPS({4, 10, 21} /*seqnos*/,
                            cb::durability::Level::Majority,
                            0 /*expectHPS*/);

    notifyPersistenceAndCheckHPS(3 /*persistedSeqno*/, 21 /*expectedHPS*/);
}

TEST_P(PassiveDurabilityMonitorTest,
       HPS_Majority_MajorityAndPersistOnMajority) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 7, 1000} /*seqnos*/,
                            cb::durability::Level::Majority,
                            1000 /*expectHPS*/);

    addSyncWriteAndCheckHPS({1004, 1010, 2021} /*seqnos*/,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            2021 /*expectHPS*/);
}

TEST_P(ActiveDurabilityMonitorTest, HPS_Majority_MajorityAndPersistOnMajority) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 7, 1000} /*seqnos*/,
                            cb::durability::Level::Majority,
                            1000 /*expectHPS*/);

    const std::vector<int64_t> seqnos{1004, 1010, 2021};
    addSyncWriteAndCheckHPS(seqnos,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            1000 /*expectHPS*/);

    for (const auto s : seqnos) {
        notifyPersistenceAndCheckHPS(s /*persistedSeqno*/, s /*expectedHPS*/);
    }
}

void DurabilityMonitorTest::testHPS_PersistToMajority_Majority() {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 2, 3} /*seqnos*/,
                            cb::durability::Level::PersistToMajority,
                            0 /*expectHPS*/);

    addSyncWriteAndCheckHPS({4, 10, 21} /*seqnos*/,
                            cb::durability::Level::Majority,
                            0 /*expectHPS*/);

    // Check that persisting s:2 moves HPS to 2 and not beyond, as s:3 is
    // Level::PersistToMajority (i.e., a durability-fence)
    notifyPersistenceAndCheckHPS(2 /*persistedSeqno*/, 2 /*expectHPS*/);

    // Now, simulate persistence of s:4. HPS reaches the latest tracked as s:3
    // is the last durability-fence.
    notifyPersistenceAndCheckHPS(4 /*persistedSeqno*/, 21 /*expectHPS*/);
}

TEST_P(PassiveDurabilityMonitorTest, HPS_PersistToMajority_Majority) {
    testHPS_PersistToMajority_Majority();
}

TEST_P(ActiveDurabilityMonitorTest, HPS_PersistToMajority_Majority) {
    testHPS_PersistToMajority_Majority();
}

void DurabilityMonitorTest::testHPS_Majority_PersistToMajority() {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 999, 1001} /*seqnos*/,
                            cb::durability::Level::Majority,
                            1001 /*expectHPS*/);

    addSyncWriteAndCheckHPS({2000, 2010, 2021} /*seqnos*/,
                            cb::durability::Level::PersistToMajority,
                            1001 /*expectHPS*/);

    notifyPersistenceAndCheckHPS(2010 /*persistedSeqno*/, 2010 /*expectHPS*/);

    // Now, simulate persistence of s:4. HPS reaches the latest tracked as s:3
    // is the last durability-fence.
    notifyPersistenceAndCheckHPS(2021 /*persistedSeqno*/, 2021 /*expectHPS*/);
}

TEST_P(PassiveDurabilityMonitorTest, HPS_Majority_PersistToMajority) {
    testHPS_Majority_PersistToMajority();
}

TEST_P(ActiveDurabilityMonitorTest, HPS_Majority_PersistToMajority) {
    testHPS_Majority_PersistToMajority();
}

TEST_P(PassiveDurabilityMonitorTest,
       HPS_PersistToMajority_MajorityAndPersistOnMaster) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 2, 3} /*seqnos*/,
                            cb::durability::Level::PersistToMajority,
                            0 /*expectHPS*/);

    addSyncWriteAndCheckHPS({4, 10, 21} /*seqnos*/,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            0 /*expectHPS*/);

    // Check that persisting s:2 moves HPS to 2 and not beyond, as s:3 is
    // Level::PersistToMajority (i.e., a durability-fence)
    notifyPersistenceAndCheckHPS(2 /*persistedSeqno*/, 2 /*expectHPS*/);

    // Now, simulate persistence of s:4. HPS reaches the latest tracked as s:3
    // is the last durability-fence.
    notifyPersistenceAndCheckHPS(4 /*persistedSeqno*/, 21 /*expectHPS*/);
}

TEST_P(ActiveDurabilityMonitorTest,
       HPS_PersistToMajority_MajorityAndPersistOnMaster) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 2, 3} /*seqnos*/,
                            cb::durability::Level::PersistToMajority,
                            0 /*expectHPS*/);

    addSyncWriteAndCheckHPS({4, 10, 21} /*seqnos*/,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            0 /*expectHPS*/);

    // Check that persisting s:2 moves HPS to 2 and not beyond, as s:3 is
    // Level::PersistToMajority (i.e., a durability-fence)
    notifyPersistenceAndCheckHPS(2 /*persistedSeqno*/, 2 /*expectHPS*/);

    // Check that persisting s:4 moves HPS to 4 and not beyond, as s:4 is
    // Level::MajorityAndPersistOnMaster (i.e., a durability-fence on Active)
    notifyPersistenceAndCheckHPS(4 /*persistedSeqno*/, 4 /*expectHPS*/);
}

TEST_P(PassiveDurabilityMonitorTest,
       HPS_MajorityAndPersistOnMaster_PersistToMajority) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 999, 1001} /*seqnos*/,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            1001 /*expectHPS*/);

    addSyncWriteAndCheckHPS({2000, 2010, 2021} /*seqnos*/,
                            cb::durability::Level::PersistToMajority,
                            1001 /*expectHPS*/);

    notifyPersistenceAndCheckHPS(2010 /*persistedSeqno*/, 2010 /*expectHPS*/);

    // Now, simulate persistence of s:4. HPS reaches the latest tracked as s:3
    // is the last durability-fence.
    notifyPersistenceAndCheckHPS(2021 /*persistedSeqno*/, 2021 /*expectHPS*/);
}

TEST_P(ActiveDurabilityMonitorTest,
       HPS_MajorityAndPersistOnMaster_PersistToMajority) {
    ASSERT_EQ(0, monitor->getNumTracked());
    ASSERT_EQ(0, monitor->getHighPreparedSeqno());

    addSyncWriteAndCheckHPS({1, 999, 1001} /*seqnos*/,
                            cb::durability::Level::MajorityAndPersistOnMaster,
                            0 /*expectHPS*/);

    addSyncWriteAndCheckHPS({2000, 2010, 2021} /*seqnos*/,
                            cb::durability::Level::PersistToMajority,
                            0 /*expectHPS*/);

    notifyPersistenceAndCheckHPS(2021 /*persistedSeqno*/, 2021 /*expectHPS*/);
}

INSTANTIATE_TEST_CASE_P(AllBucketTypes,
                        ActiveDurabilityMonitorTest,
                        STParameterizedBucketTest::persistentConfigValues(),
                        STParameterizedBucketTest::PrintToStringParamName);

INSTANTIATE_TEST_CASE_P(AllBucketTypes,
                        PassiveDurabilityMonitorTest,
                        STParameterizedBucketTest::persistentConfigValues(),
                        STParameterizedBucketTest::PrintToStringParamName);
