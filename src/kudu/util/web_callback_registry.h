// Copyright 2014 Cloudera, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef KUDU_UTIL_WEB_CALLBACK_REGISTRY_H
#define KUDU_UTIL_WEB_CALLBACK_REGISTRY_H

#include <boost/function.hpp>
#include <map>
#include <string>

namespace kudu {

// Interface for registering webserver callbacks.
class WebCallbackRegistry {
 public:
  typedef std::map<std::string, std::string> ArgumentMap;

  struct WebRequest {
    // The query string, parsed into key/value argument pairs.
    ArgumentMap parsed_args;

    // The raw query string passed in the URL. May be empty.
    std::string query_string;

    // The method (POST/GET/etc).
    std::string request_method;

    // In the case of a POST, the posted data.
    std::string post_data;
  };

  typedef boost::function<void (const WebRequest& args, std::stringstream* output)>
      PathHandlerCallback;

  virtual ~WebCallbackRegistry() {}

  // Register a callback for a URL path. Path should not include the
  // http://hostname/ prefix. If is_styled is true, the page is meant to be for
  // people to look at and is styled.  If false, it is meant to be for machines to
  // scrape.  If is_on_nav_bar is true,  a link to this page is
  // printed in the navigation bar at the top of each debug page. Otherwise the
  // link does not appear, and the page is rendered without HTML headers and
  // footers.
  // The first registration's choice of is_styled overrides all
  // subsequent registrations for that URL.
  virtual void RegisterPathHandler(const std::string& path, const std::string& alias,
                                   const PathHandlerCallback& callback,
                                   bool is_styled = true, bool is_on_nav_bar = true) = 0;
};

} // namespace kudu

#endif /* KUDU_UTIL_WEB_CALLBACK_REGISTRY_H */
