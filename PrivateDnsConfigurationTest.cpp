/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <android-base/properties.h>

#include "Experiments.h"
#include "PrivateDnsConfiguration.h"
#include "tests/dns_responder/dns_responder.h"
#include "tests/dns_responder/dns_tls_frontend.h"
#include "tests/resolv_test_utils.h"

#include <android-base/logging.h>

namespace android::net {

using namespace std::chrono_literals;

const std::string kAvoidBadPrivateDnsFlag(
        "persist.device_config.netd_native.avoid_bad_private_dns");
const std::string kMinPrivateDnsLatencyThresholdMsFlag(
        "persist.device_config.netd_native.min_private_dns_latency_threshold_ms");
const std::string kMaxPrivateDnsLatencyThresholdMsFlag(
        "persist.device_config.netd_native.max_private_dns_latency_threshold_ms");

namespace {

class ScopedSystemProperty {
  public:
    ScopedSystemProperty(const std::string& key, const std::string& value) : mStoredKey(key) {
        mStoredValue = android::base::GetProperty(key, "");
        android::base::SetProperty(key, value);
    }
    ~ScopedSystemProperty() { android::base::SetProperty(mStoredKey, mStoredValue); }

  private:
    std::string mStoredKey;
    std::string mStoredValue;
};

}  // namespace

class PrivateDnsConfigurationTest : public ::testing::Test {
  public:
    using ServerIdentity = PrivateDnsConfiguration::ServerIdentity;

    static void SetUpTestSuite() {
        // stopServer() will be called in their destructor.
        ASSERT_TRUE(tls1.startServer());
        ASSERT_TRUE(tls2.startServer());
        ASSERT_TRUE(backend.startServer());
    }

    void SetUp() {
        mPdc.setObserver(&mObserver);
        mPdc.mBackoffBuilder.withInitialRetransmissionTime(std::chrono::seconds(1))
                .withMaximumRetransmissionTime(std::chrono::seconds(1));

        // The default and sole action when the observer is notified of onValidationStateUpdate.
        // Don't override the action. In other words, don't use WillOnce() or WillRepeatedly()
        // when mObserver.onValidationStateUpdate is expected to be called, like:
        //
        //   EXPECT_CALL(mObserver, onValidationStateUpdate).WillOnce(Return());
        //
        // This is to ensure that tests can monitor how many validation threads are running. Tests
        // must wait until every validation thread finishes.
        ON_CALL(mObserver, onValidationStateUpdate)
                .WillByDefault([&](const std::string& server, Validation validation, uint32_t) {
                    if (validation == Validation::in_process) {
                        std::lock_guard guard(mObserver.lock);
                        auto it = mObserver.serverStateMap.find(server);
                        if (it == mObserver.serverStateMap.end() ||
                            it->second != Validation::in_process) {
                            // Increment runningThreads only when receive the first in_process
                            // notification. The rest of the continuous in_process notifications
                            // are due to probe retry which runs on the same thread.
                            // TODO: consider adding onValidationThreadStart() and
                            // onValidationThreadEnd() callbacks.
                            mObserver.runningThreads++;
                        }
                    } else if (validation == Validation::success ||
                               validation == Validation::fail) {
                        mObserver.runningThreads--;
                    }
                    std::lock_guard guard(mObserver.lock);
                    mObserver.serverStateMap[server] = validation;
                });

        forceExperimentsInstanceUpdate();
    }

  protected:
    class MockObserver : public PrivateDnsValidationObserver {
      public:
        MOCK_METHOD(void, onValidationStateUpdate,
                    (const std::string& serverIp, Validation validation, uint32_t netId),
                    (override));

        std::map<std::string, Validation> getServerStateMap() const {
            std::lock_guard guard(lock);
            return serverStateMap;
        }

        void removeFromServerStateMap(const std::string& server) {
            std::lock_guard guard(lock);
            if (const auto it = serverStateMap.find(server); it != serverStateMap.end())
                serverStateMap.erase(it);
        }

        // The current number of validation threads running.
        std::atomic<int> runningThreads = 0;

        mutable std::mutex lock;
        std::map<std::string, Validation> serverStateMap GUARDED_BY(lock);
    };

    void expectPrivateDnsStatus(PrivateDnsMode mode) {
        // Use PollForCondition because mObserver is notified asynchronously.
        EXPECT_TRUE(PollForCondition([&]() { return checkPrivateDnsStatus(mode); }));
    }

    bool checkPrivateDnsStatus(PrivateDnsMode mode) {
        const PrivateDnsStatus status = mPdc.getStatus(kNetId);
        if (status.mode != mode) return false;

        std::map<std::string, Validation> serverStateMap;
        for (const auto& [server, validation] : status.serversMap) {
            serverStateMap[ToString(&server.ss)] = validation;
        }
        return (serverStateMap == mObserver.getServerStateMap());
    }

