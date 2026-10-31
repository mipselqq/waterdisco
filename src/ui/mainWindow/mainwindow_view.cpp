#include "include/ui/mainwindow.h"

#include "include/database/SettingsRepo.h"
#include "include/engine/connection/ConnectionManager.h"
#include "include/engine/vpn/VpnAuthController.h"
#include "include/engine/vpn/VpnEndpoints.h"
#include "include/global/LocalNetwork.hpp"
#include "include/global/RunningProfiles.hpp"
#include "NkrVersion.h"

#include <algorithm>

#include <QApplication>
#include <QAudioOutput>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPointer>
#include <QImage>
#include <QLabel>
#include <QMediaPlayer>
#include <QPixmap>
#include <QScrollBar>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include "include/api/RPC.h"
#include "include/database/GroupsRepo.h"
#include "include/database/entities/Group.h"
#include "include/database/ProfilesRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/global/HTTPRequestHelper.hpp"
#include "include/stats/traffic/TrafficLooper.hpp"
#include "include/stats/autoselector/AutoSelectorMonitor.hpp"
#include "include/ui/mainWindow/TestRunner.h"
#include "include/ui/widget/LiveChart.h"
#include "include/sys/KillSwitch.hpp"
#include "include/ui/setting/Icon.hpp"
#include "include/global/CountryHelper.hpp"
#include "include/ui/stats/dialog_auto_selector.h"
#include "include/ui/utils/ProfilesTableFilterHeader.h"
#include "include/ui/utils/ProfilesTableModel.h"
#include "include/ui/widget/LiveChart.h"
#include "include/ui/widget/StartStopButton.hpp"

#include <QHash>
#include <QMutexLocker>

// Mirrors the language switch in main.cpp; QLocale() alone follows the system, not an explicit setting.
bool MainWindow::usesTightLabels() const {
    static const QStringList byLanguageSetting = {"", "en", "zh_CN", "fa_IR", "ru_RU"};
    const int language = Configs::dataManager->settingsRepo->language;
    const QString locale = language == 0 ? QLocale().name() : byLanguageSetting.value(language);
    return locale.startsWith("zh") || locale.startsWith("ru");
}

void MainWindow::applyTopBarMetrics() {
    const QList<QToolButton*> menuButtons = {
        ui->toolButton_program, ui->toolButton_preferences, ui->toolButton_testing,
        ui->toolButton_routing, ui->toolButton_tools,
    };
    // Drop the previous run's floor: a stale minimum gets baked into minimumSizeHint() below.
    for (auto* b : menuButtons) b->setMinimumWidth(0);

    int uniformButtonWidth = 0;
    for (auto* b : menuButtons) {
        b->ensurePolished();
        uniformButtonWidth = qMax(uniformButtonWidth, b->sizeHint().width());
    }

    // CJK glyphs and long RU labels fill QToolButton's one-space slack and run into the chevron (#1665, #1829).
    if (usesTightLabels()) {
        uniformButtonWidth += 2 * fontMetrics().horizontalAdvance(' ');
    }
    for (auto* b : menuButtons) b->setMinimumWidth(uniformButtonWidth);

    const QSize contentMin = minimumSizeHint();
    setMinimumSize(qMax(designMinimumSize.width(), contentMin.width()),
                   qMax(designMinimumSize.height(), contentMin.height()));
}

void MainWindow::runImproveMood() {
    stopImproveMood();

    auto *viewport = ui->profilesTableView->viewport();
    auto *header = ui->profilesTableView->horizontalHeader();
    if (!viewport || !header) return;
    int usedWidth = 0;
    for (int i = 0; i < header->count(); ++i) {
        if (!ui->profilesTableView->isColumnHidden(i)) usedWidth += header->sectionSize(i);
    }
    if (viewport->width() - usedWidth < 120) {
        MW_show_log(tr("No free right-side table space for video."));
        return;
    }

    moodOverlay = new QWidget(viewport);
    moodOverlay->setObjectName("moodOverlay");
    moodOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    moodOverlay->setStyleSheet("QWidget#moodOverlay { background-color: rgba(0, 0, 0, 200); }");
    auto *layout = new QHBoxLayout(moodOverlay);
    layout->setContentsMargins(0, 0, 0, 0);
    moodLabel = new QLabel(moodOverlay);
    moodLabel->setAlignment(Qt::AlignCenter);
    moodLabel->setStyleSheet("background: black;");
    layout->addWidget(moodLabel);

    moodVideoSink = new QVideoSink(this);
    connect(moodVideoSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (!moodLabel || !frame.isValid()) return;
        const QImage image = frame.toImage();
        if (image.isNull() || moodLabel->size().isEmpty()) return;
        moodLabel->setPixmap(QPixmap::fromImage(image).scaled(
            moodLabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
    });

    moodAudio = new QAudioOutput(this);
    moodAudio->setVolume(1.0);
    moodPlayer = new QMediaPlayer(this);
    moodPlayer->setAudioOutput(moodAudio);
    moodPlayer->setVideoOutput(moodVideoSink);
    moodPlayer->setSource(QUrl("qrc:/Throne/public/easter.mp4"));
    connect(moodPlayer, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia) stopImproveMood();
    });
    connect(moodPlayer, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString&) { stopImproveMood(); });

    updateImproveMoodGeometry();
    moodOverlay->show();
    moodOverlay->raise();
    moodPlayer->play();
}

