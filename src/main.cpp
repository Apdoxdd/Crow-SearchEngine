
#include "HTML_Parser.hpp"
#include <iostream>
#include <curl/curl.h>
#include <libxml/xmlversion.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <libxml/HTMLparser.h>

int main() {
    std::cout << "libcurl:   " << curl_version() << '\n';
    std::cout << "libxml2:   " << LIBXML_DOTTED_VERSION << '\n';
    std::cout << "SQLiteCpp: " << SQLITECPP_VERSION << " (SQLite " << SQLite::getLibVersion() << ")\n";
    return 0;
}
