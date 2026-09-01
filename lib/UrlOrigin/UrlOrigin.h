#pragma once
#include <string>
#include <string_view>

// scheme://host[:port] prefix, i.e. everything before the first '/', '?', or
// '#' after the scheme separator (a URL with no path but a query/fragment --
// e.g. "https://host?key=value" -- has no '/' at all, so stopping at '/'
// alone would fold the query string into the "origin" and misclassify a
// same-origin redirect as cross-origin). Used to decide whether a redirect
// crosses trust boundaries -- see the Authorization/X-Device-Serial-stripping
// logic in HttpDownloader.cpp's runGet() and runGetWolf().
//
// A view into the caller's own buffer -- allocates nothing. Deliberately so:
// this runs on every redirect hop, including ones immediately before a TLS
// handshake that needs a large contiguous heap block for RSA signature
// verification, and an extra allocation in that exact window is itself a
// fragmentation risk. Never pass the result across a C API boundary (see
// CLAUDE.md's string_view/null-termination rule) -- every call site only
// ever compares two of these with `==`.
inline std::string_view urlOrigin(std::string_view url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return url;
  const size_t pathStart = url.find_first_of("/?#", schemeEnd + 3);
  return pathStart == std::string_view::npos ? url : url.substr(0, pathStart);
}

// The rule "credentials never cross an origin boundary", for a redirect chain --
// written down once, and testable on a host, because getting it wrong hands a
// device token to whoever controls the redirect target.
//
// Construct with the URL the chain starts at, call onHop() with each redirect
// target as the chain advances, and ask credentialsAllowed() before every
// request. It answers for the request that is ABOUT to be sent, so the caller
// must set or clear its Authorization from it on every hop, not only the first.
//
// Latching is the point. Once a chain has left its starting origin the answer is
// no for the remainder of that chain, even if a later hop comes back. A server
// that can redirect can redirect A -> B -> A, and restoring credentials on the
// way back would hand them to whoever chose the timing of that last hop.
//
// "Same origin" is scheme + host + port -- exactly what urlOrigin() returns, not
// the host alone. An https -> http downgrade on the same host is a different
// origin and drops credentials, which is intended. Two spellings of one origin
// that differ textually ("https://h" vs "https://h:443") compare unequal and so
// drop credentials too: the failure direction is an unnecessary strip, never an
// unintended send.
//
// Holds a view into the caller's buffer and allocates nothing (see urlOrigin
// above). The start URL must outlive the guard -- in particular it must not be
// the same variable the caller rewrites as the chain advances.
class RedirectOriginGuard {
 public:
  explicit RedirectOriginGuard(std::string_view startUrl) : startOrigin_(urlOrigin(startUrl)) {}
  // Exact match for a string literal, which would otherwise be ambiguous between
  // the view above and the deleted overload below.
  explicit RedirectOriginGuard(const char* startUrl) : startOrigin_(urlOrigin(startUrl ? startUrl : "")) {}
  // A temporary would leave startOrigin_ dangling on the first comparison.
  explicit RedirectOriginGuard(std::string&&) = delete;

  void onHop(std::string_view url) {
    if (urlOrigin(url) != startOrigin_) leftStartOrigin_ = true;
  }

  bool credentialsAllowed() const { return !leftStartOrigin_; }
  bool leftStartOrigin() const { return leftStartOrigin_; }

 private:
  std::string_view startOrigin_;
  bool leftStartOrigin_ = false;
};
