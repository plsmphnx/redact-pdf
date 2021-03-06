#include <iostream>
#include <regex>
#include <string>
#include <utility>
#include <vector>

using namespace std;

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QUtil.hh>

// Enum representing the scope at which the redaction will take place
enum scope_t {
    // Redact only the matching text
    s_match,
    // Redact the operator (e.g. Tj) containing the matching text
    s_operator,
    // Redact the text object (BT/ET) containing the matching text
    s_text_object,
    // Redact the graphics state block (q/Q) containing the matching text
    s_graphics_state,
    // Redact the content stream containing the matching text
    s_stream,
    // Redact the page containing the matching text
    s_page
};

// Argument flags used to set scope; index matches enum
const string SCOPE_FLAGS = "motqsp";

// Shorthand for which scopes contain start/end operators, and so can be nested
inline bool nestable(scope_t scope) {
    return scope == s_text_object || scope == s_graphics_state;
}

// Struct to hold the command-line arguments
struct args_t {
    const char *whoami, *regex, *infile, *outfile;
    scope_t scope;
};

// Print usage and exit
void usage(args_t &args) {
    cerr << "Usage: " << args.whoami << " [-" << SCOPE_FLAGS << "] "
         << "regex infile [outfile]" << endl;
    exit(2);
}

// Parse command-line arguments
void parseArgs(int argc, char *argv[], args_t &args) {
    args.whoami = QUtil::getWhoami(argv[0]);
    for (auto i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            args.scope = (scope_t)(SCOPE_FLAGS.find(argv[i][1]));
            if (args.scope == string::npos) {
                usage(args);
            }
        } else if (!args.regex) {
            args.regex = argv[i];
        } else if (!args.infile) {
            args.infile = argv[i];
        } else if (!args.outfile) {
            args.outfile = argv[i];
        } else {
            usage(args);
        }
    }
    if (!args.regex || !args.infile) {
        usage(args);
    }
}

// Class implementing a token filter to identify and remove matches at the
// specified scope; it will handle filtering within the stream and flag
// matches for redaction at a higher scope
class Filter : public QPDFObjectHandle::TokenFilter {
    regex _regex;
    scope_t _scope;
    bool _redact = false;
    bool _trim = false;

    // Each frame of the stack contains the unwritten raw data (which is being
    // stored in case it needs to be redacted in the future), and the collected
    // text to test for redaction
    vector<pair<string, string>> _stack;

    // Add a token to the currently active frame
    void _add(const QPDFTokenizer::Token &token) {
        auto &frame = _stack.back();
        frame.first += token.getRawValue();
        if (token.getType() == QPDFTokenizer::tt_string) {
            frame.second += token.getValue();
        }
        _trim = false;
    }

    // Flush the currently active frame to the next lower frame
    void _flush() {
        auto frame = _stack.back();
        _stack.pop_back();

        // The frame is removed either way, but the data is only added if
        // it is not being redacted
        if (!regex_search(frame.second, _regex)) {
            auto &top = _stack.back();
            top.first += frame.first;
            top.second += frame.second;
        } else {
            // Since the filter is operating on a stream, flag the immediate
            // next whitespace as also requiring redaction
            _redact = _trim = true;
        }
    }

    // Start a new frame if the desired scope matches the expected scope,
    // and add the given token at the beginning
    void _start(scope_t scope, const QPDFTokenizer::Token &token) {
        // Start a new frame only if there is none or the scope is nestable
        if (_scope == scope && (nestable(scope) || _stack.size() == 1)) {
            _stack.push_back({});
        }
        _add(token);
    }

    // Add the given token and flush the current frame if the desired scope
    // matches the expected scope
    void _end(scope_t scope, const QPDFTokenizer::Token &token) {
        _add(token);
        if (_scope == scope && _stack.size() > 1) {
            _flush();
        }
    }

  public:
    Filter(const char *regex, scope_t scope) : _regex(regex), _scope(scope) {
        _stack.push_back({});
    }

