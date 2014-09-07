#include <util/util.h>
#include <queue>
#include <streams/stream_functions_lockfree.h>

typedef std::unordered_map<std::string, streams_t> by_stream_map_t;

streams_t by_lockfree(const by_keys_t & keys, const by_stream_t stream) {

  auto atom_streams = make_shared_atom<by_stream_map_t>();

  return create_stream(

    [=](forward_fn_t forward, e_t e) mutable {

      if (keys.empty()) {
        return;
      }

      std::string key;
      for (const auto & k: keys) {
        key += event_str_value(e, k) + " ";
      }

      auto fw_stream = create_stream(
        [=](forward_fn_t, e_t e)
        {
          forward(e);
        }
      );

      map_on_sync_insert<std::string, streams_t>(
          atom_streams,
          key,
          [&]() { return stream() >> fw_stream;},
          [&](streams_t & c) { push_event(c, e); }
      );

  });
}

typedef std::unordered_map<std::string, Event> coalesce_events_t;

streams_t coalesce_lockfree(fold_fn_t fold) {

  auto coalesce = make_shared_atom<coalesce_events_t>();

  return create_stream(
    [=](forward_fn_t forward, e_t e) mutable
    {

      std::vector<Event> expired_events;

      coalesce->update(

        [&](const coalesce_events_t & events) {

          coalesce_events_t c;

          std::string key(e.host() + " " +  e.service());

          expired_events.clear();

          c[key] = e;

          for (const auto & it : events) {

            if (key == it.first) {
              continue;
            }

            if (expired_(it.second)) {
              expired_events.push_back(it.second);
            } else {
              c.insert({it.first, it.second});
            }
          }

          return c;
        },

        [&](const coalesce_events_t &, const coalesce_events_t & curr) {

          if (!expired_events.empty()) {
            forward(fold(expired_events));
          }

          events_t events;
          for (const auto & it : curr) {
            events.push_back(it.second);
          }

          if (!events.empty()) {
            forward(fold(events));
          }
        }
      );
    }
  );
}

typedef std::vector<boost::optional<Event>> project_events_t;

streams_t project_lockfree(const predicates_t predicates, fold_fn_t fold) {

  auto events = make_shared_atom<project_events_t>({predicates.size(),
                                                    boost::none});
  return create_stream(

    [=](forward_fn_t forward, e_t e) mutable {

      std::vector<Event> expired_events;

      events->update(

        [&](const project_events_t & curr) {

          auto c(curr);

          expired_events.clear();

          bool match = false;
          for (size_t i = 0; i < predicates.size(); i++) {

            if (!match && predicates[i](e)) {

              c[i] = e;
              match = true;

            } else {

              if (c[i] && expired_(*c[i])) {

                expired_events.push_back(*c[i]);
                c[i].reset();

              }

            }
          }

          return c;
        },

        [&](const project_events_t &, const project_events_t & curr) {
          forward(fold(expired_events));

          events_t events;
          for (const auto & ev : curr) {
            if (ev) {
              events.push_back(*ev);
            }
          }

          if (!events.empty()) {
            forward(fold(events));
          }
        }

      );
    }
  );
}

streams_t changed_state_lockfree(std::string initial) {
  auto state = make_shared_atom<std::string>(initial);

  return create_stream(
    [=](forward_fn_t forward, e_t e) {

      state->update(
        e.state(),
        [&](const std::string & prev, const std::string &)
        {
          if (prev != e.state()) {
            forward(e);
          }
        }
      );

    });
}

streams_t moving_event_window_lockfree(size_t n, fold_fn_t fold) {

  auto window = make_shared_atom<std::list<Event>>();

  return create_stream(

    [=](forward_fn_t forward, e_t e) {

      window->update(

        [&](const std::list<Event> w)
        {

            auto c(w);

            c.push_back(e);
            if (c.size() == (n + 1)) {
              c.pop_front();
            }

            return std::move(c);
        },

        [&](const std::list<Event> &, const std::list<Event> & curr) {

          forward(fold({begin(curr), end(curr)}));

        }

      );
    });
}

streams_t fixed_event_window_lockfree(size_t n, fold_fn_t fold) {

  auto window = make_shared_atom<std::list<Event>>();

  return create_stream(

    [=](forward_fn_t forward, e_t e) {

      std::list<Event> event_list;
      bool forward_events;

      window->update(

        [&](const std::list<Event> w) -> std::list<Event>
        {

          event_list = conj(w, e);

          if ((forward_events = (event_list.size() == n))) {
            return {};
          } else {
            return event_list;
          }

        },

        [&](const std::list<Event> &, const std::list<Event> &) {

          if (forward_events) {
            forward(fold({begin(event_list), end(event_list)}));
          }

        }
      );
    }
  );
}

struct event_time_cmp
{
  bool operator() (const Event & lhs, const Event & rhs) const
  {
    return (lhs.time() > rhs.time());
  }
};

typedef struct {
  std::priority_queue<Event, std::vector<Event>, event_time_cmp> pq;
  time_t max{0};
} moving_time_window_t;

