#pragma once
#include <httplib.h>
#include "app_context.hpp"

namespace hermes {

void register_routes(httplib::Server& svr, AppContext& ctx);

} // namespace hermes
