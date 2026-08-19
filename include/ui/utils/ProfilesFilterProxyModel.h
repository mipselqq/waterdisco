#pragma once

#include <QSortFilterProxyModel>
#include <QString>

class ProfilesTableModel;

// Filters the profiles table by the filter header's four fields. Hiding rows
// rather than resetting the source model is what keeps the selection, current
// row, scroll position and profile cache alive across a filter change.
class ProfilesFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ProfilesFilterProxyModel(QObject *parent = nullptr);

    // An empty string disables that test. `address` also accepts "port=N",
    // "port=MIN:MAX", "port=MIN:" and "port=:MAX".
    void setFilters(const QString &type, const QString &address, const QString &name, const QString &country);
    bool hasActiveFilter() const;

    ProfilesTableModel *profilesModel() const;

    // -1 when the row has no counterpart (filtered out, or out of range).
    int toSourceRow(int proxyRow) const;
    int toProxyRow(int sourceRow) const;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    bool portMatches(int port) const;
    bool profileMatches(int sourceRow) const;
    bool groupHasMatchingProfile(int headerRow) const;

    QString m_type;
    QString m_address;
    QString m_name;
    QString m_country;
};
