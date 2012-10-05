/******************************************************************************\
 *           ___        __                                                    *
 *          /\_ \    __/\ \                                                   *
 *          \//\ \  /\_\ \ \____    ___   _____   _____      __               *
 *            \ \ \ \/\ \ \ '__`\  /'___\/\ '__`\/\ '__`\  /'__`\             *
 *             \_\ \_\ \ \ \ \L\ \/\ \__/\ \ \L\ \ \ \L\ \/\ \L\.\_           *
 *             /\____\\ \_\ \_,__/\ \____\\ \ ,__/\ \ ,__/\ \__/.\_\          *
 *             \/____/ \/_/\/___/  \/____/ \ \ \/  \ \ \/  \/__/\/_/          *
 *                                          \ \_\   \ \_\                     *
 *                                           \/_/    \/_/                     *
 *                                                                            *
 * Copyright (C) 2011, 2012                                                   *
 * Dominik Charousset <dominik.charousset@haw-hamburg.de>                     *
 *                                                                            *
 * This file is part of libcppa.                                              *
 * libcppa is free software: you can redistribute it and/or modify it under   *
 * the terms of the GNU Lesser General Public License as published by the     *
 * Free Software Foundation, either version 3 of the License                  *
 * or (at your option) any later version.                                     *
 *                                                                            *
 * libcppa is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                       *
 * See the GNU Lesser General Public License for more details.                *
 *                                                                            *
 * You should have received a copy of the GNU Lesser General Public License   *
 * along with libcppa. If not, see <http://www.gnu.org/licenses/>.            *
\******************************************************************************/


#include <set>
#include <map>
#include <cerrno>
#include <vector>
#include <memory>
#include <cstring>
#include <sstream>
#include <iostream>

#include "cppa/on.hpp"
#include "cppa/actor.hpp"
#include "cppa/match.hpp"
#include "cppa/config.hpp"
#include "cppa/either.hpp"
#include "cppa/to_string.hpp"
#include "cppa/actor_proxy.hpp"
#include "cppa/binary_serializer.hpp"
#include "cppa/uniform_type_info.hpp"
#include "cppa/binary_deserializer.hpp"
#include "cppa/process_information.hpp"

#include "cppa/util/buffer.hpp"
#include "cppa/util/acceptor.hpp"
#include "cppa/util/input_stream.hpp"
#include "cppa/util/output_stream.hpp"

#include "cppa/detail/middleman.hpp"
#include "cppa/detail/actor_registry.hpp"
#include "cppa/detail/addressed_message.hpp"
#include "cppa/detail/actor_proxy_cache.hpp"

//#define VERBOSE_MIDDLEMAN

#ifdef VERBOSE_MIDDLEMAN
#define DEBUG(arg) {                                                           \
    ostringstream oss;                                                         \
    oss << "[process id: "                                                     \
        << cppa::process_information::get()->process_id()                      \
        << "] " << arg << endl;                                                \
    cout << oss.str() << flush;                                                \
} (void) 0
#else
#define DEBUG(unused) ((void) 0)
#endif

//  use epoll event-loop implementation on Linux if not explicitly overriden
//  by using CPPA_POLL_IMPL, use (default) poll implementation otherwise
#if defined(CPPA_LINUX) && !defined(CPPA_POLL_IMPL)
#   define CPPA_EPOLL_IMPL
#   include <sys/epoll.h>
#else
#   define CPPA_POLL_IMPL
#   include <poll.h>
#endif

using namespace std;

namespace cppa { namespace detail {

namespace {

const size_t ui32_size = sizeof(uint32_t);

template<typename T, typename... Args>
void call_ctor(T& var, Args&&... args) {
    new (&var) T (forward<Args>(args)...);
}

template<typename T>
void call_dtor(T& var) {
    var.~T();
}

template<class Container, class Element>
void erase_from(Container& haystack, const Element& needle) {
    typedef typename Container::value_type value_type;
    auto last = end(haystack);
    auto i = find_if(begin(haystack), last, [&](const value_type& value) {
        return value == needle;
    });
    if (i != last) haystack.erase(i);
}

template<class Container, class UnaryPredicate>
void erase_from_if(Container& container, const UnaryPredicate& predicate) {
    auto last = end(container);
    auto i = find_if(begin(container), last, predicate);
    if (i != last) container.erase(i);
}

} // namespace <anonmyous>

middleman_message::middleman_message()
: next(0), type(middleman_message_type::shutdown) { }

middleman_message::middleman_message(util::io_stream_ptr_pair a0,
                                     process_information_ptr a1)
: next(0), type(middleman_message_type::add_peer) {
   call_ctor(new_peer, move(a0), move(a1));
}

middleman_message::middleman_message(unique_ptr<util::acceptor> a0,
                                     actor_ptr a1)
: next(0), type(middleman_message_type::publish) {
    call_ctor(new_published_actor, move(a0), move(a1));
}

middleman_message::middleman_message(actor_ptr a0)
: next(0), type(middleman_message_type::unpublish) {
    call_ctor(published_actor, move(a0));
}

middleman_message::middleman_message(process_information_ptr a0,
                                     addressed_message a1)
: next(0), type(middleman_message_type::outgoing_message) {
    call_ctor(out_msg, move(a0), move(a1));
}

middleman_message::~middleman_message() {
    switch (type) {
        case middleman_message_type::add_peer: {
            call_dtor(new_peer);
            break;
        }
        case middleman_message_type::publish: {
            call_dtor(new_published_actor);
            break;
        }
        case middleman_message_type::unpublish: {
            call_dtor(published_actor);
            break;
        }
        case middleman_message_type::outgoing_message: {
            call_dtor(out_msg);
            break;
        }
        default: break;
    }
}

class middleman;

typedef intrusive::single_reader_queue<middleman_message> middleman_queue;

class network_channel : public ref_counted {

