#pragma once

#include <QWidget>
#include <QList>
#include <QString>
#include <QRectF>

class TrafficChartWidget : public QWidget {
    Q_OBJECT

public:
    struct Bar {
        long long bucketStart = 0; // epoch secs; bucket spans [bucketStart, +bucketSecs)
        long long down = 0;
        long long up = 0;
        QString label;
    };

    explicit TrafficChartWidget(QWidget* parent = nullptr);

    void setData(const QList<Bar>& bars, int labelStride = 1, long long bucketSecs = 3600);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString bucketRangeText(long long bucketStart) const;

    QList<Bar> bars_;
    int labelStride_ = 1;
    long long bucketSecs_ = 3600;
    // Written by the last paint; index-parallel to bars_.
    QList<QRectF> barRects_;
    int hovered_ = -1;
};
