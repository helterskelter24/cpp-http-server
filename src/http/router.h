#pragma once

#include <functional>
#include <string>
#include <vector>

#include "http/request.h"
#include "http/response.h"

namespace hs {

using Handler = std::function<Response(const Request&)>;

// Maps (method, path pattern) to a handler. Register every route before the
// server starts; after that dispatch() only reads, so many threads may call it
// at once without locking.
class Router {
public:
    // Pattern segments are literals or ":name" parameters, e.g. "/api/users/:id".
    void add(Method method, std::string pattern, Handler handler);

    // Used for GET/HEAD requests whose path matches no route (static files).
    void set_fallback(Handler handler);

    // Runs the matching handler and fills req.params. Never throws: a handler
    // exception becomes a 500.
    Response dispatch(Request& req) const;

private:
    struct Route {
        Method method;
        std::vector<std::string> segments;  // pre-split pattern; ":x" marks a parameter
        Handler handler;
    };

    std::vector<Route> routes_;
    Handler fallback_;
};

}  // namespace hs