 public:

    network_channel(middleman* ptr, native_socket_type read_fd)
    : m_parent(ptr), m_read_handle(read_fd) { }

    virtual bool continue_reading() = 0;

    inline native_socket_type read_handle() const {
        return m_read_handle;
    }

    virtual bool is_acceptor_of(const actor_ptr&) const {
        return false;
    }

 protected:

    inline middleman* parent() { return m_parent; }
    inline const middleman* parent() const { return m_parent; }

 private:

    middleman* m_parent;
    native_socket_type m_read_handle;

};

typedef intrusive_ptr<network_channel> network_channel_ptr;
typedef vector<network_channel_ptr> network_channel_ptr_vector;

class peer_connection : public network_channel {

    typedef network_channel super;

 public:

    peer_connection(middleman* parent,
                    util::input_stream_ptr istream,
                    util::output_stream_ptr ostream,
                    process_information_ptr peer_ptr = nullptr)
    : super(parent, istream->read_file_handle())
    , m_istream(istream), m_ostream(ostream), m_peer(peer_ptr)
    , m_rd_state((peer_ptr) ? wait_for_msg_size : wait_for_process_info)
    , m_meta_msg(uniform_typeid<addressed_message>())
    , m_has_unwritten_data(false)
    , m_write_handle(ostream->write_file_handle()) {
        m_rd_buf.reset(m_rd_state == wait_for_process_info
                       ? ui32_size + process_information::node_id_size
                       : ui32_size);
    }

    ~peer_connection() {
        if (m_peer) {
            // collect all children (proxies to actors of m_peer)
            vector<actor_proxy_ptr> children;
            children.reserve(20);
            get_actor_proxy_cache().erase_all(m_peer->node_id(),
                                              m_peer->process_id(),
                                              [&](actor_proxy_ptr& pptr) {
                children.push_back(move(pptr));
            });
            // kill all proxies
            for (actor_proxy_ptr& pptr: children) {
                pptr->enqueue(nullptr,
                              make_any_tuple(atom("KILL_PROXY"),
                                             exit_reason::remote_link_unreachable));
            }
        }
    }

    inline native_socket_type write_handle() const {
        return m_write_handle;
    }

    bool continue_reading();

    bool continue_writing() {
        DEBUG("peer_connection::continue_writing, try to write "
              << m_wr_buf.size() << " bytes");
        if (has_unwritten_data()) {
            size_t written;
            written = m_ostream->write_some(m_wr_buf.data(),
                                            m_wr_buf.size());
            if (written != m_wr_buf.size()) {
                m_wr_buf.erase_leading(written);
                DEBUG("only " << written  << " bytes written");
            }
            else {
                m_wr_buf.reset();
                has_unwritten_data(false);
            }
        }
        return true;
    }

    void write(const addressed_message& msg) {
        binary_serializer bs(&m_wr_buf);
        std::uint32_t size = 0;
        auto before = m_wr_buf.size();
        m_wr_buf.write(sizeof(std::uint32_t), &size, util::grow_if_needed);
        bs << msg;
        size = (m_wr_buf.size() - before) - sizeof(std::uint32_t);
        // update size in buffer
        memcpy(m_wr_buf.data() + before, &size, sizeof(std::uint32_t));
        if (!has_unwritten_data()) {
            size_t written = m_ostream->write_some(m_wr_buf.data(),
                                                   m_wr_buf.size());
            if (written != m_wr_buf.size()) {
                DEBUG("tried to write " << m_wr_buf.size()
                      << " bytes, only " << written << " bytes written");
                m_wr_buf.erase_leading(written);
                has_unwritten_data(true);
            }
            else {
                DEBUG(written << " bytes written");
                m_wr_buf.reset();
            }
        }
    }

    inline bool has_unwritten_data() const {
        return m_has_unwritten_data;
    }

