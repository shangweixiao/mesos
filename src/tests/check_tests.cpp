// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <mesos/v1/mesos.hpp>

#include <process/clock.hpp>
#include <process/owned.hpp>

#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include <stout/os/getcwd.hpp>

#include "checks/checker.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "tests/flags.hpp"
#include "tests/health_check_test_helper.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;

using process::Future;
using process::Owned;

using std::pair;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

// This command fails every other invocation. Assuming `path` does not
// initially exist, for all runs i in Nat0, the following case i % 2 applies:
//
// Case 0:
//   - Attempt to remove the nonexistent temporary file.
//   - Create the temporary file.
//   - Exit with a non-zero status.
//
// Case 1:
//   - Remove the temporary file.
//   - Exit with a zero status.
#ifndef __WINDOWS__
#define FLAPPING_CHECK_COMMAND(path)                                    \
  string("rm ") + path + " || (touch " + path + "; exit 1)"
#else
#define FLAPPING_CHECK_COMMAND(path)                                    \
  string("powershell -command ") +                                      \
  "$ri_err = Remove-Item -ErrorAction SilentlyContinue"                 \
    " \"" + path + "\";"                                                \
  "if (-not $?) {"                                                      \
  "  Set-Content -Path (\"" + path + "\") -Value ($null);"              \
  "  exit 1"                                                            \
  "}"
#endif // !__WINDOWS__


// This command stalls each invocation except the first one. Assuming `path`
// does not initially exist, for all runs i in Nat0, the following applies:
//
// Case 0:
//   - Test whether the nonexistent temporary file exists.
//   - Create the temporary file.
//   - Exit with a non-zero status.
//
// Cases 1..n:
//   - Hang for 1000 seconds.
//   - Exit with a zero status.
#ifndef __WINDOWS__
#define STALLING_CHECK_COMMAND(path)                                    \
  string("(ls ") + path + " && " + SLEEP_COMMAND(1000) +                \
  ") || (touch " + path + "; exit 1)"
#else
#define STALLING_CHECK_COMMAND(path)                                    \
  string("powershell -command ") +                                      \
  "if (Test-Path \"" + path + "\") {" +                                 \
     SLEEP_COMMAND(1000) +                                              \
  "} else {"                                                            \
  "  Set-Content -Path (\"" + path + "\") -Value ($null);"              \
  "  exit 1"                                                            \
  "}"
#endif // !__WINDOWS__


// Tests for checks support in built in executors. Logically the tests
// are elements of the cartesian product `executor-type` x `check-type`
// and are split into groups by `executor-type`:
//   * command executor tests,
//   * docker executor tests,
//   * default executor tests.

class CheckTest : public MesosTest
{
public:
  virtual void acknowledge(
      Mesos* mesos,
      const v1::FrameworkID& frameworkId,
      const v1::TaskStatus& status)
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(status.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(status.agent_id());
    acknowledge->set_uuid(status.uuid());

    mesos->send(call);
  }

  virtual void launchTask(
      Mesos* mesos,
      const v1::Offer& offer,
      const v1::TaskInfo& task)
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(offer.framework_id());
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(task);

    mesos->send(call);
  }

  virtual void launchTaskGroup(
      Mesos* mesos,
      const v1::Offer& offer,
      const v1::ExecutorInfo& executor,
      const v1::TaskGroupInfo& taskGroup)
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(offer.framework_id());
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos->send(call);
  }

  virtual void reconcile(
      Mesos* mesos,
      const v1::FrameworkID& frameworkId,
      const vector<pair<const v1::TaskID, const v1::AgentID>>& tasks)
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::RECONCILE);

    call.mutable_reconcile();

    foreach (const auto& task, tasks) {
      Call::Reconcile::Task* reconcile =
        call.mutable_reconcile()->add_tasks();
      reconcile->mutable_task_id()->CopyFrom(task.first);
      reconcile->mutable_agent_id()->CopyFrom(task.second);
    }

    mesos->send(call);
  }

  virtual void subscribe(
      Mesos* mesos,
      const v1::FrameworkInfo& framework)
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(framework);

    mesos->send(call);
  }

  virtual void teardown(
      Mesos* mesos,
      const v1::FrameworkID& frameworkId)
  {
    Call call;
    call.set_type(Call::TEARDOWN);
    call.mutable_framework_id()->CopyFrom(frameworkId);

    mesos->send(call);
  }
};


// These are check tests with the command executor.
class CommandExecutorCheckTest : public CheckTest {};

