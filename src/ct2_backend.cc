// ct2_backend.cc — InferenceBackend implementation: CTranslate2 + GPT-2 ByteLevel BPE.
//
// Copyright RIME Developers
// Distributed under the BSD License

#include "ct2_backend.h"

#include <glog/logging.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <fstream>
#include <optional>

#include <ctranslate2/translator.h>
#include <ctranslate2/translation.h>
#include <nlohmann/json.hpp>

namespace rime {
namespace predict {

namespace {

std::string UnkTokenForVocab(const std::unordered_map<std::string, int>& token_to_id) {
  static const char* kCandidates[] = {"<unk>", "<UNK>", "[UNK]", "<|endoftext|>"};
  for (const char* t : kCandidates) {
    if (token_to_id.count(t)) {
      return t;
    }
  }
  return "<unk>";
}

}  // namespace

CT2Backend::~CT2Backend() {
  Shutdown();
}

bool CT2Backend::Initialize(const InferenceBackendConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (initialized_.load(std::memory_order_acquire)) {
    LOG(WARNING) << "CT2Backend already initialized";
    return false;
  }

  const std::string& model_path = config.model_path;
  const std::string& device = config.device;

  LOG(INFO) << "CT2Backend initialize model_path=" << model_path
            << " device=" << device;

  try {
    ctranslate2::Device ct2_device = ctranslate2::Device::CPU;
    if (device.find("cuda") != std::string::npos ||
        device.find("CUDA") != std::string::npos) {
      ct2_device = ctranslate2::Device::CUDA;
      LOG(INFO) << "CT2Backend using CUDA";
    }

    ctranslate2::ComputeType compute_type =
        ctranslate2::str_to_compute_type("default");

    ctranslate2::ReplicaPoolConfig pool_config;
    pool_config.num_threads_per_replica =
        static_cast<size_t>(kCt2DefaultIntraThreads);

    translator_ = std::make_unique<ctranslate2::Translator>(
        model_path, ct2_device, compute_type, std::vector<int>{0}, false,
        pool_config);

    if (!translator_) {
      LOG(ERROR) << "CT2Backend failed to create translator";
      return false;
    }

    const std::string vocab_path = model_path + "/shared_vocabulary.json";
    if (!LoadSharedVocabulary(vocab_path)) {
      LOG(ERROR) << "CT2Backend failed to load shared_vocabulary.json";
      translator_.reset();
      return false;
    }

    unk_token_ = UnkTokenForVocab(token_to_id_);

    const std::string tokenizer_path = model_path + "/tokenizer.json";
    LoadTokenizerJson(tokenizer_path);
    buildUnicodeToByteMap();

    LOG(INFO) << "CT2Backend tokenizer: vocab=" << token_to_id_.size()
              << " merges=" << merges_.size();

    initialized_.store(true, std::memory_order_release);
    return true;

  } catch (const std::exception& e) {
    LOG(ERROR) << "CT2Backend initialize: " << e.what();
    translator_.reset();
    return false;
  }
}

void CT2Backend::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_.load(std::memory_order_acquire)) {
    return;
  }

  LOG(INFO) << "CT2Backend shutting down";
  translator_.reset();
  token_to_id_.clear();
  id_to_token_.clear();
  added_tokens_.clear();
  added_tokens_sorted_.clear();
  special_tokens_.clear();
  special_tokens_sorted_.clear();
  merges_.clear();
  merge_ranks_.clear();
  unicode_to_byte_.clear();
  byte_to_unicode_.clear();
  unk_token_.clear();
  initialized_.store(false, std::memory_order_release);
}

