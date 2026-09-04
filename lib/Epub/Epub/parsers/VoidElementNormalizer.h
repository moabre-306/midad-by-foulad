#pragma once

#include <cstddef>
#include <cstring>

// Closes unclosed HTML void elements so an XHTML document that a browser would
// accept can still be parsed as XML.
//
// EPUB requires XHTML, i.e. well-formed XML, but real-world content is routinely
// produced by HTML-shaped tooling that writes <br> rather than <br/>. expat then
// reports "mismatched tag" and the whole section fails. Measured on two Midad
// catalogue books, both failing identically at line 8 with 13 <br> and 1 <hr>
// between them, which points at one conversion pipeline rather than one bad file.
//
// This is deliberately NOT general HTML tolerance. Void elements are the one class
// where a missing "/" is unambiguous: the spec says they can never have children,
// so <br> and <br/> denote exactly the same node and closing it changes no meaning.
// Every other malformed shape -- crossed nesting, a stray '<', an unclosed <p> --
// still fails exactly as before. Nothing here weakens validation; it canonicalises
// a closed, known-safe set.
//
// It matters beyond the catalogue: a server-side repair cannot reach a book the
// user sideloaded over SD or the web-upload page, and cannot reach a catalogue book
// a device already downloaded (same URL, same filename, the existence check wins).
// Those only get fixed here.
//
// Pure and free of Arduino/ESP-IDF types so test/void_element_normalizer exercises
// the exact code the firmware links.
namespace void_elements {

// Longest tag this will buffer across a read boundary. A "<" with no ">" within
// this many bytes is passed through untouched rather than held forever.
inline constexpr size_t MAX_CARRY = 64;

// Worst case growth: "<br>" (4 bytes) -> "<br/>" (5), so one extra byte per four.
inline constexpr size_t growthBound(const size_t inLen) { return inLen / 4 + 1; }

inline bool isNameChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

inline bool isVoidName(const char* name, const size_t len) {
  static const char* const kVoid[] = {"area", "base", "br",    "col",    "embed", "hr",  "img",
                                      "link", "meta", "param", "source", "track", "wbr", "input"};
  for (const char* v : kVoid) {
    const size_t vlen = strlen(v);
    if (vlen != len) continue;
    size_t i = 0;
    for (; i < len; ++i) {
      char a = name[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (a != v[i]) break;
    }
    if (i == len) return true;
  }
  return false;
}

/**
 * Copies [in, in+inLen) to out, inserting "/" before ">" on any unclosed void
 * element. `carry` holds a tag fragment split across chunk boundaries: pass the
 * same buffer (and its length) back on the next call. outCap must be at least
 * inLen + MAX_CARRY + growthBound(inLen + MAX_CARRY).
 *
 * Returns bytes written to out. `done` flushes any carried fragment verbatim.
 */
inline size_t normalize(const char* in, const size_t inLen, char* out, const size_t outCap, char* carry,
                        size_t& carryLen, const bool done) {
  size_t o = 0;
  const auto emit = [&](const char c) {
    if (o < outCap) out[o++] = c;
  };

  // A tag held from the previous chunk is completed against this one.
  size_t i = 0;
  if (carryLen > 0) {
    while (i < inLen && carryLen < MAX_CARRY) {
      const char c = in[i++];
      carry[carryLen++] = c;
      if (c == '>') break;
    }
    const bool complete = carryLen > 0 && carry[carryLen - 1] == '>';
    if (!complete && !done && carryLen < MAX_CARRY) return 0;  // still waiting
    // Re-run the completed (or abandoned) fragment through the scanner below by
    // emitting it via a nested call on a zero-carry state.
    size_t inner = 0;
    char none[1];
    size_t noneLen = 0;
    inner = normalize(carry, carryLen, out, outCap, none, noneLen, true);
    o = inner;
    carryLen = 0;
  }

  while (i < inLen) {
    const char c = in[i];
    if (c != '<') {
      emit(c);
      i++;
      continue;
    }

    // Find this tag's '>'.
    size_t j = i + 1;
    while (j < inLen && in[j] != '>') j++;
    if (j >= inLen) {
      // Incomplete tag. Hold it for the next chunk unless it is absurdly long or
      // this is the final chunk, in which case pass it through unchanged.
      const size_t rem = inLen - i;
      if (!done && rem <= MAX_CARRY) {
        memcpy(carry, in + i, rem);
        carryLen = rem;
        return o;
      }
      for (size_t k = i; k < inLen; ++k) emit(in[k]);
      return o;
    }

    // Only a start tag whose name is a void element is touched. Closing tags,
    // comments, CDATA, doctypes and processing instructions are copied verbatim.
    const char first = in[i + 1];
    bool rewrite = false;
    if (isNameChar(first) && first != '/' && first != '!' && first != '?') {
      size_t n = i + 1;
      while (n < j && isNameChar(in[n])) n++;
      const size_t nameLen = n - (i + 1);
      if (isVoidName(in + i + 1, nameLen)) {
        // Already self-closing? Then leave it alone.
        size_t last = j - 1;
        while (last > i && (in[last] == ' ' || in[last] == '\t' || in[last] == '\n' || in[last] == '\r')) last--;
        rewrite = in[last] != '/';
      }
    }

    for (size_t k = i; k < j; ++k) emit(in[k]);
    if (rewrite) emit('/');
    emit('>');
    i = j + 1;
  }
  return o;
}

}  // namespace void_elements
