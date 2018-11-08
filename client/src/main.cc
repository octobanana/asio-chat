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
#include <thread>
#include <string>
#include <atomic>

using chat_message_queue = std::deque<chat_message>;

class chat_client
{
public:

  explicit chat_client(boost::asio::io_context& io_context, std::atomic_bool& connected,
    const tcp::resolver::results_type& endpoints) :
    io_context_ {io_context},
    socket_ {io_context},
    connected_ {connected}
  {
    do_connect(endpoints);
  }

  void write(const chat_message& msg)
  {
    boost::asio::post(io_context_,
      [this, msg]()
      {
        bool write_in_progress = !write_msgs_.empty();
        write_msgs_.push_back(msg);
        if (!write_in_progress)
        {
          do_write();
        }
      }
    );
  }

  void close()
  {
    boost::asio::post(io_context_,
      [this]()
      {
        do_close();
      }
    );
  }

private:

  void do_connect(const tcp::resolver::results_type& endpoints)
  {
    boost::asio::async_connect(socket_, endpoints,
      [this](boost::system::error_code ec, tcp::endpoint)
      {
        if (!ec)
        {
          do_read_header();
        }
        else
        {
          connected_.store(false);
          return;
        }
      }
    );
  }

  void do_read_header()
  {
    boost::asio::async_read(socket_,
      boost::asio::buffer(read_msg_.data(), chat_message::header_length),
      [this](boost::system::error_code ec, std::size_t /*length*/)
      {
        if (!ec && read_msg_.decode_header())
        {
          do_read_body();
        }
        else
        {
          do_close();
        }
      }
    );
  }

  void do_read_body()
  {
    boost::asio::async_read(socket_,
      boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
      [this](boost::system::error_code ec, std::size_t /*length*/)
      {
        if (!ec)
        {
          // parse json from body
          std::string res {read_msg_.body(), read_msg_.body_length()};
          Json jres = Json::parse(res);
          std::string type {jres["type"].get<std::string>()};

          // switch on type and perform action
          if (type == "msg")
          {
            // regular message
            std::string user {jres["user"].get<std::string>()};
            std::string msg {jres["msg"].get<std::string>()};
            std::cout << user << "> " << msg << "\n";
          }
          else if (type == "prv")
          {
            // private message
            std::string from {jres["from"].get<std::string>()};
            std::string msg {jres["msg"].get<std::string>()};
            std::cout << "[prv]" << from << "> " << msg << "\n";
          }
          else if (type == "srv")
          {
            // server message
            std::string str {jres["str"].get<std::string>()};
            std::cout << "server> " << str << "\n";
          }
          // else if (type == "")
          // {
          //   // do something else
          // }

          do_read_header();
        }
        else
        {
          do_close();
        }
      }
    );
  }

  void do_write()
  {
    boost::asio::async_write(socket_,
      boost::asio::buffer(write_msgs_.front().data(),
      write_msgs_.front().length()),
      [this](boost::system::error_code ec, std::size_t /*length*/)
      {
        if (!ec)
        {
          write_msgs_.pop_front();
          if (!write_msgs_.empty())
          {
            do_write();
          }
        }
        else
        {
          do_close();
        }
      }
    );
  }

  void do_close()
  {
    connected_.store(false);
    socket_.close();
  }

private:
  boost::asio::io_context& io_context_;
  tcp::socket socket_;
  std::atomic_bool& connected_;
  chat_message read_msg_;
  chat_message_queue write_msgs_;
};

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 2)
    {
      std::cerr << "Usage: chat_client <port>\n";
      return 1;
    }

    boost::asio::io_context io_context;

    tcp::resolver resolver(io_context);
    auto endpoints = resolver.resolve("127.0.0.1", argv[1]);
    std::atomic_bool connected {true};
    chat_client client {io_context, connected, endpoints};

    std::thread thread {[&io_context](){ io_context.run(); }};

    std::cout << "Welcome!\n";

    // main loop
    std::string input;
    std::string name;
    while (connected.load())
    {
      if (! std::getline(std::cin, input))
      {
        break;
      }

      // determine action
      if (input.empty())
      {
        // no input
        continue;
      }
      else if (input.at(0) == '/')
      {
        // treat input that begins with '/' as special command

        if (input == "/help")
        {
          std::cerr
          << "/help\n"
          << "  -> display the help output\n"
          << "/auth <user> <pass>\n"
          << "  -> login to the server\n"
          << "/quit\n"
          << "  -> close the connection and exit the program\n"
          << "/priv <user> <regular text here>\n"
          << "  -> send text as message to single user\n"
          << "<regular text here>\n"
          << "  -> send text as message to chat room\n"
          << "\n";
          continue;
        }
        else if (input == "/quit")
        {
          std::cerr << "Exiting...\n";
          // exit the program
          break;
        }
        else if (input.find("/auth") == 0)
        {
          // format : '/auth <name> <password>'

          auto pos_user = input.find_first_of(" ") + 1;
          if (pos_user == std::string::npos)
          {
            std::cerr << "Error: incorrect <name> <password> format\n";
            continue;
          }

          auto pos_pass = input.substr(pos_user).find_first_of(" ") + pos_user + 1;
          if (pos_pass == std::string::npos)
          {
            std::cerr << "Error: incorrect <name> <password> format\n";
            continue;
          }

          // set the users name
          name = input.substr(pos_user, pos_pass - pos_user - 1);

          // send user and pass
          // build up json body message
          Json jreq;
          jreq["type"] = "auth";
          jreq["user"] = name;
          jreq["pass"] = input.substr(pos_pass);
          std::string req {jreq.dump()};

          // check length of req string
          if (req.size() > chat_message::max_body_length)
          {
            std::cerr << "Error: message length too long\n";
            continue;
          }

          // send the message
          chat_message msg {req};
          client.write(msg);
          continue;
        }
        else if (input.find("/priv") == 0)
        {
          // /priv <user> <regular text here>
          // send private message to another user

          auto pos_user = input.find_first_of(" ") + 1;
          if (pos_user == std::string::npos)
          {
            std::cerr << "Error: incorrect <name> <password> format\n";
            continue;
          }

          auto pos_msg = input.substr(pos_user).find_first_of(" ") + pos_user + 1;
          if (pos_msg == std::string::npos)
          {
            std::cerr << "Error: incorrect <name> <password> format\n";
            continue;
          }

          // build up json body message
          Json jreq;
          jreq["type"] = "prv";
          jreq["user"] = name;
          jreq["to"] = input.substr(pos_user, pos_msg - pos_user - 1);
          jreq["msg"] = input.substr(pos_msg);
          std::string req {jreq.dump()};

          // check length of req string
          if (req.size() > chat_message::max_body_length)
          {
            std::cerr << "Error: message length too long\n";
            continue;
          }

          // send the message
          chat_message msg {req};
          client.write(msg);
        }
        // else if (input == "")
        // {
        //   // do something
        // }
        else
        {
          // unknown command
          std::cerr << "Error: unknown command '" << input << "'\n";
          continue;
        }
      }
      else
      {
        // send message

        // build up json body message
        Json jreq;
        jreq["type"] = "msg";
        jreq["user"] = name;
        jreq["msg"] = input;
        std::string req {jreq.dump()};

        // check length of req string
        if (req.size() > chat_message::max_body_length)
        {
          std::cerr << "Error: message length too long\n";
          continue;
        }

        // send the message
        chat_message msg {req};
        client.write(msg);
      }
    }

    client.close();
    thread.join();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
