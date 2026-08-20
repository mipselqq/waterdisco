#include "include/ui/utils/ProfilesTableView.h"
#include "include/ui/utils/ProfilesTableVerticalHeader.h"
#include "include/ui/utils/ProfilesTableFilterHeader.h"
#include "include/ui/utils/ProfilesFilterProxyModel.h"
#include "include/ui/utils/ProfilesTableModel.h"
#include <QDragMoveEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainterPath>
#include <QRegion>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>

ProfilesTableView::ProfilesTableView(QWidget *parent)
    : QTableView(parent) {
    setDragDropMode(InternalMove);
    setDropIndicatorShown(true);
    setSelectionBehavior(SelectRows);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDefaultDropAction(Qt::MoveAction);

    m_verticalHeader = new ProfilesTableVerticalHeader(this);
    setVerticalHeader(m_verticalHeader);
    m_filterHeader = new ProfilesTableFilterHeader(this);
    setHorizontalHeader(m_filterHeader);

    m_chromeMaskTimer = new QTimer(this);
    m_chromeMaskTimer->setSingleShot(true);
    m_chromeMaskTimer->setInterval(15);
    connect(m_chromeMaskTimer, &QTimer::timeout, this, [this] { updateChromeMasks(); });
}

void ProfilesTableView::setModel(QAbstractItemModel *model) {
    QTableView::setModel(model);
    m_filterProxy = qobject_cast<ProfilesFilterProxyModel*>(model);
    auto *pm = m_filterProxy ? m_filterProxy->profilesModel()
                             : qobject_cast<ProfilesTableModel*>(model);
    m_verticalHeader->setProfilesModel(pm, m_filterProxy);
}

int ProfilesTableView::firstVisibleRow() {
    QRect rect = this->viewport()->rect();

    QModelIndex topIndex = indexAt(rect.topLeft());

    if (!topIndex.isValid()) return 0;

    int startRow = topIndex.row();
    return startRow;
}

void ProfilesTableView::resizeEvent(QResizeEvent *event) {
    QTableView::resizeEvent(event);
    if (!event->oldSize().isValid()) {
        updateChromeMasks();
        return;
    }
    deferChromeMasks();
}

void ProfilesTableView::deferChromeMasks() {
    // setMask() invalidates the whole widget. Rebuilding a viewport-sized
    // QRegion on every live-resize event was a major cost on large windows.
    if (!m_chromeMaskDeferred) {
        m_chromeMaskDeferred = true;
        if (auto *area = viewport()) area->clearMask();
        if (auto *bar = verticalScrollBar()) bar->clearMask();
        m_maskedViewportSize = QSize();
        m_maskedBarSize = QSize();
    }
    m_chromeMaskTimer->start();
}

void ProfilesTableView::settleChromeMasks() {
    if (!m_chromeMaskTimer) return;
    m_chromeMaskTimer->stop();
    updateChromeMasks();
}

void ProfilesTableView::updateChromeMasks() {
    constexpr qreal radius = 4.0;
    m_chromeMaskDeferred = false;

    // The viewport is a child of QAbstractScrollArea, so a stylesheet radius
    // on the view alone cannot clip its lower-left corner. This mask is only
    // recomputed after resize settles, never while scrolling.
    if (auto *area = viewport(); area && area->width() > radius && area->height() > radius) {
        if (area->size() != m_maskedViewportSize) {
            const QRectF rect = area->rect();
            QPainterPath path;
            path.moveTo(rect.topLeft());
            path.lineTo(rect.topRight());
            path.lineTo(rect.bottomRight());
            path.lineTo(rect.left() + radius, rect.bottom());
            path.quadTo(rect.bottomLeft(), QPointF(rect.left(), rect.bottom() - radius));
            path.lineTo(rect.topLeft());
            path.closeSubpath();
            area->setMask(QRegion(path.toFillPolygon().toPolygon()));
            m_maskedViewportSize = area->size();
        }
    }

    // The right edge belongs visually to the scrollbar, not the table body.
    // Clip only its two outer-right corners; the left side stays flush with
    // the viewport and header.
    auto *bar = verticalScrollBar();
    if (!bar || bar->width() <= radius || bar->height() <= radius) {
        m_maskedBarSize = QSize();
        return;
    }
    if (bar->size() == m_maskedBarSize) return;
    const QRectF rect = bar->rect();
    QPainterPath path;
    path.moveTo(rect.topLeft());
    path.lineTo(rect.right() - radius, rect.top());
    path.quadTo(rect.topRight(), QPointF(rect.right(), rect.top() + radius));
    path.lineTo(rect.right(), rect.bottom() - radius);
    path.quadTo(rect.bottomRight(), QPointF(rect.right() - radius, rect.bottom()));
    path.lineTo(rect.bottomLeft());
    path.closeSubpath();
    bar->setMask(QRegion(path.toFillPolygon().toPolygon()));
    m_maskedBarSize = bar->size();
}