 protected:

    inline void has_unwritten_data(bool value) {
        m_has_unwritten_data = value;
    }

 private:

    enum read_state {
        // connection just established; waiting for process information
        wait_for_process_info,
        // wait for the size of the next message
        wait_for_msg_size,
        // currently reading a message
        read_message
    };

    util::input_stream_ptr m_istream;
    util::output_stream_ptr m_ostream;
    process_information_ptr m_peer;
    read_state m_rd_state;
    const uniform_type_info* m_meta_msg;
    bool m_has_unwritten_data;
    native_socket_type m_write_handle;

    util::buffer m_rd_buf;
    util::buffer m_wr_buf;

};

typedef intrusive_ptr<peer_connection> peer_connection_ptr;
typedef map<process_information, peer_connection_ptr> peer_map;

class middleman_listener {

 public:

    virtual void channel_added(const network_channel_ptr& ptr) = 0;
    virtual void channel_erased(const network_channel_ptr& ptr) = 0;
    virtual void continue_writing_later(const peer_connection_ptr& ptr) = 0;

    virtual ~middleman_listener() { }

};

class middleman {

    friend class event_loop_impl;

 public:

    middleman()
    : m_done(false), m_pself(process_information::get()), m_listener(nullptr) {}

    inline void add_channel_ptr(const network_channel_ptr& ptr) {
        m_channels.push_back(ptr);
        CPPA_REQUIRE(m_listener != nullptr);
        m_listener->channel_added(ptr);
    }

    template<class Connection, typename... Args>
    inline void add_channel(Args&&... args) {
        add_channel_ptr(new Connection(this, forward<Args>(args)...));
    }

    inline void add_peer(const process_information& pinf, peer_connection_ptr cptr) {
        auto& ptrref = m_peers[pinf];
        if (ptrref) { DEBUG("peer already defined!"); }
        else ptrref = cptr;
    }

    void operator()(int pipe_fd, middleman_queue& queue);

    inline const process_information_ptr& pself() {
        return m_pself;
    }

    inline void quit() {
        m_done = true;
    }

    peer_connection_ptr peer(const process_information& pinf) {
        auto i = m_peers.find(pinf);
        if (i != m_peers.end()) {
            CPPA_REQUIRE(i->second != nullptr);
            return i->second;
        }
        return nullptr;
    }

    network_channel_ptr acceptor_of(const actor_ptr& whom) {
        auto last = m_channels.end();
        auto i = find_if(m_channels.begin(), last, [=](network_channel_ptr& ptr) {
            return ptr->is_acceptor_of(whom);
        });
        return (i != last) ? *i : nullptr;
    }

    void continue_writing(const peer_connection_ptr& ptr) {
        CPPA_REQUIRE(m_listener != nullptr);
        m_listener->continue_writing_later(ptr);
    }

    void erase(network_channel_ptr ptr, bool invoked_from_listener = false) {
        CPPA_REQUIRE(m_listener != nullptr);
        if (!invoked_from_listener) m_listener->channel_erased(ptr);
        erase_from(m_channels, ptr);
    }

    inline bool done() const { return m_done; }

    middleman_listener* listener() {
        return m_listener;
    }

    void listener(middleman_listener* listener) {
        m_listener = listener;
    }

 private:

    bool m_done;
    process_information_ptr m_pself;
    middleman_listener* m_listener;

