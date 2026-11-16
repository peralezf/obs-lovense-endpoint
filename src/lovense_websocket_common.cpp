// See lovense_websocket_common.hpp. AES via OpenSSL EVP; base64 kept local
// so this TU needs nothing beyond OpenSSL + the C++ standard library.
#include "lovense_websocket_common.hpp"

#include <openssl/evp.h>

namespace lovense {
namespace ws {

namespace {

// Raw ASCII bytes, zero-padded/truncated to 16 (matches the compat
// EncryptionAES framing the bridge RE validated on the SmartCam wire).
void fill16(const std::string &s, unsigned char out[16])
{
	for (int i = 0; i < 16; i++)
		out[i] = i < (int)s.size() ? (unsigned char)s[i] : 0;
}

std::string base64_encode(const unsigned char *in, size_t len)
{
	static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				      "abcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		unsigned n = (unsigned)in[i] << 16;
		size_t chunk = 1;
		if (i + 1 < len) {
			n |= (unsigned)in[i + 1] << 8;
			chunk = 2;
		}
		if (i + 2 < len) {
			n |= in[i + 2];
			chunk = 3;
		}
		out.push_back(alphabet[(n >> 18) & 63]);
		out.push_back(alphabet[(n >> 12) & 63]);
		out.push_back(chunk > 1 ? alphabet[(n >> 6) & 63] : '=');
		out.push_back(chunk > 2 ? alphabet[n & 63] : '=');
	}
	return out;
}

bool base64_decode(const std::string &in, std::string &out)
{
	static signed char rev[256];
	static bool init = false;
	if (!init) {
		for (int i = 0; i < 256; i++)
			rev[i] = -1;
		const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				    "abcdefghijklmnopqrstuvwxyz0123456789+/";
		for (int i = 0; i < 64; i++)
			rev[(unsigned char)alpha[i]] = (signed char)i;
		init = true;
	}
	out.clear();
	if (in.size() % 4 != 0)
		return false;
	for (size_t i = 0; i < in.size(); i += 4) {
		unsigned n = 0;
		int pad = 0;
		for (int k = 0; k < 4; k++) {
			char c = in[i + k];
			if (c == '=') {
				pad++;
				n <<= 6;
			} else {
				signed char v = rev[(unsigned char)c];
				if (v < 0)
					return false;
				n = (n << 6) | (unsigned)v;
			}
		}
		out.push_back((char)((n >> 16) & 0xff));
		if (pad < 2)
			out.push_back((char)((n >> 8) & 0xff));
		if (pad < 1)
			out.push_back((char)(n & 0xff));
	}
	return true;
}

} // namespace

std::string GetCertDir()
{
	return {}; // no captured cert dir yet; WSS stays down (OPEN 10)
}

std::string EncryptionAES(const std::string &plain, const std::string &key,
			  const std::string &iv)
{
	unsigned char k[16], v[16];
	fill16(key, k);
	fill16(iv, v);

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return {};
	std::string out;
	out.resize(plain.size() + 16);
	int len = 0, total = 0;
	bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, k, v) == 1 &&
		  EVP_EncryptUpdate(ctx, (unsigned char *)out.data(), &len,
				    (const unsigned char *)plain.data(),
				    (int)plain.size()) == 1 &&
		  (total = len,
		   EVP_EncryptFinal_ex(ctx, (unsigned char *)out.data() + len,
				       &len) == 1) &&
		  (total += len, true);
	EVP_CIPHER_CTX_free(ctx);
	if (!ok)
		return {};
	out.resize((size_t)total);
	return base64_encode((const unsigned char *)out.data(), out.size());
}

std::string DecryptionAES(const std::string &cipher_b64,
			  const std::string &key, const std::string &iv)
{
	std::string raw;
	if (!base64_decode(cipher_b64, raw) || raw.empty() ||
	    raw.size() % 16 != 0)
		return {};

	unsigned char k[16], v[16];
	fill16(key, k);
	fill16(iv, v);

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return {};
	std::string out;
	out.resize(raw.size());
	int len = 0, total = 0;
	bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, k, v) == 1 &&
		  EVP_DecryptUpdate(ctx, (unsigned char *)out.data(), &len,
				    (const unsigned char *)raw.data(),
				    (int)raw.size()) == 1 &&
		  (total = len,
		   EVP_DecryptFinal_ex(ctx, (unsigned char *)out.data() + len,
				       &len) == 1) &&
		  (total += len, true);
	EVP_CIPHER_CTX_free(ctx);
	if (!ok)
		return {};
	out.resize((size_t)total);
	return out;
}

std::string EncryptionAES(const std::string &plain)
{
	return EncryptionAES(plain, SMARTCAM_AES_KEY, SMARTCAM_AES_IV);
}

std::string DecryptionAES(const std::string &cipher_b64)
{
	return DecryptionAES(cipher_b64, SMARTCAM_AES_KEY, SMARTCAM_AES_IV);
}

bool CertificateManager::get_public_file(std::string &out)
{
	out.clear();
	return false;
}

bool CertificateManager::get_private_key(std::string &out)
{
	out.clear();
	return false;
}

} // namespace ws
} // namespace lovense
