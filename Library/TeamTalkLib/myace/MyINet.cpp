#include <android/log.h>
#include "MyINet.h"
#include "MyACE.h"

#include <ace/INET_Addr.h>
#include <ace/OS.h>

#if defined(WIN32)
#else
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <map>
#include <vector>

// ۱. تعریف پشتیبانی از OpenSSL و هدر httplib
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

// تابع کمکی برای تفکیک بخش هاست و مسیر از کل آدرس URL
void ParseUrl(const std::string& url, std::string& base_url, std::string& path) {
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        size_t path_start = url.find("/");
        if (path_start == std::string::npos) {
            base_url = "http://" + url;
            path = "/";
        } else {
            base_url = "http://" + url.substr(0, path_start);
            path = url.substr(path_start);
        }
    } else {
        size_t host_start = scheme_end + 3;
        size_t path_start = url.find("/", host_start);
        if (path_start == std::string::npos) {
            base_url = url;
            path = "/";
        } else {
            base_url = url.substr(0, path_start);
            path = url.substr(path_start);
        }
    }
}

// تابع ارسال فایل/داده به صورت POST با استفاده از cpp-httplib
int HttpPostRequest(const ACE_CString& url, const char* data, int len,
                    const std::map<std::string, std::string>& headers,
                    std::string& result, ACE::HTTP::Status::Code* statusCode /*= nullptr*/)
{
    std::string url_str(url.c_str());
    std::string base_url, path;
    ParseUrl(url_str, base_url, path);

    __android_log_print(ANDROID_LOG_INFO, "TT_NET", "httplib POST: Base=%s, Path=%s", base_url.c_str(), path.c_str());

    // ساخت کلاینت (به طور خودکار نوع اتصال http یا https را تشخیص می‌دهد)
    httplib::Client cli(base_url);

    // غیرفعال کردن موقت تایید گواهی برای جلوگیری از باگ عدم لود روت سرتیفیکیت‌ها در اندروید
    cli.enable_server_certificate_verification(false);

    // تنظیم تایم‌اوت‌های استاندارد ۱۵ ثانیه‌ای
    cli.set_connection_timeout(15, 0);
    cli.set_read_timeout(15, 0);
    cli.set_write_timeout(15, 0);

    // آماده‌سازی هدرها برای کتابخانه جدید
    httplib::Headers httplib_headers;
    std::string content_type = "application/octet-stream"; // پیش‌فرض برای دیتای باینری

    for (const auto& h : headers) {
        if (h.first == "Content-Type" || h.first == "content-type") {
            content_type = h.second;
        } else {
            httplib_headers.emplace(h.first, h.second);
        }
    }

    // ارسال درخواست POST
    auto res = cli.Post(path, httplib_headers, data, len, content_type);

    if (res) {
        if (statusCode != nullptr) {
            *statusCode = static_cast<ACE::HTTP::Status::Code>(res->status);
        }
        result = res->body; // پاسخ سرور به طور کامل در این متغیر قرار می‌گیرد
        __android_log_print(ANDROID_LOG_INFO, "TT_NET", "httplib POST success! Status: %d, Size: %d bytes", res->status, (int)result.size());
        
        return (res->status >= 200 && res->status < 300) ? 1 : 0;
    } else {
        auto err = res.error();
        __android_log_print(ANDROID_LOG_ERROR, "TT_NET", "httplib POST failed! Error code: %d", (int)err);
        if (statusCode != nullptr) {
            *statusCode = ACE::HTTP::Status::HTTP_INTERNAL_SERVER_ERROR;
        }
        return -1;
    }
}

// تابع دریافت داده به صورت GET با استفاده از cpp-httplib
int HttpGetRequest(const ACE_CString& url, std::string& result, ACE::HTTP::Status::Code* statusCode /*= nullptr*/)
{
    std::string url_str(url.c_str());
    std::string base_url, path;
    ParseUrl(url_str, base_url, path);

    httplib::Client cli(base_url);
    cli.enable_server_certificate_verification(false);
    cli.set_connection_timeout(15, 0);
    cli.set_read_timeout(15, 0);

    auto res = cli.Get(path);

    if (res) {
        if (statusCode != nullptr) {
            *statusCode = static_cast<ACE::HTTP::Status::Code>(res->status);
        }
        result = res->body;
        return (res->status >= 200 && res->status < 300) ? 1 : 0;
    } else {
        if (statusCode != nullptr) {
            *statusCode = ACE::HTTP::Status::HTTP_INTERNAL_SERVER_ERROR;
        }
        return -1;
    }
}