    peer_map m_peers;
    network_channel_ptr_vector m_channels;

};

bool peer_connection::continue_reading() {
    //DEBUG("peer_connection::continue_reading");
    for (;;) {
        m_rd_buf.append_from(m_istream.get());
        if (!m_rd_buf.full()) return true; // try again later
        switch (m_rd_state) {
            case wait_for_process_info: {
                //DEBUG("peer_connection::continue_reading: "
                //      "wait_for_process_info");
                uint32_t process_id;
                process_information::node_id_type node_id;
                memcpy(&process_id, m_rd_buf.data(), sizeof(uint32_t));
                memcpy(node_id.data(), m_rd_buf.data() + sizeof(uint32_t),
                       process_information::node_id_size);
                m_peer.reset(new process_information(process_id, node_id));
                if (*(parent()->pself()) == *m_peer) {
#                   ifdef VERBOSE_MIDDLEMAN
                    DEBUG("incoming connection from self");
#                   elif defined(CPPA_DEBUG)
                    std::cerr << "*** middleman warning: "
                                 "incoming connection from self"
                              << std::endl;
#                   endif
                    throw std::ios_base::failure("refused connection from self");
                }
                parent()->add_peer(*m_peer, this);
                // initialization done
                m_rd_state = wait_for_msg_size;
                m_rd_buf.reset(sizeof(uint32_t));
                DEBUG("pinfo read: "
                      << m_peer->process_id()
                      << "@"
                      << to_string(m_peer->node_id()));
                break;
            }
            case wait_for_msg_size: {
                //DEBUG("peer_connection::continue_reading: wait_for_msg_size");
                uint32_t msg_size;
                memcpy(&msg_size, m_rd_buf.data(), sizeof(uint32_t));
                //DEBUG("msg_size: " << msg_size);
                m_rd_buf.reset(msg_size);
                m_rd_state = read_message;
                break;
            }
            case read_message: {
                //DEBUG("peer_connection::continue_reading: read_message");
                addressed_message msg;
                binary_deserializer bd(m_rd_buf.data(), m_rd_buf.size());
                m_meta_msg->deserialize(&msg, &bd);
                auto& content = msg.content();
                //DEBUG("<-- " << to_string(msg));
                match(content) (
                    // monitor messages are sent automatically whenever
                    // actor_proxy_cache creates a new proxy
                    // note: aid is the *original* actor id
                    on(atom("MONITOR"), arg_match) >> [&](const process_information_ptr& peer, actor_id aid) {
                        if (!peer) {
                            DEBUG("MONITOR received from invalid peer");
                            return;
                        }
                        auto ar = singleton_manager::get_actor_registry();
                        auto reg_entry = ar->get_entry(aid);
                        auto pself = parent()->pself();
                        auto send_kp = [=](uint32_t reason) {
                            middleman_enqueue(peer,
                                              nullptr,
                                              nullptr,
                                              make_any_tuple(
                                                  atom("KILL_PROXY"),
                                                  pself,
                                                  aid,
                                                  reason
                                              ));
                        };
                        if (reg_entry.first == nullptr) {
                            if (reg_entry.second == exit_reason::not_exited) {
                                // invalid entry
                                DEBUG("MONITOR for an unknown actor received");
                            }
                            else {
                                // this actor already finished execution;
                                // reply with KILL_PROXY message
                                send_kp(reg_entry.second);
                            }
                        }
                        else {
                            reg_entry.first->attach_functor(send_kp);
                        }
                    },
                    on(atom("KILL_PROXY"), arg_match) >> [&](const process_information_ptr& peer, actor_id aid, std::uint32_t reason) {
                        auto& cache = get_actor_proxy_cache();
                        auto proxy = cache.get(aid,
                                               peer->process_id(),
                                               peer->node_id());
                        if (proxy) {
                            proxy->enqueue(nullptr,
                                           make_any_tuple(
                                               atom("KILL_PROXY"), reason));
                        }
                        else {
                            DEBUG("received KILL_PROXY message but didn't "
                                  "found matching instance in cache");
                        }
                    },
                    on(atom("LINK"), arg_match) >> [&](const actor_ptr& ptr) {
                        if (msg.sender()->is_proxy() == false) {
                            DEBUG("msg.sender() is not a proxy");
                            return;
                        }
                        auto whom = msg.sender().downcast<actor_proxy>();
                        if ((whom) && (ptr)) whom->local_link_to(ptr);
                    },
                    on(atom("UNLINK"), arg_match) >> [](const actor_ptr& ptr) {
                        if (ptr->is_proxy() == false) {
                            DEBUG("msg.sender() is not a proxy");
                            return;
                        }
                        auto whom = ptr.downcast<actor_proxy>();
                        if ((whom) && (ptr)) whom->local_unlink_from(ptr);
                    },
                    others() >> [&] {
                        auto receiver = msg.receiver().get();
                        if (receiver) {
                            if (msg.id().valid()) {
                                auto ra = dynamic_cast<actor*>(receiver);
                                DEBUG("sync message for actor "
                                      << ra->id());
                                if (ra) {
                                    ra->sync_enqueue(
                                        msg.sender().get(),
                                        msg.id(),
                                        move(msg.content()));
                                }
                                else{
                                    DEBUG("ERROR: sync message to a non-actor");
                                }
                            }
                            else {
                                DEBUG("async message (sender is "
                                      << (msg.sender() ? "valid" : "NULL")
                                      << ")");
                                receiver->enqueue(
                                    msg.sender().get(),
                                    move(msg.content()));
                            }
                        }
                        else {
                            DEBUG("empty receiver");
                        }
                    }
                );
                m_rd_buf.reset(sizeof(uint32_t));
                m_rd_state = wait_for_msg_size;
                break;
            }
            default: {
                CPPA_CRITICAL("illegal state");
            }
        }
        // try to read more (next iteration)
    }
}

class peer_acceptor : public network_channel {

    typedef network_channel super;

 public:

    peer_acceptor(middleman* parent,
                  actor_id aid,
                  unique_ptr<util::acceptor> acceptor)
    : super(parent, acceptor->acceptor_file_handle())
    , m_actor_id(aid)
    , m_acceptor(move(acceptor)) { }

    bool is_doorman_of(actor_id aid) const {
        return m_actor_id == aid;
    }

