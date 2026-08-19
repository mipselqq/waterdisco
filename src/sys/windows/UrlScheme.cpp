#include "include/sys/UrlScheme.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include <shlobj.h>

// Per-user registration under HKCU\Software\Classes — needs no admin and takes
// precedence over any system-wide handler. In QSettings NativeFormat the value
// name "Default" maps to a registry key's unnamed (Default) value, and '/'
// separates subkeys.

static const QString kClasses = "HKEY_CURRENT_USER\\Software\\Classes";
static const QString kProgId = "Throne.Config";

// Extensions config files usually arrive with. Registering these only adds
// Throne to the "Open with" list; the extension keeps whatever default it has.
static const QStringList kConfigExtensions = {".json", ".conf", ".yaml", ".yml", ".ini", ".txt"};

static QString openCommand() {
    return "\"" + QDir::toNativeSeparators(QApplication::applicationFilePath()) + "\" \"%1\"";
}

QString UrlScheme_DesiredState() {
    return "v2|" + openCommand();
}

void UrlScheme_Apply() {
    const QString command = openCommand();
    const QString exe = QDir::toNativeSeparators(QApplication::applicationFilePath());

    QSettings scheme(kClasses + "\\throne", QSettings::NativeFormat);
    scheme.setValue("Default", "URL:Waterdisco Protocol");
    scheme.setValue("URL Protocol", "");
    scheme.setValue("shell/open/command/Default", command);

    QSettings progId(kClasses + "\\" + kProgId, QSettings::NativeFormat);
    progId.setValue("Default", "Waterdisco profile");
    progId.setValue("DefaultIcon/Default", exe + ",0");
    progId.setValue("shell/open/command/Default", command);

    // OpenWithProgids is the additive half of an association: the extension lists
    // us as one possible handler, its default (HKCU\...\<ext>\Default) is left alone.
    for (const QString &ext : kConfigExtensions) {
        QSettings assoc(kClasses + "\\" + ext + "\\OpenWithProgids", QSettings::NativeFormat);
        assoc.setValue(kProgId, "");
    }

    // Applications\<exe> is what "Open with > Choose another app" reads, which is
    // the only route for a config file that has no extension at all.
    QSettings app(kClasses + "\\Applications\\" + QFileInfo(exe).fileName(), QSettings::NativeFormat);
    app.setValue("FriendlyAppName", "Waterdisco");
    app.setValue("shell/open/command/Default", command);
    for (const QString &ext : kConfigExtensions) {
        app.setValue("SupportedTypes/" + ext, "");
    }

    // QSettings only reaches the registry on sync, and the shell caches association
    // data until told otherwise, so flush before announcing the change.
    scheme.sync();
    progId.sync();
    app.sync();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
