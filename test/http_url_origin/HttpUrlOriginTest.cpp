#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "UrlOrigin.h"

// Regression coverage for the origin-comparison logic HttpDownloader's
// redirect loop uses to decide whether to strip Authorization/X-Device-Serial
// headers. Exercised on real hardware as the OTA GitHub -> release-CDN hop
// (see docs/ota-signing-key-management.md and the redirect loop in
// src/network/HttpDownloader.cpp) -- a real device hit a TLS handshake
// failure immediately after this code ran, traced to heap fragmentation from
// an extra allocation this function's std::string-returning predecessor
// caused on every redirect hop. urlOrigin() was moved out of
// HttpDownloader.cpp into this header specifically so its behavior can be
// pinned down independent of ESP-IDF/Arduino, matching a std::string-based
// reference implementation kept here to guarantee refactor-safety.

namespace {
// The exact behavior urlOrigin() replaced: a std::string-allocating version.
// Kept here, not in production code, purely so every case below can assert
// both implementations agree -- if this ever drifts from urlOrigin(), the
// heap-free rewrite silently changed origin-comparison semantics.
std::string referenceUrlOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return url;
  const size_t pathStart = url.find_first_of("/?#", schemeEnd + 3);
  return pathStart == std::string::npos ? url : url.substr(0, pathStart);
}
}  // namespace

TEST(UrlOrigin, ExtractsSchemeHostFromPathUrl) {
  EXPECT_EQ(urlOrigin("https://github.com/sfoulad/midad-by-foulad/releases/download/v1/firmware.bin"),
            "https://github.com");
}

TEST(UrlOrigin, ExtractsSchemeHostFromCdnRedirectTarget) {
  // The actual redirect target class that failed on real hardware: GitHub's
  // release asset CDN, a different host than github.com.
  EXPECT_EQ(urlOrigin("https://objects.githubusercontent.com/github-production-release-asset/1/firmware.bin?X-Amz="
                      "abc"),
            "https://objects.githubusercontent.com");
}

TEST(UrlOrigin, KeepsPortInOrigin) {
  EXPECT_EQ(urlOrigin("http://192.168.1.50:8080/opds/root.xml"), "http://192.168.1.50:8080");
}

TEST(UrlOrigin, NoPathStopsAtQueryString) {
  // A URL with no '/' at all after the scheme -- stopping at '/' alone would
  // fold the query string into the "origin" and misclassify a same-origin
  // redirect as cross-origin. This is the exact edge case the function's own
  // header comment calls out.
  EXPECT_EQ(urlOrigin("https://host?key=value"), "https://host");
}

TEST(UrlOrigin, NoPathStopsAtFragment) { EXPECT_EQ(urlOrigin("https://host#section"), "https://host"); }

TEST(UrlOrigin, BareOriginUnchanged) { EXPECT_EQ(urlOrigin("https://midad.one"), "https://midad.one"); }

TEST(UrlOrigin, MissingSchemeReturnsWholeInput) { EXPECT_EQ(urlOrigin("not-a-url"), "not-a-url"); }

TEST(UrlOrigin, SameOriginDifferentPathsCompareEqual) {
  EXPECT_EQ(urlOrigin("https://midad.one/opds/a.xml"), urlOrigin("https://midad.one/opds/b.xml"));
}

TEST(UrlOrigin, CrossOriginComparesUnequal) {
  EXPECT_NE(urlOrigin("https://github.com/owner/repo/releases/download/v1/firmware.bin"),
            urlOrigin("https://objects.githubusercontent.com/github-production-release-asset/1/firmware.bin"));
}

// Refactor-safety: the heap-free std::string_view implementation must never
// disagree with the std::string-returning implementation it replaced, across
// every case above plus a few more edge shapes.
TEST(UrlOrigin, MatchesReferenceImplementationAcrossShapes) {
  const char* const cases[] = {
      "https://github.com/sfoulad/midad-by-foulad/releases/download/v1/firmware.bin",
      "https://objects.githubusercontent.com/github-production-release-asset/1/firmware.bin?X-Amz=abc",
      "http://192.168.1.50:8080/opds/root.xml",
      "https://host?key=value",
      "https://host#section",
      "https://midad.one",
      "not-a-url",
      "",
      "https://",
  };
  for (const char* const c : cases) {
    EXPECT_EQ(std::string(urlOrigin(c)), referenceUrlOrigin(c)) << "mismatch for input: " << c;
  }
}

// --- RedirectOriginGuard -----------------------------------------------------
//
// The rule that keeps a device token off a third party's server when a cover or
// download link redirects. runGetWolf() asks credentialsAllowed() before EVERY
// request in a chain and sets or clears Basic auth from the answer, so these
// cases are the security boundary itself, not a helper's convenience.