void MainWindow::stopImproveMood() {
    if (stoppingMoodPlayer) return;
    stoppingMoodPlayer = true;
    auto *player = moodPlayer.data();
    auto *audio = moodAudio.data();
    auto *sink = moodVideoSink.data();
    auto *overlay = moodOverlay.data();
    moodPlayer = nullptr;
    moodAudio = nullptr;
    moodVideoSink = nullptr;
    moodLabel = nullptr;
    moodOverlay = nullptr;

    if (player) {
        player->disconnect(this);
        player->stop();
        player->setSource({});
        player->setVideoOutput(static_cast<QVideoSink*>(nullptr));
        player->setAudioOutput(nullptr);
        player->deleteLater();
    }
    if (audio) audio->deleteLater();
    if (sink) sink->deleteLater();
    if (overlay) {
        overlay->hide();
        overlay->deleteLater();
    }
    if (ui && ui->profilesTableView && ui->profilesTableView->viewport()) {
        ui->profilesTableView->viewport()->update();
    }
    stoppingMoodPlayer = false;
}

void MainWindow::updateImproveMoodGeometry() {
    if (!moodOverlay || stoppingMoodPlayer) return;
    auto *viewport = ui->profilesTableView->viewport();
    auto *header = ui->profilesTableView->horizontalHeader();
    if (!viewport || !header) return;

    int usedWidth = 0;
    for (int i = 0; i < header->count(); ++i) {
        if (!ui->profilesTableView->isColumnHidden(i)) usedWidth += header->sectionSize(i);
    }
    const int x = qBound(0, usedWidth, viewport->width());
    const int width = viewport->width() - x;
    if (width <= 0 || viewport->height() <= 0) {
        stopImproveMood();
        return;
    }
    moodOverlay->setGeometry(x, 0, width, viewport->height());
}

void MainWindow::UpdateDataView(bool)
{
    // One queued setHtml at a time. Pollers used to force a rebuild every tick;
    // QTextBrowser::setHtml on the GUI thread is what made Cancel look hung.
    if (dataViewUpdateScheduled_.exchange(true)) return;
    QPointer<MainWindow> self(this);
    QMetaObject::invokeMethod(qApp, [self] {
        if (!self) return;
        self->dataViewUpdateScheduled_.store(false);
        const auto content = self->dataViewHtmlGenerator_.build();
        self->ui->data_view->setHtml(content.html);
        self->ui->data_view->setToolTip(content.toolTip);
        self->lastUpdatedMs.store(QDateTime::currentMSecsSinceEpoch());
    }, Qt::QueuedConnection);
}

void MainWindow::testPanelSeeded(Engine::TestKind kind, int totalProfiles)
{
    switch (kind) {
        case Engine::TestKind::Latency:
            dataViewHtmlGenerator_.seedLatencyTest(DataViewHtmlGenerator::LatencyTestPanelState::Kind::Url, totalProfiles);
            break;
        case Engine::TestKind::Ip:
            dataViewHtmlGenerator_.seedLatencyTest(DataViewHtmlGenerator::LatencyTestPanelState::Kind::Ip, totalProfiles);
            break;
        case Engine::TestKind::Speed:
            dataViewHtmlGenerator_.seedSpeedTest(totalProfiles);
            break;
    }
}

void MainWindow::testProgressed()
{
    dataViewHtmlGenerator_.addTestProgress();
}

void MainWindow::testPanelsCleared()
{
    dataViewHtmlGenerator_.clearTestSections();
}

void MainWindow::testViewChanged(bool force)
{
    UpdateDataView(force);
}

void MainWindow::speedTestProgress(const QString &profileName, const libcore::SpeedTestResult &result)
{
    dataViewHtmlGenerator_.setSpeedtestProgress(profileName, result);
}

void MainWindow::testedProfilesChanged(const QList<int> &profileIds)
{
    refresh_proxy_list(profileIds);
}

QString MainWindow::restartReasonText(Engine::RestartReason reason)
{
    switch (reason) {
        case Engine::RestartReason::Settings: return tr("Settings");
        case Engine::RestartReason::Routing: return tr("Routing");
        case Engine::RestartReason::Profile: return tr("Profile");
        case Engine::RestartReason::Group: return tr("Group");
    }
    return QObject::tr("Settings");
}

void MainWindow::showRestartReasons()
{
    QStringList reasons;
    for (const auto reason : connection()->restartReasons()) reasons << restartReasonText(reason);
    dataViewHtmlGenerator_.setPendingRestartReasons(reasons);
    UpdateDataView(true);
}

void MainWindow::showVpnEndpointStatus(const QString &summary, const QString &detail, bool problem)
{
    dataViewHtmlGenerator_.setVpnEndpointStatus(summary, detail, problem);
    UpdateDataView(true);
}

void MainWindow::setDownloadReport(const DownloadProgressReport& report, bool show)
{
    dataViewHtmlGenerator_.setDownloadReport(report, show);
}

void MainWindow::refresh_auto_selector_view()
{
    const auto view = Stats::autoSelectorMonitor->Snapshot();
    dataViewHtmlGenerator_.setAutoSelectorStatus(view.valid ? view.summary() : QString(),
                                                 view.valid ? view.detail() : QString());
    ui->actionAuto_Selector->setVisible(view.valid);
    UpdateDataView();
    if (m_autoSelectorDialog != nullptr) m_autoSelectorDialog->refresh();
}