// Verifies that a command check is supported by the command executor,
// its status is delivered in a task status update, and the last known
// status can be obtained during explicit and implicit reconciliation.
// Additionally ensures that the specified environment of the command
// check is honored.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    CommandExecutorCheckTest,
    CommandCheckDeliveredAndReconciled)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<Event::Update> updateTaskRunning;
  Future<Event::Update> updateCheckResult;
  Future<Event::Update> updateExplicitReconciliation;
  Future<Event::Update> updateImplicitReconciliation;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillOnce(FutureArg<1>(&updateExplicitReconciliation))
    .WillOnce(FutureArg<1>(&updateImplicitReconciliation))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::Resources resources =
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(10000));

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::COMMAND);
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(0);

  v1::CommandInfo* checkCommand =
    checkInfo->mutable_command()->mutable_command();
  checkCommand->set_value("exit $STATUS");

  v1::Environment::Variable* variable =
    checkCommand->mutable_environment()->add_variables();
  variable->set_name("STATUS");
  variable->set_value("1");

  launchTask(&mesos, offer, taskInfo);

  AWAIT_READY(updateTaskRunning);
  const v1::TaskStatus& taskRunning = updateTaskRunning->status();

  ASSERT_EQ(TASK_RUNNING, taskRunning.state());
  EXPECT_EQ(taskInfo.task_id(), taskRunning.task_id());
  EXPECT_TRUE(taskRunning.has_check_status());
  EXPECT_TRUE(taskRunning.check_status().has_command());
  EXPECT_FALSE(taskRunning.check_status().command().has_exit_code());

  acknowledge(&mesos, frameworkId, taskRunning);

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_EQ(taskInfo.task_id(), checkResult.task_id());
  EXPECT_TRUE(checkResult.has_check_status());
  EXPECT_TRUE(checkResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResult);

  // Trigger explicit reconciliation.
  reconcile(
      &mesos,
      frameworkId,
      {std::make_pair(checkResult.task_id(), checkResult.agent_id())});

  AWAIT_READY(updateExplicitReconciliation);
  const v1::TaskStatus& explicitReconciliation =
    updateExplicitReconciliation->status();

  ASSERT_EQ(TASK_RUNNING, explicitReconciliation.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_RECONCILIATION,
      explicitReconciliation.reason());
  EXPECT_EQ(taskInfo.task_id(), explicitReconciliation.task_id());
  EXPECT_TRUE(explicitReconciliation.has_check_status());
  EXPECT_TRUE(explicitReconciliation.check_status().command().has_exit_code());
  EXPECT_EQ(1, explicitReconciliation.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, explicitReconciliation);

  // Trigger implicit reconciliation.
  reconcile(&mesos, frameworkId, {});

  AWAIT_READY(updateImplicitReconciliation);
  const v1::TaskStatus& implicitReconciliation =
    updateImplicitReconciliation->status();

  ASSERT_EQ(TASK_RUNNING, implicitReconciliation.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_RECONCILIATION,
      implicitReconciliation.reason());
  EXPECT_EQ(taskInfo.task_id(), implicitReconciliation.task_id());
  EXPECT_TRUE(implicitReconciliation.has_check_status());
  EXPECT_TRUE(implicitReconciliation.check_status().command().has_exit_code());
  EXPECT_EQ(1, implicitReconciliation.check_status().command().exit_code());
}


// Verifies that a command check's status changes are delivered.
//
// TODO(alexr): When check mocking is available, ensure that *only*
// status changes are delivered.
TEST_F(CommandExecutorCheckTest, CommandCheckStatusChange)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<Event::Update> updateTaskRunning;
  Future<Event::Update> updateCheckResult;
  Future<Event::Update> updateCheckResultChanged;
  Future<Event::Update> updateCheckResultBack;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillOnce(FutureArg<1>(&updateCheckResultChanged))
    .WillOnce(FutureArg<1>(&updateCheckResultBack))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::Resources resources =
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::TaskInfo taskInfo =
      v1::createTask(agentId, resources, SLEEP_COMMAND(10000));

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::COMMAND);
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(0);
  checkInfo->mutable_command()->mutable_command()->set_value(
      FLAPPING_CHECK_COMMAND(path::join(os::getcwd(), "XXXXXX")));

  launchTask(&mesos, offer, taskInfo);

  AWAIT_READY(updateTaskRunning);
  ASSERT_EQ(TASK_RUNNING, updateTaskRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateTaskRunning->status().task_id());

  acknowledge(&mesos, frameworkId, updateTaskRunning->status());

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_TRUE(checkResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResult);

  AWAIT_READY(updateCheckResultChanged);
  const v1::TaskStatus& checkResultChanged = updateCheckResultChanged->status();

  ASSERT_EQ(TASK_RUNNING, checkResultChanged.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResultChanged.reason());
  EXPECT_TRUE(checkResultChanged.check_status().command().has_exit_code());
  EXPECT_EQ(0, checkResultChanged.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResultChanged);

  AWAIT_READY(updateCheckResultBack);
  const v1::TaskStatus& checkResultBack = updateCheckResultBack->status();

  ASSERT_EQ(TASK_RUNNING, checkResultBack.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResultBack.reason());
  EXPECT_TRUE(checkResultBack.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResultBack.check_status().command().exit_code());
}


