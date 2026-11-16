/**
 * @file SourceControllerTest.cpp
 * @brief Drives OneGen Single and Live through the source controller against the synthetic producer.
 *
 * Each action must write AcquisitionMode before it starts: Single selects SingleFrame and
 * delivers one converted frame, Live selects Continuous. An unwritable mode is refused with
 * an explicit reason instead of being emulated by stopping a continuous stream.
 */
#include "OneGenSourceController.h"
#include "ProducerCoordinator.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace {

bool waitFor(const std::function<bool()>& predicate, const int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return predicate();
}

} // namespace

class OneGenSourceControllerTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void singleAndLiveFollowTheSelectedAction();
    void unwritableModeRefusesSingleAndLive();
    void cleanupTestCase();

private:
    DeviceConnectionResult invoke(const std::function<DeviceConnectionResult(IDeviceConnectionControl::Completion)>& admit);
    DeviceConnectionTargets discover();
    void connectTarget();
    DeviceParameterInventory inventory();
    std::string readAcquisitionMode();
    void disconnect();

    std::shared_ptr<onegen::producer::Coordinator> _coordinator;
    std::unique_ptr<OneGenSourceController> _controller;
    QString _connectionId;
    std::atomic<int> _frames{0};
};

void OneGenSourceControllerTests::initTestCase()
{
    _coordinator = std::make_shared<onegen::producer::Coordinator>();
    QVERIFY(_coordinator->set_producer_files({ONEGEN_TEST_PRODUCER}));
    _controller = std::make_unique<OneGenSourceController>();
    _controller->configureControl(_coordinator, {});
    _controller->setFrameConsumer([this](SessionFrame&&, unsigned) { _frames.fetch_add(1); });
}

DeviceConnectionResult OneGenSourceControllerTests::invoke(
    const std::function<DeviceConnectionResult(IDeviceConnectionControl::Completion)>& admit)
{
    auto promise = std::make_shared<std::promise<DeviceConnectionResult>>();
    auto future = promise->get_future();
    const auto admitted = admit([promise](DeviceConnectionResult result) {
        promise->set_value(std::move(result));
    });
    if (admitted.error != DeviceConnectionError::None) return admitted;
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
        return {DeviceConnectionError::Timeout, "Lifecycle completion timed out."};
    return future.get();
}

DeviceConnectionTargets OneGenSourceControllerTests::discover()
{
    auto promise = std::make_shared<std::promise<DeviceConnectionTargets>>();
    auto future = promise->get_future();
    const auto admitted = _controller->requestDiscover([promise](DeviceConnectionTargets result) {
        promise->set_value(std::move(result));
    });
    if (admitted.error != DeviceConnectionError::None) return {{admitted.error, admitted.message}, {}};
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
        return {{DeviceConnectionError::Timeout, "Discovery completion timed out."}, {}};
    return future.get();
}

void OneGenSourceControllerTests::connectTarget()
{
    const auto targets = discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    QVERIFY(!targets.targets.empty());
    const auto connected = invoke([&](auto done) {
        return _controller->requestConnect(targets.targets.front().targetId, done);
    });
    QVERIFY2(connected.error == DeviceConnectionError::None, connected.message.c_str());
    QVERIFY(connected.state && connected.state->connected);
    _connectionId = QString::fromStdString(connected.state->connectionId);
}

DeviceParameterInventory OneGenSourceControllerTests::inventory()
{
    auto promise = std::make_shared<std::promise<DeviceParameterInventory>>();
    auto future = promise->get_future();
    const auto admitted = _controller->requestParameters([promise](DeviceParameterInventory value) {
        promise->set_value(std::move(value));
    });
    if (admitted.error != DeviceParameterError::None) return {{admitted.error, admitted.message}, {}};
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
        return {{DeviceParameterError::BackendFailure, "Parameter inventory timed out."}, {}};
    return future.get();
}

