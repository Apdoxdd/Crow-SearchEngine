# Crow-SearchEngine: crawler database functions

Covers `saveCrawledPages()` and `getUrlsToCrawl()`, both implemented, and how the
crawler manager and the indexer share the database. **64 of 64 tests passing**
(43 for `saveCrawledPages`, 17 for `getUrlsToCrawl`, 4 for concurrency).

---

## 1. Schema facts the code relies on

| Rule | Where | Effect |
|---|---|---|
| `url` is `NOT NULL UNIQUE` | `frontier` | one row per URL; the upsert key |
| `id INTEGER PRIMARY KEY` | `frontier` | `id` is the rowid, so it is the free last column of every index (used as the queue tiebreak) |
| `frequency INTEGER NOT NULL DEFAULT 1 CHECK (frequency >= 1)` | `frontier` | every URL starts at 1 and can only grow |
| `isCrawled`, `isIndexed` are `CHECK IN (0,1)` | `frontier` | boolean flags |
| `html`, `contentHash` are `BLOB`, nullable | `frontier` | placeholders have both `NULL` |
| `UNIQUE INDEX idx_frontier_contenthash (contentHash)` | `frontier` | one URL per distinct content; many `NULL`s allowed |
| `PRIMARY KEY (source_id, target_id)`, both `ON DELETE CASCADE` | `page_links` | an edge exists at most once; deleting either page removes it |
| `INDEX idx_frontier_queue ON frontier(frequency DESC) WHERE isCrawled = 0` | `frontier` | the crawl queue, used by `getUrlsToCrawl()` |

### Life of a `frontier` row

```
discovered via a link        placeholder    isCrawled=0  isIndexed=0  html=NULL  hash=NULL  frequency>=1
        |  page fetched, new content
        v
crawled                      isCrawled=1  isIndexed=0  html, hash set
        |  indexer finishes
        v
indexed                      isCrawled=1  isIndexed=1
        |  re-crawled
        v
back to crawled              isIndexed reset to 0

fetched, but content equals another URL's content:
duplicate                    isCrawled=1  isIndexed=1  (html/hash stay as they were; NULL for a placeholder)
```

Only rows with `isCrawled = 0` are in the crawl queue, so a row leaves the queue when it
is crawled or found to be a duplicate. A fetch that fails is not recorded anywhere (see section 7).

---

## 2. `saveCrawledPages()` exactly

```cpp
bool saveCrawledPages(const std::vector<html_url>& html_urls,
                      const std::vector<page_links>& extracted_links);
```

- `html_urls`: pages the crawler fetched (`url`, `html`, `contentHash`).
- `extracted_links`: for each fetched page, the URLs it links to (`url`, `links`).
- **Returns `true`** if the batch was committed, **even if some entries were skipped as invalid**.
- **Returns `false`** only on a real error (SQLite or other exception). In that case nothing from the batch is stored.

### What it does, in order

**0. One transaction.** `BEGIN IMMEDIATE` for the whole batch. The write lock is taken up front, waiting up to 30 s (`busy_timeout` set in the constructor). The six SQL statements are prepared once and reused for every entry. A URL-to-id cache is reserved up front (`pages + links + sources`) so it never rehashes.

**1. Save pages**, for each entry of `html_urls`, in order:

1. **Validate.** The page is skipped, logged to `stderr`, and its URL added to `skippedUrls` if it has an empty `url` (not added, no URL to record), empty `html`, empty `contentHash`, or `html` larger than `INT_MAX` bytes. One bad page never fails the batch.
2. **Upsert.**
   ```sql
   INSERT INTO frontier (url, html, contentHash, isCrawled)
   SELECT ?1, ?2, ?3, 1
   WHERE NOT EXISTS (SELECT 1 FROM frontier WHERE contentHash = ?3 AND url != ?1)
   ON CONFLICT(url) DO UPDATE SET html = excluded.html,
        contentHash = excluded.contentHash, isCrawled = 1, isIndexed = 0
   RETURNING id;
   ```
   - **A row comes back:** the page was inserted (new URL), or updated (URL already existed, including a placeholder). The id is cached. The existing `frequency` is never touched.
   - **No row comes back:** the same content already exists under a *different* URL. The content is **not stored**. Instead `UPDATE frontier SET isCrawled = 1, isIndexed = 1 WHERE url = ?` runs (it only affects a row that already exists, e.g. a placeholder), and the URL goes into `skippedUrls`.

