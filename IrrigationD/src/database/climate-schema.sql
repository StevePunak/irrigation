CREATE TABLE info (
    id                      INTEGER PRIMARY KEY,
    sw_version              TEXT NOT NULL
);

CREATE TABLE readings (
    at_utc                  INTEGER NOT NULL,
    temperature_c           REAL NOT NULL,
    humidity_pct            REAL NOT NULL
);

CREATE INDEX readings_at_utc ON readings (at_utc);
