#include "database.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>



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