void ProfilesTableView::keyPressEvent(QKeyEvent *event) {
    if (event->matches(QKeySequence::SelectAll) && selectAllRequested) {
        selectAllRequested();
        event->accept();
        return;
    }
    // A full viewport leaves no blank area to click, so Escape is the only way to
    // drop the selection.
    if (event->key() == Qt::Key_Escape && selectionModel() && selectionModel()->hasSelection()) {
        clearSelection();
        selectionModel()->clearCurrentIndex();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Up && m_filterHeader->filtersVisible()
        && currentIndex().isValid() && currentIndex().row() == 0) {
        m_filterHeader->focusLastFilterField();
        event->accept();
        return;
    }
    QTableView::keyPressEvent(event);
}

void ProfilesTableView::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasFormat("application/profile-row-number")) {

        event->accept();
        QTableView::dragEnterEvent(event);
    } else {
        event->ignore();
    }
}

void ProfilesTableView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasFormat("application/profile-row-number")) {
        QPoint pos = event->position().toPoint();
        QModelIndex targetIndex = indexAt(pos);
        if (!targetIndex.isValid()) {
            QModelIndex lastRowIndex = model()->index(model()->rowCount() - 1, 0);
            QRect rect = visualRect(lastRowIndex);
            if (event->pos().y() > rect.bottom()) {
                QPoint fakePos(rect.center().x(), rect.bottom() - 5);

                QDragMoveEvent fakeEvent(
                    fakePos,
                    event->possibleActions(),
                    event->mimeData(),
                    event->mouseButtons(),
                    event->keyboardModifiers()
                );
                QTableView::dragMoveEvent(&fakeEvent);
                event->accept();
                return;
            }
        }
        event->accept();
        QTableView::dragMoveEvent(event);
    } else {
        event->ignore();
    }
}

void ProfilesTableView::dropEvent(QDropEvent *event) {
    if (event->source() == this && event->mimeData()->hasFormat("application/profile-row-number")) {
        QByteArray encodedData = event->mimeData()->data("application/profile-row-number");
        QDataStream stream(&encodedData, QIODevice::ReadOnly);
        // The proxy maps drag indices to the source, so this is a source row.
        int rowNum;
        stream >> rowNum;

        QPoint pos = event->position().toPoint();
        QModelIndex targetIndex = indexAt(pos);

        int newRow;
        if (!targetIndex.isValid()) {
            newRow = model()->rowCount() - 1;
        } else {
            DropIndicatorPosition indicatorPos = dropIndicatorPosition();
            newRow = targetIndex.row();
            if (indicatorPos == AboveItem) {
                newRow--;
            }
        }
        // The drop target is a view row; bring it into rowNum's space.
        if (m_filterProxy && newRow >= 0) {
            newRow = m_filterProxy->toSourceRow(newRow);
            if (newRow < 0) return;
        }
        rowsSwapped(rowNum, newRow);
        event->accept();
    } else {
        QTableView::dropEvent(event);
    }
}
