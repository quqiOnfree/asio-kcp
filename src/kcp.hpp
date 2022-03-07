#include <string>
#include <memory>
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <cstdarg>
#include <queue>
#include <ctime>
#include <asio.hpp>
#include "kcp/ikcp.h"

namespace moon
{
    namespace kcp
    {
        constexpr uint8_t close_key[32] = { 87, 51, 90, 138, 98, 219, 143, 116, 18, 21, 204, 102, 221, 3, 139, 89, 217, 225, 60, 125, 27, 82, 146, 49, 196, 39, 80, 40, 80, 236, 104, 67 };

        constexpr time_t timeout_duration = 30 * 1000;//millseconds

        static std::tm* localtime(std::time_t* t, std::tm* result)
        {
#ifdef _MSC_VER
            localtime_s(result, t);
#else
            localtime_r(t, result);
#endif
            return result;
        }

        inline void console_log(const char* format, ...)
        {
            auto now = time(nullptr);
            std::tm m;
            localtime(&now, &m);
            char tmbuffer[80];
            strftime(tmbuffer, 80, "%Y-%m-%d, %H:%M:%S", &m);

            va_list args;
            va_start(args, format);
            char buffer[4 * 1024];
            vsprintf(buffer, format, args);
            printf("[%s] %s\n", tmbuffer, buffer);
            va_end(args);
            fflush(stdout);
        }

        constexpr uint8_t packet_handshark = 1;
        constexpr uint8_t packet_keepalive = 2;
        constexpr uint8_t packet_data = 3;
        constexpr uint8_t packet_disconnect = 4;
        constexpr uint8_t packet_type_max = 5;

        using asio::ip::udp;

        inline std::pair<std::string, unsigned short> parse_host_port(const std::string& host_port, unsigned short default_port) {
            std::string host, port;
            host.reserve(host_port.size());
            bool parse_port = false;
            int square_count = 0; // To parse IPv6 addresses
            for (auto chr : host_port) {
                if (chr == '[')
                    ++square_count;
                else if (chr == ']')
                    --square_count;
                else if (square_count == 0 && chr == ':')
                    parse_port = true;
                else if (!parse_port)
                    host += chr;
                else
                    port += chr;
            }

            if (port.empty())
                return { std::move(host), default_port };
            else {
                try {
                    return { std::move(host), static_cast<unsigned short>(std::stoul(port)) };
                }
                catch (...) {
                    return { std::move(host), default_port };
                }
            }
        }

        inline time_t clock()
        {
            using time_point = std::chrono::time_point<std::chrono::steady_clock>;
            static const time_point start_time_point = std::chrono::steady_clock::now();
            auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_point);
            return diff.count();
        }

        class operation
        {
        public:
            bool complete(void* owner, const std::error_code& ec,std::size_t bytes_transferred)
            {
                return func_(owner, this, ec, bytes_transferred);
            }
        protected:
            using func_type = bool(*)(void*, operation*, const std::error_code&, std::size_t);
            operation(func_type func):func_(func){}
            ~operation(){}
        private:
            func_type func_;
        };

        class static_buffer
        {
            size_t size_ = 0;
            void* user_ = nullptr;
            std::array<char, 2048> data_{};
        public:
            static_buffer(void* user)
                :user_(user)
            {
            }

            char* data()
            {
                return data_.data();
            }

            const char* data() const
            {
                return data_.data();
            }

            size_t max_size() const
            {
                return data_.max_size();
            }

            void set_size(size_t n)
            {
                size_ = n;
            }

            size_t size() const
            {
                return size_;
            }

            asio::mutable_buffer mutable_buffer()
            {
                return asio::buffer(data_.data(), size_);
            }

            asio::const_buffer const_buffer() const
            {
                return asio::buffer(data_.data(), size_);
            }

            std::string to_string() const
            {
                return std::string{ data_.data(), size_ };
            }

            void* get_user() const
            {
                return user_;
            }
        };

        //--------------------------------connection--------------------------------------

        class connection: public std::enable_shared_from_this<connection>
        {
            friend class acceptor;

            struct kcp_obj_deleter{void operator()(ikcpcb* p){ikcp_release(p);}};

            using kcp_context_ptr = std::unique_ptr<ikcpcb, kcp_obj_deleter>;

            struct static_buffer_deleter {
                void operator()(static_buffer* p)
                {
                    connection* user = static_cast<connection*>(p->get_user());
                    if (user->pool_.size() < 32)
                    {
                        user->pool_.push_back(p);
                    }
                    else
                    {
                        delete p;
                    }
                }
            };

