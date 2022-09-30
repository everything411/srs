//
// Copyright (c) 2013-2022 The SRS Authors
//
// SPDX-License-Identifier: MIT or MulanPSL-2.0
//

#ifndef SRS_APP_CONN_HPP
#define SRS_APP_CONN_HPP

#include <srs_core.hpp>

#include <string>
#include <vector>
#include <map>

#include <openssl/ssl.h>

#include <srs_app_st.hpp>
#include <srs_protocol_kbps.hpp>
#include <srs_app_reload.hpp>
#include <srs_protocol_conn.hpp>

class SrsWallClock;
class SrsBuffer;

// Hooks for connection manager, to handle the event when disposing connections.
class ISrsDisposingHandler
{
public:
    ISrsDisposingHandler();
    virtual ~ISrsDisposingHandler();
public:
    // When before disposing resource, trigger when manager.remove(c), sync API.
    // @remark Recommend to unref c, after this, no other objects refs to c.
    virtual void on_before_dispose(ISrsResource* c) = 0;
    // When disposing resource, async API, c is freed after it.
    // @remark Recommend to stop any thread/timer of c, after this, fields of c is able
    // to be deleted in any order.
    virtual void on_disposing(ISrsResource* c) = 0;
};

// The item to identify the fast id object.
class SrsResourceFastIdItem
{
public:
    // If available, use the resource in item.
    bool available;
    // How many resource have the same fast-id, which contribute a collision.
    int nn_collisions;
    // The first fast-id of resources.
    uint64_t fast_id;
    // The first resource object.
    ISrsResource* impl;
public:
    SrsResourceFastIdItem() {
        available = false;
        nn_collisions = 0;
        fast_id = 0;
        impl = NULL;
    }
};

// The resource manager remove resource and delete it asynchronously.
class SrsResourceManager : public ISrsCoroutineHandler, public ISrsResourceManager
{
private:
    std::string label_;
    SrsContextId cid_;
    bool verbose_;
private:
    SrsCoroutine* trd;
    srs_cond_t cond;
    // Callback handlers.
    std::vector<ISrsDisposingHandler*> handlers_;
    // Unsubscribing handlers, skip it for notifying.
    std::vector<ISrsDisposingHandler*> unsubs_;
    // Whether we are removing resources.
    bool removing_;
    // The zombie connections, we will delete it asynchronously.
    std::vector<ISrsResource*> zombies_;
    std::vector<ISrsResource*>* p_disposing_;
private:
    // The connections without any id.
    std::vector<ISrsResource*> conns_;
    // The connections with resource id.
    std::map<std::string, ISrsResource*> conns_id_;
    // The connections with resource fast(int) id.
    std::map<uint64_t, ISrsResource*> conns_fast_id_;
    // The level-0 fast cache for fast id.
    int nn_level0_cache_;
    SrsResourceFastIdItem* conns_level0_cache_;
    // The connections with resource name.
    std::map<std::string, ISrsResource*> conns_name_;
public:
    SrsResourceManager(const std::string& label, bool verbose = false);
    virtual ~SrsResourceManager();
public:
    srs_error_t start();
    bool empty();
    size_t size();
// Interface ISrsCoroutineHandler
public:
    virtual srs_error_t cycle();
public:
    void add(ISrsResource* conn, bool* exists = NULL);
    void add_with_id(const std::string& id, ISrsResource* conn);
    void add_with_fast_id(uint64_t id, ISrsResource* conn);
    void add_with_name(const std::string& name, ISrsResource* conn);
    ISrsResource* at(int index);
    ISrsResource* find_by_id(std::string id);
    ISrsResource* find_by_fast_id(uint64_t id);
    ISrsResource* find_by_name(std::string name);
public:
    void subscribe(ISrsDisposingHandler* h);
    void unsubscribe(ISrsDisposingHandler* h);
// Interface ISrsResourceManager
public:
    virtual void remove(ISrsResource* c);
private:
    void do_remove(ISrsResource* c);
    void check_remove(ISrsResource* c, bool& in_zombie, bool& in_disposing);
    void clear();
    void do_clear();
    void dispose(ISrsResource* c);
};

// A simple lazy-sweep GC, just wait for a long time to delete the disposable resources.
class SrsSweepGc : public ISrsLazyGc
{
public:
    SrsSweepGc();
    virtual ~SrsSweepGc();
public:
    virtual srs_error_t start();
    virtual void remove(ISrsLazyResource* c);
};

extern ISrsLazyGc* _srs_gc;

// A wrapper template for lazy-sweep resource.
// See https://github.com/ossrs/srs/issues/3176#lazy-sweep
template<typename T>
class SrsLazyResourceWrapper : public ISrsResource
{
private:
    T* resource_;
    ISrsResource* wrapper_;
    bool is_root_;
public:
    SrsLazyResourceWrapper(T* resource = NULL, ISrsResource* wrapper = NULL) {
        wrapper_ = wrapper ? wrapper : this;
        resource_ = resource ? resource : new T();
        resource_->gc_use(wrapper_);

        is_root_ = !resource;
        if (!resource) {
            resource_->gc_set_creator_wrapper(wrapper_);
        }
    }
    virtual ~SrsLazyResourceWrapper() {
        resource_->gc_dispose(wrapper_);

        if (is_root_) {
            resource_->gc_set_creator_wrapper(NULL);
        }

        if (resource_->gc_ref() == 0) {
            _srs_gc->remove(resource_);
        }
    }
public:
    SrsLazyResourceWrapper<T>* copy() {
        return new SrsLazyResourceWrapper<T>(resource_);
    }
    T* resource() {
        return resource_;
    }
// Interface ISrsResource
public:
    virtual const SrsContextId& get_id() {
        return resource_->get_id();
    }
    virtual std::string desc() {
        return resource_->desc();
    }
};

