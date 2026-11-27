# Search Engine Database Layer: Full Context and Handoff

Paste this whole file at the start of a new chat so Claude has the same context as the chat it was written in. Everything below was designed and reviewed step by step with the project owner. Code is reproduced with tidied indentation, otherwise identical to what the owner has. **Nothing has been compiled or run yet.** Tests are planned for later.

---

## 1. Project goal

A small search engine in C++ with three components:

| Component | Role | Database access |
|---|---|---|
| **Crawler** | picks URLs from the DB, downloads pages, extracts links, saves everything | read-write |
| **Indexer** | reads crawled pages, builds the search index, marks pages indexed | read-write |
| **Query engine** | answers searches | read-only |

Each component creates **its own `database` object** (its own SQLite connection) and calls functions through it. Objects are never shared across threads.

Stack: C++17 (structured bindings are used), SQLiteCpp wrapper over SQLite, a single SQLite file (default `db/search_engine.db`), schema in `db/schema.sql`. Needs SQLite >= 3.35 (`RETURNING`). Project path seen in the owner's terminal: `~/dev/miniProjects/basic-search-db`, edited with nvim.

### Goals for the function `saveCrawledPages`

The owner's explicit requirements:

1. **Dedupe by content.** If a downloaded page's content hash already exists under another URL, do **not** save its content. Mark the new URL as crawled so the crawler doesn't fetch it again.
2. **URL already exists:** on conflict of URL, update its html and hash and set `isCrawled = 1`.
3. **Linked pages go into the frontier** so the crawler can find them later; every source->target pair is recorded in `page_links`.
4. **`frequency` = how many different pages reference this URL.** Repeated links from one page, or re-crawls, must not inflate it.
5. **One bad entry must not lose the batch.** Skip the whole bad entry (save nothing from it) and continue.
6. **Fast bulk writes:** one transaction per batch, prepared statements reused, ID cache, frequency applied once per target.
7. **Built and understood piece by piece**, as ONE function (not classes), with each part explained.

---

## 2. Schema (`db/schema.sql`)

```sql
CREATE TABLE IF NOT EXISTS frontier (
	id INTEGER PRIMARY KEY,
	url TEXT NOT NULL UNIQUE,

	frequency INTEGER NOT NULL DEFAULT 1
		CHECK (frequency >= 1),

	isCrawled INTEGER NOT NULL DEFAULT 0
		CHECK (isCrawled IN (0, 1)),

	isIndexed INTEGER NOT NULL DEFAULT 0
		CHECK (isIndexed IN (0, 1)),

	html BLOB,
	contentHash BLOB

	);

CREATE TABLE IF NOT EXISTS page_links(
	source_id INTEGER NOT NULL,
	target_id INTEGER NOT NULL,

	PRIMARY KEY(source_id, target_id),

	FOREIGN KEY (source_id) REFERENCES frontier(id) ON DELETE CASCADE,
	FOREIGN KEY (target_id) REFERENCES frontier(id) ON DELETE CASCADE 
	) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_page_links_target
ON page_links(target_id);

CREATE UNIQUE INDEX IF NOT EXISTS idx_frontier_contenthash
ON frontier(contentHash);

CREATE INDEX IF NOT EXISTS idx_frontier_queue
ON frontier(frequency DESC) WHERE isCrawled = 0;
```

Notes on the schema:

- `frontier` holds **one row per URL**, whether crawled or only discovered. A discovered-but-not-crawled URL is a "placeholder": `isCrawled = 0`, `html` and `contentHash` NULL.
- **`frequency DEFAULT 1`**: a new row already counts its first referrer. (This was initially misread as default 0 in this chat; the code is built around default 1.) Side effect: a row created with no referrer (a seed page saved by `savePage`, or a link *source* created in the link loop) also starts at 1, so it is a consistent overcount of 1 versus strictly "number of referrers". The owner accepted this.
- `html` and `contentHash` are BLOBs. Always bind them with `bind(i, ptr, size)`. Binding a `std::string` directly stores TEXT, which never equals a BLOB in comparisons.
- `idx_frontier_contenthash` is UNIQUE. SQLite allows many NULLs in a unique index, so uncrawled/stub rows (NULL hash) coexist. It also makes the duplicate-content lookup fast. Creating it on an existing DB that already has duplicate hashes fails; clean up or start from a fresh file.
- `page_links` has one row per distinct (source, target) pair. `frontier` has one row per URL. If three pages link to X, X has `frequency` about 3 and there are three `page_links` rows.
- `ON DELETE CASCADE` only works with `PRAGMA foreign_keys = ON` (set in the constructor).
- `idx_frontier_queue` is a partial index meant for the "next URLs to crawl" query (`WHERE isCrawled = 0 ORDER BY frequency DESC`).