            using static_buffer_ptr = std::unique_ptr<static_buffer, static_buffer_deleter>;

        private:
            enum class state
            {
                idle = 0,
                opened = 1,
                closed = 2,
            };

            bool isserver_ = true;
            state state_ = state::idle;
            uint32_t conv_ = 0;
            time_t next_tick_ = 0;
            time_t now_tick_ = 0;
            udp::socket* sock_ = nullptr;
            udp::endpoint endpoint_;
            std::unique_ptr<ikcpcb, kcp_obj_deleter> obj_;
            std::unique_ptr<asio::steady_timer> timer_;
            std::shared_ptr<operation> read_op_;
            asio::streambuf read_buffer_;
            std::vector<static_buffer*> pool_;

            template <typename Handler, typename MutableBuffer>
            class read_op :public operation
            {
            public:
                read_op(Handler&& handler, const MutableBuffer& buffer, bool some)
                    :operation(&read_op::do_complete)
                    , handler_(std::move(handler))
                    , buffer_(buffer)
                    , some_(some)
                {
                }

                static bool do_complete(void* user, operation* base, const asio::error_code& ec, std::size_t)
                {
                    read_op* op(static_cast<read_op*>(base));
                    if (ec)
                    {
                        op->handler_(ec, 0);
                        return true;
                    }
                    else
                    {
                        connection* c = static_cast<connection*>(user);
                        int n = ikcp_peeksize(c->obj_.get());
                        if (n > 0)
                        {
                            auto buffer = c->read_buffer_.prepare(n);
                            ikcp_recv(c->obj_.get(), reinterpret_cast<char*>(buffer.data()), n);
                            c->read_buffer_.commit(n);
                        }
                        size_t need = (op->some_?std::min(op->buffer_.size(), c->read_buffer_.size()) : op->buffer_.size());
                        if (c->read_buffer_.size() >= need)
                        {
                            asio::buffer_copy(op->buffer_, c->read_buffer_.data(), need);
                            c->read_buffer_.consume(need);
                            op->handler_(ec, need);
                            return true;
                        }
                        return false;
                    }
                }

                size_t buffer_size() const
                {
                    return buffer_.size();
                }
            private:
                Handler handler_;
                const MutableBuffer& buffer_;
                bool some_ = false;
            };

            class initiate_async_read
            {
            public:
                typedef  asio::any_io_executor executor_type;

                explicit initiate_async_read(connection* self)
                    : self_(self)
                {
                }

                executor_type get_executor() const
                {
                    return self_->get_executor();
                }

                template<typename MutableBuffer, typename ReadHandler>
                void operator()(ReadHandler&& handler, const MutableBuffer& buffer, bool some) const
                {
                    if (nullptr != self_->read_op_ || self_->closed())
                    {
                        auto executor = asio::get_associated_executor(
                            handler, self_->get_executor());

                        asio::post(
                            asio::bind_executor(executor,
                                std::bind(std::forward<decltype(handler)>(
                                    handler), self_->closed()? asio::error::operation_aborted : asio::error::in_progress, 0)));
                    }
                    else
                    {
                        using op_t = read_op<std::decay_t<ReadHandler>, std::decay_t<MutableBuffer>>;
                        size_t need = buffer.size();
                        self_->read_op_ = std::make_unique<op_t>(std::forward<ReadHandler>(handler), buffer, some);
                        if (self_->read_buffer_.size() >= need)
                        {
                            asio::post([self_ = self_]() {
                                self_->check_read_op();
                                });
                        }
                    }
                }

            private:
                connection* self_;
            };

            void init_kcp_context()
            {
                obj_ = std::unique_ptr<ikcpcb, kcp_obj_deleter>{ ikcp_create(conv_, this) };
                ikcp_wndsize(obj_.get(), 256, 256);
                ikcp_nodelay(obj_.get(), 1, 10, 2, 1);
                ikcp_setmtu(obj_.get(), 1200);
                obj_->rx_minrto = 10;
                obj_->stream = 1;
                ikcp_setoutput(obj_.get(), [](const char* buf, int len, ikcpcb*, void* user) {
                    connection* conn = (connection*)user;
                    conn->raw_send(buf, len);
                    return 0;
                    });
            }

            static_buffer_ptr create_buffer()
            {
                static_buffer* buffer;
                if (!pool_.empty())
                {
                    buffer = pool_.back();
                    pool_.pop_back();
                    buffer->set_size(0);
                }
                else
                {
                    buffer = new static_buffer{ this };
                }
                return static_buffer_ptr{ buffer };
            }

