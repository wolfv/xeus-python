/***************************************************************************
* Copyright (c) 2019, Martin Renou, Johan Mabille, Sylvain Corlay and      *
* Wolf Vollprecht                                                          *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#include <iostream>
#include <string>
#include <thread>

#include "zmq_addon.hpp"

#include "xeus/xinterpreter.hpp"
#include "xeus/xmessage.hpp"

#include "xeus-python/xinterpreter.hpp"
#include "xcomm.hpp"
#include "xutils.hpp"

#include "pybind11/pybind11.h"
#include "pybind11/functional.h"

namespace py = pybind11;
using namespace pybind11::literals;


#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


namespace xpyt
{
    std::string get_end_point(const std::string& ip, const std::size_t& port)
    {
        return "tcp://" + ip + ':' + std::to_string(port);
    }

    /*************************
     * xdebugger declaration *
     *************************/

    class xdebugger
    {
    public:

        xdebugger(py::object comm);
        virtual ~xdebugger();

        void start_ptvsd();
        void start_client();

        void send_to_ptvsd(py::object message);

        std::pair<int, int> parse_dpa(std::vector<char>& buf);

    private:
        void start_client_impl();

        std::string m_host;
        std::size_t m_server_port;
        py::object m_comm;
        py::object m_secondary_thread;

        std::vector<char> m_buf;

        int m_socket;
        struct sockaddr_in m_addr;
    };

    /****************************
     * xdebugger implementation *
     ****************************/

    xdebugger::xdebugger(py::object comm)
        : m_host("127.0.0.1")
        , m_server_port(5678) // Hardcoded for now, we need a way to find an available port
        , m_comm(comm)
    {
        // m_client_socket.connect(get_end_point(m_host, m_server_port));
        // auto resp = m_client_socket.send(testmsg.begin(), testmsg.end(), ZMQ_DONTWAIT);
        
        m_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (m_socket < 0)
        {
            std::cout << "Cannot open socket" << std::endl;
            // exit(1);
        }
        else
        {
            std::cout << "Open'd socket! " << std::endl;
        }

        m_addr.sin_family = AF_INET;
        m_addr.sin_port = htons(m_server_port);
        m_addr.sin_addr.s_addr = inet_addr(m_host.c_str());

        // std::cout << "DEBUG ME PLEAAAAAASE" << std::endl;
        // std::string testmsg = "HELERELRERLERLERLELRELRLRE";
        // std::cout << "SEND CONFIRM? " << resp << std::endl;
    }

    xdebugger::~xdebugger()
    {
        // TODO: Send stop event to ptvsd
    }

    void xdebugger::start_ptvsd()
    {
        py::module ptvsd = py::module::import("ptvsd");
        ptvsd.attr("enable_attach")(py::make_tuple(m_host, m_server_port), "log_dir"_a="/tmp/");
        // ptvsd.attr("enable_attach")(py::make_tuple(m_host, m_server_port), "log_dir"_a="/tmp/");
    }

    void xdebugger::start_client()
    {
        py::module threading = py::module::import("threading");

        py::object thread = threading.attr("Thread")("target"_a=py::cpp_function([this] () {
            start_client_impl();
        }));
        thread.attr("start")();
    }

    bool matches(const char* lhs, const char* rhs, std::size_t n)
    {
        std::size_t i = 0;
        while (*(lhs + i) == *(rhs + i) && i < n)
        {
            std::cout << *(lhs + i) << ", " << *(rhs + i);
            ++i;
        }
        std::cout << i << std::endl;
        if (i == n)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    std::pair<int, int>
    xdebugger::parse_dpa(std::vector<char>& buf)
    {
        const std::string search_for = "Content-Length: ";
        const std::string fourctrlr = "\r\n\r\n";
        std::size_t i = 0;
        bool found_content_length = false;
        for (; i < buf.size(); ++i)
        {
            if (buf.size() > search_for.size())
            {
                if (matches(&buf[i], &search_for[0], search_for.size()))
                {
                    found_content_length = true;
                    break;
                }
            }
        }
        // std::cout << "Found content legnht? " << found_content_length << std::endl;

        int content_length = -1;
        if (found_content_length)
        {
            std::string slength;
            bool found_end = false;
            i += search_for.size();
            for (; i < buf.size(); ++i)
            {
                std::cout << buf[i] << std::endl;
                if (buf[i + 0] == '\r' && buf[i + 1] == '\n' && 
                    buf[i + 2] == '\r' && buf[i + 3] == '\n')
                {
                    found_end = true;
                    break;
                }
                else
                {
                    slength.push_back(buf[i]);
                }
            }
            // std::cout << "Found end? " << found_end << std::endl;
            if (found_end)
            {
                content_length = std::atoi(slength.c_str());
            }
        }

        // std::cout << "Content lenght == " << content_length << std::endl;

        if (content_length != -1)
        {
            i += 4;
            std::cout << buf.size() << " -> " << buf.size() - i << ", " << content_length << std::endl;
            if (buf.size() - i >= content_length)
            {
                return std::make_pair(i, i + content_length);
            }
        }
        return std::make_pair(-1, -1);
    }

    void xdebugger::start_client_impl()
    {
        if(connect(m_socket, (const sockaddr*) &m_addr, sizeof(m_addr)) < 0)
        {
            std::cout << "Cannot connect to socket" << std::endl;
            exit(1);
        }
        else
        {
            std::cout << "Connected to socket! " << std::endl;
        }

        while (true) // TODO Find a stop condition (ptvsd exit message?)
        {
            // Releasing the GIL before the blocking call
            py::gil_scoped_release release;

            // zmq::pollitem_t items[] = { { m_client_socket, 0, ZMQ_POLLIN, 0 } };

            // Blocking call
            // zmq::poll(&items[0], 1, -1);

            std::array<char, 1024> recv_buf;
            std::fill(recv_buf.begin(), recv_buf.end(), 0);

            // std::cout << "receiving " << std::endl;
            int size = recv(m_socket, (char*) recv_buf.data(), recv_buf.size(), 0);
            if (size < 0)
            {
                // std::cout << "Could not receive message" << std::endl;
                // continue;
                // do nothing
            }
            else
            {
                std::cout << green_text("client::got new message") << std::endl;

                for (int i = 0; i < size; ++i)
                {
                    m_buf.push_back(recv_buf[i]);
                }
                // Acquire the GIL before executing Python code
                py::gil_scoped_acquire acquire;

                while (true)
                {
                    auto res = parse_dpa(m_buf);
                    if (res.first != -1)
                    {
                        std::string json_str = std::string(&m_buf[res.first], &m_buf[res.second]);
                        std::cout << "Sending JSON " << json_str << std::endl;
                        py::dict msg;
                        msg["json"] = json_str;
                        m_comm.attr("send")("data"_a=msg);
                        m_buf.erase(m_buf.begin(), m_buf.begin() + res.second);
                    }
                    else
                    {
                        break;
                    }
                }
            }
        } 
        // while (true) // TODO Find a stop condition (ptvsd exit message?)
        // {
        //     // Releasing the GIL before the blocking call
        //     py::gil_scoped_release release;

        //     zmq::pollitem_t items[] = { { m_client_socket, 0, ZMQ_POLLIN, 0 } };

        //     std::cout << green_text("client::waiting for new messages") << std::endl;
        //     // Blocking call
        //     zmq::poll(&items[0], 1, -1);

        //     if (items[0].revents & ZMQ_POLLIN)
        //     {
    
        //         zmq::multipart_t multipart;
        //         multipart.recv(m_client_socket);

        //         std::string msg_string;
        //         while (!multipart.empty()) {

        //             zmq::message_t msg = multipart.pop();

        //             try
        //             {
        //                 const char* buf = msg.data<const char>();
        //                 for (std::size_t i = 0; i < msg.size(); ++i)
        //                 {
        //                     m_buf.push_back(*(buf + i));
        //                 }
        //                 // std::cout << "Got from PTVSD!: " << buf << std::endl;
        //                 msg_string += std::string(buf, msg.size());
        //             }
        //             catch (...)
        //             {
        //                 // Could not decode the message, not sending anything
        //                 continue;
        //             }
        //         }

        //         std::cout << green_text("client::sending through comms: ") << msg_string << std::endl;
        //         {
        //             // Acquire the GIL before executing Python code
        //             py::gil_scoped_acquire acquire;

        //             while (true)
        //             {
        //                 auto res = parse_dpa(m_buf);
        //                 if (res.first != -1)
        //                 {
        //                     std::string json_str = std::string(&m_buf[res.first], &m_buf[res.second]);
        //                     std::cout << "Sending JSON " << json_str << std::endl;
        //                     py::dict msg;
        //                     msg["json"] = json_str;
        //                     m_comm.attr("send")("data"_a=msg);
        //                     m_buf.erase(m_buf.begin(), m_buf.begin() + res.second);
        //                 }
        //                 else
        //                 {
        //                     break;
        //                 }
        //             }

        //         }
        //     }
        // }
    }

    void xdebugger::send_to_ptvsd(py::object message)
    {
        std::string cpp_msg = static_cast<std::string>(py::str(message));
        std::stringstream add_hdr;
        add_hdr << "Content-Length: " << cpp_msg.size() << "\r\n\r\n"
                << cpp_msg;
        std::string xstr = add_hdr.str();
        std::cout << green_text("client::send to ptvsd: ") << xstr << std::endl;
        // zmq::message_t msg(xstr.data(), xstr.size());
        int size = write(m_socket, xstr.c_str(), xstr.size());
        if (size < 0)
        {
            std::cout << "Could not send message" << std::endl;
        }

        // auto resp = m_client_socket.send(msg, ZMQ_DONTWAIT);
        // std::cout << "SEND CONFIRM? " << resp << std::endl;
    }

    interpreter& get_interpreter()
    {
        return dynamic_cast<interpreter&>(xeus::get_interpreter());
    }

    void debugger_callback(py::object comm, py::object msg)
    {
        py::object debugger = get_interpreter().start_debugging(comm);

        // Start client in a secondary thread

        std::cout << red_text("-- start_ptvsd") << std::endl;
        debugger.attr("start_ptvsd")();

        std::cout << red_text("-- start_client") << std::endl;
        debugger.attr("start_client")();

        std::cout << red_text("debugger::successfully initialized") << std::endl;

        // On message, forward it to ptvsd and send back the response to the client?
        comm.attr("on_msg")(py::cpp_function([debugger] (py::object msg) {
            std::cout << blue_text("comm::received: ") << py::str(msg).cast<std::string>() << std::endl;
            debugger.attr("send_to_ptvsd")(msg["content"]["data"]);
        }));

        // On Comm close, stop the communication?
        // comm.on_close();
    }

    /*******************
     * debugger module *
     *******************/

    py::module get_debugger_module_impl()
    {
        py::module debugger_module = py::module("debugger");

        py::class_<xdebugger>(debugger_module, "Debugger")
            .def(py::init<py::object>())
            .def("start_client", &xdebugger::start_client)
            .def("start_ptvsd", &xdebugger::start_ptvsd)
            .def("send_to_ptvsd", &xdebugger::send_to_ptvsd);

        debugger_module.def("debugger_callback", &debugger_callback);

        return debugger_module;
    }

    py::module get_debugger_module()
    {
        static py::module debugger_module = get_debugger_module_impl();
        return debugger_module;
    }

    void register_debugger_comm()
    {
        get_kernel_module().attr("register_target")(
            "jupyter.debugger", get_debugger_module().attr("debugger_callback")
        );
    }
}
