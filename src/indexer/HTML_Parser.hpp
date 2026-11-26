#ifndef HTML_PARSER_HPP
#define HTML_PARSER_HPP

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <string>

void extract_html_sections(
    const std::string& html ,
    std::string& body_text,
    std::string& title_text,
    std::string& header1_text,
    std::string& header2_text );
#endif