// Verifies that when a command check times out after a successful check,
// an empty check status update is delivered.
TEST_F(CommandExecutorCheckTest, CommandCheckTimeout)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<Event::Update> updateTaskRunning;
  Future<Event::Update> updateCheckResult;
  Future<Event::Update> updateCheckResultTimeout;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillOnce(FutureArg<1>(&updateCheckResultTimeout))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::Resources resources =
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::TaskInfo taskInfo =
      v1::createTask(agentId, resources, SLEEP_COMMAND(10000));

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::COMMAND);
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(0);
  checkInfo->set_timeout_seconds(1);
  checkInfo->mutable_command()->mutable_command()->set_value(
      STALLING_CHECK_COMMAND(path::join(os::getcwd(), "XXXXXX")));

  launchTask(&mesos, offer, taskInfo);

  AWAIT_READY(updateTaskRunning);
  ASSERT_EQ(TASK_RUNNING, updateTaskRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateTaskRunning->status().task_id());

  acknowledge(&mesos, frameworkId, updateTaskRunning->status());

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_TRUE(checkResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResult);

  AWAIT_READY(updateCheckResultTimeout);
  const v1::TaskStatus& checkResultTimeout = updateCheckResultTimeout->status();

  ASSERT_EQ(TASK_RUNNING, checkResultTimeout.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResultTimeout.reason());
  EXPECT_FALSE(checkResultTimeout.check_status().command().has_exit_code());
}


// Verifies that when both command check and health check are specified,
// health and check updates include both statuses. Also verifies that
// both statuses are included upon reconciliation.
TEST_F(CommandExecutorCheckTest, CommandCheckAndHealthCheckNoShadowing)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<Event::Update> updateTaskRunning;
  Future<Event::Update> updateCheckResult;
  Future<Event::Update> updateHealthResult;
  Future<Event::Update> updateImplicitReconciliation;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillOnce(FutureArg<1>(&updateHealthResult))
    .WillOnce(FutureArg<1>(&updateImplicitReconciliation))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::Resources resources =
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::TaskInfo taskInfo =
      v1::createTask(agentId, resources, SLEEP_COMMAND(10000));

  // Set both check and health check interval to an increased value to
  // prevent a second update coming before reconciliation response.
  int interval = 10;

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::COMMAND);
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(interval);
  checkInfo->mutable_command()->mutable_command()->set_value("exit 1");

  // Delay health check for 1s to ensure health update comes after check update.
  //
  // TODO(alexr): This can lead to flakiness on busy agents. A more robust
  // approach could be setting the grace period to MAX_INT, and make the
  // health check pass iff a file created by the check exists. Alternatively,
  // we can relax the expectation that the check update is delivered first.
  v1::HealthCheck* healthCheckInfo = taskInfo.mutable_health_check();
  healthCheckInfo->set_type(v1::HealthCheck::COMMAND);
  healthCheckInfo->set_delay_seconds(1);
  healthCheckInfo->set_interval_seconds(interval);
  healthCheckInfo->mutable_command()->set_value("exit 0");

  launchTask(&mesos, offer, taskInfo);

  AWAIT_READY(updateTaskRunning);
  ASSERT_EQ(TASK_RUNNING, updateTaskRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateTaskRunning->status().task_id());

  acknowledge(&mesos, frameworkId, updateTaskRunning->status());

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_EQ(taskInfo.task_id(), checkResult.task_id());
  EXPECT_FALSE(checkResult.has_healthy());
  EXPECT_TRUE(checkResult.has_check_status());
  EXPECT_TRUE(checkResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResult);

  AWAIT_READY(updateHealthResult);
  const v1::TaskStatus& healthResult = updateHealthResult->status();

  ASSERT_EQ(TASK_RUNNING, healthResult.state());
  EXPECT_EQ(taskInfo.task_id(), healthResult.task_id());
  EXPECT_TRUE(healthResult.has_healthy());
  EXPECT_TRUE(healthResult.healthy());
  EXPECT_TRUE(healthResult.has_check_status());
  EXPECT_TRUE(healthResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, healthResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, healthResult);

  // Trigger implicit reconciliation.
  reconcile(&mesos, frameworkId, {});

  AWAIT_READY(updateImplicitReconciliation);
  const v1::TaskStatus& implicitReconciliation =
    updateImplicitReconciliation->status();

  ASSERT_EQ(TASK_RUNNING, implicitReconciliation.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_RECONCILIATION,
      implicitReconciliation.reason());
  EXPECT_EQ(taskInfo.task_id(), implicitReconciliation.task_id());
  EXPECT_TRUE(implicitReconciliation.has_healthy());
  EXPECT_TRUE(implicitReconciliation.healthy());
  EXPECT_TRUE(implicitReconciliation.has_check_status());
  EXPECT_TRUE(implicitReconciliation.check_status().command().has_exit_code());
  EXPECT_EQ(1, implicitReconciliation.check_status().command().exit_code());
}