**2. Save links**, for each entry of `extracted_links`:

1. Skip the entry (logged) if the source `url` is empty.
2. Skip the entry silently if the source is in `skippedUrls` (invalid page or duplicate content: its links are dropped).
3. Skip the **whole entry** (logged) if **any** link in it is empty. Nothing from that entry is written.
4. Resolve the source id (cache, else insert-or-lookup; an unknown source becomes a placeholder).
5. For each link:
   - skip if it equals the source (self-link);
   - resolve the target id; an unknown target is created as a placeholder with `frequency = 1`;
   - `INSERT ... ON CONFLICT DO NOTHING` into `page_links`; `changes() == 1` means a new edge;
   - **frequency rule:** if the edge is new **and** the target already existed before this lookup (database row, or created/cached earlier in this batch), add 1 to that target's pending delta. A brand-new target already starts at 1 for its first referrer, so it gets no extra.

**3. Apply frequency.** One `UPDATE frontier SET frequency = frequency + delta WHERE id = ?` per distinct target (deltas are accumulated in memory, so 1,000 referrers cost one UPDATE, not 1,000).

**4. Commit and return `true`.**

If any exception is thrown, the transaction object is destroyed without commit, which **rolls the whole batch back**; the error is logged and the function returns `false`.

### Frequency meaning

| URL | frequency |
|---|---|
| created by a link | number of distinct referrers |
| crawled directly (e.g. a seed) | 1 + number of distinct referrers |

Self-links and repeated links (same source, same target, in any call) never count twice.

### Two behaviors worth a conscious decision

Both are covered by tests that pin the **current** behavior, so changing them later is a deliberate test edit:

1. **Re-crawl with identical content resets `isIndexed` to 0**, so unchanged pages get re-indexed every time they are re-crawled.
2. **An existing page re-crawled with another page's content** keeps its old html and hash and is set to `isIndexed = 1`. If its old content was never indexed, it never will be.

### Performance observed (your machine, release build)

| Test | Time |
|---|---|
| 50,000 distinct pages, one batch | about 0.3 to 0.9 s |
| 50,000 links from one page, one batch | about 0.3 to 1.4 s |

---

## 3. What the `saveCrawledPages()` tests prove (43 of 43 passing)

| Area | Tests | What is guaranteed |
|---|---|---|
| Setup | Database initialization, Foreign key enforcement, Cascade deletion (source and target) | tables exist; foreign keys enforced; deleting either end of an edge removes it |
| Storing pages | Save a new page, Multiple pages in one batch, Update existing URL, Same URL and same hash, Same URL twice in one batch, Empty batch | insert, update, id and frequency preserved, last write wins, empty batch is a no-op |
| Validation | Empty URL, Empty HTML, Empty content hash, Valid and invalid pages in one batch | bad pages skipped, good ones in the same batch still saved, returns `true` |
| Content dedup | Duplicate content hash, Duplicate hashes within one batch (first page wins), Hash released by an update can be reused, Duplicate content on a placeholder | one URL per content; placeholder with duplicate content becomes crawled and indexed with NULL html |
| Links | Create placeholder URLs, Insert outgoing links, Duplicate relationship, Self-referencing link, Empty source URL in links, Empty target URL in links | placeholders created, edges unique, self-links ignored, bad link entries skipped whole |
| Skipped pages | Links of an invalid page are dropped, Links of a duplicate-content page are dropped | skipped pages leak no URLs or edges |
| Frequency | Increment, no increment on duplicate edge, same-batch link, two sources to one new target, repeated target in one list, same entry twice, many sources to one target, accumulation across batches | every counting rule above |
| Lifecycle | Placeholder becomes a crawled page | keeps id, edges, frequency; gains content |
| Indexing flag | Changed content clears `isIndexed`, identical content (pinned) | re-index behavior |
| Data integrity | Binary html and hash round trip | NUL and non-UTF-8 bytes survive |
| Atomicity | Failed batch rolls back every change, Failed frequency update rolls back the batch | real injected SQLite errors undo the whole batch, return `false`, and leave the connection usable |
| Performance | 50,000 pages, 50,000 relationships | scale smoke tests (no time threshold yet) |

