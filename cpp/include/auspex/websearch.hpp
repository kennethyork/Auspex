// Looking something up.
//
// The Researcher can read the project and nothing else, so a task that turns on a
// fact it does not have -- which flag this library's version takes, what the error
// message means -- is a task it answers by guessing. A model guessing about an API
// is confident and specific and wrong, which is the worst combination.
//
// OFF UNLESS ASKED FOR. This is the only part of Auspex that reaches the network
// on the crew's behalf, and it sends the query somewhere. A crew that quietly
// posted fragments of your task to a search engine would be a surprise, and a bad
// one, so it is a switch and the switch is off.
//
// DuckDuckGo's HTML endpoint, which needs no key. A provider that needs an API key
// would mean this feature does nothing until you have signed up for something,
// which is the same as not shipping it.
//
// WHAT COMES BACK IS UNTRUSTED TEXT. It is a search result: somebody else wrote
// it, and it goes into a prompt. It is labelled as quoted material where it lands,
// and it never becomes a tool call, a path or a command -- the coder's verb table
// is fixed and nothing here can add to it.
#pragma once

#include <string>
#include <vector>

namespace auspex {

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;

    bool operator==(const SearchHit&) const = default;
};

struct SearchResult {
    bool                  ok = false;
    std::vector<SearchHit> hits;
    std::string           error;

    bool empty() const { return hits.empty(); }
};

// Search the web. Blocking; run it off the GTK thread.
//
// `limit` caps the hits kept. A handful is the useful number: the point is to
// find the one page worth reading, and twenty snippets in a prompt is the context
// budget spent on a list.
SearchResult web_search(const std::string& query, int limit = 5,
                        const std::string& endpoint = {});

// Generic link extraction, for an endpoint that is not DuckDuckGo.
//
// Every result page is anchors with text; this pulls those out and drops the ones
// that are plainly navigation. Cruder than parse_duckduckgo and it works on more
// things, which is the trade -- the specific parser is tried first.
std::vector<SearchHit> parse_links(const std::string& html, int limit = 5);

// Parsing separated from the network, because the shape of somebody else's HTML
// is exactly the thing that changes without warning and must be testable against
// a captured page rather than against the live internet.
std::vector<SearchHit> parse_duckduckgo(const std::string& html, int limit = 5);

// HTML entities a search result actually contains. Not a general decoder: the
// five that matter, plus numeric escapes.
std::string decode_entities(const std::string& text);

// Tags, scripts and styles removed; runs of whitespace collapsed.
std::string strip_html(const std::string& html);

struct FetchedPage {
    bool        ok = false;
    long        status = 0;
    std::string text;    // readable text, capped
    std::string error;
};

// Fetch one page and reduce it to readable text.
//
// Capped hard: a page is arbitrary and this ends up in a prompt, so a megabyte of
// minified script would spend the whole context window on nothing. http and https
// only -- a file:// URL would turn a search result into a local file read.
FetchedPage fetch_page(const std::string& url, std::size_t max_bytes = 20'000);

// The block handed to a model, with the results marked as somebody else's words.
std::string search_note(const std::string& query, const SearchResult& result);

}  // namespace auspex
