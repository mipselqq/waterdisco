#include "include/ui/profile/edit_hysteria2.h"

EditHysteria2::EditHysteria2(QWidget *parent)
    : QWidget(parent),
      ui(new Ui::EditHysteria2) {
    ui->setupUi(this);
}

EditHysteria2::~EditHysteria2() {
    delete ui;
}

void EditHysteria2::onStart(std::shared_ptr<Configs::ProxyEntity> _ent) {
    this->ent = _ent;
    auto outbound = this->ent->Hysteria2();

    ui->server_ports->setText(outbound->server_ports.join(","));
    ui->hop_interval->setText(outbound->hop_interval);
    ui->up_mbps->setText(Int2String(outbound->up_mbps));
    ui->down_mbps->setText(Int2String(outbound->down_mbps));
    ui->obfs_password->setText(outbound->obfsPassword);
    ui->password->setText(outbound->password);
}

bool EditHysteria2::onEnd() {
    auto outbound = this->ent->Hysteria2();

    outbound->server_ports = SplitAndTrim(ui->server_ports->text(), ",");
    outbound->hop_interval = ui->hop_interval->text();
    outbound->up_mbps = ui->up_mbps->text().toInt();
    outbound->down_mbps = ui->down_mbps->text().toInt();
    outbound->obfsPassword = ui->obfs_password->text();
    outbound->password = ui->password->text();
    return true;
}