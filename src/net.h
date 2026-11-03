#pragma once
// Fetching a web page or file: over WinHTTP on Windows, and with curl elsewhere, as there's no TLS library in
// this program. Waits on the network, so it belongs on a thread of its own.
#include <string>

namespace net {

// GETs an http or https URL. True with the body if the server answered 200; false with a message if not.
// Anything longer than maxBytes is an error rather than a body.
bool httpGet(const std::string& url, std::string& body, std::string& error, size_t maxBytes = 4u << 20);

// %-escapes everything in s but letters, digits and -_.~, for a URL's query or a form.
std::string urlEscape(const std::string& s);

// A browser of sorts: it keeps the cookies a site sets, in memory, for as long as it lives, and follows
// redirects. One thread at a time.
class Session {
 public:
  Session();
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  // headers are extra request headers, each ending in \r\n.
  bool get(const std::string& url, std::string& body, std::string& error, const std::string& headers = "", size_t maxBytes = 4u << 20);
  // form is already escaped: a=1&b=2.
  bool post(const std::string& url, const std::string& form, std::string& body, std::string& error, const std::string& headers = "", size_t maxBytes = 4u << 20);

 private:
  bool send(const char* method, const std::string& url, const std::string& form, const std::string& headers, std::string& body, std::string& error, size_t maxBytes);
  void* handle_ = nullptr;  // WinHTTP's session, which keeps the cookies
  std::string jar_;         // curl's cookie file, elsewhere
};

}  // namespace net