            void check_read_op(asio::error_code ec = asio::error_code{})
            {
                if (nullptr != read_op_)
                {
                    auto op = std::move(read_op_);
                    if (!op->complete(this, ec, 0))
                    {
                        read_op_ = std::move(op);
                    }
                }
            }

            void on_receive(const char* data, size_t size, time_t t)
            {
                if(state_ == state::idle)
                    state_ = state::opened;

                now_tick_ = t;

                uint8_t opcode = static_cast<uint8_t>(data[0]) & 0xF;
                switch (opcode)
                {
                case packet_disconnect:
                {
                    if(nullptr != obj_)
                        close(asio::error::make_error_code(asio::error::eof));
                    return;
                }
                case packet_keepalive:
                {
                    return;
                }
                case packet_data:
                {
                    if (nullptr == obj_)
                    {
                        init_kcp_context();
                    }
                    ikcp_input(obj_.get(), data +1, (long)(size - 1));
                    next_tick_ = 0;
                    check_read_op();
                    return;
                }
                default:
                    break;
                }
            }

            void start_timer()
            {
                if (nullptr == timer_)
                    timer_ = std::make_unique<asio::steady_timer>(get_executor());
                timer_->expires_after(std::chrono::milliseconds(5));
                timer_->async_wait([this, self = shared_from_this()](const asio::error_code& e) {
                    if (e)
                    {
                        return;
                    }
                    time_t now = clock();
                    time_t t = update(now);
                    if ((now - t) > timeout_duration)
                    {
                        close(asio::error::timed_out);
                        return;
                    }
                    start_timer();
                });
            }

            time_t update(time_t now)
            {
                if (state_ == state::opened)
                {
                    check_read_op();
                }
                if (obj_ != nullptr && next_tick_ <= now)
                {
                    ikcp_update(obj_.get(), (IUINT32)now);
                    next_tick_ = ikcp_check(obj_.get(), (IUINT32)now);
                }
                return now_tick_;
            }

            void do_receive(static_buffer_ptr buffer)
            {
                assert(!isserver_);
                if (state_ == state::closed)
                    return;
                buffer->set_size(buffer->max_size());
                auto mutable_buffer = buffer->mutable_buffer();
                sock_->async_receive_from(
                    mutable_buffer,
                    endpoint_,
                    [this, self = shared_from_this(), buffer = std::move(buffer)](const std::error_code& ec, size_t size) mutable
                {
                    if (ec)
                    {
                        close(ec);
                    }
                    else
                    {
                        if (size > 24)
                        {
                            on_receive(buffer->data(), size, clock());
                        }
                        do_receive(std::move(buffer));
                    }
                });
            }
        public:
            connection(udp::socket* sock, uint32_t conv, udp::endpoint& endpoint, bool isserver = true)
                : isserver_(isserver)
                , conv_(conv)
                , now_tick_(clock())
                , sock_(sock)
                , endpoint_(std::move(endpoint))
            {
                assert(sock_);
                assert(conv_ > 0);
                if (!isserver)
                {
                    init_kcp_context();
                }
            }

            virtual ~connection()
            {
                close(asio::error::operation_aborted);
                console_log("%s.connection destructer: %u", (isserver_ ? "server": "client" ), conv_);
                if (!isserver_ && nullptr!= sock_)
                {
                    delete sock_;
                    sock_ = nullptr;
                }

                for (auto p : pool_)
                {
                    delete p;
                }
                pool_.clear();
            }

            void start_client()
            {
                if (state_ == state::idle)
                {
                    state_ = state::opened;
                    do_receive(create_buffer());
                    start_timer();
                }
            }

            asio::any_io_executor get_executor()
            {
                return sock_->get_executor();
            }

            template<typename MutableBuffer,typename Handler>
            auto async_read(const MutableBuffer& buffer, Handler&& handler)
            {
                return asio::async_initiate<Handler, void(const std::error_code&, size_t)>(
                    initiate_async_read(this), handler, buffer, false);
            }

            template<typename MutableBuffer, typename Handler>
            auto async_read_some(const MutableBuffer& buffer, Handler&& handler)
            {
                return asio::async_initiate<Handler, void(const std::error_code&, size_t)>(
                    initiate_async_read(this), handler, buffer, true);
            }

