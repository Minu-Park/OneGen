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
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

/** @brief Restores one environment variable when the test leaves its scope. */
class EnvironmentVariable {
public:
    EnvironmentVariable(const char* name, const char* value) : _name(name)
    {
        _hadValue = qEnvironmentVariableIsSet(name);
        _previous = qgetenv(name);
        qputenv(name, value);
    }
    ~EnvironmentVariable()
    {
        if (_hadValue) qputenv(_name, _previous);
        else qunsetenv(_name);
    }
    EnvironmentVariable(const EnvironmentVariable&) = delete;
    EnvironmentVariable& operator=(const EnvironmentVariable&) = delete;

private:
    QByteArray _name;
    QByteArray _previous;
    bool _hadValue = false;
};

struct Fixture {
    Fixture(const std::function<std::shared_ptr<void>(const onegen::InterfaceInfo&)>& interfaceClaim = {},
        const std::function<std::shared_ptr<void>(const std::string&, std::string&)>& cameraClaim = {})
    {
        _coordinator = std::make_shared<onegen::producer::Coordinator>(
            std::filesystem::path{}, std::chrono::milliseconds{10000}, interfaceClaim);
        if (!_coordinator->set_producer_files({ONEGEN_TEST_PRODUCER}))
            throw std::runtime_error("The synthetic producer could not be selected.");
        _controller = std::make_unique<OneGenSourceController>();
        _controller->configureControl(_coordinator, {});
        _controller->setResourceClaim(cameraClaim);
        _controller->setFrameConsumer([this](SessionFrame&&, unsigned int index) {
            _frames.fetch_add(1);
            std::lock_guard lock(_framesMutex);
            ++_framesByIndex[index];
        });
    }
    ~Fixture()
    {
        _controller.reset();
        _coordinator->shutdown();
    }
    DeviceConnectionTargets discover()
    {
        auto promise = std::make_shared<std::promise<DeviceConnectionTargets>>();
        auto future = promise->get_future();
        const auto admitted = _controller->requestDiscover([promise](DeviceConnectionTargets result) {
            promise->set_value(std::move(result));
        });
        if (admitted.error != DeviceConnectionError::None)
            return {{admitted.error, admitted.message}, {}};
        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
            return {{DeviceConnectionError::Timeout, "Discovery completion timed out."}, {}};
        return future.get();
    }
    DeviceConnectionResult invoke(
        const std::function<DeviceConnectionResult(IDeviceConnectionControl::Completion)>& admit,
        std::future<DeviceConnectionResult>* pending = nullptr)
    {
        auto promise = std::make_shared<std::promise<DeviceConnectionResult>>();
        auto future = promise->get_future();
        const auto admitted = admit([promise](DeviceConnectionResult result) {
            promise->set_value(std::move(result));
        });
        if (admitted.error != DeviceConnectionError::None) return admitted;
        if (pending) {
            *pending = std::move(future);
            return {};
        }
        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
            return {DeviceConnectionError::Timeout, "Lifecycle completion timed out."};
        return future.get();
    }
    DeviceConnectionResult connect(const std::string& target)
    {
        return invoke([&](auto done) { return _controller->requestConnect(target, done); });
    }
    DeviceConnectionResult disconnect(const std::string& connection)
    {
        return invoke([&](auto done) { return _controller->requestDisconnect(connection, done); });
    }
    std::vector<DeviceConnectionState> states() const
    {
        auto promise = std::make_shared<std::promise<DeviceConnectionStates>>();
        auto future = promise->get_future();
        const auto admitted = _controller->requestConnectionStates(
            [promise](DeviceConnectionStates value) { promise->set_value(std::move(value)); });
        if (admitted.error != DeviceConnectionError::None) return {};
        if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) return {};
        return future.get().connections;
    }
    int frames() const { return _frames.load(); }
    int framesFor(const unsigned index) const
    {
        std::lock_guard lock(_framesMutex);
        return _framesByIndex.count(index) ? _framesByIndex.at(index) : 0;
    }
    std::shared_ptr<onegen::producer::Coordinator> _coordinator;
    std::unique_ptr<OneGenSourceController> _controller;
    std::atomic<int> _frames{0};
    mutable std::mutex _framesMutex;
    std::map<unsigned, int> _framesByIndex;
};

const DeviceConnectionTarget* targetOfKind(const DeviceConnectionTargets& targets,
    const DeviceConnectionTargetKind kind, const std::size_t position)
{
    std::size_t seen = 0;
    for (const auto& target : targets.targets) {
        if (target.kind != kind) continue;
        if (seen++ == position) return &target;
    }
    return nullptr;
}

} // namespace

class OneGenSourceControllerTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void singleAndLiveFollowTheSelectedAction();
    void unwritableModeRefusesSingleAndLive();
    void severalCamerasShareOneInterface();
    void explicitInterfaceStaysOpenUntilDisconnect();
    void discoveryAnnotatesLossWithoutTeardown();
    void claimsFollowInterfaceType();
    void partialStartReportsFailures();
    void perConnectionAdmissionIsolation();
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

void OneGenSourceControllerTests::severalCamerasShareOneInterface()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "2");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto* interfaceTarget = targetOfKind(targets, DeviceConnectionTargetKind::Interface, 0);
    const auto* firstCameraTarget = targetOfKind(targets, DeviceConnectionTargetKind::Device, 0);
    const auto* secondCameraTarget = targetOfKind(targets, DeviceConnectionTargetKind::Device, 1);
    QVERIFY(interfaceTarget && firstCameraTarget && secondCameraTarget);
    QCOMPARE(QString::fromStdString(firstCameraTarget->parentTargetId),
        QString::fromStdString(interfaceTarget->targetId));
    QVERIFY(interfaceTarget->interfaceInfo.has_value());
    QCOMPARE(QString::fromStdString(interfaceTarget->interfaceInfo->type), QStringLiteral("CXP"));
    QVERIFY(interfaceTarget->interfaceInfo->exclusive);

    const auto first = fixture.connect(firstCameraTarget->targetId);
    QVERIFY2(first.error == DeviceConnectionError::None, first.message.c_str());
    QVERIFY(first.state && first.state->connected);
    QVERIFY(first.state->parentConnectionId.empty() == false);
    QCOMPARE(first.state->sources.size(), std::size_t(1));
    const auto second = fixture.connect(secondCameraTarget->targetId);
    QVERIFY2(second.error == DeviceConnectionError::None, second.message.c_str());
    const auto duplicate = fixture.connect(firstCameraTarget->targetId);
    QCOMPARE(duplicate.error, DeviceConnectionError::Conflict);

    auto states = fixture.states();
    QCOMPARE(states.size(), std::size_t(3));
    QCOMPARE(states.front().kind, DeviceConnectionTargetKind::Interface);
    QVERIFY(states.front().implicitlyOpened);
    QVERIFY(states.front().capabilities);
    QVERIFY(!states.front().capabilities->single.available);
    QVERIFY(states.front().parentConnectionId.empty());
    const auto interfaceConnection = states.front().connectionId;
    QCOMPARE(states.at(1).parentConnectionId, interfaceConnection);
    QCOMPARE(states.at(2).parentConnectionId, interfaceConnection);
    QCOMPARE(states.at(1).sources.at(0).sourceIndex + 1, states.at(2).sources.at(0).sourceIndex);
    const auto firstIndex = states.at(1).sources.at(0).sourceIndex;
    const auto secondIndex = states.at(2).sources.at(0).sourceIndex;

    auto single = fixture.invoke([&](auto done) {
        return fixture._controller->requestSingle(states.at(1).connectionId, done);
    });
    QVERIFY2(single.error == DeviceConnectionError::None, single.message.c_str());
    QVERIFY(fixture.framesFor(firstIndex) >= 1);
    auto live = fixture.invoke([&](auto done) {
        return fixture._controller->requestLiveStart(states.at(2).connectionId, done);
    });
    QVERIFY2(live.error == DeviceConnectionError::None, live.message.c_str());
    QVERIFY(waitFor([&] { return fixture.framesFor(secondIndex) >= 2; }, 30000));
    QVERIFY2(fixture.invoke([&](auto done) {
        return fixture._controller->requestLiveStop(states.at(2).connectionId, done);
    }).error == DeviceConnectionError::None, "The second camera did not stop.");

    fixture._controller->start();
    QVERIFY(fixture._controller->sourceOperationFailures().empty());
    QVERIFY(waitFor([&] {
        return fixture.framesFor(firstIndex) >= 2 && fixture.framesFor(secondIndex) >= 2;
    }, 30000));
    fixture._controller->stop();
    QVERIFY(fixture._controller->sourceOperationFailures().empty());

    QVERIFY2(fixture.disconnect(states.at(1).connectionId).error == DeviceConnectionError::None,
        "The first camera did not disconnect.");
    states = fixture.states();
    QCOMPARE(states.size(), std::size_t(2));
    QVERIFY2(fixture.disconnect(states.at(1).connectionId).error == DeviceConnectionError::None,
        "The last camera did not disconnect.");
    QVERIFY(fixture.states().empty());
    QCOMPARE(fixture._controller->connectionCapabilities().connectionModel,
        DeviceConnectionModel::Hierarchical);
}

