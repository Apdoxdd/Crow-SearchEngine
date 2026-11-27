#include "database.hpp"
#include <SQLiteCpp/SQLiteCpp.h>

#include <iostream>
#include <string>
#include <functional>
#include <vector>
#include <filesystem>
#include <exception>
#include <chrono>

const std::filesystem::path TEST_DB_PATH {"tests/db/temp/crawler_tests.db"};
const std::filesystem::path SCHEMA_PATH {"db/schema.sql"};

///////////////////////DATABASE HELPERS/////////////////////////////
///
const std::filesystem::path CTOR_DB_PATH {"tests/db/temp/constructor_tests.db"};

struct page_row
{
    bool exists = false;
    int id = -1;
    int frequency = -1;
    int isCrawled = -1;
    int isIndexed = -1;
    bool htmlIsNull = true;
    bool hashIsNull = true;
    std::string html;
    std::string contentHash;
};

// Reads a BLOB column byte-for-byte (keeps embedded NULs).
std::string blobToString(const SQLite::Column& column)
{
    if (column.isNull())
        return {};

    const void* data = column.getBlob();

    if (!data)
        return {};

    return std::string(
        static_cast<const char*>(data),
        static_cast<std::size_t>(column.getBytes())
    );
}

page_row getRow(const std::string& url)
{
    SQLite::Database inspect(TEST_DB_PATH.string(), SQLite::OPEN_READONLY);

    SQLite::Statement query(
        inspect,
        "SELECT id, frequency, isCrawled, isIndexed, html, contentHash "
        "FROM frontier WHERE url = ?;"
    );

    query.bind(1, url);

    page_row row;

    if (!query.executeStep())
        return row;

    row.exists = true;
    row.id = query.getColumn(0).getInt();
    row.frequency = query.getColumn(1).getInt();
    row.isCrawled = query.getColumn(2).getInt();
    row.isIndexed = query.getColumn(3).getInt();
    row.htmlIsNull = query.getColumn(4).isNull();
    row.hashIsNull = query.getColumn(5).isNull();
    row.html = blobToString(query.getColumn(4));
    row.contentHash = blobToString(query.getColumn(5));

    return row;
}

int getTotalLinkCount()
{
    SQLite::Database inspect(TEST_DB_PATH.string(), SQLite::OPEN_READONLY);
    SQLite::Statement query(inspect, "SELECT COUNT(*) FROM page_links;");
    query.executeStep();
    return query.getColumn(0).getInt();
}

int getIncomingLinkCount(const std::string& url)
{
    SQLite::Database inspect(TEST_DB_PATH.string(), SQLite::OPEN_READONLY);

    SQLite::Statement query(
        inspect,
        "SELECT COUNT(*) FROM page_links "
        "WHERE target_id = (SELECT id FROM frontier WHERE url = ?);"
    );

    query.bind(1, url);
    query.executeStep();
    return query.getColumn(0).getInt();
}

bool edgeExists(const std::string& source, const std::string& target)
{
    SQLite::Database inspect(TEST_DB_PATH.string(), SQLite::OPEN_READONLY);

    SQLite::Statement query(
        inspect,
        "SELECT 1 FROM page_links "
        "WHERE source_id = (SELECT id FROM frontier WHERE url = ?1) "
        "AND target_id = (SELECT id FROM frontier WHERE url = ?2);"
    );

    query.bind(1, source);
    query.bind(2, target);
    return query.executeStep();
}

// Same as urlExists() but for any database file.
bool urlExistsAt(const std::filesystem::path& dbPath, const std::string& url)
{
    SQLite::Database inspect(dbPath.string(), SQLite::OPEN_READONLY);
    SQLite::Statement query(inspect, "SELECT 1 FROM frontier WHERE url = ?;");
    query.bind(1, url);
    return query.executeStep();
}

// Simulates the indexer.
void setIndexed(const std::string& url, int value)
{
    SQLite::Database writer(TEST_DB_PATH.string(), SQLite::OPEN_READWRITE);
    SQLite::Statement update(writer, "UPDATE frontier SET isIndexed = ? WHERE url = ?;");
    update.bind(1, value);
    update.bind(2, url);
    update.exec();
}

void deleteFrontierRow(const std::string& url)
{
    SQLite::Database writer(TEST_DB_PATH.string(), SQLite::OPEN_READWRITE);
    writer.exec("PRAGMA foreign_keys = ON;");
    SQLite::Statement remove(writer, "DELETE FROM frontier WHERE url = ?;");
    remove.bind(1, url);
    remove.exec();
}

