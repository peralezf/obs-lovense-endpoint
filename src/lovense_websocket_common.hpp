// Clean-room Linux replacement for lovense_websocket_common.hpp.
// Provides the ws:: surface used by lovense-relay-server: WsocketType,
// WebServerImpl, CLIENT_FROM_* tags, GetCertDir, and the 1-arg SmartCam
// AES helpers (fixed key/iv, AES-128-CBC + base64 — same framing as
// lovense::EncryptionAES in lovense_util_helper.cpp).
// This header intentionally exposes only the transport and crypto surface
// needed by the standalone OBS endpoint.
#pragma once

#include <memory>
#include <string>

namespace lovense {
namespace ws {

// Socket-side selector used across rs_server / camera / insta360 handlers.
enum class WsocketType { WS_SOCKET = 0, WSS_SOCKET = 1 };

// Client-type tags threaded through ForwardMessage / SendMessageToClient.
// Values are internal (the wire uses from:"cam"/"smart" strings); see OPEN 3.
inline constexpr int CLIENT_FROM_CAM = 0;
inline constexpr int CLIENT_FROM_SMART = 1;

// Fixed SmartCam wire-compatibility constants.
inline constexpr char SMARTCAM_AES_KEY[] = "3d4ab4b0bb42dd54";
inline constexpr char SMARTCAM_AES_IV[] = "262ef81aafa7a095";

// Abstract server endpoint. smart::Proxy<T> derives this; app code
// (rs_server, camera/insta360 handlers) only ever touches this interface.
class WebServerImpl {
public:
	virtual ~WebServerImpl() = default;

	virtual WsocketType SocketType() const = 0;
	virtual void Start() = 0;
	virtual void Stop() = 0;
	virtual int port() const = 0;

	// Deliver msg to clients. clientType selects the side (compat routes to
	// the OPPOSITE side, mirroring the 202 relay rule; see OPEN 1-2).
	// is_encryption=true encrypts plaintext msg before sending.
	virtual void SendMessageToClient(const std::string &msg, int clientType,
					 bool is_encryption = false) = 0;
};

// Directory holding the WSS certificate material. Empty = WSS stays down.
// No captured cert dir exists yet, so this returns "" by design (OPEN 10).
std::string GetCertDir();

// SmartCam-crypto shorthands (fixed key/iv above).
std::string EncryptionAES(const std::string &plain);
std::string DecryptionAES(const std::string &cipher_b64);
// Explicit key/iv overloads (same semantics as lovense::EncryptionAES).
std::string EncryptionAES(const std::string &plain, const std::string &key,
			  const std::string &iv);
std::string DecryptionAES(const std::string &cipher_b64,
			  const std::string &key, const std::string &iv);

// Minimal stand-in for the proprietary CertificateManager
// (lovense_certificate_manager.hpp, still missing). Both getters return
// false until real cert handling is captured (OPEN 10); the WSS side then
// cleanly stays down while WS serves.
class CertificateManager {
public:
	virtual ~CertificateManager() = default;
	virtual bool get_public_file(std::string &out);
	virtual bool get_private_key(std::string &out);
};

} // namespace ws
} // namespace lovense
