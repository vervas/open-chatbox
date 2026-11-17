/**
 * library_schema.h — Music library schema, shared by the indexer (writer)
 * and the UI's library reader, so the two can't drift.
 * See docs/player-spec.md §2.
 *
 * Conventions the reader can rely on:
 *   - art_id is NULL when there is no art; the UI draws the fallback cover.
 *   - Thumbnails live at <music_art_dir>/<art_id>_<size>.png for every size
 *     in LIBRARY_ART_SIZES. They are renamed into place before the
 *     transaction that references the art_id commits.
 *   - artist.sort_name is the name case-folded with a leading "The " dropped.
 *   - Ordering: artists by sort_name; albums by year, then title; tracks by
 *     disc_no, then track_no, then path (file name within the album).
 *   - meta.generation (an integer as text) goes up by one after every
 *     committed pass that changed something.
 *   - album.dir is the album's identity: its directory relative to music_dir
 *     ("." for the root). Under _inbox/, which mixes unrelated files,
 *     it is "<dir>\x1f<album artist>\x1f<album title>" instead.
 *   - track.path is absolute (music_dir joined with the relative path);
 *     track.mtime is in nanoseconds since the epoch, track.size in bytes.
 *   - The -wal and -shm files persist when the indexer closes
 *     (SQLITE_FCNTL_PERSIST_WAL): a SQLITE_OPEN_READONLY reader can't create
 *     them, so this is what lets the UI read while the indexer is stopped.
 */

#ifndef CHATBOX_LIBRARY_SCHEMA_H
#define CHATBOX_LIBRARY_SCHEMA_H

#define LIBRARY_SCHEMA_VERSION 1

/* Thumbnail edge lengths in px: lists, tiles, Now Playing. */
#define LIBRARY_ART_SIZES      { 96, 256, 512 }
#define LIBRARY_ART_SIZE_COUNT 3

/* meta keys */
#define LIBRARY_META_SCHEMA_VERSION "schema_version"
#define LIBRARY_META_GENERATION     "generation"
#define LIBRARY_META_LAST_SCAN      "last_scan"

/* Executed once by the indexer on a fresh DB (inside a transaction). */
#define LIBRARY_SCHEMA_SQL \
    "CREATE TABLE artist (id INTEGER PRIMARY KEY, name TEXT UNIQUE, sort_name TEXT, art_id INTEGER);\n" \
    "CREATE TABLE album  (id INTEGER PRIMARY KEY, artist_id INTEGER REFERENCES artist,\n" \
    "                     title TEXT, year INTEGER, dir TEXT UNIQUE, art_id INTEGER);\n" \
    "CREATE TABLE track  (id INTEGER PRIMARY KEY, album_id INTEGER REFERENCES album,\n" \
    "                     path TEXT UNIQUE, title TEXT, track_no INTEGER, disc_no INTEGER,\n" \
    "                     duration_ms INTEGER, mtime INTEGER, size INTEGER);\n" \
    "CREATE TABLE art    (id INTEGER PRIMARY KEY, sha1 TEXT UNIQUE, src TEXT);\n" \
    "CREATE TABLE meta   (key TEXT PRIMARY KEY, value TEXT);\n" \
    "CREATE INDEX track_album ON track(album_id);\n" \
    "CREATE INDEX album_artist ON album(artist_id);\n"

#endif /* CHATBOX_LIBRARY_SCHEMA_H */