// Verifies that an HTTP check is supported by the command executor and
// its status is delivered in a task status update.
//
// TODO(josephw): Enable this. Mesos builds its own `curl.exe`, since it
// can't rely on a package manager to get it. We need to make this test use
// that executable.
TEST_F_TEMP_DISABLED_ON_WINDOWS(CommandExecutorCheckTest, HTTPCheckDelivered)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<v1::scheduler::Event::Update> updateTaskRunning;
  Future<v1::scheduler::Event::Update> updateCheckResult;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  const uint16_t testPort = getFreePort().get();

  // Use `test-helper` to launch a simple HTTP
  // server to respond to HTTP checks.
  const string command = strings::format(
      "%s %s --ip=127.0.0.1 --port=%u",
      getTestHelperPath("test-helper"),
      HealthCheckTestHelper::NAME,
      testPort).get();

  v1::Resources resources =
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, command);

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::HTTP);
  checkInfo->mutable_http()->set_port(testPort);
  checkInfo->mutable_http()->set_path("/help");
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(0);

  launchTask(&mesos, offer, taskInfo);

  AWAIT_READY(updateTaskRunning);
  const v1::TaskStatus& taskRunning = updateTaskRunning->status();

  ASSERT_EQ(TASK_RUNNING, taskRunning.state());
  EXPECT_EQ(taskInfo.task_id(), taskRunning.task_id());
  EXPECT_TRUE(taskRunning.has_check_status());
  EXPECT_TRUE(taskRunning.check_status().has_http());
  EXPECT_FALSE(taskRunning.check_status().http().has_status_code());

  acknowledge(&mesos, frameworkId, taskRunning);

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_EQ(taskInfo.task_id(), checkResult.task_id());
  EXPECT_TRUE(checkResult.has_check_status());
  EXPECT_TRUE(checkResult.check_status().http().has_status_code());
  EXPECT_EQ(200, checkResult.check_status().http().status_code());
}


// TODO(alexr): Implement following tests for the docker executor once
// the docker executor supports checks.
//
// 1. COMMAND check with env var works, is delivered, and is reconciled
//    properly.
// 2. COMMAND check's status change is delivered. TODO(alexr): When check
//    mocking is available, ensure only status changes are delivered.
// 3. COMMAND check times out.
// 4. COMMAND check and health check do not shadow each other; upon
//    reconciliation both statuses are available.
// 5. HTTP check works and is delivered.


// These are check tests with the default executor.
class DefaultExecutorCheckTest : public CheckTest {};


