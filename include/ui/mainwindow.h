#pragma once

#include <QMainWindow>
#include <include/global/HTTPRequestHelper.hpp>
#ifndef Q_MOC_RUN
#include <core/server/gen/libcore.pb.h>
#endif

#include "include/global/Configs.hpp"
#include "include/stats/connections/connectionLister.hpp"
#include "3rdparty/qv2ray/v2/ui/widgets/speedchart/SpeedWidget.hpp"
#include "include/database/entities/Profile.h"
#ifdef Q_OS_LINUX
#include <QtDBus>
#endif

#ifndef MW_INTERFACE

#include <QKeyEvent>
#include <QSystemTrayIcon>
#include <QQueue>
#include <QWaitCondition>
#include <QProcess>
#include <QTextDocument>
#include <QShortcut>
#include <QSemaphore>
#include <QMutex>
#include <QThreadPool>
#include <QPointer>

#include "group/GroupSort.hpp"
#include "include/global/GuiUtils.hpp"
#include "include/ui/utils/ProfilesTableModel.h"
#include "ui_mainwindow.h"

#endif

namespace Configs_sys {
    class CoreProcess;
}

QT_BEGIN_NAMESPACE
namespace Ui {
    class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    enum class SpeedtestConnectMode {
        None = 0,
        BestConnectionTime,
        BestSiteScore
    };

    enum class SpeedtestStartMode {
        AsIs = 0,
        ByConnectionTime,
        BySavedSiteScore
    };

    explicit MainWindow(QWidget *parent = nullptr);

    ~MainWindow() override;

    void prepare_exit();

    void refresh_proxy_list(const QList<int> &ids = {}, bool mayNeedReset = false);

    void show_group(int gid);

    void refresh_groups();

    void refresh_status(const QString &traffic_update = "");

    void update_traffic_graph(int proxyDl, int proxyUp, int directDl, int directUp);

    void profile_start(int _id = -1);

    void profile_stop(bool crash = false, bool block = false, bool manual = false);

    void set_spmode_system_proxy(bool enable, bool save = true);

    void toggle_system_proxy();

    void set_spmode_vpn(bool enable, bool save = true);

    bool get_elevated_permissions(int reason = 3);

    void start_select_mode(QObject *context, const std::function<void(int)> &callback);

    void RegisterHotkey(bool unregister);

    bool StopVPNProcess();

    void UpdateConnectionList(const QMap<QString, Stats::ConnectionMetadata>& toUpdate, const QMap<QString, Stats::ConnectionMetadata>& toAdd);

    void UpdateConnectionListWithRecreate(const QList<Stats::ConnectionMetadata>& connections);

    void UpdateDataView(bool force = false);

    void setDownloadReport(const DownloadProgressReport& report, bool show);

signals:

    void profile_selected(int id);

public slots:

    void on_commitDataRequest();

    void on_menu_exit_triggered();

#ifndef MW_INTERFACE

private slots:

    void on_masterLogBrowser_customContextMenuRequested(const QPoint &pos);

    void on_menu_basic_settings_triggered();

    void on_menu_routing_settings_triggered();

    void on_menu_vpn_settings_triggered();

    void on_menu_hotkey_settings_triggered();

    void on_menu_add_from_input_triggered();

    static void on_menu_add_from_clipboard_triggered();

    void on_menu_clone_triggered();

    void on_menu_delete_repeat_triggered();

    void on_menu_delete_triggered();

    void on_menu_reset_traffic_triggered();

    void on_menu_copy_links_triggered();

    void on_menu_copy_links_nkr_triggered();

    void on_menu_export_config_triggered();

    void display_qr_link(bool nkrFormat = false);

    void on_menu_scan_qr_triggered();

    void on_menu_clear_test_result_triggered();

    void on_menu_manage_groups_triggered();

    void on_menu_select_all_triggered();

    void on_menu_remove_unavailable_triggered();

    void on_menu_remove_invalid_triggered();

    void on_menu_resolve_selected_triggered();

    void on_menu_resolve_domain_triggered();

    void on_menu_update_subscription_triggered();

    void on_profilesTableView_doubleClicked(const QModelIndex &index);

    void on_profilesTableView_customContextMenuRequested(const QPoint &pos);

    void on_tabWidget_currentChanged(int index);

    void on_tabWidget_customContextMenuRequested(const QPoint& p);

private:
    class QMediaPlayer *easterPlayer = nullptr;
    class QAudioOutput *easterAudioOutput = nullptr;
    class QVideoSink *easterVideoSink = nullptr;
    class QLabel *easterVideoLabel = nullptr;
    class QWidget *easterOverlay = nullptr;
    bool easterStopping = false;

    Ui::MainWindow *ui;
    ProfilesTableModel *profilesTableModel = nullptr;
    QSystemTrayIcon *tray;
    QShortcut *shortcut_esc = new QShortcut(QKeySequence::Cancel, this);
    //
    QThreadPool *parallelCoreCallPool = new QThreadPool(this);
    std::atomic<bool> stopSpeedtest = false;
    std::atomic<bool> speedtestRunning = false;
    //
    Configs_sys::CoreProcess *core_process;
    qint64 vpn_pid = 0;
    //
    bool qvLogAutoScoll = true;
    QTextDocument *qvLogDocument = new QTextDocument(this);
    //
    QString title_error;
    int icon_status = -1;
    std::shared_ptr<Configs::Profile> running;
    QString traffic_update_cache;
    qint64 last_test_time = 0;
    //
    int proxy_last_order = -1;
    int live_sort_column = -1;
    bool live_sort_descending = false;
    std::atomic<int> deferred_profile_start_after_speedtest = -1919;
    bool select_mode = false;
    QMutex mu_starting;
    QMutex mu_stopping;
    QMutex mu_exit;
    int exit_reason = 0;
    //
    QMutex mu_download_update;
    //
    QMutex connectionListMu;
    //
    int toolTipID;
    //
    SpeedWidget *speedChartWidget;
    //
    // for data view
    QDateTime lastUpdated = QDateTime::currentDateTime();
    QString currentSptProfileName;
    QString currentTestStatusText;
    bool showSpeedtestData = false;
    bool showDownloadData = false;
    libcore::SpeedTestResult currentTestResult;
    DownloadProgressReport currentDownloadReport; // could use a list, but don't think can show more than one anyways

