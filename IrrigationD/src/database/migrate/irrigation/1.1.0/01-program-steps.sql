CREATE TABLE program_steps (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    program_id              INTEGER NOT NULL,
    sequence                INTEGER NOT NULL,
    duration_seconds        INTEGER NOT NULL,
    FOREIGN KEY (program_id) REFERENCES programs(id) ON DELETE CASCADE
);

CREATE TABLE program_step_zones (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    step_id                 INTEGER NOT NULL,
    zone_id                 INTEGER NOT NULL,
    FOREIGN KEY (step_id) REFERENCES program_steps(id) ON DELETE CASCADE,
    FOREIGN KEY (zone_id) REFERENCES zones(id)         ON DELETE CASCADE
);

INSERT INTO program_steps (id, program_id, sequence, duration_seconds)
    SELECT id, program_id, sequence, duration_seconds FROM program_zones
    WHERE program_id IN (SELECT id FROM programs) AND zone_id IN (SELECT id FROM zones);

INSERT INTO program_step_zones (step_id, zone_id)
    SELECT id, zone_id FROM program_zones
    WHERE id IN (SELECT id FROM program_steps) ORDER BY id;

DROP TABLE program_zones;

CREATE INDEX idx_program_steps_program  ON program_steps(program_id, sequence);
CREATE INDEX idx_step_zones_step        ON program_step_zones(step_id);

INSERT OR IGNORE INTO settings (key, value) VALUES ('max_concurrent_zones', '2');
