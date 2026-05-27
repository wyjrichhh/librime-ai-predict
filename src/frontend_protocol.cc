// Copyright RIME Developers
// Distributed under the BSD License

#include "frontend_protocol.h"

namespace rime {
namespace predict {
namespace protocol {

std::string MakeRefreshUIPayload(const std::string& source,
                                 const std::string& kind) {
  // Both fields are plugin-controlled identifiers ([a-z0-9_]+ by
  // convention), so they're safe to embed without percent-encoding.
  // If the convention is ever broken the worst-case is a malformed
  // query string -- the frontend's URL parser drops it silently and
  // the action still fires (refresh has no required fields).
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