std::optional<std::string> CT2Backend::Predict(const std::string& prompt,
                                               int max_tokens) {
  if (!initialized_.load(std::memory_order_acquire)) {
    LOG(ERROR) << "CT2Backend::Predict: service not initialized";
    return std::nullopt;
  }

  auto start_time = std::chrono::steady_clock::now();

  try {
    if (prompt.empty()) {
      return std::nullopt;
    }

    std::vector<std::string> source_tokens = Tokenize(prompt);

    if (source_tokens.size() <= 2) {
      return std::nullopt;
    }

    ctranslate2::TranslationOptions options;
    options.beam_size = kCt2DefaultBeamSize;
    options.max_decoding_length = max_tokens;
    options.length_penalty = kCt2DefaultLengthPenalty;
    options.repetition_penalty = kCt2DefaultRepetitionPenalty;
    options.sampling_topk = kCt2DefaultSamplingTopk;
    options.num_hypotheses = kCt2DefaultNumHypotheses;

    std::vector<std::string> end_tokens;
    if (token_to_id_.count("</hanzi_start>")) {
      end_tokens.push_back("</hanzi_start>");
    }
    if (token_to_id_.count("</s>")) {
      end_tokens.push_back("</s>");
    }
    if (!end_tokens.empty()) {
      options.end_token = end_tokens;
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start_time)
                          .count();

    if (elapsed_ms > kCt2DefaultTimeoutMs) {
      return std::nullopt;
    }

    std::vector<std::vector<std::string>> batch = {source_tokens};
    auto results = translator_->translate_batch(batch, options);

    if (results.empty() || results[0].hypotheses.empty()) {
      return std::nullopt;
    }

    const auto& output_tokens = results[0].output();
    std::string result = Detokenize(output_tokens);

    const std::string special_tags[] = {
        "<hanzi_start>", "</hanzi_start>", "<pinyin_start>", "</pinyin_start>",
        "<s>", "</s>", "<pad>"};
    for (const auto& tag : special_tags) {
      size_t pos;
      while ((pos = result.find(tag)) != std::string::npos) {
        result.replace(pos, tag.length(), "");
      }
    }

    result.erase(std::remove(result.begin(), result.end(), ' '), result.end());

    if (result.empty()) {
      return std::nullopt;
    }

    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start_time)
                           .count();
    DLOG(INFO) << "CT2Backend::Predict " << result.size() << " chars in "
               << duration_ms << "ms";

    return result;

  } catch (const std::exception& e) {
    LOG(ERROR) << "CT2Backend::Predict: " << e.what();
    return std::nullopt;
  }
}

bool CT2Backend::LoadSharedVocabulary(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    LOG(ERROR) << "CT2Backend: cannot open " << path;
    return false;
  }

  try {
    nlohmann::json j;
    file >> j;
    if (!j.is_array()) {
      LOG(ERROR) << "CT2Backend: shared_vocabulary.json must be a JSON array";
      return false;
    }
    int id = 0;
    for (const auto& item : j) {
      if (!item.is_string()) {
        continue;
      }
      const std::string token = item.get<std::string>();
      token_to_id_[token] = id;
      id_to_token_.push_back(token);
      ++id;
    }
    LOG(INFO) << "CT2Backend: loaded " << id
              << " tokens from shared_vocabulary.json";
    return id > 0;
  } catch (const std::exception& e) {
    LOG(ERROR) << "CT2Backend: parse shared_vocabulary.json: " << e.what();
    return false;
  }
}

void CT2Backend::LoadTokenizerJson(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    LOG(WARNING) << "CT2Backend: cannot open tokenizer.json at " << path;
    return;
  }

  try {
    nlohmann::json j;
    file >> j;
    ParseAddedTokensJson(j);
    ParseMergesJson(j);

    added_tokens_sorted_.assign(added_tokens_.begin(), added_tokens_.end());
    std::sort(added_tokens_sorted_.begin(), added_tokens_sorted_.end(),
              [](const std::string& a, const std::string& b) {
                return a.length() > b.length();
              });

    special_tokens_sorted_.assign(special_tokens_.begin(),
                                 special_tokens_.end());
    std::sort(special_tokens_sorted_.begin(), special_tokens_sorted_.end(),
              [](const std::string& a, const std::string& b) {
                return a.length() > b.length();
              });
  } catch (const std::exception& e) {
    LOG(WARNING) << "CT2Backend: parse tokenizer.json: " << e.what();
  }
}

