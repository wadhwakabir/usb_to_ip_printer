#include <unity.h>

#include "http_status_utils.h"

void test_escape_html_escapes_special_characters() {
  TEST_ASSERT_EQUAL_STRING(
      "&lt;tag attr=&quot;a&amp;b&quot;&gt;",
      http_status_utils::EscapeHtml("<tag attr=\"a&b\">").c_str());
}

void test_escape_json_escapes_control_and_quote_characters() {
  TEST_ASSERT_EQUAL_STRING(
      "\\\"line1\\nline2\\t\\\\",
      http_status_utils::EscapeJson("\"line1\nline2\t\\").c_str());
}

void test_classify_request_line_matches_root_exactly() {
  TEST_ASSERT_EQUAL(
      static_cast<int>(http_status_utils::RequestRoute::kRoot),
      static_cast<int>(
          http_status_utils::ClassifyRequestLine("GET / HTTP/1.1")));
}

void test_classify_request_line_matches_status_json_exactly() {
  TEST_ASSERT_EQUAL(
      static_cast<int>(http_status_utils::RequestRoute::kStatusJson),
      static_cast<int>(http_status_utils::ClassifyRequestLine(
          "GET /status.json HTTP/1.1")));
}

void test_classify_request_line_rejects_partial_prefix_match() {
  TEST_ASSERT_EQUAL(
      static_cast<int>(http_status_utils::RequestRoute::kNotFound),
      static_cast<int>(http_status_utils::ClassifyRequestLine(
          "GET /status.jsonx HTTP/1.1")));
}

void test_classify_request_line_rejects_query_string_route() {
  TEST_ASSERT_EQUAL(
      static_cast<int>(http_status_utils::RequestRoute::kNotFound),
      static_cast<int>(http_status_utils::ClassifyRequestLine(
          "GET /status.json?verbose=1 HTTP/1.1")));
}

void test_classify_request_line_rejects_bad_method() {
  TEST_ASSERT_EQUAL(
      static_cast<int>(http_status_utils::RequestRoute::kBadRequest),
      static_cast<int>(
          http_status_utils::ClassifyRequestLine("POST / HTTP/1.1")));
}

void test_classify_request_line_rejects_missing_http_version() {
  TEST_ASSERT_EQUAL(
      static_cast<int>(http_status_utils::RequestRoute::kBadRequest),
      static_cast<int>(
          http_status_utils::ClassifyRequestLine("GET /status.json")));
}

void test_format_hex16_zero_pads() {
  TEST_ASSERT_EQUAL_STRING("04a9",
                           http_status_utils::FormatHex16(0x04A9).c_str());
  TEST_ASSERT_EQUAL_STRING("0000",
                           http_status_utils::FormatHex16(0x0000).c_str());
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  UNITY_BEGIN();
  RUN_TEST(test_escape_html_escapes_special_characters);
  RUN_TEST(test_escape_json_escapes_control_and_quote_characters);
  RUN_TEST(test_classify_request_line_matches_root_exactly);
  RUN_TEST(test_classify_request_line_matches_status_json_exactly);
  RUN_TEST(test_classify_request_line_rejects_partial_prefix_match);
  RUN_TEST(test_classify_request_line_rejects_query_string_route);
  RUN_TEST(test_classify_request_line_rejects_bad_method);
  RUN_TEST(test_classify_request_line_rejects_missing_http_version);
  RUN_TEST(test_format_hex16_zero_pads);
  return UNITY_END();
}
