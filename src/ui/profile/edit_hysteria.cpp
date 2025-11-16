#include "include/ui/profile/edit_hysteria.h"

EditHysteria::EditHysteria(QWidget *parent)
    : QWidget(parent),
      ui(new Ui::EditHysteria) {
    ui->setupUi(this);
}

EditHysteria::~EditHysteria() {
    delete ui;
}

void EditHysteria::onStart(std::shared_ptr<Configs::ProxyEntity> _ent) {
    this->ent = _ent;
    auto outbound = _ent->Hysteria();

    ui->server_ports->setText(outbound->server_ports.join(","));
    ui->hop_interval->setText(outbound->hop_interval);
    ui->up_mbps->setText(Int2String(outbound->up_mbps));
    ui->down_mbps->setText(Int2String(outbound->down_mbps));
    ui->obfs->setText(outbound->obfs);
    ui->auth->setText(outbound->auth);
    ui->auth_str->setText(outbound->auth_str);
    ui->recv_window->setText(Int2String(outbound->recv_window));
    ui->recv_window_conn->setText(Int2String(outbound->recv_window_conn));
}

bool EditHysteria::onEnd() {
    auto outbound = ent->Hysteria();
    outbound->server_ports = SplitAndTrim(ui->server_ports->text(), ",");
    outbound->hop_interval = ui->hop_interval->text();
    outbound->up_mbps = ui->up_mbps->text().toInt();
    outbound->down_mbps = ui->down_mbps->text().toInt();
    outbound->obfs = ui->obfs->text();
    outbound->auth = ui->auth->text();
    outbound->auth_str = ui->auth_str->text();
    outbound->recv_window = ui->recv_window->text().toInt();
    outbound->recv_window_conn = ui->recv_window_conn->text().toInt();
    return true;
}

