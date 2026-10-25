-- Схема сховища PAO (SQLite; сумісна з PostgreSQL за винятком типів часу).
-- Принцип: незмінний журнал подій і рішень. Будь-який стан можна відтворити
-- повторною подачею подій у ядро, що важливо для судового контролю.

CREATE TABLE IF NOT EXISTS hubs (
    id           TEXT PRIMARY KEY,
    name         TEXT NOT NULL,
    coordinator  TEXT
);

CREATE TABLE IF NOT EXISTS entities (
    id          TEXT PRIMARY KEY,
    name        TEXT NOT NULL,
    kind        TEXT,
    mode        TEXT NOT NULL CHECK (mode IN ('sea','water','rail','road','air')),
    class       TEXT NOT NULL CHECK (class IN ('essential','important')),
    hub_id      TEXT REFERENCES hubs(id),
    suppliers   TEXT NOT NULL DEFAULT '[]'          -- JSON-масив
);

CREATE TABLE IF NOT EXISTS events (
    seq         INTEGER PRIMARY KEY AUTOINCREMENT,
    t           REAL NOT NULL,
    type        TEXT NOT NULL,
    entity_id   TEXT,
    payload     TEXT NOT NULL                       -- подія як JSON
);
CREATE INDEX IF NOT EXISTS events_t ON events(t);

CREATE TABLE IF NOT EXISTS decisions (
    seq         INTEGER PRIMARY KEY AUTOINCREMENT,
    t           REAL NOT NULL,
    type        TEXT NOT NULL,                      -- escalation, urgent_open, deadline_breach, ...
    entity_id   TEXT,
    tier_from   INTEGER,
    tier_to     INTEGER,
    score       INTEGER,
    payload     TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS decisions_entity ON decisions(entity_id, t);

CREATE TABLE IF NOT EXISTS acts (
    number                 TEXT PRIMARY KEY,
    t                      REAL NOT NULL,
    entity_id              TEXT NOT NULL,
    kind                   TEXT NOT NULL,
    deadline               REAL NOT NULL,
    requires_human_review  INTEGER NOT NULL,
    status                 TEXT NOT NULL DEFAULT 'draft'
                           CHECK (status IN ('draft','approved','rejected','appealed','executed')),
    reviewer               TEXT,
    payload                TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS audits (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    t           REAL NOT NULL,
    entity_id   TEXT NOT NULL,
    outcome     TEXT NOT NULL CHECK (outcome IN ('confirmed','cleared')),
    score       INTEGER,
    e REAL, threat REAL, m REAL, h REAL
);

-- Звіт про прозорість (ст. 32(2)(e) NIS2: об'єктивні й прозорі критерії)
CREATE VIEW IF NOT EXISTS transparency_report AS
SELECT
    (SELECT COUNT(*) FROM decisions WHERE type = 'escalation')                       AS escalations,
    (SELECT COUNT(*) FROM decisions WHERE type = 'urgent_open')                      AS urgent_procedures,
    (SELECT COUNT(*) FROM decisions WHERE type = 'deadline_breach')                  AS reporting_breaches,
    (SELECT COUNT(*) FROM audits)                                                    AS audits,
    (SELECT COUNT(*) FROM audits WHERE outcome = 'cleared')                          AS audits_cleared,
    (SELECT COUNT(*) FROM acts WHERE requires_human_review = 1)                      AS acts_human_review;
