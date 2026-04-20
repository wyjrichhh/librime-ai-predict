// CTranslate2 implementation of InferenceBackend (seq2seq / BART-style).
//
// Copyright RIME Developers
// Distributed under the BSD License

#ifndef RIME_PREDICT_CT2_BACKEND_H_
#define RIME_PREDICT_CT2_BACKEND_H_

#include "inference_backend.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <ctranslate2/translator.h>
#include <nlohmann/json.hpp>

namespace rime {
namespace predict {

constexpr int kCt2DefaultMaxTokens = 64;
constexpr int kCt2DefaultBeamSize = 1;
constexpr float kCt2DefaultLengthPenalty = 1.0f;
constexpr float kCt2DefaultRepetitionPenalty = 1.2f;
constexpr int kCt2DefaultSamplingTopk = 1;
constexpr int kCt2DefaultNumHypotheses = 1;
constexpr int64_t kCt2DefaultTimeoutMs = 5000;
constexpr int kCt2DefaultIntraThreads = 4;

class CT2Backend : public InferenceBackend {
 public:
  CT2Backend() = default;
  ~CT2Backend() override;

  bool Initialize(const InferenceBackendConfig& config) override;
  void Shutdown() override;
  bool IsInitialized() const override {
    return initialized_.load(std::memory_order_acquire);
  }

  std::optional<std::string> Predict(const std::string& prompt,
                                     int max_tokens) override;

 private:
  bool LoadSharedVocabulary(const std::string& path);
  void LoadTokenizerJson(const std::string& path);
  void ParseAddedTokensJson(const nlohmann::json& root);
  void ParseMergesJson(const nlohmann::json& root);

  std::vector<std::string> Tokenize(const std::string& text);

  std::unique_ptr<ctranslate2::Translator> translator_;
  std::atomic<bool> initialized_{false};
  std::mutex mutex_;

  std::unordered_map<std::string, int> token_to_id_;
  std::vector<std::string> id_to_token_;
  std::string unk_token_;

  std::set<std::string> added_tokens_;
  std::vector<std::string> added_tokens_sorted_;

  std::set<std::string> special_tokens_;
  std::vector<std::string> special_tokens_sorted_;

  std::vector<std::pair<std::string, std::string>> merges_;
  std::unordered_map<std::string, int> merge_ranks_;

  std::unordered_map<uint32_t, uint8_t> unicode_to_byte_;
  std::unordered_map<uint8_t, uint32_t> byte_to_unicode_;

  void splitOnSpecialTokens(const std::string& text,
                            std::vector<std::string>& segments,
                            std::vector<bool>& is_special);

  std::vector<std::string> tokenizeSegment(const std::string& text);

  void splitOnAddedTokens(const std::string& text,
                          std::vector<std::string>& parts,
                          std::vector<bool>& is_added);

  std::vector<std::string> bpeTokenize(const std::string& text);

  std::string Detokenize(const std::vector<std::string>& tokens);

  void buildUnicodeToByteMap();
  std::string byteLevelEncode(const std::string& text);
  std::string byteLevelDecode(const std::string& text);

  static size_t utf8CharLength(unsigned char c);
};

}  // namespace predict
}  // namespace rime

#endif  // RIME_PREDICT_CT2_BACKEND_H_
