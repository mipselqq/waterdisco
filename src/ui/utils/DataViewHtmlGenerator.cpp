#include "include/ui/utils/DataViewHtmlGenerator.h"

#include "include/global/Configs.hpp"

#include <QDateTime>

namespace {
    constexpr auto kPanelLineStyle = "text-align:left;margin:0;padding-left:12px;";

    QString formatElapsedHms(qint64 elapsedMs) {
        const int totalSeconds = static_cast<int>(elapsedMs / 1000);
        const int hours = totalSeconds / 3600;
        const int minutes = (totalSeconds % 3600) / 60;
        const int seconds = totalSeconds % 60;
        return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }

    QString elapsedPanelLine(qint64 startedAtMs) {
        if (startedAtMs <= 0) return {};
        const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - startedAtMs;
        return QStringLiteral("<p style='%1opacity:0.75;'>%2</p>")
            .arg(QLatin1String(kPanelLineStyle), formatElapsedHms(elapsedMs));
    }
}

void DataViewHtmlGenerator::setDownloadReport(const DownloadProgressReport &report, bool show) {
    QMutexLocker lk(&mu_);
    download_.visible = show;
    download_.report = report;
}

void DataViewHtmlGenerator::seedSpeedTest(int totalProfiles) {
    QMutexLocker lk(&mu_);
    testProgress.store(0);
    Configs::dataManager->settingsRepo->speed_test_mode == Configs::TestConfig::COUNTRY ? speedtest_.kind = SpeedtestPanelState::Kind::Country : speedtest_.kind = SpeedtestPanelState::Kind::Speed;
    speedtest_.totalProfiles = totalProfiles;
    speedtest_.visible = true;
    speedtest_.startedAtMs = QDateTime::currentMSecsSinceEpoch();
}

void DataViewHtmlGenerator::setRankedSpeedtestProgress(const QString &stage, const QString &profileName,
                                                        int tested, int skipped, int totalProfiles) {
    QMutexLocker lk(&mu_);
    if (!speedtest_.visible) speedtest_.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    speedtest_.kind = SpeedtestPanelState::Kind::Speed;
    speedtest_.visible = true;
    speedtest_.stage = stage;
    speedtest_.profileName = profileName;
    speedtest_.tested = tested;
    speedtest_.skipped = skipped;
    speedtest_.totalProfiles = totalProfiles;
}

void DataViewHtmlGenerator::seedLatencyTest(LatencyTestPanelState::Kind kind, int totalProfiles) {
    QMutexLocker lk(&mu_);
    testProgress.store(0);
    latencyTest_.visible = true;
    latencyTest_.kind = kind;
    latencyTest_.totalProfiles = totalProfiles;
}

void DataViewHtmlGenerator::setAutoSelectorStatus(const QString &summary, const QString &detail) {
    QMutexLocker lk(&mu_);
    autoSelector_.summary = summary;
    autoSelector_.detail = detail;
    autoSelector_.visible = !summary.isEmpty();
}

void DataViewHtmlGenerator::clearTestSections() {
    QMutexLocker lk(&mu_);
    latencyTest_ = {};
    speedtest_ = {};
    testProgress.store(0);
}

void DataViewHtmlGenerator::addTestProgress(int count) {
    testProgress.fetch_add(count);
}

QString DataViewHtmlGenerator::buildHtml() {
    QMutexLocker lk(&mu_);
    QString html;
    if (download_.visible) {
        html += downloadSectionHtml();
    }
    if (speedtest_.visible) {
        html += speedtestSectionHtml();
    }
    if (latencyTest_.visible) {
        html += latencyTestSectionHtml();
    }
    // Deliberately last and conditional: the selector panel is ambient status,
    // so it yields the view entirely whenever a job wants to report progress.
    if (html.isEmpty() && autoSelector_.visible) {
        html += autoSelectorSectionHtml();
    }
    return html;
}