void OneGenSourceControllerTests::explicitInterfaceStaysOpenUntilDisconnect()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "2");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto* interfaceTarget = targetOfKind(targets, DeviceConnectionTargetKind::Interface, 0);
    QVERIFY(interfaceTarget);
    const auto opened = fixture.connect(interfaceTarget->targetId);
    QVERIFY2(opened.error == DeviceConnectionError::None, opened.message.c_str());
    QVERIFY(opened.state && opened.state->kind == DeviceConnectionTargetKind::Interface);
    QVERIFY(!opened.state->implicitlyOpened);
    QVERIFY(opened.state->capabilities && !opened.state->capabilities->live.available);
    const auto interfaceConnection = opened.state->connectionId;
    const auto firstCamera = fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId);
    QVERIFY2(firstCamera.error == DeviceConnectionError::None, firstCamera.message.c_str());
    const auto secondCamera = fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 1)->targetId);
    QVERIFY2(secondCamera.error == DeviceConnectionError::None, secondCamera.message.c_str());
    QCOMPARE(firstCamera.state->parentConnectionId, interfaceConnection);

    QVERIFY2(fixture.disconnect(firstCamera.state->connectionId).error == DeviceConnectionError::None,
        "The first camera did not disconnect.");
    QVERIFY2(fixture.disconnect(secondCamera.state->connectionId).error == DeviceConnectionError::None,
        "The second camera did not disconnect.");
    auto states = fixture.states();
    QCOMPARE(states.size(), std::size_t(1));
    QCOMPARE(states.front().connectionId, interfaceConnection);
    QVERIFY(!states.front().implicitlyOpened);
    QVERIFY2(fixture.disconnect(interfaceConnection).error == DeviceConnectionError::None,
        "The explicit Interface did not disconnect.");
    QVERIFY(fixture.states().empty());
}

void OneGenSourceControllerTests::discoveryAnnotatesLossWithoutTeardown()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "2");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId).error
        == DeviceConnectionError::None, "The first camera did not connect.");
    QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 1)->targetId).error
        == DeviceConnectionError::None, "The second camera did not connect.");
    auto states = fixture.states();
    QCOMPARE(states.size(), std::size_t(3));
    const auto firstIndex = states.at(1).sources.at(0).sourceIndex;
    const auto secondIndex = states.at(2).sources.at(0).sourceIndex;
    fixture._controller->start();
    QVERIFY(waitFor([&] {
        return fixture.framesFor(firstIndex) >= 2 && fixture.framesFor(secondIndex) >= 2;
    }, 30000));

    qputenv("ONEGEN_SYNTHETIC_HIDDEN_DEVICES", "1");
    QCOMPARE(fixture.discover().result.error, DeviceConnectionError::None);
    states = fixture.states();
    QCOMPARE(states.size(), std::size_t(3));
    QVERIFY(states.at(1).connected);
    QVERIFY(states.at(1).error.empty());
    QVERIFY(states.at(2).connected);
    QVERIFY(states.at(2).error.find("not discovered") != std::string::npos);
    const auto before = fixture.framesFor(firstIndex);
    QVERIFY(waitFor([&] { return fixture.framesFor(firstIndex) > before; }, 30000));

    qunsetenv("ONEGEN_SYNTHETIC_HIDDEN_DEVICES");
    QCOMPARE(fixture.discover().result.error, DeviceConnectionError::None);
    states = fixture.states();
    QVERIFY(states.at(2).error.empty());

    qputenv("ONEGEN_SYNTHETIC_HIDDEN_INTERFACE", "1");
    QCOMPARE(fixture.discover().result.error, DeviceConnectionError::None);
    states = fixture.states();
    QCOMPARE(states.size(), std::size_t(3));
    QCOMPARE(states.front().kind, DeviceConnectionTargetKind::Interface);
    QVERIFY(!states.front().error.empty());
    QVERIFY(states.at(1).connected);
    QVERIFY(states.at(1).error.find("not discovered") != std::string::npos);
    QVERIFY(states.at(2).error.find("not discovered") != std::string::npos);
    qunsetenv("ONEGEN_SYNTHETIC_HIDDEN_INTERFACE");
    QCOMPARE(fixture.discover().result.error, DeviceConnectionError::None);
    QVERIFY(fixture.states().front().error.empty());
    fixture._controller->stop();
}

