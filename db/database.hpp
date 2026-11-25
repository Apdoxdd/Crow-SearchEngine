#pragma once

#include <SQLiteCpp/SQLiteCpp.h>
#include <string>

class database 
{
private:
	SQLite::Database _db;

public:
	explicit database(
			const std::string& dbPath = "db/search_engine.db",
			const std::string& schemaPath = "db/schema.sql"
	       ); // prevetns implicit construction of a databse obj from a string


};
