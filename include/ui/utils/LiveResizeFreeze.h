#pragma once

#include <QPointer>
#include <QWidget>

// Keeps the last painted frame visible while a widget is frozen during live
// window resize. The overlay is parented next to the target widget.
class LiveResizeFreeze {
public:
    void show(QWidget *widget);
    void syncGeometry();
    void hide();

    bool isActive() const { return m_widget != nullptr; }

private:
    QPointer<QWidget> m_widget;
    QPointer<QWidget> m_overlay;
};