void CT2Backend::ParseAddedTokensJson(const nlohmann::json& root) {
  if (!root.contains("added_tokens") || !root["added_tokens"].is_array()) {
    return;
  }
  for (const auto& t : root["added_tokens"]) {
    if (!t.is_object()) {
      continue;
    }
    const std::string content = t.value("content", "");
    if (content.empty()) {
      continue;
    }
    const bool is_special = t.value("special", false);
    if (is_special) {
      special_tokens_.insert(content);
    } else {
      added_tokens_.insert(content);
    }
  }
}

void CT2Backend::ParseMergesJson(const nlohmann::json& root) {
  const nlohmann::json* merges_ptr = nullptr;
  if (root.contains("model") && root["model"].is_object() &&
      root["model"].contains("merges")) {
    merges_ptr = &root["model"]["merges"];
  } else if (root.contains("merges")) {
    merges_ptr = &root["merges"];
  }
  if (!merges_ptr || !merges_ptr->is_array()) {
    return;
  }

  int rank = 0;
  for (const auto& item : *merges_ptr) {
    if (item.is_string()) {
      const std::string merge_str = item.get<std::string>();
      const size_t space_pos = merge_str.find(' ');
      if (space_pos != std::string::npos) {
        const std::string first = merge_str.substr(0, space_pos);
        const std::string second = merge_str.substr(space_pos + 1);
        merges_.push_back({first, second});
        merge_ranks_[first + " " + second] = rank++;
      }
    } else if (item.is_array() && item.size() >= 2 && item[0].is_string() &&
               item[1].is_string()) {
      const std::string first = item[0].get<std::string>();
      const std::string second = item[1].get<std::string>();
      merges_.push_back({first, second});
      merge_ranks_[first + " " + second] = rank++;
    }
  }
}

void CT2Backend::splitOnSpecialTokens(const std::string& text,
                                      std::vector<std::string>& segments,
                                      std::vector<bool>& is_special) {
  std::string remaining = text;

  while (!remaining.empty()) {
    size_t earliest_pos = std::string::npos;
    std::string earliest_token;

    for (const auto& st : special_tokens_sorted_) {
      const size_t pos = remaining.find(st);
      if (pos != std::string::npos &&
          (pos < earliest_pos ||
           (pos == earliest_pos && st.length() > earliest_token.length()))) {
        earliest_pos = pos;
        earliest_token = st;
      }
    }

    if (earliest_pos == std::string::npos) {
      if (!remaining.empty()) {
        segments.push_back(remaining);
        is_special.push_back(false);
      }
      break;
    }

    if (earliest_pos > 0) {
      segments.push_back(remaining.substr(0, earliest_pos));
      is_special.push_back(false);
    }

    segments.push_back(earliest_token);
    is_special.push_back(true);

    remaining = remaining.substr(earliest_pos + earliest_token.length());
  }
}

std::vector<std::string> CT2Backend::Tokenize(const std::string& text) {
  std::vector<std::string> tokens;

  const bool has_start = (text.find("<pinyin_start>") != std::string::npos);
  std::string input_text = text;
  if (!has_start) {
    input_text = "<pinyin_start>" + text + "</pinyin_start>";
  }

  std::vector<std::string> segments;
  std::vector<bool> is_special;
  splitOnSpecialTokens(input_text, segments, is_special);

  for (size_t i = 0; i < segments.size(); ++i) {
    if (is_special[i]) {
      tokens.push_back(segments[i]);
    } else {
      auto sub_tokens = tokenizeSegment(segments[i]);
      tokens.insert(tokens.end(), sub_tokens.begin(), sub_tokens.end());
    }
  }

  return tokens;
}