---

## 3. Header (`database.hpp`)

```cpp
#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <string>
#include <vector>

enum class db_mode
{
    read_write,   // crawler, indexer
    read_only     // query engine
};

struct html_url 
{
    std::string url;
    std::string html;
    std::string contentHash;   // raw hash bytes (e.g. SHA-256), not hex text
};

struct page_links 
{
    std::string url;                  // source page
    std::vector<std::string> links;   // urls it links to
};

class database 
{
private:
    SQLite::Database _db;

public:
    explicit database(
            const std::string& dbPath = "db/search_engine.db",
            const std::string& schemaPath = "db/schema.sql",
            db_mode mode = db_mode::read_write
        );

    // CRAWLER OPERATIONS 
    bool saveCrawledPages( 
            const std::vector<html_url>& html_urls,
            const std::vector<page_links>& extracted_links 
            );

    bool getUrlsToCrawl(
            int limit,
            std::vector<std::string>& urls 
            );   // DECLARED BUT NOT YET DEFINED (see open issues)
};
```

History: there were originally two constructors (read-write with schema, and a read-only one with only a path). Both were callable as `database("x.db")` which is ambiguous, so they were merged into one constructor with a `db_mode` enum (the owner's idea of a "defined int" selecting read-write vs read-only; an `enum class` was chosen over a raw int so invalid values can't compile).

---

## 4. Constructor (`database.cpp`)

```cpp
database::database(const std::string& dbPath, const std::string& schemaPath, db_mode mode)
    : _db(dbPath,
          mode == db_mode::read_only
              ? SQLite::OPEN_READONLY
              : (SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE))
{
    _db.exec("PRAGMA busy_timeout = 30000;");

    // read-only connections stop here: no schema, no WAL switch, no writes
    if (mode == db_mode::read_only)
        return;

    _db.exec("PRAGMA foreign_keys = ON;");
    _db.exec("PRAGMA journal_mode = WAL;");
    _db.exec("PRAGMA synchronous = NORMAL;");

    std::ifstream schemaFile(schemaPath);

    if (!schemaFile.is_open())
    {
        throw std::runtime_error("Failed to open database schema: " + schemaPath);
    }

    std::stringstream buffer;
    buffer << schemaFile.rdbuf();

    if (schemaFile.bad())
    {
        throw std::runtime_error("Failed to read database schema: " + schemaPath);
    }

    SQLite::Transaction transaction(_db);
    _db.exec(buffer.str());
    transaction.commit();
}
```

Why each setting:

- **`busy_timeout = 30000`** (30 s): the crawler and indexer are both writers and take turns; the second waits for the lock instead of failing immediately. Set first so even the WAL switch waits.
- **`foreign_keys = ON`**: per-connection; activates the CASCADE rules.
- **`journal_mode = WAL`**: readers (query engine) don't block the writer and vice versa. Persists in the file. Needs a local disk (not a network share).
- **`synchronous = NORMAL`**: safe with WAL (no corruption); on power loss the last few commits may be lost, an app crash loses nothing. Chosen because the crawler can refetch.
- **Read-only mode** skips schema and writes. The DB file must already exist (start crawler/indexer first), otherwise the constructor throws, which is intended.
- The schema is idempotent (`IF NOT EXISTS`) so it runs on every read-write start. It will NOT alter an existing table (new columns/indexes on old DB files need `ALTER TABLE` / separate `CREATE INDEX`).

Usage:

```cpp
database db;                                                                    // read-write, default paths
database db("db/search_engine.db", "db/schema.sql", db_mode::read_only);        // query engine
```

---

## 5. `saveCrawledPages`: the function

Includes needed at the top of `database.cpp`: `<fstream> <sstream> <stdexcept> <climits> <cstdint> <iostream> <string> <unordered_map> <unordered_set> <vector> <algorithm>`, plus `SQLiteCpp/Transaction.h` (already via the umbrella header).