void OneGenSourceControllerTests::claimsFollowInterfaceType()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    {
        EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
        EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
        std::vector<std::string> interfaceClaims;
        std::vector<std::string> cameraClaims;
        Fixture fixture(
            [&](const onegen::InterfaceInfo& info) {
                interfaceClaims.push_back(info.id);
                return std::shared_ptr<void>(new int(1), [](void* value) { delete static_cast<int*>(value); });
            },
            [&](const std::string& key, std::string&) {
                cameraClaims.push_back(key);
                return std::shared_ptr<void>(new int(1), [](void* value) { delete static_cast<int*>(value); });
            });
        const auto targets = fixture.discover();
        QCOMPARE(targets.result.error, DeviceConnectionError::None);
        const auto baseline = interfaceClaims.size();
        QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId).error
            == DeviceConnectionError::None, "The frame-grabber camera did not connect.");
        QVERIFY(interfaceClaims.size() > baseline);
        QVERIFY(cameraClaims.empty());
    }
    {
        EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
        EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "GEV");
        std::vector<std::string> interfaceClaims;
        std::vector<std::string> cameraClaims;
        Fixture fixture(
            [&](const onegen::InterfaceInfo& info) {
                interfaceClaims.push_back(info.id);
                return std::shared_ptr<void>(new int(1), [](void* value) { delete static_cast<int*>(value); });
            },
            [&](const std::string& key, std::string&) {
                cameraClaims.push_back(key);
                return std::shared_ptr<void>(new int(1), [](void* value) { delete static_cast<int*>(value); });
            });
        const auto targets = fixture.discover();
        QCOMPARE(targets.result.error, DeviceConnectionError::None);
        QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId).error
            == DeviceConnectionError::None, "The network camera did not connect.");
        QVERIFY(interfaceClaims.empty());
        QCOMPARE(cameraClaims.size(), std::size_t(1));
        QVERIFY(cameraClaims.front().rfind("onegen-camera:", 0) == 0);
    }
}

void OneGenSourceControllerTests::partialStartReportsFailures()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "2");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId).error
        == DeviceConnectionError::None, "The first camera did not connect.");
    QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 1)->targetId).error
        == DeviceConnectionError::None, "The second camera did not connect.");
    auto states = fixture.states();
    const auto firstIndex = states.at(1).sources.at(0).sourceIndex;
    const auto secondIndex = states.at(2).sources.at(0).sourceIndex;

    EnvironmentVariable failure("ONEGEN_SYNTHETIC_START_FAILURE", "1");
    fixture._controller->start();
    const auto failures = fixture._controller->sourceOperationFailures();
    QCOMPARE(failures.size(), std::size_t(1));
    QCOMPARE(failures.front().sourceIndex, secondIndex);
    QCOMPARE(failures.front().sourceId, states.at(2).sources.at(0).sourceId);
    QVERIFY(waitFor([&] { return fixture.framesFor(firstIndex) >= 2; }, 30000));
    QCOMPARE(fixture.framesFor(secondIndex), 0);
    fixture._controller->stop();
    QVERIFY(fixture._controller->sourceOperationFailures().empty());
}

void OneGenSourceControllerTests::perConnectionAdmissionIsolation()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "2");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId).error
        == DeviceConnectionError::None, "The first camera did not connect.");
    QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 1)->targetId).error
        == DeviceConnectionError::None, "The second camera did not connect.");
    const auto states = fixture.states();
    EnvironmentVariable delay("ONEGEN_SYNTHETIC_ACQUISITION_DELAY_MS", "400");
    std::future<DeviceConnectionResult> first;
    QCOMPARE(fixture.invoke([&](auto done) {
        return fixture._controller->requestSingle(states.at(1).connectionId, done);
    }, &first).error, DeviceConnectionError::None);
    std::future<DeviceConnectionResult> second;
    const auto admitted = fixture.invoke([&](auto done) {
        return fixture._controller->requestSingle(states.at(2).connectionId, done);
    }, &second);
    QVERIFY2(admitted.error == DeviceConnectionError::None,
        "One camera's operation parked another connection's admission.");
    const auto structural = fixture._controller->requestDiscover([](DeviceConnectionTargets) {});
    QCOMPARE(structural.error, DeviceConnectionError::Busy);
    QCOMPARE(first.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    QCOMPARE(second.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    QCOMPARE(first.get().error, DeviceConnectionError::None);
    QCOMPARE(second.get().error, DeviceConnectionError::None);
    QVERIFY(fixture.framesFor(states.at(1).sources.at(0).sourceIndex) >= 1);
    QVERIFY(fixture.framesFor(states.at(2).sources.at(0).sourceIndex) >= 1);
}

void OneGenSourceControllerTests::cleanupTestCase()
{
    _controller.reset();
    if (_coordinator) _coordinator->shutdown();
}

QTEST_GUILESS_MAIN(OneGenSourceControllerTests)
#include "SourceControllerTest.moc"
