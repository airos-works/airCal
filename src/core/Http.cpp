#include "Http.h"

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <unistd.h>

namespace aircal {

std::string
UrlEncode(const std::string& value)
{
	static const char kHex[] = "0123456789ABCDEF";
	std::string result;
	for (unsigned char c : value) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
			|| c == '~')
			result += (char)c;
		else {
			result += '%';
			result += kHex[c >> 4];
			result += kHex[c & 15];
		}
	}
	return result;
}


std::string
UrlDecode(const std::string& text)
{
	auto digit = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	std::string out;
	for (size_t i = 0; i < text.size(); i++) {
		char c = text[i];
		if (c == '%' && i + 2 < text.size() && digit(text[i + 1]) >= 0
			&& digit(text[i + 2]) >= 0) {
			out += (char)(digit(text[i + 1]) * 16 + digit(text[i + 2]));
			i += 2;
		} else if (c == '+')
			out += ' ';
		else
			out += c;
	}
	return out;
}


std::string
FormEncode(const std::vector<std::pair<std::string, std::string>>& fields)
{
	std::string result;
	for (auto& field : fields) {
		if (!result.empty())
			result += '&';
		result += UrlEncode(field.first) + "=" + UrlEncode(field.second);
	}
	return result;
}


std::map<std::string, std::string>
ParseQuery(const std::string& query)
{
	std::map<std::string, std::string> result;
	size_t start = 0;
	while (start <= query.size()) {
		size_t end = query.find('&', start);
		if (end == std::string::npos)
			end = query.size();
		std::string pair = query.substr(start, end - start);
		if (!pair.empty()) {
			size_t equals = pair.find('=');
			if (equals == std::string::npos)
				result[UrlDecode(pair)] = "";
			else {
				result[UrlDecode(pair.substr(0, equals))]
					= UrlDecode(pair.substr(equals + 1));
			}
		}
		start = end + 1;
	}
	return result;
}


std::string
UrlOrigin(const std::string& url)
{
	size_t scheme = url.find("://");
	if (scheme == std::string::npos)
		return "";
	size_t path = url.find('/', scheme + 3);
	return path == std::string::npos ? url : url.substr(0, path);
}


std::string
UrlPath(const std::string& url)
{
	std::string origin = UrlOrigin(url);
	std::string path = url.substr(origin.size());
	return path.empty() ? "/" : path;
}


std::string
ResolveUrl(const std::string& base, const std::string& reference)
{
	if (reference.empty())
		return base;
	if (reference.find("://") != std::string::npos)
		return reference;
	std::string origin = UrlOrigin(base);
	if (reference.compare(0, 2, "//") == 0) {
		size_t scheme = base.find("://");
		return base.substr(0, scheme + 1) + reference;
	}
	if (reference[0] == '/')
		return origin + reference;
	std::string path = UrlPath(base);
	size_t query = path.find('?');
	if (query != std::string::npos)
		path = path.substr(0, query);
	size_t slash = path.rfind('/');
	path = slash == std::string::npos ? "/" : path.substr(0, slash + 1);
	return origin + path + reference;
}


std::string
Base64Encode(const std::string& data)
{
	static const char kAlphabet[]
		= "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	unsigned int bits = 0;
	int count = 0;
	for (unsigned char c : data) {
		bits = (bits << 8) | c;
		count += 8;
		while (count >= 6) {
			count -= 6;
			result += kAlphabet[(bits >> count) & 63];
		}
	}
	if (count > 0)
		result += kAlphabet[(bits << (6 - count)) & 63];
	while (result.size() % 4 != 0)
		result += '=';
	return result;
}


std::string
Base64UrlEncode(const std::string& data)
{
	std::string result = Base64Encode(data);
	while (!result.empty() && result.back() == '=')
		result.pop_back();
	for (char& c : result) {
		if (c == '+')
			c = '-';
		else if (c == '/')
			c = '_';
	}
	return result;
}


// --- SHA-256 (FIPS 180-4), for PKCE -----------------------------------------------

namespace {

const uint32_t kK[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};


inline uint32_t
Rotate(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

}	// namespace


std::string
Sha256(const std::string& data)
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	std::string message = data;
	uint64_t bitLength = (uint64_t)data.size() * 8;
	message += (char)0x80;
	while (message.size() % 64 != 56)
		message += (char)0;
	for (int i = 7; i >= 0; i--)
		message += (char)((bitLength >> (i * 8)) & 0xff);

	for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
		uint32_t w[64];
		for (int i = 0; i < 16; i++) {
			const unsigned char* p
				= (const unsigned char*)message.data() + chunk + i * 4;
			w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
				| ((uint32_t)p[2] << 8) | p[3];
		}
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18)
				^ (w[i - 15] >> 3);
			uint32_t s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19)
				^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5],
			g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t S1 = Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
			uint32_t ch = (e & f) ^ (~e & g);
			uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
			uint32_t S0 = Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
			uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = S0 + maj;
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
		h[5] += f;
		h[6] += g;
		h[7] += hh;
	}
	std::string digest;
	for (int i = 0; i < 8; i++) {
		digest += (char)(h[i] >> 24);
		digest += (char)(h[i] >> 16);
		digest += (char)(h[i] >> 8);
		digest += (char)h[i];
	}
	return digest;
}


std::string
RandomBytes(size_t count)
{
	std::string result(count, '\0');
	int fd = open("/dev/urandom", O_RDONLY);
	size_t got = 0;
	if (fd >= 0) {
		while (got < count) {
			ssize_t n = read(fd, &result[got], count - got);
			if (n <= 0)
				break;
			got += n;
		}
		close(fd);
	}
	if (got < count) {
		std::random_device device;
		for (size_t i = got; i < count; i++)
			result[i] = (char)device();
	}
	return result;
}

}	// namespace aircal