```cpp
bool database::saveCrawledPages(
        const std::vector<html_url>& html_urls,
        const std::vector<page_links>& extracted_links
        )
{
    try 
    {
        // One transaction for the entire batch (IMMEDIATE = take the write lock up front)
        SQLite::Transaction transaction(_db, SQLite::TransactionBehavior::IMMEDIATE);

        /* check if hash already exists; if so drop row 
         *   if no match exists insert the url
         * check if url exists; if so update that url hash and content
         * if insert or update succeeds, the row comes back (RETURNING id)
         */
        SQLite::Statement savePage(
            _db,
            "INSERT INTO frontier (url, html, contentHash, isCrawled) "
            "SELECT ?1, ?2, ?3, 1 "
            "WHERE NOT EXISTS ("
            "    SELECT 1 FROM frontier "
            "    WHERE contentHash = ?3 AND url != ?1"
            ") "
            "ON CONFLICT(url) DO UPDATE SET "
            "html = excluded.html, "
            "contentHash = excluded.contentHash, "
            "isCrawled = 1, "
            "isIndexed = 0 "
            "RETURNING id;"
        );

        // mark a url whose content equals another url's as crawled so it isn't crawled over and over
        SQLite::Statement markCrawled(
            _db,
            "UPDATE frontier SET isCrawled = 1 WHERE url = ?;"
        );

        // insert a discovered (not yet crawled) url; a row comes back ONLY if it was newly inserted
        SQLite::Statement insertUrl(
            _db,
            "INSERT INTO frontier (url) VALUES (?) "
            "ON CONFLICT(url) DO NOTHING "
            "RETURNING id;"
        );

        // fetch the id of a url that already existed
        SQLite::Statement getId(
            _db,
            "SELECT id FROM frontier WHERE url = ?;"
        );

        // record one source -> target edge; DO NOTHING for duplicates
        // exec() returns 1 for a new edge and 0 for an existing one
        SQLite::Statement insertLink(
            _db,
            "INSERT INTO page_links (source_id, target_id) "
            "VALUES (?, ?) "
            "ON CONFLICT(source_id, target_id) DO NOTHING;"
        );

        // add a delta to a url's frequency (relative, not overwrite)
        SQLite::Statement applyFrequency(
            _db,
            "UPDATE frontier "
            "SET frequency = frequency + ? "
            "WHERE id = ?;"
        );

        ////////////////////////////// URL TO ID //////////////////////////////
        // estimate how many urls we will see so the map doesn't rehash as it grows
        std::size_t totalLinks = 0;
        for (const auto& page : extracted_links)
            totalLinks += page.links.size() + 1;        // +1 for the source url itself

        // page_links stores ids, but the crawler gives us url strings;
        // this block translates url -> id and creates the frontier row if the url is new
        std::unordered_map<std::string, std::int64_t> urlCache;
        urlCache.reserve(html_urls.size() + totalLinks);

        // returns the id for a url, creating the row if needed.
        // 'inserted' is true only if THIS call created the row.
        auto getorcreateid = [&](const std::string& url, bool& inserted) -> std::int64_t
        {
            auto it = urlCache.find(url);
            if (it != urlCache.end())
            {
                inserted = false;                       // cache hit: no database work
                return it->second;
            }
            std::int64_t id {0};

            insertUrl.bind(1, url);
            if (insertUrl.executeStep())
            {
                inserted = true;                        // a row came back: newly created
                id = insertUrl.getColumn(0).getInt64();
                insertUrl.reset();
            }
            else
            {
                inserted = false;                       // no row back: it already existed
                insertUrl.reset();
                getId.bind(1, url);
                if (!getId.executeStep())
                {
                    getId.reset();
                    throw std::runtime_error("Failed to retrieve URL ID: " + url);
                }
                id = getId.getColumn(0).getInt64();
                getId.reset();
            }
            urlCache.emplace(url, id);
            return id;
        };

        ////////////////////////////// SAVE PAGES //////////////////////////////
        std::unordered_set<std::string> skippedUrls;
        for (const auto& page : html_urls)
        {
            // bad entry: skip it instead of failing the whole batch
            if (page.url.empty() ||
                page.contentHash.empty() ||
                page.html.size() > static_cast<std::size_t>(INT_MAX))
            {
                std::cerr << "Skipping invalid page: "
                          << (page.url.empty() ? "<empty url>" : page.url) << '\n';

                if (!page.url.empty())
                    skippedUrls.insert(page.url);       // its links get skipped too

                continue;
            }

            savePage.bind(1, page.url);
            savePage.bind(2, page.html.data(), static_cast<int>(page.html.size()));
            savePage.bind(3, page.contentHash.data(), static_cast<int>(page.contentHash.size()));

            if (savePage.executeStep())
            {
                // a row came back: the page was inserted or updated.
                // remember its id so the link step needs no lookup for it
                urlCache[page.url] = savePage.getColumn(0).getInt64();
                savePage.reset();
            }
            else 
            {
                // no row back: this content already exists under another url.
                // don't store the content, just mark the url crawled
                savePage.reset();

                markCrawled.bind(1, page.url);
                markCrawled.exec();
                markCrawled.reset();

                skippedUrls.insert(page.url);
            }
        }

        ////////////////////////////// SAVE LINKS //////////////////////////////
        // put every linked url into frontier so the crawler crawls it later,
        // record each source->target pair in page_links,
        // and count how many NEW references each existing url gained (applied later)
        std::unordered_map<std::int64_t, std::int64_t> frequencyDelta;
        for (const auto& page : extracted_links)
        {
            // bad entry: no source url
            if (page.url.empty())
            {
                std::cerr << "Skipping link entry with empty source URL\n";
                continue;
            }
            // invalid or duplicate-content page: don't save its links
            if (skippedUrls.count(page.url))
                continue;

            // validate ALL links before touching the database,
            // so a bad entry saves nothing at all
            const bool hasEmptyLink = std::any_of(
                page.links.begin(), page.links.end(),
                [](const std::string& l) { return l.empty(); });

            if (hasEmptyLink)
            {
                std::cerr << "Skipping link entry with an empty link: " << page.url << '\n';
                continue;
            }
            bool sourceInserted = false;
            const std::int64_t sourceId = getorcreateid(page.url, sourceInserted);

            for (const auto& link : page.links)
            {
                // a page referencing itself isn't "referenced by others"
                if (link == page.url)
                    continue;

                bool targetInserted = false;
                const std::int64_t targetId = getorcreateid(link, targetInserted);

                insertLink.bind(1, sourceId);
                insertLink.bind(2, targetId);
                const int changed = insertLink.exec();      // 1 = new edge, 0 = already there
                insertLink.reset();

                // a brand-new url already starts at frequency 1 (its first referrer),
                // so only new edges to EXISTING urls add to the count
                if (changed > 0 && !targetInserted)
                    ++frequencyDelta[targetId];
            }
        }

        ////////////////////////////// APPLY FREQUENCY //////////////////////////////
        // add each target's accumulated count to its row: one UPDATE per distinct target
        for (const auto& [id, delta] : frequencyDelta)
        {
            applyFrequency.bind(1, delta);
            applyFrequency.bind(2, id);
            applyFrequency.exec();
            applyFrequency.reset();
        }
        transaction.commit();
        return true;
    }
    catch (const SQLite::Exception& e)
    {
        std::cerr << "saveCrawledPages SQLite error " << e.getErrorCode()
                  << " (extended " << e.getExtendedErrorCode() << "): "
                  << e.what() << '\n';
        return false;
    }
    catch (const std::exception& e)
    {
        std::cerr << "saveCrawledPages failed: " << e.what() << std::endl;
        return false;
    }
}
```

