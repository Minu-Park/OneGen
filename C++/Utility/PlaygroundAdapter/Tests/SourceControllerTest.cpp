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
#include "DeviceRuntime/Contract/HardwareResourceKey.h"
#include "Tools/ToolProvider.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QScopeGuard>
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

std::mutex imageRejectionMutex;
std::vector<DeviceImageRejectionObservation> imageRejections;

void rememberImageRejection(const DeviceImageRejectionObservation& observation) noexcept
{
    try {
        std::lock_guard lock(imageRejectionMutex);
        imageRejections.push_back(observation);
    } catch (...) {}
}

std::atomic<int> firstInputLogs{0};
std::atomic<int> firstConversionLogs{0};

void countAcquisitionDetails(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type != QtDebugMsg || QLatin1String(context.category) != QLatin1String("playground.device.acquisition.details")) return;
    if (message.startsWith(QLatin1String("[GenICam] first input:"))) ++firstInputLogs;
    if (message.startsWith(QLatin1String("[GenICam] first frame converted:"))) ++firstConversionLogs;
}

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
        const std::function<std::shared_ptr<void>(const onegen::InterfaceInfo&, const onegen::DeviceInfo&)>&
            cameraClaim = {})
    {
        _coordinator = std::make_shared<onegen::producer::Coordinator>(
            std::filesystem::path{}, std::chrono::milliseconds{10000}, interfaceClaim, cameraClaim);
        if (!_coordinator->set_producer_files({ONEGEN_TEST_PRODUCER}))
            throw std::runtime_error("The synthetic producer could not be selected.");
        _controller = std::make_unique<OneGenSourceController>();
        _controller->configureControl(_coordinator, {});
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
    DeviceParameterInventory inventory()
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
    DeviceParameterResult readParameter(const DeviceParameterTarget& target)
    {
        auto promise = std::make_shared<std::promise<DeviceParameterResult>>();
        auto future = promise->get_future();
        const auto admitted = _controller->requestReadParameter(target,
            [promise](DeviceParameterResult value) { promise->set_value(std::move(value)); });
        if (admitted.error != DeviceParameterError::None) return admitted;
        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
            return {DeviceParameterError::BackendFailure, "Parameter read timed out.",
                    DeviceParameterEffect::None, std::nullopt};
        return future.get();
    }
    DeviceParameterResult writeParameter(const DeviceParameterTarget& target,
        const DeviceParameterValue& value, const std::optional<std::uint64_t> revision = {})
    {
        auto promise = std::make_shared<std::promise<DeviceParameterResult>>();
        auto future = promise->get_future();
        const auto admitted = _controller->requestWriteParameter(target, value, revision,
            [promise](DeviceParameterResult result) { promise->set_value(std::move(result)); });
        if (admitted.error != DeviceParameterError::None) return admitted;
        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
            return {DeviceParameterError::BackendFailure, "Parameter write timed out.",
                    DeviceParameterEffect::None, std::nullopt};
        return future.get();
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
    void singleWithUnusableImageReportsAssemblyFailure();
    void firstInputDetailsFollowCategory();
    void unwritableModeRefusesSingleAndLive();
    void severalCamerasShareOneInterface();
    void explicitInterfaceStaysOpenUntilDisconnect();
    void discoveryAnnotatesLossWithoutTeardown();
    void pathChangeUsesSessionStateAndRefreshesInventory();
    void claimsFollowInterfaceType();
    void claimDenialRetiresTheCameraConnection();
    void interfaceFeaturesFallBackWithoutIfGetPort();
    void partialStartReportsFailures();
    void perConnectionAdmissionIsolation();
    void interfaceAndSystemParametersUseTheInterfaceConnection();
    void parameterTreeStructureAndRevisionFencing();
    void parameterChangeNotificationsResyncOnOverflow();
    void concurrentParameterRequestsAreRefused();
    void moduleActionsNotifySubscribers();
    void declaredModuleActions();
    void cleanupTestCase();

private:
    DeviceConnectionResult invoke(const std::function<DeviceConnectionResult(IDeviceConnectionControl::Completion)>& admit);
    DeviceConnectionTargets discover();
    void connectTarget();
    DeviceParameterInventory inventory();
    ApiResult<QJsonValue> invokeTool(IToolProvider& provider, const QString& name,
        const QJsonObject& arguments = {});
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
    _controller = std::make_unique<OneGenSourceController>(nullptr, &rememberImageRejection);
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

ApiResult<QJsonValue> OneGenSourceControllerTests::invokeTool(IToolProvider& provider,
    const QString& name, const QJsonObject& arguments)
{
    std::optional<ApiResult<QJsonValue>> result;
    const auto admitted = provider.requestTool(name, 1, arguments,
        [&result](ApiResult<QJsonValue> value) { result = std::move(value); });
    if (!admitted.succeeded()) {
        const auto* error = admitted.error();
        return ApiResult<QJsonValue>::failure(error ? error->code : ApiErrorCode::Internal,
            error ? error->message : QStringLiteral("The tool was not admitted."));
    }
    if (!result && !waitFor([&] { return result.has_value(); }, 30000))
        return ApiResult<QJsonValue>::failure(ApiErrorCode::Timeout, "The tool did not complete.");
    return result ? std::move(*result)
                  : ApiResult<QJsonValue>::failure(ApiErrorCode::Internal, "The tool did not complete.");
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

void OneGenSourceControllerTests::singleWithUnusableImageReportsAssemblyFailure()
{
    if (!qEnvironmentVariableIsSet("ONEGEN_TEST_IMAGE_REJECTION"))
        QSKIP("This case uses the synthetic big-endian packed image.");
    connectTarget();
    _frames.store(0);
    const auto single = invoke([&](auto done) {
        return _controller->requestSingle(_connectionId.toStdString(), done);
    });
    QCOMPARE(single.error, DeviceConnectionError::BackendFailure);
    QCOMPARE(_frames.load(), 0);
    {
        std::lock_guard lock(imageRejectionMutex);
        QCOMPARE(imageRejections.size(), std::size_t{1});
        QCOMPARE(imageRejections.front().reason, DeviceImageRejection::UnsupportedConversion);
        QCOMPARE(imageRejections.front().pixelFormat, std::uint64_t{0x010a0046});
        QCOMPARE(imageRejections.front().width, std::uint32_t{64});
        QVERIFY(imageRejections.front().readableBytes > 0);
    }
    disconnect();
}

void OneGenSourceControllerTests::firstInputDetailsFollowCategory()
{
    const auto expected = qgetenv("ONEGEN_TEST_ACQUISITION_DETAILS");
    if (expected != "on" && expected != "off")
        QSKIP("This case runs with an explicit acquisition detail category rule.");
    connectTarget();
    _frames.store(0);
    firstInputLogs.store(0);
    firstConversionLogs.store(0);
    const auto single = [&] {
        const auto previous = qInstallMessageHandler(countAcquisitionDetails);
        const auto restore = qScopeGuard([previous] { qInstallMessageHandler(previous); });
        return invoke([&](auto done) { return _controller->requestSingle(_connectionId.toStdString(), done); });
    }();
    QCOMPARE(single.error, DeviceConnectionError::None);
    QCOMPARE(_frames.load(), 1);
    const int expectedLogs = expected == "on" ? 1 : 0;
    QCOMPARE(firstInputLogs.load(), expectedLogs);
    QCOMPARE(firstConversionLogs.load(), expectedLogs);
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

void OneGenSourceControllerTests::pathChangeUsesSessionStateAndRefreshesInventory()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "2");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    QVERIFY(fixture._controller->genTLPathChangeBlockReason().isEmpty());
    // Selecting a camera without opening it creates a child controller but no connection.
    fixture._controller->cameraController("selected-camera", "Selected camera");
    QVERIFY(fixture._controller->genTLPathChangeBlockReason().isEmpty());

    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto* camera = targetOfKind(targets, DeviceConnectionTargetKind::Device, 0);
    QVERIFY(camera);
    QVERIFY2(fixture.connect(camera->targetId).error == DeviceConnectionError::None,
        "The camera did not connect.");
    QVERIFY(fixture._controller->genTLPathChangeBlockReason().contains("Disconnect"));
    QString error;
    QVERIFY(!fixture._controller->retryDiscovery(&error));
    QVERIFY(error.contains("Disconnect"));

    const auto states = fixture.states();
    QVERIFY(!states.empty());
    QVERIFY2(fixture.disconnect(states.back().connectionId).error == DeviceConnectionError::None,
        "The connection did not close.");
    QVERIFY(fixture._controller->genTLPathChangeBlockReason().isEmpty());

    const auto targetCount = [&]() -> std::size_t {
        DeviceConnectionTargets value;
        const auto admitted = fixture._controller->requestConnectionTargets(
            [&value](DeviceConnectionTargets result) { value = std::move(result); });
        if (admitted.error != DeviceConnectionError::None) return 0;
        return static_cast<std::size_t>(std::count_if(value.targets.cbegin(), value.targets.cend(),
            [](const auto& target) { return target.kind == DeviceConnectionTargetKind::Device; }));
    };
    {
        EnvironmentVariable hidden("ONEGEN_SYNTHETIC_HIDDEN_DEVICES", "0");
        QVERIFY2(fixture._controller->retryDiscovery(&error), qPrintable(error));
        QVERIFY(waitFor([&] { return targetCount() == 1; }, 30000));
    }
    QVERIFY2(fixture._controller->retryDiscovery(&error), qPrintable(error));
    QVERIFY(waitFor([&] { return targetCount() > 1; }, 30000));
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
            [&](const onegen::InterfaceInfo&, const onegen::DeviceInfo& device) {
                cameraClaims.push_back(device.serial);
                if (device.serial.empty()) return std::shared_ptr<void>{};
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
            [&](const onegen::InterfaceInfo&, const onegen::DeviceInfo& device) {
                cameraClaims.push_back(device.serial);
                if (device.serial.empty()) return std::shared_ptr<void>{};
                return std::shared_ptr<void>(new int(1), [](void* value) { delete static_cast<int*>(value); });
            });
        const auto targets = fixture.discover();
        QCOMPARE(targets.result.error, DeviceConnectionError::None);
        QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId).error
            == DeviceConnectionError::None, "The network camera did not connect.");
        QVERIFY(interfaceClaims.empty());
        QCOMPARE(cameraClaims.size(), std::size_t(1));
        QCOMPARE(cameraClaims.front(), std::string("SYNTH-0001"));
    }
    {
        EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
        EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "GEV");
        EnvironmentVariable noSerial("ONEGEN_SYNTHETIC_NO_SERIAL", "1");
        std::vector<std::string> interfaceClaims;
        std::vector<std::string> cameraClaims;
        Fixture fixture(
            [&](const onegen::InterfaceInfo& info) {
                interfaceClaims.push_back(info.id);
                return std::shared_ptr<void>(new int(1), [](void* value) { delete static_cast<int*>(value); });
            },
            [&](const onegen::InterfaceInfo&, const onegen::DeviceInfo& device) {
                cameraClaims.push_back(device.serial);
                return std::shared_ptr<void>{};
            });
        const auto targets = fixture.discover();
        QCOMPARE(targets.result.error, DeviceConnectionError::None);
        QVERIFY2(fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId).error
            == DeviceConnectionError::None, "An unclaimed camera without a serial did not connect.");
        QVERIFY(interfaceClaims.empty());
        QCOMPARE(cameraClaims.size(), std::size_t(1));
        QVERIFY(cameraClaims.front().empty());
        // The shared key rule produces no key, so the module records an unclaimed camera.
        QVERIFY(playgroundCameraResourceKey(QString::fromStdString(cameraClaims.front())).isEmpty());
    }
}

