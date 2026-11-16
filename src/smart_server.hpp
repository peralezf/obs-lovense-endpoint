// Clean-room Linux replacement for lovense_smart_server.hpp.
// SmartCam WS/WSS server core: websocketpp + asio (+ OpenSSL for WSS),
// AES-128-CBC SmartCam framing (see lovense_websocket_common.hpp).
//
// SmartCam protocol behavior implemented here:
//   on open ......... server sends {type:"105", to:<client-id>, data:{...}}
//                     (encrypted). The data block is REQUIRED by the cam
//                     extension (it dereferences data.version unguarded;
//                     2026-10-20 live capture + replay). Default values are
//                     replay-proven; set_pack_105_hook lets the app override
//                     the whole block. Side comes from the ?from=cam|smart
//                     query (default smart); a client-sent {type:"105",
//                     from:...} re-registers the side and re-sends the 105.
//   client -> server  {type:"103"} heartbeat (ignored, internal; client has
//                     no heartbeat timeout — deob 2026-10-20 — no echo).
//   client -> server  101 wrappers: data OBJECT {id,scale,algin,platform,v,
//                     type,data} = state/blast traffic (REQUIRED for video
//                     feedback) → dispatched to the app via
//                     set_dispatch_hook/Proxy on_plain_message (inner
//                     envelope only); data STRING = pre-encrypted inner
//                     frame (heartbeat lane when inner type is 103, other
//                     inner types go to the hook).
//   client -> server  {type:"105", from} re-registers the side.
//   client -> server  {type:"202", from, data} relayed VERBATIM
//                     (re-encrypted) to clients of the opposite side.
//   server -> client  101 snapshots {type:"101", to, data:{...}} via
//                     SendMessageToClient (routed to the opposite side).
// obs-compat query answering is OUT of scope (P2 plugin owns it).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/buffer.hpp>

#include "lovense_websocket_common.hpp"

namespace lovense {
namespace smart {

// Plugin version pair carried in Server::Config (rs_server.cpp:124-132).
struct PluginInfo {
	std::string dist_version;
	std::string toolset_version;
};

class WsServer;
class WssServer;

// Non-template base so Server can drive any Proxy<T> without knowing T.
class ProxyBase : public ws::WebServerImpl {
public:
	virtual void set_port_strategy(int strategy) = 0;
};

// Template proxy: owns a transport (WsServer/WssServer), binds it, and
// exposes the app hooks used in server/rs_proxy.hpp (on_start,
// on_open_client, get_dist/toolset_version, pack_105_message).
// socket_type requirements: nested Connection with send(string);
// listen(int) throwing on bind failure; run(); stop(); SocketType();
// SendMessageToClient; open-callback hook on_client_open.
template <typename socket_type> class Proxy : public ProxyBase {
public:
	using Connection = typename socket_type::Connection;

	Proxy(std::shared_ptr<socket_type> server, int port, PluginInfo plugin)
		: server_(server), port_(port), plugin_(plugin),
		  strategy_(0) // set via set_port_strategy before Start()
	{
	}

	void set_port_strategy(int strategy) override { strategy_ = strategy; }

	ws::WsocketType SocketType() const override
	{
		return server_->SocketType();
	}

	int port() const override { return port_; }

	// Bind with the configured strategy, then serve. Never throws:
	// failures are reported through on_start(err).
	void Start() override
	{
		if (!server_) {
			on_start("proxy: null transport");
			return;
		}
		server_->set_open_callback([this](int client_type,
						  std::shared_ptr<Connection> c) {
			this->on_open_client(client_type, c);
		});
		server_->set_pack_105_hook([this] {
			return this->pack_105_message(
				true, false, get_dist_version(),
				get_toolset_version());
		});
		server_->set_dispatch_hook([this](const std::string &id,
						  int side,
						  const std::string &plain) {
			this->on_plain_message(id, side, plain);
		});
		int port = port_;
		for (int attempt = 0;; attempt++) {
			try {
				server_->listen(port);
				break;
			} catch (const std::exception &e) {
				// PORT_INC10 == 1: hop +10 and retry.
				if (strategy_ != 1 || attempt >= 11) {
					on_start(std::string("bind failed on ") +
						 std::to_string(port) + ": " +
						 e.what());
					return;
				}
				port += 10;
			}
		}
		port_ = port;
		try {
			server_->run();
		} catch (const std::exception &e) {
			on_start(e.what());
			return;
		}
		on_start("");
	}

	void Stop() override
	{
		if (server_)
			server_->stop();
	}

	void SendMessageToClient(const std::string &msg, int clientType,
				 bool is_encryption = false) override
	{
		if (server_)
			server_->SendMessageToClient(msg, clientType,
						     is_encryption);
	}


protected:
	// App hooks (ObsProxy overrides all of these).
	virtual void on_start(const std::string & /*err_message*/) {}
	virtual void on_open_client(int /*client_type*/,
				    std::shared_ptr<Connection> /*connection*/)
	{
	}
	virtual std::string get_dist_version() { return plugin_.dist_version; }
	virtual std::string get_toolset_version()
	{
		return plugin_.toolset_version;
	}
	virtual std::string pack_105_message(bool /*smartOnline*/,
					     bool /*camOnline*/,
					     const std::string /*tSetVersion*/,
					     const std::string /*pVersion*/)
	{
		return {};
	}

	// Inbound dispatch seam: fires for every decrypted inbound message the
	// framing layer did not fully consume — the cam extension's 101-object
	// blast envelopes ({id,scale,algin,platform,v,type,data}: tip/state
	// traffic, REQUIRED for video feedback) and every direct outer type
	// (360/109/209/108/...). args: transport client id, side, plain JSON
	// string (full outer envelope for the 101-object form). Heartbeats
	// stay internal (client runs no heartbeat timeout; echo not required).
	virtual void on_plain_message(const std::string & /*id*/,
				      int /*side*/,
				      const std::string & /*plain*/)
	{
	}

	std::shared_ptr<socket_type> server_;
	int port_;
	PluginInfo plugin_;
	int strategy_;
};

// Framework server (rs_server.hpp ObsServer derives this).
class Server {
public:
	enum PortStrategy { PORT_STATIC = 0, PORT_INC10 = 1 };

