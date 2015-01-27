/**
* Redox - A modern, asynchronous, and wicked fast C++11 client for Redis
*
*    https://github.com/hmartiro/redox
*
* Copyright 2015 - Hayk Martirosyan <hayk.mart at gmail dot com>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#pragma once

#include <iostream>
#include <functional>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <string>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libev.h>

#include "utils/logger.hpp"
#include "command.hpp"

namespace redox {

static const std::string REDIS_DEFAULT_HOST = "localhost";
static const int REDIS_DEFAULT_PORT = 6379;

/**
* Redox intro here.
*/
class Redox {

public:

  // Connection states
  static const int NOT_YET_CONNECTED = 0;
  static const int CONNECTED = 1;
  static const int DISCONNECTED = 2;
  static const int CONNECT_ERROR = 3;
  static const int DISCONNECT_ERROR = 4;

  // ------------------------------------------------
  // Core public API
  // ------------------------------------------------

  /**
  * Initializes everything, connects over TCP to a Redis server.
  */
  Redox(
    const std::string& host = REDIS_DEFAULT_HOST,
    const int port = REDIS_DEFAULT_PORT,
    std::function<void(int)> connection_callback = nullptr,
    std::ostream& log_stream = std::cout,
    log::Level log_level = log::Info
  );

  /**
  * Initializes everything, connects over unix sockets to a Redis server.
  */
  Redox(
    const std::string& path,
    std::function<void(int)> connection_callback,
    std::ostream& log_stream = std::cout,
    log::Level log_level = log::Info
  );

  /**
  * Disconnects from the Redis server, shuts down the event loop, and cleans up.
  * Internally calls disconnect() and wait().
  */
  ~Redox();

  /**
  * Connects to Redis and starts an event loop in a separate thread. Returns
  * true once everything is ready, or false on failure.
  */
  bool connect();

  /**
  * Signal the event loop thread to disconnect from Redis and shut down.
  */
  void disconnect();

  /**
  * Blocks until the event loop exits and disconnection is complete, then returns.
  * Usually no need to call manually as it is handled in the destructor.
  */
  void wait();

  /**
  * Asynchronously runs a command and invokes the callback when a reply is
  * received or there is an error. The callback is guaranteed to be invoked
  * exactly once. The Command object is provided to the callback, and the
  * memory for it is automatically freed when the callback returns.
  */
  template<class ReplyT>
  void command(
      const std::string& cmd,
      const std::function<void(Command<ReplyT>&)>& callback = nullptr
  );

  /**
  * Asynchronously runs a command and ignores any errors or replies.
  */
  void command(const std::string& cmd);

  /**
  * Synchronously runs a command, returning the Command object only once
  * a reply is received or there is an error. The user is responsible for
  * calling the Command object's .free() method when done with it.
  */
  template<class ReplyT>
  Command<ReplyT>& commandSync(const std::string& cmd);

  /**
  * Synchronously runs a command, returning only once a reply is received
  * or there's an error. Returns true on successful reply, false on error.
  */
  bool commandSync(const std::string& cmd);

  /**
  * Creates an asynchronous command that is run every [repeat] seconds,
  * with the first one run in [after] seconds. If [repeat] is 0, the
  * command is run only once.
  */
  template<class ReplyT>
  Command<ReplyT>& commandLoop(
      const std::string& cmd,
      const std::function<void(Command<ReplyT>&)>& callback,
      double repeat,
      double after = 0.0
  );

  // ------------------------------------------------
  // Wrapper methods for convenience only
  // ------------------------------------------------

  /**
  * Redis GET command wrapper - return the value for the given key, or throw
  * an exception if there is an error. Blocking call.
  */
  std::string get(const std::string& key);

  /**
  * Redis SET command wrapper - set the value for the given key. Return
  * true if succeeded, false if error. Blocking call.
  */
  bool set(const std::string& key, const std::string& value);

  /**
  * Redis DEL command wrapper - delete the given key. Return true if succeeded,
  * false if error. Blocking call.
  */
  bool del(const std::string& key);

  /**
  * Redis PUBLISH command wrapper - publish the given message to all subscribers.
  * Non-blocking call.
  */
  void publish(const std::string& topic, const std::string& msg);

  // ------------------------------------------------
  // Public members
  // ------------------------------------------------

  // Hiredis context, left public to allow low-level access
  redisAsyncContext * ctx_;

  // Redox server over TCP
  const std::string host_;
  const int port_;

  // Redox server over unix
  const std::string path_;

  // Logger
  log::Logger logger_;

private:

  // ------------------------------------------------
  // Private methods
  // ------------------------------------------------

  // One stop shop for creating commands. The base of all public
  // methods that run commands.
  template<class ReplyT>
  Command<ReplyT>& createCommand(
      const std::string& cmd,
      const std::function<void(Command<ReplyT>&)>& callback = nullptr,
      double repeat = 0.0,
      double after = 0.0,
      bool free_memory = true
  );

  // Setup code for the constructors
  void init_ev();
  void init_hiredis();

  // Callbacks invoked on server connection/disconnection
  static void connectedCallback(const redisAsyncContext* c, int status);
  static void disconnectedCallback(const redisAsyncContext* c, int status);

  // Main event loop, run in a separate thread
  void runEventLoop();

  // Return the command map corresponding to the templated reply type
  template<class ReplyT>
  std::unordered_map<long, Command<ReplyT>*>& getCommandMap();

