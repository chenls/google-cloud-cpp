// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/pubsub/internal/subscription_session.h"
#include "google/cloud/pubsub/testing/mock_subscriber_stub.h"
#include "google/cloud/log.h"
#include "google/cloud/testing_util/assert_ok.h"
#include "google/cloud/testing_util/mock_completion_queue.h"
#include <gmock/gmock.h>
#include <atomic>

namespace google {
namespace cloud {
namespace pubsub_internal {
inline namespace GOOGLE_CLOUD_CPP_PUBSUB_NS {
namespace {

using ::testing::_;
using ::testing::AtLeast;
using ::testing::InSequence;

/// @test Verify callbacks are scheduled in the background threads.
TEST(SubscriptionSessionTest, ScheduleCallbacks) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  pubsub::Subscription const subscription("test-project", "test-subscription");

  std::mutex mu;
  int count = 0;
  EXPECT_CALL(*mock, AsyncPull(_, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly([&](google::cloud::CompletionQueue&,
                          std::unique_ptr<grpc::ClientContext>,
                          google::pubsub::v1::PullRequest const& request) {
        EXPECT_EQ(subscription.FullName(), request.subscription());
        google::pubsub::v1::PullResponse response;
        for (int i = 0; i != 2; ++i) {
          auto& m = *response.add_received_messages();
          std::lock_guard<std::mutex> lk(mu);
          m.set_ack_id("test-ack-id-" + std::to_string(count));
          m.set_delivery_attempt(42);
          m.mutable_message()->set_message_id("test-message-id-" +
                                              std::to_string(count));
          ++count;
        }
        return make_ready_future(make_status_or(response));
      });

  std::mutex ack_id_mu;
  std::condition_variable ack_id_cv;
  int expected_ack_id = 0;
  auto constexpr kAckCount = 100;
  EXPECT_CALL(*mock, AsyncAcknowledge(_, _, _))
      .Times(AtLeast(1))
      .WillRepeatedly(
          [&](google::cloud::CompletionQueue&,
              std::unique_ptr<grpc::ClientContext>,
              google::pubsub::v1::AcknowledgeRequest const& request) {
            EXPECT_EQ(subscription.FullName(), request.subscription());
            for (auto const& a : request.ack_ids()) {
              std::lock_guard<std::mutex> lk(ack_id_mu);
              EXPECT_EQ("test-ack-id-" + std::to_string(expected_ack_id), a);
              ++expected_ack_id;
              if (expected_ack_id >= kAckCount) ack_id_cv.notify_one();
            }
            return make_ready_future(Status{});
          });
  EXPECT_CALL(*mock, AsyncModifyAckDeadline(_, _, _))
      .WillRepeatedly([](google::cloud::CompletionQueue&,
                         std::unique_ptr<grpc::ClientContext>,
                         google::pubsub::v1::ModifyAckDeadlineRequest const&) {
        return make_ready_future(Status{});
      });

  google::cloud::CompletionQueue cq;
  std::vector<std::thread> tasks;
  std::generate_n(std::back_inserter(tasks), 4,
                  [&] { return std::thread([&cq] { cq.Run(); }); });
  std::set<std::thread::id> ids;
  auto const main_id = std::this_thread::get_id();
  std::transform(tasks.begin(), tasks.end(), std::inserter(ids, ids.end()),
                 [](std::thread const& t) { return t.get_id(); });

  std::atomic<int> expected_message_id{0};
  auto handler = [&](pubsub::Message const& m, pubsub::AckHandler h) {
    EXPECT_EQ(42, h.delivery_attempt());
    EXPECT_EQ("test-message-id-" + std::to_string(expected_message_id),
              m.message_id());
    auto pos = ids.find(std::this_thread::get_id());
    EXPECT_NE(ids.end(), pos);
    EXPECT_NE(main_id, std::this_thread::get_id());
    // Increment the counter before acking, as the ack() may trigger a new call
    // before this function gets to run.
    ++expected_message_id;
    std::move(h).ack();
  };

  auto response =
      CreateTestingSubscriptionSession(mock, cq,
                                       {subscription.FullName(), handler,
                                        pubsub::SubscriptionOptions{}
                                            .set_message_count_watermarks(0, 1)
                                            .set_concurrency_watermarks(0, 1)});
  {
    std::unique_lock<std::mutex> lk(ack_id_mu);
    ack_id_cv.wait(lk, [&] { return expected_ack_id >= kAckCount; });
  }
  response.cancel();
  EXPECT_STATUS_OK(response.get());

  cq.Shutdown();
  for (auto& t : tasks) t.join();
}

/// @test Verify callbacks are scheduled in sequence.
TEST(SubscriptionSessionTest, SequencedCallbacks) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  pubsub::Subscription const subscription("test-project", "test-subscription");

  std::mutex mu;
  int count = 0;
  auto generate_3 = [&](google::cloud::CompletionQueue&,
                        std::unique_ptr<grpc::ClientContext>,
                        google::pubsub::v1::PullRequest const& request) {
    EXPECT_EQ(subscription.FullName(), request.subscription());
    google::pubsub::v1::PullResponse response;
    for (int i = 0; i != 3; ++i) {
      auto& m = *response.add_received_messages();
      std::lock_guard<std::mutex> lk(mu);
      m.set_ack_id("test-ack-id-" + std::to_string(count));
      m.mutable_message()->set_message_id("test-message-id-" +
                                          std::to_string(count));
      ++count;
    }
    return make_ready_future(make_status_or(response));
  };

  EXPECT_CALL(*mock, AsyncModifyAckDeadline(_, _, _))
      .WillRepeatedly([](google::cloud::CompletionQueue&,
                         std::unique_ptr<grpc::ClientContext>,
                         google::pubsub::v1::ModifyAckDeadlineRequest const&) {
        return make_ready_future(Status{});
      });
  {
    InSequence sequence;

    EXPECT_CALL(*mock, AsyncPull(_, _, _)).WillOnce(generate_3);
    EXPECT_CALL(*mock, AsyncAcknowledge(_, _, _))
        .Times(3)
        .WillRepeatedly([](google::cloud::CompletionQueue&,
                           std::unique_ptr<grpc::ClientContext>,
                           google::pubsub::v1::AcknowledgeRequest const&) {
          return make_ready_future(Status{});
        });

    EXPECT_CALL(*mock, AsyncPull(_, _, _)).WillOnce(generate_3);
    EXPECT_CALL(*mock, AsyncAcknowledge(_, _, _))
        .Times(3)
        .WillRepeatedly([](google::cloud::CompletionQueue&,
                           std::unique_ptr<grpc::ClientContext>,
                           google::pubsub::v1::AcknowledgeRequest const&) {
          return make_ready_future(Status{});
        });

    EXPECT_CALL(*mock, AsyncPull(_, _, _)).WillOnce(generate_3);
    EXPECT_CALL(*mock, AsyncAcknowledge(_, _, _))
        .Times(3)
        .WillRepeatedly([](google::cloud::CompletionQueue&,
                           std::unique_ptr<grpc::ClientContext>,
                           google::pubsub::v1::AcknowledgeRequest const&) {
          return make_ready_future(Status{});
        });
  }

  promise<void> enough_messages;
  std::atomic<int> received_counter{0};
  auto constexpr kMaximumMessages = 9;
  auto handler = [&](pubsub::Message const& m, pubsub::AckHandler h) {
    auto c = received_counter.load();
    EXPECT_LE(c, kMaximumMessages);
    SCOPED_TRACE("Running for message " + m.message_id() +
                 ", counter=" + std::to_string(c));
    EXPECT_EQ("test-message-id-" + std::to_string(c), m.message_id());
    if (++received_counter == kMaximumMessages) {
      enough_messages.set_value();
    }
    std::move(h).ack();
  };

  google::cloud::CompletionQueue cq;
  std::thread t([&cq] { cq.Run(); });
  auto response =
      CreateTestingSubscriptionSession(mock, cq,
                                       {subscription.FullName(), handler,
                                        pubsub::SubscriptionOptions{}
                                            .set_message_count_watermarks(0, 1)
                                            .set_concurrency_watermarks(0, 1)});
  enough_messages.get_future()
      .then([&](future<void>) { response.cancel(); })
      .get();
  EXPECT_STATUS_OK(response.get());

  cq.Shutdown();
  t.join();
}

/// @test Verify callbacks are scheduled in sequence.
TEST(SubscriptionSessionTest, UpdateAckDeadlines) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  pubsub::Subscription const subscription("test-project", "test-subscription");

  std::mutex mu;
  int count = 0;
  auto generate_3 = [&](google::cloud::CompletionQueue&,
                        std::unique_ptr<grpc::ClientContext>,
                        google::pubsub::v1::PullRequest const& request) {
    EXPECT_EQ(subscription.FullName(), request.subscription());
    google::pubsub::v1::PullResponse response;
    for (int i = 0; i != 3; ++i) {
      auto& m = *response.add_received_messages();
      std::lock_guard<std::mutex> lk(mu);
      m.set_ack_id("test-ack-id-" + std::to_string(count));
      m.mutable_message()->set_message_id("test-message-id-" +
                                          std::to_string(count));
      ++count;
    }
    return make_ready_future(make_status_or(response));
  };
  auto generate_ack_response =
      [&](google::cloud::CompletionQueue&, std::unique_ptr<grpc::ClientContext>,
          google::pubsub::v1::AcknowledgeRequest const&) {
        return make_ready_future(Status{});
      };
  // The basic expectations, pull some data, ack each message, pull more data,
  // must be in a specific order.
  {
    InSequence sequence;
    EXPECT_CALL(*mock, AsyncPull(_, _, _)).WillOnce(generate_3);
    EXPECT_CALL(*mock, AsyncAcknowledge(_, _, _))
        .Times(3)
        .WillRepeatedly(generate_ack_response);
    EXPECT_CALL(*mock, AsyncPull(_, _, _)).WillOnce(generate_3);
    EXPECT_CALL(*mock, AsyncAcknowledge(_, _, _))
        .Times(3)
        .WillRepeatedly(generate_ack_response);
    EXPECT_CALL(*mock, AsyncPull(_, _, _)).WillOnce(generate_3);
    EXPECT_CALL(*mock, AsyncAcknowledge(_, _, _))
        .Times(3)
        .WillRepeatedly(generate_ack_response);
  }

  // The expectations for timers and AsyncModifyAckDeadline are more complex.
  // The setup is as follows:
  // - The subscription handler stops after receiving specific messages,
  //   e.g. test-ack-id-2.
  // - Because it is stopped, the timer to update the deadlines will eventually
  //   expire, and (again eventually) will contain only the messages that have
  //   not been acked.
  // - When that happens we signal the handler to continue.
  //
  // The set of points when to stop is expressed in this array of "tripwires":
  struct Tripwire {
    // When is this triggered
    std::string const ack_id;
    // How many times it needs to repeat before the barrier is lifted
    int repeats;
    // The barrier to wake up the waiting thread
    promise<void> barrier;
  } tripwires[] = {
      {"test-ack-id-2", 1, {}},
      {"test-ack-id-5", 2, {}},
  };
  std::mutex tripwires_mu;

  // This is how we mock all the AsyncModifyAckDeadline() calls, signaling the
  // handler at the right points.
  EXPECT_CALL(*mock, AsyncModifyAckDeadline(_, _, _))
      .WillRepeatedly(
          [&](google::cloud::CompletionQueue&,
              std::unique_ptr<grpc::ClientContext>,
              google::pubsub::v1::ModifyAckDeadlineRequest const& r) {
            auto ids_are = [&r](std::string const& s) {
              return r.ack_ids_size() == 1 && r.ack_ids(0) == s;
            };
            std::lock_guard<std::mutex> lk(tripwires_mu);
            for (auto& tw : tripwires) {
              if (!ids_are(tw.ack_id)) continue;
              if (--tw.repeats == 0) tw.barrier.set_value();
            }
            return make_ready_future(Status{});
          });

  // Now unto the handler, basically it
  promise<void> enough_messages;
  std::atomic<int> message_count{0};
  auto constexpr kMaximumMessages = 9;
  auto handler = [&](pubsub::Message const&, pubsub::AckHandler h) {
    for (auto& tw : tripwires) {
      // Note the lack of locking, this is Okay because the values we use either
      // never changes (`ack_id` is const) or they are thread-safe by design:
      // (barrier is a promise<void> which better be thread-safe.
      if (tw.ack_id == h.ack_id()) tw.barrier.get_future().get();
    }
    // When enough messages are received signal the main thread to stop the
    // test.
    if (++message_count == kMaximumMessages) enough_messages.set_value();
    std::move(h).ack();
  };

  google::cloud::CompletionQueue cq;
  std::vector<std::thread> pool;
  // we need more than one thread because `handler()` blocks and the CQ needs to
  // make progress.
  std::generate_n(std::back_inserter(pool), 4,
                  [&cq] { return std::thread{[&cq] { cq.Run(); }}; });

  auto response = CreateTestingSubscriptionSession(
      mock, cq,
      {subscription.FullName(), handler,
       pubsub::SubscriptionOptions{}
           .set_message_count_watermarks(0, 1)
           .set_concurrency_watermarks(0, 1)
           .set_max_deadline_time(std::chrono::seconds(60))});
  enough_messages.get_future()
      .then([&](future<void>) { response.cancel(); })
      .get();
  EXPECT_STATUS_OK(response.get());

  cq.Shutdown();
  for (auto& t : pool) t.join();
}

/// @test Verify pending callbacks are nacked on shutdown.
TEST(SubscriptionSessionTest, ShutdownNackCallbacks) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  pubsub::Subscription const subscription("test-project", "test-subscription");

  std::mutex mu;
  int count = 0;
  auto generate_5 = [&](google::cloud::CompletionQueue&,
                        std::unique_ptr<grpc::ClientContext>,
                        google::pubsub::v1::PullRequest const& request) {
    EXPECT_EQ(subscription.FullName(), request.subscription());
    google::pubsub::v1::PullResponse response;
    for (int i = 0; i != 5; ++i) {
      auto& m = *response.add_received_messages();
      std::lock_guard<std::mutex> lk(mu);
      m.set_ack_id("test-ack-id-" + std::to_string(count));
      m.mutable_message()->set_message_id("test-message-id-" +
                                          std::to_string(count));
      ++count;
    }
    return make_ready_future(make_status_or(response));
  };
  auto generate_ack_response =
      [&](google::cloud::CompletionQueue&, std::unique_ptr<grpc::ClientContext>,
          google::pubsub::v1::AcknowledgeRequest const&) {
        return make_ready_future(Status{});
      };
  auto generate_nack_response =
      [&](google::cloud::CompletionQueue&, std::unique_ptr<grpc::ClientContext>,
          google::pubsub::v1::ModifyAckDeadlineRequest const&) {
        return make_ready_future(Status{});
      };

  EXPECT_CALL(*mock, AsyncPull).WillOnce(generate_5);
  EXPECT_CALL(*mock, AsyncAcknowledge)
      .Times(2)
      .WillRepeatedly(generate_ack_response);
  EXPECT_CALL(*mock, AsyncModifyAckDeadline)
      .Times(AtLeast(1))
      .WillRepeatedly(generate_nack_response);

  // Now unto the handler, basically it counts messages and from the second one
  // onwards it just nacks.
  promise<void> enough_messages;
  std::atomic<int> ack_count{0};
  auto constexpr kMaximumAcks = 2;
  auto handler = [&](pubsub::Message const&, pubsub::AckHandler h) {
    auto count = ++ack_count;
    if (count == kMaximumAcks) enough_messages.set_value();
    std::move(h).ack();
  };

  google::cloud::CompletionQueue cq;
  auto response = CreateTestingSubscriptionSession(
      mock, cq,
      {subscription.FullName(), handler,
       pubsub::SubscriptionOptions{}
           .set_message_count_watermarks(0, 1)
           .set_concurrency_watermarks(0, 1)
           .set_max_deadline_time(std::chrono::seconds(60))});
  // Setup the system to cancel after the second message.
  auto done = enough_messages.get_future().then(
      [&](future<void>) { response.cancel(); });
  std::thread t{[&cq] { cq.Run(); }};
  done.get();
  EXPECT_STATUS_OK(response.get());

  cq.Shutdown();
  t.join();
}

/// @test Verify AsyncPull responses are nacked after shutdown.
TEST(SubscriptionSessionTest, ShutdownNackOnPull) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  pubsub::Subscription const subscription("test-project", "test-subscription");

