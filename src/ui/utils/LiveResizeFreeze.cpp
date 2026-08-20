#include "include/ui/utils/LiveResizeFreeze.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>

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

void LiveResizeFreeze::show(QWidget *widget) {
    if (!widget || !widget->parentWidget()) return;

    m_widget = widget;
    const QPixmap snap = widget->grab();

    auto *overlay = static_cast<LastFrameOverlay *>(m_overlay.data());
    if (!overlay) {
        overlay = new LastFrameOverlay(widget->parentWidget());
        m_overlay = overlay;
    } else if (overlay->parentWidget() != widget->parentWidget()) {
        overlay->setParent(widget->parentWidget());
    }
    overlay->setSnapshot(snap, widget->palette().color(QPalette::Base));
    overlay->setGeometry(widget->geometry());
    overlay->raise();
    overlay->show();
    widget->setUpdatesEnabled(false);
}

void LiveResizeFreeze::syncGeometry() {
    if (!m_widget || !m_overlay) return;
    m_overlay->setGeometry(m_widget->geometry());
}

void LiveResizeFreeze::hide() {
    if (m_widget) m_widget->setUpdatesEnabled(true);
    if (auto *overlay = static_cast<LastFrameOverlay *>(m_overlay.data())) {
        overlay->hide();
        overlay->clearSnapshot();
    }
    m_widget = nullptr;
}