std::vector<std::string> CT2Backend::tokenizeSegment(const std::string& text) {
  std::vector<std::string> tokens;

  std::vector<std::string> parts;
  std::vector<bool> part_is_added;
  splitOnAddedTokens(text, parts, part_is_added);

  for (size_t i = 0; i < parts.size(); ++i) {
    if (part_is_added[i]) {
      tokens.push_back(parts[i]);
    } else {
      auto bpe_tokens = bpeTokenize(parts[i]);
      tokens.insert(tokens.end(), bpe_tokens.begin(), bpe_tokens.end());
    }
  }

  return tokens;
}

void CT2Backend::splitOnAddedTokens(const std::string& text,
                                    std::vector<std::string>& parts,
                                    std::vector<bool>& is_added) {
  size_t pos = 0;

  while (pos < text.size()) {
    std::string best_match;

    for (const auto& token : added_tokens_sorted_) {
      if (token.length() <= text.size() - pos &&
          text.compare(pos, token.length(), token) == 0) {
        best_match = token;
        break;
      }
    }

    if (!best_match.empty()) {
      parts.push_back(best_match);
      is_added.push_back(true);
      pos += best_match.length();
    } else {
      const size_t char_len = utf8CharLength(static_cast<unsigned char>(text[pos]));
      if (pos + char_len > text.size()) {
        break;
      }

      if (!parts.empty() && !is_added.back()) {
        parts.back() += text.substr(pos, char_len);
      } else {
        parts.push_back(text.substr(pos, char_len));
        is_added.push_back(false);
      }
      pos += char_len;
    }
  }
}

std::vector<std::string> CT2Backend::bpeTokenize(const std::string& text) {
  if (text.empty()) {
    return {};
  }

  std::string encoded = byteLevelEncode(text);

  std::vector<std::string> symbols;
  size_t pos = 0;
  while (pos < encoded.size()) {
    const size_t char_len = utf8CharLength(static_cast<unsigned char>(encoded[pos]));
    if (pos + char_len > encoded.size()) {
      break;
    }
    symbols.push_back(encoded.substr(pos, char_len));
    pos += char_len;
  }

  if (symbols.size() <= 1) {
    if (!symbols.empty() && token_to_id_.count(symbols[0])) {
      return symbols;
    }
    return {unk_token_};
  }

  while (symbols.size() > 1) {
    int best_rank = INT_MAX;
    int best_idx = -1;

    for (size_t i = 0; i + 1 < symbols.size(); ++i) {
      const std::string pair = symbols[i] + " " + symbols[i + 1];
      auto it = merge_ranks_.find(pair);
      if (it != merge_ranks_.end() && it->second < best_rank) {
        best_rank = it->second;
        best_idx = static_cast<int>(i);
      }
    }

    if (best_idx < 0) {
      break;
    }

    symbols[static_cast<size_t>(best_idx)] =
        symbols[static_cast<size_t>(best_idx)] +
        symbols[static_cast<size_t>(best_idx) + 1];
    symbols.erase(symbols.begin() + best_idx + 1);
  }

  std::vector<std::string> result;
  for (const auto& sym : symbols) {
    if (token_to_id_.count(sym)) {
      result.push_back(sym);
    } else {
      result.push_back(unk_token_);
    }
  }

  return result;
}

std::string CT2Backend::Detokenize(const std::vector<std::string>& tokens) {
  std::string raw_text;

  for (const auto& token : tokens) {
    if (token == "<hanzi_start>" || token == "</hanzi_start>" ||
        token == "<s>" || token == "</s>" || token == "<pad>" ||
        token == "<|endoftext|>" || token == "<|im_start|>" ||
        token == "<|im_end|>" || token == "<|PAD|>" ||
        token == "<|EOS|>" || token == "<|BOS|>" ||
        token == "[CLS]" || token == "[SEP]") {
      continue;
    }
    raw_text += token;
  }

  std::string decoded = byteLevelDecode(raw_text);
  decoded.erase(std::remove(decoded.begin(), decoded.end(), ' '), decoded.end());

  return decoded;
}

