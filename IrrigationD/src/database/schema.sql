CREATE TABLE info (
    id                      INTEGER PRIMARY KEY,
    sw_version              TEXT NOT NULL
);

CREATE TABLE zones (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    number                  INTEGER NOT NULL UNIQUE,
    name                    TEXT NOT NULL DEFAULT '',
    enabled                 INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE programs (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    name                    TEXT NOT NULL,
    enabled                 INTEGER NOT NULL DEFAULT 1,
    day_mode                TEXT NOT NULL DEFAULT 'DaysOfWeek',
    dow_mask                INTEGER NOT NULL DEFAULT 0,
    interval_days           INTEGER,
    anchor_date             TEXT
);

CREATE TABLE program_start_times (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    program_id              INTEGER NOT NULL,
    minutes_after_midnight  INTEGER NOT NULL,
    timezone                TEXT NOT NULL,
    FOREIGN KEY (program_id) REFERENCES programs(id) ON DELETE CASCADE
);

CREATE TABLE program_zones (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    program_id              INTEGER NOT NULL,
    zone_id                 INTEGER NOT NULL,
    sequence                INTEGER NOT NULL,
    duration_seconds        INTEGER NOT NULL,
    FOREIGN KEY (program_id) REFERENCES programs(id) ON DELETE CASCADE,
    FOREIGN KEY (zone_id)    REFERENCES zones(id)    ON DELETE CASCADE
);

CREATE TABLE fired_instants (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    program_id              INTEGER NOT NULL,
    start_time_id           INTEGER NOT NULL,
    scheduled_at_utc        TEXT NOT NULL,
    outcome                 TEXT NOT NULL,
    UNIQUE (program_id, start_time_id, scheduled_at_utc)
);

CREATE TABLE settings (
    key                     TEXT PRIMARY KEY,
    value                   TEXT NOT NULL
);

CREATE INDEX idx_program_zones_program  ON program_zones(program_id, sequence);
CREATE INDEX idx_start_times_program    ON program_start_times(program_id);
CREATE INDEX idx_fired_scheduled        ON fired_instants(scheduled_at_utc);

INSERT INTO zones (number, name, enabled) VALUES
    (1, 'Zone 1', 1), (2, 'Zone 2', 1), (3, 'Zone 3', 1), (4, 'Zone 4', 1),
    (5, 'Zone 5', 1), (6, 'Zone 6', 1), (7, 'Zone 7', 1), (8, 'Zone 8', 1);

INSERT INTO settings (key, value) VALUES
    ('rain_delay_until', ''),
    ('master_enabled',   '1'),
    ('max_zone_seconds', '3600'),
    ('log_level',        'info');
