#ifndef CORE_REAL_CORE_H
#define CORE_REAL_CORE_H

#include <core/core.h>
#include <streams/stream_functions.h>
#include <async/async_loop.h>
#include <pub_sub/pub_sub.h>
#include <index/index.h>
#include <riemann_tcp_pool.h>
#include <riemann_udp_pool.h>
#include <websocket_pool.h>
#include <external/graphite.h>
#include <external/rieman_tcp_client.h>
#include <scheduler/scheduler.h>
#include <config/config.h>

class real_core : public core_interface {
public:

  real_core(const config &);
  void start();
  void add_stream(std::shared_ptr<streams_t> stream);
  void send_to_graphite(const std::string host, const int port,
                        const Event & event);
  void forward(const std::string, const int port, const Event & event);
  std::shared_ptr<class index> index();
  std::shared_ptr<class scheduler> sched();
  std::shared_ptr<external> externals();

private:
  config config_;

  std::shared_ptr<class main_async_loop> main_loop_;
  std::shared_ptr<class scheduler> scheduler_;
  std::shared_ptr<class streams> streams_;
  std::shared_ptr<class pub_sub> pubsub_;
  std::shared_ptr<class index> index_;
  std::shared_ptr<class riemann_tcp_pool> tcp_server_;
  std::shared_ptr<class riemann_udp_pool> udp_server_;
  std::shared_ptr<class websocket_pool> ws_server_;
  std::shared_ptr<class graphite> graphite_;
  std::shared_ptr<class riemann_tcp_client> riemann_client_;
  std::shared_ptr<class external> externals_;

  std::vector<std::shared_ptr<streams_t>> sh_streams_;

};

#endif