            bool async_write(const char* data, size_t len)
            {
                if (nullptr == obj_)
                    return false;
                next_tick_ = 0;
                return ikcp_send(obj_.get(), data, static_cast<int>(len)) >= 0;
            }

            bool raw_send(const char* data, size_t size, uint8_t packet_type = packet_data)
            {
                assert(size <= 2048);
                auto buffer = create_buffer();
                char* p = buffer->data();
                p[0] = (char)packet_type;
                memcpy(p + 1, data, size);
                buffer->set_size(size + 1);
                auto const_buffer = buffer->const_buffer();
                if (packet_type != packet_disconnect)
                {
                    sock_->async_send_to(
                        const_buffer,
                        endpoint_,
                        [self = shared_from_this(), buffer = std::move(buffer)]
                    (const std::error_code&, size_t) {
                        //console_log("udp send size: %zu", size);
                    });
                }
                else
                {
                    sock_->send_to(const_buffer, endpoint_);
                }
                return true;
            }

            uint32_t get_conv() const
            {
                return conv_;
            }

            bool closed() const
            {
                return state_ == state::closed;
            }

            bool idle() const
            {
                return state_ == state::idle;
            }

            bool is_server() const
            {
                return isserver_;
            }

            udp::socket& get_socket()
            {
                return *sock_;
            }

            void close(asio::error_code ec)
            {
                if (state_ != state::opened)
                    return;

                if (ec == asio::error::timed_out)
                {
                    console_log("connection(%s):  %u timeout", (is_server() ? "server" : "client"), conv_);
                }

                state_ = state::closed;
                ikcp_flush(obj_.get());
                check_read_op(ec);
                std::string key{(char*)close_key, 32};
                raw_send(key.data(), key.size(), packet_disconnect);
                if (!isserver_)
                {
                    asio::error_code ignore;
                    sock_->close(ignore);
                    if (timer_)
                        timer_->cancel();
                }
            }
        };

        using connection_ptr = std::shared_ptr<connection>;

        //------------------------------acceptor------------------------------------

        class acceptor
        {
        public:
            using endpoint_type = udp::endpoint;
            using executor_type = asio::any_io_executor ;
        private:
            class initiate_async_accept
            {
            public:
                explicit initiate_async_accept(acceptor* self)
                    : self_(self)
                {
                }

                executor_type get_executor() const
                {
                    return self_->get_executor();
                }

                template <typename AcceptHandler>
                void operator()(AcceptHandler&& handler) const
                {
                    using op = accept_op<std::decay_t<AcceptHandler>>;
                    self_->accept_ops_.push(std::make_shared<op>(std::forward<AcceptHandler>(handler)));
                }

            private:
                acceptor* self_;
            };

            class accept_op_ :public operation
            {
            public:
                connection_ptr c;
            protected:
                accept_op_(func_type complete_func)
                    : operation(complete_func)
                {
                }
            };

            template <typename Handler>
            class accept_op :public accept_op_
            {
            public:
                accept_op(Handler&& handler)
                    :accept_op_(&accept_op::do_complete)
                    , handler_(std::move(handler))
                {
                }

                static bool do_complete(void*, operation* base, const asio::error_code&, std::size_t /*bytes_transferred*/)
                {
                    accept_op* o(static_cast<accept_op*>(base));
                    o->handler_(o->c);
                    return true;
                }
            private:
                Handler handler_;
            };
        public:
            acceptor(const executor_type& executor, udp::endpoint endpoint, std::string magic)
                :magic_(magic)
                , sock_(executor, endpoint)
                , timer_(executor)
            {
                do_receive(std::make_unique<static_buffer>(this));
                update();
            }

            acceptor(const acceptor&) = delete;

            acceptor& operator=(const acceptor&) = delete;

            acceptor(acceptor&&) = delete;

            acceptor& operator=(acceptor&&) = delete;

            executor_type get_executor()
            {
                return timer_.get_executor();
            }

            template <typename AcceptHandler>
            auto async_accept(AcceptHandler&& handler)
            {
                return asio::async_initiate<AcceptHandler, void(const connection_ptr& c)>(
                    initiate_async_accept(this), handler);
            }
        private:
            void update()
            {
                timer_.expires_after(std::chrono::milliseconds(5));
                timer_.async_wait([this](const asio::error_code& e) {
                    if (e)
                    {
                        return;
                    }

                    now_ = clock();
                    for (auto iter = connections_.begin(); iter != connections_.end();)
                    {
                        auto lastrecvtime = iter->second->update(now_);
                        if (iter->second->closed() || (now_ - lastrecvtime) > timeout_duration)
                        {
                            used_conv_.erase(iter->second->get_conv());
                            iter->second->close(iter->second->closed()?asio::error::operation_aborted:asio::error::timed_out);
                            iter = connections_.erase(iter);
                        }
                        else
                        {
                            ++iter;
                        }
                    }
                    update();
                    });
            }

