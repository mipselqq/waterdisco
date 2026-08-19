#pragma once

#include <QTableView>
#include <functional>

// Table view for the profiles list: drag-drop reorder and custom vertical header.
class ProfilesTableView : public QTableView {
    Q_OBJECT
public:
    explicit ProfilesTableView(QWidget *parent = nullptr);

    // Drop-to-reorder: (from, to) as source-model rows, not view rows.
    std::function<void(int row1, int row2)> rowsSwapped;
    std::function<void()> selectAllRequested;

    void setModel(QAbstractItemModel *model) override;

    int firstVisibleRow();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    class ProfilesTableVerticalHeader *m_verticalHeader = nullptr;
    class ProfilesTableFilterHeader *m_filterHeader = nullptr;
    class ProfilesFilterProxyModel *m_filterProxy = nullptr;
};
