//
// test_http_client.cpp
// ~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2026 Jack (jack dot wgm at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/test/unit_test.hpp>

#include "httpc/httpc.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace httpc_test {
namespace {

using httpc::http_client;
using httpc::http_request;
using httpc::http_response;
using httpc::http_result;
using httpc::verb;

// 发起一次完整请求.
net::awaitable<http_result> perform(http_client& client, const std::string& url,
    const http_request& req)
{
    co_return co_await client.async_perform(url, req);
}

std::string body_of(const http_response& resp)
{
    return beast::buffers_to_string(resp.body().data());
}

// 可移植的临时文件路径 (相对当前工作目录, 测试结束后删除).
std::string temp_file_path(const std::string& name)
{
    return (std::filesystem::current_path() / ("httpc-" + name + ".tmp")).string();
}

// 集成测试夹具: 每个用例拥有一个独立的本地服务器与执行器.
struct server_fixture
{
    test_server server;
    net::io_context ioc;

    http_client make_client()
    {
        return http_client(ioc.get_executor());
    }

    static http_request make_get_request()
    {
        http_request req;
        req.method(verb::get);
        req.version(11);
        return req;
    }
};

BOOST_FIXTURE_TEST_SUITE(client_suite, server_fixture)

BOOST_AUTO_TEST_CASE(default_configuration)
{
    http_client client = make_client();

    BOOST_TEST(client.check_certificate() == false);
    BOOST_TEST(client.max_redirects() == 5);
    BOOST_TEST(client.connect_timeout() == std::chrono::milliseconds::max());
    BOOST_TEST(client.timeout() == std::chrono::milliseconds::max());
    BOOST_TEST(!client.get_transfer_handler());
    BOOST_TEST(!client.get_http_result_handler());
}

BOOST_AUTO_TEST_CASE(configuration_setters)
{
    http_client client = make_client();

    client.user_agent("httpc-test/1.0");
    client.set_sni("sni.example.com");
    client.check_certificate(true);
    BOOST_TEST(client.check_certificate() == true);
    client.check_certificate(false);
    BOOST_TEST(client.check_certificate() == false);

    client.max_redirects(2);
    BOOST_TEST(client.max_redirects() == 2);

    client.connect_timeout(std::chrono::milliseconds(1234));
    BOOST_TEST(client.connect_timeout() == std::chrono::milliseconds(1234));

    client.timeout(std::chrono::milliseconds(4321));
    BOOST_TEST(client.timeout() == std::chrono::milliseconds(4321));

    client.set_transfer_handler([](void*, std::size_t) { return 0; });
    BOOST_TEST(static_cast<bool>(client.get_transfer_handler()));

    client.set_http_result_handler([](const http_response&) {});
    BOOST_TEST(static_cast<bool>(client.get_http_result_handler()));

    BOOST_TEST(static_cast<bool>(client.get_executor()));
}

BOOST_AUTO_TEST_CASE(reject_invalid_url)
{
    http_client client = make_client();
    auto result = run_sync(perform(client, "http://exa mple.com/", make_get_request()));
    BOOST_TEST(!result.has_value());
}

BOOST_AUTO_TEST_CASE(connect_failure_is_reported)
{
    http_client client = make_client();
    client.connect_timeout(std::chrono::milliseconds(5000));
    auto result = run_sync(perform(client, "http://127.0.0.1:1/", make_get_request()));
    BOOST_TEST(!result.has_value());
}

BOOST_AUTO_TEST_CASE(get_request)
{
    server.start(
        [](const http::request<http::string_body>& req)
        {
            BOOST_TEST(req.method() == http::verb::get);
            BOOST_TEST(req.target() == "/hello");
            BOOST_TEST(req[http::field::user_agent] == "httpc/1.0 (Boost.Beast)");
            BOOST_TEST(req.count(http::field::host) == 1u);

            http::response<http::string_body> res {http::status::ok, req.version()};
            res.body() = "hello world";
            return res;
        });

    http_client client = make_client();
    auto result = run_sync(perform(client, server.url("/hello"), make_get_request()));

    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->result_int() == 200u);
    BOOST_TEST(body_of(*result) == "hello world");
}

