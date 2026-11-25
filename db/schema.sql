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