void OneGenSourceControllerTests::claimDenialRetiresTheCameraConnection()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "GEV");
    bool deny = true;
    Fixture fixture({}, [&](const onegen::InterfaceInfo&, const onegen::DeviceInfo& device) {
        if (deny) throw onegen::DeviceClaimRefused("camera:synth-0001 is in use by another session.");
        return std::shared_ptr<void>(new int(1), [](void* value) { delete static_cast<int*>(value); });
    });
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto* camera = targetOfKind(targets, DeviceConnectionTargetKind::Device, 0);
    QVERIFY(camera);
    const auto refused = fixture.connect(camera->targetId);
    QCOMPARE(refused.error, DeviceConnectionError::Conflict);
    QCOMPARE(refused.effect, DeviceConnectionEffect::None);
    QVERIFY(refused.message.find("in use") != std::string::npos);
    QVERIFY(refused.state == std::nullopt || !refused.state->connected);
    // The failed attempt retires its child and implicit Interface, so the same camera can retry.
    QVERIFY(fixture.states().empty());
    deny = false;
    const auto connected = fixture.connect(camera->targetId);
    QVERIFY2(connected.error == DeviceConnectionError::None, connected.message.c_str());
    QCOMPARE(fixture.states().size(), std::size_t(2));
}

void OneGenSourceControllerTests::interfaceFeaturesFallBackWithoutIfGetPort()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    EnvironmentVariable noPort("ONEGEN_SYNTHETIC_NO_IF_GET_PORT", "1");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto interfaces = fixture._coordinator->interfaces();
    QVERIFY(!interfaces.empty());
    fixture._coordinator->open_interface(interfaces.front());
    const auto features = fixture._coordinator->interface_features(interfaces.front());
    QVERIFY(!features.children.empty());
    fixture._coordinator->close_interface(interfaces.front());
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

