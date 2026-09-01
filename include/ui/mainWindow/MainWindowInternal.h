#pragma once

// Shared internals of src/ui/mainWindow/: everything must be inline or a macro (unity builds).

#include <QDialog>

#include "include/global/Configs.hpp"
#include "include/database/GroupsRepo.h"

inline bool dialog_is_using = false;

inline bool mw_sub_updating = false;

// Expands inside a MainWindow member function: uses `this` and `connect`.
#define USE_DIALOG(a)                                    \
    if (dialog_is_using) return;                         \
    dialog_is_using = true;                              \
    auto dialog = new a(this);                           \
    connect(dialog, &QDialog::finished, this, [=, this] { \
        dialog->deleteLater();                           \
        dialog_is_using = false;                         \
    });                                                  \
    dialog->show();
