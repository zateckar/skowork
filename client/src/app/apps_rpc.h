// The Apps view's host methods (client/local-apps Apps View), UI only: apps.list, apps.open, apps.confirm, apps.stop,
// apps.delete and apps.reveal. Asking the agent to change an app is a new Thread sent through inbox.send.
#pragma once

#include "apps/apps.h"
#include "host/rpc.h"

#include <functional>
#include <string>

namespace wb {

struct AppsRpcEnv {
    std::function<apps::AppManager*()> apps;  // null while the agent is not running
    std::function<apps::AppServer*()> server;
    std::function<void(const std::string& url)> open_url;  // the user's default browser
    std::function<Status(const std::string& folder)> reveal;  // File Explorer at the folder
};

void register_apps_rpc(RpcRegistry& rpc, AppsRpcEnv env);

}  // namespace wb
