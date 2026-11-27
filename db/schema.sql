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




