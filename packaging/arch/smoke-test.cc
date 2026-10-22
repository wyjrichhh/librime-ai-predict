// Copyright RIME Developers
// Distributed under the BSD License

#include <rime_api.h>

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> refreshed{false};

void Notify(void*, RimeSessionId, const char* type, const char* value) {
  if (std::strcmp(type, "property") == 0 &&
      std::strcmp(value, "_refresh_ui") == 0) {
    refreshed.store(true, std::memory_order_release);
  }
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Require(argc == 6,
            "usage: smoke-test SHARED_DATA USER_DATA SCHEMA MODEL FRONTEND");
    const std::string shared_data = argv[1];
    const std::string user_data = argv[2];
    fs::create_directories(user_data);
    fs::copy_file(argv[3], fs::path(user_data) / "ai_pinyin.schema.yaml",
                  fs::copy_options::overwrite_existing);
    {
      std::ofstream defaults(fs::path(user_data) / "default.custom.yaml");
      defaults << "patch:\n  schema_list:\n    - schema: ai_pinyin\n";
      Require(defaults.good(), "cannot write test schema list");
      std::ofstream custom(fs::path(user_data) / "ai_pinyin.custom.yaml");
      custom << "patch:\n  ai_predict/model_path: " << std::quoted(argv[4])
             << "\n";
      Require(custom.good(), "cannot write test model path");
    }

    // Resolve the compiled frontend against this test process's bundled Rime.
    // This checks ELF dependencies; it does not exercise Fcitx's desktop UI.
    void* frontend = dlopen(argv[5], RTLD_NOW | RTLD_LOCAL);
    if (!frontend) {
      throw std::runtime_error(dlerror());
    }
    std::cout << "Frontend ELF load: OK\n";

    RimeApi* api = rime_get_api();
    RimeTraits traits{};
    RIME_STRUCT_INIT(RimeTraits, traits);
    traits.shared_data_dir = shared_data.c_str();
    traits.user_data_dir = user_data.c_str();
    traits.app_name = "rime.ai-predict-smoke";
    traits.log_dir = "";
    traits.min_log_level = 0;
    api->setup(&traits);

    // Reinitialization also checks that the plugin leaves host-owned glog alive.
    for (int cycle = 0; cycle < 2; ++cycle) {
      api->initialize(&traits);
      Require(api->find_module("ai_predict") != nullptr,
              "AI module missing from bundled librime");
      api->set_notification_handler(Notify, nullptr);
      api->start_maintenance(true);
      api->join_maintenance_thread();

      auto session = api->create_session();
      Require(session != 0, "cannot create Rime session");
      Require(api->select_schema(session, "ai_pinyin"),
              "cannot deploy or select AI Pinyin");
      refreshed.store(false, std::memory_order_release);
      for (char key : std::string("jintiantianqihenhao")) {
        api->process_key(session, key, 0);
      }
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(30);
      while (!refreshed.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      Require(refreshed.load(std::memory_order_acquire),
              "AI inference did not publish an asynchronous refresh");
      char prediction[4096] = {};
      api->get_property(session, "ai_predict/text", prediction,
                         sizeof(prediction));
      Require(prediction[0] != '\0', "AI prediction is empty");

      RimeContext context{};
      RIME_STRUCT_INIT(RimeContext, context);
      Require(api->get_context(session, &context), "cannot read candidates");
      bool found = false;
      for (int i = 0; i < context.menu.num_candidates; ++i) {
        const auto& candidate = context.menu.candidates[i];
        std::cout << "candidate " << i << ": " << candidate.text << '\n';
        found = found || std::strcmp(candidate.text, prediction) == 0;
      }
      api->free_context(&context);
      Require(found, "AI prediction missing from candidate menu");
      Require(api->commit_composition(session), "cannot commit composition");
      RimeCommit commit{};
      RIME_STRUCT_INIT(RimeCommit, commit);
      Require(api->get_commit(session, &commit), "committed text missing");
      Require(commit.text && commit.text[0], "committed text is empty");
      std::cout << "cycle " << cycle + 1 << ": prediction=" << prediction
                << "; committed=" << commit.text << '\n';
      api->free_commit(&commit);
      api->destroy_session(session);
      api->finalize();
    }
    dlclose(frontend);
    std::cout << "Rime deployment, inference, refresh, commit and reload: OK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Smoke test failed: " << error.what() << '\n';
    return 1;
  }
}
