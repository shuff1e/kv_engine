/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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
#include "testapp_client_test.h"
#include <cctype>
#include <limits>
#include <thread>

class ArithmeticTest : public TestappXattrClientTest {};

/**
 * Test fixture for arithmetic tests which always need XATTR support on.
 */
class ArithmeticXattrOnTest : public TestappXattrClientTest {};

INSTANTIATE_TEST_CASE_P(
        TransportProtocols,
        ArithmeticTest,
        ::testing::Combine(::testing::Values(TransportProtocols::McbpPlain,
                                             TransportProtocols::McbpSsl),
                           ::testing::Values(XattrSupport::Yes,
                                             XattrSupport::No),
                           ::testing::Values(ClientJSONSupport::Yes,
                                             ClientJSONSupport::No),
                           ::testing::Values(ClientSnappySupport::Yes,
                                             ClientSnappySupport::No)),
        PrintToStringCombinedName());

// Tests which always need XATTR support (hence only instantiated with
// XattrSupport::Yes).
INSTANTIATE_TEST_CASE_P(
        TransportProtocols,
        ArithmeticXattrOnTest,
        ::testing::Combine(::testing::Values(TransportProtocols::McbpPlain,
                                             TransportProtocols::McbpSsl),
                           ::testing::Values(XattrSupport::Yes),
                           ::testing::Values(ClientJSONSupport::Yes,
                                             ClientJSONSupport::No),
                           ::testing::Values(ClientSnappySupport::Yes,
                                             ClientSnappySupport::No)),
        PrintToStringCombinedName());

TEST_P(ArithmeticTest, TestArithmeticNoCreateOnNotFound) {
    auto& connection = getConnection();

    try {
        connection.increment(name, 1, 0, 0xffffffff);
        FAIL() << "Document should not be created";
    } catch (const ConnectionError& error) {
        EXPECT_TRUE(error.isNotFound()) << error.getReason();
    }

    try {
        connection.decrement(name, 1, 0, 0xffffffff);
        FAIL() << "Document should not be created";
    } catch (const ConnectionError& error) {
        EXPECT_TRUE(error.isNotFound()) << error.getReason();
    }
}

TEST_P(ArithmeticTest, TestIncrementCreateOnNotFound) {
    auto& connection = getConnection();
    EXPECT_EQ(0, connection.increment(name+"_incr", 1, 0));
    EXPECT_EQ(0, connection.increment(name+"_decr", 1, 0));
}

static void incr_decr_loop(MemcachedConnection& connection,
                           const std::string& key,
                           int delta) {

    uint64_t expected = 0;

    for (int ii = 0; ii < 101; ++ii) {
        EXPECT_EQ(expected, connection.arithmetic(key, delta));
        expected += delta;
    }
    // (remove the last delta, it was not added to the one on the server
    expected -= delta;

    for (int ii = 0; ii < 100; ++ii) {
        expected -= delta;
        EXPECT_EQ(expected, connection.arithmetic(key, delta * -1));
    }

    // Verify that we ended back at 0 by trying to add 0 ;-)
    EXPECT_EQ(0, connection.arithmetic(key, 0));
}

TEST_P(ArithmeticTest, TestBasicArithmetic_1) {
    incr_decr_loop(getConnection(), name, 1);
}

TEST_P(ArithmeticTest, TestBasicArithmetic_33) {
    incr_decr_loop(getConnection(), name, 33);
}

TEST_P(ArithmeticTest, TestDecrementDontWrap) {
    auto& connection = getConnection();
    for (int ii = 0; ii < 10; ++ii) {
        EXPECT_EQ(0, connection.decrement(name, 1));
    }
}

TEST_P(ArithmeticTest, TestIncrementDoesWrap) {
    auto& connection = getConnection();
    uint64_t initial = std::numeric_limits<uint64_t>::max();

    // Create the initial value so that we know where we should start off ;)
    EXPECT_EQ(0, connection.increment(name, 1));

    EXPECT_EQ(initial, connection.increment(name, initial));
    EXPECT_EQ(0, connection.increment(name, 1));
    EXPECT_EQ(initial, connection.increment(name, initial));
    EXPECT_EQ(0, connection.increment(name, 1));
}

