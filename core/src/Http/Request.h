#pragma once

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>

#include <types.h>
#include <utils/ClearFile.h>
#include <utils/GetFolder.h>
#include <utils/strFmt.h>

namespace http_get { inline namespace Http {

class Request {
public:
    Request(const std::string &data) : m_data(data) {}
    template<typename Iter>
    Request(Iter l, Iter r) : m_data(l, r) {}
    ~Request() {
        if (m_file) {
            m_buffer.close();
            // m_buffer.open(m_buffer_filename,
            //               std::ofstream::out | std::ofstream::trunc);
            // m_buffer.close();
        }
    }

    std::string query() const {
        return m_data.substr(
                0,
                std::min(m_data.size(),
                         m_data.find(' ', m_data.find(' ') + 1)));
    }
    template<typename Iter>
    Request &append(Iter l, Iter r) {
        if (!m_file) {
            m_buffer.open(m_buffer_filename, std::ios::out);
            m_file = true;
        }
        std::copy(l, r, std::ostream_iterator<i8>{m_buffer});
        return *this;
    }
    Request *fin() {
        if (m_file) {
            m_buffer.close();
        }
        return this;
    }
    std::string body() const {
        return m_data.substr(
                std::min(m_data.size() - 4, m_data.find("\r\n\r\n")) +
                4);
    }
    std::size_t payloadSize() const {
        return std::stoull(
                m_data.substr(m_data.find("Content-Length: ") + 16));
    }
    bool copyTo(const std::string &filename) const {
        if (!m_file) {
            std::ofstream copy_to(filename,
                                  std::ios::binary | std::ios::trunc |
                                          std::ios::out);
            copy_to << this->body();
            return true;
        }
        std::ignore = std::system(
                utils::strFmt("mkdir -p %", utils::GetFolder(filename))
                        .c_str());

        std::ignore = std::system(
                utils::strFmt("mv % %", m_buffer_filename, filename)
                        .c_str());

        return true;
    }
    bool needContinue() const {
        return m_data.find("Expect: 100-continue") != std::string::npos;
    }

private:
    const std::string m_buffer_filename = "tmp";
    const std::string m_data;
    std::fstream m_buffer;
    bool m_file = false;
};

}} // namespace http_get::Http