void removeDbFiles(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

// Installs a trigger for its lifetime, then drops it.
// Used to force a REAL SQLite error in the middle of saveCrawledPages().
class scoped_trigger
{
public:
    scoped_trigger(const std::string& name, const std::string& createSql)
        : _name(name),
          _db(TEST_DB_PATH.string(), SQLite::OPEN_READWRITE)
    {
        _db.exec(createSql);
    }

    ~scoped_trigger()
    {
        try { _db.exec("DROP TRIGGER IF EXISTS " + _name + ";"); }
        catch (...) {}
    }

    scoped_trigger(const scoped_trigger&) = delete;
    scoped_trigger& operator=(const scoped_trigger&) = delete;

private:
    std::string _name;
    SQLite::Database _db;
};

// Returns the database ID of a URL, or -1 if it doesn't exist.
int getPageId(const std::string& url)
{
    SQLite::Database inspect(
        TEST_DB_PATH.string(),
        SQLite::OPEN_READONLY
    );

    SQLite::Statement query(
        inspect,
        "SELECT id FROM frontier WHERE url = ?;"
    );

    query.bind(1, url);

    if (!query.executeStep())
        return -1;

    return query.getColumn(0).getInt();
}

// Returns the frequency of a URL, or -1 if it doesn't exist.
int getFrequency(const std::string& url)
{
    SQLite::Database inspect(
        TEST_DB_PATH.string(),
        SQLite::OPEN_READONLY
    );

    SQLite::Statement query(
        inspect,
        "SELECT frequency FROM frontier WHERE url = ?;"
    );

    query.bind(1, url);

    if (!query.executeStep())
        return -1;

    return query.getColumn(0).getInt();
}

// Returns the number of outgoing relationships belonging to a URL.
int getOutgoingLinkCount(const std::string& url)
{
    SQLite::Database inspect(
        TEST_DB_PATH.string(),
        SQLite::OPEN_READONLY
    );

    SQLite::Statement query(
        inspect,
        "SELECT COUNT(*) "
        "FROM page_links "
        "WHERE source_id = ("
        "    SELECT id FROM frontier WHERE url = ?"
        ");"
    );

    query.bind(1, url);
    query.executeStep();

    return query.getColumn(0).getInt();
}

// Checks whether a table exists in the database.
bool tableExists(SQLite::Database& db, const std::string& tableName)
{
    SQLite::Statement query(
        db,
        "SELECT 1 FROM sqlite_master "
        "WHERE type = 'table' AND name = ?;"
    );

    query.bind(1, tableName);

    return query.executeStep();
}

// Checks whether a page exists and has the expected values.
bool verifyPage(
    const std::string& url,
    const std::string& expectedHtml,
    const std::string& expectedHash)
{
    SQLite::Database inspect(
        TEST_DB_PATH.string(),
        SQLite::OPEN_READONLY
    );

    SQLite::Statement query(
        inspect,
        "SELECT frequency, isCrawled, isIndexed, html, contentHash "
        "FROM frontier WHERE url = ?;"
    );

    query.bind(1, url);

    if (!query.executeStep())
        return false;

    return
        query.getColumn(0).getInt() == 1 &&
        query.getColumn(1).getInt() == 1 &&
        query.getColumn(2).getInt() == 0 &&
        query.getColumn(3).getString() == expectedHtml &&
        query.getColumn(4).getString() == expectedHash;
}

bool urlExists(const std::string& url)
{
    SQLite::Database inspect(
        TEST_DB_PATH.string(),
        SQLite::OPEN_READONLY
    );

    SQLite::Statement query(
        inspect,
        "SELECT 1 FROM frontier WHERE url = ?;"
    );

    query.bind(1, url);

    return query.executeStep();
}

int getFrontierCount()
{
    SQLite::Database inspect(
        TEST_DB_PATH.string(),
        SQLite::OPEN_READONLY
    );

    SQLite::Statement query(
        inspect,
        "SELECT COUNT(*) FROM frontier;"
    );

    query.executeStep();

    return query.getColumn(0).getInt();
}

struct test_case 
{
	std::string name;
	std::function<bool()> run;
};

////////////////////////////MAIN////////////////////////////////////


int main()
{
    // Create the temporary database directory if necessary.
    std::filesystem::create_directories(
        TEST_DB_PATH.parent_path()
    );

    // Start every test run with a fresh database.
    std::filesystem::remove(TEST_DB_PATH);
    std::filesystem::remove(TEST_DB_PATH.string() + "-wal");
    std::filesystem::remove(TEST_DB_PATH.string() + "-shm");

    // Initialize the database.
    database db(
        TEST_DB_PATH.string(),
        SCHEMA_PATH.string()
    );

    std::vector<test_case> tests;

    int passed = 0;
    int failed = 0;

    // databse init
    tests.push_back({
        "Database initialization",
        [&]()
        {
            SQLite::Database inspect(
                TEST_DB_PATH.string(),
                SQLite::OPEN_READONLY
            );

            return
                tableExists(inspect, "frontier") &&
                tableExists(inspect, "page_links");
        }
    });


    // insert new page
    tests.push_back({
        "Save a new page",
        [&]()
        {
            const html_url page {
                "https://example.com",
                "<html><body>Hello World</body></html>",
                "hash_001"
            };

            const bool saved = db.saveCrawledPages(
                {page},
                {}
            );

            if (!saved)
                return false;

            return verifyPage(
                page.url,
                page.html,
                page.contentHash
            );
        }
	});

	// Tests that inserting a page with a content hash that already exists
	// does not create another frontier entry or duplicate the stored content.
	tests.push_back({
		"Duplicate content hash",
    		[&]()
    	{
        	const html_url original {
            	"https://example.com/original",
            	"<html>Original content</html>",
            	"duplicate_hash"
        	};

        	const html_url duplicate {
            	"https://example.com/duplicate",
            	"<html>Original content</html>",
            	"duplicate_hash"
        	};

        	if (!db.saveCrawledPages({original}, {}))
            		return false;

        	const int countBefore = getFrontierCount();

        	if (!db.saveCrawledPages({duplicate}, {}))
            		return false;

        	const int countAfter = getFrontierCount();

        	return
            		urlExists(original.url) &&
            		!urlExists(duplicate.url) &&
			verifyPage(
					original.url,
					original.html,
					original.contentHash
				  )&&
            		countBefore == countAfter;
    		}
	});

	// Tests that a page with an empty URL is rejected and
	// does not create a new entry in the frontier table.
	tests.push_back({
    		"Empty URL",
    		[&]()
    	{
        	const html_url invalid {
            	"",
            	"<html>Some content</html>",
            	"empty_url_hash"
        	};

        	const int countBefore = getFrontierCount();

        	const bool result = db.saveCrawledPages(
            	{invalid},
            	{}
        	);

        	const int countAfter = getFrontierCount();

        	return
            		result &&
            		!urlExists(invalid.url) &&
            		countBefore == countAfter;
    	}
	});

	// Tests that a page with empty HTML is rejected and
	// does not create a new entry in the frontier table.
	tests.push_back({
    		"Empty HTML",
    		[&]()
    	{
        	const html_url invalid {
            	"https://example.com/empty-html",
            	"",
            	"empty_html_hash"
        	};

        	const int countBefore = getFrontierCount();

        	const bool result = db.saveCrawledPages(
            	{invalid},
            	{}
        	);

        	const int countAfter = getFrontierCount();

        	return
            		result &&
            		!urlExists(invalid.url) &&
            		countBefore == countAfter;
    	}
	});

	// Tests that inserting an existing URL with new content updates its HTML
// and content hash while preserving its ID and frequency.
tests.push_back({
    "Update existing URL",
    [&]()
    {
        const html_url original {
            "https://example.com/update",
            "<html>Old content</html>",
            "old_hash"
        };

        const html_url updated {
            "https://example.com/update",
            "<html>New content</html>",
            "new_hash"
        };

        if (!db.saveCrawledPages({original}, {}))
            return false;

        const int originalId = getPageId(original.url);
        const int originalFrequency = getFrequency(original.url);

        if (!db.saveCrawledPages({updated}, {}))
            return false;

        return
            getPageId(updated.url) == originalId &&
            getFrequency(updated.url) == originalFrequency &&
            verifyPage(
                updated.url,
                updated.html,
                updated.contentHash
            );
    }
});


// Tests that saving the same URL with the same content hash succeeds
// without creating another frontier entry or changing its stored content.
tests.push_back({
    "Same URL and same hash",
    [&]()
    {
        const html_url original {
            "https://example.com/same",
            "<html>Same content</html>",
            "same_hash"
        };

        if (!db.saveCrawledPages({original}, {}))
            return false;

        const int countBefore = getFrontierCount();
        const int originalId = getPageId(original.url);

        if (!db.saveCrawledPages({original}, {}))
            return false;

        return
            getFrontierCount() == countBefore &&
            getPageId(original.url) == originalId &&
            verifyPage(
                original.url,
                original.html,
                original.contentHash
            );
    }
});


// Tests that multiple distinct pages can be inserted successfully
// in a single saveCrawledPages() call.
tests.push_back({
    "Multiple pages in one batch",
    [&]()
    {
        const std::vector<html_url> pages {
            {
                "https://example.com/batch1",
                "<html>Page 1</html>",
                "batch_hash_1"
            },
            {
                "https://example.com/batch2",
                "<html>Page 2</html>",
                "batch_hash_2"
            },
            {
                "https://example.com/batch3",
                "<html>Page 3</html>",
                "batch_hash_3"
            }
        };

        const int countBefore = getFrontierCount();

        if (!db.saveCrawledPages(pages, {}))
            return false;

        return
            getFrontierCount() == countBefore + 3 &&
            verifyPage(
                pages[0].url,
                pages[0].html,
                pages[0].contentHash
            ) &&
            verifyPage(
                pages[1].url,
                pages[1].html,
                pages[1].contentHash
            ) &&
            verifyPage(
                pages[2].url,
                pages[2].html,
                pages[2].contentHash
            );
    }
});


// Tests that a page with an empty content hash is rejected
// and does not create a new entry in the frontier table.
tests.push_back({
    "Empty content hash",
    [&]()
    {
        const html_url invalid {
            "https://example.com/empty-hash",
            "<html>Some content</html>",
            ""
        };

        const int countBefore = getFrontierCount();

        const bool result = db.saveCrawledPages(
            {invalid},
            {}
        );

        return
            result &&
            !urlExists(invalid.url) &&
            getFrontierCount() == countBefore;
    }
});

// Tests that unknown target URLs are automatically inserted into
// the frontier as uncrawled placeholder entries.
tests.push_back({
    "Create placeholder URLs",
    [&]()
    {
        const html_url source {
            "https://example.com/placeholder-source",
            "<html>Source</html>",
            "placeholder_source_hash"
        };

        const page_links links {
            source.url,
            {
                "https://example.com/placeholder1",
                "https://example.com/placeholder2"
            }
        };

        const int countBefore = getFrontierCount();

        if (!db.saveCrawledPages({source}, {links}))
            return false;

        return
            getFrontierCount() == countBefore + 3 &&
            urlExists(links.links[0]) &&
            urlExists(links.links[1]) &&
            getFrequency(links.links[0]) == 1 &&
            getFrequency(links.links[1]) == 1;
    }
});


// Tests that inserting the same source-target relationship twice
// does not create duplicate entries in page_links.
tests.push_back({
    "Duplicate relationship",
    [&]()
    {
        const html_url source {
            "https://example.com/duplicate-link-source",
            "<html>Source</html>",
            "duplicate_link_source_hash"
        };

        const page_links links {
            source.url,
            {
                "https://example.com/duplicate-link-target"
            }
        };

        if (!db.saveCrawledPages({source}, {links}))
            return false;

        if (!db.saveCrawledPages({}, {links}))
            return false;

        return
            getOutgoingLinkCount(source.url) == 1 &&
            urlExists(links.links[0]);
    }
});


// Tests that linking to an existing frontier URL increments its
// frequency because another page references it.
tests.push_back({
    "Frequency increment",
    [&]()
    {
        const html_url source {
            "https://example.com/frequency-source",
            "<html>Source</html>",
            "frequency_source_hash"
        };

        const html_url target {
            "https://example.com/frequency-target",
            "<html>Target</html>",
            "frequency_target_hash"
        };

        if (!db.saveCrawledPages({source, target}, {}))
            return false;

        const int frequencyBefore = getFrequency(target.url);

        const page_links links {
            source.url,
            {target.url}
        };

        if (!db.saveCrawledPages({}, {links}))
            return false;

        return getFrequency(target.url) == frequencyBefore + 1;
    }
});


// Tests that inserting an existing relationship again does not
// increment the target frequency a second time.
tests.push_back({
    "Duplicate relationship does not increment frequency",
    [&]()
    {
        const html_url source {
            "https://example.com/frequency-duplicate-source",
            "<html>Source</html>",
            "frequency_duplicate_source_hash"
        };

        const html_url target {
            "https://example.com/frequency-duplicate-target",
            "<html>Target</html>",
            "frequency_duplicate_target_hash"
        };

        if (!db.saveCrawledPages({source, target}, {}))
            return false;

        const page_links links {
            source.url,
            {target.url}
        };

        if (!db.saveCrawledPages({}, {links}))
            return false;

        const int frequencyAfterFirstLink = getFrequency(target.url);

        if (!db.saveCrawledPages({}, {links}))
            return false;

        return
            getOutgoingLinkCount(source.url) == 1 &&
            getFrequency(target.url) == frequencyAfterFirstLink;
    }
});

// Tests that a page can link to itself, but the self-reference
// is ignored and is not stored in page_links.
tests.push_back({
    "Self-referencing link",
    [&]()
    {
        const html_url source {
            "https://example.com/self-link",
            "<html>Self-referencing page</html>",
            "self_link_hash"
        };

        const page_links links {
            source.url,
            {source.url}
        };

        const int countBefore = getFrontierCount();

        std::cout << "    URL exists before insert: "
                  << urlExists(source.url) << '\n';

        if (!db.saveCrawledPages({source}, {links}))
            return false;

        const int countAfter = getFrontierCount();
        const int outgoingLinks = getOutgoingLinkCount(source.url);
        const int frequency = getFrequency(source.url);

        std::cout << "    Frontier before: " << countBefore << '\n'
                  << "    Frontier after:  " << countAfter << '\n'
                  << "    Outgoing links:  " << outgoingLinks << '\n'
                  << "    Frequency:      " << frequency << '\n';

        return
            countAfter == countBefore + 1 &&
            outgoingLinks == 0 &&
            frequency == 1;
    }
});



// Tests that a relationship with an empty source URL is ignored
// and does not create its target URL in the frontier.
tests.push_back({
    "Empty source URL in links",
    [&]()
    {
        const page_links invalid {
            "",
            {"https://example.com/empty-source-target"}
        };

        const int countBefore = getFrontierCount();

        const bool result = db.saveCrawledPages({}, {invalid});

        return
            result &&
            getFrontierCount() == countBefore &&
            !urlExists(invalid.links[0]);
    }
});

// Tests that valid pages are still saved when the same batch
// contains an invalid page that should be skipped.
tests.push_back({
    "Valid and invalid pages in one batch",
    [&]()
    {
        const html_url valid1 {
            "https://example.com/mixed1",
            "<html>Valid page 1</html>",
            "mixed_hash_1"
        };

        const html_url invalid {
            "https://example.com/mixed-invalid",
            "",
            "mixed_invalid_hash"
        };

        const html_url valid2 {
            "https://example.com/mixed2",
            "<html>Valid page 2</html>",
            "mixed_hash_2"
        };

        const int countBefore = getFrontierCount();

        const bool result = db.saveCrawledPages(
            {valid1, invalid, valid2},
            {}
        );

        return
            result &&
            getFrontierCount() == countBefore + 2 &&
            verifyPage(
                valid1.url,
                valid1.html,
                valid1.contentHash
            ) &&
            verifyPage(
                valid2.url,
                valid2.html,
                valid2.contentHash
            ) &&
            !urlExists(invalid.url);
    }
});


// Tests that outgoing links create page_links relationships
// and that previously unknown target URLs are added to frontier
// as uncrawled placeholder entries.
tests.push_back({
    "Insert outgoing links",
    [&]()
    {
        const html_url source {
            "https://example.com/source",
            "<html>Source page</html>",
            "source_hash"
        };

        const page_links links {
            "https://example.com/source",
            {
                "https://example.com/target1",
                "https://example.com/target2"
            }
        };

        const int countBefore = getFrontierCount();

        const bool result = db.saveCrawledPages(
            {source},
            {links}
        );

        if (!result)
            return false;

        return
            getFrontierCount() == countBefore + 3 &&
            urlExists(source.url) &&
            urlExists(links.links[0]) &&
            urlExists(links.links[1]) &&
            getOutgoingLinkCount(source.url) == 2;
    }
});

// Tests that a page_links entry containing an empty target URL
// is skipped entirely, including its otherwise valid target URLs.
tests.push_back({
    "Empty target URL in links",
    [&]()
    {
        const html_url source {
            "https://example.com/empty-target-source",
            "<html>Source</html>",
            "empty_target_source_hash"
        };

        const page_links links {
            source.url,
            {
                "",
                "https://example.com/valid-target"
            }
        };

        const int countBefore = getFrontierCount();

        if (!db.saveCrawledPages({source}, {links}))
            return false;

        return
            getFrontierCount() == countBefore + 1 &&
            urlExists(source.url) &&
            !urlExists(links.links[1]) &&
            getOutgoingLinkCount(source.url) == 0;
    }
});


// Tests that two pages with the same content hash in one batch
// do not both get inserted into the frontier.
tests.push_back({
    "Duplicate hashes within one batch",
    [&]()
    {
        const html_url page1 {
            "https://example.com/batch-duplicate1",
            "<html>Duplicate content</html>",
            "batch_duplicate_hash"
        };

        const html_url page2 {
            "https://example.com/batch-duplicate2",
            "<html>Duplicate content</html>",
            "batch_duplicate_hash"
        };

        const int countBefore = getFrontierCount();

        if (!db.saveCrawledPages({page1, page2}, {}))
            return false;

        const bool page1Exists = urlExists(page1.url);
        const bool page2Exists = urlExists(page2.url);
        const int countAfter = getFrontierCount();

	 std::cout << "    page1Exists=" << page1Exists
                  << " page2Exists=" << page2Exists << '\n'
                  << "    countBefore=" << countBefore
                  << " countAfter=" << countAfter << '\n';

        return
            countAfter == countBefore + 1 &&
            page1Exists &&
            !page2Exists;
    }
});


// Tests that SQLite rejects a page_links relationship containing
// IDs that do not exist in the frontier table.
tests.push_back({
    "Foreign key constraint enforcement",
    [&]()
    {
        SQLite::Database testDb(
            TEST_DB_PATH.string(),
            SQLite::OPEN_READWRITE
        );

        testDb.exec("PRAGMA foreign_keys = ON;");

        try
        {
            testDb.exec(
                "INSERT INTO page_links (source_id, target_id) "
                "VALUES (-1, -2);"
            );
        }
        catch (const SQLite::Exception&)
        {
            return true;
        }

        return false;
    }
});


// Tests that deleting a page from frontier automatically deletes
// relationships in page_links that reference that page.
tests.push_back({
    "Cascade deletion",
    [&]()
    {
        const html_url source {
            "https://example.com/cascade-source",
            "<html>Source</html>",
            "cascade_source_hash"
        };

        const html_url target {
            "https://example.com/cascade-target",
            "<html>Target</html>",
            "cascade_target_hash"
        };

        const page_links links {
            source.url,
            {target.url}
        };

        if (!db.saveCrawledPages({source, target}, {links}))
            return false;

        if (getOutgoingLinkCount(source.url) != 1)
            return false;

        const int sourceId = getPageId(source.url);

        SQLite::Database testDb(
            TEST_DB_PATH.string(),
            SQLite::OPEN_READWRITE
        );

        testDb.exec("PRAGMA foreign_keys = ON;");

        SQLite::Statement deletePage(
            testDb,
            "DELETE FROM frontier WHERE id = ?;"
        );

        deletePage.bind(1, sourceId);
        deletePage.exec();

        SQLite::Statement countLinks(
            testDb,
            "SELECT COUNT(*) FROM page_links WHERE source_id = ?;"
        );

        countLinks.bind(1, sourceId);

        if (!countLinks.executeStep())
            return false;

        return countLinks.getColumn(0).getInt() == 0;
    }
});


// Performance test: inserts 50,000 distinct pages in a single batch.
// Measures how long saveCrawledPages() takes to process the batch.
tests.push_back({
    "Insert 50000 pages",
    [&]()
    {
        constexpr int PAGE_COUNT = 50'000;

        std::vector<html_url> pages;
        pages.reserve(PAGE_COUNT);

        for (int i = 0; i < PAGE_COUNT; ++i)
        {
            pages.push_back({
                "https://example.com/performance-page-" + std::to_string(i),
                "<html>Performance page " + std::to_string(i) + "</html>",
                "performance_hash_" + std::to_string(i)
            });
        }

        const int countBefore = getFrontierCount();

        const auto start = std::chrono::steady_clock::now();

        const bool result = db.saveCrawledPages(pages, {});

        const auto end = std::chrono::steady_clock::now();

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start
            ).count();

        std::cout << "    Inserted " << PAGE_COUNT
                  << " pages in " << elapsed << " ms\n";

        return
            result &&
            getFrontierCount() == countBefore + PAGE_COUNT &&
            urlExists(pages.front().url) &&
            urlExists(pages.back().url);
    }
});

// A URL discovered through links (placeholder) is later crawled: it must keep
// its id, edges and frequency, and gain html, hash and isCrawled = 1.
tests.push_back({
    "Placeholder becomes a crawled page",
    [&]()
    {
        const html_url s1 {
            "https://example.com/lifecycle-source-1",
            "<html>S1</html>",
            "lifecycle_source_hash_1"
        };

        const html_url s2 {
            "https://example.com/lifecycle-source-2",
            "<html>S2</html>",
            "lifecycle_source_hash_2"
        };

        const std::string target = "https://example.com/lifecycle-target";

        const page_links links1 { s1.url, {target} };
        const page_links links2 { s2.url, {target} };

        // two pages discover the target -> placeholder with frequency 2
        if (!db.saveCrawledPages({s1, s2}, {links1, links2}))
            return false;

        const page_row placeholder = getRow(target);

        if (!placeholder.exists ||
            placeholder.isCrawled != 0 ||
            !placeholder.htmlIsNull ||
            !placeholder.hashIsNull ||
            placeholder.frequency != 2)
            return false;

        const html_url crawled {
            target,
            "<html>Target content</html>",
            "lifecycle_target_hash"
        };

        if (!db.saveCrawledPages({crawled}, {}))
            return false;

        const page_row after = getRow(target);

        return
            after.id == placeholder.id &&
            after.frequency == 2 &&
            after.isCrawled == 1 &&
            after.isIndexed == 0 &&
            !after.htmlIsNull &&
            after.html == crawled.html &&
            after.contentHash == crawled.contentHash &&
            getIncomingLinkCount(target) == 2;
    }
});

// A placeholder whose fetched content duplicates another page must be marked
// crawled AND indexed, keep NULL html/hash, and keep its frequency.
// (Your existing duplicate test only covers a URL that is NOT in the frontier,
// where markCrawled updates zero rows.)
tests.push_back({
    "Duplicate content on a placeholder",
    [&]()
    {
        const html_url original {
            "https://example.com/dup-placeholder-original",
            "<html>Shared body</html>",
            "dup_placeholder_hash"
        };

        const html_url source {
            "https://example.com/dup-placeholder-source",
            "<html>Source</html>",
            "dup_placeholder_source_hash"
        };

        const std::string placeholderUrl = "https://example.com/dup-placeholder-target";

        const page_links links { source.url, {placeholderUrl} };

        if (!db.saveCrawledPages({original, source}, {links}))
            return false;

        const page_row before = getRow(placeholderUrl);

        if (!before.exists || before.isCrawled != 0)
            return false;

        const html_url duplicate {
            placeholderUrl,
            original.html,
            original.contentHash
        };

        if (!db.saveCrawledPages({duplicate}, {}))
            return false;

        const page_row after = getRow(placeholderUrl);
        const page_row originalRow = getRow(original.url);

        return
            after.exists &&
            after.id == before.id &&
            after.isCrawled == 1 &&
            after.isIndexed == 1 &&
            after.htmlIsNull &&
            after.hashIsNull &&
            after.frequency == before.frequency &&
            originalRow.html == original.html &&
            originalRow.contentHash == original.contentHash;
    }
});

// CURRENT BEHAVIOR, decide if you want it: an existing page re-crawled with
// content identical to ANOTHER page keeps its old html/hash and is force-marked
// isIndexed = 1. If it was never indexed, its old content now never will be.
// Flip the expectations if you change the design.
tests.push_back({
    "Existing URL re-saved with another page's content (current behavior)",
    [&]()
    {
        const html_url a {
            "https://example.com/stale-a",
            "<html>A</html>",
            "stale_hash_a"
        };

        const html_url b {
            "https://example.com/stale-b",
            "<html>B</html>",
            "stale_hash_b"
        };

        if (!db.saveCrawledPages({a, b}, {}))
            return false;

        const int countBefore = getFrontierCount();

        const html_url aNowSameAsB { a.url, b.html, b.contentHash };

        if (!db.saveCrawledPages({aNowSameAsB}, {}))
            return false;

        const page_row rowA = getRow(a.url);
        const page_row rowB = getRow(b.url);

        return
            getFrontierCount() == countBefore &&
            rowA.html == a.html &&
            rowA.contentHash == a.contentHash &&
            rowA.isCrawled == 1 &&
            rowA.isIndexed == 1 &&
            rowB.html == b.html &&
            rowB.contentHash == b.contentHash;
    }
});

// The UNIQUE index on contentHash must release a hash once the only row using
// it has been updated to new content.
tests.push_back({
    "Hash released by an update can be reused",
    [&]()
    {
        const html_url a {
            "https://example.com/reuse-a",
            "<html>A v1</html>",
            "reuse_hash_old"
        };

        if (!db.saveCrawledPages({a}, {}))
            return false;

        const html_url aUpdated {
            a.url,
            "<html>A v2</html>",
            "reuse_hash_new"
        };

        if (!db.saveCrawledPages({aUpdated}, {}))
            return false;

        const html_url b {
            "https://example.com/reuse-b",
            a.html,
            a.contentHash
        };

        if (!db.saveCrawledPages({b}, {}))
            return false;

        return
            urlExists(b.url) &&
            verifyPage(b.url, b.html, b.contentHash) &&
            verifyPage(aUpdated.url, aUpdated.html, aUpdated.contentHash);
    }
});

// Same URL twice in one batch with different content: last one wins, one row.
tests.push_back({
    "Same URL twice in one batch",
    [&]()
    {
        const html_url first {
            "https://example.com/twice",
            "<html>v1</html>",
            "twice_hash_1"
        };

        const html_url second {
            "https://example.com/twice",
            "<html>v2</html>",
            "twice_hash_2"
        };

        const int countBefore = getFrontierCount();

        if (!db.saveCrawledPages({first, second}, {}))
            return false;

        return
            getFrontierCount() == countBefore + 1 &&
            verifyPage(second.url, second.html, second.contentHash);
    }
});

// An empty batch is a successful no-op.
tests.push_back({
    "Empty batch",
    [&]()
    {
        const int countBefore = getFrontierCount();
        const int linksBefore = getTotalLinkCount();

        return
            db.saveCrawledPages({}, {}) &&
            getFrontierCount() == countBefore &&
            getTotalLinkCount() == linksBefore;
    }
});

// A and B are saved in the SAME call and A links to B: B still gets +1.
tests.push_back({
    "Frequency: link between two pages of one batch",
    [&]()
    {
        const html_url a {
            "https://example.com/same-batch-a",
            "<html>A</html>",
            "same_batch_hash_a"
        };

        const html_url b {
            "https://example.com/same-batch-b",
            "<html>B</html>",
            "same_batch_hash_b"
        };

        const page_links links { a.url, {b.url} };

        const int countBefore = getFrontierCount();

        if (!db.saveCrawledPages({a, b}, {links}))
            return false;

        return
            getFrontierCount() == countBefore + 2 &&
            getFrequency(a.url) == 1 &&
            getFrequency(b.url) == 2 &&
            edgeExists(a.url, b.url);
    }
});

// Two sources link to the same brand-new target in one batch.
tests.push_back({
    "Frequency: two sources, one new target, one batch",
    [&]()
    {
        const html_url s1 {
            "https://example.com/new-target-source-1",
            "<html>S1</html>",
            "new_target_hash_1"
        };

        const html_url s2 {
            "https://example.com/new-target-source-2",
            "<html>S2</html>",
            "new_target_hash_2"
        };

        const std::string target = "https://example.com/new-target-shared";

        const page_links links1 { s1.url, {target} };
        const page_links links2 { s2.url, {target} };

        const int countBefore = getFrontierCount();

        if (!db.saveCrawledPages({s1, s2}, {links1, links2}))
            return false;

        return
            getFrontierCount() == countBefore + 3 &&
            getFrequency(target) == 2 &&
            getIncomingLinkCount(target) == 2;
    }
});

// The same target repeated inside ONE link list counts once, for a new target
// (stays 1) and for an existing one (+1 only).
tests.push_back({
    "Frequency: repeated target inside one link list",
    [&]()
    {
        const html_url source {
            "https://example.com/repeat-source",
            "<html>Source</html>",
            "repeat_source_hash"
        };

        const html_url existing {
            "https://example.com/repeat-existing",
            "<html>Existing</html>",
            "repeat_existing_hash"
        };

        const std::string fresh = "https://example.com/repeat-new";

        if (!db.saveCrawledPages({source, existing}, {}))
            return false;

        const int existingBefore = getFrequency(existing.url);

        const page_links links {
            source.url,
            {fresh, fresh, existing.url, existing.url}
        };

        if (!db.saveCrawledPages({}, {links}))
            return false;

        return
            getFrequency(fresh) == 1 &&
            getFrequency(existing.url) == existingBefore + 1 &&
            getOutgoingLinkCount(source.url) == 2;
    }
});

// The same link entry twice in one batch must not double count.
tests.push_back({
    "Frequency: same link entry twice in one batch",
    [&]()
    {
        const html_url source {
            "https://example.com/twice-entry-source",
            "<html>Source</html>",
            "twice_entry_source_hash"
        };

        const html_url target {
            "https://example.com/twice-entry-target",
            "<html>Target</html>",
            "twice_entry_target_hash"
        };

        if (!db.saveCrawledPages({source, target}, {}))
            return false;

        const int before = getFrequency(target.url);

        const page_links links { source.url, {target.url} };

        if (!db.saveCrawledPages({}, {links, links}))
            return false;

        return
            getFrequency(target.url) == before + 1 &&
            getOutgoingLinkCount(source.url) == 1;
    }
});

// Five pages link to one existing page in a single batch: delta of +5.
tests.push_back({
    "Frequency: many sources, one existing target",
    [&]()
    {
        const html_url target {
            "https://example.com/many-target",
            "<html>Target</html>",
            "many_target_hash"
        };

        if (!db.saveCrawledPages({target}, {}))
            return false;

        const int before = getFrequency(target.url);

        std::vector<html_url> sources;
        std::vector<page_links> links;

        for (int i = 0; i < 5; ++i)
        {
            sources.push_back({
                "https://example.com/many-source-" + std::to_string(i),
                "<html>Source " + std::to_string(i) + "</html>",
                "many_source_hash_" + std::to_string(i)
            });

            links.push_back({ sources.back().url, {target.url} });
        }

        if (!db.saveCrawledPages(sources, links))
            return false;

        return
            getFrequency(target.url) == before + 5 &&
            getIncomingLinkCount(target.url) == 5;
    }
});

// Frequency keeps growing correctly over several separate batches.
tests.push_back({
    "Frequency: accumulates across batches",
    [&]()
    {
        const std::string target = "https://example.com/accumulate-target";

        std::vector<html_url> sources;

        for (int i = 0; i < 3; ++i)
        {
            sources.push_back({
                "https://example.com/accumulate-source-" + std::to_string(i),
                "<html>Source " + std::to_string(i) + "</html>",
                "accumulate_source_hash_" + std::to_string(i)
            });
        }

        if (!db.saveCrawledPages(sources, {}))
            return false;

        for (int i = 0; i < 3; ++i)
        {
            const page_links links { sources[i].url, {target} };

            if (!db.saveCrawledPages({}, {links}))
                return false;

            // new placeholder starts at 1, each further referrer adds 1
            if (getFrequency(target) != i + 1)
                return false;
        }

        return getIncomingLinkCount(target) == 3;
    }
});

tests.push_back({
    "Links of an invalid page are dropped",
    [&]()
    {
        const html_url invalid {
            "https://example.com/invalid-links-source",
            "",
            "invalid_links_hash"
        };

        const std::string target = "https://example.com/invalid-links-target";

        const page_links links { invalid.url, {target} };

        const int countBefore = getFrontierCount();
        const int linksBefore = getTotalLinkCount();

        if (!db.saveCrawledPages({invalid}, {links}))
            return false;

        return
            getFrontierCount() == countBefore &&
            !urlExists(invalid.url) &&
            !urlExists(target) &&
            getTotalLinkCount() == linksBefore;
    }
});

tests.push_back({
    "Links of a duplicate-content page are dropped",
    [&]()
    {
        const html_url original {
            "https://example.com/dup-links-original",
            "<html>Shared</html>",
            "dup_links_hash"
        };

        if (!db.saveCrawledPages({original}, {}))
            return false;

        const html_url duplicate {
            "https://example.com/dup-links-duplicate",
            original.html,
            original.contentHash
        };

        const std::string target = "https://example.com/dup-links-target";

        const page_links links { duplicate.url, {target} };

        const int countBefore = getFrontierCount();
        const int linksBefore = getTotalLinkCount();

        if (!db.saveCrawledPages({duplicate}, {links}))
            return false;

        return
            getFrontierCount() == countBefore &&
            !urlExists(duplicate.url) &&
            !urlExists(target) &&
            getTotalLinkCount() == linksBefore;
    }
});

// Your existing update test would pass even if isIndexed were never reset,
// because the flag starts at 0. This one sets it to 1 first.
tests.push_back({
    "Re-crawl with changed content clears isIndexed",
    [&]()
    {
        const html_url v1 {
            "https://example.com/reindex-changed",
            "<html>v1</html>",
            "reindex_changed_hash_1"
        };

        if (!db.saveCrawledPages({v1}, {}))
            return false;

        setIndexed(v1.url, 1);

        if (getRow(v1.url).isIndexed != 1)
            return false;

        const html_url v2 {
            v1.url,
            "<html>v2</html>",
            "reindex_changed_hash_2"
        };

        if (!db.saveCrawledPages({v2}, {}))
            return false;

        const page_row row = getRow(v2.url);

        return row.isIndexed == 0 && row.html == v2.html;
    }
});

// CURRENT BEHAVIOR, decide if you want it: re-saving IDENTICAL content also
// resets isIndexed to 0, so unchanged pages get re-indexed on every re-crawl.
tests.push_back({
    "Re-crawl with identical content (current behavior: re-queued for indexing)",
    [&]()
    {
        const html_url page {
            "https://example.com/reindex-same",
            "<html>Unchanged</html>",
            "reindex_same_hash"
        };

        if (!db.saveCrawledPages({page}, {}))
            return false;

        setIndexed(page.url, 1);

        if (!db.saveCrawledPages({page}, {}))
            return false;

        return getRow(page.url).isIndexed == 0;
    }
});

// html and contentHash are BLOB columns: NUL bytes and non-UTF-8 bytes must
// survive unchanged.
tests.push_back({
    "Binary html and hash round trip",
    [&]()
    {
        std::string html = "<html>";
        html.push_back('\0');
        html.push_back('\xff');
        html.push_back('\x01');
        html += "</html>";

        const std::string hash("\x00\x01\x02\xfe", 4);

        const html_url page {
            "https://example.com/binary",
            html,
            hash
        };

        if (!db.saveCrawledPages({page}, {}))
            return false;

        const page_row row = getRow(page.url);

        return
            row.html.size() == html.size() &&
            row.html == html &&
            row.contentHash.size() == hash.size() &&
            row.contentHash == hash;
    }
});

// An error in the middle of a batch must undo the WHOLE batch and the
// connection must be usable afterwards.
tests.push_back({
    "Failed batch rolls back every change",
    [&]()
    {
        const html_url existing {
            "https://example.com/rollback-existing",
            "<html>Existing</html>",
            "rollback_existing_hash"
        };

        if (!db.saveCrawledPages({existing}, {}))
            return false;

        const html_url source {
            "https://example.com/rollback-source",
            "<html>Source</html>",
            "rollback_source_hash"
        };

        const std::string poison = "https://example.com/poison-insert";

        // existing link first, poisoned second: the first edge is already
        // written when the error happens
        const page_links links { source.url, {existing.url, poison} };

        const int countBefore = getFrontierCount();
        const int linksBefore = getTotalLinkCount();
        const int frequencyBefore = getFrequency(existing.url);

        bool failed = false;

        {
            const scoped_trigger guard(
                "test_poison_insert",
                "CREATE TRIGGER test_poison_insert BEFORE INSERT ON frontier "
                "WHEN NEW.url LIKE '%poison-insert%' "
                "BEGIN SELECT RAISE(ABORT, 'poisoned insert'); END;"
            );

            failed = !db.saveCrawledPages({source}, {links});
        }

        if (!failed)
            return false;

        if (urlExists(source.url) || urlExists(poison))
            return false;

        if (getFrontierCount() != countBefore ||
            getTotalLinkCount() != linksBefore ||
            getFrequency(existing.url) != frequencyBefore)
            return false;

        // the failed transaction must not be left open
        const html_url after {
            "https://example.com/rollback-after",
            "<html>After</html>",
            "rollback_after_hash"
        };

        return
            db.saveCrawledPages({after}, {}) &&
            verifyPage(after.url, after.html, after.contentHash);
    }
});

// An error while applying the frequency deltas (the last step) must also roll
// back the edges that were already inserted.
tests.push_back({
    "Failed frequency update rolls back the batch",
    [&]()
    {
        const html_url source {
            "https://example.com/freq-rollback-source",
            "<html>Source</html>",
            "freq_rollback_source_hash"
        };

        const html_url good {
            "https://example.com/freq-rollback-good",
            "<html>Good</html>",
            "freq_rollback_good_hash"
        };

        const html_url poisoned {
            "https://example.com/poison-freq-target",
            "<html>Poisoned</html>",
            "freq_rollback_poisoned_hash"
        };

        if (!db.saveCrawledPages({source, good, poisoned}, {}))
            return false;

        const int goodBefore = getFrequency(good.url);
        const int poisonedBefore = getFrequency(poisoned.url);
        const int linksBefore = getTotalLinkCount();

        const page_links links { source.url, {good.url, poisoned.url} };

        bool failed = false;

        {
            const scoped_trigger guard(
                "test_poison_frequency",
                "CREATE TRIGGER test_poison_frequency BEFORE UPDATE OF frequency ON frontier "
                "WHEN NEW.url LIKE '%poison-freq%' "
                "BEGIN SELECT RAISE(ABORT, 'poisoned update'); END;"
            );

            failed = !db.saveCrawledPages({}, {links});
        }

        return
            failed &&
            getFrequency(good.url) == goodBefore &&
            getFrequency(poisoned.url) == poisonedBefore &&
            getTotalLinkCount() == linksBefore &&
            getOutgoingLinkCount(source.url) == 0;
    }
});

// Your cascade test only deletes the SOURCE of an edge. This deletes the target.
tests.push_back({
    "Cascade deletion of a target removes incoming edges",
    [&]()
    {
        const html_url source {
            "https://example.com/cascade-target-source",
            "<html>Source</html>",
            "cascade_target_source_hash"
        };

        const html_url target {
            "https://example.com/cascade-target-target",
            "<html>Target</html>",
            "cascade_target_target_hash"
        };

        const page_links links { source.url, {target.url} };

        if (!db.saveCrawledPages({source, target}, {links}))
            return false;

        if (!edgeExists(source.url, target.url))
            return false;

        const int linksBefore = getTotalLinkCount();

        deleteFrontierRow(target.url);

        return
            urlExists(source.url) &&
            !urlExists(target.url) &&
            getOutgoingLinkCount(source.url) == 0 &&
            getTotalLinkCount() == linksBefore - 1;
    }
});


// Performance test: inserts 50,000 outgoing relationships from one
// source page to 50,000 distinct target URLs in a single batch.
tests.push_back({
    "Insert 50000 relationships",
    [&]()
    {
        constexpr int LINK_COUNT = 50'000;

        const html_url source {
            "https://example.com/performance-link-source",
            "<html>Source page</html>",
            "performance_link_source_hash"
        };

        page_links links;
        links.url = source.url;
        links.links.reserve(LINK_COUNT);

        for (int i = 0; i < LINK_COUNT; ++i)
        {
            links.links.push_back(
                "https://example.com/performance-link-target-" +
                std::to_string(i)
            );
        }

        const int countBefore = getFrontierCount();

        const auto start = std::chrono::steady_clock::now();

        const bool result = db.saveCrawledPages(
            {source},
            {links}
        );

        const auto end = std::chrono::steady_clock::now();

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start
            ).count();

        std::cout << "    Inserted " << LINK_COUNT
                  << " relationships in " << elapsed << " ms\n";

        return
            result &&
            getFrontierCount() == countBefore + LINK_COUNT + 1 &&
            getOutgoingLinkCount(source.url) == LINK_COUNT &&
            urlExists(links.links.front()) &&
            urlExists(links.links.back());
    }
});



    // ========================================================
    // RUN TESTS
    // ========================================================

    for (const auto& test : tests)
    {
        std::cout << "[ RUN      ] " << test.name << '\n';

        try
        {
            if (test.run())
            {
                std::cout << "[       OK ] " << test.name << '\n';
                ++passed;
            }
            else
            {
                std::cout << "[  FAILED  ] " << test.name << '\n';
                ++failed;
            }
        }
        catch (const std::exception& e)
        {
            std::cout << "[  ERROR   ] " << test.name
                      << ": " << e.what() << '\n';
            ++failed;
        }
    }

    // ========================================================
    // SUMMARY
    // ========================================================

    std::cout << "\n========== TEST SUMMARY ==========\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';
    std::cout << "Total:  " << passed + failed << '\n';

    std::cout << "\nTest database preserved at: "
              << TEST_DB_PATH << '\n';

    return failed == 0 ? 0 : 1;
}
