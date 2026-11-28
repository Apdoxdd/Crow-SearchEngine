#ifndef HTML_PARSER_HPP
#define HTML_PARSER_HPP

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <string>


void extract_html_sections(
    const std::string& html,
    std::string& body_text,
    std::string& title_text,
    std::string& header1_text,
    std::string& header2_text);

//////////   TESTS  (they only PRINT )
// Quick test for extract_html_sections().

// NOTE: whitespace is NOT handled. Strings are compared exactly
// (the parser adds one ' ' after every text node, so expected values need it too).

// Test helpers (implemented at the bottom of HTML_Parser.cpp)
//
// The test NEVER decides pass/fail. It only prints, for every case:
//   INPUT, the parser OUTPUT, the EXPECTED value (if you gave one), and the time.
// You compare OUTPUT and EXPECTED yourself.
//
//   int main() {
//       // case with an expected value: prints input, output, expected, time
//       add_html_test("mine", "<h1>Hi</h1>", {"Hi ", "", "Hi ", ""});
//
//       // case without an expected value: prints input, output, time
//       add_html_test("mine2", "<h1>Hi</h1>");
//
//       run_html_tests();                  // prints all saved cases + built-in ones
//       show_html("name", "<p>Hi</p>");    // runs this html NOW and prints it
//       show_html_file("C:/pages/a.html"); // same, html read from a file
//       return 0;
//   }
//
// Add your own cases BEFORE run_html_tests().
// Outputs are printed raw, nothing is changed.
// ----------------------------------------------------------------------------
struct ExpectedSections {
    std::string body, title, h1, h2;
};

void add_html_test(const std::string& name, const std::string& html);
void add_html_test(const std::string& name, const std::string& html,
                   const ExpectedSections& expected);

void run_html_tests();
void show_html(const std::string& name, const std::string& html);
bool show_html_file(const std::string& path);   // false if the file can't be opened

#endif