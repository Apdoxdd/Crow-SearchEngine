#include "HTML_Parser.hpp"

// ----------------------------------------------------------------------------
// KNOWN LIMITATIONS (not handled yet), most important first.
// Each one has a test below (search "KNOWN BUG") that shows the current behavior.
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
//
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

            // Extract all H1
            else if (xmlStrcasecmp(current->name, BAD_CAST "h1") == 0)
            {
                append_clean_text(current->children, header1_text);
            }

            // Extract all H2
            else if (xmlStrcasecmp(current->name, BAD_CAST "h2") == 0)
            {
                append_clean_text(current->children, header2_text);
            }
        }

        // Extract body text include the title and H1/H2
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
    const std::string& html,
    std::string& body_text,
    std::string& title_text,
    std::string& header1_text,
    std::string& header2_text)
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

    if (!doc) return;

    xmlNode* root = xmlDocGetRootElement(doc);

    if (root)
        collect_text(root,
                     body_text,
                     title_text,
                     header1_text,
                     header2_text);
    xmlFreeDoc(doc);
}


//////////   TESTS  (they only PRINT )

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif



namespace {

struct Case {
    std::string name, html;
    bool has_expected;
    ExpectedSections expected;
};

std::vector<Case>& cases()
{
    static std::vector<Case> c;
    return c;
}

struct Output {
    std::string body, title, h1, h2;
    long long us = 0;    // parse time in microseconds (median of 5 runs)
};

void init_console()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);   // so Arabic prints correctly on Windows
#endif
}

// Runs the parser 5 times and keeps the median time (the first run is often slower).
Output run_parser(const std::string& html)
{
    const int RUNS = 5;
    std::vector<long long> times;
    Output r;
    for (int i = 0; i < RUNS; ++i) {
        r.body.clear(); r.title.clear(); r.h1.clear(); r.h2.clear();
        auto t0 = std::chrono::steady_clock::now();
        extract_html_sections(html, r.body, r.title, r.h1, r.h2);
        auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    }
    std::sort(times.begin(), times.end());
    r.us = times[RUNS / 2];
    return r;
}

// Prints the text exactly as it is. [ ] only mark where it starts and ends.
// The byte count catches things the console hides (like a NUL byte).
void print_raw(const char* label, const std::string& text)
{
    std::cout << label << " (" << text.size() << " bytes) = [" << text << "]\n";
}

// One section: OUTPUT, and below it EXPECTED (only if given).
void print_section(const char* name, const std::string& out,
                   const ExpectedSections* exp, const std::string* exp_field)
{
    std::string l1 = std::string("  ") + name + " OUTPUT  ";
    print_raw(l1.c_str(), out);
    if (exp) {
        std::string l2 = std::string("  ") + name + " EXPECTED";
        print_raw(l2.c_str(), *exp_field);
    }
}

void print_case(const std::string& name, const std::string& html,
                const Output& out, const ExpectedSections* exp)
{
    std::cout << "\n========================================\n"
              << "CASE: " << name << "\n";
    print_raw("  INPUT ", html);
    print_section("body ", out.body,  exp, exp ? &exp->body  : nullptr);
    print_section("title", out.title, exp, exp ? &exp->title : nullptr);
    print_section("h1   ", out.h1,    exp, exp ? &exp->h1    : nullptr);
    print_section("h2   ", out.h2,    exp, exp ? &exp->h2    : nullptr);
    std::cout << "  time  = " << out.us << " us (" << out.us / 1000.0 << " ms)\n";
}

