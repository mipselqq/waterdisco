#ifndef EDIT_HYSTERIA2_H
#define EDIT_HYSTERIA2_H

#include <QWidget>
#include "profile_editor.h"
#include "ui_edit_hysteria2.h"

namespace Ui {
    class EditHysteria2;
}

class EditHysteria2 : public QWidget, public ProfileEditor {
    Q_OBJECT

public:
    explicit EditHysteria2(QWidget *parent = nullptr);

    ~EditHysteria2() override;

    void onStart(std::shared_ptr<Configs::ProxyEntity> _ent) override;

    bool onEnd() override;

private:
    Ui::EditHysteria2 *ui;
    std::shared_ptr<Configs::ProxyEntity> ent;
};

#endif // EDIT_HYSTERIA2_H
