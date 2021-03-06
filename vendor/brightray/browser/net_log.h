// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef BROWSER_NET_LOG_H_
#define BROWSER_NET_LOG_H_

#include "base/files/scoped_file.h"
#include "net/log/net_log.h"

namespace net {
class FileNetLogObserver;
}

namespace brightray {

class NetLog : public net::NetLog {
 public:
  NetLog();
  ~NetLog() override;

  void StartLogging();

 private:
  base::ScopedFILE log_file_;
  std::unique_ptr<net::FileNetLogObserver> file_net_log_observer_;

  DISALLOW_COPY_AND_ASSIGN(NetLog);
};

}  // namespace brightray

#endif  // BROWSER_NET_LOG_H_
