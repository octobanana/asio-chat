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

typedef std::deque<chat_message> chat_message_queue;

class chat_client
{
public:

  chat_client(boost::asio::io_context& io_context,
    const tcp::resolver::results_type& endpoints) :
    io_context_ {io_context},
    socket_ {io_context}
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
        socket_.close();
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
          socket_.close();
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
            std::string usr {jres["usr"].get<std::string>()};
            std::string msg {jres["msg"].get<std::string>()};
            std::cout << usr << "> " << msg << "\n";
          }
          // else if (type == "")
          // {
          //   // do something else
          // }

          do_read_header();
        }
        else
        {
          socket_.close();
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
          socket_.close();
        }
      }
    );
  }

private:
  boost::asio::io_context& io_context_;
  tcp::socket socket_;
  chat_message read_msg_;
  chat_message_queue write_msgs_;
};

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 4)
    {
      std::cerr << "Usage: chat_client <host> <port> <name>\n";
      return 1;
    }

    // the users name
    std::string name {argv[3]};

    boost::asio::io_context io_context;

    tcp::resolver resolver(io_context);
    auto endpoints = resolver.resolve(argv[1], argv[2]);
    chat_client c(io_context, endpoints);

    std::thread t([&io_context](){ io_context.run(); });

    std::cout << "Welcome!\n";

    // main loop
    std::string input;
    for (;;)
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
        if (input == "/quit")
        {
          std::cerr << "Exiting...\n";
          // exit the program
          break;
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
        jreq["usr"] = name;
        jreq["msg"] = input;
        std::string req {jreq.dump()};

        // check length of req string
        if (req.size() > chat_message::max_body_length)
        {
          std::cerr << "Error: message length too long\n";
          continue;
        }

        // send the message
        chat_message msg;
        msg.body_length(req.size());
        std::memcpy(msg.body(), req.data(), msg.body_length());
        msg.encode_header();
        c.write(msg);
      }
    }

    c.close();
    t.join();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
