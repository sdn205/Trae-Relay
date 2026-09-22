// main.cpp - 入口：单实例互斥、DPI 初始化、命令行（--tray / --serve / --config / --version）
#include "app/AutoStart.h"
#include "app/Config.h"
#include "window/MainWindow.h"
#include "app/Service.h"
#include "accounts/AccountPool.h"
#include "common/Log.h"
#include "common/Settings.h"
#include "accounts/Storage.h"
#include "upstream/ModelCatalog.h"
#include "Version.h"
#include <windows.h>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    bool startTray = false;
    bool serveHeadless = false;
    bool showVersion = false;
    std::string configPath;
    std::string snapshotDir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tray") startTray = true;
        else if (a == "--serve") serveHeadless = true;
        else if (a == "--version" || a == "-v") showVersion = true;
        else if (a == "--config" && i + 1 < argc) configPath = argv[++i];
        else if (a == "--snapshot" && i + 1 < argc) snapshotDir = argv[++i];
    }
    if (showVersion) {
        printf("TraeRelay %s\n", TRAERELAY_VERSION);
        return 0;
    }

    // 快照模式：渲染四页 PNG 后退出。必须在单实例互斥之前分流——
    // 互斥命中会 FindWindow + SetForegroundWindow 激活已有窗口（抢焦点），快照绝不触发。
    if (!snapshotDir.empty()) {
        auto& snapCfg = Config::instance();
        std::string err;
        snapCfg.load(configPath, err); // 出错静默用默认，不弹窗（无人值守）
        settings::set(makeCoreSettings(snapCfg));
        std::string logDir = snapCfg.logDir.empty() ? exeDir() + "\\logs" : snapCfg.logDir;
        LogLevel lv = snapCfg.logLevel == "trace" ? LogLevel::Trace
                      : snapCfg.logLevel == "debug" ? LogLevel::Debug
                      : snapCfg.logLevel == "warn" ? LogLevel::Warn
                      : snapCfg.logLevel == "error" ? LogLevel::Error : LogLevel::Info;
        logInit(logDir, lv, snapCfg.logRetainDays, snapCfg.loggingEnabled);
        if (snapCfg.accountsAutoDiscover) AccountPool::instance().autoDiscover();
        return ui::runSnapshot(snapshotDir);
    }

    // 单实例互斥
    auto& cfg = Config::instance();
    HANDLE mutex = nullptr;
    {
        std::string err;
        if (!cfg.load(configPath, err)) {
            // 配置损坏：弹窗（GUI 模式）或 stderr
            fprintf(stderr, "[config] %s\n", err.c_str());
            MessageBoxA(nullptr, ("配置文件有误，已使用默认配置：\n" + err).c_str(), "Trae Relay", MB_ICONWARNING);
        }
    }
    // 显式 --tray 或启动设置勾选时，首次启动直接进入托盘。
    startTray = startTray || cfg.startMinimizedToTray;
    // 配置就绪：映射为 core 不可变快照并推送（此后 core 只读快照，不反向依赖 app 配置）
    settings::set(makeCoreSettings(cfg));
    if (cfg.singleInstance) {
        mutex = CreateMutexA(nullptr, TRUE, "Local\\TraeRelay.SingleInstance");
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            // 激活已有窗口
            HWND w = FindWindowW(L"TraeRelayMain", nullptr);
            if (w) {
                ShowWindow(w, SW_SHOW);
                ShowWindow(w, SW_RESTORE);
                SetForegroundWindow(w);
            }
            if (mutex) CloseHandle(mutex);
            return 0;
        }
    }

    // 日志：默认写入 exe 同目录 logs（便携化），配置中显式指定 dir 时以配置为准
    std::string logDir = cfg.logDir.empty() ? exeDir() + "\\logs" : cfg.logDir;
    LogLevel lv = cfg.logLevel == "trace" ? LogLevel::Trace : cfg.logLevel == "debug" ? LogLevel::Debug
                  : cfg.logLevel == "warn" ? LogLevel::Warn : cfg.logLevel == "error" ? LogLevel::Error
                                                                                      : LogLevel::Info;
    logInit(logDir, lv, cfg.logRetainDays, cfg.loggingEnabled);
    LOG_I("=== Trae Relay 启动（pid=%lu，tray=%d，serve=%d）===", GetCurrentProcessId(), startTray, serveHeadless);
    LOG_I("生效版本头: %s / %s", cfg.ideVersion.c_str(), cfg.ideVersionCode.c_str());

    // 账号发现
    if (cfg.accountsAutoDiscover) AccountPool::instance().autoDiscover();
    ModelCatalog::instance().start();

    if (serveHeadless) {
        // 无 UI 模式：直接起服务（供测试/服务化）
        // 服务状态接口启动时即提供真实积分，避免首个查询一直显示 -1。
        AccountPool::instance().refreshCredits();
        std::string err;
        if (!service::start(err)) {
            fprintf(stderr, "[service] %s\n", err.c_str());
            return 1;
        }
        printf("Trae Relay listening on %s:%d (apiKey=%s)\n", cfg.serviceHost.c_str(), cfg.servicePort,
               cfg.apiKey.c_str());
        for (;;) Sleep(1000);
    }

    int rc = ui::runGui(startTray);
    ModelCatalog::instance().stop();
    if (mutex) CloseHandle(mutex);
    LOG_I("=== Trae Relay 退出 ===");
    return rc;
}
