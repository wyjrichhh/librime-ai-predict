//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <glog/logging.h>
#include <rime_api.h>
#include <rime/common.h>
#include <rime/registry.h>
#include <rime/service.h>

#include <cstdlib>
#include <string>

#include "predict_filter.h"
#include "predict_translator.h"

namespace {

bool owns_logging = false;

// Merged plugins can share the host's glog instance. Reuse it without taking
// ownership; finalize must not shut down logging initialized by the host.
// When an external plugin has a separate glog instance, initialize it with a
// distinct filename, following the guidance in rime/librime#983 and #984.
// The frontend supplies RIME_LOG_DIR or GOOGLE_LOG_DIR when applicable.
void EnsurePluginLoggingInitialized() {
  if (google::IsGoogleLoggingInitialized()) {
    return;
  }

  if (const char* dir = std::getenv("RIME_LOG_DIR")) {
    if (dir[0] != '\0') {
      FLAGS_log_dir = dir;
    }
  } else if (const char* dir = std::getenv("GOOGLE_LOG_DIR")) {
    if (dir[0] != '\0') {
      FLAGS_log_dir = dir;
    }
  }

  // app_name must outlive this call (glog keeps it by pointer), hence static.
  static std::string app_name = [] {
    const auto& host = rime::Service::instance().deployer().app_name;
    return host.empty() ? std::string("rime.ai_predict")
                        : host + ".ai_predict";
  }();

  google::SetLogFilenameExtension(".log");
  google::SetLogSymlink(google::GLOG_INFO, app_name.c_str());
  google::SetLogSymlink(google::GLOG_WARNING, app_name.c_str());
  google::SetLogSymlink(google::GLOG_ERROR, app_name.c_str());
  FLAGS_logfile_mode = 0600;
  google::InitGoogleLogging(app_name.c_str());
  owns_logging = true;
}

}  // namespace

static void rime_ai_predict_initialize() {
  EnsurePluginLoggingInitialized();
  LOG(INFO) << "registering components from module 'ai_predict'.";
  rime::Registry& r = rime::Registry::instance();
  r.Register("ai_predict_translator",
             new rime::Component<rime::predict::PredictTranslator>);
  r.Register("ai_predict_filter",
             new rime::Component<rime::predict::PredictFilter>);
}

static void rime_ai_predict_finalize() {
  if (owns_logging) {
    google::ShutdownGoogleLogging();
    owns_logging = false;
  }
}

RIME_REGISTER_MODULE(ai_predict)