void MainWindow::updateLogFilterFields() {
    QMutexLocker locker(&logMutex);
    includeKeywords.clear();
    excludeKeywords.clear();
    for (const auto& inKeyword : Configs::dataManager->settingsRepo->log_include_keyword) includeKeywords.append(inKeyword);
    for (const auto& exKeyword : Configs::dataManager->settingsRepo->log_exclude_keyword) excludeKeywords.append(exKeyword);
    includeCombined.setPattern(Configs::dataManager->settingsRepo->log_include_regex.join("|"));
    excludeCombined.setPattern(Configs::dataManager->settingsRepo->log_exclude_regex.join("|"));
    includeCombined.optimize();
    excludeCombined.optimize();
}

void MainWindow::applyProfileFilters()
{
    if (!profilesFilterModel) return;
    profilesFilterModel->setFilters(typeFilterString, addressFilterString, nameFilterString, countryFilterString);
    QTimer::singleShot(0, this, [this] { applyGroupSectionSpans(); });
    refresh_proxy_list_column_size();
}

void MainWindow::refresh_status(const QString &traffic_update) {
    const auto* settings = Configs::dataManager->settingsRepo.get();
    const bool foreground = isVisible() && !isMinimized();
    if (!foreground && !traffic_update.isEmpty()) {
        if (m_trayStatusThrottle.isValid() && m_trayStatusThrottle.elapsed() < 10'000) return;
        m_trayStatusThrottle.start();
    }
    Q_UNUSED(traffic_update)

    const auto running = runningSnapshot();
    QString group_name;
    if (running != nullptr) {
        auto group = Configs::dataManager->groupsRepo->GetGroup(running->gid);
        if (group != nullptr) group_name = group->name;
    }

    ui->checkBox_VPN->setChecked(settings->spmode_vpn);
    ui->checkBox_SystemProxy->setChecked(settings->spmode_system_proxy);
    if (isVisible() && !isMinimized()) {
        refreshInfoPanel();
    }

    const auto route = Configs::dataManager->routesRepo->GetRouteProfile(Configs::ActiveRouteId());
    const QString activeRouteName = (route && route->name != "Default") ? route->name : "";
    // VPN tunnel state takes this slot when an endpoint is up; otherwise the live country.
    const QString vpnExitState = vpnAuth()->exitStateText();
    const QString runningDetail = vpnExitState.isEmpty() ? (running ? running->runningCountryInfo : QString()) : vpnExitState;
    const auto killSwitchState = Sys::KillSwitch::instance()->state();
    const bool killSwitchBlocking = killSwitchState == Sys::KillSwitch::State::Failed && Sys::KillSwitch::instance()->failedForIdentity();

    auto make_title = [=,this](bool isTray) {
        QStringList tt;
        if (!isTray && Configs::KnownAdmin().value_or(false)) tt << "[Admin]";
        if (select_mode) tt << "[" + tr("Select") + "]";
        if (!title_error.isEmpty()) tt << "[" + title_error + "]";
        if (settings->spmode_vpn && !settings->spmode_system_proxy) tt << "[Tun]";
        if (!settings->spmode_vpn && settings->spmode_system_proxy) tt << "[" + tr("System Proxy") + "]";
        if (settings->spmode_vpn && settings->spmode_system_proxy) tt << "[Tun+" + tr("System Proxy") + "]";
        if (settings->kill_switch && killSwitchState == Sys::KillSwitch::State::Armed) tt << "[" + tr("Kill switch") + "]";
        if (settings->kill_switch && killSwitchState == Sys::KillSwitch::State::Failed) {
            tt << "[" + (killSwitchBlocking ? tr("Kill switch is blocking traffic") : tr("Kill switch: not active")) + "]";
        }
        tt << software_name;
        if (!isTray) tt << QString(NKR_VERSION);
        if (!activeRouteName.isEmpty()) {
            tt << "[" + activeRouteName + "]";
        }
        if (running != nullptr) {
            tt << running->outbound->DisplayTypeAndName() + "@" + group_name;
            if (!runningDetail.isEmpty()) {
                tt << runningDetail;
            }
        }
        return tt.join(isTray ? "\n" : " ");
    };

    auto icon_status_new = Icon::TrayIconStatus::None;
    if (connection()->connectionState() == ConnectionState::Connecting) icon_status_new = Icon::TrayIconStatus::Connecting;
    else if (running != nullptr) {
        if (settings->spmode_vpn) icon_status_new = Icon::TrayIconStatus::Vpn;
        else if (settings->spmode_system_proxy) icon_status_new = Icon::TrayIconStatus::SystemProxy;
        else icon_status_new = Icon::TrayIconStatus::Running;
    }

    setWindowTitle(make_title(false));
    if (icon_status_new != icon_status) QApplication::setWindowIcon(Icon::GetTaskbarIcon(icon_status_new));

    if (tray != nullptr) {
        tray->setToolTip(make_title(true));
        if (icon_status_new != icon_status) tray->setIcon(Icon::GetTrayIcon(icon_status_new));
    }

    icon_status = icon_status_new;

    refresh_startstop_button();
}

void MainWindow::refreshInfoPanel() {
    const auto running = runningSnapshot();
    const bool hasProxyIp = running != nullptr && !running->ip_out.trimmed().isEmpty();
    if (hasProxyIp) {
        const QString flag = running->test_country.isEmpty()
            ? QString()
            : CountryCodeToFlag(running->test_country) + QStringLiteral(" ");
        ui->proxy_ip_value->setText(flag + running->ip_out);
    } else {
        ui->proxy_ip_value->setText(QStringLiteral("—"));
    }
    ui->host_ip_value->setText(hostInfoIp.isEmpty() ? QStringLiteral("—") : hostInfoIp);
    ui->proxy_ip_copy->setEnabled(hasProxyIp);
    ui->host_ip_copy->setEnabled(!hostInfoIp.isEmpty());

    qint64 proxyDown = 0, proxyUp = 0, directDown = 0, directUp = 0;
    double proxyMax = 0, directMax = 0;
    {
        QMutexLocker locker(&Stats::trafficLooper->loop_mutex);
        if (const auto& proxy = Stats::trafficLooper->proxy) {
            proxyDown = proxy->downlink_total;
            proxyUp = proxy->uplink_total;
            proxyMax = proxy->max_rate;
        }
        if (const auto& direct = Stats::trafficLooper->direct) {
            directDown = direct->downlink_total;
            directUp = direct->uplink_total;
            directMax = direct->max_rate;
        }
    }
    const auto speed = [](double bytesPerSecond) {
        return QString::number(bytesPerSecond * 8.0 / 1'000'000.0, 'f', 1) + " Mbps";
    };
    const auto traffic = [](qint64 down, qint64 up) {
        return QString("↓ %1   ↑ %2").arg(ReadableSize(down), ReadableSize(up));
    };
    ui->proxy_speed_value->setText(speed(proxyMax));
    ui->proxy_traffic_value->setText(traffic(proxyDown, proxyUp));
    ui->host_speed_value->setText(speed(directMax));
    ui->host_traffic_value->setText(traffic(directDown, directUp));

    if (hostInfoIp.isEmpty() && !hostInfoProbeInFlight) refreshHostInfoIp();
}

void MainWindow::refreshHostInfoIp() {
    if (hostInfoProbeInFlight) return;
    hostInfoProbeInFlight = true;
    QPointer<MainWindow> self(this);
    runOnNewThread([self] {
        const auto response = NetworkRequestHelper::HttpGetDirect("https://api.ipify.org");
        const QString ip = response.error.isEmpty() ? QString::fromUtf8(response.data).trimmed() : QString();
        runOnUiThread([self, ip] {
            if (!self) return;
            self->hostInfoProbeInFlight = false;
            if (!ip.isEmpty()) self->hostInfoIp = ip;
            self->refreshInfoPanel();
        });
    });
}

void MainWindow::runCurrentConnectionProbe() {
    const auto running = runningSnapshot();
    if (running == nullptr || testRunner == nullptr || connectionProbeTimer == nullptr) return;

    const int profileID = running->id;
    const quint64 generation = connectionProbeGeneration;
    const bool hasIp = !running->ip_out.trimmed().isEmpty();
    static constexpr int kProbeWithIpMs = 60'000;
    static constexpr int kProbeForegroundMs = 10'000;

    // Until the first success the row is still Pending, so retry quickly even from the tray.
    if (testRunner->isRunning() || testRunner->isTestingCurrent()) {
        connectionProbeTimer->start(hasIp ? kProbeWithIpMs : 5'000);
        return;
    }

    QPointer<MainWindow> self(this);
    testRunner->runCurrentIpTest(profileID, [self, profileID, generation](bool success) {
        // Status and streak are decided here, behind the generation check: a probe that
        // outlived its run (restart, profile switch) must not touch the new run's state.
        if (!self) return;
        const auto current = self->runningSnapshot();
        if (current == nullptr || current->id != profileID
            || self->connectionProbeTimer == nullptr
            || self->connectionProbeGeneration != generation) {
            return;
        }
        const bool foreground = self->isVisible() && !self->isMinimized();
        const bool hasIpNow = !current->ip_out.trimmed().isEmpty();
        const auto profile = Configs::dataManager->profilesRepo->GetProfile(profileID);
        int delayMs;
        if (success) {
            self->connectionProbeFailStreak.store(0);
            if (profile) profile->connection_test_status = Configs::ConnectionTestStatus::Success;
            delayMs = hasIpNow ? kProbeWithIpMs : (foreground ? kProbeForegroundMs : kProbeWithIpMs);
        } else {
            const int streak = self->connectionProbeFailStreak.fetch_add(1) + 1;
            if (profile && streak >= 2) profile->connection_test_status = Configs::ConnectionTestStatus::Error;
            static constexpr int kBackoffMs[] = {5'000, 15'000, 30'000, 60'000};
            const int idx = qMin(streak - 1, 3);
            delayMs = hasIpNow ? kProbeWithIpMs : kBackoffMs[idx];
        }
        self->refresh_status();
        if (foreground) self->refresh_proxy_list({profileID});
        self->connectionProbeTimer->start(delayMs);
    });
}

void MainWindow::refresh_startstop_button() {
    auto *btn = ui->toolButton_startstop;
    if (btn == nullptr) return;

    const auto &settings = Configs::dataManager->settingsRepo;
    const auto running = runningSnapshot();
    const auto connState = connection()->connectionState();
    const bool connecting = connState == ConnectionState::Connecting;
    const bool disconnecting = connState == ConnectionState::Stopping;

    auto mode = StartStopButton::Mode::Off;
    if (running != nullptr) {
        if (settings->spmode_vpn) mode = StartStopButton::Mode::Tun;
        else if (settings->spmode_system_proxy) mode = StartStopButton::Mode::SystemProxy;
        else mode = StartStopButton::Mode::Core;
    }
    btn->setMode(mode);

    StartStopButton::State state;
    if (connecting) state = StartStopButton::State::Connecting;
    else if (disconnecting) state = StartStopButton::State::Disconnecting;
    else if (running != nullptr) state = StartStopButton::State::Running;
    else if (get_profile_to_start() >= 0) state = StartStopButton::State::Idle;
    else state = StartStopButton::State::Disabled;
    btn->setState(state);

    auto lock = StartStopButton::Lock::Hidden;
    if (settings->kill_switch) {
        switch (Sys::KillSwitch::instance()->state()) {
            case Sys::KillSwitch::State::Disabled: break;
            case Sys::KillSwitch::State::Arming: lock = StartStopButton::Lock::Pending; break;
            case Sys::KillSwitch::State::Armed:
                lock = running != nullptr && settings->spmode_vpn ? StartStopButton::Lock::Passing : StartStopButton::Lock::Blocking;
                break;
            case Sys::KillSwitch::State::Failed: lock = StartStopButton::Lock::Fault; break;
        }
    }
    btn->setLock(lock);

    if (trayConnectAction != nullptr) {
        if (connecting) {
            trayConnectAction->setText(tr("Connecting"));
        } else if (disconnecting) {
            trayConnectAction->setText(tr("Disconnecting"));
        } else {
            trayConnectAction->setText(running != nullptr ? tr("Disconnect") : tr("Connect"));
        }
        trayConnectAction->setEnabled(!connecting && !disconnecting &&
                                      (running != nullptr || get_profile_to_start() >= 0));
    }

    // Here rather than in refresh_status(): the connecting/disconnecting flags only ever refresh this button.
    auto iconStatus = Icon::TrayIconStatus::None;
    if (connecting || disconnecting) iconStatus = Icon::TrayIconStatus::Connecting;
    else if (mode == StartStopButton::Mode::Tun) iconStatus = Icon::TrayIconStatus::Vpn;
    else if (mode == StartStopButton::Mode::SystemProxy) iconStatus = Icon::TrayIconStatus::SystemProxy;
    else if (mode == StartStopButton::Mode::Core) iconStatus = Icon::TrayIconStatus::Running;
    if (iconStatus != icon_status) {
        QApplication::setWindowIcon(Icon::GetTaskbarIcon(iconStatus));
        if (tray != nullptr) tray->setIcon(Icon::GetTrayIcon(iconStatus));
        icon_status = iconStatus;
    }
}

void MainWindow::set_traffic_lines(const QList<Stats::TrafficLine> &lines) {
    m_trafficLines = lines;
    if (trafficChart == nullptr) return;
    QList<LiveChartRow> rows;
    int nextSlot = 1;
    for (const auto &line : lines) {
        LiveChartRow row;
        row.key = line.key;
        switch (line.kind) {
            case Stats::TrafficLineKind::Proxy:
                row.label = tr("Proxy");
                row.paletteSlot = 0;
                row.wash = true;
                break;
            case Stats::TrafficLineKind::Route:
            case Stats::TrafficLineKind::Aux:
                row.label = line.name;
                row.kind = line.kind == Stats::TrafficLineKind::Route ? tr("route") : tr("aux");
                row.paletteSlot = std::min(nextSlot++, 4);
                break;
            case Stats::TrafficLineKind::Other:
                row.label = line.name;
                row.paletteSlot = std::min(nextSlot++, 4);
                break;
            case Stats::TrafficLineKind::Direct:
                row.label = tr("Direct");
                row.paletteSlot = -1;
                break;
        }
        rows << row;
    }
    trafficChart->setRows(rows);
}

void MainWindow::add_traffic_point(qint64 monoMs, bool coreRunning, const QVector<double> &values,
                                   double proxyDown, double proxyUp, double directDown, double directUp) {
    if (trafficChart == nullptr) return;
    if (coreRunning && values.size() != m_trafficLines.size() * 2) return;
    trafficChart->addPoint(monoMs, coreRunning, values);
}

void MainWindow::addRunMarker(qint64 monoMs, Engine::RunMarker kind, const QString &name) {
    if (trafficChart == nullptr) return;
    auto marker = LiveChart::Marker::Started;
    switch (kind) {
        case Engine::RunMarker::Started: marker = LiveChart::Marker::Started; break;
        case Engine::RunMarker::Switched: marker = LiveChart::Marker::Switched; break;
        case Engine::RunMarker::Stopped: marker = LiveChart::Marker::Stopped; break;
    }
    trafficChart->addMarker(monoMs, marker, name);
}

void MainWindow::refresh_traffic_graph_settings() {
    const auto &settings = Configs::dataManager->settingsRepo;
    if (m_trafficStatsChartAction != nullptr) m_trafficStatsChartAction->setVisible(!settings->disable_traffic_aggregation);
    if (trafficChart == nullptr) return;
    if (settings->disable_traffic_stats) {
        trafficChart->setBlocked(tr("Traffic statistics are disabled"),
                                 tr("Untick “Disable Traffic Stats” in Basic Settings → Miscellaneous to record speeds again."));
    } else {
        trafficChart->setBlocked({});
    }
}

void MainWindow::refresh_proxy_list_column_size() {
    if (!profilesTableModel || !ui->profilesTableView->isVisible()) return;

    auto *hHeader = dynamic_cast<ProfilesTableFilterHeader*>(ui->profilesTableView->horizontalHeader());
    if (!hHeader || m_adjustingColumns) return;

    constexpr int columnCount = ProfilesTableModel::ColumnCount;
    QList<int> widths;
    for (int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
        const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
        if (!group) continue;
        if (group->column_width.size() == columnCount) {
            widths = group->column_width;
            break;
        }
    }

    const bool needsAutoWidths = widths.size() != columnCount
        || (m_columnWidthsAutoSized && m_profileStructureChanged);
    if (needsAutoWidths) {
        widths = profilesTableModel->preferredColumnWidths(ui->profilesTableView->font());
        m_columnWidthsAutoSized = true;
        // A single unified table has a single width set. Persist the automatic
        // baseline once, so a later redraw never falls back to visible-row
        // measurements.
        for (int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            if (!group) continue;
            group->column_width = widths;
            group->clearCalculatedColumnWidth();
            Configs::dataManager->groupsRepo->Save(group);
        }
    }
    m_profileStructureChanged = false;

    bool differs = false;
    for (int column = 0; column < columnCount; ++column) {
        if (hHeader->sectionSize(column) != widths[column]) {
            differs = true;
            break;
        }
    }
    if (!differs) return;

    m_adjustingColumns = true;
    hHeader->blockSignals(true);
    for (int column = 0; column < columnCount; ++column) {
        hHeader->setSectionResizeMode(column, QHeaderView::Interactive);
        hHeader->resizeSection(column, widths[column]);
    }
    hHeader->adjustPositions();
    hHeader->blockSignals(false);
    m_adjustingColumns = false;
}

void MainWindow::refresh_proxy_list(const QList<int>& ids, bool mayNeedReset, RefreshAnchor anchor) {
    if (!Configs::dataManager->settingsRepo->refreshing_group) saveProfileFocusState();
    refresh_proxy_list_impl(ids, mayNeedReset);
    if (mayNeedReset) restoreProfileFocusState(anchor);
}

void MainWindow::refresh_proxy_list_impl(const QList<int>& ids, bool mayNeedReset) {
    refresh_proxy_list_impl_refresh_data(ids, mayNeedReset);
    // Measuring content widths walks every profile. Skip it on the speedtest
    // hot path, which only repaints one (or a few) rows.
    if (ids.isEmpty()) refresh_proxy_list_column_size();
}

void MainWindow::refresh_proxy_list_impl_refresh_data(const QList<int>& ids, bool mayNeedReset) {
    const bool foreground = isVisible() && !isMinimized();
    if (!ids.isEmpty()) {
        for (auto id:ids) {
            profilesTableModel->refreshProfileId(id);
            if (foreground) liveReorderProfile(id);
        }
    } else {
        QList<ProfilesTableModel::GroupSection> sections;
        for (int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            if (group) sections.append({gid, group->Profiles()});
        }
        if (profilesTableModel->setGroupSections(sections, mayNeedReset)) {
            m_profileStructureChanged = true;
            QTimer::singleShot(0, this, [this] { applyGroupSectionSpans(); });
        }
    }
}

bool MainWindow::isLiveSortableColumn(int column) const {
    switch (column) {
    case ProfilesTableModel::ColType:
    case ProfilesTableModel::ColAddress:
    case ProfilesTableModel::ColName:
    case ProfilesTableModel::ColLatency:
    case ProfilesTableModel::ColRxSpeed:
    case ProfilesTableModel::ColConnectionTime:
    case ProfilesTableModel::ColSiteScore:
    case ProfilesTableModel::ColRxTraffic:
    case ProfilesTableModel::ColTxTraffic:
        return true;
    default:
        return false;
    }
}

bool MainWindow::sortActionFromColumn(int column, bool descending, GroupSortAction &action,
                                      Configs::testBy &testSortBy, Configs::trafficBy &trafficSortBy) const {
    action = {};
    action.descending = descending;
    testSortBy = Configs::testBy::siteScore;
    trafficSortBy = Configs::trafficBy::rx;
    switch (column) {
    case ProfilesTableModel::ColType: {
        Configs::typeBy typeSortBy = Configs::typeBy::byType;
        if (const auto group = Configs::dataManager->groupsRepo->CurrentGroup()) {
            typeSortBy = group->type_sort_by;
        }
        action.method = (Configs::dataManager->settingsRepo->show_config_security
                         && typeSortBy == Configs::typeBy::bySecurity)
                            ? GroupSortMethod::BySecurity
                            : GroupSortMethod::ByType;
        return true;
    }
    case ProfilesTableModel::ColAddress:
        action.method = GroupSortMethod::ByAddress;
        return true;
    case ProfilesTableModel::ColName:
        action.method = GroupSortMethod::ByName;
        return true;
    case ProfilesTableModel::ColLatency:
        action.method = GroupSortMethod::ByTestResult;
        testSortBy = Configs::testBy::latency;
        return true;
    case ProfilesTableModel::ColRxSpeed:
        action.method = GroupSortMethod::ByTestResult;
        testSortBy = Configs::testBy::rxSpeed;
        return true;
    case ProfilesTableModel::ColConnectionTime:
        action.method = GroupSortMethod::ByTestResult;
        testSortBy = Configs::testBy::connectTime;
        return true;
    case ProfilesTableModel::ColSiteScore:
        action.method = GroupSortMethod::ByTestResult;
        testSortBy = Configs::testBy::siteScore;
        return true;
    case ProfilesTableModel::ColRxTraffic:
        action.method = GroupSortMethod::ByTraffic;
        trafficSortBy = Configs::trafficBy::rx;
        return true;
    case ProfilesTableModel::ColTxTraffic:
        action.method = GroupSortMethod::ByTraffic;
        trafficSortBy = Configs::trafficBy::tx;
        return true;
    default:
        return false;
    }
}

void MainWindow::stampSortPrefsOnGroups(Configs::testBy testSortBy, Configs::trafficBy trafficSortBy,
                                        Configs::typeBy typeSortBy) {
    for (int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
        const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
        if (!group) continue;
        group->test_sort_by = testSortBy;
        group->traffic_sort_by = trafficSortBy;
        group->type_sort_by = typeSortBy;
    }
}

void MainWindow::persistLiveSortPrefs() {
    auto *settings = Configs::dataManager->settingsRepo.get();
    settings->profiles_sort_column = live_sort_column;
    settings->profiles_sort_descending = live_sort_descending;
    settings->Save();
}

void MainWindow::applyLiveSortIndicator() {
    auto *header = ui->profilesTableView->horizontalHeader();
    if (!header) return;
    if (isLiveSortableColumn(live_sort_column)) {
        header->setSortIndicatorShown(true);
        header->setSortIndicator(live_sort_column,
                                 live_sort_descending ? Qt::DescendingOrder : Qt::AscendingOrder);
    } else {
        header->setSortIndicatorShown(false);
    }
}

void MainWindow::runProfilesSort(const GroupSortAction &action, Configs::testBy testSortBy,
                                 Configs::trafficBy trafficSortBy) {
    if (!profilesTableModel) return;
    const bool flat = profilesTableModel->isFlatList();
    const auto groupIds = Configs::dataManager->groupsRepo->GetGroupsTabOrder();

    // Header-click sorts stay on the UI thread: the profile cache is already
    // warm, and a full model reset is cheaper than a background hop plus a
    // SQLite write per group. Persist order in the existing 1s debounce.
    if (flat) {
        QList<int> allIds;
        QHash<int, int> idToGroup;
        for (int gid : groupIds) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            if (!group) continue;
            QMutexLocker locker(&group->mutex);
            for (int id : group->profiles) {
                allIds.append(id);
                idToGroup.insert(id, gid);
            }
        }
        Configs::SortProfileIdList(allIds, action, testSortBy, trafficSortBy);
        QHash<int, QList<int>> perGroup;
        for (int id : allIds) perGroup[idToGroup.value(id)].append(id);
        for (int gid : groupIds) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            if (!group) continue;
            group->ReplaceProfiles(perGroup.value(gid));
            markGroupOrderDirty(gid);
        }
        profilesTableModel->setGlobalOrder(allIds);
    } else {
        for (int gid : groupIds) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            if (!group) continue;
            {
                QMutexLocker locker(&group->mutex);
                Configs::SortProfileIdList(group->profiles, action, testSortBy, trafficSortBy);
            }
            markGroupOrderDirty(gid);
        }
        profilesTableModel->clearGlobalOrder();
    }

    QList<ProfilesTableModel::GroupSection> sections;
    for (int gid : groupIds) {
        const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
        if (group) sections.append({gid, group->Profiles()});
    }
    saveProfileFocusState();
    if (profilesTableModel->setGroupSections(sections, false, true)) {
        m_profileStructureChanged = true;
        applyGroupSectionSpans();
    }
    restoreProfileFocusState(RefreshAnchor::KeepPlace);
    refresh_proxy_list_column_size();
}