  // In this test we are going to return futures that are satisfied much later
  // in the test, to control the sequence of events with more precision.
  std::mutex mu;
  promise<StatusOr<google::pubsub::v1::PullResponse>> async_pull_response;
  promise<void> async_pull_called;
  auto pull_handler = [&](google::cloud::CompletionQueue&,
                          std::unique_ptr<grpc::ClientContext>,
                          google::pubsub::v1::PullRequest const& request) {
    async_pull_called.set_value();
    EXPECT_EQ(subscription.FullName(), request.subscription());
    return async_pull_response.get_future();
  };
  auto nack_handler = [&](google::cloud::CompletionQueue&,
                          std::unique_ptr<grpc::ClientContext>,
                          google::pubsub::v1::ModifyAckDeadlineRequest const&) {
    return make_ready_future(Status{});
  };

  // We are going to cancel the pull loop before the first AsyncPull() responds,
  // we expect all messages to be nacked.
  {
    InSequence sequence;
    EXPECT_CALL(*mock, AsyncPull).WillOnce(pull_handler);
    EXPECT_CALL(*mock, AsyncModifyAckDeadline).WillOnce(nack_handler);
  }

  // Now unto the handler, basically it counts messages and from the second one
  // onwards it just nacks.
  std::atomic<int> handler_count{0};
  auto handler = [&](pubsub::Message const&, pubsub::AckHandler h) {
    ++handler_count;
    std::move(h).ack();
  };

