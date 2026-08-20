#include "include/ui/mainwindow.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QListWidget>
#include <QScrollBar>
#include <QTimer>

#include "include/database/GroupsRepo.h"
#include "include/ui/group/dialog_edit_group.h"

void MainWindow::show_group(int gid) {
    const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
    if (!group) return;

    Configs::dataManager->settingsRepo->current_group = gid;
    Configs::dataManager->settingsRepo->Save();
    scrollToGroup(gid);
}

void MainWindow::edit_group(int gid) {
    const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
    if (!group) return;

    auto *dialog = new DialogEditGroup(group, this);
    connect(dialog, &QDialog::finished, this, [this, dialog, group] {
        if (dialog->result() == QDialog::Accepted) {
            Configs::dataManager->groupsRepo->Save(group);
            MW_dialog_message(MwMessage::GroupsChanged, {});
        }
        dialog->deleteLater();
    });
    dialog->show();
}

void MainWindow::rebuildGroupSidebar() {
    if (!groupSidebar) return;
    refreshingGroupSidebar = true;
    groupSidebar->clear();

    int widest = groupSidebar->minimumSizeHint().width();
    for (int gid : Configs::dataManager->groupsRepo->GetGroupsTabOrder()) {
        const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
        if (!group) continue;
        auto *item = new QListWidgetItem(
            tr("%1  (%2)").arg(group->name).arg(group->Profiles().size()), groupSidebar);
        item->setData(Qt::UserRole, gid);
        item->setToolTip(group->name);
        widest = qMax(widest, groupSidebar->fontMetrics().horizontalAdvance(item->text()) + 28);
        if (gid == Configs::dataManager->settingsRepo->current_group) {
            groupSidebar->setCurrentItem(item);
        }
    }
    groupSidebar->setFixedWidth(widest);
    refreshingGroupSidebar = false;
}

void MainWindow::scrollToGroup(int groupId) {
    if (!profilesTableModel || !profilesFilterModel) return;
    for (int sourceRow = 0; sourceRow < profilesTableModel->rowCount(); ++sourceRow) {
        if (!profilesTableModel->isGroupHeader(sourceRow)
            || profilesTableModel->groupIdAt(sourceRow) != groupId) continue;
        const int row = profilesFilterModel->toProxyRow(sourceRow);
        if (row >= 0) {
            ui->profilesTableView->scrollTo(
                profilesFilterModel->index(row, 0), QAbstractItemView::PositionAtTop);
        }
        return;
    }
}

void MainWindow::applyGroupSectionSpans() {
    if (!profilesTableModel || !profilesFilterModel) return;
    auto *view = ui->profilesTableView;
    view->clearSpans();
    for (int sourceRow = 0; sourceRow < profilesTableModel->rowCount(); ++sourceRow) {
        if (!profilesTableModel->isGroupHeader(sourceRow)) continue;
        const int proxyRow = profilesFilterModel->toProxyRow(sourceRow);
        if (proxyRow >= 0) {
            view->setSpan(proxyRow, 0, 1, ProfilesTableModel::ColumnCount);
        }
    }
}

void MainWindow::refresh_groups() {
    const auto order = Configs::dataManager->groupsRepo->GetGroupsTabOrder();
    if (order.isEmpty()) return;
    if (!Configs::dataManager->groupsRepo->CurrentGroup()) {
        Configs::dataManager->settingsRepo->current_group = order.first();
        Configs::dataManager->settingsRepo->Save();
    }
    rebuildGroupSidebar();
    refresh_proxy_list({}, true);
}
