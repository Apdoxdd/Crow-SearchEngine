#include "HTML_Parser.hpp"

// ----------------------------------------------------------------------------
// KNOWN LIMITATIONS (not handled yet), most important first.
// Each one has a test in test_HTML_Parser.cpp that shows the current behavior.
//
//  1. Words split by inline tags: "un<b>believ</b>able" -> "un believ able"
//     (a ' ' is added after every text node). Hurts search. Fix: add the
//     space only at block-level tags (p, div, h1, li, br, td ...).
//  2. Page/menu/footer/cookie-banner text goes into body_text with the real
//     content (no <nav>, <header>, <footer>, <aside> filtering).
//  3. Deep nesting (> ~256 levels): libxml2 stops parsing, the page text is
//     lost. Fix: HTML_PARSE_HUGE + our own depth limit (the recursion below
//     can overflow the stack on very deep pages).
//  4. Charset is forced to UTF-8: pages in windows-1252 / windows-1256 come out
//     garbled. Fix: pass the charset from the HTTP Content-Type header.
//  5. Whitespace is not cleaned: newlines/indentation from the source stay.
//  6. A NUL byte ('\0') in the page becomes a corrupted character.
//  7. body_text also contains the title/h1/h2 text (it is repeated).
//  8. <title> text containing tags and multiple <title> are joined together.
//  9. Not extracted: alt/title attributes, <noscript>, <iframe>, <template>.
// ----------------------------------------------------------------------------

static void append_clean_text(xmlNode* node, std::string& output)
{
    for (xmlNode* current = node;
         current != nullptr;
         current = current->next)
    {
        // Skip script and style completely
        if (current->type == XML_ELEMENT_NODE)
        {
            if (xmlStrcasecmp(current->name, BAD_CAST "script") == 0 ||
                xmlStrcasecmp(current->name, BAD_CAST "style") == 0)
            {
                continue;
            }
        }

        // Extract text nodes
        if (current->type == XML_TEXT_NODE ||
            current->type == XML_CDATA_SECTION_NODE)
        {
            if (current->content != nullptr)
            {
                output += reinterpret_cast<const char*>(current->content);
                output += ' ';
            }
        }

        // Continue recursively into children
        append_clean_text(current->children, output);
    }
}

static void collect_text(
    xmlNode* node,
    std::string& body_text,
    std::string& title_text,
    std::string& header1_text,
    std::string& header2_text)
{
    for (xmlNode* current = node;
         current != nullptr;
         current = current->next)
    {
        if (current->type == XML_ELEMENT_NODE)
        {
            // Skip script and style completely
            if (xmlStrcasecmp(current->name, BAD_CAST "script") == 0 ||
                xmlStrcasecmp(current->name, BAD_CAST "style") == 0)
            {
                continue;
            }

            // Extract title
            if (xmlStrcasecmp(current->name, BAD_CAST "title") == 0)
            {
                append_clean_text(current->children, title_text);
            }

            // Extract H1
            else if (xmlStrcasecmp(current->name, BAD_CAST "h1") == 0)
            {
                append_clean_text(current->children, header1_text);
            }

            // Extract H2
            else if (xmlStrcasecmp(current->name, BAD_CAST "h2") == 0)
            {
                append_clean_text(current->children, header2_text);
            }

        }

        // Extract body text
        if (current->type == XML_TEXT_NODE ||
            current->type == XML_CDATA_SECTION_NODE)
        {
            if (current->content != nullptr)
            {
                body_text += reinterpret_cast<const char*>(current->content);
                body_text += ' ';
            }
        }

        // Recursive traversal
        collect_text(
            current->children,
            body_text,
            title_text,
            header1_text,
            header2_text);
    }
}

void extract_html_sections(
    const std::string& html ,
    std::string& body_text,
    std::string& title_text,
    std::string& header1_text,
    std::string& header2_text )
    {
        // Force UTF-8 when the page doesn't declare a charset. Without it libxml2
        // guesses Latin-1, so English text with  ’ “ ” – é  gets corrupted:
        // "don’t" becomes "donâ€™t" and a search for "don't" finds nothing.
        htmlDocPtr doc = htmlReadMemory(
        html.data(),
        static_cast<int>(html.size()),
        nullptr,
        "UTF-8",
        HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
        HTML_PARSE_NOWARNING | HTML_PARSE_NONET
    );

    if(!doc) return ;

    xmlNode* root = xmlDocGetRootElement(doc);

    if(root) 
    collect_text(root,
                 body_text, 
                 title_text,
                 header1_text, 
                 header2_text);
    xmlFreeDoc(doc);
    }