    void handleToken(const QPDFTokenizer::Token &token) {
        auto &value = token.getValue();
        switch (token.getType()) {
        case QPDFTokenizer::tt_word:
            // Mark appropriate start/end operators (which have no arguments) or
            // the end of an operator block (which may have arguments)
            if (value == "BT") {
                _start(s_text_object, token);
            } else if (value == "ET") {
                _end(s_text_object, token);
            } else if (value == "q") {
                _start(s_graphics_state, token);
            } else if (value == "Q") {
                _end(s_graphics_state, token);
            } else {
                _end(s_operator, token);
            }
            break;
        case QPDFTokenizer::tt_space:
            // Add the space token if it should not be trimmed immediately
            // following a redaction, then unmark the trimming state
            if (!_trim) {
                _add(token);
            }
            _trim = false;
            break;
        case QPDFTokenizer::tt_string:
            if (_scope == s_match) {
                // For match-scoped redactions, simply replace any matches with
                // an empty string and replace the string token with the result
                auto redacted = regex_replace(value, _regex, "");
                _add(QPDFTokenizer::Token(QPDFTokenizer::tt_string, redacted));
                break;
            }
        default:
            // Any other token may be an argument that needs to be trimmed as
            // part of redacting an operator; since operators can't be nested,
            // marking this repeatedly is safe
            _start(s_operator, token);
            break;
        }
    }

    void handleEOF() {
        // Flush any remaining open frames
        while (_stack.size() > 1) {
            _flush();
        }
    }

    // Get final raw stream data
    const string &data() { return _stack[0].first; }

    // Test final text for redaction
    bool redact() { return _redact || regex_search(_stack[0].second, _regex); }
};

// Get the contents of a page or form XObject
vector<QPDFObjectHandle> getContents(QPDFObjectHandle &obj) {
    if (obj.isPageObject()) {
        return obj.getPageContents();
    } else {
        return vector<QPDFObjectHandle>{obj};
    }
}

// Set the contents of a page (no-op for a form XObject)
void setContents(QPDFObjectHandle &obj, vector<QPDFObjectHandle> &c) {
    if (obj.isPageObject()) {
        obj.replaceKey("/Contents",
                       c.size() == 1 ? c[0] : QPDFObjectHandle::newArray(c));
    }
    // TODO: Figure out what stream-level filters for form XObjects should mean
}

// Redact the contents of a page; return whether to redact the entire page
bool redactPage(args_t &args, QPDFPageObjectHelper &page) {
    auto object = page.getObjectHandle();

    // Loop through each page contents, testing for matches
    vector<QPDFObjectHandle> contents;
    for (auto &obj : getContents(object)) {
        Filter filter(args.regex, args.scope);
        obj.filterAsContents(&filter);
        if (filter.redact()) {
            switch (args.scope) {
            case s_page:
                // For page-scoped redactions, simply bail here
                return true;
            case s_stream:
                // For stream-scoped redactions, omit the stream
                continue;
            default:
                // For all other redactions, update the stream data
                obj.replaceStreamData(filter.data(),
                                      QPDFObjectHandle::newNull(),
                                      QPDFObjectHandle::newNull());
            }
        }
        contents.push_back(obj);
    }
    setContents(object, contents);

    // Iterate through nested form XObjects
    for (auto &entry : page.getFormXObjects()) {
        QPDFPageObjectHelper form(entry.second);
        if (redactPage(args, form)) {
            return true;
        }
    }

    return false;
}

int main(int argc, char *argv[]) {
    args_t args{};
    parseArgs(argc, argv, args);

    try {
        QPDF pdf;
        pdf.processFile(args.infile);

        // Loop through each page, redacting as necessary
        QPDFPageDocumentHelper doc(pdf);
        for (auto &page : doc.getAllPages()) {
            if (redactPage(args, page)) {
                doc.removePage(page);
            }
        }

        // Remove any resources (e.g. fonts) that are no longer used once the
        // desired text has been redacted
        doc.removeUnreferencedResources();

        // If no outfile was provided (indicating an in-place edit), generate
        // a temporary file based on the infile
        auto outfile = args.outfile ? args.outfile : string(args.infile) + "~";

        QPDFWriter writer(pdf, outfile.c_str());
        writer.write();

        if (!args.outfile) {
            // Replace the infile with the temporary file
            pdf.closeInputSource();
            QUtil::remove_file(args.infile);
            QUtil::rename_file(outfile.c_str(), args.infile);
        }
    } catch (exception &e) {
        cerr << args.whoami << ": " << e.what() << endl;
        exit(2);
    }

    return 0;
}