// Verifies that a command check is supported by the default executor,
// its status is delivered in a task status update, and the last known
// status can be obtained during explicit and implicit reconciliation.
// Additionally ensures that the specified environment of the command
// check is honored.
//
// TODO(gkleiman): Check if this test works on Windows.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    DefaultExecutorCheckTest,
    CommandCheckDeliveredAndReconciled)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;

  Fetcher fetcher;

  // We have to explicitly create a `Containerizer` in non-local mode,
  // because `LaunchNestedContainerSession` (used by command checks)
  // tries to start a IO switchboard, which doesn't work in local mode yet.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  const v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);
  executorInfo.mutable_shutdown_grace_period()->set_nanoseconds(
      Seconds(10).ns());

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore teardown reconnections, see MESOS-6033.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<Event::Update> updateTaskRunning;
  Future<Event::Update> updateCheckResult;
  Future<Event::Update> updateExplicitReconciliation;
  Future<Event::Update> updateImplicitReconciliation;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillOnce(FutureArg<1>(&updateExplicitReconciliation))
    .WillOnce(FutureArg<1>(&updateImplicitReconciliation))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(10000));

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::COMMAND);
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(0);

  v1::CommandInfo* checkCommand =
    checkInfo->mutable_command()->mutable_command();
  checkCommand->set_value("exit $STATUS");

  v1::Environment::Variable* variable =
    checkCommand->mutable_environment()->add_variables();
  variable->set_name("STATUS");
  variable->set_value("1");

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  launchTaskGroup(&mesos, offer, executorInfo, taskGroup);

  AWAIT_READY(updateTaskRunning);
  const v1::TaskStatus& taskRunning = updateTaskRunning->status();

  ASSERT_EQ(TASK_RUNNING, taskRunning.state());
  EXPECT_EQ(taskInfo.task_id(), taskRunning.task_id());
  EXPECT_TRUE(taskRunning.has_check_status());
  EXPECT_TRUE(taskRunning.check_status().has_command());
  EXPECT_FALSE(taskRunning.check_status().command().has_exit_code());

  acknowledge(&mesos, frameworkId, taskRunning);

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_EQ(taskInfo.task_id(), checkResult.task_id());
  EXPECT_TRUE(checkResult.has_check_status());
  EXPECT_TRUE(checkResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResult);

  // Trigger explicit reconciliation.
  reconcile(
      &mesos,
      frameworkId,
      {std::make_pair(checkResult.task_id(), checkResult.agent_id())});

  AWAIT_READY(updateExplicitReconciliation);
  const v1::TaskStatus& explicitReconciliation =
    updateExplicitReconciliation->status();

  ASSERT_EQ(TASK_RUNNING, explicitReconciliation.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_RECONCILIATION,
      explicitReconciliation.reason());
  EXPECT_EQ(taskInfo.task_id(), explicitReconciliation.task_id());
  EXPECT_TRUE(explicitReconciliation.has_check_status());
  EXPECT_TRUE(explicitReconciliation.check_status().command().has_exit_code());
  EXPECT_EQ(1, explicitReconciliation.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, explicitReconciliation);

  // Trigger implicit reconciliation.
  reconcile(&mesos, frameworkId, {});

  AWAIT_READY(updateImplicitReconciliation);
  const v1::TaskStatus& implicitReconciliation =
    updateImplicitReconciliation->status();

  ASSERT_EQ(TASK_RUNNING, implicitReconciliation.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_RECONCILIATION,
      implicitReconciliation.reason());
  EXPECT_EQ(taskInfo.task_id(), implicitReconciliation.task_id());
  EXPECT_TRUE(implicitReconciliation.has_check_status());
  EXPECT_TRUE(implicitReconciliation.check_status().command().has_exit_code());
  EXPECT_EQ(1, implicitReconciliation.check_status().command().exit_code());

  // Cleanup all mesos launched containers.
  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);

  EXPECT_CALL(*scheduler, disconnected(_));

  teardown(&mesos, frameworkId);

  foreach (const ContainerID& containerId, containerIds.get()) {
    AWAIT_READY(containerizer->wait(containerId));
  }
}


// Verifies that a command check's status changes are delivered.
//
// TODO(alexr): When check mocking is available, ensure that *only*
// status changes are delivered.
//
// TODO(gkleiman): Check if this test works on Windows.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    DefaultExecutorCheckTest,
    CommandCheckStatusChange)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;

  Fetcher fetcher;

  // We have to explicitly create a `Containerizer` in non-local mode,
  // because `LaunchNestedContainerSession` (used by command checks)
  // tries to start a IO switchboard, which doesn't work in local mode yet.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  const v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);
  executorInfo.mutable_shutdown_grace_period()->set_nanoseconds(
      Seconds(10).ns());

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore teardown reconnections, see MESOS-6033.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<Event::Update> updateTaskRunning;
  Future<Event::Update> updateCheckResult;
  Future<Event::Update> updateCheckResultChanged;
  Future<Event::Update> updateCheckResultBack;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillOnce(FutureArg<1>(&updateCheckResultChanged))
    .WillOnce(FutureArg<1>(&updateCheckResultBack))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(10000));

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::COMMAND);
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(0);
  checkInfo->mutable_command()->mutable_command()->set_value(
      FLAPPING_CHECK_COMMAND(path::join(os::getcwd(), "XXXXXX")));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  launchTaskGroup(&mesos, offer, executorInfo, taskGroup);

  AWAIT_READY(updateTaskRunning);
  ASSERT_EQ(TASK_RUNNING, updateTaskRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateTaskRunning->status().task_id());

  acknowledge(&mesos, frameworkId, updateTaskRunning->status());

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_TRUE(checkResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResult);

  AWAIT_READY(updateCheckResultChanged);
  const v1::TaskStatus& checkResultChanged = updateCheckResultChanged->status();

  ASSERT_EQ(TASK_RUNNING, checkResultChanged.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResultChanged.reason());
  EXPECT_TRUE(checkResultChanged.check_status().command().has_exit_code());
  EXPECT_EQ(0, checkResultChanged.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResultChanged);

  AWAIT_READY(updateCheckResultBack);
  const v1::TaskStatus& checkResultBack = updateCheckResultBack->status();

  ASSERT_EQ(TASK_RUNNING, checkResultBack.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResultBack.reason());
  EXPECT_TRUE(checkResultBack.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResultBack.check_status().command().exit_code());

  // Cleanup all mesos launched containers.
  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);

  EXPECT_CALL(*scheduler, disconnected(_));

  teardown(&mesos, frameworkId);

  foreach (const ContainerID& containerId, containerIds.get()) {
    AWAIT_READY(containerizer->wait(containerId));
  }
}