// پیاده‌سازی متد ارسال فرم داده‌های آدرس‌دهی شده
int HttpPostRequest(const ACE_CString& url, const std::map<std::string,std::string>& unencodedformdata,
                    std::string& result, ACE::HTTP::Status::Code* statusCode/* = nullptr*/)
{
    std::string content;
    auto count = unencodedformdata.size();
    for (const auto& vp : unencodedformdata)
    {
        content += URLEncode(vp.first) + "=" + URLEncode(vp.second);
        if (--count > 0)
            content += "&";
    }
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";
    return HttpPostRequest(url, content.c_str(), content.size(), headers, result, statusCode);
}

std::string URLEncode(const std::string& utf8)
{
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : utf8)
    {
        if ((isalnum(c) != 0) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            escaped << c;
            continue;
        }

        escaped << std::uppercase;
        escaped << '%' << std::setw(2) << int((unsigned char)c);
        escaped << std::nouppercase;
    }

    return escaped.str();
}

std::vector<ACE_INET_Addr> DetermineHostAddress(const ACE_TString& host, uint16_t port)
{
    std::vector<ACE_INET_Addr> result;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    const int addrinfoerror = ACE_OS::getaddrinfo(UnicodeToUtf8(host).c_str(), nullptr, &hints, &res);
    if (addrinfoerror != 0)
    {
        return {};
    }

    for (addrinfo* curr = res; curr != nullptr; curr = curr->ai_next)
    {
        union ip46
        {
            sockaddr_in  in4_;
#if defined (ACE_HAS_IPV6)
            sockaddr_in6 in6_;
#endif
        } addr{};

        ACE_OS::memcpy(&addr, curr->ai_addr, curr->ai_addrlen);
#ifdef ACE_HAS_IPV6
        if (curr->ai_family == AF_INET6)
        {
            addr.in6_.sin6_port = htons(port);
            result.emplace_back(reinterpret_cast<const sockaddr_in*>(&addr.in6_), sizeof(addr.in6_));
        }
        else
#endif
        {
            addr.in4_.sin_port = htons(port);
            result.emplace_back(reinterpret_cast<const sockaddr_in*>(&addr.in4_), sizeof(addr.in4_));
        }
    }

    ACE_OS::freeaddrinfo(res);
    return result;
}

ACE_TString InetAddrToString(const ACE_INET_Addr& addr)
{
    ACE_TCHAR buf[INET6_ADDRSTRLEN+1] = {};
    addr.addr_to_string(buf, INET6_ADDRSTRLEN);
    return buf;
}

int InetAddrFamily(const ACE_TString& addr_str)
{
    char buf[INET6_ADDRSTRLEN];
    if (ACE_OS::inet_pton(AF_INET, UnicodeToUtf8(addr_str).c_str(), buf) > 0)
        return AF_INET;
    if (ACE_OS::inet_pton(AF_INET6, UnicodeToUtf8(addr_str).c_str(), buf) > 0)
        return AF_INET6;
    return -1;
}

ACE_TString INetAddrNetwork(const ACE_TString& ipaddr, uint32_t prefix)
{
    int const af = InetAddrFamily(ipaddr);
    switch (af)
    {
    case AF_INET :
    {
        struct sockaddr_in ipv4addr;
        if (inet_pton(AF_INET, UnicodeToUtf8(ipaddr).c_str(), &(ipv4addr.sin_addr)) <= 0)
            return {};
        prefix = std::min(prefix, uint32_t(32));
        uint32_t const shift = 32-prefix;
        ipv4addr.sin_addr.s_addr = ntohl(ipv4addr.sin_addr.s_addr);
        ipv4addr.sin_addr.s_addr >>= shift;
        ipv4addr.sin_addr.s_addr <<= shift;
        ipv4addr.sin_addr.s_addr = ntohl(ipv4addr.sin_addr.s_addr);
        char strnet[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &(ipv4addr.sin_addr.s_addr), strnet, INET_ADDRSTRLEN) != nullptr)
            return Utf8ToUnicode(strnet);
    }
    case AF_INET6 :
    {
        struct sockaddr_in6 ipv6addr;
        if (inet_pton(AF_INET6, UnicodeToUtf8(ipaddr).c_str(), &(ipv6addr.sin6_addr)) <= 0)
            return {};
        prefix = std::min(prefix, uint32_t(128));
        for (int i = 0; i < 16; ++i)
        {
            uint32_t const bits = std::min(uint32_t(8), prefix);
            ipv6addr.sin6_addr.s6_addr[i] >>= 8 - bits;
            ipv6addr.sin6_addr.s6_addr[i] <<= 8 - bits;
            prefix -= bits;
        }

        char strnet[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &(ipv6addr.sin6_addr), strnet, INET6_ADDRSTRLEN) != nullptr)
            return Utf8ToUnicode(strnet);
    }
    }
    return {};
}