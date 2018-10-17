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

  void join(chat_participant_ptr participant)
  {
    participants_.insert(participant);

    for (auto msg: recent_msgs_)
    {
      participant->deliver(msg);
    }
  }

  void leave(chat_participant_ptr participant)
  {
    participants_.erase(participant);
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
      participant->deliver(msg);
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
      participant->deliver(msg);
    }
  }

private:

  std::set<chat_participant_ptr> participants_;
  enum { max_recent_msgs = 100 };
  chat_message_queue recent_msgs_;

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
          room_.leave(shared_from_this());
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
          std::cerr << "jreq:\n" << jreq.dump() << "\n";
          std::string type {jreq["type"].get<std::string>()};
          std::cerr << "type: " << type << "\n\n";

          if (auth_)
          {
            // switch on type and perform action
            if (type == "msg")
            {
              room_.deliver(read_msg_);
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

              if (user == "user" && pass == "pass")
              {
                auth_ = true;

                Json jres;
                jres["type"] = "srv";
                jres["str"] = "Success: logged in";

                // send just to user
                deliver(jres.dump());
                room_.join(shared_from_this());
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
              jres["str"] = "Error: please authenticate with '/auth <password>'";

              deliver(jres.dump());
            }
          }

          do_read_header();
        }
        else
        {
          room_.leave(shared_from_this());
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
          room_.leave(shared_from_this());
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
