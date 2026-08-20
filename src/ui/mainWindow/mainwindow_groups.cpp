#include "include/ui/mainwindow.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>

#include "include/configs/sub/GroupUpdater.hpp"
#include "include/database/GroupsRepo.h"
#include "include/ui/group/dialog_edit_group.h"
#include "include/ui/mainWindow/MainWindowInternal.h"

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

void MainWindow::delete_group(int gid) {
    if (Configs::dataManager->groupsRepo->GetAllGroupIds().size() <= 1) return;
    const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
    if (!group) return;
    if (QMessageBox::question(this, tr("Confirmation"), tr("Remove %1?").arg(group->name)) !=
        QMessageBox::StandardButton::Yes) {
        return;
    }
    if (running != nullptr && running->gid == gid) profile_stop(false, true, false);
    Configs::dataManager->groupsRepo->DeleteGroup(gid);
    MW_dialog_message(MwMessage::GroupsChanged, {});
}

void MainWindow::update_group_subscription(int gid) {
    const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
    if (!group || group->url.isEmpty()) return;
    if (mw_sub_updating) return;
    mw_sub_updating = true;
    Subscription::groupUpdater->AsyncUpdate(group->url, group->id, [&] { mw_sub_updating = false; }, true);
}

void MainWindow::showGroupSidebarMenu(const QPoint &pos) {
    if (!groupSidebar) return;
    QListWidgetItem *item = groupSidebar->itemAt(pos);
    if (!item) return;
    const int gid = item->data(Qt::UserRole).toInt();
    const auto group = Configs::dataManager->groupsRepo->GetGroup(gid);
    if (!group) return;

    groupSidebar->setCurrentItem(item);

    QMenu menu(this);
    QAction *editAct = menu.addAction(tr("Edit"));
    QAction *updateAct = menu.addAction(tr("Update Subscription"));
    updateAct->setEnabled(!group->url.isEmpty());
    QAction *removeAct = menu.addAction(tr("Remove"));
    removeAct->setEnabled(Configs::dataManager->groupsRepo->GetAllGroupIds().size() > 1);

    QAction *chosen = menu.exec(groupSidebar->viewport()->mapToGlobal(pos));
    if (chosen == editAct) edit_group(gid);
    else if (chosen == updateAct) update_group_subscription(gid);
    else if (chosen == removeAct) delete_group(gid);
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
    if (mainContentSplitter) {
        if (QWidget *pane = mainContentSplitter->widget(0)) {
            const QMargins margins = pane->layout() ? pane->layout()->contentsMargins() : QMargins();
            pane->setFixedWidth(widest + margins.left() + margins.right());
        }
    }
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
    if (profilesTableModel->isFlatList()) return;
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
