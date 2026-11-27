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

        return
            getFrontierCount() == countBefore + 1 &&
            (page1Exists != page2Exists);
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