// Verifies that when a command check times out after a successful check,
// an empty check status update is delivered.
//
// TODO(gkleiman): Check if this test works on Windows.
TEST_F_TEMP_DISABLED_ON_WINDOWS(DefaultExecutorCheckTest, CommandCheckTimeout)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;

  Fetcher fetcher;

  // We have to explicitly create a `Containerizer` in non-local mode,
  // because `LaunchNestedContainerSession` (used by command checks)
  // tries to start a IO switchboard, which doesn't work in local mode yet.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  const v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);
  executorInfo.mutable_shutdown_grace_period()->set_nanoseconds(
      Seconds(10).ns());

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore teardown reconnections, see MESOS-6033.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<Event::Update> updateTaskRunning;
  Future<Event::Update> updateCheckResult;
  Future<Event::Update> updateCheckResultTimeout;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillOnce(FutureArg<1>(&updateCheckResultTimeout))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(10000));

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::COMMAND);
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(0);
  checkInfo->set_timeout_seconds(1);
  checkInfo->mutable_command()->mutable_command()->set_value(
      STALLING_CHECK_COMMAND(path::join(os::getcwd(), "XXXXXX")));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  launchTaskGroup(&mesos, offer, executorInfo, taskGroup);

  AWAIT_READY(updateTaskRunning);
  ASSERT_EQ(TASK_RUNNING, updateTaskRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateTaskRunning->status().task_id());

  acknowledge(&mesos, frameworkId, updateTaskRunning->status());

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_TRUE(checkResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResult);

  AWAIT_READY(updateCheckResultTimeout);
  const v1::TaskStatus& checkResultTimeout = updateCheckResultTimeout->status();

  ASSERT_EQ(TASK_RUNNING, checkResultTimeout.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResultTimeout.reason());
  EXPECT_FALSE(checkResultTimeout.check_status().command().has_exit_code());

  // Cleanup all mesos launched containers.
  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);

  EXPECT_CALL(*scheduler, disconnected(_));

  teardown(&mesos, frameworkId);

  foreach (const ContainerID& containerId, containerIds.get()) {
    AWAIT_READY(containerizer->wait(containerId));
  }
}


