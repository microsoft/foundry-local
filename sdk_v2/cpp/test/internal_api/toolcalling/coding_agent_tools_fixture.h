// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

namespace fl {
namespace test {

/// Exact `apply_patch` declaration captured from stock GHCP on /v1/chat/completions.
inline constexpr const char* kCodingAgentChatToolsJson = R"([
  {
    "type": "custom",
    "custom": {
      "name": "apply_patch",
      "description": "Use the `apply_patch` tool to edit files. This is a FREEFORM )"
                                                         R"(tool, so do not wrap the patch in JSON.",
      "format": {
        "type": "grammar",
        "grammar": {
          "syntax": "lark",
          "definition": "start: begin_patch hunk+ end_patch\nbegin_patch: \"*** Begin Patch\" LF\n)"
                                                         R"(end_patch: \"*** End Patch\" LF?\n\n)"
                                                         R"(hunk: add_hunk | delete_hunk | update_hunk\n)"
                                                         R"(add_hunk: \"*** Add File: \" filename LF add_line+\n)"
                                                         R"(delete_hunk: \"*** Delete File: \" filename LF\n)"
                                                         R"(update_hunk: \"*** Update File: \" filename LF )"
                                                         R"(change_move? change?\n\n)"
                                                         R"(filename: /(.+)/\n)"
                                                         R"(add_line: \"+\" /(.*)/ LF -> line\n\n)"
                                                         R"(change_move: \"*** Move to: \" filename LF\n)"
                                                         R"(change: (change_context | change_line)+ eof_line?\n)"
                                                         R"(change_context: (\"@@\" | \"@@ \" /(.+)/) LF\n)"
                                                         R"(change_line: (\"+\" | \"-\" | \" \") /(.*)/ LF\n)"
                                                         R"(eof_line: \"*** End of File\" LF\n\n%import common.LF"
}
}
}
}
])";

/// Exact `apply_patch` declaration captured from stock GHCP on /v1/responses.
inline constexpr const char* kCodingAgentResponsesToolsJson = R"([
  {
    "name": "apply_patch",
    "description": "Use the `apply_patch` tool to edit files. This is a FREEFORM )"
                                                              R"(tool, so do not wrap the patch in JSON.",
    "type": "custom",
    "format": {
      "type": "grammar",
      "syntax": "lark",
      "definition": "start: begin_patch hunk+ end_patch\nbegin_patch: \"*** Begin Patch\" LF\n)"
                                                              R"(end_patch: \"*** End Patch\" LF?\n\n)"
                                                              R"(hunk: add_hunk | delete_hunk | update_hunk\n)"
                                                              R"(add_hunk: \"*** Add File: \" filename LF add_line+\n)"
                                                              R"(delete_hunk: \"*** Delete File: \" filename LF\n)"
                                                              R"(update_hunk: \"*** Update File: \" filename LF )"
                                                              R"(change_move? change?\n\n)"
                                                              R"(filename: /(.+)/\n)"
                                                              R"(add_line: \"+\" /(.*)/ LF -> line\n\n)"
                                                              R"(change_move: \"*** Move to: \" filename LF\n)"
                                                              R"(change: (change_context | change_line)+ eof_line?\n)"
                                                              R"(change_context: (\"@@\" | \"@@ \" /(.+)/) LF\n)"
                                                              R"(change_line: (\"+\" | \"-\" | \" \") /(.*)/ LF\n)"
                                                              R"(eof_line: \"*** End of File\" LF\n\n%import common.LF"
}
}
])";

inline constexpr const char* kCodingAgentCustomToolName = "apply_patch";

}  // namespace test
}  // namespace fl
