#include "include/ui/widget/TrayProfileSelector.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QTimer>
#include <QKeyEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QPalette>

#include "include/global/Configs.hpp"
#include "include/global/Common.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/database/RoutesRepo.h"
#include "include/database/SettingsRepo.h"

namespace {
    constexpr int kPerPage = 12;
    constexpr int kSearchDebounceMs = 200;

    enum ItemKind { KindGroup = 1, KindProfile, KindRoute };
    constexpr int RoleKind = Qt::UserRole;
    constexpr int RoleId = Qt::UserRole + 1;
}

TrayProfileSelector::TrayProfileSelector(Mode mode, Callbacks cb, QWidget *parent)
    : QFrame(parent), m_mode(mode), m_cb(std::move(cb)) {
    // Translucent so the card's rounded corners come out anti-aliased; a bitmap mask would be jagged.
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setFrameShape(QFrame::NoFrame);
    setMinimumWidth(300);

    const QString bg = palette().color(QPalette::Window).name();
    const QString base = palette().color(QPalette::Base).name();
    const QString border = palette().color(QPalette::Mid).name();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("trayCard"));
    card->setStyleSheet(QStringLiteral(
        "QFrame#trayCard { background-color:%1; border:1px solid %2; border-radius:10px; }")
        .arg(bg, border));
    outer->addWidget(card);

    auto *root = new QVBoxLayout(card);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(6);

    auto *searchRow = new QHBoxLayout();
    m_search = new QLineEdit(card);
    m_search->setObjectName(QStringLiteral("traySearch"));
    m_search->setPlaceholderText(tr("Search…"));
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);
    m_search->setStyleSheet(QStringLiteral(
        "QLineEdit#traySearch { border:1px solid %1; border-radius:8px; padding:5px 9px; background-color:%2; }")
        .arg(border, base));
    auto *closeBtn = new QPushButton(QStringLiteral("✕"), card);
    closeBtn->setFixedWidth(28);
    closeBtn->setToolTip(tr("Close"));
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(closeBtn);
    root->addLayout(searchRow);

    auto *header = new QHBoxLayout();
    m_backBtn = new QPushButton(QStringLiteral("◀"), card);
    m_backBtn->setFixedWidth(28);
    m_backBtn->setToolTip(tr("Back to groups"));
    m_title = new QLabel(card);
    m_title->setStyleSheet(QStringLiteral("font-weight:600; border:none;"));
    header->addWidget(m_backBtn);
    header->addWidget(m_title, 1);
    root->addLayout(header);

    m_stopBtn = new QPushButton(card);
    root->addWidget(m_stopBtn);

    m_list = new QListWidget(card);
    m_list->setUniformItemSizes(true);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setMinimumHeight(300);
    m_list->installEventFilter(this);
    root->addWidget(m_list, 1);

    auto *footer = new QHBoxLayout();
    m_prevBtn = new QPushButton(QStringLiteral("◀"), card);
    m_prevBtn->setFixedWidth(36);
    m_pageLabel = new QLabel(card);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_pageLabel->setStyleSheet(QStringLiteral("border:none;"));
    m_nextBtn = new QPushButton(QStringLiteral("▶"), card);
    m_nextBtn->setFixedWidth(36);
    footer->addWidget(m_prevBtn);
    footer->addWidget(m_pageLabel, 1);
    footer->addWidget(m_nextBtn);
    root->addLayout(footer);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kSearchDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        m_query = m_search->text().trimmed();
        m_page = 0;
        rebuild();
    });
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_debounce->start(); });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        QListWidgetItem *it = m_list->currentItem();
        if (!it && m_list->count() > 0) it = m_list->item(0);
        activateItem(it);
    });

    connect(closeBtn, &QPushButton::clicked, this, [this] { close(); });
    connect(m_backBtn, &QPushButton::clicked, this, [this] { goBackToGroups(); });
    connect(m_prevBtn, &QPushButton::clicked, this, [this] { changePage(-1); });
    connect(m_nextBtn, &QPushButton::clicked, this, [this] { changePage(+1); });
    connect(m_stopBtn, &QPushButton::clicked, this, [this] {
        if (m_cb.stopProfile) m_cb.stopProfile();
        close();
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { activateItem(it); });
}