// Verifies that when both command check and health check are specified,
// health and check updates include both statuses. Also verifies that
// both statuses are included upon reconciliation.
//
// TODO(gkleiman): Check if this test works on Windows.
TEST_F(DefaultExecutorCheckTest, CommandCheckAndHealthCheckNoShadowing)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;

  Fetcher fetcher;

  // We have to explicitly create a `Containerizer` in non-local mode,
  // because `LaunchNestedContainerSession` (used by command checks)
  // tries to start a IO switchboard, which doesn't work in local mode yet.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  const v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);
  executorInfo.mutable_shutdown_grace_period()->set_nanoseconds(
      Seconds(10).ns());

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore teardown reconnections, see MESOS-6033.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<Event::Update> updateTaskRunning;
  Future<Event::Update> updateCheckResult;
  Future<Event::Update> updateHealthResult;
  Future<Event::Update> updateImplicitReconciliation;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillOnce(FutureArg<1>(&updateHealthResult))
    .WillOnce(FutureArg<1>(&updateImplicitReconciliation))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::TaskInfo taskInfo =
      v1::createTask(agentId, resources, SLEEP_COMMAND(10000));

  // Set both check and health check interval to an increased value to
  // prevent a second update coming before reconciliation response.
  int interval = 10;

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::COMMAND);
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(interval);
  checkInfo->mutable_command()->mutable_command()->set_value("exit 1");

  // Delay health check for 1s to ensure health update comes after check update.
  //
  // TODO(alexr): This can lead to flakiness on busy agents. A more robust
  // approach could be setting the grace period to MAX_INT, and make the
  // health check pass iff a file created by the check exists. Alternatively,
  // we can relax the expectation that the check update is delivered first.
  v1::HealthCheck* healthCheckInfo = taskInfo.mutable_health_check();
  healthCheckInfo->set_type(v1::HealthCheck::COMMAND);
  healthCheckInfo->set_delay_seconds(1);
  healthCheckInfo->set_interval_seconds(interval);
  healthCheckInfo->mutable_command()->set_value("exit 0");

  launchTask(&mesos, offer, taskInfo);

  AWAIT_READY(updateTaskRunning);
  ASSERT_EQ(TASK_RUNNING, updateTaskRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateTaskRunning->status().task_id());

  acknowledge(&mesos, frameworkId, updateTaskRunning->status());

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_EQ(taskInfo.task_id(), checkResult.task_id());
  EXPECT_FALSE(checkResult.has_healthy());
  EXPECT_TRUE(checkResult.has_check_status());
  EXPECT_TRUE(checkResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, checkResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, checkResult);

  AWAIT_READY(updateHealthResult);
  const v1::TaskStatus& healthResult = updateHealthResult->status();

  ASSERT_EQ(TASK_RUNNING, healthResult.state());
  EXPECT_EQ(taskInfo.task_id(), healthResult.task_id());
  EXPECT_TRUE(healthResult.has_healthy());
  EXPECT_TRUE(healthResult.healthy());
  EXPECT_TRUE(healthResult.has_check_status());
  EXPECT_TRUE(healthResult.check_status().command().has_exit_code());
  EXPECT_EQ(1, healthResult.check_status().command().exit_code());

  acknowledge(&mesos, frameworkId, healthResult);

  // Trigger implicit reconciliation.
  reconcile(&mesos, frameworkId, {});

  AWAIT_READY(updateImplicitReconciliation);
  const v1::TaskStatus& implicitReconciliation =
    updateImplicitReconciliation->status();

  ASSERT_EQ(TASK_RUNNING, implicitReconciliation.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_RECONCILIATION,
      implicitReconciliation.reason());
  EXPECT_EQ(taskInfo.task_id(), implicitReconciliation.task_id());
  EXPECT_TRUE(implicitReconciliation.has_healthy());
  EXPECT_TRUE(implicitReconciliation.healthy());
  EXPECT_TRUE(implicitReconciliation.has_check_status());
  EXPECT_TRUE(implicitReconciliation.check_status().command().has_exit_code());
  EXPECT_EQ(1, implicitReconciliation.check_status().command().exit_code());

  // Cleanup all mesos launched containers.
  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);

  EXPECT_CALL(*scheduler, disconnected(_));

  teardown(&mesos, frameworkId);

  foreach (const ContainerID& containerId, containerIds.get()) {
    AWAIT_READY(containerizer->wait(containerId));
  }
}


// Verifies that an HTTP check is supported by the default executor and
// its status is delivered in a task status update.
//
// TODO(josephw): Enable this. Mesos builds its own `curl.exe`, since it
// can't rely on a package manager to get it. We need to make this test use
// that executable.
TEST_F_TEMP_DISABLED_ON_WINDOWS(DefaultExecutorCheckTest, HTTPCheckDelivered)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), flags);
  ASSERT_SOME(agent);

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  const v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  subscribe(&mesos, frameworkInfo);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());
  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  Future<v1::scheduler::Event::Update> updateTaskRunning;
  Future<v1::scheduler::Event::Update> updateCheckResult;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateTaskRunning))
    .WillOnce(FutureArg<1>(&updateCheckResult))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  const uint16_t testPort = getFreePort().get();

  // Use `test-helper` to launch a simple HTTP
  // server to respond to HTTP checks.
  const string command = strings::format(
      "%s %s --ip=127.0.0.1 --port=%u",
      getTestHelperPath("test-helper"),
      HealthCheckTestHelper::NAME,
      testPort).get();

  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, command);

  v1::CheckInfo* checkInfo = taskInfo.mutable_check();
  checkInfo->set_type(v1::CheckInfo::HTTP);
  checkInfo->mutable_http()->set_port(testPort);
  checkInfo->mutable_http()->set_path("/help");
  checkInfo->set_delay_seconds(0);
  checkInfo->set_interval_seconds(0);

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  launchTaskGroup(&mesos, offer, executorInfo, taskGroup);

  AWAIT_READY(updateTaskRunning);
  const v1::TaskStatus& taskRunning = updateTaskRunning->status();

  ASSERT_EQ(TASK_RUNNING, taskRunning.state());
  EXPECT_EQ(taskInfo.task_id(), taskRunning.task_id());
  EXPECT_TRUE(taskRunning.has_check_status());
  EXPECT_TRUE(taskRunning.check_status().has_http());
  EXPECT_FALSE(taskRunning.check_status().http().has_status_code());

  // Acknowledge (to be able to get the next update).
  acknowledge(&mesos, frameworkId, taskRunning);

  AWAIT_READY(updateCheckResult);
  const v1::TaskStatus& checkResult = updateCheckResult->status();

  ASSERT_EQ(TASK_RUNNING, checkResult.state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
      checkResult.reason());
  EXPECT_EQ(taskInfo.task_id(), checkResult.task_id());
  EXPECT_TRUE(checkResult.has_check_status());
  EXPECT_TRUE(checkResult.check_status().http().has_status_code());
  EXPECT_EQ(200, checkResult.check_status().http().status_code());
}


