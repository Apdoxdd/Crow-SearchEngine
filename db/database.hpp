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
	std::string contentHash;
};

struct page_links 
{
	std::string url;
	std::vector<std::string> links;
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
			);


};