    bool hasPrivateDnsServer(const ServerIdentity& identity, unsigned netId) {
        return mPdc.getPrivateDns(identity, netId).ok();
    }

    void forceExperimentsInstanceUpdate() { Experiments::getInstance()->update(); }

    static constexpr uint32_t kNetId = 30;
    static constexpr uint32_t kMark = 30;
    static constexpr char kBackend[] = "127.0.2.1";
    static constexpr char kServer1[] = "127.0.2.2";
    static constexpr char kServer2[] = "127.0.2.3";
    static constexpr int kOpportunisticModeMaxAttempts =
            PrivateDnsConfiguration::kOpportunisticModeMaxAttempts;

    MockObserver mObserver;
    PrivateDnsConfiguration mPdc;

    // TODO: Because incorrect CAs result in validation failed in strict mode, have
    // PrivateDnsConfiguration run mocked code rather than DnsTlsTransport::validate().
    inline static test::DnsTlsFrontend tls1{kServer1, "853", kBackend, "53"};
    inline static test::DnsTlsFrontend tls2{kServer2, "853", kBackend, "53"};
    inline static test::DNSResponder backend{kBackend, "53"};
};

TEST_F(PrivateDnsConfigurationTest, ValidationSuccess) {
    testing::InSequence seq;
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::success, kNetId));

    EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
    expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

    ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));
}

TEST_F(PrivateDnsConfigurationTest, ValidationFail_Opportunistic) {
    ASSERT_TRUE(backend.stopServer());

    testing::InSequence seq;
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::fail, kNetId));

    EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
    expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

    // Strictly wait for all of the validation finish; otherwise, the test can crash somehow.
    ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));
    ASSERT_TRUE(backend.startServer());
}

TEST_F(PrivateDnsConfigurationTest, Revalidation_Opportunistic) {
    const DnsTlsServer server(netdutils::IPSockAddr::toIPSockAddr(kServer1, 853));

    // Step 1: Set up and wait for validation complete.
    testing::InSequence seq;
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::success, kNetId));

    EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
    expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);
    ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));

    // Step 2: Simulate the DNS is temporarily broken, and then request a validation.
    // Expect the validation to run as follows:
    //   1. DnsResolver notifies of Validation::in_process when the validation is about to run.
    //   2. The first probing fails. DnsResolver notifies of Validation::in_process.
    //   3. One second later, the second probing begins and succeeds. DnsResolver notifies of
    //      Validation::success.
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId))
            .Times(2);
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::success, kNetId));

    std::thread t([] {
        std::this_thread::sleep_for(1000ms);
        backend.startServer();
    });
    backend.stopServer();
    EXPECT_TRUE(mPdc.requestValidation(kNetId, ServerIdentity(server), kMark).ok());

    t.join();
    expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);
    ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));
}

TEST_F(PrivateDnsConfigurationTest, ValidationProbingTime) {
    // The probing time threshold is 500 milliseconds.
    ScopedSystemProperty sp1(kMinPrivateDnsLatencyThresholdMsFlag, "500");
    ScopedSystemProperty sp2(kMaxPrivateDnsLatencyThresholdMsFlag, "1000");

    // TODO: Complete STRICT test after the dependency of DnsTlsFrontend is removed.
    static const struct TestConfig {
        std::string dnsMode;
        bool avoidBadPrivateDns;
        int probingTime;
        int expectProbeCount;
    } testConfigs[] = {
            // clang-format off
            {"OPPORTUNISTIC", false,   50, 1},
            {"OPPORTUNISTIC", false,  750, 1},
            {"OPPORTUNISTIC", false, 1500, 1},
            {"OPPORTUNISTIC",  true,   50, 1},
            {"OPPORTUNISTIC",  true,  750, 2},
            {"OPPORTUNISTIC",  true, 1500, 2},
            // clang-format on
    };

    for (const auto& config : testConfigs) {
        SCOPED_TRACE(fmt::format("testConfig: [{}, {}, {}, {}]", config.dnsMode,
                                 config.avoidBadPrivateDns, config.probingTime,
                                 config.expectProbeCount));

        ScopedSystemProperty sp3(kAvoidBadPrivateDnsFlag, (config.avoidBadPrivateDns ? "1" : "0"));
        forceExperimentsInstanceUpdate();

        // Simulate that validation takes the certain time to complete the first probe.
        std::thread t([&] {
            backend.setResponseDelayMs(config.probingTime);
            std::this_thread::sleep_for(std::chrono::milliseconds(config.probingTime + 500));
            backend.setResponseDelayMs(0);
        });

        testing::InSequence seq;
        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId))
                .Times(config.expectProbeCount);
        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::success, kNetId));

        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        // The thread is expected to be joined before the second probe begins.
        t.join();
        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));

        // Reset the state for the next round.
        mPdc.clear(kNetId);
        testing::Mock::VerifyAndClearExpectations(&mObserver);
    }

    backend.setResponseDelayMs(0);
}