void OneGenSourceControllerTests::interfaceAndSystemParametersUseTheInterfaceConnection()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto opened = fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Interface, 0)->targetId);
    QVERIFY2(opened.error == DeviceConnectionError::None, opened.message.c_str());
    const auto interfaceConnection = opened.state->connectionId;
    const auto listed = fixture.inventory();
    QCOMPARE(listed.result.error, DeviceParameterError::None);
    const auto findParameter = [&](const std::string& scope, const std::string& name)
        -> const DeviceParameterDescriptor* {
        for (const auto& parameter : listed.parameters)
            if (parameter.target.scope == scope && parameter.target.name == name) return &parameter;
        return nullptr;
    };
    const auto* interfaceSpeed = findParameter("Interface", "InterfaceSpeed");
    QVERIFY(interfaceSpeed);
    QCOMPARE(interfaceSpeed->target.connectionId, interfaceConnection);
    QVERIFY(interfaceSpeed->categoryPath == std::vector<std::string>({"Root"}));
    QVERIFY(interfaceSpeed->representation == DeviceParameterRepresentation::Number);
    const auto* systemHeartbeat = findParameter("System/TL", "SystemHeartbeat");
    QVERIFY(systemHeartbeat);
    QCOMPARE(systemHeartbeat->target.connectionId, interfaceConnection);
    QVERIFY(systemHeartbeat->producerShared);
    QVERIFY(!interfaceSpeed->producerShared);
    QVERIFY(interfaceSpeed->value);
    QCOMPARE(std::get<std::int64_t>(*interfaceSpeed->value), std::int64_t(1000));

    const auto written = fixture.writeParameter(interfaceSpeed->target, std::int64_t(2001),
        interfaceSpeed->revision);
    QVERIFY2(written.error == DeviceParameterError::None, written.message.c_str());
    QCOMPARE(written.effect, DeviceParameterEffect::Applied);
    QVERIFY(written.descriptor && written.descriptor->value);
    QCOMPARE(std::get<std::int64_t>(*written.descriptor->value), std::int64_t(2001));
    const auto stale = fixture.writeParameter(interfaceSpeed->target, std::int64_t(2002),
        interfaceSpeed->revision);
    QCOMPARE(stale.error, DeviceParameterError::Conflict);
    QCOMPARE(stale.effect, DeviceParameterEffect::None);

    const auto systemWritten = fixture.writeParameter(systemHeartbeat->target, std::int64_t(7),
        systemHeartbeat->revision);
    QVERIFY2(systemWritten.error == DeviceParameterError::None, systemWritten.message.c_str());
    QCOMPARE(systemWritten.effect, DeviceParameterEffect::Applied);
    QVERIFY(systemWritten.descriptor && systemWritten.descriptor->value);
    QCOMPARE(std::get<std::int64_t>(*systemWritten.descriptor->value), std::int64_t(7));
    const auto systemStale = fixture.writeParameter(systemHeartbeat->target, std::int64_t(8),
        systemHeartbeat->revision);
    QCOMPARE(systemStale.error, DeviceParameterError::Conflict);

    const auto camera = fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId);
    QVERIFY2(camera.error == DeviceConnectionError::None, camera.message.c_str());
    const auto cameraConnection = camera.state->connectionId;
    const auto afterCamera = fixture.inventory();
    QCOMPARE(afterCamera.result.error, DeviceParameterError::None);
    for (const auto& parameter : afterCamera.parameters)
        if (parameter.target.connectionId == cameraConnection)
            QVERIFY(parameter.target.scope != "Interface" && parameter.target.scope != "System/TL");
    auto cameraInterface = *interfaceSpeed;
    cameraInterface.target.connectionId = cameraConnection;
    const auto cameraRefused = fixture.writeParameter(cameraInterface.target, std::int64_t(9), std::nullopt);
    QCOMPARE(cameraRefused.error, DeviceParameterError::Conflict);
    QVERIFY(cameraRefused.message.find("Interface connection") != std::string::npos);
    const auto systemWhileCamera = fixture.writeParameter(systemHeartbeat->target, std::int64_t(9), std::nullopt);
    QVERIFY2(systemWhileCamera.error == DeviceParameterError::None, systemWhileCamera.message.c_str());
    const auto busyInterface = fixture.writeParameter(interfaceSpeed->target, std::int64_t(2003), std::nullopt);
    QCOMPARE(busyInterface.error, DeviceParameterError::Unavailable);
    QVERIFY(busyInterface.message.find("Disconnect all cameras") != std::string::npos);
    QVERIFY2(fixture.disconnect(cameraConnection).error == DeviceConnectionError::None,
        "The camera did not disconnect.");
    const auto afterDisconnect = fixture.writeParameter(interfaceSpeed->target, std::int64_t(2004), std::nullopt);
    QVERIFY2(afterDisconnect.error == DeviceParameterError::None, afterDisconnect.message.c_str());
    QCOMPARE(afterDisconnect.effect, DeviceParameterEffect::Applied);
    QVERIFY2(fixture.disconnect(interfaceConnection).error == DeviceConnectionError::None,
        "The Interface did not disconnect.");
}

