-- Draft S0 subset. No plugin/comp/bus/automation storage yet.
PRAGMA foreign_keys = ON;
PRAGMA user_version = 0;

CREATE TABLE project (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL CHECK (length(name) BETWEEN 1 AND 120),
    sample_rate INTEGER NOT NULL CHECK (sample_rate IN (44100, 48000)),
    tempo_bpm REAL NOT NULL CHECK (tempo_bpm BETWEEN 20 AND 400),
    revision INTEGER NOT NULL CHECK (revision >= 0)
);

CREATE TABLE tracks (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL REFERENCES project(id),
    name TEXT NOT NULL CHECK (length(name) BETWEEN 1 AND 120),
    position INTEGER NOT NULL CHECK (position >= 0),
    channels INTEGER NOT NULL CHECK (channels IN (1, 2)),
    gain_db REAL NOT NULL DEFAULT 0 CHECK (gain_db BETWEEN -120 AND 24),
    pan REAL NOT NULL DEFAULT 0 CHECK (pan BETWEEN -1 AND 1),
    muted INTEGER NOT NULL DEFAULT 0 CHECK (muted IN (0, 1)),
    UNIQUE (project_id, position)
);

CREATE TABLE assets (
    id TEXT PRIMARY KEY NOT NULL,
    relative_path TEXT NOT NULL UNIQUE,
    sha256 TEXT NOT NULL CHECK (length(sha256) = 64 AND sha256 NOT GLOB '*[^0-9a-f]*'),
    sample_rate INTEGER NOT NULL CHECK (sample_rate IN (44100, 48000)),
    channels INTEGER NOT NULL CHECK (channels IN (1, 2)),
    frames INTEGER NOT NULL CHECK (frames > 0)
);

CREATE TABLE clips (
    id TEXT PRIMARY KEY NOT NULL,
    track_id TEXT NOT NULL REFERENCES tracks(id),
    asset_id TEXT NOT NULL REFERENCES assets(id),
    start_frame INTEGER NOT NULL CHECK (start_frame >= 0),
    source_offset INTEGER NOT NULL CHECK (source_offset >= 0),
    duration_frames INTEGER NOT NULL CHECK (duration_frames > 0),
    gain_db REAL NOT NULL DEFAULT 0 CHECK (gain_db BETWEEN -120 AND 24),
    fade_in_frames INTEGER NOT NULL DEFAULT 0 CHECK (fade_in_frames >= 0),
    fade_out_frames INTEGER NOT NULL DEFAULT 0 CHECK (fade_out_frames >= 0),
    CHECK (fade_in_frames + fade_out_frames <= duration_frames)
);
CREATE INDEX clips_track_time ON clips(track_id, start_frame);

CREATE TABLE command_log (
    command_id TEXT PRIMARY KEY NOT NULL,
    revision INTEGER NOT NULL UNIQUE CHECK (revision > 0),
    forward_json TEXT NOT NULL,
    inverse_json TEXT NOT NULL,
    committed_at TEXT NOT NULL
);