	struct Config {
		std::string app_name;
		int ws_port = 0;
		int wss_port = 0;
		int port_strategy = PORT_INC10;
		PluginInfo plugin;
		// std::string (not path): matches `config.cert_path = cert_dir`
		// at rs_server.cpp:136. See OPEN 7 about the dead #if 0 block.
		std::string cert_path;
	};

	virtual ~Server() = default;

	void initialize(const Config &config);
	void start();
	void shutdown();

	int get_ws_port() const;
	int get_wss_port() const;
	std::shared_ptr<ws::WebServerImpl>
	server(ws::WsocketType type) const;

protected:
	// Overridden by ObsServer to wrap transports in ObsProxy.
	virtual std::shared_ptr<ws::WebServerImpl>
	create_server_impl(ws::WsocketType ws_type);

	std::shared_ptr<ws::CertificateManager> certificate_manager_;

	Config config_;
	std::shared_ptr<ws::WebServerImpl> ws_impl_;
	std::shared_ptr<ws::WebServerImpl> wss_impl_;
};

// Plain-WS SmartCam transport. Default-constructible (rs_server.cpp:192).
class WsServer : public ws::WebServerImpl,
		 public std::enable_shared_from_this<WsServer> {
public:
	struct Connection {
		std::function<void(const std::string &)> send;
		std::string id;
		int client_type = ws::CLIENT_FROM_SMART;
	};

	WsServer();
	~WsServer() override;

	// WebServerImpl.
	ws::WsocketType SocketType() const override;
	void Start() override;
	void Stop() override;
	int port() const override;
	void SendMessageToClient(const std::string &msg, int clientType,
				 bool is_encryption = false) override;

	// Transport primitives used by Proxy and tests.
	void listen(int port); // throws on bind failure
	void run(); // serve in background; returns immediately
	void stop();
	using OpenCallback = std::function<void(
		int, std::shared_ptr<Connection>)>;
	void set_open_callback(OpenCallback cb);

	// Test/diagnostics hooks.
	size_t connection_count() const;
	std::vector<std::string> client_ids(int client_type) const;

	// App hook for the 105 hello's data block: returning a JSON-object
	// string merges it over the replay-proven default ("" = default).
	void set_pack_105_hook(std::function<std::string()> cb);

	// Inbound dispatch hook (deob receiveEvent 2026-10-20): fired with
	// (client id, side, plain JSON) for 101-object blast envelopes and
	// direct outer types the framing layer does not fully consume.
	void set_dispatch_hook(
		std::function<void(const std::string &id, int side,
				   const std::string &plain)>
			cb);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// TLS SmartCam transport. Constructible from a cert-file path plus a
// private-key buffer (rs_server.cpp:222-223) or two C strings
// (rs_server.cpp:248-249, dead #if 0). Stays down (Start reports via
// on_start-style error, serves nothing) when the material is unusable —
// WSS without captured certs is a no-op by design (OPEN 10).
class WssServer : public ws::WebServerImpl,
		  public std::enable_shared_from_this<WssServer> {
public:
	struct Connection {
		std::function<void(const std::string &)> send;
		std::string id;
		int client_type = ws::CLIENT_FROM_SMART;
	};

	WssServer(const std::string &cer_path,
		  const boost::asio::const_buffer &private_key);
	WssServer(const char *cer_path, const char *pem_path);
	~WssServer() override;

	ws::WsocketType SocketType() const override;
	void Start() override;
	void Stop() override;
	int port() const override;
	void SendMessageToClient(const std::string &msg, int clientType,
				 bool is_encryption = false) override;

	void listen(int port);
	void run();
	void stop();
	bool serving() const;
	using OpenCallback = std::function<void(
		int, std::shared_ptr<Connection>)>;
	void set_open_callback(OpenCallback cb);

	// Uniformity with WsServer for Proxy<T>; never fires while down.
	void set_pack_105_hook(std::function<std::string()> cb);

	// Uniform twin: stored for future cert capture; WSS has no message
	// handler yet, so nothing fires (OPEN 10).
	void set_dispatch_hook(
		std::function<void(const std::string &id, int side,
				   const std::string &plain)>
			cb);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace smart
} // namespace lovense