---

## 4. `getUrlsToCrawl()` exactly

```cpp
bool getUrlsToCrawl(int limit, std::vector<std::string>& urls);
```

- `limit`: the most URLs to return.
- `urls`: output. Replaced on every call, never appended to.
- **Returns `true`** if the query worked, **even if the queue is empty**.
- **Returns `false`** only on a real error (SQLite or other exception). In that case `urls` is left empty, never a partial list.
- Returns every URL with `isCrawled = 0`, up to `limit`: **most referenced first (highest `frequency`), ties in discovery order (lowest `id`)**.
- **Only reads.** It never changes the database, so it also works on a `db_mode::read_only` connection.

### What it does, in order

**0. No transaction.** One `SELECT` is already a consistent snapshot, so there is nothing to protect, and a transaction would only take a write lock the function doesn't need.

**1. Reset the output.** `urls.clear()` runs first so old contents never leak into the result, even on early return or error.

**2. Guard `limit`.** `limit <= 0` returns `true` with an empty result, without touching the database.

**3. Query.**
```sql
SELECT url FROM frontier
WHERE isCrawled = 0
ORDER BY frequency DESC, id
LIMIT ?;
```
- `isCrawled = 0` matches the partial index `idx_frontier_queue`, and `ORDER BY frequency DESC` follows that index's order, so SQLite reads only the first `limit` index entries: no table scan and no sort.
- `id` is the rowid, the hidden last column of every index, so the tiebreak costs nothing. Among equal frequencies the earliest discovered URL wins.
- Query plan on your build: `SCAN frontier USING INDEX idx_frontier_queue`, no temp b-tree. A test guards this.

**4. Read rows.** The vector reserves `min(limit, 4096)` slots, so it doesn't regrow for normal limits, and a huge `limit` (e.g. `INT_MAX`) can't allocate a huge block. Each row's `url` is appended in the order it arrives, which is already the final order.

**5. Return `true`.**

If any exception is thrown, `urls` is cleared, the error is logged to `stderr` (SQLite errors include the error code and extended code), and the function returns `false`.

### The two catch blocks

`SQLite::Exception` derives from `std::runtime_error`, which derives from `std::exception`, so the specific catch must come first.

| Catch | Handles | Gives you |
|---|---|---|
| `SQLite::Exception` | database errors (busy, corrupt, no such table) | `getErrorCode()` and `getExtendedErrorCode()` in the log |
| `std::exception` | everything else, mainly `std::bad_alloc` from `reserve` or `emplace_back` | `what()` only |

### Code (`database.cpp`)

