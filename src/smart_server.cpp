// SmartCam WebSocket transport implementation; see README.md for integration.
#include "smart_server.hpp"

#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/config/asio.hpp>
#include <websocketpp/server.hpp>

namespace lovense {
namespace smart {

namespace {

// ---- shared peer logic (transport-agnostic) -------------------------------

int side_from_string(const std::string &s, int fallback)
{
	if (s == "cam")
		return ws::CLIENT_FROM_CAM;
	if (s == "smart")
		return ws::CLIENT_FROM_SMART;
	return fallback;
}

// Side requested by the ?from= query in the WS resource, e.g.
// "/smartcam/ws?from=cam". Anything else (or absent) means smart.
int side_from_resource(const std::string &resource)
{
	auto q = resource.find("from=");
	if (q == std::string::npos)
		return ws::CLIENT_FROM_SMART;
	std::string v = resource.substr(q + 5);
	auto amp = v.find('&');
	if (amp != std::string::npos)
		v.resize(amp);
	return side_from_string(v, ws::CLIENT_FROM_SMART);
}

// Rich 105 data block (grounded 2026-10-20: live CDP capture + scratch replay
// on lap — the cam extension dereferences data.version unguarded at
// background.js:16:1824127, so a bare {to,type} hello is never adopted and
// the extension reports "something went wrong"). Every value below is one the
// replay-accepted hello carried.
static nlohmann::json default_105_data()
{
	nlohmann::json d;
	d["version"] = "3.7.0";
	d["toolSetVersion"] = "1.3.0";
	d["stream_v"] = 1;
	d["smart_object_toy_version"] = 1;
	d["vibeMateTipMotionSupport"] = 1;
	d["video_snapshot_version"] = 1;
	d["smartOnline"] = true;
	d["pcId"] = "lovense-linux";
	d["camName"] = "OBS";
	d["resolutionRatio"] = "1920x1080";
	d["frameRate"] = 30;
	d["fdSupport"] = true;
	d["handSupport"] = true;
	return d;
}

std::string pack_105(const std::string &client_id,
		     const std::string &app_data)
{
	nlohmann::json j;
	j["type"] = "105";
	j["to"] = client_id;
	nlohmann::json d = default_105_data();
	if (!app_data.empty()) {
		try {
			auto extra = nlohmann::json::parse(app_data);
			if (extra.is_object())
				d.merge_patch(extra);
		} catch (...) {
		}
	}
	j["data"] = d;
	return j.dump();
}

// SendMessageToClient routes to the side opposite the supplied client type:
// is_encryption=true encrypts plaintext before sending; delivery goes to
// the OPPOSITE side of clientType (mirrors the 202 relay rule).
template <typename Conn>
std::string pack_101(const std::string &target_id, const std::string &msg)
{
	nlohmann::json body;
	try {
		body = nlohmann::json::parse(msg);
	} catch (...) {
		body = nlohmann::json::object({{"type", "raw"},
					       {"data", msg}});
	}
	nlohmann::json env;
	env["type"] = "101";
	env["to"] = target_id;
	env["data"] = std::move(body);
	return env.dump();
}

} // namespace

// ---- Server ---------------------------------------------------------------

void Server::initialize(const Config &config)
{
	config_ = config;
	certificate_manager_ = std::make_shared<ws::CertificateManager>();
	ws_impl_ = create_server_impl(ws::WsocketType::WS_SOCKET);
	wss_impl_ = create_server_impl(ws::WsocketType::WSS_SOCKET);
}

void Server::start()
{
	for (auto &impl : {ws_impl_, wss_impl_}) {
		if (!impl)
			continue;
		if (auto proxy =
			    std::dynamic_pointer_cast<ProxyBase>(impl))
			proxy->set_port_strategy(config_.port_strategy);
		impl->Start();
	}
}

void Server::shutdown()
{
	for (auto &impl : {ws_impl_, wss_impl_}) {
		if (!impl)
			continue;
		try {
			impl->Stop();
		} catch (...) {
		}
	}
}

int Server::get_ws_port() const
{
	return ws_impl_ ? ws_impl_->port() : 0;
}

int Server::get_wss_port() const
{
	return wss_impl_ ? wss_impl_->port() : 0;
}

std::shared_ptr<ws::WebServerImpl>
Server::server(ws::WsocketType type) const
{
	return type == ws::WsocketType::WS_SOCKET ? ws_impl_ : wss_impl_;
}

std::shared_ptr<ws::WebServerImpl>
Server::create_server_impl(ws::WsocketType /*ws_type*/)
{
	return nullptr; // ObsServer overrides this (rs_server.cpp)
}

// ---- WsServer ---------------------------------------------------------------

struct WsServer::Impl {
	using endpoint_type = websocketpp::server<websocketpp::config::asio>;
	using hdl_type = websocketpp::connection_hdl;

