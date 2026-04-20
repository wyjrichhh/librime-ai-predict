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

// librime 的所有外部插件（包括官方 librime-lua）都会与 librime 主 dylib
// 各自静态链接一份 glog，从而在进程中存在两个互不可见的 glog 实例。这是
// librime 的设计选择：维护者明确不接受改 librime 主项目的链接方式
// (参见 rime/librime#983 与 #984)，并由原作者 lotem 给出官方建议——
// 「兩份 glog 實例的話肯定得要輸出到不同文件了」「需要嘗試不同的解法」。
//
// 因此插件必须自行初始化「自己这一份」glog 实例，否则插件代码里的 LOG
// 写入的是一个未初始化的 sink，全部丢失。我们的约定是：
//   - log_dir 通过环境变量 RIME_LOG_DIR / GOOGLE_LOG_DIR 由前端传入；
//   - program 名取主进程 deployer.app_name 加 ".ai_predict" 后缀，
//     从而生成 "rime.<frontend>.ai_predict.*.log.INFO" 之类的文件，
//     与主进程日志 "rime.<frontend>.*.log.INFO" 物理隔离、按目录并列。
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

  // app_name 必须长存（glog 内部按指针保存），故用静态存储。
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
  if (google::IsGoogleLoggingInitialized()) {
    google::ShutdownGoogleLogging();
  }
}

RIME_REGISTER_MODULE(ai_predict)