```cpp
bool database::getUrlsToCrawl(
		int limit,
		std::vector<std::string>& urls
		)
{
	/* returns up to 'limit' urls that were discovered but not crawled yet
	 * 	most referenced urls come first (highest frequency)
	 * 	urls with the same frequency come in the order they were discovered
	 * returns true if the query worked (even if the queue is empty)
	 * returns false only if a db error happened, urls is empty in that case
	 * only reads, never changes the db, so it also works on a read_only connection
	 */

	// start from a clean output so old contents never leak into the result
	urls.clear();

	// nothing was asked for: a successful, empty answer
	if (limit <= 0)
		return true;

	try
	{
		// no transaction: one SELECT is already a consistent snapshot, and a
		// transaction would only take the write lock when i don't need it

		////////////////////////////////// QUERY ////////////////////////////////////
		// "isCrawled = 0" matches the partial index idx_frontier_queue, and
		// ORDER BY frequency DESC follows that index's order, so sqlite reads only
		// the first 'limit' index entries (no table scan, no sorting).
		// id is the rowid, the hidden last column of every index, so the tiebreak
		// costs nothing: among equal frequencies the earliest discovered url wins.
		SQLite::Statement query(
				_db,
				"SELECT url FROM frontier "
				"WHERE isCrawled = 0 "
				"ORDER BY frequency DESC, id "
				"LIMIT ?;"
				);

		query.bind(1, limit);

		////////////////////////////////// READ ROWS ////////////////////////////////////

		// reserve so the vector doesn't regrow as rows come in, but cap it so a
		// huge 'limit' (e.g. INT_MAX) can't allocate a huge block up front
		urls.reserve(std::min<std::size_t>(static_cast<std::size_t>(limit), 4096));

		// each row is one url, rows already arrive in the order we want
		while (query.executeStep())
			urls.emplace_back(query.getColumn(0).getString());

		return true;
	}
	catch (const SQLite::Exception& e)
	{
		// partial results must not be handed out as if they were complete
		urls.clear();

		std::cerr << "getUrlsToCrawl SQLite error " << e.getErrorCode()
		          << " (extended " << e.getExtendedErrorCode() << "): "
		          << e.what() << '\n';
		return false;
	}
	catch (const std::exception& e)
	{
		urls.clear();

		std::cerr << "getUrlsToCrawl failed: " << e.what() << std::endl;
		return false;
	}
}
```

### Performance observed (your machine, release build)

| Test | Time |
|---|---|
| top 100 of 20,000 placeholders | 0 ms (under 1 ms; the timer truncates to whole milliseconds) |

---

## 5. What the `getUrlsToCrawl()` tests prove (17 of 17 passing)

| Area | Tests | What is guaranteed |
|---|---|---|
| Order | Highest frequency first, Ties are broken by discovery order, Order follows frequency changes | most referenced first; equal frequency goes to the earliest discovered; a URL that gains referrers moves up on the next call |
| `limit` | Respects limit, Limit of one, Limit larger than the queue (including `INT_MAX`) | only `limit` URLs, and they are the best ones; a big limit returns the whole queue and does not over-allocate |
| What leaves the queue | Skips crawled pages, Skips duplicate-content placeholders, Crawled-only database returns nothing | only `isCrawled = 0` rows are returned; a crawled or duplicate-content URL is never fetched again |
| Empty and bad arguments | Empty queue, Zero and negative limit, Clears previous contents of the output vector | `true` with an empty result; stale output never leaks |
| Read-only | Is read-only and repeatable, Works on a read-only connection | repeated calls give the same answer; the query engine's connection can read the queue |
| Errors | Database error returns false and empty output | a real SQLite error (`no such table`) returns `false` and leaves `urls` empty |
| The optimization | Query plan uses `idx_frontier_queue`, Top 100 of 20000 | partial index used with no temp sort; correct order at scale in under a second |

The query-plan test contains its own copy of the SQL. If you change the query in `getUrlsToCrawl()`, change `getQueuePlan()` in the test file too.

---

## 6. Crawler manager and indexer sharing the database

### How access is arranged

- **WAL mode:** readers and the single writer don't block each other.
- **One writer at a time:** `saveCrawledPages` takes the write lock up front with `BEGIN IMMEDIATE` and waits up to 30 s (`busy_timeout`) instead of failing.
- **One connection per thread or process:** each `database` object is its own connection. The crawler manager and the indexer each use their own, and a connection is never shared across threads.

### What the 4 concurrency tests prove (4 of 4 passing)