	endpoint_type endpoint;
	std::thread worker;
	std::atomic<bool> running{false};
	std::atomic<unsigned> next_id{1};
	std::atomic<int> bound_port{0};

	mutable std::mutex mutex;
	// id -> connection (holds the send closure, so sends need no endpoint)
	std::map<std::string, std::shared_ptr<WsServer::Connection>> peers;
	// hdl -> id for close/message lookup
	std::function<void(const std::string &, int, const std::string &)>
		dispatch_hook; // inbound app dispatch (see notify_dispatch)

	// Copies the hook under the lock, then fires it unlocked (the app
	// dispatcher may re-enter broadcast paths).
	void notify_dispatch(const std::shared_ptr<Connection> &conn,
			     const std::string &plain)
	{
		std::function<void(const std::string &, int,
				   const std::string &)>
			cb;
		{
			std::lock_guard<std::mutex> lock(mutex);
			cb = dispatch_hook;
		}
		if (!cb)
			return;
		try {
			cb(conn->id, conn->client_type, plain);
		} catch (...) {
		}
	}
	std::map<hdl_type, std::string,
		 std::owner_less<hdl_type>>
		by_hdl; // hdl -> id for close/message lookup

	WsServer::OpenCallback open_cb;
	std::function<std::string()> pack_105_hook;

	static int sender_side(const nlohmann::json &j, int registered)
	{
		if (j.is_object() && j.contains("from") &&
		    j["from"].is_string())
			return side_from_string(j["from"].get<std::string>(),
						registered);
		return registered;
	}

	void broadcast_opposite(const std::string &cipher, int from_side)
	{
		std::vector<std::shared_ptr<WsServer::Connection>> targets;
		{
			std::lock_guard<std::mutex> lock(mutex);
			for (auto &kv : peers) {
				if (kv.second->client_type != from_side)
					targets.push_back(kv.second);
			}
		}
		for (auto &c : targets) {
			try {
				c->send(cipher);
			} catch (...) {
			}
		}
	}