    bool continue_reading() {
        //DEBUG("peer_acceptor::continue_reading");
        // accept as many connections as possible
        for (;;) {
            auto opt = m_acceptor->try_accept_connection();
            if (opt) {
                auto& pair = *opt;
                auto& pself = parent()->pself();
                uint32_t process_id = pself->process_id();
                pair.second->write(&m_actor_id, sizeof(actor_id));
                pair.second->write(&process_id, sizeof(uint32_t));
                pair.second->write(pself->node_id().data(),
                                   pself->node_id().size());
                parent()->add_channel<peer_connection>(pair.first,
                                                       pair.second);
            }
            else {
                return true;
            }
       }
    }

 private:

    actor_id m_actor_id;
    unique_ptr<util::acceptor> m_acceptor;

};

class middleman_overseer : public network_channel {

    typedef network_channel super;

 public:

    middleman_overseer(middleman* parent, int pipe_fd, middleman_queue& q)
    : super(parent, pipe_fd), m_queue(q) { }

    bool continue_reading() {
        //DEBUG("middleman_overseer::continue_reading");
        static constexpr size_t num_dummies = 256;
        uint8_t dummies[num_dummies];
        auto read_result = ::read(read_handle(), dummies, num_dummies);
        if (read_result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // try again later
                return true;
            }
            else {
                CPPA_CRITICAL("cannot read from pipe");
            }
        }
        atomic_thread_fence(memory_order_seq_cst);
        for (int i = 0; i < read_result; ++i) {
            unique_ptr<middleman_message> msg(m_queue.try_pop());
            if (!msg) { CPPA_CRITICAL("nullptr dequeued"); }
            switch (msg->type) {
                case middleman_message_type::add_peer: {
                    DEBUG("middleman_overseer: add_peer: "
                          << to_string(*(msg->new_peer.second)));
                    auto& new_peer = msg->new_peer;
                    auto& io_ptrs = new_peer.first;
                    peer_connection_ptr peer;
                    peer.reset(new peer_connection(parent(),
                                                   io_ptrs.first,
                                                   io_ptrs.second,
                                                   new_peer.second));
                    parent()->add_channel_ptr(peer);
                    parent()->add_peer(*(new_peer.second), peer);
                    break;
                }
                case middleman_message_type::publish: {
                    DEBUG("middleman_overseer: publish");
                    auto& ptrs = msg->new_published_actor;
                    parent()->add_channel<peer_acceptor>(ptrs.second->id(),
                                                         move(ptrs.first));
                    break;
                }
                case middleman_message_type::unpublish: {
                    if (msg->published_actor) {
                        //DEBUG("middleman_overseer: unpublish actor id "
                        //      << msg->published_actor->id());
                        auto channel = parent()->acceptor_of(msg->published_actor);
                        if (channel) {
                            parent()->erase(channel);
                        }
                    }
                    break;
                }
                case middleman_message_type::outgoing_message: {
                    //DEBUG("middleman_overseer: outgoing_message");
                    auto& target_peer = msg->out_msg.first;
                    auto& out_msg = msg->out_msg.second;
                    CPPA_REQUIRE(target_peer != nullptr);
                    auto peer = parent()->peer(*target_peer);
                    if (!peer) {
                        DEBUG("message to an unknown peer: " << to_string(out_msg));
                        break;
                    }
                    //DEBUG("--> " << to_string(out_msg));
                    auto had_unwritten_data = peer->has_unwritten_data();
                    try {
                        peer->write(out_msg);
                        if (!had_unwritten_data && peer->has_unwritten_data()) {
                            parent()->continue_writing(peer);
                        }
                    }
                    catch (exception& e) {
                        DEBUG("peer disconnected: " << e.what());
                        parent()->erase(peer);
                    }
                    break;
                }
                case middleman_message_type::shutdown: {
                    DEBUG("middleman: shutdown");
                    parent()->quit();
                    break;
                }
            }
        }
        return true;
    }

 private:

    middleman_queue& m_queue;

};

#ifdef CPPA_POLL_IMPL

class event_loop_impl : public middleman_listener {

 public:

