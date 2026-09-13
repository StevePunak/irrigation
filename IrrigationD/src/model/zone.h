#ifndef ZONE_H
#define ZONE_H

#include <QList>
#include <QString>

/**
 * @brief One physical valve zone on the manifold.
 *
 * number is the manifold position, 1 through 8.
 */
class Zone
{
public:
    int id = 0;
    int number = 0;
    QString name;
    bool enabled = true;

    /** @brief Returns true when this zone came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<Zone> ZoneList;

#endif // ZONE_H