// ----------------------------------------------------------------------------
// Built-in cases
// ----------------------------------------------------------------------------
struct Defaults {
    Defaults()
    {
        // ===== Normal cases: what the parser should give today =====
        add_html_test("basic page",
            "<html><head><title>My Title</title></head>"
            "<body><h1>Main</h1><h2>Sub</h2><p>Hello</p></body></html>",
            {"My Title Main Sub Hello ", "My Title ", "Main ", "Sub "});

        add_html_test("script/style skipped",
            "<html><head><title>T</title><style>x{}</style></head>"
            "<body><script>a()</script><p>Visible</p></body></html>",
            {"T Visible ", "T ", "", ""});

        add_html_test("many h1 / h2",
            "<html><body><h1>One</h1><h1>Two</h1><h2>A</h2><h2>B</h2></body></html>",
            {"One Two A B ", "", "One Two ", "A B "});

        add_html_test("entities",
            "<html><body><p>Tom &amp; Jerry &#65;</p></body></html>",
            {"Tom & Jerry A ", "", "", ""});

        add_html_test("malformed html (unclosed tags)",
            "<p>One<p>Two",
            {"One Two ", "", "", ""});

        add_html_test("empty input", "", {"", "", "", ""});

        add_html_test("english curly quote, no charset",
            "<html><body><p>don\xE2\x80\x99t stop</p></body></html>",
            {"don\xE2\x80\x99t stop ", "", "", ""});

        // NOTE (MSVC): save this file as UTF-8 with BOM, or compile with /utf-8,
        // otherwise the Arabic literal below is mangled before it reaches the parser.
        add_html_test("arabic without charset",
            "<html><body><h1>مرحبا</h1></body></html>",
            {"مرحبا ", "", "مرحبا ", ""});

        // No expected value: just look at the output and the time.
        {
            std::string html = "<html><body>";
            for (int i = 0; i < 2000; ++i) html += "<p>Paragraph " + std::to_string(i) + "</p>";
            html += "</body></html>";
            add_html_test("large doc (2000 paragraphs)", html);
        }

        // ===== Unhandled cases: EXPECTED = the CORRECT result we want one day.
        // OUTPUT is what the parser gives now. The numbers match the list at the top.
        add_html_test("[LIMITATION 1] inline tag inside a word: un<b>believ</b>able",
            "<html><body><p>un<b>believ</b>able</p></body></html>",
            {"unbelievable ", "", "", ""});

        add_html_test("[LIMITATION 1] inline tag inside h1 gives double spaces",
            "<html><body><h1>Hello <b>big</b> world</h1></body></html>",
            {"Hello big world ", "", "Hello big world ", ""});

        add_html_test("[LIMITATION 2] menu/footer text in body",
            "<html><body><nav>Home About</nav><p>Real content</p><footer>Copyright</footer></body></html>",
            {"Real content ", "", "", ""});

        {
            std::string html = "<html><body>";
            for (int i = 0; i < 300; ++i) html += "<div>";
            html += "deep text";
            for (int i = 0; i < 300; ++i) html += "</div>";
            html += "</body></html>";
            add_html_test("[LIMITATION 3] 300 nested divs", html,
                          {"deep text ", "", "", ""});
        }

        add_html_test("[LIMITATION 6] NUL byte inside text", std::string("<p>A\0B</p>", 10),
            {"AB ", "", "", ""});
    }
} defaults;

} // namespace

// ----------------------------------------------------------------------------
// Functions declared in HTML_Parser.hpp
// ----------------------------------------------------------------------------
void add_html_test(const std::string& name, const std::string& html)
{
    cases().push_back({name, html, false, {}});
}

void add_html_test(const std::string& name, const std::string& html,
                   const ExpectedSections& expected)
{
    cases().push_back({name, html, true, expected});
}

void run_html_tests()
{
    init_console();
    std::cout << "\n=== extract_html_sections: " << cases().size() << " cases ===\n";

    for (const Case& c : cases())
        print_case(c.name, c.html, run_parser(c.html), c.has_expected ? &c.expected : nullptr);

    std::cout << "\n=== end ===\n\n";
}

// Run ANY html string now and print input, outputs and parse time.
void show_html(const std::string& name, const std::string& html)
{
    init_console();
    print_case(name, html, run_parser(html), nullptr);
}

// Same, but the html comes from a file on disk.
bool show_html_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);   // binary = bytes are not changed
    if (!f) {
        std::cout << "Cannot open file: " << path << "\n";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    show_html(path, ss.str());
    return true;
}
