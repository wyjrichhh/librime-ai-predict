// Copyright RIME Developers
// Distributed under the BSD License
//
// frontend_protocol.h
//
// Wire-level helpers for talking to Rime frontends via librime's
// notification_handler. Implements the plugin side of the reserved
// property-key protocol discussed in rime/squirrel#1124.
//
// Why centralised:
//   - Reserved keys ("_refresh_ui", "_comment_highlight", ...) are a
//     cross-frontend contract. Letting them leak as string literals
//     across translator / filter / engine code makes it hard to audit
//     "what does this plugin tell the frontend?" and to upgrade later.
//   - Value encoding (URL query string) is the same protocol concern
//     and benefits from a single producer.
//
// What goes in:
//   - Reserved key names (must match the frontend table, e.g.
//     Squirrel's ReservedPropertyKey enum).
//   - Pure builders that turn typed arguments into the wire string.
//     They never touch librime types -- callers wrap the result in
//     ctx->set_property(key, value).
//
// What stays out:
//   - Anything that depends on Context / Engine / Translation -- those
//     are business concerns of the filter / engine that own the call site.

#ifndef RIME_AI_PREDICT_FRONTEND_PROTOCOL_H_
#define RIME_AI_PREDICT_FRONTEND_PROTOCOL_H_

#include <string>

namespace rime {
namespace predict {
namespace protocol {

// === Reserved property keys (cross-frontend contract) ======================

// Action: ask the frontend to re-pull the candidate menu because an
// async task has produced new candidates.
inline constexpr const char* kRefreshUI = "_refresh_ui";

// State: candidates at these indices should render their comment with
// the active color scheme's accent_text_color.
inline constexpr const char* kCommentHighlight = "_comment_highlight";

// === Plugin identity =======================================================

// Used as the `source` field in event payloads so frontends (and log
// readers) can tell who triggered an async refresh.
inline constexpr const char* kPluginCodename = "ai_predict";

// === Value builders ========================================================

// Builds the value for kRefreshUI.
//
// Encoding: URL query string (e.g. "source=ai_predict&kind=full").
// Frontends that don't parse the body still call rimeUpdate(), so the
// body is purely informational -- but the convention lets frontends
// log / debounce / route per-source.
//
// `kind` describes the scope of the refresh ("full" rebuilds the menu;
// "partial" hints at "only my row changed"). The default covers the
// common AI-predict case (cache hit triggers a fresh Compose).
std::string MakeRefreshUIPayload(const std::string& source,
                                 const std::string& kind = "full");

// Builds the value for kCommentHighlight from a single non-negative
// index. Returns "" when index < 0 (means "no row to highlight this
// frame" -- frontends clear the cache).
//
// Encoding: bare-list shorthand ("0", "0,2"). The new protocol
// normalises this to { indices: "0,2" }, and the form is also accepted
// by frontends that hard-code the bare-list shape (lotem 2026-05-06).
// Keeping a single integer (vs. the long-form "indices=0") shaves bytes
// on the IPC path that runs every Compose().
std::string MakeCommentHighlightPayload(int index);

}  // namespace protocol
}  // namespace predict
}  // namespace rime

#endif  // RIME_AI_PREDICT_FRONTEND_PROTOCOL_H_