void TrayProfileSelector::ensureServerCache() {
    if (m_serverCacheBuilt) return;
    m_serverCacheBuilt = true;
    QList<int> allIds;
    for (auto gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
        auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
        if (!group || group->archive) continue;
        allIds.append(group->Profiles());
    }
    // GetProfileIDNameMappedBatch chunks internally, so a large id list is safe.
    m_serverCache = Configs::dataManager->profilesRepo->GetProfileIDNameMappedBatch(allIds);
    m_serverCache = FixProfileDisplayName(m_serverCache);
    m_serverCacheLower.reserve(m_serverCache.size());
    for (const auto &[id, name] : m_serverCache) m_serverCacheLower.append(name.toLower());
}

void TrayProfileSelector::ensureRouteCache() {
    if (m_routeCacheBuilt) return;
    m_routeCacheBuilt = true;
    for (const auto &route : Configs::dataManager->routesRepo->GetAllRouteProfiles())
        m_routeCache.append({route->id, route->name});
    m_routeCacheLower.reserve(m_routeCache.size());
    for (const auto &[id, name] : m_routeCache) m_routeCacheLower.append(name.toLower());
}

void TrayProfileSelector::rebuild() {
    m_list->clear();
    const QString q = m_query.toLower();
    const bool searching = !q.isEmpty();

    struct Entry {
        int kind;
        int id;
        QString text;
        bool enabled;
        bool current;
    };
    QList<Entry> entries;
    QString title;
    bool showBack = false;
    bool showStop = false;
    QString stopText;

    if (m_mode == Routing) {
        title = tr("Select Routing");
        ensureRouteCache();
        const int cur = Configs::dataManager->settingsRepo->current_route_id;
        for (int i = 0; i < m_routeCache.size(); ++i) {
            if (searching && !m_routeCacheLower[i].contains(q)) continue;
            entries.append({KindRoute, m_routeCache[i].first, m_routeCache[i].second, true,
                            m_routeCache[i].first == cur});
        }
        if (entries.isEmpty())
            entries.append({0, -1, searching ? tr("No matches") : tr("No routing profiles"), false, false});
    } else {
        const bool running = m_cb.isRunning && m_cb.isRunning();
        const int rid = (running && m_cb.runningId) ? m_cb.runningId() : -1;
        if (searching) {
            title = tr("Select Server");
            ensureServerCache();
            for (int i = 0; i < m_serverCache.size(); ++i) {
                if (!m_serverCacheLower[i].contains(q)) continue;
                entries.append({KindProfile, m_serverCache[i].first, m_serverCache[i].second, true,
                                m_serverCache[i].first == rid});
            }
            if (entries.isEmpty()) entries.append({0, -1, tr("No matches"), false, false});
        } else if (m_groupId < 0) {
            title = tr("Select Server");
            if (running) {
                showStop = true;
                stopText = tr("Stop: %1").arg(m_cb.runningName ? m_cb.runningName() : QString());
            }
            const int rgid = (running && m_cb.runningGid) ? m_cb.runningGid() : -1;
            for (auto gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
                auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
                if (!group || group->archive || group->Profiles().isEmpty()) continue;
                entries.append({KindGroup, gid, group->name, true, gid == rgid});
            }
            if (entries.isEmpty()) entries.append({0, -1, tr("No servers"), false, false});
        } else {
            auto group = Configs::dataManager->groupsRepo->GetGroup(m_groupId);
            if (!group || group->archive) {
                goBackToGroups();
                return;
            }
            showBack = true;
            title = group->name;
            auto mapped = Configs::dataManager->profilesRepo->GetProfileIDNameMappedBatch(group->Profiles());
            mapped = FixProfileDisplayName(mapped);
            for (const auto &[id, name] : mapped) {
                entries.append({KindProfile, id, name, true, id == rid});
            }
            if (entries.isEmpty()) entries.append({0, -1, tr("No servers"), false, false});
        }
    }

    const int total = static_cast<int>(entries.size());
    const int pages = qMax(1, (total + kPerPage - 1) / kPerPage);
    m_page = qBound(0, m_page, pages - 1);
    const int start = m_page * kPerPage;
    const int end = qMin(start + kPerPage, total);

    for (int i = start; i < end; ++i) {
        const Entry &e = entries[i];
        QString label = e.text;
        if (e.current) label = QStringLiteral("✓ ") + label;
        if (e.kind == KindGroup) label += QStringLiteral("   ▶");
        auto *it = new QListWidgetItem(label, m_list);
        it->setData(RoleKind, e.kind);
        it->setData(RoleId, e.id);
        if (!e.enabled) it->setFlags(it->flags() & ~Qt::ItemIsEnabled);
    }
    if (m_list->count() > 0) m_list->setCurrentRow(0);

    m_title->setText(title);
    m_backBtn->setVisible(showBack);
    m_stopBtn->setVisible(showStop);
    if (showStop) m_stopBtn->setText(stopText);

    const bool paginate = pages > 1;
    m_prevBtn->setVisible(paginate);
    m_nextBtn->setVisible(paginate);
    m_pageLabel->setVisible(paginate);
    if (paginate) {
        m_pageLabel->setText(tr("Page %1/%2").arg(m_page + 1).arg(pages));
        m_prevBtn->setEnabled(m_page > 0);
        m_nextBtn->setEnabled(m_page < pages - 1);
    }
}

