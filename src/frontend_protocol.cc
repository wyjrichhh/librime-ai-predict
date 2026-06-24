// Copyright RIME Developers
// Distributed under the BSD License

#include "frontend_protocol.h"

namespace rime {
namespace predict {
namespace protocol {

std::string MakeRefreshUIPayload(const std::string& source,
                                 const std::string& kind) {
  // Both fields are plugin-controlled identifiers ([a-z0-9_]+ by convention),
  // so they need no percent-encoding. A broken convention only yields a
  // malformed query string, which the frontend drops silently -- the refresh
  // still fires (it has no required fields).
  std::string out;
  out.reserve(source.size() + kind.size() + 16);
  out.append("source=").append(source);
  if (!kind.empty()) {
    out.append("&kind=").append(kind);
  }
  return out;
}

std::string MakeCommentHighlightPayload(int index) {
  if (index < 0) return "";
  return std::to_string(index);
}

}  // namespace protocol
}  // namespace predict
}  // namespace rime
