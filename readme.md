# redact-pdf

This is a simple tool designed to redact text from PDF files in an automated
fashion using regular expressions.

## Usage

```
redact-pdf [-motqsp] regex infile [outfile]
```

- `regex` - The regular expression to redact (using ECMAScript syntax via the
  C++ standard library).
- `infile` - The PDF file from which to redact.
- `outfile` - The new PDF file to write; if not specified, the input file will
  be edited in-place.

### Scope Flags

One of the following flags may be specified in order to control the scope of the
redactions:

- `-m` - Redact only the matching text. This is the default if unspecified.
- `-o` - Redact the operator (e.g. `Tj`) containing the matching text.
- `-t` - Redact the text object (`BT`/`ET`) containing the matching text.
- `-q` - Redact the graphics state block (`q`/`Q`) containing the matching text.
- `-s` - Redact the content stream containing the matching text.
- `-p` - Redact the page containing the matching text.

## Known Limitations

- Text is only matched within its scope; this means that text that crosses the
  boundary of its scope can't be redacted.
- Text across tokens is simply concatenated for purposes of testing for
  redaction; this means that text crossing tokens (e.g. across lines, assuming
  the lines are stored as separate operators) will not have any intervening
  whitespace in the tested string, despite having such visually.
- Stream redaction is not currently well-defined on XObject streams, only page
  content streams.