	void on_message_text(const std::string &cipher,
			     const std::shared_ptr<WsServer::Connection> &conn)
	{
		std::string plain = ws::DecryptionAES(cipher);
		if (plain.empty())
			return;
		nlohmann::json j;
		try {
			j = nlohmann::json::parse(plain);
		} catch (...) {
			return;
		}
		if (!j.is_object() || !j.contains("type"))
			return;
		std::string type = j["type"].is_string()
					   ? j["type"].get<std::string>()
					   : std::string();
		if (type == "103")
			return; // heartbeat
		if (type == "101") {
			// Client 101 wrapper — two forms (deob sendWsData,
			// 2026-10-20): data OBJECT {id,scale,algin,platform,
			// v,type,data} carries the state/blast traffic
			// (REQUIRED for video feedback); data STRING carries
			// the pre-encrypted inner heartbeat {type:"103"}.
			auto inner = j.contains("data") ? j["data"]
							: nlohmann::json();
			if (inner.is_object()) {
				// Forward the INNER envelope (header + event),
				// not the client's outer 101 — the overlay
				// lane consumes one 101 wrapper: server
				// pack_101 {to, data:{id,scale,...,type,data}}.
				notify_dispatch(conn, inner.dump());
			} else if (inner.is_string()) {
				try {
					std::string p = ws::DecryptionAES(
						inner.get<std::string>());
					auto ij = p.empty()
							  ? nlohmann::json()
							  : nlohmann::json::parse(
								  p);
					// Heartbeat inner frame: consumed
					// here (the client runs no heartbeat
					// timeout — deob — so no echo).
					if (!p.empty() &&
					    !(ij.is_object() &&
					      ij.contains("type") &&
					      ij["type"].is_string() &&
					      ij["type"].get<
						      std::string>() ==
						      "103"))
						notify_dispatch(conn, p);
				} catch (...) {
				}
			}
			return;
		}
		if (type == "105") {
			// Client declaring its side; re-register + re-send 105.
			if (j.contains("from") && j["from"].is_string()) {
				std::lock_guard<std::mutex> lock(mutex);
				conn->client_type = side_from_string(
					j["from"].get<std::string>(),
					conn->client_type);
			}
			try {
				std::string app_data;
				{
					std::lock_guard<std::mutex> lock(
						mutex);
					if (pack_105_hook)
						app_data = pack_105_hook();
				}
				conn->send(ws::EncryptionAES(
					pack_105(conn->id, app_data)));
			} catch (...) {
			}
			return;
		}
		if (type == "202") {
			// Relay the ORIGINAL envelope intact to the other side.
			broadcast_opposite(cipher,
					   sender_side(j, conn->client_type));
			return;
		}
		// Decrypted inbound types the framing layer does not consume
		// (360/109/209/108/119/120/400/module/stream, plus any future
		// 101-string inner types) go to the app dispatch hook.
		notify_dispatch(conn, plain);
	}
};

WsServer::WsServer() : impl_(new Impl)
{
	impl_->endpoint.init_asio();
	impl_->endpoint.set_reuse_addr(true);
	impl_->endpoint.clear_access_channels(websocketpp::log::alevel::all);
	impl_->endpoint.clear_error_channels(websocketpp::log::elevel::all);

	impl_->endpoint.set_open_handler(
		[this](websocketpp::connection_hdl hdl) {
			int side = ws::CLIENT_FROM_SMART;
			try {
				auto con = impl_->endpoint.get_con_from_hdl(
					hdl);
				side = side_from_resource(
					con->get_resource());
			} catch (...) {
			}
			unsigned n = impl_->next_id++;
			std::string id =
				(side == ws::CLIENT_FROM_CAM ? "cam-"
							     : "smart-") +
				std::to_string(n);

			auto conn = std::make_shared<Connection>();
			conn->id = id;
			conn->client_type = side;
			std::weak_ptr<WsServer::Connection> weak = conn;
			// Capture endpoint by Impl* — safe: stop() joins the
			// worker before Impl is destroyed.
			Impl *self = impl_.get();
			conn->send = [self, hdl](const std::string &payload) {
				self->endpoint.send(
					hdl, payload,
					websocketpp::frame::opcode::text);
			};
			{
				std::lock_guard<std::mutex> lock(impl_->mutex);
				impl_->peers[id] = conn;
				impl_->by_hdl[hdl] = id;
			}
			try {
				std::string app_data;
				{
					std::lock_guard<std::mutex> lock(
						impl_->mutex);
					if (impl_->pack_105_hook)
						app_data =
							impl_->pack_105_hook();
				}
				conn->send(ws::EncryptionAES(
					pack_105(id, app_data)));
			} catch (...) {
			}
			WsServer::OpenCallback cb;
			{
				std::lock_guard<std::mutex> lock(impl_->mutex);
				cb = impl_->open_cb;
			}
			if (cb) {
				try {
					cb(side, conn);
				} catch (...) {
				}
			}
			(void)weak;
		});

	impl_->endpoint.set_close_handler(
		[this](websocketpp::connection_hdl hdl) {
			std::lock_guard<std::mutex> lock(impl_->mutex);
			auto it = impl_->by_hdl.find(hdl);
			if (it == impl_->by_hdl.end())
				return;
			impl_->peers.erase(it->second);
			impl_->by_hdl.erase(it);
		});

	impl_->endpoint.set_message_handler(
		[this](websocketpp::connection_hdl hdl,
		       Impl::endpoint_type::message_ptr msg) {
			if (msg->get_opcode() !=
			    websocketpp::frame::opcode::text)
				return;
			std::shared_ptr<Connection> conn;
			{
				std::lock_guard<std::mutex> lock(impl_->mutex);
				auto it = impl_->by_hdl.find(hdl);
				if (it == impl_->by_hdl.end())
					return;
				auto jt = impl_->peers.find(it->second);
				if (jt == impl_->peers.end())
					return;
				conn = jt->second;
			}
			impl_->on_message_text(msg->get_payload(), conn);
		});
}

WsServer::~WsServer()
{
	stop();
}

ws::WsocketType WsServer::SocketType() const
{
	return ws::WsocketType::WS_SOCKET;
}

void WsServer::listen(int port)
{
	impl_->endpoint.listen(websocketpp::lib::asio::ip::tcp::endpoint(websocketpp::lib::asio::ip::address_v4::loopback(), port));
	impl_->bound_port = port;
}

void WsServer::run()
{
	bool expected = false;
	if (!impl_->running.compare_exchange_strong(expected, true))
		return;
	impl_->endpoint.start_accept();
	impl_->worker = std::thread([this] {
		try {
			impl_->endpoint.run();
		} catch (...) {
		}
	});
}

void WsServer::Start()
{
	run(); // direct use requires listen(port) first; Proxy calls both
}

void WsServer::stop()
{
	try {
		impl_->endpoint.stop_listening();
	} catch (...) {
	}
	try {
		impl_->endpoint.stop();
	} catch (...) {
	}
	if (impl_->worker.joinable())
		impl_->worker.join();
	impl_->running = false;
}

void WsServer::Stop()
{
	stop();
}

int WsServer::port() const
{
	return impl_->bound_port;
}

void WsServer::set_open_callback(OpenCallback cb)
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->open_cb = std::move(cb);
}

void WsServer::set_pack_105_hook(std::function<std::string()> cb)
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->pack_105_hook = std::move(cb);
}