    // shortcuts
    QList<QShortcut*> hiddenMenuShortcuts;

    QStringList remoteRouteProfiles;
    QMutex mu_remoteRouteProfiles;

    // search
    bool searchEnabled = false;
    QString addressFilterString;
    QString nameFilterString;
    QString typeFilterString;
    QString countryFilterString;

    // log
    QStringList includeKeywords;
    QStringList excludeKeywords;
    QRegularExpression includeCombined;
    QRegularExpression excludeCombined;
    QMutex logMutex;
    QQueue<QString> logQueue;
    QWaitCondition logWaiter;

    void append_log(const QString &log);

    void log_process_loop();

    bool should_print_log(const QString &log);

    void updateLogFilterFields();

    void runEasterVideo();
    void stopEasterVideo();
    void updateEasterVideoGeometry();

    QList<int> filterProfilesList(const QList<int>& profileIDs);

    QList<int> get_now_selected_list();

    QList<int> get_selected_or_group();

    void clearUnavailableProfiles(bool confirm = true, QList<int> profileIDs = {});

    void dialog_message_impl(const QString &sender, const QString &info);

    void refresh_proxy_list_column_size();

    void refresh_proxy_list_impl(const QList<int> &ids = {}, bool mayNeedReset = false);

    void refresh_proxy_list_impl_refresh_data(const QList<int>& ids = {}, bool mayNeedReset = false);

    void parseQrImage(const QPixmap *image);

    void keyPressEvent(QKeyEvent *event) override;

    void closeEvent(QCloseEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event);

    void dropEvent(QDropEvent* event) override;

    //

    void HotkeyEvent(const QString &key);

    void RegisterHiddenMenuShortcuts(bool unregister = false);

    void setActionsData();

    QList<QAction*> getActionsForShortcut();

    void loadShortcuts();

    // rpc

    static void setup_rpc();

    void urltest_current_group(const QList<int>& profileIDs, bool connectionTimeTest = false);

    void iptest_current_group(const QList<int>& profileIDs);

    void stopTests();

    void runURLTest(const QString& config, const QString& xrayConfig, bool useDefault, const QStringList& outboundTags, const QMap<QString, int>& tag2entID, int entID = -1,
                    const QString& testUrl = {}, bool saveConnectTime = false);

    void runIPTest(const QString& config, const QString& xrayConfig, bool useDefault, const QStringList& outboundTags, const QMap<QString, int>& tag2entID, int entID = -1);

    void url_test_current();

    void speedtest_current_group(const QList<int>& profileIDs,
                                 SpeedtestConnectMode connectMode = SpeedtestConnectMode::None,
                                 SpeedtestStartMode startMode = SpeedtestStartMode::AsIs);

    void speedtest_current_group_fall_short(const QList<int>& profileIDs,
                                            SpeedtestConnectMode connectMode = SpeedtestConnectMode::None,
                                            SpeedtestStartMode startMode = SpeedtestStartMode::AsIs);
    QList<int> getOrderedSpeedtestProfileIDs(const QList<int>& profileIDs, SpeedtestStartMode startMode) const;
    bool runConnectionTimeTestsForProfiles(const QList<int>& profileIDs, bool clearUnavailableAfter = true);

    void runSpeedTest(const QString& config, const QString& xrayConfig, bool useDefault, const QStringList& outboundTags, const QMap<QString, int>& tag2entID, int entID = -1);

    void runSpeedTestFallShort(const QString& config, const QString& xrayConfig, bool useDefault, const QStringList& outboundTags,
                               const QMap<QString, int>& tag2entID, int entID, int timeoutMs,
                               qint64* elapsedMsOut = nullptr, bool* successOut = nullptr, bool* skippedOut = nullptr);

    bool set_system_dns(bool set, bool save_set = true);

    void CheckUpdate();

    void setupConnectionList();

    void querySpeedtest(QDateTime lastProxyListUpdate, const QMap<QString, int>& tag2entID);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

#endif // MW_INTERFACE
};

inline MainWindow *GetMainWindow() {
    return (MainWindow *) mainwindow;
}

void UI_InitMainWindow();

#ifdef Q_OS_LINUX
/*
 * Proxy class for interface org.freedesktop.portal.Request
 */
class OrgFreedesktopPortalRequestInterface : public QDBusAbstractInterface
{
    Q_OBJECT
public:
    OrgFreedesktopPortalRequestInterface(const QString& service,
                                         const QString& path,
                                         const QDBusConnection& connection,
                                         QObject* parent = nullptr);

    ~OrgFreedesktopPortalRequestInterface();

public Q_SLOTS:
    inline QDBusPendingReply<> Close()
    {
    QList<QVariant> argumentList;
    return asyncCallWithArgumentList(QStringLiteral("Close"), argumentList);
    }

Q_SIGNALS: // SIGNALS
    void Response(uint response, QVariantMap results);
};

namespace org {
namespace freedesktop {
namespace portal {
typedef ::OrgFreedesktopPortalRequestInterface Request;
}
}
}
#endif
