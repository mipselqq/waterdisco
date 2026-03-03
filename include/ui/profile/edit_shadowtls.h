#ifndef EDIT_SHADOWTLS_H
#define EDIT_SHADOWTLS_H

#include <QWidget>
#include "profile_editor.h"
#include "ui_edit_shadowtls.h"

namespace Ui {
    class EditShadowTLS;
}

class EditShadowTLS : public QWidget, public ProfileEditor {
    Q_OBJECT

public:
    explicit EditShadowTLS(QWidget *parent = nullptr);

    ~EditShadowTLS() override;

    void onStart(std::shared_ptr<Configs::Profile> _ent) override;

    bool onEnd() override;

private:
    Ui::EditShadowTLS *ui;
    std::shared_ptr<Configs::Profile> ent;
};

#endif // EDIT_SHADOWTLS_H