// Tests that Private DNS validation won't be endless if the server works and it's slow.
TEST_F(PrivateDnsConfigurationTest, ValidationMaxProbes) {
    ScopedSystemProperty sp1(kMinPrivateDnsLatencyThresholdMsFlag, "100");
    ScopedSystemProperty sp2(kMaxPrivateDnsLatencyThresholdMsFlag, "200");
    const int serverLatencyMs = 300;

    // TODO: Complete STRICT test after the dependency of DnsTlsFrontend is removed.
    static const struct TestConfig {
        std::string dnsMode;
        bool avoidBadPrivateDns;
        Validation expectedValidationResult;
        int expectedProbes;
    } testConfigs[] = {
            // clang-format off
            {"OPPORTUNISTIC", false, Validation::success, 1},
            {"OPPORTUNISTIC", true,  Validation::fail,    kOpportunisticModeMaxAttempts},
            // clang-format on
    };

    for (const auto& config : testConfigs) {
        SCOPED_TRACE(
                fmt::format("testConfig: [{}, {}]", config.dnsMode, config.avoidBadPrivateDns));

        ScopedSystemProperty sp3(kAvoidBadPrivateDnsFlag, (config.avoidBadPrivateDns ? "1" : "0"));
        forceExperimentsInstanceUpdate();
        backend.setResponseDelayMs(serverLatencyMs);

        testing::InSequence seq;
        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId))
                .Times(config.expectedProbes);
        EXPECT_CALL(mObserver,
                    onValidationStateUpdate(kServer1, config.expectedValidationResult, kNetId));

        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        // probing time + backoff delay
        const int expectedValidationTimeMs =
                config.expectedProbes * serverLatencyMs + (config.expectedProbes - 1) * 1000;
        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; },
                                     std::chrono::milliseconds(expectedValidationTimeMs + 1000)));

        // Reset the state for the next round.
        mPdc.clear(kNetId);
        testing::Mock::VerifyAndClearExpectations(&mObserver);
    }

    backend.setResponseDelayMs(0);
}

TEST_F(PrivateDnsConfigurationTest, ValidationBlock) {
    backend.setDeferredResp(true);

    // onValidationStateUpdate() is called in sequence.
    {
        testing::InSequence seq;
        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 1; }));
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer2, Validation::in_process, kNetId));
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer2}, {}, {}), 0);
        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 2; }));
        mObserver.removeFromServerStateMap(kServer1);
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        // No duplicate validation as long as not in OFF mode; otherwise, an unexpected
        // onValidationStateUpdate() will be caught.
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1, kServer2}, {}, {}), 0);
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer2}, {}, {}), 0);
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        // The status keeps unchanged if pass invalid arguments.
        EXPECT_EQ(mPdc.set(kNetId, kMark, {"invalid_addr"}, {}, {}), -EINVAL);
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);
    }

    // The update for |kServer1| will be Validation::fail because |kServer1| is not an expected
    // server for the network.
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::fail, kNetId));
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer2, Validation::success, kNetId));
    backend.setDeferredResp(false);

    ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));

    // kServer1 is not a present server and thus should not be available from
    // PrivateDnsConfiguration::getStatus().
    mObserver.removeFromServerStateMap(kServer1);

    expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);
}

TEST_F(PrivateDnsConfigurationTest, Validation_NetworkDestroyedOrOffMode) {
    for (const std::string_view config : {"OFF", "NETWORK_DESTROYED"}) {
        SCOPED_TRACE(config);
        backend.setDeferredResp(true);

        testing::InSequence seq;
        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 1; }));
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        if (config == "OFF") {
            EXPECT_EQ(mPdc.set(kNetId, kMark, {}, {}, {}), 0);
        } else if (config == "NETWORK_DESTROYED") {
            mPdc.clear(kNetId);
        }

        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::fail, kNetId));
        backend.setDeferredResp(false);

        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));
        mObserver.removeFromServerStateMap(kServer1);
        expectPrivateDnsStatus(PrivateDnsMode::OFF);
    }
}