BOOST_AUTO_TEST_CASE(get_with_custom_headers_and_query)
{
    server.start(
        [](const http::request<http::string_body>& req)
        {
            BOOST_TEST(req.target() == "/search?q=boost+beast&page=2");
            BOOST_TEST(req[http::field::accept] == "application/json");
            BOOST_TEST(req["x-request-id"] == "42");

            http::response<http::string_body> res {http::status::ok, req.version()};
            res.body() = "custom";
            return res;
        });

    http_client client = make_client();
    client.user_agent("my-agent");

    http_request req = make_get_request();
    req.set(http::field::accept, "application/json");
    req.set("x-request-id", "42");

    auto result =
        run_sync(perform(client, server.url("/search?q=boost+beast&page=2"), req));

    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->result_int() == 200u);
}

BOOST_AUTO_TEST_CASE(not_found_response)
{
    server.start(
        [](const http::request<http::string_body>& req)
        {
            http::response<http::string_body> res {http::status::not_found, req.version()};
            res.body() = "missing";
            return res;
        });

    http_client client = make_client();
    auto result = run_sync(perform(client, server.url("/nothere"), make_get_request()));

    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->result_int() == 404u);
    BOOST_TEST(body_of(*result) == "missing");
}

BOOST_AUTO_TEST_CASE(follows_redirect)
{
    server.start(
        [this](const http::request<http::string_body>& req)
        {
            if (req.target() == "/redirect")
            {
                http::response<http::string_body> res {http::status::found, req.version()};
                res.set(http::field::location, server.url("/final"));
                return res;
            }

            http::response<http::string_body> res {http::status::ok, req.version()};
            res.body() = "final-body";
            return res;
        });

    http_client client = make_client();
    auto result = run_sync(perform(client, server.url("/redirect"), make_get_request()));

    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->result_int() == 200u);
    BOOST_TEST(body_of(*result) == "final-body");
}

BOOST_AUTO_TEST_CASE(redirect_disabled_returns_original_response)
{
    server.start(
        [this](const http::request<http::string_body>& req)
        {
            http::response<http::string_body> res {http::status::found, req.version()};
            res.set(http::field::location, server.url("/final"));
            return res;
        });

    http_client client = make_client();
    client.max_redirects(0);

    auto result = run_sync(perform(client, server.url("/redirect"), make_get_request()));

    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->result_int() == 302u);
    BOOST_TEST((*result)[http::field::location] == server.url("/final"));
}

BOOST_AUTO_TEST_CASE(result_handler_reports_every_response)
{
    server.start(
        [this](const http::request<http::string_body>& req)
        {
            http::response<http::string_body> res;
            if (req.target() == "/redirect")
            {
                res = {http::status::found, req.version()};
                res.set(http::field::location, server.url("/final"));
            }
            else
            {
                res = {http::status::ok, req.version()};
                res.body() = "final-body";
            }
            return res;
        });

    http_client client = make_client();
    auto statuses = std::make_shared<std::vector<int>>();
    client.set_http_result_handler(
        [statuses](const http_response& resp)
        {
            statuses->push_back(resp.result_int());
        });

    auto result = run_sync(perform(client, server.url("/redirect"), make_get_request()));

    BOOST_REQUIRE(result.has_value());
    BOOST_REQUIRE(statuses->size() == 2);
    BOOST_TEST((*statuses)[0] == 302);
    BOOST_TEST((*statuses)[1] == 200);
}

BOOST_AUTO_TEST_CASE(download_to_file_and_transfer_handler)
{
    const std::string payload(1024, 'x');

    server.start(
        [this, payload](const http::request<http::string_body>& req)
        {
            http::response<http::string_body> res;
            if (req.target() == "/redirect")
            {
                res = {http::status::found, req.version()};
                res.set(http::field::location, server.url("/file"));
            }
            else
            {
                res = {http::status::ok, req.version()};
                res.body() = payload;
            }
            return res;
        });

    http_client client = make_client();
    const std::string path = temp_file_path("test-download");
    std::remove(path.c_str());
    client.set_download_file(path);

    auto transferred = std::make_shared<std::string>();
    client.set_transfer_handler(
        [transferred](void* data, std::size_t size) -> int
        {
            transferred->append(static_cast<const char*>(data), size);
            return 0;
        });

    auto result = run_sync(perform(client, server.url("/redirect"), make_get_request()));

    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->result_int() == 200u);
    BOOST_TEST(*transferred == payload);

    std::string file_content;
    {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        BOOST_REQUIRE(file != nullptr);
        char buffer[256];
        std::size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
            file_content.append(buffer, n);
        std::fclose(file);
    }
    std::remove(path.c_str());

    BOOST_TEST(file_content == payload);
}

