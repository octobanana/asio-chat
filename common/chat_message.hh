// Copyright (c) 2018 Brett Robinson
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// This is a derivative work, original copyright below:
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)

#ifndef CHAT_MESSAGE_HPP
#define CHAT_MESSAGE_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

class chat_message
{
public:

  enum { header_length = 4 };
  enum { max_body_length = 2048 };

  chat_message()
  {
  }

  const char* data() const
  {
    return data_;
  }

  char* data()
  {
    return data_;
  }

  std::size_t length() const
  {
    return header_length + body_length_;
  }

  const char* body() const
  {
    return data_ + header_length;
  }

  char* body()
  {
    return data_ + header_length;
  }

  std::size_t body_length() const
  {
    return body_length_;
  }

  void body_length(std::size_t new_length)
  {
    body_length_ = new_length;
    if (body_length_ > max_body_length)
    {
      body_length_ = max_body_length;
    }
  }

  bool decode_header()
  {
    char header[header_length + 1] = "";
    std::strncat(header, data_, header_length);
    body_length_ = std::stoul(header);

    if (body_length_ > max_body_length)
    {
      body_length_ = 0;
      return false;
    }

    return true;
  }

  void encode_header()
  {
    char header[header_length + 1] = "";
    std::sprintf(header, "%4d", static_cast<int>(body_length_));
    std::memcpy(data_, header, header_length);
  }

private:

  char data_[header_length + max_body_length];
  std::size_t body_length_ {0};

};

#endif // CHAT_MESSAGE_HPP