void OneGenSourceControllerTests::parameterTreeStructureAndRevisionFencing()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto connected = fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId);
    QVERIFY2(connected.error == DeviceConnectionError::None, connected.message.c_str());
    const auto connection = connected.state->connectionId;
    const auto listed = fixture.inventory();
    QCOMPARE(listed.result.error, DeviceParameterError::None);
    const auto findParameter = [&](const std::string& scope, const std::string& name)
        -> const DeviceParameterDescriptor* {
        for (const auto& parameter : listed.parameters)
            if (parameter.target.connectionId == connection && parameter.target.scope == scope
                && parameter.target.name == name) return &parameter;
        return nullptr;
    };
    const auto* width = findParameter("RemoteDevice", "Width");
    QVERIFY(width);
    QVERIFY(width->categoryPath == std::vector<std::string>({"Root"}));
    QVERIFY(width->visibility == DeviceParameterVisibility::Primary);
    QVERIFY(width->representation == DeviceParameterRepresentation::Number);
    const auto* pixelFormat = findParameter("RemoteDevice", "PixelFormat");
    QVERIFY(pixelFormat);
    QVERIFY(pixelFormat->representation == DeviceParameterRepresentation::Choice);
    const auto* trigger = findParameter("RemoteDevice", "TriggerSoftware");
    QVERIFY(trigger);
    QVERIFY(trigger->representation == DeviceParameterRepresentation::Automatic);
    const auto* componentId = findParameter("RemoteDevice", "ComponentIDValue");
    QVERIFY(componentId);
    const auto selectedBy = std::find_if(componentId->selectorRelations.begin(),
        componentId->selectorRelations.end(), [](const auto& relation) {
            return relation.name == "ComponentSelector"
                && relation.role == DeviceParameterSelectorRole::SelectedBy;
        });
    QVERIFY(selectedBy != componentId->selectorRelations.end());
    const auto* componentSelector = findParameter("RemoteDevice", "ComponentSelector");
    QVERIFY(componentSelector);
    const auto selects = std::find_if(componentSelector->selectorRelations.begin(),
        componentSelector->selectorRelations.end(), [](const auto& relation) {
            return relation.name == "ComponentIDValue"
                && relation.role == DeviceParameterSelectorRole::Selects;
        });
    QVERIFY(selects != componentSelector->selectorRelations.end());

    const auto applied = fixture.writeParameter(width->target, std::int64_t(321), std::nullopt);
    QVERIFY2(applied.error == DeviceParameterError::None, applied.message.c_str());
    const auto stale = fixture.writeParameter(width->target, std::int64_t(322), width->revision);
    QCOMPARE(stale.error, DeviceParameterError::Conflict);
    const auto current = fixture.readParameter(width->target);
    QVERIFY2(current.error == DeviceParameterError::None, current.message.c_str());
    QVERIFY(current.descriptor && current.descriptor->value);
    QCOMPARE(std::get<std::int64_t>(*current.descriptor->value), std::int64_t(321));
    const auto fresh = fixture.writeParameter(width->target, std::int64_t(322),
        current.descriptor->revision);
    QVERIFY2(fresh.error == DeviceParameterError::None, fresh.message.c_str());
    QCOMPARE(fresh.effect, DeviceParameterEffect::Applied);
}