void TrayProfileSelector::activateItem(QListWidgetItem *it) {
    if (!it || !(it->flags() & Qt::ItemIsEnabled)) return;
    const int kind = it->data(RoleKind).toInt();
    const int id = it->data(RoleId).toInt();
    if (kind == KindGroup) {
        m_groupId = id;
        m_page = 0;
        rebuild();
    } else if (kind == KindProfile) {
        if (m_cb.startProfile) m_cb.startProfile(id);
        close();
    } else if (kind == KindRoute) {
        if (m_cb.chooseRoute) m_cb.chooseRoute(id);
        close();
    }
}

void TrayProfileSelector::goBackToGroups() {
    m_groupId = -1;
    m_page = 0;
    rebuild();
}

void TrayProfileSelector::changePage(int delta) {
    m_page += delta;
    rebuild();
}

void TrayProfileSelector::popupAt(const QPoint &globalPos) {
    m_groupId = -1;
    m_page = 0;
    m_query.clear();
    m_search->blockSignals(true); // don't fire the debounce for a programmatic clear
    m_search->clear();
    m_search->blockSignals(false);
    rebuild();
    adjustSize();

    QScreen *scr = QGuiApplication::screenAt(globalPos);
    if (!scr) scr = QGuiApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1024, 768);
    const QSize sz = size();
    int x = globalPos.x();
    int y = globalPos.y();
    if (x + sz.width() > avail.right()) x = avail.right() - sz.width();
    if (y + sz.height() > avail.bottom()) y = avail.bottom() - sz.height();
    if (x < avail.left()) x = avail.left();
    if (y < avail.top()) y = avail.top();
    move(x, y);

    show();
    raise();
    activateWindow();
    m_search->setFocus();

    // Don't let the focus churn during show() dismiss us immediately.
    m_armed = false;
    QTimer::singleShot(150, this, [this] { m_armed = true; });
}

bool TrayProfileSelector::event(QEvent *e) {
    if (e->type() == QEvent::WindowDeactivate && m_armed) {
        close();
    }
    return QFrame::event(e);
}

void TrayProfileSelector::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QFrame::keyPressEvent(e);
}

bool TrayProfileSelector::eventFilter(QObject *obj, QEvent *e) {
    if (e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Escape) {
            close();
            return true;
        }
        if (obj == m_list && (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)) {
            activateItem(m_list->currentItem());
            return true;
        }
        if (obj == m_search && ke->key() == Qt::Key_Down && m_list->count() > 0) {
            m_list->setFocus();
            m_list->setCurrentRow(0);
            return true;
        }
    }
    return QFrame::eventFilter(obj, e);
}
