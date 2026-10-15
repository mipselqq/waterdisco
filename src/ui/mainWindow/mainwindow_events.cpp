#include "include/ui/mainwindow.h"

#include <QApplication>
#include <QCursor>
#include <QLineEdit>
#include <QMimeData>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QTimer>

#include "include/ui/utils/ProfilesTableView.h"
#include "include/ui/widget/TrayOtpCodes.hpp"
#include "include/ui/widget/TrayProfileSelector.hpp"

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
        // QStyleSheetStyle caches font metrics and ignores FontChange; toggling the stylesheet repolishes.
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
        // Never clear the app sheet first: that runs setStyle() and refills Qt's per-class font table over the new font (#1829).
        const QString appSheet = qApp->styleSheet();
        if (!appSheet.isEmpty()) {
            qApp->setStyleSheet(appSheet);
        }
        // After the repolish: with no font rule, QStyleSheetStyle resets a setFont() font to the parent's.
        applyLogBrowserFont();

        // Qt skips setFont when unchanged, so bump the point size to force a real FontChange.
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

        applyTopBarMetrics();
        syncInfoPanelTop();
    }
    if (type == QEvent::FontChange ||
        type == QEvent::PaletteChange ||
        type == QEvent::StyleChange) {
        scheduleProxyListRefresh();
        refreshConnectionIcons();
    }
    if (type == QEvent::WindowStateChange) {
        syncConnectionViewState();
    }
    if (type == QEvent::ActivationChange) {
        // Not stamped from WindowDeactivate in eventFilter(): that only reaches visible filtered children.
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
    syncLiveResizeOverlays();
}

void MainWindow::scheduleProxyListRefresh() {
    constexpr int proxyListRefreshDebounceMs = 200;
    if (m_proxyListRefreshDebounce) m_proxyListRefreshDebounce->start(proxyListRefreshDebounceMs);
}

QTextBrowser *MainWindow::activeLogBrowser() const {
    if (!ui->stats_widget) return nullptr;
    if (ui->stats_widget->currentWidget() == ui->Logs) return ui->masterLogBrowser;
    if (coreLogBrowser && coreLogBrowser->isVisible()) return coreLogBrowser;
    return nullptr;
}

void MainWindow::syncLiveResizeOverlays() {
    if (!m_liveResizing) return;
    m_tableResizeFreeze.syncGeometry();
    m_logResizeFreeze.syncGeometry();
}

void MainWindow::beginLiveResize() {
    if (!m_liveResizeTimer) return;
    if (!m_liveResizing) {
        m_liveResizing = true;
        m_tableResizeFreeze.show(ui->profilesTableView);
        if (const auto browser = activeLogBrowser()) {
            m_logResizeFreeze.show(browser);
        }
    }
    m_liveResizeTimer->start();
}

void MainWindow::endLiveResize() {
    if (!m_liveResizing) return;
    m_liveResizing = false;
    if (auto *table = qobject_cast<ProfilesTableView *>(ui->profilesTableView)) {
        table->settleChromeMasks();
    }
    m_tableResizeFreeze.hide();
    m_logResizeFreeze.hide();
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
    // Recreated on each open; WA_DeleteOnClose frees the old one and clears the QPointer.
    if (traySelector) traySelector->close();

    TrayProfileSelector::Callbacks cb;
    cb.startProfile = [this](int id) { profile_start(id); };
    cb.stopProfile = [this]() { profile_stop(false, false, true); };
    cb.chooseRoute = [this](int id) { choose_route(id); };
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

    // Accepting the override keeps any window-level Esc shortcut from taking the key first.
    if ((type == QEvent::ShortcutOverride || type == QEvent::KeyPress) && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape
        && obj->isWidgetType() && ui->logToolbar->isAncestorOf(static_cast<QWidget *>(obj))) {
        if (type == QEvent::ShortcutOverride) event->accept();
        else logFilterButton->setChecked(false);
        return true;
    }

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
