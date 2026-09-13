#ifndef ZONE_H
#define ZONE_H

#include <QList>
#include <QString>

/** @brief One physical valve zone on the manifold. */
class Zone
{
public:
    /** @brief Primary key. Zero when not yet persisted. */
    int id = 0;

    /** @brief Manifold position, 1 through 8. */
    int number = 0;

    /** @brief Display name. */
    QString name;

    /** @brief Whether this zone may be scheduled or run manually. */
    bool enabled = true;

    /** @brief Returns true when this zone came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<Zone> ZoneList;

#endif // ZONE_H