  // Return the given Command from the relevant command map, or nullptr if not there
  template<class ReplyT>
  Command<ReplyT>* findCommand(long id);

  // Send all commands in the command queue to the server
  static void processQueuedCommands(struct ev_loop* loop, ev_async* async, int revents);

  // Process the command with the given ID. Return true if the command had the
  // templated type, and false if it was not in the map of that type.
  template<class ReplyT>
  bool processQueuedCommand(long id);

  // Callback given to libev for a Command's timer watcher, to be processed in
  // a deferred or looping state
  template<class ReplyT>
  static void submitCommandCallback(struct ev_loop* loop, ev_timer* timer, int revents);

  // Submit an asynchronous command to the Redox server. Return
  // true if succeeded, false otherwise.
  template<class ReplyT>
  static bool submitToServer(Command<ReplyT>* c);

  // Callback given to hiredis to invoke when a reply is received
  template<class ReplyT>
  static void commandCallback(redisAsyncContext* ctx, void* r, void* privdata);

  // Invoked by Command objects when they are completed. Removes them
  // from the command map.
  template<class ReplyT>
  void remove_active_command(const long id) {
    std::lock_guard<std::mutex> lg1(command_map_guard_);
    getCommandMap<ReplyT>().erase(id);
    commands_deleted_ += 1;
  }

  // ------------------------------------------------
  // Private members
  // ------------------------------------------------

  // Manage connection state
  std::atomic_int connect_state_ = {NOT_YET_CONNECTED};
  std::mutex connect_lock_;
  std::condition_variable connect_waiter_;

  // User connect/disconnect callbacks
  std::function<void(int)> user_connection_callback_;

  // Dynamically allocated libev event loop
  struct ev_loop* evloop_;

  // Asynchronous watchers
  ev_async watcher_command_; // For processing commands
  ev_async watcher_stop_; // For breaking the loop

  // Track of Command objects allocated. Also provides unique Command IDs.
  std::atomic_long commands_created_ = {0};
  std::atomic_long commands_deleted_ = {0};

  // Separate thread to have a non-blocking event loop
  std::thread event_loop_thread_;

  // Variable and CV to know when the event loop starts running
  std::atomic_bool running_ = {false};
  std::mutex running_waiter_lock_;
  std::condition_variable running_waiter_;

  // Variable and CV to know when the event loop stops running
  std::atomic_bool to_exit_ = {false}; // Signal to exit
  std::atomic_bool exited_ = {false}; // Event thread exited
  std::mutex exit_waiter_lock_;
  std::condition_variable exit_waiter_;

  // Maps of each Command, fetchable by the unique ID number
  // In C++14, member variable templates will replace all of these types
  // with a single templated declaration
  // ---------
  // template<class ReplyT>
  // std::unordered_map<long, Command<ReplyT>*> commands_;
  // ---------
  std::unordered_map<long, Command<redisReply*>*> commands_redis_reply_;
  std::unordered_map<long, Command<std::string>*> commands_string_;
  std::unordered_map<long, Command<char*>*> commands_char_p_;
  std::unordered_map<long, Command<int>*> commands_int_;
  std::unordered_map<long, Command<long long int>*> commands_long_long_int_;
  std::unordered_map<long, Command<std::nullptr_t>*> commands_null_;
  std::unordered_map<long, Command<std::vector<std::string>>*> commands_vector_string_;
  std::unordered_map<long, Command<std::set<std::string>>*> commands_set_string_;
  std::unordered_map<long, Command<std::unordered_set<std::string>>*> commands_unordered_set_string_;
  std::mutex command_map_guard_; // Guards access to all of the above

  // Command IDs pending to be sent to the server
  std::queue<long> command_queue_;
  std::mutex queue_guard_;

  // Commands use this method to deregister themselves from Redox,
  // give it access to private members
  template<class ReplyT>
  friend void Command<ReplyT>::freeCommand(Command<ReplyT>* c);
};

// ------------------------------------------------
// Implementation of templated methods
// ------------------------------------------------

template<class ReplyT>
Command<ReplyT>& Redox::createCommand(
    const std::string& cmd,
    const std::function<void(Command<ReplyT>&)>& callback,
    double repeat,
    double after,
    bool free_memory
) {

  if(!running_) {
    throw std::runtime_error("[ERROR] Need to connect Redox before running commands!");
  }

  commands_created_ += 1;
  auto* c = new Command<ReplyT>(
      this, commands_created_, cmd,
      callback, repeat, after, free_memory, logger_
  );

  std::lock_guard<std::mutex> lg(queue_guard_);
  std::lock_guard<std::mutex> lg2(command_map_guard_);

  getCommandMap<ReplyT>()[c->id_] = c;
  command_queue_.push(c->id_);

  // Signal the event loop to process this command
  ev_async_send(evloop_, &watcher_command_);

  return *c;
}

template<class ReplyT>
void Redox::command(
    const std::string& cmd,
    const std::function<void(Command<ReplyT>&)>& callback
) {
  createCommand(cmd, callback);
}

template<class ReplyT>
Command<ReplyT>& Redox::commandLoop(
    const std::string& cmd,
    const std::function<void(Command<ReplyT>&)>& callback,
    double repeat,
    double after
) {
  return createCommand(cmd, callback, repeat, after);
}

template<class ReplyT>
Command<ReplyT>& Redox::commandSync(const std::string& cmd) {
  auto& c = createCommand<ReplyT>(cmd, nullptr, 0, 0, false);
  c.wait();
  return c;
}

} // End namespace redis
