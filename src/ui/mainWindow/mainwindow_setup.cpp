#include "include/ui/mainwindow.h"

#include "include/ui/mainWindow/MainWindowInternal.h"
// Full definition: MainWindow's destructor lives here and destroys the unique_ptr.
#include "include/ui/mainWindow/TestRunner.h"

#include <QCursor>
#include <QMenu>

#include "include/configs/sub/GroupUpdater.hpp"
#include "include/configs/sub/RouteUpdater.hpp"
#include "include/global/PeriodicRunner.hpp"
#include "include/global/Logger.hpp"
#include "include/stats/autoselector/AutoSelectorMonitor.hpp"
#include "include/ui/stats/dialog_auto_selector.h"
#include "include/sys/Process.hpp"
#include "include/sys/AutoRun.hpp"
#include "include/sys/UrlScheme.hpp"

#include "include/ui/utils/ConnectionsFilterHeader.h"
#include "include/ui/setting/ThemeManager.hpp"
#include "include/ui/setting/Icon.hpp"
#include "include/ui/stats/dialog_traffic_stats.h"
#include "include/ui/stats/dialog_runtime_stats.h"
#include "include/ui/widget/StartStopButton.hpp"

#include "include/configs/generate.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/global/Common.h"

#include "include/ui/utils/ProfilesTableFilterHeader.h"
#include "include/ui/utils/ProfilesTableModel.h"

#include "include/ui/group/dialog_edit_group.h"

#ifdef Q_OS_WIN
#include <windows.h>
// <windows.h> defines SetPort, which under unity builds clobbers Configs::outbound::SetPort.
#undef SetPort
#else
#ifdef Q_OS_LINUX
#include <QDBusInterface>
#include <QDBusReply>
#include <sys/socket.h>
#endif
#ifdef Q_OS_MACOS
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <unistd.h>
#endif

#include <QUuid>

#include <algorithm>
#include <memory>

#include <QClipboard>
#include <QScrollBar>
#include <QDesktopServices>
#include <QTimer>
#include <QMessageBox>
#include <QDir>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif
#include <QFileDialog>
#include <QHBoxLayout>
#include <QToolButton>
#include <QStyle>
#include <QSizePolicy>
#include <QTabBar>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QStyledItemDelegate>
#include <QStyleOptionButton>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QListWidget>
#include <QSplitter>
#include <include/global/HTTPRequestHelper.hpp>
#include "include/global/DeviceDetailsHelper.hpp"

namespace {
class CenteredCheckBoxDelegate final : public QStyledItemDelegate {
public:
    explicit CenteredCheckBoxDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    static QRect indicatorRect(const QStyleOptionViewItem &option) {
        QStyleOptionButton checkbox;
        checkbox.state = QStyle::State_Enabled;
        const QRect indicator = QApplication::style()->subElementRect(
            QStyle::SE_CheckBoxIndicator, &checkbox);
        return QStyle::alignedRect(option.direction, Qt::AlignCenter,
                                   indicator.size(), option.rect);
    }

protected:
    void initStyleOption(QStyleOptionViewItem *option,
                         const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.data(ProfilesTableModel::GroupHeaderRole).toBool()) return;
        option->features &= ~QStyleOptionViewItem::HasCheckIndicator;
        option->features &= ~QStyleOptionViewItem::HasDisplay;
        option->text.clear();
    }

public:
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        const QVariant checkState = index.data(Qt::CheckStateRole);
        if (!checkState.isValid()) return;

        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        QStyleOptionButton checkbox;
        checkbox.state = QStyle::State_Enabled |
            (checkState.toInt() == Qt::Checked ? QStyle::State_On : QStyle::State_Off);
        checkbox.rect = indicatorRect(opt);
        QApplication::style()->drawControl(QStyle::CE_CheckBox, &checkbox, painter);
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override {
        if (!(index.flags() & Qt::ItemIsEnabled)
            || !(index.flags() & Qt::ItemIsUserCheckable)) {
            return false;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() != Qt::LeftButton
                || !indicatorRect(option).contains(mouseEvent->position().toPoint())) {
                return false;
            }
        } else if (event->type() == QEvent::KeyPress) {
            const auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() != Qt::Key_Space && keyEvent->key() != Qt::Key_Select) {
                return false;
            }
        } else {
            return false;
        }

        const auto current = static_cast<Qt::CheckState>(
            index.data(Qt::CheckStateRole).toInt());
        return model->setData(index,
            current == Qt::Checked ? Qt::Unchecked : Qt::Checked,
            Qt::CheckStateRole);
    }
};
}

void UI_InitMainWindow() {
    mainwindow = new MainWindow;
}

// Caller must hold coreProcessMutex (reads core_process lock-free by design).
bool MainWindow::verify_core_pid(QLocalSocket *socket) {
    if (!core_process) return false;
    qint64 expectedPid = core_process->processId();
    if (expectedPid <= 0) return false;

#if defined(Q_OS_LINUX)
    struct ucred cred = {};
    socklen_t credLen = sizeof(cred);
    if (getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &cred, &credLen) == 0) {
        return static_cast<qint64>(cred.pid) == expectedPid;
    }
    return false;
#elif defined(Q_OS_MACOS)
    pid_t pid = 0;
    socklen_t pidLen = sizeof(pid);
    if (getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_LOCAL, LOCAL_PEERPID, &pid, &pidLen) == 0) {
        return static_cast<qint64>(pid) == expectedPid;
    }
    return false;
#elif defined(Q_OS_WIN)
    ULONG pid = 0;
    HANDLE hPipe = reinterpret_cast<HANDLE>(static_cast<qintptr>(socket->socketDescriptor()));
    if (GetNamedPipeClientProcessId(hPipe, &pid)) {
        return static_cast<qint64>(pid) == expectedPid;
    }
    return false;
#else
    Q_UNUSED(socket)
    return true;
#endif
}

