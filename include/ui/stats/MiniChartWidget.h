#pragma once

#include <QWidget>
#include <QColor>

#include <deque>
#include <functional>

class MiniChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit MiniChartWidget(QWidget* parent = nullptr);

    void setCapacity(int n);
    // An invalid QColor keeps the palette-derived default.
    void setColors(const QColor& primary, const QColor& secondary);
    // <= 0 auto-scales to a nice ceiling above the peak.
    void setMaxValue(double m);
    void setFormatter(std::function<QString(double)> formatter);
    void setCaption(const QString& caption);
    void push(double primary, double secondary);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::deque<double> a_;
    std::deque<double> b_;
    int cap_ = 60;
    double fixedMax_ = -1.0;
    QColor primary_;
    QColor secondary_;
    std::function<QString(double)> formatter_;
    QString caption_;
};
