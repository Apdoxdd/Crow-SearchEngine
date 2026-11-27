#include "database.hpp"
#include "SQLiteCpp/Transaction.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <climits>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <algorithm>


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
		throw std::runtime_error(
				"Failed to open database schema: " + schemaPath 
				);
	}

	std::stringstream buffer;
	buffer<<schemaFile.rdbuf();

	if (schemaFile.bad())
	{
		throw std::runtime_error(
				"Failed to read database schema: " + schemaPath 
				);
	}

	SQLite::Transaction transaction(_db);

	_db.exec(buffer.str());
	transaction.commit();
}

bool database::saveCrawledPages(
		const std::vector<html_url>& html_urls,
		const std::vector<page_links>& extracted_links
		)
{
	try 
	{
		SQLite::Transaction transaction(_db, SQLite::TransactionBehavior::IMMEDIATE);
		// One transaction for the entire batch


		/* check if hash already exists if so drop row 
		 * 	if no match exist insert the url
		 * check if url exists if so update that url hash and content
		 * if insert or update succeeds rerturn the ID
		 *
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

		// mark a url that had its content the same as another as crawled so we dont keep on crawling it over and over 
        	SQLite::Statement markCrawled(
            		_db,
            		"UPDATE frontier SET isCrawled = 1 WHERE url = ?;"
        	);

		// when inserting a url after it was discovered (not yet crawled) so we only insert urls that are unique`
        	SQLite::Statement insertUrl(
        		_db,
            		"INSERT INTO frontier (url) VALUES (?) "
            		"ON CONFLICT(url) DO NOTHING "
            		"RETURNING id;"
        	);

        	// fetches the id of a url that already existed
        	SQLite::Statement getId(
            		_db,
            		"SELECT id FROM frontier WHERE url = ?;"
        	);
		
		//record 1 source -> for multi target edges, do nothing for duplicates
		//exec() returns 1 for a new edge and zero for an old one
        	SQLite::Statement insertLink(
            		_db,
            		"INSERT INTO page_links (source_id, target_id) "
            		"VALUES (?, ?) "
            		"ON CONFLICT(source_id, target_id) DO NOTHING;"
        	);

        	// Adds a delta to a url's frequency (relative, not overwrite).
        	SQLite::Statement applyFrequency(
            		_db,
            		"UPDATE frontier "
            		"SET frequency = frequency + ? "
            		"WHERE id = ?;"
        	);

        	////////////////////////////////// URL TO ID ////////////////////////////////////
        	// get how many links we will see, to set the map size so that the map doesnt re-hash as it grows
        	std::size_t totalLinks = 0;
		for (const auto& page : extracted_links)
    		totalLinks += page.links.size() + 1;        // +1 for the source url itself
    							    
		// pages_links stores (source_id, targer_id), but crawler gets us ids as strings
		// this blocks transaltes the string-urls into the ids that are stored in db 
		// creates the db row if the urls is new
		// throws if theres a db error which could be by hardware

    		// reserving the map
    		std::unordered_map<std::string, std::int64_t> urlCache;
		urlCache.reserve(html_urls.size() + totalLinks);

		// Returns the id for a url, creating the frontier row if it doesn't exist.
		// 'inserted' is set to true only if THIS call created the row.
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

////////////////////////////////// SAVE PAGES ////////////////////////////////////
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
    			savePage.bind(3, page.contentHash.data(),
                  	static_cast<int>(page.contentHash.size()));

                  	if (savePage.executeStep())
    			{
        			// a row came back: the page was inserted or updated
        			// remember its id so the link step needs no lookup for it
        			urlCache[page.url] = savePage.getColumn(0).getInt64();
        			savePage.reset();
			}
			else 
    			{
        			// no row back: this content already exists under another
        			// don't store the content, just mark the url crawled
        			savePage.reset();

        			markCrawled.bind(1, page.url);
        			markCrawled.exec();
        			markCrawled.reset();

        			skippedUrls.insert(page.url);
    			}
		}

////////////////////////////////// SAVE LINKS //////////////////////////////////////////
///// how many new references each existing url gained during this batch
/// put every linked URL into frontier so crawler crawls it later, record each source->pair in page_links
/// calc how many refernces each existing url gained, not updated in database yet

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
        			std::cerr << "Skipping link entry with an empty link: "
                  			  << page.url << '\n';
        			continue;
    			}
    			bool sourceInserted = false;
    			const std::int64_t sourceId = getorcreateid(page.url, sourceInserted);

			for (const auto& link : page.links)
			{
        			// a page referencing itself isnt referenced by others
        			if (link == page.url)
            				continue;

        			bool targetInserted = false;
        			const std::int64_t targetId = getorcreateid(link, targetInserted);

        			insertLink.bind(1, sourceId);
        			insertLink.bind(2, targetId);
        			const int changed = insertLink.exec();      // 1 = new edge, 0 = already there
        			insertLink.reset();

				// a brand-new url already starts at frequency 1 (its first referrer),
        			// so only new edges to exisitng urls add to the count
        			if (changed > 0 && !targetInserted)
            				++frequencyDelta[targetId];
    			}
		}

////////////////////////////////// APPLY FREQUENCY ///////////////////////////////////////
// add each target's accumulated count to its row, one UPDATE per distinct target
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
		std::cerr<<"saveCrawledPages failed: "<<e.what()<<std::endl;
		return false;
	}
}