QString DataViewHtmlGenerator::autoSelectorSectionHtml() {
    QString res = QString("<p style='text-align:center;margin:0;'>%1</p>").arg(autoSelector_.summary.toHtmlEscaped());
    if (!autoSelector_.detail.isEmpty()) {
        res += QString("<p style='text-align:center;margin:0;opacity:0.75;'>%1</p>")
                   .arg(autoSelector_.detail.toHtmlEscaped());
    }
    return res;
}

QString DataViewHtmlGenerator::getProgressBar(long long current, long long total) {
    qint64 count = 0;
    if (total > 0) {
        count = 10 * current / total;
    }
    QString progressText;
    for (int i = 0; i < 10; i++) {
        if (count--; count >= 0) {
            progressText += "#";
        } else {
            progressText += "-";
        }
    }
    return progressText;
}

QString DataViewHtmlGenerator::downloadSectionHtml() {
    auto progressText = getProgressBar(download_.report.downloadedSize, download_.report.totalSize);
    const QString stat =
        ReadableSize(download_.report.downloadedSize) + "/" + ReadableSize(download_.report.totalSize);
    return QString("<p style='text-align:center;margin:0;'>Downloading %1: %2 %3</p>")
        .arg(download_.report.fileName, stat, progressText);
}

QString DataViewHtmlGenerator::speedtestSectionHtml() {
    if (speedtest_.kind == SpeedtestPanelState::Kind::Speed) {
        auto firstLine = speedtest_.stage.isEmpty()
            ? QStringLiteral("Running Speedtest: %1").arg(speedtest_.profileName)
            : QStringLiteral("%1: %2").arg(speedtest_.stage, speedtest_.profileName);
        if (speedtest_.totalProfiles > 1) {
            firstLine += QString(" (%1 / %2)").arg(Int2String(testProgress.load()), Int2String(speedtest_.totalProfiles));
        }
        if (speedtest_.tested != 0 || speedtest_.skipped != 0) {
            firstLine += QStringLiteral(" — %1 tested, %2 skipped")
                             .arg(speedtest_.tested).arg(speedtest_.skipped);
        }
        const QString elapsedLine = elapsedPanelLine(speedtest_.startedAtMs);
        return QString("<p style='%1'>%2</p>%3").arg(QLatin1String(kPanelLineStyle), firstLine, elapsedLine);
    } else {
        QString res;
        auto content = QString("Running Country Test");
        if (speedtest_.totalProfiles > 1) {
            auto progress = getProgressBar(testProgress.load(), speedtest_.totalProfiles);
            progress += QString(" ") + Int2String(100 * testProgress.load() / speedtest_.totalProfiles) + "%";
            res = QString("<p style='%1'>%2</p>").arg(QLatin1String(kPanelLineStyle), progress);
            content += QString(" (%1 / %2)").arg(Int2String(testProgress.load()), Int2String(speedtest_.totalProfiles));
        }
        res += QString("<p style='%1'>%2</p>").arg(QLatin1String(kPanelLineStyle), content);
        res += elapsedPanelLine(speedtest_.startedAtMs);
        return res;
    }
}

QString DataViewHtmlGenerator::latencyTestSectionHtml() {
    QString res;
    auto content =
        latencyTest_.kind == LatencyTestPanelState::Kind::Url ? QString("Running URL test") : QString("Running IP test");
    if (latencyTest_.totalProfiles > 1) {
        auto progress = getProgressBar(testProgress.load(), latencyTest_.totalProfiles);
        progress += QString(" ") + Int2String(100 * testProgress.load() / latencyTest_.totalProfiles) + "%";
        res = QString("<p style='text-align:center;margin:0;'>%1</p>").arg(progress);
        content += QString(" (%1 / %2)").arg(Int2String(testProgress.load()), Int2String(latencyTest_.totalProfiles));
    }
    res += QString("<p style='text-align:center;margin:0;'>%1</p>").arg(content);
    return res;
}
