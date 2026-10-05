#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace aes67 {

class WebUi {
 public:
  WebUi(HINSTANCE hinst, std::function<void(const std::string&)> on_message);
  ~WebUi();

  WebUi(const WebUi&) = delete;
  WebUi& operator=(const WebUi&) = delete;

  void Show();
  bool visible() const;
  void Post(const std::string& json);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}
