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


database::database(const std::string& dbPath, const std::string& schemaPath)
    : _db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
{
	_db.exec("PRAGMA foreign_keys = ON;");
	_db.exec("PRAGMA journal_mode = WAL;");
	_db.exec("PRAGMA busy_timeout = 5000;");

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
				"Failed to read databse schema: " + schemaPath 
				);
	}

	SQLite::Transaction transaction(_db);

	_db.exec(buffer.str());
	transaction.commit();



}

bool database::saveCrawledPages(
		const std::vector<html_url>& html_urls,
		const std::vector<page_links>& extracted_liks
		)
{
	try 
	{
		// One transaction for the entire batch
		SQLite::Transaction transaction(_db);


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
            		"isCrawled = 1 "
            		"RETURNING id;"
        	);

		// mark a url that had its content the same as another as crawled so we dont keep on crawling it over and over 
        	SQLite::Statement markCrawled(
            		_db,
            		"UPDATE frontier SET isCrawled = 1 WHERE url = ?;"
        	);

		// what does it do?
        	SQLite::Statement insertUrl(
        		_db,
            		"INSERT INTO frontier (url) VALUES (?) "
            		"ON CONFLICT(url) DO NOTHING "
            		"RETURNING id;"
        	);



		transaction.commit();
		return true;
	}
	catch (const std::exception& e)
	{
		std::cerr<<"saveCrawledPages failed: "<<e.what()<<std::endl;
		return false;
	}
}