std::string OneGenSourceControllerTests::readAcquisitionMode()
{
    const auto listed = inventory();
    if (listed.result.error != DeviceParameterError::None) return {};
    const auto found = std::find_if(listed.parameters.begin(), listed.parameters.end(),
        [](const auto& parameter) { return parameter.target.name == "AcquisitionMode"; });
    if (found == listed.parameters.end()) return {};
    auto promise = std::make_shared<std::promise<DeviceParameterResult>>();
    auto future = promise->get_future();
    const auto admitted = _controller->requestReadParameter(found->target,
        [promise](DeviceParameterResult value) { promise->set_value(std::move(value)); });
    if (admitted.error != DeviceParameterError::None) return {};
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) return {};
    const auto read = future.get();
    if (read.error != DeviceParameterError::None || !read.descriptor || !read.descriptor->value) return {};
    return std::get<std::string>(*read.descriptor->value);
}

void OneGenSourceControllerTests::disconnect()
{
    const auto disconnected = invoke([&](auto done) {
        return _controller->requestDisconnect(_connectionId.toStdString(), done);
    });
    QCOMPARE(disconnected.error, DeviceConnectionError::None);
}

void OneGenSourceControllerTests::singleAndLiveFollowTheSelectedAction()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");

    const auto disconnected = _controller->connectionCapabilities();
    QVERIFY(!disconnected.single.available);
    QVERIFY(!disconnected.single.reason.empty());

    connectTarget();
    const auto connected = _controller->connectionCapabilities();
    QVERIFY2(connected.single.available, connected.single.reason.c_str());

    _frames.store(0);
    auto single = invoke([&](auto done) {
        return _controller->requestSingle(_connectionId.toStdString(), done);
    });
    QVERIFY2(single.error == DeviceConnectionError::None, single.message.c_str());
    QCOMPARE(_frames.load(), 1);
    QCOMPARE(readAcquisitionMode(), std::string("SingleFrame"));

    _frames.store(0);
    auto live = invoke([&](auto done) {
        return _controller->requestLiveStart(_connectionId.toStdString(), done);
    });
    QVERIFY2(live.error == DeviceConnectionError::None, live.message.c_str());
    QVERIFY(waitFor([this] { return _frames.load() >= 2; }, 30000));
    QCOMPARE(readAcquisitionMode(), std::string("Continuous"));
    auto stopped = invoke([&](auto done) {
        return _controller->requestLiveStop(_connectionId.toStdString(), done);
    });
    QVERIFY2(stopped.error == DeviceConnectionError::None, stopped.message.c_str());

    _frames.store(0);
    single = invoke([&](auto done) {
        return _controller->requestSingle(_connectionId.toStdString(), done);
    });
    QVERIFY2(single.error == DeviceConnectionError::None, single.message.c_str());
    QCOMPARE(_frames.load(), 1);
    QCOMPARE(readAcquisitionMode(), std::string("SingleFrame"));

    QCOMPARE(invoke([&](auto done) {
        return _controller->requestLiveStart(_connectionId.toStdString(), done);
    }).error, DeviceConnectionError::None);
    const auto refused = invoke([&](auto done) {
        return _controller->requestSingle(_connectionId.toStdString(), done);
    });
    QCOMPARE(refused.error, DeviceConnectionError::Busy);
    QVERIFY(!refused.message.empty());
    QCOMPARE(invoke([&](auto done) {
        return _controller->requestLiveStop(_connectionId.toStdString(), done);
    }).error, DeviceConnectionError::None);

    disconnect();
}

void OneGenSourceControllerTests::unwritableModeRefusesSingleAndLive()
{
    if (!qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The writable-mode run covers applied acquisition modes.");

    connectTarget();
    const auto capabilities = _controller->connectionCapabilities();
    QVERIFY(!capabilities.single.available);
    QVERIFY(!capabilities.single.reason.empty());
    _frames.store(0);
    const auto single = invoke([&](auto done) {
        return _controller->requestSingle(_connectionId.toStdString(), done);
    });
    QCOMPARE(single.error, DeviceConnectionError::Unavailable);
    QVERIFY(!single.message.empty());
    const auto live = invoke([&](auto done) {
        return _controller->requestLiveStart(_connectionId.toStdString(), done);
    });
    QCOMPARE(live.error, DeviceConnectionError::Unavailable);
    QVERIFY(!live.message.empty());
    QCOMPARE(_frames.load(), 0);
    disconnect();
}

void OneGenSourceControllerTests::cleanupTestCase()
{
    _controller.reset();
    if (_coordinator) _coordinator->shutdown();
}

QTEST_GUILESS_MAIN(OneGenSourceControllerTests)
#include "SourceControllerTest.moc"
