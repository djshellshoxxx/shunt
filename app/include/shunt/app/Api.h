// Route table for the HTTP server (RS-07 section 8).
#pragma once
#include "shunt/app/Core.h"
#include "shunt/app/Http.h"

namespace shunt::app {

HttpResponse handleRequest(Core& core, const HttpRequest& req);

} // namespace shunt::app
