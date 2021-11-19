#include <iostream>
#include <regex>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QUtil.hh>

enum scope_t {
    s_match,
    s_operator,
    s_text_object,
    s_graphics_state,
    s_stream,
    s_page
};

inline bool nestable(scope_t scope) {
    return scope == s_text_object || scope == s_graphics_state;
}

struct args_t {
    const char *whoami, *regex, *infile, *outfile;
    scope_t scope;
};

void usage(args_t &args) {
    std::cerr << "Usage: " << args.whoami << " [-psqtom] regex infile [outfile]"
              << std::endl;
    exit(2);
}

void parseArgs(int argc, char *argv[], args_t &args) {
    args.whoami = QUtil::getWhoami(argv[0]);
    for (auto i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            switch (argv[i][1]) {
            case 'p':
                args.scope = s_page;
                break;
            case 's':
                args.scope = s_stream;
                break;
            case 'q':
                args.scope = s_graphics_state;
                break;
            case 't':
                args.scope = s_text_object;
                break;
            case 'o':
                args.scope = s_operator;
                break;
            case 'm':
                args.scope = s_match;
                break;
            default:
                usage(args);
                break;
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

class Filter : public QPDFObjectHandle::TokenFilter {
  private:
    std::regex _regex;
    scope_t _scope;
    bool *_redact;

    struct buffer {
        bool redact;
        std::vector<std::string> data;
    };
    std::vector<buffer> _stack;
    bool _trim = false;

    void _add(const std::string &raw) {
        if (_stack.size() > 0) {
            _stack.back().data.push_back(raw);
        } else {
            write(raw);
        }
    }

    void _add(QPDFTokenizer::Token const &token) {
        _add(token.getRawValue());
        _trim = false;
    }

    void _flush() {
        auto buf = _stack.back();
        _stack.pop_back();
        if (!buf.redact) {
            for (auto token : buf.data) {
                _add(token);
            }
        }
        _trim = buf.redact;
    }

    void _start(scope_t scope, QPDFTokenizer::Token const &token) {
        if (_scope == scope && (nestable(scope) || _stack.size() == 0)) {
            _stack.push_back(buffer{false});
        }
        _add(token);
    }

    void _end(scope_t scope, QPDFTokenizer::Token const &token) {
        _add(token);
        if (_scope == scope && _stack.size() > 0) {
            _flush();
        }
    }

  public:
    Filter(const char *const regex, scope_t scope, bool *redact)
        : _regex(regex), _scope(scope), _redact(redact) {}

    void handleToken(QPDFTokenizer::Token const &token) {
        auto &value = token.getValue();
        switch (token.getType()) {
        case QPDFTokenizer::tt_word:
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
        case QPDFTokenizer::tt_string:
            if (_scope == s_match) {
                auto redacted = regex_replace(value, _regex, "");
                _add(QPDFTokenizer::Token(QPDFTokenizer::tt_string, redacted)
                         .getRawValue());
            } else {
                _start(s_operator, token);
                if (regex_search(value, _regex)) {
                    if (_stack.size() > 0) {
                        _stack.back().redact = true;
                    } else {
                        *_redact = true;
                    }
                }
            }
            break;
        case QPDFTokenizer::tt_space:
            if (!_trim) {
                _add(token);
            }
            _trim = false;
            break;
        default:
            _start(s_operator, token);
            break;
        }
    }

    void handleEOF() {
        while (_stack.size() > 0) {
            _flush();
        }
    }
};

std::vector<QPDFObjectHandle> getContents(QPDFObjectHandle &obj) {
    if (obj.isPageObject()) {
        return obj.getPageContents();
    } else {
        return std::vector<QPDFObjectHandle>{obj};
    }
}

void setContents(QPDFObjectHandle &obj, std::vector<QPDFObjectHandle> &c) {
    if (obj.isPageObject()) {
        auto contents = c.size() == 1 ? c.at(0) : QPDFObjectHandle::newArray(c);
        obj.replaceKey("/Contents", contents);
    }
}

bool redactPage(args_t &args, QPDFPageObjectHelper &page) {
    auto object = page.getObjectHandle();
    std::vector<QPDFObjectHandle> contents;

    for (auto &obj : getContents(object)) {
        auto redact = false;
        Filter f(args.regex, args.scope, &redact);
        obj.filterAsContents(&f);
        if (redact) {
            if (args.scope >= s_page) {
                return true;
            } else if (args.scope < s_stream) {
                contents.push_back(obj);
            }
        } else {
            contents.push_back(obj);
        }
    }
    setContents(object, contents);

    {
        auto redact = false;
        page.addContentTokenFilter(new Filter(args.regex, args.scope, &redact));
    }

    for (auto &entry : page.getFormXObjects()) {
        auto form = QPDFPageObjectHelper(entry.second);
        if (redactPage(args, form)) {
            return true;
        }
    }

    return false;
}

int main(int argc, char *argv[]) {
    args_t args = {0};
    parseArgs(argc, argv, args);

    try {
        QPDF pdf;
        pdf.processFile(args.infile);

        auto doc = QPDFPageDocumentHelper(pdf);
        for (auto &page : doc.getAllPages()) {
            if (redactPage(args, page)) {
                doc.removePage(page);
            }
        }
        doc.removeUnreferencedResources();

        std::string outfile =
            args.outfile ? args.outfile : std::string(args.infile) + "~";

        QPDFWriter w(pdf, outfile.c_str());
        w.write();

        if (!args.outfile) {
            pdf.closeInputSource();
            QUtil::remove_file(args.infile);
            QUtil::rename_file(outfile.c_str(), args.infile);
        }
    } catch (std::exception &e) {
        std::cerr << args.whoami << ": " << e.what() << std::endl;
        exit(2);
    }

    return 0;
}
