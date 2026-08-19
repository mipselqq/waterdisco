#include "include/ui/mainwindow.h"
#include "NkrVersion.h"

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
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include "include/api/RPC.h"
#include "include/database/GroupsRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/global/HTTPRequestHelper.hpp"
#include "include/stats/traffic/TrafficLooper.hpp"
#include "include/stats/autoselector/AutoSelectorMonitor.hpp"
#include "include/ui/setting/Icon.hpp"
#include "include/ui/stats/dialog_auto_selector.h"
#include "include/ui/utils/ProfilesTableFilterHeader.h"
#include "include/ui/utils/ProfilesTableModel.h"
#include "include/ui/widget/StartStopButton.hpp"

void MainWindow::applyTopBarMetrics() {
    // Give the menu toolButtons a uniform width (the widest one's) so the top
    // bar reads as an even row. The start/stop button keeps its own square size.
    const QList<QToolButton*> menuButtons = {
        ui->toolButton_program, ui->toolButton_preferences, ui->toolButton_testing,
        ui->toolButton_routing, ui->toolButton_tools,
    };
    // Drop the previous run's floor first: a stale minimum would otherwise be
    // baked into minimumSizeHint() below and never shrink back.
    for (auto* b : menuButtons) b->setMinimumWidth(0);

    // Content width only: the chevron already clears the label via ::menu-indicator, so
    // reserving arrow padding would widen all five for a gap only the widest needs.
    int uniformButtonWidth = 0;
    for (auto* b : menuButtons) {
        b->ensurePolished();
        uniformButtonWidth = qMax(uniformButtonWidth, b->sizeHint().width());
    }
    for (auto* b : menuButtons) b->setMinimumWidth(uniformButtonWidth);

    // Translated labels (RU runs ~2x English) outgrow the designed 800x600 floor and
    // clip the widgets after it, so follow what the layout actually needs (#1665).
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

void MainWindow::UpdateDataView(bool force)
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - lastUpdatedMs.load() < 100)
    {
        return;
    }
    auto html = dataViewHtmlGenerator_.buildHtml();
    runOnUiThread([=, this] {
        ui->data_view->setHtml(html);
    }, true);
    lastUpdatedMs.store(QDateTime::currentMSecsSinceEpoch());
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
    // The Tools entry only makes sense while a selector is actually running.
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
    refresh_proxy_list_column_size();
}

void MainWindow::refresh_status(const QString &traffic_update) {
    const auto* settings = Configs::dataManager->settingsRepo.get();
    Q_UNUSED(traffic_update)

    // From UI
    QString group_name;
    if (running != nullptr) {
        auto group = Configs::dataManager->groupsRepo->GetGroup(running->gid);
        if (group != nullptr) group_name = group->name;
    }

    ui->checkBox_VPN->setChecked(settings->spmode_vpn);
    ui->checkBox_SystemProxy->setChecked(settings->spmode_system_proxy);
    refreshInfoPanel();

    const auto route = Configs::dataManager->routesRepo->GetRouteProfile(settings->current_route_id);
    const QString activeRouteName = (route && route->name != "Default") ? route->name : "";

    auto make_title = [=,this](bool isTray) {
        QStringList tt;
        if (!isTray && Configs::IsAdmin()) tt << "[Admin]";
        if (select_mode) tt << "[" + tr("Select") + "]";
        if (!title_error.isEmpty()) tt << "[" + title_error + "]";
        if (settings->spmode_vpn && !settings->spmode_system_proxy) tt << "[Tun]";
        if (!settings->spmode_vpn && settings->spmode_system_proxy) tt << "[" + tr("System Proxy") + "]";
        if (settings->spmode_vpn && settings->spmode_system_proxy) tt << "[Tun+" + tr("System Proxy") + "]";
        tt << software_name;
        if (!isTray) tt << QString(NKR_VERSION);
        if (!activeRouteName.isEmpty()) {
            tt << "[" + activeRouteName + "]";
        }
        if (running != nullptr) {
            tt << running->outbound->DisplayTypeAndName() + "@" + group_name;
            if (!running->runningCountryInfo.isEmpty()) {
                tt << running->runningCountryInfo;
            }
        }
        return tt.join(isTray ? "\n" : " ");
    };

    auto icon_status_new = Icon::NONE;

    if (running != nullptr) {
        if (settings->spmode_vpn) {
            icon_status_new = Icon::VPN;
        } else if (settings->system_dns_set && settings->spmode_system_proxy) {
            icon_status_new = Icon::SYSTEM_PROXY_DNS;
        } else if (settings->system_dns_set) {
            icon_status_new = Icon::DNS;
        } else if (settings->spmode_system_proxy) {
            icon_status_new = Icon::SYSTEM_PROXY;
        } else {
            icon_status_new = Icon::RUNNING;
        }
    }

    // refresh title & window icon
    setWindowTitle(make_title(false));
    if (icon_status_new != icon_status) QApplication::setWindowIcon(GetTrayIcon(icon_status_new));

    // refresh tray
    if (tray != nullptr) {
        tray->setToolTip(make_title(true));
        if (icon_status_new != icon_status) tray->setIcon(Icon::GetTrayIcon(icon_status_new));
    }

    icon_status = icon_status_new;

    refresh_startstop_button();
}