void WsServer::set_dispatch_hook(
	std::function<void(const std::string &id, int side,
			   const std::string &plain)>
		cb)
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->dispatch_hook = std::move(cb);
}

size_t WsServer::connection_count() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->peers.size();
}

std::vector<std::string> WsServer::client_ids(int client_type) const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<std::string> out;
	for (auto &kv : impl_->peers) {
		if (kv.second->client_type == client_type)
			out.push_back(kv.first);
	}
	return out;
}

void WsServer::SendMessageToClient(const std::string &msg, int clientType,
				   bool is_encryption)
{
	// 101 snapshot to every client on the opposite side (OPEN 2).
	// is_encryption=true encrypts the envelope for the wire (OPEN 1).
	std::vector<std::pair<std::shared_ptr<Connection>, std::string>> jobs;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		for (auto &kv : impl_->peers) {
			if (kv.second->client_type == clientType)
				continue; // opposite side only (OPEN 2)
			std::string envelope =
				pack_101<Connection>(kv.first, msg);
			if (is_encryption)
				envelope = ws::EncryptionAES(envelope);
			jobs.emplace_back(kv.second, std::move(envelope));
		}
	}
	for (auto &job : jobs) {
		try {
			job.first->send(job.second);
		} catch (...) {
		}
	}
}

// ---- WssServer ---------------------------------------------------------------
// Constructible and linkable; stays down unless usable cert material is
// present (OPEN 10).

struct WssServer::Impl {
	using endpoint_type = websocketpp::server<websocketpp::config::asio_tls>;

	endpoint_type endpoint;
	std::thread worker;
	std::atomic<bool> running{false};
	std::atomic<int> bound_port{0};
	std::atomic<bool> serving{false};
	mutable std::mutex mutex;
	std::map<std::string, std::shared_ptr<WssServer::Connection>> peers;

	std::string cer_path;
	std::string private_key; // as supplied (format OPEN 10: PEM tried)

	bool have_material() const
	{
		if (!private_key.empty())
			return true;
		std::ifstream f(cer_path);
		return f.good();
	}
};

