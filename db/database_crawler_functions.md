# Crow-SearchEngine: crawler database functions

Covers `saveCrawledPages()` (implemented, 43/43 tests passing) and
`getUrlsToCrawl()` (proposed implementation plus tests, **not yet compiled or run**).

---

## 1. Schema facts the code relies on

| Rule | Where | Effect |
|---|---|---|
| `url` is `NOT NULL UNIQUE` | `frontier` | one row per URL; the upsert key |
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
| 50,000 distinct pages, one batch | about 0.5 to 0.9 s |
| 50,000 links from one page, one batch | about 0.5 to 1.4 s |

---

## 3. What the tests prove (43 of 43 passing)

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

Not covered: concurrent writers, oversized html (`INT_MAX`), the constructor tests (missing schema, reopen, read-only mode), and `getUrlsToCrawl()`.

---