    void operator()() {
        while (!m_parent->done()) {
            CPPA_REQUIRE(!m_pollset.empty() && m_pollset.size() == m_ioset.size());
            int presult;
            do {
                DEBUG("poll() on " << pollset.size() << " sockets");
                presult = poll(m_pollset.data(), m_pollset.size(), -1);
                DEBUG("poll() returned " << presult);
                if (presult < 0) {
                    presult = 0; // die hard
                    switch (errno) {
                        // a signal was caught
                        case EINTR: {
                            // just try again
                            break;
                        }
                        // nfds is negative or the value
                        // contained within timeout is invalid
                        case EINVAL: {
                            CPPA_CRITICAL("poll EINVAL: too much sockets");
                            break;
                        }
                        case ENOMEM: {
                            // there's not much we can do other than try again
                            // sleep some time in hope someone releases memory
                            // while we are sleeping
                            //this_thread::yield();
                            break;
                        }
                        // array given as argument was not contained
                        // in the calling program's address space
                        case EFAULT: {
                            // must not happen
                            CPPA_CRITICAL("poll EFAULT");
                            break;
                        }
                        case EBADF: {
                            // this really shouldn't happen
                            // try IO on each single socket and try again
                            size_t i = 0;
                            while (i < m_pollset.size()) {
                                if (!proceed(m_ioset[i])) erase_at(i);
                                else ++i;
                            }
                            break;
                        }
                        default: {
                            CPPA_CRITICAL("select() failed for an unknown reason");
                        }
                    }
                }
            }
            while (presult == 0);
            //DEBUG("continue reading ...");
            // presult is the number of structures which have nonzero revents fields
            // iterate over m_pollset until we've handled all events
            size_t i = 0;    // position in both m_pollset and m_ioset
            int handled = 0; // number of handled events
            while (handled < presult) {
                auto revents = m_pollset[i].revents;
                m_pollset[i].revents = 0;
                if (revents == 0) {
                    // there's nothing to see here
                    ++i; // move along
                }
                else {
                    ++handled; // got 'em!
                    if (revents & (POLLIN | POLLOUT)) {
                        // continue and erase on error or done
                        if (!proceed(m_ioset[i])) erase_at(i);
                        else ++i;
                    }
                    else {
                        // treat everything else as error
                        erase_at(i);
                    }
                }
            }
            // sweep deleted channels
            while (!m_marked_as_deleted.empty()) {
                auto ptr = m_marked_as_deleted.back();
                m_marked_as_deleted.pop_back();
                auto first = begin(m_ioset);
                auto last = end(m_ioset);
                auto iter = find_if(first, last, [=](const io_element& e) {
                    return e.inspect<bool>([=](const network_channel_ptr& rhs) {
                        return ptr == rhs;
                    });
                });
                if (iter != last) {
                    erase_at(distance(first, iter));
                }
            }
        }
    }

    void channel_added(const network_channel_ptr& ptr) {
        pollfd pfd;
        pfd.fd = ptr->read_handle();
        pfd.events = POLLIN;
        pfd.revents = 0;
        m_pollset.push_back(pfd);
        m_ioset.emplace_back(ptr);
    }

    void channel_erased(const network_channel_ptr& ptr) {
        m_marked_as_deleted.push_back(ptr);
    }

    void continue_writing_later(const peer_connection_ptr& ptr) {
        pollfd pfd;
        pfd.fd = ptr->write_handle();
        pfd.events = POLLOUT;
        pfd.revents = 0;
        m_pollset.push_back(pfd);
        m_ioset.emplace_back(ptr);
    }

    void init(middleman* parent) {
        CPPA_REQUIRE(parent->m_channels.empty());
        m_parent = parent;
        parent->listener(this);
    }

 private:

    typedef either<network_channel_ptr, peer_connection_ptr> io_element;

    inline bool proceed(const io_element& ioe) {
        try {
            return ioe.is_left() ? ioe.left()->continue_reading()
                                 : (    ioe.right()->continue_writing()
                                    && !ioe.right()->has_unwritten_data());
        }
        catch (ios_base::failure& e) {
            DEBUG(demangle(typeid(e)) << ": " << e.what());
        }
        catch (runtime_error& e) {
            // thrown whenever serialize/deserialize fails
            cerr << "*** runtime_error in middleman: " << e.what() << endl;
        }
        catch (exception& e) {
            DEBUG(demangle(typeid(e)) << ": " << e.what());
        }
        return false;
    }

    inline void erase_at(size_t pos) {
        if (m_ioset[pos].is_left()) {
            m_parent->erase(m_ioset[pos].left(), true);
        }
        // else: output channels are removed as they don't have any more data
        //       to write, therefore it's not treated as error here
        m_ioset.erase(m_ioset.begin() + pos);
        m_pollset.erase(m_pollset.begin() + pos);
    }

    middleman* m_parent;
    vector<network_channel_ptr> m_marked_as_deleted;
                                  // _
    vector<pollfd>     m_pollset; //  \                                        |
                                  //   > these two vectors are always in sync  |
    vector<io_element> m_ioset;   // _/                                        |
                                  //

};

#elif defined(CPPA_EPOLL_IMPL)

class event_loop_impl : public middleman_listener {

 public:

    event_loop_impl() : m_epollfd(-1) { }

    void init(middleman* parent) {
        CPPA_REQUIRE(parent != nullptr);
        parent->listener(this);
        m_parent = parent;
        m_epollfd = epoll_create1(EPOLL_CLOEXEC);
        if (m_epollfd == -1) throw ios_base::failure(  string("epoll_create1: ")
                                                     + strerror(errno));
        // handle at most 64 events at a time
        m_events.resize(64);
    }