BOOST_AUTO_TEST_CASE(upload_file)
{
    const std::string payload = "upload-file-payload";

    server.start(
        [payload](const http::request<http::string_body>& req)
        {
            BOOST_TEST(req.method() == http::verb::put);
            BOOST_TEST(req.target() == "/upload");
            BOOST_TEST(req.body() == payload);
            BOOST_TEST(req[http::field::authorization] == "Bearer token");

            http::response<http::string_body> res {http::status::ok, req.version()};
            res.body() = "uploaded";
            return res;
        });

    const std::string path = temp_file_path("test-upload");
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        BOOST_REQUIRE(file != nullptr);
        std::fwrite(payload.data(), 1, payload.size(), file);
        std::fclose(file);
    }

    http_client client = make_client();
    http_request req;
    req.method(verb::put);
    req.set(http::field::authorization, "Bearer token");

    auto result = run_sync(client.async_upload_file(server.url("/upload"), path, req));
    std::remove(path.c_str());

    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->result_int() == 200u);
    BOOST_TEST(body_of(*result) == "uploaded");
}

BOOST_AUTO_TEST_CASE(upload_missing_file)
{
    server.start(
        [](const http::request<http::string_body>& req)
        {
            http::response<http::string_body> res {http::status::ok, req.version()};
            res.body() = "never";
            return res;
        });

    http_client client = make_client();
    auto result = run_sync(
        client.async_upload_file(server.url("/upload"), temp_file_path("missing-file"), {}));

    BOOST_TEST(!result.has_value());
}

BOOST_AUTO_TEST_CASE(upload_stream)
{
    const std::string payload(4096, 'z');
    auto offset = std::make_shared<std::size_t>(0);

    server.start(
        [payload](const http::request<http::string_body>& req)
        {
            BOOST_TEST(req.method() == http::verb::post);
            BOOST_TEST(req.target() == "/stream");
            BOOST_TEST(req.body() == payload);

            http::response<http::string_body> res {http::status::ok, req.version()};
            res.body() = "streamed:" + std::to_string(payload.size());
            return res;
        });

    http_client client = make_client();
    client.set_transfer_handler(
        [payload, offset](void* data, std::size_t size) -> int
        {
            // 作为上传数据源: 数据发送完毕后返回 0 表示结束.
            if (*offset >= payload.size())
                return 0;

            const std::size_t n = (std::min)(size, payload.size() - *offset);
            std::memcpy(data, payload.data() + *offset, n);
            *offset += n;
            return static_cast<int>(n);
        });

    http_request req;
    req.method(verb::post);

    auto result = run_sync(client.async_upload_stream(server.url("/stream"), req));

    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->result_int() == 200u);
}

BOOST_AUTO_TEST_CASE(upload_stream_receives_response_body)
{
    const std::string payload(2048, 'q');
    auto offset = std::make_shared<std::size_t>(0);
    auto received = std::make_shared<std::string>();

    server.start(
        [](const http::request<http::string_body>& req)
        {
            http::response<http::string_body> res {http::status::ok, req.version()};
            res.body() = "streamed:" + std::to_string(req.body().size());
            return res;
        });

    http_client client = make_client();
    client.set_transfer_handler(
        [payload, offset, received](void* data, std::size_t size) -> int
        {
            if (*offset < payload.size())
            {
                const std::size_t n = (std::min)(size, payload.size() - *offset);
                std::memcpy(data, payload.data() + *offset, n);
                *offset += n;
                return static_cast<int>(n);
            }

            // 上传结束后, 回调用于消费响应体; 此时不应再写入缓冲区.
            received->assign(static_cast<const char*>(data), size);
            return 0;
        });

    http_request req;
    req.method(verb::post);

    auto result = run_sync(client.async_upload_stream(server.url("/stream"), req));

    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->result_int() == 200u);
    BOOST_TEST(*received == "streamed:2048");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace httpc_test