void CT2Backend::buildUnicodeToByteMap() {
  std::vector<int> direct_bytes;
  for (int b = 33; b <= 126; ++b) {
    direct_bytes.push_back(b);
  }
  for (int b = 161; b <= 172; ++b) {
    direct_bytes.push_back(b);
  }
  for (int b = 174; b <= 255; ++b) {
    direct_bytes.push_back(b);
  }

  for (int b : direct_bytes) {
    unicode_to_byte_[static_cast<uint32_t>(b)] = static_cast<uint8_t>(b);
    byte_to_unicode_[static_cast<uint8_t>(b)] = static_cast<uint32_t>(b);
  }

  std::set<int> direct_set(direct_bytes.begin(), direct_bytes.end());
  int n = 0;
  for (int b = 0; b < 256; ++b) {
    if (direct_set.find(b) == direct_set.end()) {
      const uint32_t unicode_val = 256 + n;
      unicode_to_byte_[unicode_val] = static_cast<uint8_t>(b);
      byte_to_unicode_[static_cast<uint8_t>(b)] = unicode_val;
      ++n;
    }
  }
}

static std::string codepointToUtf8(uint32_t cp) {
  std::string r;
  if (cp < 0x80) {
    r += static_cast<char>(cp);
  } else if (cp < 0x800) {
    r += static_cast<char>(0xC0 | (cp >> 6));
    r += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    r += static_cast<char>(0xE0 | (cp >> 12));
    r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    r += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    r += static_cast<char>(0xF0 | (cp >> 18));
    r += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    r += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return r;
}

std::string CT2Backend::byteLevelEncode(const std::string& text) {
  std::string encoded;
  for (unsigned char byte_val : text) {
    auto it = byte_to_unicode_.find(byte_val);
    if (it != byte_to_unicode_.end()) {
      encoded += codepointToUtf8(it->second);
    } else {
      encoded += static_cast<char>(byte_val);
    }
  }
  return encoded;
}

std::string CT2Backend::byteLevelDecode(const std::string& text) {
  std::vector<uint8_t> bytes;
  size_t pos = 0;

  while (pos < text.size()) {
    uint32_t codepoint = 0;
    size_t char_len = 1;
    const unsigned char c = static_cast<unsigned char>(text[pos]);

    if ((c & 0xF8) == 0xF0 && pos + 3 < text.size()) {
      codepoint = (c & 0x07) << 18;
      codepoint |= (static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 12;
      codepoint |= (static_cast<unsigned char>(text[pos + 2]) & 0x3F) << 6;
      codepoint |= (static_cast<unsigned char>(text[pos + 3]) & 0x3F);
      char_len = 4;
    } else if ((c & 0xF0) == 0xE0 && pos + 2 < text.size()) {
      codepoint = (c & 0x0F) << 12;
      codepoint |= (static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 6;
      codepoint |= (static_cast<unsigned char>(text[pos + 2]) & 0x3F);
      char_len = 3;
    } else if ((c & 0xE0) == 0xC0 && pos + 1 < text.size()) {
      codepoint = (c & 0x1F) << 6;
      codepoint |= (static_cast<unsigned char>(text[pos + 1]) & 0x3F);
      char_len = 2;
    } else {
      codepoint = c;
      char_len = 1;
    }

    auto it = unicode_to_byte_.find(codepoint);
    if (it != unicode_to_byte_.end()) {
      bytes.push_back(it->second);
    } else {
      for (size_t i = 0; i < char_len; ++i) {
        bytes.push_back(static_cast<uint8_t>(text[pos + i]));
      }
    }

    pos += char_len;
  }

  return std::string(bytes.begin(), bytes.end());
}

size_t CT2Backend::utf8CharLength(unsigned char c) {
  if ((c & 0xF8) == 0xF0) {
    return 4;
  }
  if ((c & 0xF0) == 0xE0) {
    return 3;
  }
  if ((c & 0xE0) == 0xC0) {
    return 2;
  }
  return 1;
}

}  // namespace predict
}  // namespace rime
