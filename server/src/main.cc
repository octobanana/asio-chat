// Copyright (c) 2018 Brett Robinson
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// This is a derivative work, original copyright below:
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)

#include "chat_message.hh"

#include "json.hh"
using Json = nlohmann::json;

#include <boost/asio.hpp>
using boost::asio::ip::tcp;

#include <cstdlib>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <set>
#include <utility>
#include <unordered_map>

// passwords should obviously be hashed and salted for real use
using Users = std::unordered_map<std::string, std::string>;
Users const user_db {
  {"admin", "password"},
  {"alice", "hunter2"},
  {"rabbit", "verylate"},
  {"madhatter", "teaparty"},
};

using chat_message_queue = std::deque<chat_message>;

class chat_participant
{
public:

  virtual ~chat_participant() {}
  virtual void deliver(const chat_message& msg) = 0;

};

using chat_participant_ptr = std::shared_ptr<chat_participant>;

class chat_room
{
public:

  bool contains(std::string name)
  {
    if (participants_.find(name) == participants_.end())
    {
      return false;
    }

    return true;
  }

  void join(std::string name, chat_participant_ptr participant)
  {
    participants_.emplace(name, participant);

    for (auto msg: recent_msgs_)
    {
      participant->deliver(msg);
    }
  }

  void leave(std::string name)
  {
    participants_.erase(name);
  }

  void deliver(chat_message const& msg)
  {
    recent_msgs_.emplace_back(msg);

    while (recent_msgs_.size() > max_recent_msgs)
    {
      recent_msgs_.pop_front();
    }

    for (auto participant: participants_)
    {
      participant.second->deliver(msg);
    }
  }

  void deliver(std::string const& str)
  {
    chat_message msg {str};

    recent_msgs_.emplace_back(msg);

    while (recent_msgs_.size() > max_recent_msgs)
    {
      recent_msgs_.pop_front();
    }

    for (auto participant: participants_)
    {
      participant.second->deliver(msg);
    }
  }

  void deliver(std::string to, std::string from, std::string const& msg)
  {
    auto user = participants_.find(to);
    if (user != participants_.end())
    {
      Json jres;
      jres["type"] = "prv";
      jres["from"] = from;
      jres["msg"] = msg;

      // send a message to the user
      user->second->deliver(jres.dump());
    }
  }

private:

  int const max_recent_msgs {128};
  chat_message_queue recent_msgs_;
  std::unordered_map<std::string, chat_participant_ptr> participants_;
};

class chat_session :
  public chat_participant,
  public std::enable_shared_from_this<chat_session>
{
public:

  chat_session(tcp::socket socket, chat_room& room) :
    socket_ {std::move(socket)},
    room_ {room}
  {
  }

  void start()
  {
    do_read_header();
  }

  void deliver(chat_message const& msg)
  {
    bool write_in_progress = !write_msgs_.empty();
    write_msgs_.emplace_back(msg);

    if (! write_in_progress)
    {
      do_write();
    }
  }

  void deliver(std::string const& str)
  {
    chat_message msg {str};

    bool write_in_progress = !write_msgs_.empty();
    write_msgs_.emplace_back(msg);

    if (! write_in_progress)
    {
      do_write();
    }
  }

private:
  void do_read_header()
  {
    auto self {shared_from_this()};

    boost::asio::async_read(socket_,
      boost::asio::buffer(read_msg_.data(), chat_message::header_length),
      [this, self](boost::system::error_code ec, std::size_t /*length*/)
      {
        if (! ec && read_msg_.decode_header())
        {
          do_read_body();
        }
        else
        {
          room_.leave(user_);
        }
      }
    );
  }

  void do_read_body()
  {
    auto self(shared_from_this());

    boost::asio::async_read(socket_,
      boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
      [this, self](boost::system::error_code ec, std::size_t /*length*/)
      {
        if (! ec)
        {
          // parse json from body
          std::string req {read_msg_.body(), read_msg_.body_length()};
          Json jreq = Json::parse(req);
          std::cerr << "request: " << jreq.dump() << "\n";
          std::string type {jreq["type"].get<std::string>()};
          std::cerr << "type: " << type << "\n\n";

          if (auth_)
          {
            // switch on type and perform action
            if (type == "msg")
            {
              room_.deliver(read_msg_);
            }
            else if (type == "prv")
            {
              std::string to {jreq["to"].get<std::string>()};
              std::string msg {jreq["msg"].get<std::string>()};

              // send private message to user
              room_.deliver(to, user_, msg);
            }
            // else if (type == "")
            // {
            //   // do something else
            // }
          }
          else
          {
            if (type == "auth")
            {
              std::string user {jreq["user"].get<std::string>()};
              std::string pass {jreq["pass"].get<std::string>()};

              auto check_user = user_db.find(user);
              if (check_user != user_db.end() && check_user->first == user && check_user->second == pass && ! room_.contains(user))
              {
                auth_ = true;
                user_ = user;

                Json jres;
                jres["type"] = "srv";
                jres["str"] = "Success: logged in";

                // send a message to user
                deliver(jres.dump());

                // add user to chat room
                room_.join(user_, shared_from_this());
              }
              else
              {
                Json jres;
                jres["type"] = "srv";
                jres["str"] = "Error: incorrect user or pass, disconnecting...";

                // send just to user
                deliver(jres.dump());

                // close connection
                do_close();
              }
            }
            else
            {
              Json jres;
              jres["type"] = "srv";
              jres["str"] = "Error: please authenticate with '/auth <user> <pass>'";

              deliver(jres.dump());
            }
          }

          do_read_header();
        }
        else
        {
          room_.leave(user_);
        }
      }
    );
  }

  void do_write()
  {
    auto self(shared_from_this());

    boost::asio::async_write(socket_,
      boost::asio::buffer(write_msgs_.front().data(),
      write_msgs_.front().length()),
      [this, self](boost::system::error_code ec, std::size_t /*length*/)
      {
        if (! ec)
        {
          write_msgs_.pop_front();

          if (! write_msgs_.empty())
          {
            do_write();
          }
        }
        else
        {
          room_.leave(user_);
        }
      }
    );
  }

  void do_close()
  {
    // send a tcp shutdown
    boost::system::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_send, ec);

    if (ec)
    {
      return;
    }
  }

  tcp::socket socket_;
  chat_room& room_;
  chat_message read_msg_;
  chat_message_queue write_msgs_;
  bool auth_ {false};
  std::string user_ {};
};

class chat_server
{
public:
  chat_server(boost::asio::io_context& io_context,
    const tcp::endpoint& endpoint) :
    acceptor_ {io_context, endpoint}
  {
    do_accept();
  }

private:
  void do_accept()
  {
    acceptor_.async_accept(
      [this](boost::system::error_code ec, tcp::socket socket)
      {
        if (! ec)
        {
          std::make_shared<chat_session>(std::move(socket), room_)->start();
        }

        do_accept();
      }
    );
  }

  tcp::acceptor acceptor_;
  chat_room room_;
};

int main(int argc, char *argv[])
{
  try
  {
    if (argc < 2)
    {
      std::cerr << "Usage: chat_server <port> [<port> ...]\n";
      return 1;
    }

    boost::asio::io_context io_context;

    std::list<chat_server> servers;
    for (int i = 1; i < argc; ++i)
    {
      tcp::endpoint endpoint(tcp::v4(), std::atoi(argv[i]));
      servers.emplace_back(io_context, endpoint);
    }

    io_context.run();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