int MainWindow::liveInsertDestinationRow(int profileId) const {
    if (!profilesTableModel || !isLiveSortableColumn(live_sort_column)) return -1;
    const int from = profilesTableModel->indexOfProfile(profileId);
    if (from < 0) return -1;

    GroupSortAction action;
    Configs::testBy testSortBy;
    Configs::trafficBy trafficSortBy;
    if (!sortActionFromColumn(live_sort_column, live_sort_descending, action, testSortBy, trafficSortBy)) {
        return -1;
    }

    const bool disabled = Configs::dataManager->settingsRepo->IsProfileDisabled(profileId);
    const bool flat = profilesTableModel->isFlatList();
    const int groupId = profilesTableModel->groupIdAt(from);
    int lo = 0;
    int hi = profilesTableModel->rowCount() - 1;
    if (!flat) {
        lo = from;
        while (lo > 0 && !profilesTableModel->isGroupHeader(lo)) --lo;
        if (profilesTableModel->isGroupHeader(lo)) ++lo;
        hi = from;
        while (hi + 1 < profilesTableModel->rowCount()
               && !profilesTableModel->isGroupHeader(hi + 1)
               && profilesTableModel->groupIdAt(hi + 1) == groupId) {
            ++hi;
        }
    }

    int firstDisabled = hi + 1;
    for (int row = lo; row <= hi; ++row) {
        if (profilesTableModel->isGroupHeader(row)) continue;
        const int id = profilesTableModel->data(
            profilesTableModel->index(row, 0), ProfilesTableModel::ProfileIdRole).toInt();
        if (Configs::dataManager->settingsRepo->IsProfileDisabled(id)) {
            firstDisabled = row;
            break;
        }
    }
    if (disabled) lo = firstDisabled;
    else hi = firstDisabled - 1;
    if (lo > hi) return from;

    auto comesBefore = [&](int a, int b) {
        return Configs::ProfileIdComesBefore(a, b, action, testSortBy, trafficSortBy);
    };

    auto idAt = [&](int row) {
        return profilesTableModel->data(
            profilesTableModel->index(row, 0), ProfilesTableModel::ProfileIdRole).toInt();
    };

    int prev = -1;
    for (int row = from - 1; row >= lo; --row) {
        if (!profilesTableModel->isGroupHeader(row)) {
            prev = idAt(row);
            break;
        }
    }
    int next = -1;
    for (int row = from + 1; row <= hi; ++row) {
        if (!profilesTableModel->isGroupHeader(row)) {
            next = idAt(row);
            break;
        }
    }
    if ((prev < 0 || !comesBefore(profileId, prev)) && (next < 0 || !comesBefore(next, profileId))) {
        return from;
    }

    int insertRow = hi + 1;
    for (int row = lo; row <= hi; ++row) {
        if (row == from || profilesTableModel->isGroupHeader(row)) continue;
        if (comesBefore(profileId, idAt(row))) {
            insertRow = row;
            break;
        }
    }
    const int dest = insertRow > from ? insertRow - 1 : insertRow;
    return dest;
}