    ~event_loop_impl() {
        if (m_epollfd != -1) close(m_epollfd);
    }

    // operation: EPOLL_CTL_ADD or EPOLL_CTL_DEL
    // fd_op: EPOLLIN, EPOLLOUT or (EPOLLIN | EPOLLOUT)
    template<typename T = void>
    void epoll_op(int operation, int fd, int fd_op, T* ptr = nullptr) {
        CPPA_REQUIRE(operation == EPOLL_CTL_ADD || operation == EPOLL_CTL_DEL);
        CPPA_REQUIRE(operation == EPOLL_CTL_DEL || ptr != nullptr);
        CPPA_REQUIRE(fd_op == EPOLLIN || fd_op == EPOLLOUT || fd_op == (EPOLLIN | EPOLLOUT));
        // make sure T has correct type
        CPPA_REQUIRE(   (operation == EPOLL_CTL_DEL && is_same<T,void>::value)
                     || ((fd_op & EPOLLIN) && is_same<T,network_channel>::value)
                     || (fd_op == EPOLLOUT && is_same<T,peer_connection>::value));
        epoll_event ee;
        // also fire event on peer shutdown on input operations
        ee.events = (fd_op & EPOLLIN) ? (fd_op | EPOLLRDHUP) : fd_op;
        // always store peer_connection_ptr, because we don't have full type information
        // in case of epoll_wait error otherwise
        ee.data.ptr = static_cast<peer_connection*>(ptr);
        // check wheter fd is already registered to epoll
        auto iter = m_epoll_data.find(fd);
        if (iter != end(m_epoll_data)) {
            if (operation == EPOLL_CTL_ADD) {
                // modify instead
                operation = EPOLL_CTL_MOD;
                ee.events |= iter->second.events;
                iter->second.events = ee.events;
                CPPA_REQUIRE(ee.data.ptr == iter->second.data.ptr);
            }
            else if (operation == EPOLL_CTL_DEL) {
                // check wheter we have this fd registered for other fd_ops
                ee.events = iter->second.events & ~(ee.events);
                if (ee.events != 0) {
                    // modify instead
                    ee.data.ptr = iter->second.data.ptr;
                    iter->second.events = ee.events;
                    operation = EPOLL_CTL_MOD;
                }
                else {
                    // erase from map as well
                    m_epoll_data.erase(iter);
                }
            }
        }
        else if (operation == EPOLL_CTL_DEL) {
            // nothing to delete here
            return;
        }
        else { // operation == EPOLL_CTL_ADD
            CPPA_REQUIRE(operation == EPOLL_CTL_ADD);
            m_epoll_data.insert(make_pair(fd, ee));
        }
        if (epoll_ctl(m_epollfd, operation, fd, &ee) < 0) {
            switch (errno) {
                // m_epollfd or read_handle() is not a valid file descriptor
                case EBADF: {
                    // this is a critical bug, there's no plan B here
                    CPPA_CRITICAL("epoll_ctl returned EBADF");
                    break;
                }
                // supplied file descriptor is already registered
                case EEXIST: {
                    // shouldn't happen, but no big deal
                    cerr << "*** warning: file descriptor registered twice\n"
                         << flush;
                    break;
                }
                // m_pollfd not an epoll file descriptor, or read_handle()
                // is the same as m_pollfd, or read_handle() isn't supported
                // by epoll
                case EINVAL: {
                    // point of no return
                    CPPA_CRITICAL("epoll_ctl returned EINVAL");
                    break;
                }
                // op was EPOLL_CTL_MOD or EPOLL_CTL_DEL,
                // and fd is not registered with this epoll instance.
                case ENOENT: {
                    cerr << "*** warning: cannot delete file descriptor "
                            "because it isn't registered\n"
                         << flush;
                    break;
                }
                // insufficient memory to handle requested
                case ENOMEM: {
                    // what the ... ?
                    CPPA_CRITICAL("not enough memory for epoll operation");
                    break;
                }
                // The limit imposed by /proc/sys/fs/epoll/max_user_watches
                // was encountered while trying to register a new file descriptor
                case ENOSPC: {
                    CPPA_CRITICAL("reached max_user_watches limit");
                    break;
                }
                // The target file fd does not support epoll.
                case EPERM: {
                    CPPA_CRITICAL("tried to add illegal file descriptor");
                    break;
                }
            }
        }
    }

    void channel_added(const network_channel_ptr& ptr) {
        epoll_op(EPOLL_CTL_ADD, ptr->read_handle(), EPOLLIN, ptr.get());
    }

    void channel_erased(const network_channel_ptr& ptr) {
        m_marked_inputs.push_back(ptr);
    }

