#pragma once

#include <QFrame>
#include <QString>
#include <QStringList>
#include <QList>
#include <functional>
#include <utility>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

// A window, not a tray submenu: Linux SNI/DBusMenu and macOS NSMenu won't reliably expand one populated dynamically.
class TrayProfileSelector : public QFrame {
    Q_OBJECT
public:
    enum Mode { Server, Routing };

    struct Callbacks {
        std::function<void(int id)> startProfile;
        std::function<void()>       stopProfile;
        std::function<void(int id)> chooseRoute;
        std::function<bool()>       isRunning;
        std::function<int()>        runningId;
        std::function<int()>        runningGid;
        std::function<QString()>    runningName;
    };

    TrayProfileSelector(Mode mode, Callbacks cb, QWidget *parent = nullptr);

    void popupAt(const QPoint &globalPos);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void rebuild();
    void activateItem(QListWidgetItem *it);
    void goBackToGroups();
    void changePage(int delta);
    void ensureServerCache();
    void ensureRouteCache();

    Mode m_mode;
    Callbacks m_cb;
    int m_groupId = -1;   // Server mode: -1 = group list; else the drilled-in group id
    int m_page = 0;
    bool m_armed = false;
    QString m_query;

    bool m_serverCacheBuilt = false;
    QList<std::pair<int, QString>> m_serverCache;
    QStringList m_serverCacheLower;
    bool m_routeCacheBuilt = false;
    QList<std::pair<int, QString>> m_routeCache;
    QStringList m_routeCacheLower;

    QLineEdit *m_search = nullptr;
    QTimer *m_debounce = nullptr;
    QPushButton *m_backBtn = nullptr;
    QLabel *m_title = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_prevBtn = nullptr;
    QLabel *m_pageLabel = nullptr;
    QPushButton *m_nextBtn = nullptr;
};