void OneGenSourceControllerTests::parameterChangeNotificationsResyncOnOverflow()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    EnvironmentVariable queueLimit("ONEGEN_PARAMETER_CHANGE_QUEUE_LIMIT", "2");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto connected = fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId);
    QVERIFY2(connected.error == DeviceConnectionError::None, connected.message.c_str());
    const auto connection = connected.state->connectionId;
    std::mutex changesMutex;
    std::mutex gateMutex;
    std::vector<DeviceParameterChange> changes;
    const auto collect = [&](DeviceParameterChange change) {
        std::lock_guard gate(gateMutex);
        std::lock_guard lock(changesMutex);
        changes.push_back(std::move(change));
    };
    QCOMPARE(fixture._controller->requestParameterChanges(connection, "test-watch", collect).error,
        DeviceParameterError::None);
    QVERIFY(waitFor([&] {
        std::lock_guard lock(changesMutex);
        return !changes.empty();
    }, 30000));
    {
        std::lock_guard lock(changesMutex);
        QVERIFY(changes.front().kind == DeviceParameterChangeKind::Snapshot);
        QCOMPARE(changes.front().sequence, std::uint64_t(1));
        QVERIFY(!changes.front().descriptors.empty());
        QVERIFY(!changes.front().descriptors.front().categoryPath.empty());
        changes.clear();
    }

    const auto listed = fixture.inventory();
    QCOMPARE(listed.result.error, DeviceParameterError::None);
    const auto width = std::find_if(listed.parameters.begin(), listed.parameters.end(),
        [&](const auto& parameter) {
            return parameter.target.connectionId == connection
                && parameter.target.scope == "RemoteDevice"
                && parameter.target.name == "Width";
        });
    QVERIFY(width != listed.parameters.end());
    const auto written = fixture.writeParameter(width->target, std::int64_t(100), std::nullopt);
    QVERIFY2(written.error == DeviceParameterError::None, written.message.c_str());
    QVERIFY(waitFor([&] {
        std::lock_guard lock(changesMutex);
        return !changes.empty();
    }, 30000));
    {
        std::lock_guard lock(changesMutex);
        QVERIFY(changes.front().kind == DeviceParameterChangeKind::Delta);
        QCOMPARE(changes.front().sequence, std::uint64_t(2));
        QCOMPARE(changes.front().descriptors.size(), std::size_t(1));
        QVERIFY(changes.front().descriptors.front().revision > width->revision);
        changes.clear();
    }

    {
        std::unique_lock gate(gateMutex);
        for (int step = 0; step < 4; ++step) {
            const auto applied = fixture.writeParameter(width->target,
                std::int64_t(200 + step), std::nullopt);
            QVERIFY2(applied.error == DeviceParameterError::None, applied.message.c_str());
        }
    }
    QVERIFY(waitFor([&] {
        std::lock_guard lock(changesMutex);
        return std::any_of(changes.begin(), changes.end(),
            [](const auto& change) { return change.resync; });
    }, 30000));
    {
        std::lock_guard lock(changesMutex);
        std::uint64_t previousSequence = 2;
        bool sawResync = false;
        for (const auto& change : changes) {
            QVERIFY(change.sequence > previousSequence);
            previousSequence = change.sequence;
            if (!change.resync) continue;
            sawResync = true;
            QVERIFY(change.kind == DeviceParameterChangeKind::Snapshot);
            QVERIFY(!change.overflowReason.empty());
        }
        QVERIFY(sawResync);
        changes.clear();
    }

    std::promise<DeviceParameterResult> released;
    auto releasedFuture = released.get_future();
    QCOMPARE(fixture._controller->releaseParameterChanges(connection, "test-watch",
        [&released](DeviceParameterResult result) { released.set_value(std::move(result)); }).error,
        DeviceParameterError::None);
    QCOMPARE(releasedFuture.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    QCOMPARE(releasedFuture.get().error, DeviceParameterError::None);
    QCOMPARE(fixture.writeParameter(width->target, std::int64_t(300), std::nullopt).error,
        DeviceParameterError::None);
    QTest::qWait(100);
    QCoreApplication::processEvents();
    {
        std::lock_guard lock(changesMutex);
        QVERIFY(changes.empty());
    }
}

