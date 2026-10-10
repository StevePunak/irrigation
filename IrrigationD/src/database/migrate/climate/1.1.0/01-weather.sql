CREATE TABLE weather_hours (
    hour_end_utc            INTEGER PRIMARY KEY,
    latitude                REAL NOT NULL,
    longitude               REAL NOT NULL,
    precipitation_mm        REAL,
    et0_mm                  REAL,
    temperature_c           REAL,
    humidity_pct            REAL,
    fetched_at_utc          INTEGER NOT NULL
);

CREATE TABLE weather_current (
    at_utc                  INTEGER PRIMARY KEY,
    latitude                REAL NOT NULL,
    longitude               REAL NOT NULL,
    precipitation_mm        REAL,
    temperature_c           REAL,
    humidity_pct            REAL,
    fetched_at_utc          INTEGER NOT NULL
);