namespace {

std::shared_ptr<boost::asio::ssl::context>
make_tls_context(const std::string &cer_path, const std::string &key_pem)
{
	using boost::asio::ssl::context;
	auto ctx = std::make_shared<context>(context::tlsv12_server);
	ctx->set_options(context::default_workarounds |
			 context::no_sslv2 | context::no_sslv3 |
			 context::single_dh_use);
	if (!cer_path.empty()) {
		std::ifstream f(cer_path);
		if (f.good())
			ctx->use_certificate_chain_file(cer_path);
		else
			ctx->use_certificate_chain(
				boost::asio::buffer(cer_path.data(),
						    cer_path.size()));
	}
	if (!key_pem.empty())
		ctx->use_private_key(boost::asio::buffer(key_pem.data(),
							 key_pem.size()),
				     context::pem);
	return ctx;
}

} // namespace

WssServer::WssServer(const std::string &cer_path,
		     const boost::asio::const_buffer &private_key)
	: impl_(new Impl)
{
	impl_->cer_path = cer_path;
	if (private_key.size() > 0)
		impl_->private_key.assign(
			(const char *)private_key.data(), private_key.size());
	impl_->endpoint.init_asio();
	impl_->endpoint.clear_access_channels(websocketpp::log::alevel::all);
	impl_->endpoint.clear_error_channels(websocketpp::log::elevel::all);
	Impl *self = impl_.get();
	impl_->endpoint.set_tls_init_handler(
		[self](websocketpp::connection_hdl) {
			return make_tls_context(self->cer_path,
						self->private_key);
		});
}

WssServer::WssServer(const char *cer_path, const char *pem_path)
	: WssServer(cer_path ? std::string(cer_path) : std::string(),
		    boost::asio::const_buffer(
			    pem_path ? pem_path : "",
			    pem_path ? std::char_traits<char>::length(pem_path)
				     : 0))
{
}

WssServer::~WssServer()
{
	stop();
}

ws::WsocketType WssServer::SocketType() const
{
	return ws::WsocketType::WSS_SOCKET;
}

void WssServer::listen(int port)
{
	if (!impl_->have_material())
		throw std::runtime_error("wss disabled: no certificate material");
	impl_->endpoint.listen(websocketpp::lib::asio::ip::tcp::endpoint(websocketpp::lib::asio::ip::address_v4::loopback(), port));
	impl_->bound_port = port;
}

void WssServer::run()
{
	bool expected = false;
	if (!impl_->running.compare_exchange_strong(expected, true))
		return;
	impl_->endpoint.start_accept();
	impl_->serving = true;
	impl_->worker = std::thread([this] {
		try {
			impl_->endpoint.run();
		} catch (...) {
		}
	});
}

void WssServer::Start()
{
	try {
		// Direct use: bind the configured port if listen() was not
		// called explicitly. Without certs this throws -> stays down.
		if (impl_->bound_port == 0)
			throw std::runtime_error(
				"wss disabled: no certificate material");
		run();
	} catch (...) {
	}
}

void WssServer::stop()
{
	try {
		impl_->endpoint.stop_listening();
	} catch (...) {
	}
	try {
		impl_->endpoint.stop();
	} catch (...) {
	}
	if (impl_->worker.joinable())
		impl_->worker.join();
	impl_->running = false;
	impl_->serving = false;
}

void WssServer::Stop()
{
	stop();
}

int WssServer::port() const
{
	return impl_->bound_port;
}

bool WssServer::serving() const
{
	return impl_->serving;
}

void WssServer::set_open_callback(WssServer::OpenCallback /*cb*/)
{
	// Accepted for Proxy<T> uniformity; never fires while down.
}

void WssServer::set_pack_105_hook(std::function<std::string()> /*cb*/)
{
	// Uniformity with WsServer; the hook would apply once WSS serves.
}

void WssServer::set_dispatch_hook(
	std::function<void(const std::string &, int, const std::string &)>
	/*cb*/)
{
	// Uniform twin: WSS has no message handler until the first cert
	// capture (OPEN 10), so nothing can fire yet.
}

void WssServer::SendMessageToClient(const std::string &,
				    int, bool)
{
	// Down without certs: no-op. (Full fan-out mirrors WsServer once
	// serving; add it when the first WSS capture exists.)
}

} // namespace smart
} // namespace lovense