void OneGenSourceControllerTests::concurrentParameterRequestsAreRefused()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    EnvironmentVariable delay("ONEGEN_SYNTHETIC_PARAMETER_DELAY_MS", "100");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto connected = fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId);
    QVERIFY2(connected.error == DeviceConnectionError::None, connected.message.c_str());

    auto firstPromise = std::make_shared<std::promise<DeviceParameterInventory>>();
    auto firstFuture = firstPromise->get_future();
    const auto first = fixture._controller->requestParameters(
        [firstPromise](DeviceParameterInventory value) { firstPromise->set_value(std::move(value)); });
    QCOMPARE(first.error, DeviceParameterError::None);
    const auto second = fixture._controller->requestParameters([](DeviceParameterInventory) {});
    QCOMPARE(second.error, DeviceParameterError::Busy);
    const auto structural = fixture._controller->requestDiscover([](DeviceConnectionTargets) {});
    QCOMPARE(structural.error, DeviceConnectionError::Busy);
    QCOMPARE(firstFuture.wait_for(std::chrono::seconds(60)), std::future_status::ready);
    QCOMPARE(firstFuture.get().result.error, DeviceParameterError::None);

    auto thirdPromise = std::make_shared<std::promise<DeviceParameterInventory>>();
    auto thirdFuture = thirdPromise->get_future();
    const auto third = fixture._controller->requestParameters(
        [thirdPromise](DeviceParameterInventory value) { thirdPromise->set_value(std::move(value)); });
    QCOMPARE(third.error, DeviceParameterError::None);
    QCOMPARE(thirdFuture.wait_for(std::chrono::seconds(60)), std::future_status::ready);
    QCOMPARE(thirdFuture.get().result.error, DeviceParameterError::None);
}

void OneGenSourceControllerTests::moduleActionsNotifySubscribers()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto connected = fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId);
    QVERIFY2(connected.error == DeviceConnectionError::None, connected.message.c_str());
    const auto cameraConnection = connected.state->connectionId;
    std::string interfaceConnection;
    for (const auto& state : fixture.states())
        if (state.kind == DeviceConnectionTargetKind::Interface) interfaceConnection = state.connectionId;
    QVERIFY(!interfaceConnection.empty());
    auto* tools = dynamic_cast<IToolProvider*>(fixture._controller.get());
    QVERIFY(tools);
    std::mutex changesMutex;
    std::vector<DeviceParameterChange> changes;
    const auto collect = [&](DeviceParameterChange change) {
        std::lock_guard lock(changesMutex);
        changes.push_back(std::move(change));
    };
    const auto clearChanges = [&] {
        std::lock_guard lock(changesMutex);
        changes.clear();
    };
    const auto waitForResync = [&] {
        return waitFor([&] {
            std::lock_guard lock(changesMutex);
            return std::any_of(changes.begin(), changes.end(),
                [](const auto& change) { return change.resync; });
        }, 30000);
    };
    const auto hasDescriptor = [&](const std::string& name, const std::string& value) {
        std::lock_guard lock(changesMutex);
        for (const auto& change : changes)
            for (const auto& descriptor : change.descriptors)
                if (descriptor.target.name == name && descriptor.value
                    && std::holds_alternative<std::string>(*descriptor.value)
                    && std::get<std::string>(*descriptor.value) == value) return true;
        return false;
    };

    QCOMPARE(fixture._controller->requestParameterChanges(cameraConnection, "action-watch",
        collect).error, DeviceParameterError::None);
    QVERIFY(waitFor([&] {
        std::lock_guard lock(changesMutex);
        return !changes.empty();
    }, 30000));
    clearChanges();
    const auto copied = invokeTool(*tools, QStringLiteral("onegen.copy-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)},
                    {"module", QStringLiteral("Device")}});
    QVERIFY2(copied.succeeded(), copied.error() ? qPrintable(copied.error()->message) : "");
    const auto settings = copied.value().toObject().value("settings").toString();
    const auto listed = fixture.inventory();
    QCOMPARE(listed.result.error, DeviceParameterError::None);
    const auto acquisitionMode = std::find_if(listed.parameters.begin(), listed.parameters.end(),
        [&](const auto& parameter) {
            return parameter.target.connectionId == cameraConnection
                && parameter.target.scope == "Device"
                && parameter.target.name == "AcquisitionMode";
        });
    QVERIFY(acquisitionMode != listed.parameters.end());
    QVERIFY2(fixture.writeParameter(acquisitionMode->target, std::string("SingleFrame"),
        std::nullopt).error == DeviceParameterError::None, "The acquisition mode write failed.");
    clearChanges();
    const auto restored = invokeTool(*tools, QStringLiteral("onegen.restore-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)},
                    {"module", QStringLiteral("Device")}, {"settings", settings}});
    QVERIFY2(restored.succeeded(), restored.error() ? qPrintable(restored.error()->message) : "");
    QVERIFY(waitForResync());
    QVERIFY(hasDescriptor("AcquisitionMode", "Continuous"));
    {
        std::lock_guard lock(changesMutex);
        for (const auto& change : changes)
            if (change.resync) QVERIFY(!change.overflowReason.empty());
    }

    clearChanges();
    QCOMPARE(fixture._controller->releaseParameterChanges(cameraConnection, "action-watch",
        [](DeviceParameterResult) {}).error, DeviceParameterError::None);
    QCOMPARE(fixture._controller->requestParameterChanges(interfaceConnection, "system-watch",
        collect).error, DeviceParameterError::None);
    QVERIFY(waitFor([&] {
        std::lock_guard lock(changesMutex);
        return !changes.empty();
    }, 30000));
    clearChanges();
    const auto systemCopy = invokeTool(*tools, QStringLiteral("onegen.copy-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(interfaceConnection)},
                    {"module", QStringLiteral("System/TL")}});
    QVERIFY2(systemCopy.succeeded(), systemCopy.error() ? qPrintable(systemCopy.error()->message) : "");
    const auto systemSettings = systemCopy.value().toObject().value("settings").toString();
    const auto systemRestore = invokeTool(*tools, QStringLiteral("onegen.restore-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(interfaceConnection)},
                    {"module", QStringLiteral("System/TL")}, {"settings", systemSettings}});
    QVERIFY2(systemRestore.succeeded(), systemRestore.error() ? qPrintable(systemRestore.error()->message) : "");
    QVERIFY(waitForResync());
    QCOMPARE(fixture._controller->releaseParameterChanges(interfaceConnection, "system-watch",
        [](DeviceParameterResult) {}).error, DeviceParameterError::None);

    clearChanges();
    QCOMPARE(fixture._controller->requestParameterChanges(cameraConnection, "stream-watch",
        collect).error, DeviceParameterError::None);
    QVERIFY(waitFor([&] {
        std::lock_guard lock(changesMutex);
        return !changes.empty();
    }, 30000));
    clearChanges();
    const auto selected = invokeTool(*tools, QStringLiteral("onegen.stream-select"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)},
                    {"stream", QStringLiteral("0")}});
    QVERIFY2(selected.succeeded(), selected.error() ? qPrintable(selected.error()->message) : "");
    QVERIFY(waitForResync());
    QCOMPARE(fixture._controller->releaseParameterChanges(cameraConnection, "stream-watch",
        [](DeviceParameterResult) {}).error, DeviceParameterError::None);
}