streams_t moving_time_window_lockfree(time_t dt, fold_fn_t fold) {

  auto window = make_shared_atom<moving_time_window_t>();

  return create_stream(

    [=](forward_fn_t forward, e_t e) {

      window->update(

        [&](const moving_time_window_t  w )
        {

          auto c(w);

          if (!e.has_time()) {
            return c;
           }

          if (e.time() > w.max) {
            c.max = e.time();
          }

          c.pq.push(e);

          if (c.max < dt) {
            return c;
          }

          while (!c.pq.empty() && c.pq.top().time() <= (c.max - dt)) {
            c.pq.pop();
          }

          return c;
        },

        [&](const moving_time_window_t &, const moving_time_window_t & curr) {

          auto c(curr);

           std::vector<Event> events;

           while (!c.pq.empty()) {
             events.push_back(c.pq.top());
             c.pq.pop();
           }

           forward(fold(events));
        }
      );
  });
}

typedef struct {
  std::priority_queue<Event, std::vector<Event>, event_time_cmp> pq;
  time_t start{0};
  time_t max{0};
  bool started{false};
} fixed_time_window_t;

streams_t fixed_time_window_lockfree(time_t dt, fold_fn_t fold) {

  auto window = make_shared_atom<fixed_time_window_t>();

  return create_stream(

    [=](forward_fn_t forward, e_t e) {

      std::vector<Event> flush;

      window->update(

        [&](const fixed_time_window_t  w)
        {
          auto c(w);
          flush.clear();

          // Ignore event with no time
          if (!e.has_time()) {
            return c;
          }

          if (!c.started) {
            c.started = true;
            c.start = e.time();
            c.pq.push(e);
            c.max = e.time();
            return c;
          }

          // Too old
          if (e.time() < c.start) {
            return c;
          }

          if (e.time() > c.max) {
            c.max = e.time();
          }

          time_t next_interval = c.start - (c.start % dt) + dt;

          c.pq.push(e);

          if (c.max < next_interval) {
            return c;
          }

          // We can flush a window
          while (!c.pq.empty() && c.pq.top().time() < next_interval) {
            flush.emplace_back(c.pq.top());
            c.pq.pop();
          }

          c.start = next_interval;

          return c;
        },

        [&](const fixed_time_window_t &, const fixed_time_window_t &) {
           forward(fold(flush));
        }
      );
  });
}


typedef struct {
  std::string state;
  std::vector<Event> buffer;
  time_t start{0};
} stable_t;

streams_t stable_lockfree(time_t dt) {

  auto stable = make_shared_atom<stable_t>();

  return create_stream(

    [=](forward_fn_t forward, e_t e)
    {

      std::vector<Event> flush;
      bool schedule_flush;

      stable->update(

          [&](const stable_t & s) {

            auto c(s);
            flush.clear();
            schedule_flush = false;

            if (s.state != e.state()) {
              c.start = e.time();
              c.buffer.clear();
              c.buffer.push_back(e);
              c.state = e.state();
              schedule_flush = true;
              return c;
            }

            if (e.time() < c.start) {
              return c;
            }

            if (c.start + dt > e.time()) {
              c.buffer.push_back(e);
              return c;
            }

            if (!c.buffer.empty()) {
              flush = std::move(c.buffer);
            }
            flush.push_back(e);

            return c;

          },

          [&](const stable_t &, const stable_t &) {
            for (const auto & flush_event: flush) {
              forward(flush_event);
            }
          }
      );
    }
  );

}

typedef struct {
  size_t forwarded{0};
 time_t new_interval{0};
} throttle_t;

streams_t throttle_lockfree(size_t n, time_t dt) {

  auto throttled = make_shared_atom<throttle_t>();

  return create_stream(
    [=](forward_fn_t forward, e_t e) mutable {

      bool forward_event;

      throttled->update(
        [&](const throttle_t & t) {

          auto c(t);

          if (c.new_interval < e.time()) {
            c.new_interval += e.time() + dt;
            c.forwarded = 0;
          }

          forward_event = (c.forwarded < n);

          if (forward_event) {
            c.forwarded++;
          }

          return c;
        },

        [&](const throttle_t &, const throttle_t &) {

          if (forward_event) {
            forward(e);
          }

      });
    }
  );

}

typedef struct {
  double metric;
  int64_t time;
  bool initialized;
} ddt_prev_t;

streams_t ddt_lockfree() {
  auto prev = make_shared_atom<ddt_prev_t>({0, 0, false});

  return create_stream(
    [=](forward_fn_t forward, e_t e) {

      ddt_prev_t curr({metric_to_double(e), e.time(), true});

      prev->update(
        curr,
        [&](const ddt_prev_t & prev, const ddt_prev_t &)
        {

          if (!prev.initialized) {
            return;
          }

          auto dt = e.time() - prev.time;

          if (dt == 0) {
            return;
          }

          Event ne(e);
          set_metric(ne, (curr.metric - prev.metric) / dt);

          forward(ne);
        }
      );

    });
}

