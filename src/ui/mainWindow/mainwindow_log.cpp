#include "include/ui/mainwindow.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QFontDatabase>
#include <QMenu>
#include <QMutexLocker>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

#include "3rdparty/qv2ray/v2/ui/LogHighlighter.hpp"

namespace {
    constexpr qsizetype MAX_PENDING_LOG_CHARS = 2 * 1024 * 1024;

    inline void FastAppendTextDocument(const QString &message, QTextDocument *doc) {
        QTextCursor cursor(doc);
        cursor.movePosition(QTextCursor::End);
        cursor.beginEditBlock();
        cursor.insertBlock();
        cursor.insertText(message);
        cursor.endEditBlock();
    }
}

void MainWindow::applyLogBrowserFont() {
    QFont logFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    int pt = qApp->font().pointSize();
    if (pt <= 0) pt = Configs::dataManager->settingsRepo->font_size;
    if (pt > 0) logFont.setPointSize(pt);
    ui->masterLogBrowser->setFont(logFont);
    if (coreLogBrowser != nullptr) coreLogBrowser->setFont(logFont);
}

void MainWindow::setLogHighlighter(bool darkMode) {
    // A QSyntaxHighlighter is never evicted by constructing another, so the old one must be deleted.
    delete logHighlighter;
    delete coreLogHighlighter;
    logHighlighter = new SyntaxHighlighter(darkMode, qvLogDocument);
    coreLogHighlighter = new SyntaxHighlighter(darkMode, coreLogDocument);
}

void MainWindow::append_log(const QString &log) {
    if (log.size() > 20000) {
        append_log(QString("TRUNCATED LONG LOG: ") + log.first(1000) + "...");
        return;
    }
    QMutexLocker locker(&logMutex);
    if (logQueue.size() > 1000) return;
    logQueue.enqueue({log, false});
    if (logQueue.size() == 1) logWaiter.wakeOne();
}

void MainWindow::append_core_log(const QString &log) {
    if (log.size() > 20000) {
        append_core_log(QString("TRUNCATED LONG LOG: ") + log.first(1000) + "...");
        return;
    }
    QMutexLocker locker(&logMutex);
    if (logQueue.size() > 1000) return;
    logQueue.enqueue({log, true});
    if (logQueue.size() == 1) logWaiter.wakeOne();
}

void MainWindow::log_process_loop() {
    while (true) {
        logMutex.lock();
        while (logQueue.isEmpty()) {
            logWaiter.wait(&logMutex);
        }
        // Drain and snapshot under one lock, then filter unlocked: a burst becomes a
        // single UI append and producers never block on the regex work.
        QQueue<LogEntry> pending;
        pending.swap(logQueue);
        const LogFilter filter{
            Configs::dataManager->settingsRepo->log_enable_include,
            Configs::dataManager->settingsRepo->log_enable_exclude,
            includeKeywords, excludeKeywords, includeCombined, excludeCombined,
        };
        logMutex.unlock();

        QString appBatch;
        QString coreBatch;
        for (const auto& entry : pending) {
            QString &batchToPrint = entry.core ? coreBatch : appBatch;
            for (const auto& logLine : entry.text.split('\n')) {
                if (should_print_log(logLine, filter)) {
                    batchToPrint += logLine;
                    batchToPrint += '\n';
                }
            }
        }

        bool needsPost;
        {
            QMutexLocker pendingLocker(&logPendingMutex);
            const auto appendBatch = [](QString &destination, const QString &batch) {
                const QString trimmed = batch.trimmed();
                if (trimmed.isEmpty()) return;
                if (!destination.isEmpty()) destination += '\n';
                destination += trimmed;
                if (destination.size() > MAX_PENDING_LOG_CHARS) {
                    const auto cut = destination.indexOf('\n', destination.size() - MAX_PENDING_LOG_CHARS);
                    destination = cut < 0 ? QString() : destination.mid(cut + 1);
                }
            };
            appendBatch(logPendingText, appBatch);
            appendBatch(coreLogPendingText, coreBatch);
            if (logPendingText.isEmpty() && coreLogPendingText.isEmpty()) continue;
            needsPost = !logFlushScheduled;
            logFlushScheduled = true;
        }
        // At most one flush in flight; later text rides the pending one, so the event queue cannot grow.
        if (needsPost) runOnUiThread([this] { flush_log_batch(); });
    }
}

void MainWindow::flush_log_batch() {
    QString appBatch;
    QString coreBatch;
    {
        QMutexLocker pendingLocker(&logPendingMutex);
        appBatch.swap(logPendingText);
        coreBatch.swap(coreLogPendingText);
        logFlushScheduled = false;
    }
    const auto appendPreservingPosition = [this](QTextBrowser *browser, QTextDocument *document,
                                                  const QString &batch) {
        if (batch.isEmpty() || browser == nullptr) return;
        auto *bar = browser->verticalScrollBar();
        const bool followTail = Configs::dataManager->settingsRepo->log_auto_scroll
            && bar->value() >= bar->maximum() - 1;
        auto *layout = document->documentLayout();
        QTextBlock anchorBlock = browser->cursorForPosition(QPoint(0, 0)).block();
        const int viewportOffset = anchorBlock.isValid()
            ? bar->value() - static_cast<int>(layout->blockBoundingRect(anchorBlock).y())
            : 0;
        FastAppendTextDocument(batch, document);
        if (followTail) {
            bar->setValue(bar->maximum());
        } else if (anchorBlock.isValid()) {
            const int newY = static_cast<int>(layout->blockBoundingRect(anchorBlock).y());
            bar->setValue(newY + viewportOffset);
        }
    };
    appendPreservingPosition(ui->masterLogBrowser, qvLogDocument, appBatch);
    appendPreservingPosition(coreLogBrowser, coreLogDocument, coreBatch);
}

bool MainWindow::should_print_log(const QString &log, const LogFilter &filter) {
    if (QStringView(log).trimmed().isEmpty()) return false;
    bool result = true;
    if (filter.enableInclude) {
        result = false;
        for (const auto& includeKeyword : filter.includeKeywords) {
            if (log.contains(includeKeyword)) {
                result = true;
                break;
            }
        }
        if (!result && !filter.includeCombined.pattern().isEmpty() && filter.includeCombined.match(log).hasMatch()) {
            result = true;
        }
    }
    if (result && filter.enableExclude) {
        for (const auto& excludeKeyword : filter.excludeKeywords) {
            if (log.contains(excludeKeyword)) {
                result = false;
                break;
            }
        }
        if (result && !filter.excludeCombined.pattern().isEmpty() && filter.excludeCombined.match(log).hasMatch()) {
            result = false;
        }
    }
    return result;
}

void MainWindow::on_masterLogBrowser_customContextMenuRequested(const QPoint &pos) {
    QMenu *menu = ui->masterLogBrowser->createStandardContextMenu();

    auto sep = new QAction(this);
    sep->setSeparator(true);
    menu->addAction(sep);

    auto action_clear = new QAction(this);
    action_clear->setText(tr("Clear"));
    connect(action_clear, &QAction::triggered, this, [=,this] {
        {
            // Otherwise a flush already in flight repaints what was just cleared.
            QMutexLocker pendingLocker(&logPendingMutex);
            logPendingText.clear();
        }
        qvLogDocument->clear();
        ui->masterLogBrowser->clear();
    });
    menu->addAction(action_clear);

    menu->exec(ui->masterLogBrowser->viewport()->mapToGlobal(pos));
}