void MainWindow::liveReorderProfile(int profileId) {
    if (!profilesTableModel || !isLiveSortableColumn(live_sort_column)) return;
    const int dest = liveInsertDestinationRow(profileId);
    const int from = profilesTableModel->indexOfProfile(profileId);
    if (dest < 0 || from < 0 || dest == from) return;
    const int groupId = profilesTableModel->groupIdAt(from);
    if (!profilesTableModel->moveProfileToRow(profileId, dest)) return;
    if (groupId >= 0) {
        if (auto group = Configs::dataManager->groupsRepo->GetGroup(groupId)) {
            group->ReplaceProfiles(profilesTableModel->profileIdsForGroup(groupId));
            markGroupOrderDirty(groupId);
        }
    }
}

void MainWindow::markGroupOrderDirty(int groupId) {
    if (groupId < 0) return;
    m_dirtySortGroupIds.insert(groupId);
    if (!m_groupOrderSaveDebounce) {
        m_groupOrderSaveDebounce = new QTimer(this);
        m_groupOrderSaveDebounce->setSingleShot(true);
        m_groupOrderSaveDebounce->setInterval(1000);
        connect(m_groupOrderSaveDebounce, &QTimer::timeout, this, [this] { flushDirtyGroupOrders(); });
    }
    m_groupOrderSaveDebounce->start();
}