| Test | Result on your machine | What is guaranteed |
|---|---|---|
| Crawler batches and indexer writes at the same time | `crawlerFailures=0 indexerFailures=0` | 40 batches of 50 pages (2,000 pages) saved while the indexer wrote on its own connection: no batch failed or was lost, row, edge and frequency counts exact, every crawled page ended up indexed |
| `saveCrawledPages` waits for a held write lock | waited 329 ms for a 300 ms lock | the crawler waits for the lock and then succeeds, not an immediate failure |
| Deferred transaction fails after a concurrent commit | `SQLITE_BUSY_SNAPSHOT` (517) | a plain `BEGIN` that reads and then writes after another connection committed fails at once, and `busy_timeout` cannot help |
| Indexer must guard its update with the content hash it read | guarded update changes 0 rows | if the crawler re-saved the page while the indexer worked, a guarded update leaves it queued; an unguarded one marks stale content as indexed |

The indexer is simulated in these tests (`indexerPass`), so they prove the **database layer** is
safe for both. The real indexer has to follow the rules below.

### Rules for the indexer

1. **Start every write transaction with `BEGIN IMMEDIATE`**, never a plain `BEGIN`. Otherwise it can fail with `SQLITE_BUSY_SNAPSHOT`.
2. **Mark a page indexed only with a guarded update:**
   ```sql
   UPDATE frontier SET isIndexed = 1 WHERE url = ?1 AND contentHash = ?2;
   ```
   Bind `?2` as a BLOB (the column is a BLOB; binding text will never match). Zero changed rows means the page was re-crawled meanwhile, so leave it queued.
3. **Keep write transactions short.** Batches of about 100 rows are fine. A long one blocks the crawler's writes for its whole duration.
4. **The crawler manager retries when `saveCrawledPages` returns `false`.** If something ever held the write lock past 30 s, that batch is lost unless the caller retries.

### Flakiness check

A thread test passing once proves less than it seems. From the `build` folder:

```bash
for i in $(seq 20); do ./db_crawler_tests | grep -q "Failed: 0" || echo "run $i failed"; done
```

No output means 20 clean runs.

---

## 7. Known gaps and open decisions

**Design decisions still open**

| Item | Today | Consequence |
|---|---|---|
| Failed fetches | nothing records them | a URL that times out or returns 404 stays at `isCrawled = 0` and comes back on every call, near the top if its frequency is high. Needs an attempts or retry-after column, or a failed state the query filters on |
| Handing out URLs | `getUrlsToCrawl` only reads | two crawler workers calling it at the same time get the same URLs. Fine for one manager; for several, add a lease column and claim rows with `UPDATE ... RETURNING` in an `IMMEDIATE` transaction |
| Re-crawl with identical content | resets `isIndexed` to 0 | unchanged pages are re-indexed on every re-crawl (pinned by a test) |
| Re-crawl with another page's content | keeps old html and hash, sets `isIndexed = 1` | if the old content was never indexed, it never will be (pinned by a test) |

**Not covered by tests**

- The constructor tests: missing schema file, reopening an existing database, read-only mode rejecting writes, read-only open of a missing database.
- Oversized html (`INT_MAX`), which would need the limit to become a constant that tests can lower.
- The write lock held longer than 30 s (it would take 30 s of waiting, or a changed timeout).
- Time limits on the two 50,000-row performance tests; they print timings but can't fail on a slowdown.
- A real indexer: the concurrency tests use a simulated one.
- ALL OF THE ABOVE ARE HANDLES SOMEWHERE ELSE

## 8. Test inventory

| Group | Tests | Passing |
|---|---|---|
| `saveCrawledPages` (sections 2 and 3) | 43 | 43 |
| `getUrlsToCrawl` (section 5) | 17 | 17 |
| Concurrency (section 6) | 4 | 4 |
| **Total** | **64** | **64** |

Housekeeping: `tests/db/temp/` holds generated test databases; keep it in `.gitignore`.