  google::cloud::CompletionQueue cq;
  std::thread t{[&cq] { cq.Run(); }};
  auto response = CreateTestingSubscriptionSession(
      mock, cq,
      {subscription.FullName(), handler,
       pubsub::SubscriptionOptions{}
           .set_message_count_watermarks(0, 1)
           .set_max_deadline_time(std::chrono::seconds(60))});
  async_pull_called.get_future().get();
  response.cancel();

  // Now generate the response for AsyncPull():
  {
    google::pubsub::v1::PullResponse re;
    for (int i = 0; i != 5; ++i) {
      auto& m = *re.add_received_messages();
      std::lock_guard<std::mutex> lk(mu);
      m.set_ack_id("test-ack-id-" + std::to_string(i));
      m.mutable_message()->set_message_id("test-message-id-" +
                                          std::to_string(i));
    }
    // This satisfies the pending AsyncPull() response, but the subscription has
    // already been canceled.
    async_pull_response.set_value(std::move(re));
  }
  EXPECT_STATUS_OK(response.get());

  cq.Shutdown();
  t.join();
  EXPECT_EQ(0, handler_count);
}

/// @test Verify shutting down a session waits for pending tasks.
TEST(SubscriptionSessionTest, ShutdownWaitsFutures) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  pubsub::Subscription const subscription("test-project", "test-subscription");