TEST_P(ArithmeticTest, TestConcurrentAccess) {
    auto& conn = getConnection();
    auto conn1 = conn.clone();
    auto conn2 = conn.clone();
    const int iterationCount = 100;
    const int incrDelta = 7;
    const int decrDelta = -3;

    // Create the starting point
    uint64_t expected = std::numeric_limits<uint32_t>::max();
    ASSERT_EQ(0, conn.increment(name, 0));
    ASSERT_EQ(expected, conn.increment(name, expected));

    std::string doc = name;

    std::thread t1 {
        [&conn1, &doc, &iterationCount, incrDelta]() {
            conn1->arithmetic(doc + "_t1", 0, 0);

            // wait for the other thread to start
            bool running = false;
            while (!running) {
                try {
                    conn1->arithmetic(doc + "_t2", 1, 0, 0xffffffff);
                    running = true;
                } catch (const ConnectionError& error) {
                    ASSERT_TRUE(error.isNotFound());
                }
            }

            for (int ii = 0; ii < iterationCount; ++ii) {
                conn1->arithmetic(doc, incrDelta);
            }
        }
    };

    std::thread t2 {
        [&conn2, &doc, &iterationCount, &decrDelta]() {
            conn2->arithmetic(doc + "_t2", 0, 0);
            bool running = false;
            while (!running) {
                try {
                    conn2->arithmetic(doc + "_t1", 1, 0, 0xffffffff);
                    running = true;
                } catch (const ConnectionError& error) {
                    ASSERT_TRUE(error.isNotFound());
                }
            }

            for (int ii = 0; ii < iterationCount; ++ii) {
                conn2->arithmetic(doc, decrDelta);
            }
        }
    };

    // Wait for them to complete
    t1.join();
    t2.join();

    expected += (iterationCount * incrDelta) + (iterationCount * decrDelta);
    EXPECT_EQ(expected, conn.increment(name, 0));
}

TEST_P(ArithmeticTest, TestMutationInfo) {
    auto& conn = getConnection();
    MutationInfo info;
    ASSERT_EQ(0, conn.increment(name, 0, 0, 0, &info));

    // Not all the backends supports the vbucket seqno and uuid..
    // The cas should be filled out, so we should be able to do a CAS replace
    Document doc;
    doc.info.cas = info.cas;
    doc.info.flags = 0xcaffee;
    doc.info.id = name;
    doc.value = memcached_cfg.dump();

    conn.mutate(doc, Vbid(0), MutationType::Replace);
}

TEST_P(ArithmeticTest, TestIllegalDatatype) {
    auto& conn = getConnection();

    Document doc;
    doc.info.cas = mcbp::cas::Wildcard;
    doc.info.flags = 0xcaffee;
    doc.info.id = name;
    doc.value = memcached_cfg.dump();

    ASSERT_NO_THROW(conn.mutate(doc, Vbid(0), MutationType::Add));

    try {
        conn.increment(name, 0);
    } catch (const ConnectionError& error) {
        EXPECT_TRUE(error.isDeltaBadval()) << error.getReason();
    }
}

/*
 * Unit test to test the fix for MB-33813
 * This test checks that we do not return NOT_STORED back to the client
 * if bucket_store returns it to ArithmeticCommandContext::storeNewItem().
 * Instead ArithmeticCommandContext::storeNewItem() should reset the
 * ArithmeticCommandContext state machine and try again.
 *
 * To check that ENGINE_NOT_STORED/NOT_STORED is not returned we use
 * eWouldBlockEngine to return ENGINE_NOT_STORED on the 3rd engine request using
 * the binary sequence 0b100.
 *
 * This works as the process we expect to see by the ArithmeticCommandContext
 * state machine is as follows:
 *  start: GetItem
 *      calls bucket_get() should return ENGINE_SUCCESS
 *  -> CreateNewItem
 *      calls bucket_allocate_ex should return ENGINE_SUCCESS
 *  -> StoreNewItem
 *      calls bucket_store return ENGINE_NOT_STORED
 *      ENGINE_NOT_STORED returned so reset and try again.
 *  -> Reset
 *  -> GetItem
 *  .... Do the work to increment the value again
 *  -> Done
 */
TEST_P(ArithmeticTest, MB33813) {
    auto& connection = getConnection();
    std::string key(name + "_inc");

    // Make the 3rd request send to the engine return ENGINE_NOT_STORED
    // In this case this will be the store that happens as a result of
    // a call to ArithmeticCommandContext::storeNewItem()
    // We also set all the higher bits after the 6th. So that if the sequence
    // of engine accesses change this test will fail.
    connection.configureEwouldBlockEngine(
            EWBEngineMode::Sequence,
            ENGINE_NOT_STORED,
            /* 0b11111111111111111111111111000100 */ 0xffffffc4);

    EXPECT_EQ(1, connection.increment(key, 0, 1));

    connection.disableEwouldBlockEngine();

    Vbid vb(0);
    Document doc = connection.get(key, vb);
    EXPECT_EQ("1", doc.value);

    EXPECT_EQ(2, connection.increment(key, 1));

    doc = connection.get(key, vb);
    EXPECT_EQ("2", doc.value);

    // Sanity check do the same thing but with a decrement
    key = name + "_dec";
    // Make the 3rd request send to the engine return ENGINE_NOT_STORED
    // In this case this will be the store that happens as a result of
    // a call to ArithmeticCommandContext::storeNewItem()
    // We also set all the higher bits after the 6th. So that if the sequence
    // of engine accesses change this test will fail.
    connection.configureEwouldBlockEngine(
            EWBEngineMode::Sequence,
            ENGINE_NOT_STORED,
            /* 0b11111111111111111111111111000100 */ 0xffffffc4);

    EXPECT_EQ(2, connection.decrement(key, 0, 2));

    connection.disableEwouldBlockEngine();

    doc = connection.get(key, vb);
    EXPECT_EQ("2", doc.value);

    EXPECT_EQ(1, connection.decrement(key, 1));

    doc = connection.get(key, vb);
    EXPECT_EQ("1", doc.value);
}