void OneGenSourceControllerTests::declaredModuleActions()
{
    if (qEnvironmentVariableIsSet("ONEGEN_TEST_ACQ_MODE"))
        QSKIP("The unwritable-mode run covers only its own refusal.");
    EnvironmentVariable devices("ONEGEN_SYNTHETIC_DEVICES", "1");
    EnvironmentVariable type("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    Fixture fixture;
    const auto targets = fixture.discover();
    QCOMPARE(targets.result.error, DeviceConnectionError::None);
    const auto connected = fixture.connect(targetOfKind(targets, DeviceConnectionTargetKind::Device, 0)->targetId);
    QVERIFY2(connected.error == DeviceConnectionError::None, connected.message.c_str());
    const auto cameraConnection = connected.state->connectionId;
    std::string interfaceConnection;
    for (const auto& state : fixture.states())
        if (state.kind == DeviceConnectionTargetKind::Interface) interfaceConnection = state.connectionId;
    QVERIFY(!interfaceConnection.empty());
    auto* tools = dynamic_cast<IToolProvider*>(fixture._controller.get());
    QVERIFY(tools);
    const auto descriptors = tools->toolDescriptors();
    const auto findTool = [&](const QString& name) -> const ToolDescriptor* {
        for (const auto& descriptor : descriptors)
            if (descriptor.name == name) return &descriptor;
        return nullptr;
    };
    const auto* streamTool = findTool(QStringLiteral("onegen.stream-select"));
    QVERIFY(streamTool);
    QCOMPARE(streamTool->presentation.actionId, QStringLiteral("onegen.stream-select"));
    QCOMPARE(streamTool->presentation.group, QStringLiteral("onegen"));
    const auto* copyTool = findTool(QStringLiteral("onegen.copy-module-settings"));
    QVERIFY(copyTool);
    QCOMPARE(copyTool->presentation.actionId, QStringLiteral("onegen.copy-module-settings"));
    const auto* restoreTool = findTool(QStringLiteral("onegen.restore-module-settings"));
    QVERIFY(restoreTool);
    QCOMPARE(restoreTool->presentation.actionId, QStringLiteral("onegen.restore-module-settings"));

    const auto missingStream = invokeTool(*tools, QStringLiteral("onegen.stream-select"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)}});
    QVERIFY(!missingStream.succeeded());
    QVERIFY(missingStream.error());
    QCOMPARE(missingStream.error()->code, ApiErrorCode::ArgumentRequired);
    const auto prompt = missingStream.error()->details.value("argumentRequired").toObject();
    QCOMPARE(prompt.value("argument").toString(), QStringLiteral("stream"));
    QCOMPARE(prompt.value("format").toString(), QStringLiteral("enum"));
    QVERIFY(prompt.value("options").toArray().contains(QStringLiteral("0")));
    const auto selected = invokeTool(*tools, QStringLiteral("onegen.stream-select"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)},
                    {"stream", QStringLiteral("0")}});
    QVERIFY2(selected.succeeded(), selected.error() ? qPrintable(selected.error()->message) : "");
    QCOMPARE(selected.value().toObject().value("stream").toObject().value("index").toInt(), 0);

    const auto copied = invokeTool(*tools, QStringLiteral("onegen.copy-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)},
                    {"module", QStringLiteral("Device")}});
    QVERIFY2(copied.succeeded(), copied.error() ? qPrintable(copied.error()->message) : "");
    const auto settings = copied.value().toObject().value("settings").toString();
    QVERIFY(!settings.isEmpty());
    const auto listed = fixture.inventory();
    QCOMPARE(listed.result.error, DeviceParameterError::None);
    const auto acquisitionMode = std::find_if(listed.parameters.begin(), listed.parameters.end(),
        [&](const auto& parameter) {
            return parameter.target.connectionId == cameraConnection
                && parameter.target.scope == "Device"
                && parameter.target.name == "AcquisitionMode";
        });
    QVERIFY(acquisitionMode != listed.parameters.end());
    QVERIFY(acquisitionMode->value);
    QCOMPARE(std::get<std::string>(*acquisitionMode->value), std::string("Continuous"));
    QVERIFY2(fixture.writeParameter(acquisitionMode->target, std::string("SingleFrame"), std::nullopt).error
        == DeviceParameterError::None, "The acquisition mode write failed.");
    const auto restored = invokeTool(*tools, QStringLiteral("onegen.restore-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)},
                    {"module", QStringLiteral("Device")}, {"settings", settings}});
    QVERIFY2(restored.succeeded(), restored.error() ? qPrintable(restored.error()->message) : "");
    const auto readBack = fixture.readParameter(acquisitionMode->target);
    QVERIFY2(readBack.error == DeviceParameterError::None, readBack.message.c_str());
    QVERIFY(readBack.descriptor && readBack.descriptor->value);
    QCOMPARE(std::get<std::string>(*readBack.descriptor->value), std::string("Continuous"));

    const auto missingModule = invokeTool(*tools, QStringLiteral("onegen.copy-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)}});
    QVERIFY(!missingModule.succeeded());
    QCOMPARE(missingModule.error()->code, ApiErrorCode::ArgumentRequired);
    QCOMPARE(missingModule.error()->details.value("argumentRequired").toObject()
        .value("format").toString(), QStringLiteral("enum"));
    const auto missingSettings = invokeTool(*tools, QStringLiteral("onegen.restore-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)},
                    {"module", QStringLiteral("Device")}});
    QVERIFY(!missingSettings.succeeded());
    QCOMPARE(missingSettings.error()->code, ApiErrorCode::ArgumentRequired);
    QCOMPARE(missingSettings.error()->details.value("argumentRequired").toObject()
        .value("format").toString(), QStringLiteral("text"));

    const auto interfaceCopy = invokeTool(*tools, QStringLiteral("onegen.copy-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(interfaceConnection)}, {"module", QStringLiteral("Interface")}});
    QVERIFY2(interfaceCopy.succeeded(), interfaceCopy.error() ? qPrintable(interfaceCopy.error()->message) : "");
    const auto systemCopy = invokeTool(*tools, QStringLiteral("onegen.copy-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(interfaceConnection)}, {"module", QStringLiteral("System/TL")}});
    QVERIFY2(systemCopy.succeeded(), systemCopy.error() ? qPrintable(systemCopy.error()->message) : "");
    const auto systemSettings = systemCopy.value().toObject().value("settings").toString();
    QVERIFY(!systemSettings.isEmpty());
    const auto systemRestore = invokeTool(*tools, QStringLiteral("onegen.restore-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(interfaceConnection)}, {"module", QStringLiteral("System/TL")},
                    {"settings", systemSettings}});
    QVERIFY2(systemRestore.succeeded(), systemRestore.error() ? qPrintable(systemRestore.error()->message) : "");
    const auto invalidModule = invokeTool(*tools, QStringLiteral("onegen.copy-module-settings"),
        QJsonObject{{"connectionId", QString::fromStdString(cameraConnection)},
                    {"module", QStringLiteral("Interface")}});
    QVERIFY(!invalidModule.succeeded());
    QCOMPARE(invalidModule.error()->code, ApiErrorCode::InvalidArgument);
}

void OneGenSourceControllerTests::cleanupTestCase()
{
    _controller.reset();
    if (_coordinator) _coordinator->shutdown();
}

QTEST_GUILESS_MAIN(OneGenSourceControllerTests)
#include "SourceControllerTest.moc"