void MainWindow::refreshInfoPanel() {
    const bool trulyConnected = running && !running->ip_out.trimmed().isEmpty();
    ui->proxy_connected_value->setText(trulyConnected ? tr("Yes") : tr("No"));
    ui->proxy_connected_value->setStyleSheet(
        trulyConnected ? "color: #2eaf57; font-weight: 600;" : "color: #d9534f; font-weight: 600;");
    ui->proxy_ip_value->setText(trulyConnected ? running->ip_out : QStringLiteral("—"));
    ui->host_ip_value->setText(hostInfoIp.isEmpty() ? QStringLiteral("—") : hostInfoIp);

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

void MainWindow::refresh_startstop_button() {
    auto *btn = ui->toolButton_startstop;
    if (btn == nullptr) return;

    const auto &settings = Configs::dataManager->settingsRepo;

    // Ring colour reflects the active proxy mode (mirrors the tray-icon logic
    // above); it only shows while running.
    auto mode = StartStopButton::Mode::Off;
    if (running != nullptr) {
        if (settings->spmode_vpn) mode = StartStopButton::Mode::Tun;
        else if (settings->system_dns_set && settings->spmode_system_proxy) mode = StartStopButton::Mode::SystemProxyDns;
        else if (settings->system_dns_set) mode = StartStopButton::Mode::Dns;
        else if (settings->spmode_system_proxy) mode = StartStopButton::Mode::SystemProxy;
        else mode = StartStopButton::Mode::Core;
    }
    btn->setMode(mode);

    StartStopButton::State state;
    if (m_profileConnecting) state = StartStopButton::State::Connecting;
    else if (m_profileDisconnecting) state = StartStopButton::State::Disconnecting;
    else if (running != nullptr) state = StartStopButton::State::Running;
    else if (get_profile_to_start() >= 0) state = StartStopButton::State::Idle;
    else state = StartStopButton::State::Disabled;
    btn->setState(state);
}

void MainWindow::update_traffic_graph(int proxyDl, int proxyUp, int directDl, int directUp)
{
    if (speedChartWidget) {
        QMap<SpeedWidget::GraphType, long> pointData;
        pointData[SpeedWidget::OUTBOUND_PROXY_UP] = proxyUp;
        pointData[SpeedWidget::OUTBOUND_PROXY_DOWN] = proxyDl;
        pointData[SpeedWidget::OUTBOUND_DIRECT_UP] = directUp;
        pointData[SpeedWidget::OUTBOUND_DIRECT_DOWN] = directDl;

        speedChartWidget->AddPointData(pointData);
    }
}

void MainWindow::refresh_proxy_list_column_size() {
    const auto group = Configs::dataManager->groupsRepo->CurrentGroup();
    if (!group || !ui->profilesTableView->isVisible()) return;

    auto *hHeader = dynamic_cast<ProfilesTableFilterHeader*>(ui->profilesTableView->horizontalHeader());
    QTimer::singleShot(0, ui->profilesTableView, [=, this]() {
        // Stop the resizeSection / scrollbar-policy changes below from re-entering
        // this routine via the vertical scrollbar's valueChanged signal.
        if (m_adjustingColumns) return;
        m_adjustingColumns = true;
        QScrollBar *vBar = ui->profilesTableView->verticalScrollBar();
        const bool vBarBlocked = vBar->blockSignals(true);
        hHeader->blockSignals(true);
        constexpr int columnCount = ProfilesTableModel::ColumnCount;
        // Widths saved before the column set last changed no longer line up with
        // the header, so fall back to auto-sizing instead of indexing past the end.
        if (!group->column_width.isEmpty() && group->column_width.size() != columnCount) {
            group->column_width.clear();
        }
        if (group->column_width.isEmpty()) {
            hHeader->setSectionResizeMode(ProfilesTableModel::ColType, QHeaderView::ResizeToContents);
            hHeader->setSectionResizeMode(ProfilesTableModel::ColAddress, QHeaderView::Stretch);
            hHeader->setSectionResizeMode(ProfilesTableModel::ColName, QHeaderView::Stretch);
            hHeader->setSectionResizeMode(ProfilesTableModel::ColLatency, QHeaderView::ResizeToContents);
            hHeader->setSectionResizeMode(ProfilesTableModel::ColRxSpeed, QHeaderView::ResizeToContents);
            hHeader->setSectionResizeMode(ProfilesTableModel::ColConnectionTime, QHeaderView::ResizeToContents);
            hHeader->setSectionResizeMode(ProfilesTableModel::ColSiteScore, QHeaderView::ResizeToContents);
            hHeader->setSectionResizeMode(ProfilesTableModel::ColRxTraffic, QHeaderView::ResizeToContents);
            hHeader->setSectionResizeMode(ProfilesTableModel::ColTxTraffic, QHeaderView::ResizeToContents);
            // ResizeToContents only measures on-screen rows, so pin these columns to the
            // widest seen for this group or they jitter while scrolling.
            for (int col : {ProfilesTableModel::ColType, ProfilesTableModel::ColLatency,
                            ProfilesTableModel::ColRxSpeed, ProfilesTableModel::ColConnectionTime,
                            ProfilesTableModel::ColSiteScore, ProfilesTableModel::ColRxTraffic,
                            ProfilesTableModel::ColTxTraffic}) {
                if (group->calculated_column_width.size() > col &&
                    group->calculated_column_width[col] > hHeader->sectionSize(col)) {
                    hHeader->setSectionResizeMode(col, QHeaderView::Fixed);
                    hHeader->resizeSection(col, group->calculated_column_width[col]);
                }
            }
            ui->profilesTableView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            group->clearCalculatedColumnWidth();
            for (int i = 0; i < columnCount; i++) {
                auto size = hHeader->sectionSize(i);
                hHeader->setSectionResizeMode(i, QHeaderView::Interactive);
                hHeader->resizeSection(i, size);
                group->calculated_column_width << size;
            }
        } else {
            group->clearCalculatedColumnWidth();
            for (int i = 0; i < columnCount; i++) {
                hHeader->setSectionResizeMode(i, QHeaderView::Interactive);
                hHeader->resizeSection(i, group->column_width.at(i));
            }
            ui->profilesTableView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        }
        hHeader->adjustPositions();
        hHeader->blockSignals(false);
        vBar->blockSignals(vBarBlocked);
        m_adjustingColumns = false;
    });
}

void MainWindow::refresh_proxy_list(const QList<int>& ids, bool mayNeedReset, RefreshAnchor anchor) {
    if (!Configs::dataManager->settingsRepo->refreshing_group) saveProfileFocusState();
    refresh_proxy_list_impl(ids, mayNeedReset);
    if (mayNeedReset) restoreProfileFocusState(anchor);
}

void MainWindow::refresh_proxy_list_impl(const QList<int>& ids, bool mayNeedReset) {
    const auto currentGroup = Configs::dataManager->groupsRepo->CurrentGroup();
    if (currentGroup == nullptr)
    {
        MW_show_log("Could not find current group!");
        return;
    }
    // refresh data
    refresh_proxy_list_impl_refresh_data(ids, mayNeedReset);
    // now refresh column sizes
    refresh_proxy_list_column_size();
}

void MainWindow::refresh_proxy_list_impl_refresh_data(const QList<int>& ids, bool mayNeedReset) {
    const auto currentGroup = Configs::dataManager->groupsRepo->CurrentGroup();
    if (currentGroup == nullptr) return;
    // The model holds the group in full; the proxy decides what is on screen.
    if (!ids.isEmpty()) {
        for (auto id:ids) profilesTableModel->refreshProfileId(id);
        GroupSortAction action;
        action.descending = live_sort_descending;
        if (live_sort_column == ProfilesTableModel::ColType) {
            action.method = (Configs::dataManager->settingsRepo->show_config_security
                             && currentGroup->type_sort_by == Configs::typeBy::bySecurity)
                ? GroupSortMethod::BySecurity : GroupSortMethod::ByType;
        } else if (live_sort_column == ProfilesTableModel::ColAddress) {
            action.method = GroupSortMethod::ByAddress;
        } else if (live_sort_column == ProfilesTableModel::ColName) {
            action.method = GroupSortMethod::ByName;
        } else if (live_sort_column >= ProfilesTableModel::ColLatency
                   && live_sort_column <= ProfilesTableModel::ColSiteScore) {
            action.method = GroupSortMethod::ByTestResult;
        } else if (live_sort_column == ProfilesTableModel::ColRxTraffic
                   || live_sort_column == ProfilesTableModel::ColTxTraffic) {
            action.method = GroupSortMethod::ByTraffic;
        } else {
            return;
        }
        if (currentGroup->SortProfiles(action)) {
            profilesTableModel->reorderProfiles(currentGroup->Profiles());
        }
    } else {
        profilesTableModel->refreshTable(currentGroup->profiles, mayNeedReset);
    }
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

        runOnUiThread([=,this] {
            if (!result.results[0].error.value().empty()) {
                MW_show_log(QString("UrlTest error: %1").arg(QString::fromStdString(result.results[0].error.value())));
            }
            if (latency <= 0) {
                MW_show_log(tr("Test Result") + ": " + tr("Unavailable"));
            } else if (latency > 0) {
                MW_show_log(tr("Test Result") + ": " + QString("%1 ms").arg(latency));
            }
        });
    });
}