// Use macro to generate a wrapper class, because typedef will cause IDE incorrect tips.
// See https://github.com/ossrs/srs/issues/3176#lazy-sweep
#define SRS_LAZY_WRAPPER_GENERATOR(Resource, IWrapper, IResource) \
    private: \
        SrsLazyResourceWrapper<Resource> impl_; \
    public: \
        Resource##Wrapper(Resource* resource = NULL) : impl_(resource, this) { \
        } \
        virtual ~Resource##Wrapper() { \
        } \
    public: \
        IWrapper* copy() { \
            return new Resource##Wrapper(impl_.resource()); \
        } \
        IResource* resource() { \
            return impl_.resource(); \
        } \
    public: \
        virtual const SrsContextId& get_id() { \
            return impl_.get_id(); \
        } \
        virtual std::string desc() { \
            return impl_.desc(); \
        } \

// If a connection is able be expired, user can use HTTP-API to kick-off it.
class ISrsExpire
{
public:
    ISrsExpire();
    virtual ~ISrsExpire();
public:
    // Set connection to expired to kick-off it.
    virtual void expire() = 0;
};

// The basic connection of SRS, for TCP based protocols,
// all connections accept from listener must extends from this base class,
// server will add the connection to manager, and delete it when remove.
class SrsTcpConnection : public ISrsProtocolReadWriter
{
private:
    // The underlayer st fd handler.
    srs_netfd_t stfd;
    // The underlayer socket.
    SrsStSocket* skt;
public:
    SrsTcpConnection(srs_netfd_t c);
    virtual ~SrsTcpConnection();
public:
    // Set socket option TCP_NODELAY.
    virtual srs_error_t set_tcp_nodelay(bool v);
    // Set socket option SO_SNDBUF in srs_utime_t.
    virtual srs_error_t set_socket_buffer(srs_utime_t buffer_v);
// Interface ISrsProtocolReadWriter
public:
    virtual void set_recv_timeout(srs_utime_t tm);
    virtual srs_utime_t get_recv_timeout();
    virtual srs_error_t read_fully(void* buf, size_t size, ssize_t* nread);
    virtual int64_t get_recv_bytes();
    virtual int64_t get_send_bytes();
    virtual srs_error_t read(void* buf, size_t size, ssize_t* nread);
    virtual void set_send_timeout(srs_utime_t tm);
    virtual srs_utime_t get_send_timeout();
    virtual srs_error_t write(void* buf, size_t size, ssize_t* nwrite);
    virtual srs_error_t writev(const iovec *iov, int iov_size, ssize_t* nwrite);
};

// With a small fast read buffer, to support peek for protocol detecting. Note that directly write to io without any
// cache or buffer.
class SrsBufferedReadWriter : public ISrsProtocolReadWriter
{
private:
    // The under-layer transport.
    ISrsProtocolReadWriter* io_;
    // Fixed, small and fast buffer. Note that it must be very small piece of cache, make sure matches all protocols,
    // because we will full fill it when peeking.
    char cache_[16];
    // Current reading position.
    SrsBuffer* buf_;
public:
    SrsBufferedReadWriter(ISrsProtocolReadWriter* io);
    virtual ~SrsBufferedReadWriter();
public:
    // Peek the head of cache to buf in size of bytes.
    srs_error_t peek(char* buf, int* size);
private:
    srs_error_t reload_buffer();
// Interface ISrsProtocolReadWriter
public:
    virtual srs_error_t read(void* buf, size_t size, ssize_t* nread);
    virtual srs_error_t read_fully(void* buf, size_t size, ssize_t* nread);
    virtual void set_recv_timeout(srs_utime_t tm);
    virtual srs_utime_t get_recv_timeout();
    virtual int64_t get_recv_bytes();
    virtual int64_t get_send_bytes();
    virtual void set_send_timeout(srs_utime_t tm);
    virtual srs_utime_t get_send_timeout();
    virtual srs_error_t write(void* buf, size_t size, ssize_t* nwrite);
    virtual srs_error_t writev(const iovec *iov, int iov_size, ssize_t* nwrite);
};

// The SSL connection over TCP transport, in server mode.
class SrsSslConnection : public ISrsProtocolReadWriter
{
private:
    // The under-layer plaintext transport.
    ISrsProtocolReadWriter* transport;
private:
    SSL_CTX* ssl_ctx;
    SSL* ssl;
    BIO* bio_in;
    BIO* bio_out;
public:
    SrsSslConnection(ISrsProtocolReadWriter* c);
    virtual ~SrsSslConnection();
public:
    virtual srs_error_t handshake(std::string key_file, std::string crt_file);
// Interface ISrsProtocolReadWriter
public:
    virtual void set_recv_timeout(srs_utime_t tm);
    virtual srs_utime_t get_recv_timeout();
    virtual srs_error_t read_fully(void* buf, size_t size, ssize_t* nread);
    virtual int64_t get_recv_bytes();
    virtual int64_t get_send_bytes();
    virtual srs_error_t read(void* buf, size_t size, ssize_t* nread);
    virtual void set_send_timeout(srs_utime_t tm);
    virtual srs_utime_t get_send_timeout();
    virtual srs_error_t write(void* buf, size_t size, ssize_t* nwrite);
    virtual srs_error_t writev(const iovec *iov, int iov_size, ssize_t* nwrite);
};

#endif