            uint32_t make_conv()
            {
                while (!used_conv_.emplace(++conv_).second) {};
                return conv_;
            }

            void do_receive(std::unique_ptr<static_buffer> buffer)
            {
                buffer->set_size(buffer->max_size());
                auto mutable_buffer = asio::buffer(buffer->data(), buffer->size());
                sock_.async_receive_from(
                    mutable_buffer,
                    from_,
                    [this, buffer = std::move(buffer)](const std::error_code& ec, size_t size) mutable
                {
                    if (ec)
                    {
                        //console_log("kcp.acceptor do_receive error: %s", ec.message().data());
                        do_receive(std::move(buffer));
                        return;
                    }

                    do
                    {
                        if (size < 24)
                            break;

                        uint8_t packet_type = buffer->data()[0] & 0xF;
                        if (packet_type >= packet_type_max)
                            break;

                        if (packet_type == packet_handshark)
                        {
                            if (accept_ops_.empty())
                                break;

                            if (size - 1 != magic_.size() || magic_ != std::string{ buffer->data()+1, size -1 })
                            {
                                console_log("acceptor ignore packet: handshark magic not match.");
                                break;
                            }

                            connection_ptr conn;
                            if (auto iter = connections_.find(from_); iter != connections_.end())
                            {
                                if (iter->second->idle())
                                {
                                    conn = iter->second;
                                }
                                else
                                {
                                    iter->second->close(asio::error::make_error_code(asio::error::operation_aborted));
                                    connections_.erase(iter);
                                }
                            }

                            if(!conn)
                            {
                                conn = std::make_shared<connection>(&sock_, make_conv(), from_, true);
                                connections_.emplace(from_, conn);
                            }

                            std::unique_ptr<std::string> response = std::make_unique<std::string>();
                            uint32_t conv = conn->get_conv();
                            response->append(reinterpret_cast<const char*>(&conv), sizeof(conv));
                            auto b = asio::buffer(response->data(), response->size());
                            sock_.async_send_to(b, from_, [response = std::move(response)](std::error_code, size_t) {});

                            auto op = accept_ops_.front();
                            accept_ops_.pop();
                            static_cast<accept_op_*>(op.get())->c = conn;
                            op->complete(this, std::error_code(), 0);
                        }
                        else
                        {
                            if (auto iter = connections_.find(from_); iter != connections_.end())
                            {
                                iter->second->on_receive(buffer->data(), size, now_);
                            }
                        }
                    }while (false);

                    do_receive(std::move(buffer));
                });
            }
        private:
            uint32_t conv_ = 0;
            time_t now_ = clock();
            std::string magic_;
            udp::socket sock_;
            udp::endpoint from_;
            asio::steady_timer timer_;
            std::queue<std::shared_ptr<operation>> accept_ops_;
            std::unordered_map<udp::endpoint, connection_ptr> connections_;
            std::unordered_set<uint32_t> used_conv_;
        };

        //------------------------------connector------------------------------------

        template<typename Executor>
        inline asio::awaitable<connection_ptr> async_connect(const Executor& executor,  udp::endpoint endpoint, std::string magic, time_t millseconds_timeout)
        {
            udp::socket sock(executor);

            asio::steady_timer timer{executor};
            timer.expires_after(std::chrono::milliseconds(millseconds_timeout));
            timer.async_wait([&sock](const asio::error_code& e) {
                if (e)
                {
                    return;
                }
                std::error_code ignore;
                sock.close(ignore);
                console_log("async_connect timeout");
                });

            co_await sock.async_connect(endpoint, asio::use_awaitable);
            std::string data;
            data.push_back((char)packet_handshark);
            data.append(magic);
            co_await sock.async_send(asio::buffer(data), asio::use_awaitable);
            data.clear();
            data.resize(16, 0);
            co_await sock.async_receive(asio::buffer(data), asio::use_awaitable);
            uint32_t conv;
            memcpy(&conv, data.data(), sizeof(conv));
            auto conn = std::make_shared<connection>(new udp::socket(std::move(sock)), conv, endpoint, false);
            conn->start_client();
            timer.cancel();
            co_return conn;
        }
    }
}