// These are protobuf validation tests.
//
// TODO(alexr): Move these tests once validation code is moved closer to
// protobuf definitions.

// This tests ensures `CheckInfo` protobuf is validated correctly.
TEST_F(CheckTest, CheckInfoValidation)
{
  using namespace mesos::internal::checks;

  // Check type must be set to a known value.
  {
    CheckInfo checkInfo;

    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("CheckInfo must specify 'type'", validate->message);

    checkInfo.set_type(CheckInfo::UNKNOWN);
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("'UNKNOWN' is not a valid check type", validate->message);
  }

  // The associated message for a given type must be set.
  {
    CheckInfo checkInfo;

    checkInfo.set_type(CheckInfo::COMMAND);
    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'command' to be set for COMMAND check",
        validate->message);

    checkInfo.set_type(CheckInfo::HTTP);
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'http' to be set for HTTP check",
        validate->message);
  }

  // Command check must specify an actual command in `command.command.value`.
  {
    CheckInfo checkInfo;

    checkInfo.set_type(CheckInfo::COMMAND);
    checkInfo.mutable_command()->CopyFrom(CheckInfo::Command());
    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("Command check must contain 'shell command'", validate->message);

    checkInfo.mutable_command()->mutable_command()->CopyFrom(CommandInfo());
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("Command check must contain 'shell command'", validate->message);
  }

  // Command check must specify a command with a valid environment.
  // Environment variable's `value` field must be set in this case.
  {
    CheckInfo checkInfo;
    checkInfo.set_type(CheckInfo::COMMAND);
    checkInfo.mutable_command()->CopyFrom(CheckInfo::Command());
    checkInfo.mutable_command()->mutable_command()->CopyFrom(
        createCommandInfo("exit 0"));

    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_NONE(validate);

    Environment::Variable* variable =
      checkInfo.mutable_command()->mutable_command()->mutable_environment()
          ->mutable_variables()->Add();
    variable->set_name("ENV_VAR_KEY");

    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
  }

  // HTTP check may specify a path starting with '/'.
  {
    CheckInfo checkInfo;

    checkInfo.set_type(CheckInfo::HTTP);
    checkInfo.mutable_http()->set_port(8080);

    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_NONE(validate);

    checkInfo.mutable_http()->set_path("healthz");
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "The path 'healthz' of HTTP check must start with '/'",
        validate->message);
  }

  // Check's duration parameters must be non-negative.
  {
    CheckInfo checkInfo;

    checkInfo.set_type(CheckInfo::HTTP);
    checkInfo.mutable_http()->set_port(8080);

    checkInfo.set_delay_seconds(-1.0);
    Option<Error> validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'delay_seconds' to be non-negative",
        validate->message);

    checkInfo.set_delay_seconds(0.0);
    checkInfo.set_interval_seconds(-1.0);
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'interval_seconds' to be non-negative",
        validate->message);

    checkInfo.set_interval_seconds(0.0);
    checkInfo.set_timeout_seconds(-1.0);
    validate = validation::checkInfo(checkInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'timeout_seconds' to be non-negative",
        validate->message);

    checkInfo.set_timeout_seconds(0.0);
    validate = validation::checkInfo(checkInfo);
    EXPECT_NONE(validate);
  }
}


// This tests ensures `CheckStatusInfo` protobuf is validated correctly.
TEST_F(CheckTest, CheckStatusInfoValidation)
{
  using namespace mesos::internal::checks;

  // Check status type must be set to a known value.
  {
    CheckStatusInfo checkStatusInfo;

    Option<Error> validate = validation::checkStatusInfo(checkStatusInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ("CheckStatusInfo must specify 'type'", validate->message);

    checkStatusInfo.set_type(CheckInfo::UNKNOWN);
    validate = validation::checkStatusInfo(checkStatusInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "'UNKNOWN' is not a valid check's status type",
        validate->message);
  }

  // The associated message for a given type must be set.
  {
    CheckStatusInfo checkStatusInfo;

    checkStatusInfo.set_type(CheckInfo::COMMAND);
    Option<Error> validate = validation::checkStatusInfo(checkStatusInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'command' to be set for COMMAND check's status",
        validate->message);

    checkStatusInfo.set_type(CheckInfo::HTTP);
    validate = validation::checkStatusInfo(checkStatusInfo);
    EXPECT_SOME(validate);
    EXPECT_EQ(
        "Expecting 'http' to be set for HTTP check's status",
        validate->message);
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
