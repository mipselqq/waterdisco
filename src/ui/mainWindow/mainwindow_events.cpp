#include "include/ui/mainwindow.h"

#include <QApplication>
#include <QCursor>
#include <QLineEdit>
#include <QMimeData>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QTimer>
#include <QWidget>

#include "include/ui/utils/ProfilesTableView.h"
#include "include/ui/widget/TrayOtpCodes.hpp"
#include "include/ui/widget/TrayProfileSelector.hpp"

namespace {
class LastFrameOverlay final : public QWidget {
public:
    explicit LastFrameOverlay(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
    }

    void setSnapshot(const QPixmap &snap, const QColor &fill) {
        m_snap = snap;
        m_fill = fill;
        update();
    }

    void clearSnapshot() { m_snap = QPixmap(); }

protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.fillRect(rect(), m_fill);
        if (!m_snap.isNull()) painter.drawPixmap(0, 0, m_snap);
    }

private:
    QPixmap m_snap;
    QColor m_fill{QStringLiteral("#19232D")};
};
} // namespace

void MainWindow::trayClickEvent() {
    constexpr qint64 recentlyActiveMs = 350;
    const bool wasRecentlyActive = isActiveWindow() ||
        (sinceWindowDeactivated.isValid() && sinceWindowDeactivated.elapsed() <= recentlyActiveMs);

    if (isVisible() && !isMinimized() && wasRecentlyActive) {
        HideWindow(this);
    } else {
        ActivateWindow(this);
        refresh_proxy_list_column_size();
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (tray->isVisible()) {
        HideWindow(this);
        event->ignore();
    } else {
        on_menu_exit_triggered();
    }
}

void MainWindow::changeEvent(QEvent *event) {
    const QEvent::Type type = event->type();

    if (type == QEvent::FontChange) {
        // masterLogBrowser keeps its monospace family but follows the user's point size
        applyLogBrowserFont();

        // QStyleSheetStyle caches font-dependent metrics and does not invalidate them on
        // FontChange; toggling the stylesheet through "" forces a repolish.
        auto refreshStylesheetCache = [](QWidget *w) {
            const QString ss = w->styleSheet();
            if (ss.isEmpty()) return;
            w->setStyleSheet("");
            w->setStyleSheet(ss);
        };
        const auto allChildren = findChildren<QWidget*>();
        for (QWidget *w : allChildren) {
            refreshStylesheetCache(w);
        }

        // No per-widget stylesheet here, so force a real FontChange via a different point
        // size (Qt skips setFont when unchanged), then return to inheriting from qApp.
        auto forceFontReapply = [](QWidget *w) {
            if (!w) return;
            const QFont currentFont = QApplication::font();
            QFont diffFont = currentFont;
            diffFont.setPointSize(currentFont.pointSize() + 1);
            w->setFont(diffFont);
            w->setFont(QFont());
            w->updateGeometry();
        };
        forceFontReapply(ui->profilesTableView);

        // The toolButton widths and the window floor were derived from the old
        // font; redo them now that the stylesheet caches above are clean.
        applyTopBarMetrics();
        syncInfoPanelTop();
    }
    if (type == QEvent::FontChange ||
        type == QEvent::PaletteChange ||
        type == QEvent::StyleChange) {
        scheduleProxyListRefresh();
    }
    if (type == QEvent::WindowStateChange) {
        syncConnectionViewState();
    }
    if (type == QEvent::ActivationChange) {
        // Stamped here, not from WindowDeactivate in eventFilter(): that only reaches
        // visible filtered children, so state-dependent widgets could drop it.
        if (isActiveWindow()) sinceWindowDeactivated.invalidate();
        else sinceWindowDeactivated.start();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    syncConnectionViewState();
    syncInfoPanelTop();
    QTimer::singleShot(0, this, [this] { syncInfoPanelTop(); });
    scheduleProxyListRefresh();
}

void MainWindow::hideEvent(QHideEvent *event) {
    QMainWindow::hideEvent(event);
    syncConnectionViewState();
}

void MainWindow::syncConnectionViewState() {
    const bool inView = isVisible() && !isMinimized()
        && ui->stats_widget->currentWidget() == ui->connections_tab;
    Stats::connection_lister->SetInView(inView);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    updateImproveMoodGeometry();
    if (event->oldSize().isValid()) beginLiveResize();
    syncTableResizeOverlay();
}

void MainWindow::scheduleProxyListRefresh() {
    constexpr int proxyListRefreshDebounceMs = 200;
    if (m_proxyListRefreshDebounce) m_proxyListRefreshDebounce->start(proxyListRefreshDebounceMs);
}

void MainWindow::showTableResizeSnapshot() {
    auto *table = ui->profilesTableView;
    if (!table || !table->parentWidget()) return;

    const QPixmap snap = table->grab();

    auto *overlay = static_cast<LastFrameOverlay *>(m_tableResizeOverlay.data());
    if (!overlay) {
        overlay = new LastFrameOverlay(table->parentWidget());
        m_tableResizeOverlay = overlay;
    } else if (overlay->parentWidget() != table->parentWidget()) {
        overlay->setParent(table->parentWidget());
    }
    overlay->setSnapshot(snap, table->palette().color(QPalette::Base));
    overlay->setGeometry(table->geometry());
    overlay->raise();
    overlay->show();
    table->setUpdatesEnabled(false);
}

void MainWindow::syncTableResizeOverlay() {
    if (!m_liveResizing || !m_tableResizeOverlay || !ui->profilesTableView) return;
    m_tableResizeOverlay->setGeometry(ui->profilesTableView->geometry());
}

void MainWindow::hideTableResizeSnapshot() {
    if (ui->profilesTableView) ui->profilesTableView->setUpdatesEnabled(true);
    if (auto *overlay = static_cast<LastFrameOverlay *>(m_tableResizeOverlay.data())) {
        overlay->hide();
        overlay->clearSnapshot();
    }
}

void MainWindow::beginLiveResize() {
    if (!m_liveResizeTimer) return;
    if (!m_liveResizing) {
        m_liveResizing = true;
        showTableResizeSnapshot();
    }
    m_liveResizeTimer->start();
}

void MainWindow::endLiveResize() {
    if (!m_liveResizing) return;
    m_liveResizing = false;
    if (auto *table = qobject_cast<ProfilesTableView *>(ui->profilesTableView)) {
        table->settleChromeMasks();
    }
    hideTableResizeSnapshot();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() || event->mimeData()->hasText()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const auto mimeData = event->mimeData();

    if (mimeData->hasUrls()) {
        QStringList paths;
        for (const QUrl &url : mimeData->urls()) {
            if (url.isLocalFile()) paths << url.toLocalFile();
        }
        // Remote urls (a link dragged out of a browser) carry no file and fall
        // through to the text handler below.
        if (!paths.isEmpty()) {
            importFromFiles(paths);
            event->acceptProposedAction();
            return;
        }
    }

    if (mimeData->hasText()) {
        import_or_handle_deeplink(mimeData->text());
        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

void MainWindow::openTraySelector(bool routing) {
    // Recreate on each open so it always shows fresh data. A previous one (if the user
    // reopened quickly) closes itself; WA_DeleteOnClose frees it and the QPointer clears.
    if (traySelector) traySelector->close();

    TrayProfileSelector::Callbacks cb;
    cb.startProfile = [this](int id) { profile_start(id); };
    cb.stopProfile = [this]() { profile_stop(false, false, true); };
    cb.chooseRoute = [this](int id) {
        if (Configs::dataManager->settingsRepo->current_route_id == id) return;
        Configs::dataManager->settingsRepo->current_route_id = id;
        Configs::dataManager->settingsRepo->Save();
        if (Configs::dataManager->settingsRepo->started_id >= 0) profile_start(Configs::dataManager->settingsRepo->started_id);
    };
    cb.isRunning = [this]() { return running != nullptr; };
    cb.runningId = [this]() { return running ? running->id : -1; };
    cb.runningGid = [this]() { return running ? running->gid : -1; };
    cb.runningName = [this]() { return running ? running->name : QString(); };

    traySelector = new TrayProfileSelector(
        routing ? TrayProfileSelector::Routing : TrayProfileSelector::Server, cb, this);
    traySelector->popupAt(QCursor::pos());
}

void MainWindow::openTrayOtpCodes() {
    if (trayOtpCodes) trayOtpCodes->close();
    trayOtpCodes = new TrayOtpCodes(this);
    trayOtpCodes->popupAt(QCursor::pos());
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (!qobject_cast<QLineEdit *>(QApplication::focusWidget())) {
            profile_start();
            return;
        }
    }
    QMainWindow::keyPressEvent(event);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
    const QEvent::Type type = event->type();

    if (type == QEvent::Resize && obj == ui->toolButton_program) {
        const int h = ui->toolButton_program->height();
        if (h > 0 && ui->toolButton_startstop->height() != h) {
            ui->toolButton_startstop->setFixedSize(h, h);
        }
    }
    if (type == QEvent::MouseButtonDblClick) {
        if (obj == ui->splitter) {
            const auto size = ui->splitter->size();
            ui->splitter->setSizes({size.height() / 2, size.height() / 2});
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::start_select_mode(QObject *context, const std::function<void(int)> &callback) {
    select_mode = true;
    connectOnce(this, &MainWindow::profile_selected, context, callback);
    refresh_status();
}