static bool themeUsesDarkLog(const QString &theme) {
    const auto lower = theme.toLower();
    if (lower.contains("vista") || lower.contains("flatgray") || lower.contains("lightblue") || lower.contains("softpink")) {
        return false;
    }
    if (lower.contains("qdarkstyle") || lower.contains("blacksoft")) {
        return true;
    }
    return isDarkMode();
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow) {
    mainwindow = this;
    setAcceptDrops(true);
    MW_dialog_message = [=,this](MwMessage cmd, QStringList args) {
        runOnUiThread([=,this]
        {
            dialog_message_impl(cmd, args);
        });
    };
    MW_handle_deeplink = [=,this](const QString &url) {
        runOnUiThread([=,this]
        {
            handle_deeplink_impl(url);
        });
    };
    MW_import_files = [=,this](const QStringList &paths) {
        runOnUiThread([=,this]
        {
            importFromFiles(paths);
        });
    };

    AutoRun_FixTaskIfNeeded();
    AutoRun_MigrateIfNeeded();

    UrlScheme_RegisterIfNeeded();

    // migrate old themes
    bool isNum;
    Configs::dataManager->settingsRepo->theme.toInt(&isNum);
    if (isNum) {
        Configs::dataManager->settingsRepo->theme = "System";
    }
    themeManager->ApplyTheme(Configs::dataManager->settingsRepo->theme);
    ui->setupUi(this);
    // The right-hand pane currently has exactly one page. Hiding the tab bar
    // makes the Proxy and Host cards start at the same height as the lower
    // tools instead of wasting a row on a non-actionable "Info" tab.
    ui->info_widget->tabBar()->hide();
    ui->infoLayout->setContentsMargins(8, 0, 8, 8);
    infoTabAlignSpacer = new QWidget(ui->info_tab);
    infoTabAlignSpacer->setObjectName(QStringLiteral("infoTabAlignSpacer"));
    infoTabAlignSpacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    ui->infoLayout->insertWidget(0, infoTabAlignSpacer);

    const auto setupInfoIpCopyButton = [](QToolButton *button, QLabel *valueLabel) {
        const auto makeCopyIcon = [](const QColor &color) {
            QPixmap pix(16, 16);
            pix.fill(Qt::transparent);
            QPainter painter(&pix);
            painter.setRenderHint(QPainter::Antialiasing);
            QPen pen(color, 1.4);
            pen.setJoinStyle(Qt::MiterJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(5, 2, 7, 7);
            painter.drawRect(2, 5, 7, 7);
            return QIcon(pix);
        };
        button->setIcon(makeCopyIcon(button->palette().color(QPalette::ButtonText)));
        button->setIconSize(QSize(14, 14));
        QObject::connect(button, &QToolButton::clicked, valueLabel, [valueLabel] {
            const QString ip = valueLabel->text().trimmed();
            if (ip.isEmpty() || ip == QStringLiteral("—")) return;
            QApplication::clipboard()->setText(ip);
        });
    };
    setupInfoIpCopyButton(ui->proxy_ip_copy, ui->proxy_ip_value);
    setupInfoIpCopyButton(ui->host_ip_copy, ui->host_ip_value);

    // Keep application diagnostics separate from the extremely chatty core.
    // The existing browser becomes App logs so all existing UI call sites keep
    // their simple MW_show_log() API; core stdout/stderr use the inserted page.
    ui->stats_widget->setTabText(ui->stats_widget->indexOf(ui->Logs), tr("App logs"));
    auto *coreLogPage = new QWidget(ui->stats_widget);
    auto *coreLogLayout = new QHBoxLayout(coreLogPage);
    coreLogLayout->setContentsMargins(1, 0, 1, 0);
    coreLogLayout->setSpacing(0);
    coreLogBrowser = new QTextBrowser(coreLogPage);
    coreLogBrowser->setObjectName(QStringLiteral("coreLogBrowser"));
    coreLogBrowser->setContextMenuPolicy(Qt::CustomContextMenu);
    coreLogBrowser->setOpenLinks(false);
    coreLogLayout->addWidget(coreLogBrowser);
    ui->stats_widget->insertTab(0, coreLogPage, tr("Core logs"));
    syncInfoPanelTop();

    setActionsData();
    loadShortcuts();

    last_running_profile_id = Configs::dataManager->settingsRepo->remember_id;

    if (!Configs::dataManager->settingsRepo->mainWindowGeometry.isEmpty()) {
        auto geo = DecodeB64IfValid(Configs::dataManager->settingsRepo->mainWindowGeometry);
        this->restoreGeometry(geo);
    }

    setLogHighlighter(themeUsesDarkLog(Configs::dataManager->settingsRepo->theme));
    qvLogDocument->setUndoRedoEnabled(false);
    const int maxUiLogLines = qMax(1, Configs::dataManager->settingsRepo->max_log_line);
    qvLogDocument->setMaximumBlockCount(maxUiLogLines);
    coreLogDocument->setUndoRedoEnabled(false);
    coreLogDocument->setMaximumBlockCount(maxUiLogLines);
    ui->masterLogBrowser->setUndoRedoEnabled(false);
    ui->masterLogBrowser->setDocument(qvLogDocument);
    coreLogBrowser->setUndoRedoEnabled(false);
    coreLogBrowser->setDocument(coreLogDocument);
    applyLogBrowserFont();
    updateLogFilterFields();
    m_liveResizeTimer = new QTimer(this);
    m_liveResizeTimer->setSingleShot(true);
    m_liveResizeTimer->setInterval(15);
    connect(m_liveResizeTimer, &QTimer::timeout, this, [this] { endLiveResize(); });
    runOnThread([=, this] {
        log_process_loop();
    }, LogThread);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [=,this](const Qt::ColorScheme& scheme) {
        setLogHighlighter(scheme == Qt::ColorScheme::Dark);
        themeManager->ApplyTheme(Configs::dataManager->settingsRepo->theme, true);
    });
#endif
    connect(themeManager, &ThemeManager::themeChanged, this, [=,this](const QString& theme){
        setLogHighlighter(themeUsesDarkLog(theme));
        scheduleProxyListRefresh();
    });
    MW_show_log = [=,this](const QString &log) {
        append_log(log);
        Logging::WriteUserLog(log);
    };
    MW_show_core_log = [=,this](const QString &log) {
        append_core_log(log);
        Logging::WriteUserLog(log);
    };

    connect(coreLogBrowser, &QTextBrowser::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu *menu = coreLogBrowser->createStandardContextMenu();
        auto *separator = new QAction(this);
        separator->setSeparator(true);
        menu->addAction(separator);
        auto *clear = new QAction(tr("Clear"), menu);
        connect(clear, &QAction::triggered, this, [this] {
            QMutexLocker lock(&logPendingMutex);
            coreLogPendingText.clear();
            coreLogDocument->clear();
        });
        menu->addAction(clear);
        menu->exec(coreLogBrowser->viewport()->mapToGlobal(pos));
    });

    if (Configs::dataManager->settingsRepo->random_inbound_port)
    {
        Configs::dataManager->settingsRepo->inbound_socks_port = MkPort(Configs::dataManager->settingsRepo->inbound_address);
    }

    runOnNewThread([=, this] {GetDeviceDetails(); });

    auto core_path = QApplication::applicationDirPath() + "/";
    core_path += "ThroneCore";

    bool coreDebugMode = (Configs::dataManager->settingsRepo->log_level == "debug");

    Configs::dataManager->settingsRepo->core_socket_name =
        "throneIPC-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    core_server = new QLocalServer(this);
    core_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!core_server->listen(Configs::dataManager->settingsRepo->core_socket_name)) {
        qWarning() << "Failed to start IPC server:" << core_server->errorString();
        qApp->quit();
    }

    connect(core_server, &QLocalServer::newConnection, this, [=, this]() {
        auto socket = core_server->nextPendingConnection();
        int profileId = -1;
        {
            // Hold coreProcessMutex: DS_cores may still be constructing core_process.
            QMutexLocker lock(&coreProcessMutex);
            if (!verify_core_pid(socket)) {
                MW_show_log("[Warn] IPC connection from unexpected process rejected");
                socket->close();
                socket->deleteLater();
                return;
            }
            if (core_process) {
                profileId = core_process->start_profile_when_core_is_up;
                core_process->start_profile_when_core_is_up = -1;
            }
        }
        setup_rpc(socket);
        Configs::dataManager->settingsRepo->core_running = true;
        LOG_INFO(QString("elevated: %1").arg(Configs::IsAdmin() ? "yes" : "no"));
        MW_dialog_message(MwMessage::CoreStarted, {Int2String(profileId)});
    });

    auto socketFullName = core_server->fullServerName();
    runOnThread(
        [=, this] {
            QMutexLocker lock(&coreProcessMutex);
            core_process = new Configs_sys::CoreProcess(core_path, socketFullName, coreDebugMode);
            core_process->Start();
        },
        DS_cores);

    if (!Configs::dataManager->settingsRepo->font.isEmpty()) {
        auto font = qApp->font();
        font.setFamily(Configs::dataManager->settingsRepo->font);
        qApp->setFont(font);
    }
    if (Configs::dataManager->settingsRepo->font_size != 0) {
        auto font = qApp->font();
        font.setPointSize(Configs::dataManager->settingsRepo->font_size);
        qApp->setFont(font);
    }

    parallelCoreCallPool->setMaxThreadCount(10);
    testRunner = std::make_unique<TestRunner>(this);
    // A profile can stay up for hours while its egress changes or disappears.
    // Re-test the real one-profile egress regularly; never interrupt a manual
    // test session just to refresh the Info panel.
    connectionProbeTimer = new QTimer(this);
    connectionProbeTimer->setSingleShot(true);
    connect(connectionProbeTimer, &QTimer::timeout, this, &MainWindow::runCurrentConnectionProbe);
    connectionProbeTimer->start(10'000);
    //
    // The .ui carries Return; numpad Enter is the same gesture.
    ui->menu_start->setShortcuts({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    connect(ui->menu_start, &QAction::triggered, this, [=,this]() { profile_start(); });
    connect(ui->menu_stop, &QAction::triggered, this, [=,this]() { profile_stop(false, false, true); });
#ifdef Q_OS_MACOS
    // The main menu is intentionally hidden in Waterdisco, so macOS has no
    // native Quit item to provide this standard application shortcut for us.
    auto *quitShortcut = new QShortcut(QKeySequence::Quit, this);
    quitShortcut->setContext(Qt::ApplicationShortcut);
    connect(quitShortcut, &QShortcut::activated, this, &MainWindow::on_menu_exit_triggered);
#endif
    connect(ui->toolButton_startstop, &QAbstractButton::clicked, this, [=,this]() {
        // The button is disabled while Connecting, so a click is stop-running or start-selected.
        if (running != nullptr) profile_stop(false, false, true);
        else profile_start();
    });
    // The sidebar owns group navigation. Keep the original vertical splitter
    // intact inside a horizontal splitter so the lower tools begin exactly at
    // the table edge instead of underneath the sidebar.
    mainContentSplitter = new QSplitter(Qt::Horizontal, ui->centralwidget);
    mainContentSplitter->setChildrenCollapsible(false);
    auto *sidebarPane = new QWidget(mainContentSplitter);
    auto *sidebarLayout = new QVBoxLayout(sidebarPane);
    // The central layout already supplies the left gutter. Mirror it on the
    // table side so both the group list and its Update button do not touch the
    // splitter while still sharing the sidebar's full usable width.
    sidebarLayout->setContentsMargins(0, 0, 6, 0);
    sidebarLayout->setSpacing(3);
    auto *sidebarActions = new QHBoxLayout;
    sidebarActions->setContentsMargins(0, 0, 0, 0);
    sidebarActions->setSpacing(3);
    auto *newGroupButton = new QToolButton(sidebarPane);
    newGroupButton->setText(tr("New group"));
    newGroupButton->setToolTip(tr("Create a new group"));
    newGroupButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(newGroupButton, &QToolButton::clicked,
            ui->actionAdd_New_Group, &QAction::trigger);
    sidebarActions->addWidget(newGroupButton, 1);

    auto *updateAllButton = new QToolButton(sidebarPane);
    updateAllButton->setText(tr("Update"));
    updateAllButton->setToolTip(tr("Update all subscriptions"));
    updateAllButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(updateAllButton, &QToolButton::clicked,
            ui->actionUpdate_All_Subscriptions, &QAction::trigger);
    sidebarActions->addWidget(updateAllButton, 1);
    sidebarLayout->addLayout(sidebarActions);

    groupSidebar = new QListWidget(sidebarPane);
    groupSidebar->setSelectionMode(QAbstractItemView::SingleSelection);
    groupSidebar->setDragDropMode(QAbstractItemView::InternalMove);
    groupSidebar->setDefaultDropAction(Qt::MoveAction);
    groupSidebar->setDragEnabled(true);
    groupSidebar->setAcceptDrops(true);
    groupSidebar->setDropIndicatorShown(true);
    groupSidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    groupSidebar->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    groupSidebar->setFrameShape(QFrame::NoFrame);
    groupSidebar->setSpacing(1);
    groupSidebar->setToolTip(tr("Drag groups to reorder"));
    groupSidebar->setMinimumWidth(150);
    sidebarLayout->addWidget(groupSidebar, 1);

    auto *rootLayout = qobject_cast<QVBoxLayout *>(ui->centralwidget->layout());
    const int splitterIndex = rootLayout->indexOf(ui->splitter);
    rootLayout->removeWidget(ui->splitter);
    mainContentSplitter->addWidget(sidebarPane);
    mainContentSplitter->addWidget(ui->splitter);
    rootLayout->insertWidget(splitterIndex, mainContentSplitter, 1);
    mainContentSplitter->setStretchFactor(0, 0);
    mainContentSplitter->setStretchFactor(1, 1);
    sidebarPane->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    // The group list sizes itself to its labels. The handle would only let the
    // New/Update row stretch into empty space, so do not let it move.
    if (auto *handle = mainContentSplitter->handle(1)) {
        handle->setEnabled(false);
        handle->setCursor(Qt::ArrowCursor);
    }

    auto *profilePane = new QWidget(ui->splitter);
    auto *profileLayout = new QVBoxLayout(profilePane);
    profileLayout->setContentsMargins({1, 0, 1, 0});
    profileLayout->setSpacing(0);
    ui->profilesTableView->setParent(profilePane);
    profileLayout->addWidget(ui->profilesTableView);
    ui->tabWidget->setParent(nullptr);
    ui->tabWidget->deleteLater();
    ui->splitter->insertWidget(0, profilePane);
    ui->splitter->restoreState(DecodeB64IfValid(Configs::dataManager->settingsRepo->splitter_state));
    QTimer::singleShot(0, this, [this] {
        const auto sizes = ui->splitter->sizes();
        if (sizes.size() != 2 || sizes[1] <= 0) return;
        const int lower = qMax(1, static_cast<int>(sizes[1] / 1.7));
        ui->splitter->setSizes({sizes[0] + sizes[1] - lower, lower});
    });

    ui->splitter->installEventFilter(this);
    //
    btnProfilesListMode = new QToolButton(this);
    btnProfilesListMode->setCheckable(true);
    btnProfilesListMode->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));
    const bool flatList = Configs::dataManager->settingsRepo->profiles_flat_list;
    btnProfilesListMode->setChecked(flatList);
    auto updateListModeTip = [this](bool flat) {
        btnProfilesListMode->setToolTip(flat
            ? tr("Show group sections")
            : tr("Show as a single list"));
    };
    updateListModeTip(flatList);
    connect(btnProfilesListMode, &QToolButton::toggled, this, [this, updateListModeTip](bool flat) {
        auto *settings = Configs::dataManager->settingsRepo.get();
        settings->profiles_flat_list = flat;
        settings->Save();
        updateListModeTip(flat);
        if (!profilesTableModel) return;
        profilesTableModel->setFlatList(flat);
        profilesTableModel->clearGlobalOrder();
        selectAllGroupId = -1;
        selectAllIsGlobal = false;
        refresh_proxy_list({}, true);
    });
    ui->horizontalLayout_2->addWidget(btnProfilesListMode, 0, Qt::AlignRight | Qt::AlignVCenter);
    auto btnFilter = new QToolButton(this);
    btnFilter->setIcon(QIcon(":/icon/filter.png"));
    btnFilter->setToolTip(QString("%1\n%2").arg(tr("Enable Filter"), QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText)));
    btnFilter->setShortcut(QKeySequence::Find);
    btnFilter->setCheckable(true);
    connect(btnFilter, &QToolButton::toggled, static_cast<ProfilesTableFilterHeader*>(ui->profilesTableView->horizontalHeader()), &ProfilesTableFilterHeader::setFiltersVisible);
    connect(static_cast<ProfilesTableFilterHeader*>(ui->profilesTableView->horizontalHeader()), &ProfilesTableFilterHeader::closeRequested,
            btnFilter, [btnFilter] { btnFilter->setChecked(false); });
    ui->horizontalLayout_2->addWidget(btnFilter, 0, Qt::AlignRight | Qt::AlignVCenter);
    //
    RegisterHotkey(false);
    auto last_size = Configs::dataManager->settingsRepo->mw_size.split("x");
    if (last_size.length() == 2) {
        auto w = last_size[0].toInt();
        auto h = last_size[1].toInt();
        if (w > 0 && h > 0) {
            resize(w, h);
        }
    }

    // software_name
    software_name = "Waterdisco";
    software_core_name = "sing-box";
    if (auto dashDir = QDir("dashboard"); !dashDir.exists() && QDir().mkdir("dashboard")) {
        if (auto dashFile = QFile(":/Throne/dashboard-notice.html"); dashFile.exists() && dashFile.open(QIODevice::ReadOnly))
        {
            auto data = dashFile.readAll();
            if (auto dest = QFile("dashboard/index.html"); dest.open(QIODevice::Truncate | QIODevice::WriteOnly))
            {
                dest.write(data);
                dest.close();
            }
            dashFile.close();
        }
    }
    // Leaving the dir non-empty marks it user-provided, disabling the core's own updater.
    SeedDashboard();
    if (auto iconsDir = QDir("icons"); !iconsDir.exists()) {
        QDir().mkdir("icons") ? qDebug("created icons dir") : qDebug("Failed to create icons dir");
    }

    ui->toolButton_program->setMenu(ui->menu_program);
    ui->toolButton_preferences->setMenu(ui->menu_preferences);
    ui->toolButton_routing->setMenu(ui->menuRouting_Menu);
    ui->toolButton_testing->setMenu(ui->menuTesting);
    ui->toolButton_tools->setMenu(ui->menuTools);
    ui->toolButton_program->installEventFilter(this);

    designMinimumSize = minimumSize();
    applyTopBarMetrics();

    ui->menubar->setVisible(false);
    connect(ui->actionRuntime_Stats, &QAction::triggered, this, [=, this]() {
        USE_DIALOG(DialogRuntimeStats)
    });
    ui->actionTraffic_Stats->setVisible(!Configs::dataManager->settingsRepo->disable_traffic_aggregation);
    connect(ui->actionTraffic_Stats, &QAction::triggered, this, [=, this]() {
        USE_DIALOG(DialogTrafficStats)
    });
    // refresh_auto_selector_view shows and hides this as the selector monitor starts and stops.
    ui->actionAuto_Selector->setVisible(false);
    connect(ui->actionAuto_Selector, &QAction::triggered, this, [=,this]() {
        if (m_autoSelectorDialog == nullptr) {
            m_autoSelectorDialog = new DialogAutoSelector(this);
            connect(m_autoSelectorDialog, &QDialog::finished, this, [this] {
                m_autoSelectorDialog->deleteLater();
                m_autoSelectorDialog = nullptr;
            });
        }
        m_autoSelectorDialog->refresh();
        m_autoSelectorDialog->show();
        m_autoSelectorDialog->raise();
        m_autoSelectorDialog->activateWindow();
    });
    connect(ui->actionCheck_For_Update, &QAction::triggered, this, [=,this] { runOnNewThread([=,this] { CheckUpdate(); }); });
    if (!QFile::exists(QApplication::applicationDirPath() + "/updater") && !QFile::exists(QApplication::applicationDirPath() + "/updater.exe"))
    {
        ui->actionCheck_For_Update->setDisabled(true);
    }

    setupConnectionList();
    ui->stats_widget->tabBar()->setCurrentIndex(Configs::dataManager->settingsRepo->stats_tab);
    connect(ui->stats_widget->tabBar(), &QTabBar::currentChanged, this, [=,this](int index)
    {
        Configs::dataManager->settingsRepo->stats_tab = ui->stats_widget->tabBar()->currentIndex();
        syncConnectionViewState();
    });
    syncConnectionViewState();
    connect(ui->connections->horizontalHeader(), &QHeaderView::sectionClicked, this, [=,this](int index)
    {
            // The close column has no sort of its own; without this it would fall through and reset sorting.
            if (index == ConnectionsFilterHeader::ColClose) return;

            Stats::ConnectionSort sortType;

            switch (index)
            {
            case 1: sortType = Stats::ByProcess; break;
            case 2: sortType = Stats::ByProtocol; break;
            case 3: sortType = Stats::ByOutbound; break;
            case 4: sortType = Stats::ByTraffic; break;
            case 5: sortType = Stats::BySpeed; break;
            default: sortType = Stats::Default; break;
            }

            applyConnectionSort(sortType);
    });

    speedChartWidget = new SpeedWidget(this);
    ui->graph_tab->layout()->addWidget(speedChartWidget);

    profilesTableModel = new ProfilesTableModel(this);
    profilesTableModel->setFlatList(Configs::dataManager->settingsRepo->profiles_flat_list);
    live_sort_column = Configs::dataManager->settingsRepo->profiles_sort_column;
    live_sort_descending = Configs::dataManager->settingsRepo->profiles_sort_descending;
    if (!isLiveSortableColumn(live_sort_column)) {
        live_sort_column = ProfilesTableModel::ColSiteScore;
        live_sort_descending = true;
    }
    profilesFilterModel = new ProfilesFilterProxyModel(this);
    profilesFilterModel->setSourceModel(profilesTableModel);
    ui->profilesTableView->setModel(profilesFilterModel);
    // Row numbers are a real compact column, not a detached vertical header.
    // This gives them the same horizontal separators and selection background
    // as every other cell.
    ui->profilesTableView->verticalHeader()->hide();
    ui->profilesTableView->selectAllRequested = [this] { selectAllProfiles(); };
    connect(profilesFilterModel, &QAbstractItemModel::modelReset, this,
            [this] { QTimer::singleShot(0, this, [this] { applyGroupSectionSpans(); }); });
    connect(groupSidebar, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (refreshingGroupSidebar || !item) return;
        show_group(item->data(Qt::UserRole).toInt());
    });
    connect(groupSidebar, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (refreshingGroupSidebar || !item) return;
        edit_group(item->data(Qt::UserRole).toInt());
    });
    groupSidebar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(groupSidebar, &QListWidget::customContextMenuRequested, this, &MainWindow::showGroupSidebarMenu);
    connect(groupSidebar->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        if (refreshingGroupSidebar) return;
        QList<int> order;
        for (int row = 0; row < groupSidebar->count(); ++row) {
            order.append(groupSidebar->item(row)->data(Qt::UserRole).toInt());
        }
        Configs::dataManager->groupsRepo->SetGroupsTabOrder(order);
        refresh_proxy_list({}, true);
    });
    ui->profilesTableView->setItemDelegateForColumn(
        ProfilesTableModel::ColStartup,
        new CenteredCheckBoxDelegate(ui->profilesTableView));
    ui->profilesTableView->setItemDelegateForColumn(
        ProfilesTableModel::ColDisabled,
        new CenteredCheckBoxDelegate(ui->profilesTableView));
    ui->profilesTableView->setColumnHidden(ProfilesTableModel::ColAddress, true);
    ui->profilesTableView->setColumnHidden(ProfilesTableModel::ColLatency, true);
    connect(profilesTableModel, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                   const QList<int> &roles) {
        if (!roles.contains(Qt::CheckStateRole)
            || topLeft.column() > ProfilesTableModel::ColDisabled
            || bottomRight.column() < ProfilesTableModel::ColDisabled) {
            return;
        }
        // Defer the reset until the checkbox editor has returned from setData.
        const int groupId = topLeft.data(ProfilesTableModel::GroupIdRole).toInt();
        QTimer::singleShot(0, this, [this, groupId] {
            auto group = Configs::dataManager->groupsRepo->GetGroup(groupId);
            if (!group) return;
            GroupSortAction action;
            action.method = GroupSortMethod::Raw;
            if (group->SortProfiles(action)) {
                Configs::dataManager->groupsRepo->Save(group);
                profilesTableModel->clearGlobalOrder();
                refresh_proxy_list({}, true);
            }
            const auto selected = ui->profilesTableView->selectionModel()->selectedRows(0);
            for (const QModelIndex &index : selected) {
                const int id = index.data(ProfilesTableModel::ProfileIdRole).toInt();
                if (Configs::dataManager->settingsRepo->IsProfileDisabled(id)) {
                    ui->profilesTableView->selectionModel()->select(
                        index, QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
                }
            }
            refresh_startstop_button();
        });
    });
    // Keep the start/stop button's enabled/disabled state in sync with selection.
    connect(ui->profilesTableView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] {
                if (!handlingSelectAll) {
                    selectAllGroupId = -1;
                    selectAllIsGlobal = false;
                }
                refresh_startstop_button();
            });
    ui->profilesTableView->rowsSwapped = [this](int row1, int row2)
    {
        // A drop position in a filtered list says nothing about the persisted
        // order. Nor may a profile cross a section divider by drag-and-drop.
        if (profilesFilterModel->hasActiveFilter()) return;
        if (row1 == row2) return;
        if (profilesTableModel->isGroupHeader(row1) || profilesTableModel->isGroupHeader(row2)) return;
        const int groupId = profilesTableModel->groupIdAt(row1);
        if (groupId < 0 || groupId != profilesTableModel->groupIdAt(row2)) return;
        const int movedId = profilesTableModel->data(
            profilesTableModel->index(row1, ProfilesTableModel::ColStartup),
            ProfilesTableModel::ProfileIdRole).toInt();
        if (Configs::dataManager->settingsRepo->IsProfileDisabled(movedId)) return;

        const auto group = Configs::dataManager->groupsRepo->GetGroup(groupId);
        if (!group) return;
        QList<int> ordered = group->Profiles();
        int from = ordered.indexOf(movedId);
        const int targetId = profilesTableModel->data(
            profilesTableModel->index(row2, ProfilesTableModel::ColStartup),
            ProfilesTableModel::ProfileIdRole).toInt();
        int to = ordered.indexOf(targetId);
        if (from < 0 || to < 0) return;
        int firstDisabledRow = ordered.size();
        for (int row = 0; row < ordered.size(); ++row) {
            const int id = ordered[row];
            if (Configs::dataManager->settingsRepo->IsProfileDisabled(id)) {
                firstDisabledRow = row;
                break;
            }
        }
        if (firstDisabledRow > 0) to = std::min(to, firstDisabledRow - 1);
        group->EmplaceProfile(from, to);
        profilesTableModel->emplaceProfiles(row1, row2);
        Configs::dataManager->groupsRepo->Save(group);
    };
    connect(ui->profilesTableView->horizontalHeader(), &QHeaderView::sectionClicked, this, [=, this](int logicalIndex) {
        if (logicalIndex == ProfilesTableModel::ColStartup
            || logicalIndex == ProfilesTableModel::ColDisabled) {
            const QList<int> ids = profilesTableModel->allProfileIds();
            if (ids.isEmpty()) return;
            auto *settings = Configs::dataManager->settingsRepo.get();
            if (logicalIndex == ProfilesTableModel::ColStartup) {
                QList<int> enabled;
                for (int id : ids) {
                    if (!settings->IsProfileDisabled(id)) enabled.append(id);
                }
                const bool allChecked = !enabled.isEmpty()
                    && std::all_of(enabled.cbegin(), enabled.cend(),
                                   [settings](int id) { return settings->IsStartupProfile(id); });
                for (int id : enabled) settings->SetStartupProfile(id, !allChecked);
                settings->Save();
                profilesTableModel->refreshTable();
                return;
            } else {
                const bool allChecked = !ids.isEmpty()
                    && std::all_of(ids.cbegin(), ids.cend(),
                                   [settings](int id) { return settings->IsProfileDisabled(id); });
                for (int id : ids) settings->SetProfileDisabled(id, !allChecked);
            }
            settings->Save();
            // Disabled entries must remain at the bottom of every section even
            // though the header now controls the one unified table.
            GroupSortAction action;
            action.method = GroupSortMethod::Raw;
            for (int groupId : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
                const auto group = Configs::dataManager->groupsRepo->GetGroup(groupId);
                if (group && group->SortProfiles(action)) {
                    Configs::dataManager->groupsRepo->Save(group);
                }
            }
            profilesTableModel->clearGlobalOrder();
            refresh_proxy_list({}, true);
            ui->profilesTableView->clearSelection();
            refresh_startstop_button();
            return;
        }
        GroupSortAction action;
        Configs::testBy testSortBy;
        Configs::trafficBy trafficSortBy;
        const bool sameColumn = live_sort_column == logicalIndex;
        const bool descending = sameColumn
            ? !live_sort_descending
            : logicalIndex == ProfilesTableModel::ColSiteScore;
        if (!sortActionFromColumn(logicalIndex, descending, action, testSortBy, trafficSortBy)) {
            return;
        }
        proxy_last_order = logicalIndex;
        live_sort_column = logicalIndex;
        live_sort_descending = descending;
        Configs::typeBy typeSortBy = Configs::typeBy::byType;
        if (const auto current = Configs::dataManager->groupsRepo->CurrentGroup()) {
            typeSortBy = current->type_sort_by;
        }
        stampSortPrefsOnGroups(testSortBy, trafficSortBy, typeSortBy);
        persistLiveSortPrefs();
        applyLiveSortIndicator();
        runProfilesSort(action, testSortBy, trafficSortBy);
    });
    connect(ui->profilesTableView->horizontalHeader(), &QHeaderView::sectionResized, this, [=, this](int, int, int) {
        updateImproveMoodGeometry();
        if (Configs::dataManager->settingsRepo->refreshing_group || m_adjustingColumns) return;
        m_columnWidthsAutoSized = false;
        QList<int> widths;
        for (int i = 0; i < ui->profilesTableView->horizontalHeader()->count(); i++)
            widths.push_back(ui->profilesTableView->horizontalHeader()->sectionSize(i));
        // There is one shared header now, so keep its dimensions identical for
        // every group instead of making a sidebar click unexpectedly reshape it.
        for (int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
            auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            if (!group) continue;
            group->column_width = widths;
            Configs::dataManager->groupsRepo->Save(group);
        }
    });
    ui->profilesTableView->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->profilesTableView->horizontalHeader(), &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* header = ui->profilesTableView->horizontalHeader();
        int columnIndex = header->logicalIndexAt(pos);
        auto group = Configs::dataManager->groupsRepo->CurrentGroup();
        if (group == nullptr) return;
        if (columnIndex == ProfilesTableModel::ColType) {
            if (!Configs::dataManager->settingsRepo->show_config_security) return;
            QMenu menu(this);
            auto* sortByLabel = menu.addAction(tr("Sort By:"));
            sortByLabel->setEnabled(false);

            struct TypeSortOption { Configs::typeBy value; QString label; };
            const QList<TypeSortOption> options = {
                { Configs::typeBy::byType, tr("Type") },
                { Configs::typeBy::bySecurity, tr("Security") },
            };
            for (const auto& opt : options) {
                auto* act = menu.addAction(opt.label);
                act->setData(static_cast<int>(opt.value));
                act->setCheckable(true);
                act->setChecked(group->type_sort_by == opt.value);
            }

            auto* chosen = menu.exec(header->mapToGlobal(pos));
            if (chosen == nullptr || !chosen->data().isValid()) return;

            const auto typeSortBy = static_cast<Configs::typeBy>(chosen->data().toInt());
            GroupSortAction action;
            action.method = typeSortBy == Configs::typeBy::bySecurity
                                ? GroupSortMethod::BySecurity
                                : GroupSortMethod::ByType;
            live_sort_column = ProfilesTableModel::ColType;
            live_sort_descending = false;
            stampSortPrefsOnGroups(group->test_sort_by, group->traffic_sort_by, typeSortBy);
            persistLiveSortPrefs();
            applyLiveSortIndicator();
            runProfilesSort(action, group->test_sort_by, group->traffic_sort_by);
            return;
        }
        if (columnIndex >= ProfilesTableModel::ColLatency
            && columnIndex <= ProfilesTableModel::ColSiteScore) {
            QMenu menu(this);
            auto* sortByLabel = menu.addAction(tr("Sort By:"));
            sortByLabel->setEnabled(false);

            struct SortOption { int value; QString label; };
            QList<SortOption> options = {
                { static_cast<int>(Configs::testBy::latency), tr("Latency") },
                { static_cast<int>(Configs::testBy::rxSpeed), tr("Rx Speed") },
                { static_cast<int>(Configs::testBy::connectTime), tr("Connection Time") },
                { static_cast<int>(Configs::testBy::siteScore), tr("Site Score") }
            };
            for (const auto& opt : options) {
                auto* act = menu.addAction(opt.label);
                act->setData(opt.value);
                act->setCheckable(true);
                act->setChecked(static_cast<int>(group->test_sort_by) == opt.value);
            }

            auto* chosen = menu.exec(header->mapToGlobal(pos));
            if (chosen == nullptr || !chosen->data().isValid()) return;

            const auto testSortBy = static_cast<Configs::testBy>(chosen->data().toInt());
            GroupSortAction action;
            action.method = GroupSortMethod::ByTestResult;
            action.descending = testSortBy == Configs::testBy::siteScore;
            if (testSortBy == Configs::testBy::latency)
                live_sort_column = ProfilesTableModel::ColLatency;
            else if (testSortBy == Configs::testBy::rxSpeed)
                live_sort_column = ProfilesTableModel::ColRxSpeed;
            else if (testSortBy == Configs::testBy::connectTime)
                live_sort_column = ProfilesTableModel::ColConnectionTime;
            else
                live_sort_column = ProfilesTableModel::ColSiteScore;
            live_sort_descending = action.descending;
            stampSortPrefsOnGroups(testSortBy, group->traffic_sort_by, group->type_sort_by);
            persistLiveSortPrefs();
            applyLiveSortIndicator();
            runProfilesSort(action, testSortBy, group->traffic_sort_by);
            return;
        }
        if (columnIndex == ProfilesTableModel::ColRxTraffic
            || columnIndex == ProfilesTableModel::ColTxTraffic) {
            QMenu menu(this);
            auto* sortByLabel = menu.addAction(tr("Sort By:"));
            sortByLabel->setEnabled(false);

            struct TrafficSortOption { int value; QString label; };
            QList<TrafficSortOption> options = {
                { static_cast<int>(Configs::trafficBy::rx), tr("Rx traffic") },
                { static_cast<int>(Configs::trafficBy::tx), tr("Tx traffic") }
            };

            for (const auto& opt : options) {
                auto* act = menu.addAction(opt.label);
                act->setData(opt.value);
                act->setCheckable(true);
                act->setChecked(static_cast<int>(group->traffic_sort_by) == opt.value);
            }

            auto* chosen = menu.exec(header->mapToGlobal(pos));
            if (chosen == nullptr || !chosen->data().isValid()) return;

            const auto trafficSortBy = static_cast<Configs::trafficBy>(chosen->data().toInt());
            GroupSortAction action;
            action.method = GroupSortMethod::ByTraffic;
            action.descending = false;
            live_sort_column = trafficSortBy == Configs::trafficBy::rx
                ? ProfilesTableModel::ColRxTraffic : ProfilesTableModel::ColTxTraffic;
            live_sort_descending = false;
            stampSortPrefsOnGroups(group->test_sort_by, trafficSortBy, group->type_sort_by);
            persistLiveSortPrefs();
            applyLiveSortIndicator();
            runProfilesSort(action, group->test_sort_by, trafficSortBy);
            return;
        }
    });
    ui->profilesTableView->verticalHeader()->setStretchLastSection(false);
    ui->profilesTableView->verticalHeader()->setDefaultSectionSize(24);
    ui->profilesTableView->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    ui->profilesTableView->setTabKeyNavigation(false);
    ui->profilesTableView->horizontalHeader()->setResizeContentsPrecision(0);
    applyLiveSortIndicator();

    auto *filterHeader = static_cast<ProfilesTableFilterHeader*>(ui->profilesTableView->horizontalHeader());
    filterHeader->setLastFilterColumn(Configs::dataManager->settingsRepo->last_filter_column);
    connect(filterHeader, &ProfilesTableFilterHeader::lastFilterColumnChanged, this, [](int column)
    {
        Configs::dataManager->settingsRepo->last_filter_column = column;
        Configs::dataManager->settingsRepo->Save();
    });

    m_filterRefreshDebounce = new QTimer(this);
    m_filterRefreshDebounce->setSingleShot(true);
    m_filterRefreshDebounce->setInterval(50);
    connect(m_filterRefreshDebounce, &QTimer::timeout, this, [this] { applyProfileFilters(); });

    connect(filterHeader, &ProfilesTableFilterHeader::typeFilterChanged, this, [this](const QString& currentText)
    {
       typeFilterString = currentText;
       m_filterRefreshDebounce->start();
    });
    connect(filterHeader, &ProfilesTableFilterHeader::addressFilterChanged, this, [this](const QString& currentText)
    {
       addressFilterString = currentText;
       m_filterRefreshDebounce->start();
    });
    connect(filterHeader, &ProfilesTableFilterHeader::nameFilterChanged, this, [this](const QString& currentText)
    {
       nameFilterString = currentText;
       m_filterRefreshDebounce->start();
    });
    connect(filterHeader, &ProfilesTableFilterHeader::testFilterChanged, this, [this](const QString& currentText)
    {
       countryFilterString = currentText;
       m_filterRefreshDebounce->start();
    });
    connect(filterHeader, &ProfilesTableFilterHeader::focusTableRequested, this,
            [this](bool selectFirst) { focusProfilesTable(selectFirst); });

    this->refresh_groups();

    tray = new QSystemTrayIcon(nullptr);
    tray->setIcon(Icon::GetTrayIcon(Icon::NONE));
    QApplication::setWindowIcon(Icon::GetTaskbarIcon(Icon::NONE));
    trayMenu = new QMenu();
    auto *routeSeparator = trayMenu->addSeparator();
    trayMenu->addAction(ui->actionRestart_Program);
    trayMenu->addAction(ui->menu_exit);

    auto routeIds = std::make_shared<QList<int>>();
    auto routeActions = std::make_shared<QList<QAction*>>();
    auto rebuildRouteActions = [this, routeIds, routeActions, routeSeparator] {
        for (auto *action : *routeActions) {
            trayMenu->removeAction(action);
            action->deleteLater();
        }
        routeActions->clear();
        routeIds->clear();
        for (const auto &route : Configs::dataManager->routesRepo->GetAllRouteProfiles()) {
            routeIds->append(route->id);
            auto *action = new QAction(route->name, trayMenu);
            action->setData(route->id);
            action->setCheckable(true);
            connect(action, &QAction::triggered, this, [this, action] {
                const int routeId = action->data().toInt();
                if (Configs::dataManager->settingsRepo->current_route_id == routeId) return;
                Configs::dataManager->settingsRepo->current_route_id = routeId;
                Configs::dataManager->settingsRepo->Save();
                if (Configs::dataManager->settingsRepo->started_id >= 0) {
                    profile_start(Configs::dataManager->settingsRepo->started_id);
                }
            });
            trayMenu->insertAction(routeSeparator, action);
            routeActions->append(action);
        }
    };
    rebuildRouteActions();
    connect(trayMenu, &QMenu::aboutToShow, this, [this, routeIds, routeActions, rebuildRouteActions] {
        QList<int> currentIds;
        for (const auto &route : Configs::dataManager->routesRepo->GetAllRouteProfiles()) currentIds.append(route->id);
        if (currentIds != *routeIds) rebuildRouteActions();
        for (auto *action : *routeActions) {
            action->setChecked(Configs::dataManager->settingsRepo->current_route_id == action->data().toInt());
        }
    });
    tray->setVisible(!Configs::dataManager->settingsRepo->disable_tray);