void MainWindow::flushDirtyGroupOrders() {
    for (int gid : m_dirtySortGroupIds) {
        if (const auto group = Configs::dataManager->groupsRepo->GetGroup(gid)) {
            Configs::dataManager->groupsRepo->Save(group);
        }
    }
    m_dirtySortGroupIds.clear();
}

void MainWindow::syncInfoPanelTop() {
    if (!ui || !infoTabAlignSpacer) return;
    QWidget *anchor = ui->down_tab;
    QWidget *logsPage = ui->stats_widget ? ui->stats_widget->currentWidget() : nullptr;
    if (!anchor || !logsPage || !ui->proxyInfoGroup) {
        QTabBar *bar = ui->stats_widget ? ui->stats_widget->tabBar() : nullptr;
        const int tabH = (bar && bar->isVisible()) ? bar->height() : 0;
        infoTabAlignSpacer->setFixedHeight(qMax(0, tabH));
        return;
    }
    if (logsPage->height() <= 0) {
        QTabBar *bar = ui->stats_widget->tabBar();
        infoTabAlignSpacer->setFixedHeight(qMax(0, (bar && bar->isVisible()) ? bar->height() : 0));
        return;
    }
    const int logsY = logsPage->mapTo(anchor, QPoint(0, 0)).y();
    const int proxyY = ui->proxyInfoGroup->mapTo(anchor, QPoint(0, 0)).y();
    const int delta = proxyY - logsY;
    if (delta == 0) return;
    infoTabAlignSpacer->setFixedHeight(qMax(0, infoTabAlignSpacer->height() - delta));
}