static void test_stored_doc(MemcachedConnection& conn,
                            const std::string& key,
                            const std::string& content,
                            bool badval) {

    Document doc;
    doc.info.cas = mcbp::cas::Wildcard;
    doc.info.flags = 0xcaffee;
    doc.info.id = key;
    doc.value = content;

    ASSERT_NO_THROW(conn.mutate(doc, Vbid(0), MutationType::Set));
    if (badval) {
        try {
            conn.increment(key, 1);
            FAIL() << "Did not catch: " << content;
        } catch (const ConnectionError& error) {
            EXPECT_TRUE(error.isDeltaBadval()) << error.getReason();
        }
    } else {
        EXPECT_EQ(1, conn.increment(key, 1));
    }
}

TEST_P(ArithmeticTest, TestOperateOnStoredDocument) {
#ifdef THREAD_SANITIZER
    // This test takes ~20 secs under Thread sanitizer, and it is
    // mostly here in order to validate that we correctly detect if
    // we can perform incr/decr on a document stored in the cache
    // (depending on the content being a legal value or not)
    // To speed up CV, just run the test for a single parameterization.
    if (GetParam() != std::make_tuple(TransportProtocols::McbpPlain,
                                      XattrSupport::Yes,
                                      ClientJSONSupport::Yes,
                                      ClientSnappySupport::Yes)) {
        return;
    }
#endif
    auto& conn = getConnection();

    // It is "allowed" for the value to be padded with whitespace
    // before and after the numeric value.
    // but no other characters should be present!
    for (unsigned int ii = 0; ii < 256; ++ii) {
        if (std::isspace(ii)) {
            std::string content;
            content = std::string{"0"} + char(ii);
            test_stored_doc(conn, name, content, false);
            content = char(ii) + std::string{"0"};
            test_stored_doc(conn, name, content, false);
        } else if (!std::isdigit(ii)) {
            std::string content;
            if (ii != 0) {
                content = std::string{"0"} + char(ii);
                test_stored_doc(conn, name, content, true);
            }

            if (ii != '-' && ii != '+') {
                content = char(ii) + std::string{"0"};
                test_stored_doc(conn, name, content, true);
            }
        }
    }
}

TEST_P(ArithmeticXattrOnTest, TestDocWithXattr) {
    auto& conn = getConnection();
    EXPECT_EQ(0, conn.increment(name, 1));

    // Add an xattr
    {
        BinprotSubdocCommand cmd;
        cmd.setOp(cb::mcbp::ClientOpcode::SubdocDictAdd);
        cmd.setKey(name);
        cmd.setPath("meta.author");
        cmd.setValue("\"Trond Norbye\"");
        cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH | SUBDOC_FLAG_MKDIR_P);
        conn.sendCommand(cmd);

        BinprotResponse resp;
        conn.recvResponse(resp);
        ASSERT_TRUE(resp.isSuccess()) << to_string(resp.getStatus());
    }

    // Perform the normal operation
    EXPECT_EQ(1, conn.increment(name, 1));

    // The xattr should have been preserved!
    {
        BinprotSubdocCommand cmd;
        cmd.setOp(cb::mcbp::ClientOpcode::SubdocGet);
        cmd.setKey(name);
        cmd.setPath("meta.author");
        cmd.addPathFlags(SUBDOC_FLAG_XATTR_PATH);
        conn.sendCommand(cmd);

        BinprotSubdocResponse resp;
        conn.recvResponse(resp);
        ASSERT_TRUE(resp.isSuccess()) << to_string(resp.getStatus());
        EXPECT_EQ("\"Trond Norbye\"", resp.getValue());
    }
}

TEST_P(ArithmeticXattrOnTest, MB25402) {
    // Increment and decrement should not update the expiry time on existing
    // documents
    auto& conn = getConnection();

    // Start by creating the counter without expiry time
    conn.increment(name, 1, 0, 0, nullptr);
    // increment the counter (which should already exists causing the expiry
    // time to be ignored)
    conn.increment(name, 1, 0, 3600, nullptr);

    // Verify that the expiry time is still
    BinprotSubdocMultiLookupCommand cmd;
    cmd.setKey(name);
    cmd.addGet("$document", SUBDOC_FLAG_XATTR_PATH);
    conn.sendCommand(cmd);

    BinprotSubdocMultiLookupResponse multiResp;
    conn.recvResponse(multiResp);

    auto& results = multiResp.getResults();

    EXPECT_EQ(cb::mcbp::Status::Success, multiResp.getStatus());
    EXPECT_EQ(cb::mcbp::Status::Success, results[0].status);

    // Ensure that we found all we expected and they're of the correct type:
    auto meta = nlohmann::json::parse(results[0].value);
    EXPECT_EQ(0, meta["exptime"].get<int>());
}