### Part-by-part: what each part does and what it solves

1. **Transaction (IMMEDIATE).** One transaction for the whole batch: SQLite syncs once at commit instead of once per INSERT (the biggest bulk-write cost), and the batch is all-or-nothing. If anything throws, the destructor rolls back (statements are declared after the transaction, so they are finalized first). IMMEDIATE takes the write lock up front so a second writer waits properly under `busy_timeout`.
2. **Prepared statements.** Compiled once, then bind/run/`reset()` per row. Faster, and values are bound (no SQL injection or escaping issues). Every statement must be `reset()` after use or the next bind misbehaves.
3. **`savePage`.** `INSERT ... SELECT ?1,?2,?3,1 WHERE NOT EXISTS(other url with same hash)` + `ON CONFLICT(url) DO UPDATE` + `RETURNING id`. If another URL already has this hash, the SELECT yields no row, so nothing is inserted and nothing is returned. `url != ?1` stops a re-crawl of the same URL from being a duplicate of itself. On URL conflict it replaces html/hash, sets `isCrawled = 1` and `isIndexed = 0` (changed content must be re-indexed). `id` and `frequency` are untouched. The `WHERE` on the SELECT is required so SQLite parses the `ON CONFLICT` as belonging to the INSERT. `?1` and `?3` are numbered params, bound once and reused.
4. **`markCrawled`.** Plain UPDATE (the owner deliberately chose this): a duplicate-content URL that already has a row gets `isCrawled = 1`; one with no row stores nothing.
5. **`insertUrl` + `getId` + `getorcreateid` + `urlCache`.** `INSERT ... DO NOTHING RETURNING id` returns a row only when the insert really happened, which tells us whether the URL was NEW (needed because new rows already start at frequency 1). If no row comes back the URL existed and `getId` fetches the id. The cache means each URL hits the DB at most once per batch (ids don't change inside a transaction). Saved pages seed the cache so the link step needs no lookup for them.
6. **Save pages loop.** Validation skips bad pages (empty URL, empty hash, html > INT_MAX because SQLite's bind length is an `int`) and records the URL in `skippedUrls`. `executeStep()` true = inserted/updated (cache the id); false = duplicate content (mark crawled, add to `skippedUrls`).
7. **Save links loop.** Skips entries with empty source, sources in `skippedUrls`, and entries with ANY empty link (checked first with `std::any_of`, so a bad entry saves nothing). Self-links are skipped. `insertLink.exec()` returns the number of rows that statement changed (1 new edge / 0 duplicate), so repeated links and re-crawls never inflate frequency. Only new edges to URLs that already existed add +1 (a URL created in this batch starts at 1 already).
8. **Apply frequency.** Deltas accumulated in a map, applied with one UPDATE per distinct target (a URL linked 500 times costs 1 UPDATE, not 500).
9. **Commit / catch.** Commit makes everything permanent. `catch` logs SQLite error codes (`SQLite::Exception` is caught before `std::exception`) and returns false; the rollback has already happened.

---

## 6. Decisions made (so a new chat doesn't reopen them)

- **Duplicate content that already has a row:** keep it as a stub (`isCrawled = 1`, html/hash untouched/NULL). NOT deleted: deleting would make the crawler refetch it forever (links recreate the placeholder) and CASCADE would destroy its edges and frequency. A `canonicalId` alias column was suggested and declined.
- **Duplicate content with no row yet:** nothing stored. If some page later links to it, a placeholder is created, fetched once more, detected as duplicate, then marked crawled. Settles after one extra fetch.
- **Links from skipped pages (invalid or duplicate content) are not saved.**
- **`frequency`** = number of distinct pages referencing the URL. Self-links excluded.
- **Bad entries are skipped and logged, not thrown.** Only SQLite errors (and the impossible "id not found") abort the batch.
- **`markCrawled` stays a plain UPDATE** (not an upsert).
- **Single function, no helper classes** (an earlier class-based breakdown was rejected).
- **`isIndexed = 0` on every re-crawl update**, even if the content is unchanged. Accepted for now.
- **The `isIndexed` race** (indexer reads page, crawler re-crawls and resets the flag, indexer marks the OLD content indexed) is known; the owner decided to defer it since re-crawls are rare. Fix when the indexer exists: `markIndexed(id, hash)` = `UPDATE frontier SET isIndexed = 1 WHERE id = ? AND contentHash = ?` (bind the hash as a BLOB).

---

## 7. What makes `saveCrawledPages` return false

It returns false only when something throws; invalid entries, duplicate content, existing URLs/edges never do. Everything is rolled back; the reason goes to `std::cerr`.

| Cause | Code | Transient? |
|---|---|---|
| write lock held longer than busy_timeout | BUSY / LOCKED | yes, retry with backoff |
| disk full | FULL | until space freed |
| I/O error, corruption | IOERR / CORRUPT | no |
| write through a read-only connection / file | READONLY | no |
| SQL error (missing tables, SQLite < 3.35 rejecting RETURNING) | ERROR | no, setup problem |
| constraint violation (CHECK/NOT NULL/FK) | CONSTRAINT | no, a bug |
| out of memory | `std::bad_alloc` | rare |

Retrying a failed or even a committed batch is safe: pages are upserts, existing edges are ignored, frequency only grows on new edges (side effect: re-saved pages get `isIndexed = 0` again). Suggested caller pattern: keep the batch, retry up to 3 times with 1 s / 2 s / 4 s sleeps, and if still failing stop and report instead of discarding downloaded pages. Possible future improvement: return an enum (`Saved`, `Retry`, `Fatal`) instead of bool.

---

## 8. Open issues and pending changes (not yet applied to the code)

1. **Failed fetches / empty html (real bug).** The validation does not check `page.html.empty()`. If a failed crawl is passed in with a hash, the FIRST such URL is stored as crawled with an empty blob, and every later failure has the same hash, so it is treated as duplicate content and marked crawled permanently. Failed URLs are never retried. Fix: (a) the crawler must not create an `html_url` for a failed fetch (check status/body BEFORE compressing; the gzip of an empty body is not zero bytes, so the function can't catch that), and (b) add `page.html.empty() ||` to the validation condition so such a page is skipped and stays `isCrawled = 0`. To avoid retrying dead URLs forever, add `failCount INTEGER NOT NULL DEFAULT 0` to `frontier`, a `markFailed(url)` function (`UPDATE frontier SET failCount = failCount + 1 WHERE url = ?`), and `AND failCount < 3` in the crawl-queue query.
2. **Mark duplicate-content URLs as indexed too (recommended).** Change `markCrawled` to `"UPDATE frontier SET isCrawled = 1, isIndexed = 1 WHERE url = ?;"` so the indexer never sees stubs. The indexer query should still include `html IS NOT NULL` as a safety net.
3. **`getUrlsToCrawl` is declared but not defined.** Calling it is a link error. Intended query: `SELECT url FROM frontier WHERE isCrawled = 0 ORDER BY frequency DESC LIMIT ?` (the partial index `idx_frontier_queue` backs it).
4. **Cosmetic:** the comment block at the top of `saveCrawledPages` once said "return the ID" (the function returns bool); duplicate `#include <stdexcept>`; the unused `sourceInserted` variable is required by the lambda signature.
5. **Statements are prepared on every call.** Negligible for big batches; if batches become small and frequent, move them into members prepared once.
6. **Caller can't tell entries were skipped** (only the log shows it). Optionally return a skipped count.
7. **The crawler side is not written yet:** computing the hash into `html_url::contentHash` (use SHA-256 over a consistent representation; NOT `std::hash`, collisions would silently drop real pages), retry handling for a false return, and `getUrlsToCrawl`.
8. **Read-only connections on WAL** need access to the `-shm`/`-wal` files; keep read transactions short so the WAL can checkpoint.

---

## 9. Corrections made during the chat (history)

- Early on it was claimed `_db.getChanges()` could report stale counts after a no-op upsert and `exec()`'s return value is safer. SQLiteCpp's `exec()` returns `sqlite3_changes()` itself, so they are effectively the same call; the original logic there was fine. The real gains of the rewrite are batching, `RETURNING`, the frequency accumulation and the ID cache.
- It was assumed `frequency` defaulted to 0, which made "bug #2" in the first review wrong. The schema default is 1, so the original (insert-first, increment only for already-existing targets) logic was consistent, and the final code keeps it.
- An `INSERT ... DO UPDATE SET url = url RETURNING id` single-statement lookup was proposed first, then replaced by `DO NOTHING RETURNING id` + `getId` because the no-op update can't tell whether the row was new.
- A bug in the owner's intermediate code (two nested `SQLite::Transaction`s, one outside the try) was found and fixed; there is now exactly one, first line inside the `try`.

---

## 10. Test plan (owner will write the tests later)

Behaviors that are deliberate and should be asserted, not "fixed":

- New page: inserted, `isCrawled = 1`, html and hash stored.
- Same URL re-saved with same content: updated (not treated as duplicate of itself), `isIndexed` reset to 0, `id` and `frequency` unchanged.
- Same URL re-saved with new content: html and hash replaced.
- Different URL, same hash: content NOT stored; if the URL already had a row it is `isCrawled = 1` with html/hash untouched; if it had no row, no row is created; its links are not saved.
- Two pages with the same hash in ONE batch: first stored, second skipped.
- Placeholder URL (from an earlier link) later downloaded: row filled in, same `id`, frequency kept.
- Links: new target creates a frontier row (`isCrawled = 0`, frequency 1); a source linking to it twice or re-crawled adds nothing; a second distinct source adds +1; self-link ignored; `page_links` has one row per pair.
- Frequency: A, B, C link to X -> X frequency 3, 3 `page_links` rows, 1 frontier row.
- Bad entries: empty page url / empty hash / oversized html skipped, others saved, function returns true; link entry with one empty link saves none of its links; empty source url skipped.
- Atomicity: force a SQLite error mid-batch (e.g. drop a table or use a read-only connection) -> returns false, nothing from the batch persisted.
- Read-only `database` object: constructing on a missing file throws; `saveCrawledPages` returns false.
- Return value stays true when entries were skipped.

---

## 11. Working style the owner prefers

- Build things **bit by bit**, explaining what each part does and how it works.
- When a change is suggested, say **exactly where** it goes and **what to write**.
- Keep it as **one function** with comments separating the parts, explaining what each solves.
- Wants honest review of the code ("is anything wrong?") and corrections when earlier advice was wrong.
- Tests will be written later, after the design is settled.