TEST_F(PrivateDnsConfigurationTest, NoValidation) {
    // If onValidationStateUpdate() is called, the test will fail with uninteresting mock
    // function calls in the end of the test.

    const auto expectStatus = [&]() {
        const PrivateDnsStatus status = mPdc.getStatus(kNetId);
        EXPECT_EQ(status.mode, PrivateDnsMode::OFF);
        EXPECT_THAT(status.serversMap, testing::IsEmpty());
    };

    EXPECT_EQ(mPdc.set(kNetId, kMark, {"invalid_addr"}, {}, {}), -EINVAL);
    expectStatus();

    EXPECT_EQ(mPdc.set(kNetId, kMark, {}, {}, {}), 0);
    expectStatus();
}

TEST_F(PrivateDnsConfigurationTest, ServerIdentity_Comparison) {
    DnsTlsServer server(netdutils::IPSockAddr::toIPSockAddr("127.0.0.1", 853));
    server.name = "dns.example.com";

    // Different socket address.
    DnsTlsServer other = server;
    EXPECT_EQ(ServerIdentity(server), ServerIdentity(other));
    other.ss = netdutils::IPSockAddr::toIPSockAddr("127.0.0.1", 5353);
    EXPECT_NE(ServerIdentity(server), ServerIdentity(other));
    other.ss = netdutils::IPSockAddr::toIPSockAddr("127.0.0.2", 853);
    EXPECT_NE(ServerIdentity(server), ServerIdentity(other));

    // Different provider hostname.
    other = server;
    EXPECT_EQ(ServerIdentity(server), ServerIdentity(other));
    other.name = "other.example.com";
    EXPECT_NE(ServerIdentity(server), ServerIdentity(other));
    other.name = "";
    EXPECT_NE(ServerIdentity(server), ServerIdentity(other));
}

TEST_F(PrivateDnsConfigurationTest, RequestValidation) {
    const DnsTlsServer server(netdutils::IPSockAddr::toIPSockAddr(kServer1, 853));
    const ServerIdentity identity(server);

    testing::InSequence seq;

    for (const std::string_view config : {"SUCCESS", "IN_PROGRESS", "FAIL"}) {
        SCOPED_TRACE(config);

        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
        if (config == "SUCCESS") {
            EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::success, kNetId));
        } else if (config == "IN_PROGRESS") {
            backend.setDeferredResp(true);
        } else {
            // config = "FAIL"
            ASSERT_TRUE(backend.stopServer());
            EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::fail, kNetId));
        }
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        // Wait until the validation state is transitioned.
        const int runningThreads = (config == "IN_PROGRESS") ? 1 : 0;
        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == runningThreads; }));

        if (config == "SUCCESS") {
            EXPECT_CALL(mObserver,
                        onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
            EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::success, kNetId));
            EXPECT_TRUE(mPdc.requestValidation(kNetId, identity, kMark).ok());
        } else if (config == "IN_PROGRESS") {
            EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::success, kNetId));
            EXPECT_FALSE(mPdc.requestValidation(kNetId, identity, kMark).ok());
        } else if (config == "FAIL") {
            EXPECT_FALSE(mPdc.requestValidation(kNetId, identity, kMark).ok());
        }

        // Resending the same request or requesting nonexistent servers are denied.
        EXPECT_FALSE(mPdc.requestValidation(kNetId, identity, kMark).ok());
        EXPECT_FALSE(mPdc.requestValidation(kNetId, identity, kMark + 1).ok());
        EXPECT_FALSE(mPdc.requestValidation(kNetId + 1, identity, kMark).ok());

        // Reset the test state.
        backend.setDeferredResp(false);
        backend.startServer();

        // Ensure the status of mObserver is synced.
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));
        mPdc.clear(kNetId);
    }
}

TEST_F(PrivateDnsConfigurationTest, GetPrivateDns) {
    const DnsTlsServer server1(netdutils::IPSockAddr::toIPSockAddr(kServer1, 853));
    const DnsTlsServer server2(netdutils::IPSockAddr::toIPSockAddr(kServer2, 853));

    EXPECT_FALSE(hasPrivateDnsServer(ServerIdentity(server1), kNetId));
    EXPECT_FALSE(hasPrivateDnsServer(ServerIdentity(server2), kNetId));

    // Suppress the warning.
    EXPECT_CALL(mObserver, onValidationStateUpdate).Times(2);

    EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
    expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

    EXPECT_TRUE(hasPrivateDnsServer(ServerIdentity(server1), kNetId));
    EXPECT_FALSE(hasPrivateDnsServer(ServerIdentity(server2), kNetId));
    EXPECT_FALSE(hasPrivateDnsServer(ServerIdentity(server1), kNetId + 1));

    ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));
}

// TODO: add ValidationFail_Strict test.

}  // namespace android::net