  // A number of mocks that return futures satisfied a bit after the call is
  // made. This better simulates the behavior when running against an actual
  // service.
  auto constexpr kMaximumAcks = 10;
  std::mutex generate_mu;
  int generate_count = 0;

  using TimerFuture = future<StatusOr<std::chrono::system_clock::time_point>>;
  auto generate = [&](google::cloud::CompletionQueue& cq,
                      std::unique_ptr<grpc::ClientContext>,
                      google::pubsub::v1::PullRequest const& r) {
    auto const count = (std::max)(r.max_messages(), 2 * kMaximumAcks);
    return cq.MakeRelativeTimer(std::chrono::microseconds(10))
        .then([&generate_mu, &generate_count, count](TimerFuture) {
          std::unique_lock<std::mutex> lk(generate_mu);
          google::pubsub::v1::PullResponse response;
          for (int i = 0; i != count; ++i) {
            auto& m = *response.add_received_messages();
            m.set_ack_id("test-ack-id-" + std::to_string(generate_count));
            m.mutable_message()->set_message_id("test-message-id-" +
                                                std::to_string(generate_count));
            ++generate_count;
          }
          lk.unlock();
          return make_status_or(response);
        });
  };
  auto generate_ack_response =
      [](google::cloud::CompletionQueue& cq,
         std::unique_ptr<grpc::ClientContext>,
         google::pubsub::v1::AcknowledgeRequest const&) {
        return cq.MakeRelativeTimer(std::chrono::microseconds(2))
            .then([](TimerFuture) { return Status{}; });
      };
  auto generate_nack_response =
      [](google::cloud::CompletionQueue& cq,
         std::unique_ptr<grpc::ClientContext>,
         google::pubsub::v1::ModifyAckDeadlineRequest const&) {
        return cq.MakeRelativeTimer(std::chrono::microseconds(2))
            .then([](TimerFuture) { return Status{}; });
      };

