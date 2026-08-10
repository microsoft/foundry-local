import unittest

from orchestrator import assertions as a


class AssertionLibraryTests(unittest.TestCase):
    def test_chat_nonempty(self):
        self.assertTrue(a.assert_chat_nonempty("hello")["ok"])
        self.assertFalse(a.assert_chat_nonempty("  \n\t ")["ok"])
        self.assertFalse(a.assert_chat_nonempty(None)["ok"])

    def test_chat_completion_shape(self):
        obj = {"choices": [{"message": {"role": "assistant", "content": "hi"}, "finish_reason": "stop"}]}
        self.assertTrue(a.assert_chat_completion_shape(obj)["ok"])
        bad = {"choices": [{"message": {"role": "user"}}]}
        result = a.assert_chat_completion_shape(bad)
        self.assertFalse(result["ok"])
        self.assertIn("role", result["detail"])
        self.assertIn("finish_reason", result["detail"])

    def test_stream_reconstruction(self):
        chunks = [
            {"choices": [{"delta": {"content": "hel"}, "finish_reason": None}]},
            {"choices": [{"delta": {"content": "lo"}, "finish_reason": None}]},
            {"choices": [{"delta": {}, "finish_reason": "stop"}]},
        ]
        self.assertTrue(a.assert_stream_reconstruction(chunks, "hello")["ok"])
        self.assertFalse(a.assert_stream_reconstruction(chunks, "goodbye")["ok"])
        self.assertFalse(a.assert_stream_reconstruction(chunks[:-1], "hello")["ok"])

    def test_tool_call(self):
        obj = {
            "choices": [{
                "message": {
                    "tool_calls": [{"function": {"name": "get_weather", "arguments": '{"city":"Seattle"}'}}]
                }
            }]
        }
        self.assertTrue(a.assert_tool_call(obj, "get_weather", ["city"])["ok"])
        self.assertFalse(a.assert_tool_call(obj, "get_time", ["city"])["ok"])
        self.assertFalse(a.assert_tool_call(obj, "get_weather", ["city", "units"])["ok"])
        malformed = {"function_call": {"name": "get_weather", "arguments": "not-json"}}
        self.assertFalse(a.assert_tool_call(malformed, "get_weather", ["city"])["ok"])

    def test_embedding(self):
        self.assertTrue(a.assert_embedding([[0.1, 0.2], [0.3, 0.4]], 2, expected_dim=2)["ok"])
        self.assertFalse(a.assert_embedding([[0.0, 0.0]], 1)["ok"])
        self.assertFalse(a.assert_embedding([[0.1], [0.2, 0.3]], 2)["ok"])
        self.assertFalse(a.assert_embedding([[float("inf")]], 1)["ok"])
        self.assertFalse(a.assert_embedding([[1.0]], 2)["ok"])

    def test_transcription_wer_thresholds(self):
        self.assertTrue(a.assert_transcription("the quick brown fox", "the quick brown fox", 0.0)["ok"])
        self.assertTrue(a.assert_transcription("the quick brown fox", "the quick red fox", 0.25)["ok"])
        result = a.assert_transcription("one two", "the quick red fox", 0.4)
        self.assertFalse(result["ok"])
        self.assertIn("WER", result["detail"])

    def test_http_and_error_shape(self):
        self.assertTrue(a.assert_http_ok(200)["ok"])
        self.assertTrue(a.assert_http_ok(299)["ok"])
        self.assertFalse(a.assert_http_ok(500)["ok"])
        error = {"error": {"message": "bad", "type": "invalid_request_error"}}
        self.assertTrue(a.assert_openai_error_shape(error)["ok"])
        self.assertFalse(a.assert_openai_error_shape({"error": {"message": "bad"}})["ok"])
        self.assertFalse(a.assert_openai_error_shape({"message": "bad", "type": "invalid_request_error"})["ok"])

    def test_json_schema_subset(self):
        obj = {"choices": [{"message": {"content": None}}], "usage": {"total_tokens": 3}}
        self.assertTrue(a.assert_json_schema_subset(obj, ["choices.0.message.content", "usage.total_tokens"])["ok"])
        result = a.assert_json_schema_subset(obj, ["choices.1.message.content"])
        self.assertFalse(result["ok"])
        self.assertIn("choices.1.message.content", result["detail"])

    def test_version_equals(self):
        self.assertTrue(a.assert_version_equals("2.0.0rc1", "2.0.0-rc1")["ok"])
        self.assertTrue(a.assert_version_equals("v2.0.0-preview.1+build.5", "2.0.0-pre1")["ok"])
        self.assertFalse(a.assert_version_equals("2.0.0", "2.0.1")["ok"])

    def test_all_ok_and_summarize(self):
        assertions = [a.assert_chat_nonempty("x"), a.assert_http_ok(204)]
        self.assertTrue(a.all_ok(assertions))
        self.assertEqual(a.summarize(assertions), "2/2 assertions passed")
        failed = assertions + [a.assert_http_ok(404)]
        self.assertFalse(a.all_ok(failed))
        self.assertIn("failed: http status is 2xx", a.summarize(failed))


if __name__ == "__main__":
    unittest.main()