#if defined(Q_OS_MACOS)
    // HACK: Do not use setContextMenu on macOS. Qt 6.11 attaches trayMenu to NSStatusItem.menu and
    // registers an NSMenuDidBeginTracking observer; on macOS 27 (Tahoe) status-item clicks are handled
    // out-of-process (MenuBarAgent), so when Qt's observer calls emitActivated() NSApp.currentEvent is
    // no longer a mouse event and -[NSEvent clickCount] aborts inside libqcocoa. Manual QMenu::popup
    // avoids that native menu path until Qt 6.11.3+ ships the qt_mac_isMouseEvent guard upstream.
    // tray->setContextMenu(trayMenu);
#else
    tray->setContextMenu(trayMenu);
#endif
    connect(tray, &QSystemTrayIcon::activated, qApp, [=, this](QSystemTrayIcon::ActivationReason reason) {
#if defined(Q_OS_MACOS)
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::Context) {
            const QRect trayGeom = tray->geometry();
            trayMenu->popup(trayGeom.isValid() ? trayGeom.bottomLeft() : QCursor::pos());
        }
#else
        if (reason == QSystemTrayIcon::Trigger) {
            trayClickEvent();
        }
#endif
    });

    ui->actionRemember_last_proxy->setChecked(Configs::dataManager->settingsRepo->remember_enable);
    ui->actionAuto_connect_with_best_site_score->setChecked(
        Configs::dataManager->settingsRepo->auto_connect_best_site_score);
    ui->actionSpeed_test_fall_short->setChecked(
        Configs::dataManager->settingsRepo->speed_test_fall_short);
    ui->actionRoute_requests_via_connected_profile->setChecked(
        Configs::dataManager->settingsRepo->net_use_proxy);
    ui->actionStart_with_system->setChecked(AutoRun_IsEnabled());
    ui->actionAllow_LAN->setChecked(QStringList{"::", "0.0.0.0"}.contains(Configs::dataManager->settingsRepo->inbound_address));

    connect(ui->menu_open_config_folder, &QAction::triggered, this, [=,this] { QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::currentPath())); });
    connect(ui->menu_open_dashboard, &QAction::triggered, this, [=,this] { OpenDashboard(); });
    connect(ui->actionRestart_Proxy, &QAction::triggered, this, [=,this] { RestartCore(); });
    connect(ui->actionRestart_Program, &QAction::triggered, this, [=,this] { MW_dialog_message(MwMessage::RestartProgram, {}); });
    connect(ui->actionExport_application_state, &QAction::triggered,
            this, &MainWindow::on_menu_export_application_state_triggered);
    connect(ui->actionImport_application_state, &QAction::triggered,
            this, &MainWindow::on_menu_import_application_state_triggered);
    connect(ui->actionShow_window, &QAction::triggered, this, [=,this] { ActivateWindow(this); });
    connect(ui->actionRemember_last_proxy, &QAction::triggered, this, [=,this](bool checked) {
        Configs::dataManager->settingsRepo->remember_enable = checked;
        ui->actionRemember_last_proxy->setChecked(checked);
        Configs::dataManager->settingsRepo->Save();
    });
    connect(ui->actionAuto_connect_with_best_site_score, &QAction::toggled, this, [=, this](bool checked) {
        Configs::dataManager->settingsRepo->auto_connect_best_site_score = checked;
        Configs::dataManager->settingsRepo->Save();
    });
    connect(ui->actionSpeed_test_fall_short, &QAction::toggled, this, [=, this](bool checked) {
        Configs::dataManager->settingsRepo->speed_test_fall_short = checked;
        Configs::dataManager->settingsRepo->Save();
        MW_show_log(checked ? tr("Speedtest fall-short mode enabled")
                            : tr("Speedtest fall-short mode disabled"));
    });
    connect(ui->actionRoute_requests_via_connected_profile, &QAction::toggled, this, [=, this](bool checked) {
        Configs::dataManager->settingsRepo->net_use_proxy = checked;
        Configs::dataManager->settingsRepo->Save();
    });
    connect(ui->actionImprove_mood, &QAction::triggered, this, &MainWindow::runImproveMood);
    connect(ui->actionStart_with_system, &QAction::triggered, this, [=,this](bool checked) {
        AutoRun_SetEnabled(checked);
        ui->actionStart_with_system->setChecked(checked);
    });
    connect(ui->actionAllow_LAN, &QAction::triggered, this, [=,this](bool checked) {
        Configs::dataManager->settingsRepo->inbound_address = checked ? "::" : "127.0.0.1";
        ui->actionAllow_LAN->setChecked(checked);
        MW_dialog_message(MwMessage::UpdateSettings, {});
    });
    connect(ui->checkBox_VPN, &QCheckBox::clicked, this, [=,this](bool checked) { set_spmode_vpn(checked); });
    connect(ui->checkBox_SystemProxy, &QCheckBox::clicked, this, [=,this](bool checked) { set_spmode_system_proxy(checked); });
    connect(ui->menu_spmode, &QMenu::aboutToShow, this, [=,this]() {
        ui->menu_spmode_disabled->setChecked(!(Configs::dataManager->settingsRepo->spmode_system_proxy || Configs::dataManager->settingsRepo->spmode_vpn));
        ui->menu_spmode_system_proxy->setChecked(Configs::dataManager->settingsRepo->spmode_system_proxy);
        ui->menu_spmode_vpn->setChecked(Configs::dataManager->settingsRepo->spmode_vpn);
    });
    connect(ui->menu_spmode_system_proxy, &QAction::triggered, this, [=,this](bool checked) { set_spmode_system_proxy(checked); });
    connect(ui->menu_spmode_vpn, &QAction::triggered, this, [=,this](bool checked) { set_spmode_vpn(checked); });
    connect(ui->menu_spmode_disabled, &QAction::triggered, this, [=,this]() {
        set_spmode_system_proxy(false);
        set_spmode_vpn(false);
    });
    connect(ui->menu_qr, &QAction::triggered, this, [=,this]() { display_qr_link(false); });
    connect(ui->system_dns, &QCheckBox::clicked, this, [=,this](bool checked) {
        if (const auto ok = set_system_dns(checked); !ok) {
            ui->system_dns->setChecked(!checked);
        } else {
            refresh_status();
        }
    });
    if (Configs::dataManager->settingsRepo->show_system_dns) ui->system_dns->show();
    else ui->system_dns->hide();

    auto startSelectedRankedSpeedtest = [=, this](TestRunner::RankedStartMode mode, bool connectBest = false) {
        const auto selected = get_now_selected_list();
        if (selected.isEmpty()) return;
        testRunner->runRankedSpeedTests(selected, mode, connectBest,
                                        Configs::dataManager->settingsRepo->speed_test_fall_short);
    };
    auto* actionSpeedtestByConnTime = new QAction(tr("Speedtest By Connection Time"), ui->menu_server);
    auto* actionSpeedtestBySavedScore = new QAction(tr("Speedtest By Saved Site Score"), ui->menu_server);
    ui->menu_server->insertAction(ui->actionSpeedtest_Selected, actionSpeedtestBySavedScore);
    ui->menu_server->insertAction(ui->actionSpeedtest_Selected, actionSpeedtestByConnTime);
    ui->actionSpeedtest_Selected->setVisible(false);
    connect(actionSpeedtestByConnTime, &QAction::triggered, this, [=, this] {
        startSelectedRankedSpeedtest(TestRunner::RankedStartMode::ByConnectionTime,
                                     Configs::dataManager->settingsRepo->auto_connect_best_site_score);
    });
    connect(actionSpeedtestBySavedScore, &QAction::triggered, this, [=, this] {
        startSelectedRankedSpeedtest(TestRunner::RankedStartMode::BySavedSiteScore,
                                     Configs::dataManager->settingsRepo->auto_connect_best_site_score);
    });

    connect(ui->menu_server, &QMenu::aboutToShow, this, [=,this](){
        if (auto selected = get_now_selected_list(); selected.empty())
        {
            actionSpeedtestByConnTime->setEnabled(false);
            actionSpeedtestBySavedScore->setEnabled(false);
            ui->actionUrl_Test_Selected->setEnabled(false);
            ui->menu_resolve_selected->setEnabled(false);
            ui->actionResolve_Selected_Out_IP->setEnabled(false);
        } else
        {
            actionSpeedtestByConnTime->setEnabled(true);
            actionSpeedtestBySavedScore->setEnabled(true);
            ui->actionUrl_Test_Selected->setEnabled(true);
            ui->menu_resolve_selected->setEnabled(true);
            ui->actionResolve_Selected_Out_IP->setEnabled(true);
        }
        if (testRunner->isRunning()) {
            ui->menu_server->addAction(ui->menu_stop_testing);
        } else {
            ui->menu_server->removeAction(ui->menu_stop_testing);
        }
    });

    connect(ui->menuTesting, &QMenu::aboutToShow, this, [=,this](){
        ui->actionDelete_Group->setEnabled(Configs::dataManager->groupsRepo->GetAllGroupIds().size() > 1);
        if (testRunner->isRunning()) {
            ui->menuTesting->addAction(ui->menu_stop_testing);
        } else {
            ui->menuTesting->removeAction(ui->menu_stop_testing);
        }
    });

    connect(ui->menuTools, &QMenu::aboutToShow, this, [=,this](){
        ui->actionSpeedtest_Current->setEnabled(running != nullptr);
    });

    connect(ui->actionAdd_New_Group, &QAction::triggered, this, [=,this]{
        auto ent = Configs::dataManager->groupsRepo->NewGroup();
        auto dialog = new DialogEditGroup(ent, this);
        int ret = dialog->exec();
        dialog->deleteLater();

        if (ret == QDialog::Accepted) {
            Configs::dataManager->groupsRepo->AddGroup(ent);
            MW_dialog_message(MwMessage::GroupsChanged, {});
        }
    });

    connect(ui->actionEdit_Group, &QAction::triggered, this, [=,this]{
        auto ent = Configs::dataManager->groupsRepo->CurrentGroup();
        auto dialog = new DialogEditGroup(ent, this);
        connect(dialog, &QDialog::finished, this, [=,this] {
            if (dialog->result() == QDialog::Accepted) {
                Configs::dataManager->groupsRepo->Save(ent);
                MW_dialog_message(MwMessage::GroupsChanged, {});
            }
            dialog->deleteLater();
        });
        dialog->show();
    });

    connect(ui->actionDelete_Group, &QAction::triggered, this, [=,this]{
        const auto group = Configs::dataManager->groupsRepo->CurrentGroup();
        if (group) delete_group(group->id);
    });

    connect(ui->actionUpdate_All_Subscriptions, &QAction::triggered, this, [=,this]{
        UI_update_all_groups();
    });

    connect(ui->actionRefresh_Column_Widths, &QAction::triggered, this, [=, this] {
        for (int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
            const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
            if (!group) continue;
            group->column_width.clear();
            group->clearCalculatedColumnWidth();
            Configs::dataManager->groupsRepo->Save(group);
        }
        m_columnWidthsAutoSized = true;
        refresh_proxy_list_column_size();
    });

    connect(ui->menuRouting_Menu, &QMenu::aboutToShow, this, [=,this]()
    {
        ui->menuRouting_Menu->clear();
        ui->menuRouting_Menu->addAction(ui->menu_routing_settings);

        auto* actionAdblock = new QAction(ui->menuRouting_Menu);
        actionAdblock->setText(tr("Enable AdBlock"));
        actionAdblock->setCheckable(true);
        actionAdblock->setChecked(Configs::dataManager->settingsRepo->adblock_enable);
        connect(actionAdblock, &QAction::triggered, this, [=,this](bool checked) {
            Configs::dataManager->settingsRepo->adblock_enable = checked;
            actionAdblock->setChecked(checked);
            Configs::dataManager->settingsRepo->Save();
            if (Configs::dataManager->settingsRepo->started_id >= 0) profile_start(Configs::dataManager->settingsRepo->started_id);
        });
        ui->menuRouting_Menu->addAction(actionAdblock);

        auto* actionWarp = new QAction(ui->menuRouting_Menu);
        actionWarp->setText(tr("Enable Warp"));
        actionWarp->setCheckable(true);
        actionWarp->setChecked(Configs::dataManager->settingsRepo->enable_warp);
        connect(actionWarp, &QAction::triggered, this, [=,this](bool checked) {
            Configs::dataManager->settingsRepo->enable_warp = checked;
            actionWarp->setChecked(checked);
            Configs::dataManager->settingsRepo->Save();
            if (Configs::dataManager->settingsRepo->started_id >= 0) profile_start(Configs::dataManager->settingsRepo->started_id);
        });
        ui->menuRouting_Menu->addAction(actionWarp);

        QMenu* profilesMenu = ui->menuRouting_Menu->addMenu(QObject::tr("Download Profiles"));
        for (const QString &country : QStringList{"China", "Iran", "Russia"})
        {
            auto* action = new QAction(profilesMenu);
            action->setText(country);
            connect(action, &QAction::triggered, this, [=,this]()
            {
                auto resp = NetworkRequestHelper::HttpGet(Configs::get_jsdelivr_link("https://raw.githubusercontent.com/throneproj/routeprofiles/profile/Profile_" + country));
                if (!resp.error.isEmpty()) {
                    runOnUiThread([=] {
                        MessageBoxWarning(QObject::tr("Download Profiles"), QObject::tr("Requesting profile error: %1").arg(resp.error + "\n" + resp.data));
                    });
                    return;
                }
                handle_add_remote_routes(resp.data);
            });
            profilesMenu->addAction(action);
        }

        ui->menuRouting_Menu->addSeparator();
        for (const auto& route : Configs::dataManager->routesRepo->GetAllRouteProfiles())
        {
            auto* action = new QAction(ui->menuRouting_Menu);
            action->setText(route->name);
            action->setData(route->id);
            action->setCheckable(true);
            action->setChecked(Configs::dataManager->settingsRepo->current_route_id == route->id);
            connect(action, &QAction::triggered, this, [=,this]()
            {
                auto routeID = action->data().toInt();
                if (Configs::dataManager->settingsRepo->current_route_id == routeID) return;
                Configs::dataManager->settingsRepo->current_route_id = routeID;
                Configs::dataManager->settingsRepo->Save();
                if (Configs::dataManager->settingsRepo->started_id >= 0) profile_start(Configs::dataManager->settingsRepo->started_id);
            });
            ui->menuRouting_Menu->addAction(action);
        }
    });
    connect(ui->actionClear_Test_Result, &QAction::triggered, this, [=, this]() {
        auto entIDs = get_now_selected_list();
        auto ents = Configs::dataManager->profilesRepo->GetProfileBatch(entIDs);
        if (ents.empty()) return;
        for (const auto &ent: ents) {
            ent->ClearTestResults();
        }
        Configs::dataManager->profilesRepo->SaveBatch(ents);
        if (auto group = Configs::dataManager->groupsRepo->GetGroup(ents.first()->gid))
            group->clearCalculatedColumnWidth();
        refresh_proxy_list();
    });
    connect(ui->actionUrl_Test_Selected, &QAction::triggered, this, [=,this]() {
        testRunner->runConnectionTimeTests(get_now_selected_list());
    });
    connect(ui->actionUrl_Test_Group, &QAction::triggered, this, [=,this]() {
        testRunner->runUrlTests(Configs::dataManager->groupsRepo->CurrentGroup()->Profiles());
    });
    connect(ui->actionSpeedtest_Current, &QAction::triggered, this, [=,this]()
    {
        if (running != nullptr)
        {
            testRunner->runSpeedTests({}, true);
        }
    });
    connect(ui->actionSpeedtest_Selected, &QAction::triggered, this, [=,this]()
    {
        startSelectedRankedSpeedtest(TestRunner::RankedStartMode::BySavedSiteScore,
                                     Configs::dataManager->settingsRepo->auto_connect_best_site_score);
    });
    connect(ui->actionSpeedtest_Group, &QAction::triggered, this, [=,this]()
    {
        testRunner->runRankedSpeedTests(Configs::dataManager->groupsRepo->CurrentGroup()->Profiles(),
                                        TestRunner::RankedStartMode::BySavedSiteScore,
                                        Configs::dataManager->settingsRepo->auto_connect_best_site_score,
                                        Configs::dataManager->settingsRepo->speed_test_fall_short);
    });
    connect(ui->actionResolve_Selected_Out_IP, &QAction::triggered, this, [=,this]() {
        testRunner->runIpTests(get_now_selected_list());
    });
    connect(ui->actionResolve_Out_IP, &QAction::triggered, this, [=,this]() {
        testRunner->runIpTests(Configs::dataManager->groupsRepo->CurrentGroup()->Profiles());
    });
    connect(ui->menu_stop_testing, &QAction::triggered, this, [=,this]() { testRunner->stop(); });
    connect(ui->pushButton_cancel_speedtest, &QPushButton::clicked, this, [=, this] {
        ui->pushButton_cancel_speedtest->setVisible(false);
        ui->pushButton_cancel_speedtest->setEnabled(false);
        testRunner->stop();
    });
    //
    auto set_selected_or_group = [=,this](int mode) {
        // 0=group 1=select 2=unknown(menu is hide)
        ui->menu_server->setProperty("selected_or_group", mode);
    };
    connect(ui->menu_server, &QMenu::aboutToHide, this, [=,this] {
        setTimeout([=,this] { set_selected_or_group(2); }, this, 200);
    });
    set_selected_or_group(2);
    connect(ui->menu_share_item, &QMenu::aboutToShow, this, [=,this] {
        QString name;
        auto selected = get_now_selected_list();

        ui->menu_export_config->setVisible(false);
        ui->actionExport_Xray_config->setVisible(false);
        if (selected.isEmpty()) return;

        auto profile = Configs::dataManager->profilesRepo->GetProfile(selected.first());
        if (!profile) return;

        if (selected.count() == 1 && profile->DisplayTestResult().trimmed().isEmpty()) {
            ui->actionCopy_Test_Result->setVisible(false);
        } else {
            ui->actionCopy_Test_Result->setVisible(true);
        }

        ui->menu_export_config->setVisible(true);
        if (profile->outbound->IsXray() || profile->type == "chain") ui->actionExport_Xray_config->setVisible(true);
    });
    connect(ui->actionExport_Xray_config, &QAction::triggered, this, [=,this]() {
        auto ents = get_now_selected_list();
        if (ents.count() != 1) return;
        auto ent = Configs::dataManager->profilesRepo->GetProfile(ents.first());

        auto result = Configs::BuildSingBoxConfig(ent);
        if (!result->error.isEmpty()) {
            MessageBoxWarning("Build config error", result->error);
            return;
        }
        QString config_core = QJsonObject2QString(result->xrayConfig, true);
        QApplication::clipboard()->setText(config_core);

        QMessageBox msg(QMessageBox::Information, tr("Config copied"), config_core);
        QPushButton *button_1 = msg.addButton(tr("Copy core config"), QMessageBox::YesRole);
        QPushButton *button_2 = msg.addButton(tr("Copy test config"), QMessageBox::YesRole);
        msg.addButton(QMessageBox::Ok);
        msg.setEscapeButton(QMessageBox::Ok);
        msg.setDefaultButton(QMessageBox::Ok);
        msg.exec();
        if (msg.clickedButton() == button_1) {
            QApplication::clipboard()->setText(config_core);
        } else if (msg.clickedButton() == button_2) {
            auto res = Configs::BuildTestConfig({ent});
            if (!res->error.isEmpty()) {
                MessageBoxWarning("Build Test config error", res->error);
                return;
            }
            config_core = QJsonObject2QString(res->xrayConfig, true);
            QApplication::clipboard()->setText(config_core);
        }
    });
    connect(ui->actionCopy_Test_Result, &QAction::triggered, this, [=,this]() {
        auto ents = get_now_selected_list();
        if (ents.count() == 0 || ents.count() > 1000) return;
        auto entList = Configs::dataManager->profilesRepo->GetProfileBatch(ents);
        QString res;
        int counter = 0;
        for (auto ent : entList) {
            auto testRes = ent->DisplayTestResult();
            if (!testRes.trimmed().isEmpty()) {
                res += testRes.trimmed() + "\n";
                counter++;
            }
        }
        QApplication::clipboard()->setText(res);
        MW_show_log(QString::number(counter) + tr(" Test result(s) copied to clipboard!"));
    });
    connect(ui->actionAdd_profile_from_File, &QAction::triggered, this, [=,this]()
    {
        // QFileDialog defaults to the first filter; config files routinely carry no extension.
        const auto filters = QStringList{
            tr("All files (*)"),
            tr("Config files (*.json *.conf *.txt *.yaml *.yml *.ini)"),
            tr("QR code images (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"),
        };
        const auto paths = QFileDialog::getOpenFileNames(this, tr("Select profile files"), QString(), filters.join(";;"));
        if (paths.isEmpty()) return;
        importFromFiles(paths);
    });

    connect(qApp, &QGuiApplication::commitDataRequest, this, &MainWindow::on_commitDataRequest);

    auto t = new QTimer;
    connect(t, &QTimer::timeout, this, [=,this]() { refresh_status(); });
    t->start(2000);

    t = new QTimer;
    connect(t, &QTimer::timeout, this, [&] { Configs_sys::logCounter.fetchAndStoreRelaxed(0); });
    t->start(1000);

    // Debounced so font/theme changes settle; fired from changeEvent, showEvent
    // and ThemeManager::themeChanged. Window resize does not change table data.
    m_proxyListRefreshDebounce = new QTimer(this);
    m_proxyListRefreshDebounce->setSingleShot(true);
    connect(m_proxyListRefreshDebounce, &QTimer::timeout, this, [this] { refresh_proxy_list({}, false); });

    // The selector monitor emits from its own poll thread.
    connect(Stats::autoSelectorMonitor, &Stats::AutoSelectorMonitor::poolExhausted, this,
            [this](int profileID) { on_auto_selector_exhausted(profileID); }, Qt::QueuedConnection);
    connect(Stats::autoSelectorMonitor, &Stats::AutoSelectorMonitor::updated, this,
            [this] { refresh_auto_selector_view(); }, Qt::QueuedConnection);

    {
        auto* runner = Throne::PeriodicRunner::instance();
        // Interval is sign-encoded in settings (negative = disabled); < 30 min counts as off.
        const auto minutesOf = [](int v) { return v >= 30 ? v : 0; };
        runner->Add({
            tr("subscriptions"),
            [minutesOf] { return minutesOf(Configs::dataManager->settingsRepo->sub_auto_update); },
            [] { return Configs::dataManager->settingsRepo->sub_auto_update_last; },
            [](qint64 t) {
                Configs::dataManager->settingsRepo->sub_auto_update_last = t;
                Configs::dataManager->settingsRepo->Save();
            },
            [] { UI_update_all_groups(true); },
        });
        runner->Add({
            tr("routing profiles"),
            [minutesOf] { return minutesOf(Configs::dataManager->settingsRepo->route_auto_update); },
            [] { return Configs::dataManager->settingsRepo->route_auto_update_last; },
            [](qint64 t) {
                Configs::dataManager->settingsRepo->route_auto_update_last = t;
                Configs::dataManager->settingsRepo->Save();
            },
            [] { UI_update_all_remote_routes(true); },
        });
    }

    if (!Configs::dataManager->settingsRepo->flag_tray) show();

    ui->data_view->setStyleSheet("background: transparent; border: none;");
}

MainWindow::~MainWindow() {
    stopImproveMood();
    delete ui;
}