    void continue_writing_later(const peer_connection_ptr& ptr) {
        epoll_op(EPOLL_CTL_ADD, ptr->write_handle(), EPOLLOUT, ptr.get());
    }

    template<typename F>
    inline bool proceed(const F& fun) {
        try { return fun(); }
        catch (ios_base::failure& e) {
            DEBUG(demangle(typeid(e)) << ": " << e.what());
        }
        catch (runtime_error& e) {
            // thrown whenever serialize/deserialize fails
            cerr << "*** runtime_error in middleman: " << e.what() << endl;
        }
        catch (exception& e) {
            DEBUG(demangle(typeid(e)) << ": " << e.what());
        }
        return false;
    }

    void operator()() {
        while (!m_parent->done()) {
            // make sure m_events grows with the number of observed sockets
            int presult;
            do {
                DEBUG("epoll_wait on " << m_epoll_data.size() << " sockets");
                presult = epoll_wait(m_epollfd, m_events.data(), (int) m_events.size(), -1);
                DEBUG("epoll_wait returned " << presult);
                if (presult < 0) {
                    // try again unless critical error occured
                    presult = 0;
                    switch (errno) {
                        // a signal was caught
                        case EINTR: {
                            // just try again
                            break;
                        }
                        // m_epollfd is not an epoll file descriptor,
                        // or maxevents is less than or equal to zero.
                        case EINVAL: {
                            CPPA_CRITICAL("epoll_wait returned EINVAL");
                            break;
                        }
                        // array given as argument was not contained
                        // in the calling program's address space
                        case EFAULT: {
                            // must not happen
                            CPPA_CRITICAL("epoll_wait returned EFAULT");
                            break;
                        }
                        // m_epollfd is not a file descriptor
                        case EBADF: {
                            CPPA_CRITICAL("epoll_wait returned EBADF");
                            break;
                        }
                        default: {
                            CPPA_CRITICAL("epoll_wait failed with unknown error code");
                        }
                    }
                }
            }
            while (presult == 0);
            for (int i = 0; i < presult; ++i) {
                auto& e = m_events[i];
                // skip invalid entries
                if (e.events == 0) continue;
                if (e.data.ptr == nullptr) {
                    // this *really* shouldn't happen
                    CPPA_CRITICAL("epoll_wait returned nullptr instead of registered ptr");
                }
                if (e.events & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)) {
                    auto ptr = reinterpret_cast<network_channel*>(e.data.ptr);
                    // see you later!
                    m_marked_inputs.push_back(ptr);
                }
                else {
                    // can be both!
                    if (e.events & EPOLLIN) {
                        auto ptr = reinterpret_cast<network_channel*>(e.data.ptr);
                        if (!proceed([ptr] { return ptr->continue_reading(); })) {
                            epoll_op(EPOLL_CTL_DEL, ptr->read_handle(), EPOLLIN);
                        }
                    }
                    if (e.events & EPOLLOUT) {
                        auto bptr = reinterpret_cast<network_channel*>(e.data.ptr);
                        // this cast is safe, because we *always* make sure
                        // to register network_channel pointers with EPOLLOUT
                        auto ptr = static_cast<peer_connection*>(bptr);
                        if (!proceed([ptr] { return ptr->continue_writing(); })) {
                            epoll_op(EPOLL_CTL_DEL, ptr->write_handle(), EPOLLOUT);
                        }
                    }
                }
            }
            // sweep marked pointers
            for (auto& ptr : m_marked_inputs) {
                auto fd = ptr->read_handle();
                epoll_op(EPOLL_CTL_DEL, fd, EPOLLIN | EPOLLOUT);
                // remove write handle as well if possible, since we don't
                // support read-only connections anyways
                auto wptr = dynamic_cast<peer_connection*>(ptr.get());
                if (wptr) {
                    auto wfd = wptr->write_handle();
                    if (fd != wfd) epoll_op(EPOLL_CTL_DEL, wfd, EPOLLOUT);
                    // else: already deleted
                }
            }
            // cleanup
            m_marked_inputs.clear();
            // next iteration
        }
    }

 private:

    int m_epollfd;
    middleman* m_parent;
    vector<network_channel_ptr> m_marked_inputs;

    vector<epoll_event> m_events;
    map<int,epoll_event> m_epoll_data;

};

#endif

void middleman::operator()(int pipe_fd, middleman_queue& queue) {
    DEBUG("pself: " << to_string(*m_pself));
    event_loop_impl loop;
    loop.init(this);
    add_channel_ptr(new middleman_overseer(this, pipe_fd, queue));
    loop();
    DEBUG("middleman done");
}

void middleman_loop(int pipe_fd, middleman_queue& queue) {
    DEBUG("run middleman loop");
    middleman mm;
    mm(pipe_fd, queue);
    DEBUG("middleman loop done");
}

} } // namespace cppa::detail