namespace {

// Walks a chain the way runGetWolf() does -- ask before each request, advance on
// each redirect -- and reports whether credentials went out on the final request.
bool credentialsSurviveChain(const std::string& start, const std::vector<std::string>& hops) {
  RedirectOriginGuard guard(start);
  bool allowedOnLastRequest = guard.credentialsAllowed();
  for (const std::string& hop : hops) {
    guard.onHop(hop);
    allowedOnLastRequest = guard.credentialsAllowed();
  }
  return allowedOnLastRequest;
}

}  // namespace

TEST(RedirectOriginGuard, AllowsCredentialsOnTheFirstRequest) {
  const RedirectOriginGuard guard("https://api.midad.one/opds/cover/1.jpg");
  EXPECT_TRUE(guard.credentialsAllowed());
  EXPECT_FALSE(guard.leftStartOrigin());
}

TEST(RedirectOriginGuard, SameOriginRedirectKeepsCredentials) {
  // A signed cover URL that redirects within the catalogue's own origin is the
  // case keep-alive exists for; stripping here would break authenticated covers.
  EXPECT_TRUE(credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg",
                                      {"https://api.midad.one/media/signed/1.jpg?sig=abc"}));
}

TEST(RedirectOriginGuard, DifferentHostDropsCredentials) {
  // The R2/CDN shape: the token must not reach object storage.
  EXPECT_FALSE(credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg",
                                       {"https://pub-example.r2.dev/covers/1.jpg?X-Amz-Signature=abc"}));
}

TEST(RedirectOriginGuard, SubdomainChangeDropsCredentials) {
  EXPECT_FALSE(
      credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg", {"https://cdn.midad.one/covers/1.jpg"}));
}

TEST(RedirectOriginGuard, LegacyHostRedirectDropsCredentials) {
  // foulad.one redirects to midad.one server-side for devices paired before the
  // rename (FouladEbooksConfig.h) -- a real cross-origin hop this firmware takes.
  EXPECT_FALSE(credentialsSurviveChain("http://foulad.one/opds/cover/1.jpg", {"http://midad.one/opds/cover/1.jpg"}));
}

TEST(RedirectOriginGuard, SchemeDowngradeDropsCredentialsOnTheSameHost) {
  // Same host, different scheme: still a different origin. Sending Basic auth
  // over the http hop would put the token on the wire in clear.
  EXPECT_FALSE(credentialsSurviveChain("https://midad.one/opds/cover/1.jpg", {"http://midad.one/opds/cover/1.jpg"}));
}

TEST(RedirectOriginGuard, PortChangeDropsCredentialsOnTheSameHost) {
  EXPECT_FALSE(credentialsSurviveChain("http://192.168.1.50:8080/opds/cover/1.jpg",
                                       {"http://192.168.1.50:9090/opds/cover/1.jpg"}));
}

TEST(RedirectOriginGuard, ReturningToTheStartOriginDoesNotRestoreCredentials) {
  // The latch. A server that can redirect can redirect A -> B -> A; if the guard
  // reset on the way back, the token would ride the final hop after an attacker
  // had already had the chain in hand.
  EXPECT_FALSE(credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg",
                                       {"https://evil.example/x", "https://api.midad.one/opds/cover/1.jpg"}));
}

TEST(RedirectOriginGuard, LatchSurvivesManyLaterSameOriginHops) {
  EXPECT_FALSE(credentialsSurviveChain(
      "https://api.midad.one/a",
      {"https://evil.example/x", "https://api.midad.one/b", "https://api.midad.one/c", "https://api.midad.one/d"}));
}

TEST(RedirectOriginGuard, PathOnlyRedirectKeepsCredentials) {
  EXPECT_TRUE(credentialsSurviveChain("https://api.midad.one/opds/root.xml",
                                      {"https://api.midad.one/opds/v2/root.xml", "https://api.midad.one/opds/v2/a"}));
}

TEST(RedirectOriginGuard, QueryOnlyRedirectKeepsCredentials) {
  // urlOrigin() stops at '?', so a signature appended to the same origin is not
  // mistaken for a cross-origin hop.
  EXPECT_TRUE(credentialsSurviveChain("https://api.midad.one/opds/cover/1.jpg",
                                      {"https://api.midad.one/opds/cover/1.jpg?sig=abc"}));
}

TEST(RedirectOriginGuard, DefaultPortSpellingFailsSafe) {
  // "https://h" and "https://h:443" are the same origin in principle but differ
  // textually. Documented behaviour: strip. The failure direction must always be
  // an unnecessary strip, never an unintended send.
  EXPECT_FALSE(credentialsSurviveChain("https://api.midad.one/a", {"https://api.midad.one:443/a"}));
}

TEST(RedirectOriginGuard, LeftStartOriginTracksTheLatch) {
  RedirectOriginGuard guard("https://api.midad.one/a");
  EXPECT_FALSE(guard.leftStartOrigin());
  guard.onHop("https://api.midad.one/b");
  EXPECT_FALSE(guard.leftStartOrigin());
  guard.onHop("https://elsewhere.example/c");
  EXPECT_TRUE(guard.leftStartOrigin());
  guard.onHop("https://api.midad.one/d");
  EXPECT_TRUE(guard.leftStartOrigin());
}
