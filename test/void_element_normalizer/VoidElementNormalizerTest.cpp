#include <gtest/gtest.h>

#include <string>

#include "VoidElementNormalizer.h"

namespace {

// Runs the whole input through in one chunk.
std::string once(const std::string& in) {
  std::string out(in.size() + void_elements::MAX_CARRY + void_elements::growthBound(in.size() + 64) + 16, '\0');
  char carry[void_elements::MAX_CARRY];
  size_t carryLen = 0;
  const size_t n = void_elements::normalize(in.data(), in.size(), out.data(), out.size(), carry, carryLen, true);
  return out.substr(0, n);
}

// Feeds the input in fixed-size chunks, exercising the carry path.
std::string chunked(const std::string& in, const size_t chunk) {
  std::string result;
  char carry[void_elements::MAX_CARRY];
  size_t carryLen = 0;
  for (size_t off = 0; off < in.size(); off += chunk) {
    const size_t len = std::min(chunk, in.size() - off);
    const bool done = off + len >= in.size();
    std::string out(len + void_elements::MAX_CARRY + void_elements::growthBound(len + 64) + 16, '\0');
    const size_t n = void_elements::normalize(in.data() + off, len, out.data(), out.size(), carry, carryLen, done);
    result += out.substr(0, n);
  }
  return result;
}

// --- the defect this exists for ---------------------------------------------

TEST(VoidElementNormalizer, ClosesTheUnclosedBrThatBrokeRealBooks) {
  EXPECT_EQ(once("<p>a<br>b</p>"), "<p>a<br/>b</p>");
}

TEST(VoidElementNormalizer, ClosesHr) { EXPECT_EQ(once("<div><hr></div>"), "<div><hr/></div>"); }

TEST(VoidElementNormalizer, ClosesEveryVoidElement) {
  EXPECT_EQ(once("<img src=\"x.png\">"), "<img src=\"x.png\"/>");
  EXPECT_EQ(once("<meta charset=\"utf-8\">"), "<meta charset=\"utf-8\"/>");
  EXPECT_EQ(once("<link rel=\"stylesheet\">"), "<link rel=\"stylesheet\"/>");
}

TEST(VoidElementNormalizer, IsCaseInsensitive) { EXPECT_EQ(once("<BR><Hr>"), "<BR/><Hr/>"); }

// --- must change nothing else -----------------------------------------------

TEST(VoidElementNormalizer, LeavesAlreadySelfClosingTagsAlone) {
  EXPECT_EQ(once("<br/>"), "<br/>");
  EXPECT_EQ(once("<br />"), "<br />");
}

TEST(VoidElementNormalizer, LeavesNonVoidElementsAlone) {
  // <p> genuinely needs its closing tag; closing it here would change the tree.
  EXPECT_EQ(once("<p>text</p>"), "<p>text</p>");
  EXPECT_EQ(once("<div><span>x</span></div>"), "<div><span>x</span></div>");
}

TEST(VoidElementNormalizer, LeavesClosingTagsCommentsAndPisAlone) {
  EXPECT_EQ(once("</br>"), "</br>");
  EXPECT_EQ(once("<!-- <br> -->"), "<!-- <br> -->");
  EXPECT_EQ(once("<?xml version=\"1.0\"?>"), "<?xml version=\"1.0\"?>");
  EXPECT_EQ(once("<!DOCTYPE html>"), "<!DOCTYPE html>");
}

TEST(VoidElementNormalizer, DoesNotMatchNamesThatMerelyStartWithAVoidName) {
  // "break" and "hrefish" are not void elements; only an exact name match counts.
  EXPECT_EQ(once("<break>x</break>"), "<break>x</break>");
  EXPECT_EQ(once("<brx>"), "<brx>");
}

TEST(VoidElementNormalizer, PassesPlainTextThrough) {
  EXPECT_EQ(once("no markup at all"), "no markup at all");
  EXPECT_EQ(once(""), "");
}

// --- chunk-boundary behaviour ------------------------------------------------

TEST(VoidElementNormalizer, HandlesATagSplitAcrossChunks) {
  const std::string in = "<p>a<br>b</p>";
  for (size_t chunk = 1; chunk <= in.size(); ++chunk) {
    EXPECT_EQ(chunked(in, chunk), "<p>a<br/>b</p>") << "chunk size " << chunk;
  }
}

TEST(VoidElementNormalizer, HandlesManyVoidTagsSplitAtEveryOffset) {
  const std::string in = "x<br>y<hr>z<img src=\"a\">w";
  const std::string expected = "x<br/>y<hr/>z<img src=\"a\"/>w";
  for (size_t chunk = 1; chunk <= in.size(); ++chunk) {
    EXPECT_EQ(chunked(in, chunk), expected) << "chunk size " << chunk;
  }
}

TEST(VoidElementNormalizer, PassesThroughAnAbsurdlyLongUnterminatedTag) {
  // Longer than MAX_CARRY with no '>': emitted verbatim rather than held forever.
  const std::string in = "<" + std::string(void_elements::MAX_CARRY + 20, 'a');
  EXPECT_EQ(once(in), in);
}

TEST(VoidElementNormalizer, GrowthBoundCoversTheWorstCase) {
  // Densest possible growth: nothing but "<br>".
  std::string in;
  for (int i = 0; i < 100; ++i) in += "<br>";
  const std::string out = once(in);
  EXPECT_EQ(out.size(), in.size() + 100);
  EXPECT_LE(100u, void_elements::growthBound(in.size()));
}

}  // namespace
