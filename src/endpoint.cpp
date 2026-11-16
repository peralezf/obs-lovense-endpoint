#include <obs-module.h>

#include "smart_server.hpp"

#include <memory>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-lovense-endpoint", "en-US")
OBS_MODULE_AUTHOR("Farida")

namespace {
using namespace lovense;

class LocalProxy final : public smart::Proxy<smart::WsServer> {
public:
	using Transport = smart::WsServer;
	using Connection = Transport::Connection;
	using Base = smart::Proxy<Transport>;

	LocalProxy(std::shared_ptr<Transport> transport, int port,
		   smart::PluginInfo plugin)
		: Base(std::move(transport), port, std::move(plugin))
	{
	}

protected:
	void on_start(const std::string &error) override
	{
		if (!error.empty()) {
			blog(LOG_ERROR, "[Lovense endpoint] start failed: %s",
			     error.c_str());
			return;
		}
		blog(LOG_INFO,
		     "[Lovense endpoint] listening on 127.0.0.1:%d/smartcam/ws",
		     port());
	}

	void on_open_client(int client_type,
			    std::shared_ptr<Connection> /*connection*/) override
	{
		const char *side = client_type == ws::CLIENT_FROM_CAM
					   ? "cam extension"
					   : "video-feedback renderer";
		blog(LOG_INFO, "[Lovense endpoint] %s connected", side);
	}

	void on_plain_message(const std::string & /*client_id*/,
			      int client_type,
			      const std::string &plain) override
	{
		// The Cam Extension sends encrypted 101 state envelopes. Forward
		// their decoded inner envelope to the native renderer lane; the
		// SmartCam transport re-wraps and encrypts it for that client.
		if (client_type == ws::CLIENT_FROM_CAM)
			SendMessageToClient(plain, ws::CLIENT_FROM_CAM, true);
	}
};

class EndpointServer final : public smart::Server {
protected:
	std::shared_ptr<ws::WebServerImpl>
	create_server_impl(ws::WsocketType type) override
	{
		if (type != ws::WsocketType::WS_SOCKET)
			return {};

		auto transport = std::make_shared<smart::WsServer>();
		auto proxy = std::make_shared<LocalProxy>(
			transport, config_.ws_port, config_.plugin);
		proxy->set_port_strategy(config_.port_strategy);
		return proxy;
	}
};

std::unique_ptr<EndpointServer> g_endpoint;
} // namespace

bool obs_module_load(void)
{
	try {
		auto endpoint = std::make_unique<EndpointServer>();
		smart::Server::Config config;
		config.app_name = "OBS-local-lovense-endpoint";
		config.ws_port = 11000;
		config.wss_port = 0;
		config.port_strategy = smart::Server::PORT_STATIC;
		config.plugin.dist_version = "3.7.0";
		config.plugin.toolset_version = "1.3.0";

		endpoint->initialize(config);
		endpoint->start();
		g_endpoint = std::move(endpoint);
		return true;
	} catch (const std::exception &error) {
		blog(LOG_ERROR, "[Lovense endpoint] module startup failed: %s",
		     error.what());
		return false;
	}
}

void obs_module_unload(void)
{
	if (g_endpoint) {
		g_endpoint->shutdown();
		g_endpoint.reset();
		blog(LOG_INFO, "[Lovense endpoint] stopped");
	}
}
