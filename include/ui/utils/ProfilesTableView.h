#pragma once

#include <QTableView>
#include <QSize>
#include <functional>

class ProfilesTableView : public QTableView {
    Q_OBJECT
public:
    explicit ProfilesTableView(QWidget *parent = nullptr);

    // Drop-to-reorder: (from, to) as source-model rows, not view rows.
    std::function<void(int row1, int row2)> rowsSwapped;
    std::function<void()> selectAllRequested;

    void setModel(QAbstractItemModel *model) override;

    int firstVisibleRow();

    void settleChromeMasks();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void deferChromeMasks();
    void updateChromeMasks();
    class ProfilesTableVerticalHeader *m_verticalHeader = nullptr;
    class ProfilesTableFilterHeader *m_filterHeader = nullptr;
    class ProfilesFilterProxyModel *m_filterProxy = nullptr;
    class QTimer *m_chromeMaskTimer = nullptr;
    bool m_chromeMaskDeferred = false;
    QSize m_maskedViewportSize;
    QSize m_maskedBarSize;
};
