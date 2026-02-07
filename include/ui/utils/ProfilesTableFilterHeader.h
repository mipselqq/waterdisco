#pragma once

#include <QHeaderView>
#include <QLineEdit>
#include <QVector>
#include <QScrollBar>

class ProfilesTableFilterHeader : public QHeaderView {
    Q_OBJECT
public:
    explicit ProfilesTableFilterHeader(QWidget *parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent) {
        setSectionsClickable(true);
        setSortIndicatorShown(true);
        setDefaultAlignment(Qt::AlignHCenter | Qt::AlignTop);

        type_filter = new QLineEdit(this->viewport()); 
        type_filter->setPlaceholderText(tr("Filter..."));
        type_filter->setClearButtonEnabled(true);
        connect(type_filter, &QLineEdit::textChanged, [this](const QString &text) {
            emit typeFilterChanged(text);
        });

        address_filter = new QLineEdit(this->viewport()); 
        address_filter->setPlaceholderText(tr("Filter..."));
        address_filter->setClearButtonEnabled(true);
        connect(address_filter, &QLineEdit::textChanged, [this](const QString &text) {
            emit addressFilterChanged(text);
        });

        name_filter = new QLineEdit(this->viewport()); 
        name_filter->setPlaceholderText(tr("Filter..."));
        name_filter->setClearButtonEnabled(true);
        connect(name_filter, &QLineEdit::textChanged, [this](const QString &text) {
            emit nameFilterChanged(text);
        });

        test_filter = new QLineEdit(this->viewport()); 
        test_filter->setPlaceholderText(tr("Filter by country..."));
        test_filter->setClearButtonEnabled(true);
        connect(test_filter, &QLineEdit::textChanged, [this](const QString &text) {
            emit testFilterChanged(text);
        });

        connect(this, &QHeaderView::sectionResized, this, &ProfilesTableFilterHeader::adjustPositions);
    }

    QSize sizeHint() const override {
        QSize s = QHeaderView::sizeHint();
        s.setHeight(s.height() + 32);
        return s;
    }

protected:
    void updateGeometries() override {
        QHeaderView::updateGeometries();
        adjustPositions();
    }

private slots:
    void adjustPositions() {
        if (!address_filter || !name_filter || !type_filter || !test_filter || count() < 4) {
	        return;
	    }

        const int editHeight = 24;
        const int topPos = height() - editHeight - 4;

        type_filter->setGeometry(sectionViewportPosition(0) + 2, topPos, sectionSize(0) - 4, editHeight);
        address_filter->setGeometry(sectionViewportPosition(1) + 2, topPos, sectionSize(1) - 4, editHeight);
        name_filter->setGeometry(sectionViewportPosition(2) + 2, topPos, sectionSize(2) - 4, editHeight);
        test_filter->setGeometry(sectionViewportPosition(3) + 2, topPos, sectionSize(3) - 4, editHeight);
    }

signals:
    void typeFilterChanged(const QString &text);
    void addressFilterChanged(const QString &text);
    void nameFilterChanged(const QString &text);
    void testFilterChanged(const QString &text);

private:
    QLineEdit* type_filter;
    QLineEdit* address_filter;
    QLineEdit* name_filter;
    QLineEdit* test_filter;
};