// Owns no test session, so unlike the group sweeps it stays out of TestRunner.
void MainWindow::url_test_current() {
    last_test_time = QDateTime::currentSecsSinceEpoch();
    MW_show_log(tr("Testing current profile"));

    runOnNewThread([=,this] {
        libcore::TestReq req;
        req.test_current = true;
        req.url = Configs::dataManager->settingsRepo->test_latency_url.toStdString();

        bool rpcOK;
        auto result = API::defaultClient->Test(&rpcOK, req);
        if (!rpcOK || result.results.empty()) return;

        auto latency = result.results[0].latency_ms.value();
        last_test_time = QDateTime::currentSecsSinceEpoch();
        // Blocking RPC, so it has to resolve here rather than on the UI thread.
        const auto vpnText = latency <= 0 ? Stats::LiveVpnStateText() : QString();

        runOnUiThread([=,this] {
            if (!result.results[0].error.value().empty()) {
                MW_show_log(QString("UrlTest error: %1").arg(QString::fromStdString(result.results[0].error.value())));
            }
            if (latency <= 0) {
                MW_show_log(tr("Test Result") + ": " + (vpnText.isEmpty() ? tr("Unavailable") : vpnText));
            } else if (latency > 0) {
                MW_show_log(tr("Test Result") + ": " + QString("%1 ms").arg(latency));
            }
        });
    });
}