  EXPECT_CALL(*mock, AsyncPull).Times(AtLeast(1)).WillRepeatedly(generate);
  EXPECT_CALL(*mock, AsyncAcknowledge)
      .Times(AtLeast(1))
      .WillRepeatedly(generate_ack_response);
  EXPECT_CALL(*mock, AsyncModifyAckDeadline)
      .Times(AtLeast(1))
      .WillRepeatedly(generate_nack_response);

  internal::AutomaticallyCreatedBackgroundThreads background;
  std::atomic<int> handler_counter{0};
  {
    // Now unto the handler, basically it counts messages and nacks starting at
    // kMaximumAcks.
    promise<void> got_one;
    auto handler = [&](pubsub::Message const&, pubsub::AckHandler h) {
      if (handler_counter.load() == 0) got_one.set_value();
      if (++handler_counter > kMaximumAcks) return;
      std::move(h).ack();
    };

    auto session = CreateSubscriptionSession(
        mock, background.cq(),
        {subscription.FullName(), handler, pubsub::SubscriptionOptions{}});
    got_one.get_future()
        .then([&session](future<void>) { session.cancel(); })
        .get();
    auto status = session.get();
    EXPECT_STATUS_OK(status);
    EXPECT_LE(1, handler_counter.load());
  }
  // Schedule at least a few more iterations of the CQ loop. If the shutdown is
  // buggy, we will see TSAN/ASAN errors because the `handler` defined above
  // is still called.
  auto const initial_value = handler_counter.load();
  for (int i = 0; i != 10; ++i) {
    SCOPED_TRACE("Wait loop iteration " + std::to_string(i));
    promise<void> done;
    background.cq().RunAsync([&done] { done.set_value(); });
    done.get_future().get();
  }
  auto const final_value = handler_counter.load();
  EXPECT_EQ(initial_value, final_value);
}

/// @test Verify shutting down a session waits for pending tasks.
TEST(SubscriptionSessionTest, ShutdownWaitsConditionVars) {
  auto mock = std::make_shared<pubsub_testing::MockSubscriberStub>();
  pubsub::Subscription const subscription("test-project", "test-subscription");

  // A number of mocks that return futures satisfied a bit after the call is
  // made. This better simulates the behavior when running against an actual
  // service.
  auto constexpr kMaximumAcks = 20;
  std::mutex generate_mu;
  int generate_count = 0;

  using TimerFuture = future<StatusOr<std::chrono::system_clock::time_point>>;
  auto generate = [&](google::cloud::CompletionQueue& cq,
                      std::unique_ptr<grpc::ClientContext>,
                      google::pubsub::v1::PullRequest const& r) {
    auto const count = (std::max)(r.max_messages(), 2 * kMaximumAcks);
    return cq.MakeRelativeTimer(std::chrono::microseconds(10))
        .then([&generate_mu, &generate_count, count](TimerFuture) {
          std::unique_lock<std::mutex> lk(generate_mu);
          google::pubsub::v1::PullResponse response;
          for (int i = 0; i != count; ++i) {
            auto& m = *response.add_received_messages();
            m.set_ack_id("test-ack-id-" + std::to_string(generate_count));
            m.mutable_message()->set_message_id("test-message-id-" +
                                                std::to_string(generate_count));
            ++generate_count;
          }
          lk.unlock();
          return make_status_or(response);
        });
  };
  auto generate_ack_response =
      [](google::cloud::CompletionQueue& cq,
         std::unique_ptr<grpc::ClientContext>,
         google::pubsub::v1::AcknowledgeRequest const&) {
        return cq.MakeRelativeTimer(std::chrono::microseconds(2))
            .then([](TimerFuture) { return Status{}; });
      };
  auto generate_nack_response =
      [](google::cloud::CompletionQueue& cq,
         std::unique_ptr<grpc::ClientContext>,
         google::pubsub::v1::ModifyAckDeadlineRequest const&) {
        return cq.MakeRelativeTimer(std::chrono::microseconds(2))
            .then([](TimerFuture) { return Status{}; });
      };

  EXPECT_CALL(*mock, AsyncPull).Times(AtLeast(1)).WillRepeatedly(generate);
  EXPECT_CALL(*mock, AsyncAcknowledge)
      .Times(AtLeast(kMaximumAcks))
      .WillRepeatedly(generate_ack_response);
  EXPECT_CALL(*mock, AsyncModifyAckDeadline)
      .Times(AtLeast(1))
      .WillRepeatedly(generate_nack_response);

  internal::AutomaticallyCreatedBackgroundThreads background;
  std::atomic<int> handler_counter{0};
  {
    // Now unto the handler, basically it counts messages and nacks starting at
    // kMaximumAcks.
    std::mutex mu;
    std::condition_variable cv;
    int ack_count = 0;
    auto handler = [&](pubsub::Message const&, pubsub::AckHandler h) {
      ++handler_counter;
      std::unique_lock<std::mutex> lk(mu);
      if (++ack_count > kMaximumAcks) return;
      lk.unlock();
      cv.notify_one();
      std::move(h).ack();
    };

    auto session = CreateSubscriptionSession(
        mock, background.cq(), {subscription.FullName(), handler, {}});
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return ack_count >= kMaximumAcks; });
    }
    session.cancel();
    auto status = session.get();
    EXPECT_STATUS_OK(status);
  }
  // Schedule at least a few more iterations of the CQ loop. If the shutdown is
  // buggy, we will see TSAN/ASAN errors because the `handler` defined above
  // is still called.
  auto const initial_value = handler_counter.load();
  for (int i = 0; i != 10; ++i) {
    SCOPED_TRACE("Wait loop iteration " + std::to_string(i));
    promise<void> done;
    background.cq().RunAsync([&done] { done.set_value(); });
    done.get_future().get();
  }
  auto const final_value = handler_counter.load();
  EXPECT_EQ(initial_value, final_value);
}

}  // namespace
}  // namespace GOOGLE_CLOUD_CPP_PUBSUB_NS
}  // namespace pubsub_internal
}  // namespace cloud
}  // namespace google
