/*
 * Copyright (C) 2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <src/platform/backends/lxd/lxd_vm_image_vault.h>

#include "mock_local_socket_reply.h"
#include "mock_lxd_server_responses.h"
#include "mock_network_access_manager.h"
#include "tests/extra_assertions.h"
#include "tests/mock_image_host.h"
#include "tests/mock_logger.h"

#include <multipass/exceptions/aborted_download_exception.h>
#include <multipass/format.h>
#include <multipass/vm_image.h>

#include <QUrl>

#include <vector>

#include <gmock/gmock.h>

namespace mp = multipass;
namespace mpl = multipass::logging;
namespace mpt = multipass::test;

using namespace testing;

namespace
{
struct LXDImageVault : public Test
{
    LXDImageVault() : mock_network_access_manager{std::make_unique<NiceMock<mpt::MockNetworkAccessManager>>()}
    {
        hosts.push_back(&host);
        mpl::set_logger(logger);

        ON_CALL(host, info_for_full_hash(_)).WillByDefault([this](auto...) { return host.mock_image_info; });

        EXPECT_CALL(*logger, log(Matcher<multipass::logging::Level>(_), Matcher<multipass::logging::CString>(_),
                                 Matcher<multipass::logging::CString>(_)))
            .WillRepeatedly(Return());
    }

    std::shared_ptr<NiceMock<mpt::MockLogger>> logger = std::make_shared<NiceMock<mpt::MockLogger>>();
    std::unique_ptr<NiceMock<mpt::MockNetworkAccessManager>> mock_network_access_manager;
    std::vector<mp::VMImageHost*> hosts;
    NiceMock<mpt::MockImageHost> host;
    QUrl base_url{"unix:///foo@1.0"};
    mp::ProgressMonitor stub_monitor{[](int, int) { return true; }};
    mp::VMImageVault::PrepareAction stub_prepare{
        [](const mp::VMImage& source_image) -> mp::VMImage { return source_image; }};
    std::string instance_name{"pied-piper-valley"};
    mp::Query default_query{instance_name, "xenial", false, "", mp::Query::Type::Alias};
};
} // namespace

TEST_F(LXDImageVault, instance_exists_fetch_returns_expected_image_info)
{
    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _)).WillByDefault([](auto, auto request, auto) {
        auto op = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
        auto url = request.url().toString();

        if (op == "GET")
        {
            if (url.contains("1.0/virtual-machines/pied-piper-valley"))
            {
                return new mpt::MockLocalSocketReply(mpt::vm_info_data);
            }
        }

        return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
    });

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    mp::VMImage image;
    EXPECT_NO_THROW(image =
                        image_vault.fetch_image(mp::FetchType::ImageOnly, default_query, stub_prepare, stub_monitor));

    EXPECT_EQ(image.id, mpt::default_id);
    EXPECT_EQ(image.stream_location, mpt::default_stream_location);
    EXPECT_EQ(image.original_release, "18.04 LTS");
    EXPECT_EQ(image.release_date, mpt::default_version);
}

TEST_F(LXDImageVault, returns_expected_info_with_valid_remote)
{
    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _)).WillByDefault([](auto, auto request, auto) {
        auto op = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
        auto url = request.url().toString();

        if (op == "GET")
        {
            if (url.contains("1.0/images/e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"))
            {
                return new mpt::MockLocalSocketReply(mpt::image_info_data);
            }
        }

        return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
    });

    mp::Query query{"", "bionic", false, "release", mp::Query::Type::Alias};

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    mp::VMImage image;
    EXPECT_NO_THROW(image = image_vault.fetch_image(mp::FetchType::ImageOnly, query, stub_prepare, stub_monitor));

    EXPECT_EQ(image.id, mpt::default_id);
    EXPECT_EQ(image.stream_location, mpt::default_stream_location);
    EXPECT_EQ(image.original_release, "18.04 LTS");
    EXPECT_EQ(image.release_date, mpt::default_version);
}

TEST_F(LXDImageVault, throws_with_invalid_alias)
{
    ON_CALL(host, info_for(_)).WillByDefault([this](auto query) -> mp::optional<mp::VMImageInfo> {
        if (query.release != "bionic")
        {
            return mp::nullopt;
        }

        return host.mock_image_info;
    });

    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _)).WillByDefault([](auto...) {
        return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
    });

    const std::string alias{"xenial"};
    mp::Query query{"", alias, false, "release", mp::Query::Type::Alias};

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    MP_EXPECT_THROW_THAT(
        image_vault.fetch_image(mp::FetchType::ImageOnly, query, stub_prepare, stub_monitor), std::runtime_error,
        Property(&std::runtime_error::what, StrEq(fmt::format("Unable to find an image matching \"{}\"", alias))));
}

TEST_F(LXDImageVault, throws_with_non_alias_queries)
{
    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _)).WillByDefault([](auto...) {
        return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
    });

    mp::Query query{"", "", false, "", mp::Query::Type::HttpDownload};

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    MP_EXPECT_THROW_THAT(image_vault.fetch_image(mp::FetchType::ImageOnly, query, stub_prepare, stub_monitor),
                         std::runtime_error,
                         Property(&std::runtime_error::what, StrEq("http and file based images are not supported")));
}

TEST_F(LXDImageVault, throws_with_invalid_remote)
{
    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _)).WillByDefault([](auto...) {
        return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
    });

    const std::string remote{"bar"};
    mp::Query query{"", "foo", false, remote, mp::Query::Type::Alias};

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    MP_EXPECT_THROW_THAT(image_vault.fetch_image(mp::FetchType::ImageOnly, query, stub_prepare, stub_monitor),
                         std::runtime_error,
                         Property(&std::runtime_error::what, StrEq(fmt::format("Remote \"{}\" is unknown.", remote))));
}

TEST_F(LXDImageVault, does_not_download_if_image_exists)
{
    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _)).WillByDefault([](auto, auto request, auto) {
        auto op = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
        auto url = request.url().toString();

        if (op == "GET")
        {
            if (url.contains("1.0/images/e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"))
            {
                return new mpt::MockLocalSocketReply(mpt::image_info_data);
            }
        }
        else if (op == "POST" && url.contains("1.0/images"))
        {
            // It should not try to request an image download
            ADD_FAILURE();
        }

        return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
    });

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    EXPECT_NO_THROW(image_vault.fetch_image(mp::FetchType::ImageOnly, default_query, stub_prepare, stub_monitor));
}

TEST_F(LXDImageVault, requests_download_if_image_does_not_exist)
{
    bool download_requested{false};

    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _))
        .WillByDefault([&download_requested](auto, auto request, auto) {
            auto op = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
            auto url = request.url().toString();

            if (op == "POST" && url.contains("1.0/images"))
            {
                download_requested = true;
                return new mpt::MockLocalSocketReply(mpt::image_download_task_data);
            }

            return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
        });

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    EXPECT_NO_THROW(image_vault.fetch_image(mp::FetchType::ImageOnly, default_query, stub_prepare, stub_monitor));
    EXPECT_TRUE(download_requested);
}

TEST_F(LXDImageVault, download_deletes_and_throws_on_cancel)
{
    bool delete_requested{false};

    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _))
        .WillByDefault([&delete_requested](auto, auto request, auto) {
            auto op = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
            auto url = request.url().toString();

            if (op == "POST" && url.contains("1.0/images"))
            {
                return new mpt::MockLocalSocketReply(mpt::image_download_task_data);
            }
            else if (op == "GET" && url.contains("1.0/operations/0a19a412-03d0-4118-bee8-a3095f06d4da"))
            {
                return new mpt::MockLocalSocketReply(mpt::image_downloading_task_data);
            }
            else if (op == "DELETE" && url.contains("1.0/operations/0a19a412-03d0-4118-bee8-a3095f06d4da"))
            {
                delete_requested = true;
                return new mpt::MockLocalSocketReply(mpt::post_no_error_data);
            }

            return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
        });

    mp::ProgressMonitor monitor{[](auto, auto progress) {
        EXPECT_EQ(progress, 25);

        return false;
    }};

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    EXPECT_THROW(image_vault.fetch_image(mp::FetchType::ImageOnly, default_query, stub_prepare, monitor),
                 mp::AbortedDownloadException);

    EXPECT_TRUE(delete_requested);
}

TEST_F(LXDImageVault, percent_complete_returns_negative_on_metadata_download)
{
    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _)).WillByDefault([](auto, auto request, auto) {
        auto op = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
        auto url = request.url().toString();

        if (op == "POST" && url.contains("1.0/images"))
        {
            return new mpt::MockLocalSocketReply(mpt::image_download_task_data);
        }
        else if (op == "GET" && url.contains("1.0/operations/0a19a412-03d0-4118-bee8-a3095f06d4da"))
        {
            return new mpt::MockLocalSocketReply(mpt::metadata_downloading_task_data);
        }
        else if (op == "DELETE" && url.contains("1.0/operations/0a19a412-03d0-4118-bee8-a3095f06d4da"))
        {
            return new mpt::MockLocalSocketReply(mpt::post_no_error_data);
        }

        return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
    });

    mp::ProgressMonitor monitor{[](auto, auto progress) {
        EXPECT_EQ(progress, -1);

        return false;
    }};

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    EXPECT_THROW(image_vault.fetch_image(mp::FetchType::ImageOnly, default_query, stub_prepare, monitor),
                 mp::AbortedDownloadException);
}

TEST_F(LXDImageVault, delete_requested_on_instance_remove)
{
    bool delete_requested{false};

    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _))
        .WillByDefault([&delete_requested](auto, auto request, auto) {
            auto op = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
            auto url = request.url().toString();

            if (op == "DELETE" && url.contains("1.0/virtual-machines/pied-piper-valley"))
            {
                delete_requested = true;
                return new mpt::MockLocalSocketReply(mpt::post_no_error_data);
            }

            return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
        });

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    EXPECT_NO_THROW(image_vault.remove(instance_name));
    EXPECT_TRUE(delete_requested);
}

TEST_F(LXDImageVault, logs_warning_when_removing_nonexistent_instance)
{
    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _)).WillByDefault([](auto, auto request, auto) {
        auto op = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
        auto url = request.url().toString();

        if (op == "DELETE" && url.contains("1.0/virtual-machines/pied-piper-valley"))
        {
            return new mpt::MockLocalSocketReply(mpt::post_no_error_data);
        }

        return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
    });

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    const std::string name{"foo"};
    EXPECT_CALL(*logger, log(Eq(mpl::Level::warning), mpt::MockLogger::make_cstring_matcher(StrEq("lxd image vault")),
                             mpt::MockLogger::make_cstring_matcher(
                                 StrEq(fmt::format("Instance \'{}\' does not exist: not removing", name)))))
        .Times(1);
    EXPECT_NO_THROW(image_vault.remove(name));
}

TEST_F(LXDImageVault, has_record_for_returns_expected_values)
{
    ON_CALL(*mock_network_access_manager.get(), createRequest(_, _, _)).WillByDefault([](auto, auto request, auto) {
        auto op = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
        auto url = request.url().toString();

        if (op == "GET")
        {
            if (url.contains("1.0/virtual-machines/pied-piper-valley"))
            {
                return new mpt::MockLocalSocketReply(mpt::vm_info_data);
            }
        }

        return new mpt::MockLocalSocketReply(mpt::not_found_data, QNetworkReply::ContentNotFoundError);
    });

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    EXPECT_TRUE(image_vault.has_record_for(instance_name));
    EXPECT_FALSE(image_vault.has_record_for("foo"));
}

TEST_F(LXDImageVault, unimplemented_functions_log_trace_message)
{
    EXPECT_CALL(*logger, log(Eq(mpl::Level::trace), mpt::MockLogger::make_cstring_matcher(StrEq("lxd image vault")),
                             mpt::MockLogger::make_cstring_matcher(StrEq("Pruning expired images not implemented"))))
        .Times(1);

    EXPECT_CALL(*logger, log(Eq(mpl::Level::trace), mpt::MockLogger::make_cstring_matcher(StrEq("lxd image vault")),
                             mpt::MockLogger::make_cstring_matcher(StrEq("Updating images not implemented"))))
        .Times(1);

    mp::LXDVMImageVault image_vault{hosts, mock_network_access_manager.get(), base_url};

    image_vault.prune_expired_images();

    image_vault.update_images(mp::FetchType::ImageOnly, stub_prepare, stub_monitor);
}
