#include "app/Service.h"
#include "app/Config.h"
#include "api/Facade.h"
#include "common/HttpServer.h"
#include "common/Log.h"
#include <atomic>
#include <memory>

namespace service {
namespace {
struct ServiceState {
    std::atomic<bool> running{false};
    std::unique_ptr<HttpServer> server;
};
ServiceState& state() { static ServiceState value; return value; }
}
bool running() { return state().running.load(); }

bool start(std::string& error) {
    if (state().running.load()) return true;
    auto& cfg = Config::instance();
    if (!state().server) state().server = std::make_unique<HttpServer>();
    apiRegisterRoutes(*state().server);
    std::string host = cfg.allowLan ? "0.0.0.0" : cfg.serviceHost;
    if (!state().server->start(host, cfg.servicePort, error)) {
        LOG_E("服务启动失败: %s", error.c_str());
        return false;
    }
    state().running = true;
    LOG_I("服务已启动（%s:%d，局域网=%s）", host.c_str(), cfg.servicePort, cfg.allowLan ? "开" : "关");
    return true;
}

void stop() {
    if (!state().server) return;
    state().server->stop();
    state().running = false;
    LOG_I("服务已停止");
}
} // namespace service
