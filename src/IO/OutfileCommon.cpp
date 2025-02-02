#include <Common/config.h>

#include <IO/OutfileCommon.h>
#include <IO/WriteBuffer.h>

#include <IO/CompressionMethod.h>
#include <IO/HTTPSender.h>
#include <IO/OSSCommon.h>
#include <IO/VETosCommon.h>
#include <IO/WriteBufferFromFile.h>
#include <Interpreters/Context.h>
#include <ServiceDiscovery/IServiceDiscovery.h>
#include <ServiceDiscovery/ServiceDiscoveryConsul.h>
#include <ServiceDiscovery/ServiceDiscoveryFactory.h>
#include <Storages/HDFS/WriteBufferFromHDFS.h>
#include <Common/config.h>

#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTQueryWithOutput.h>

#include <string>

#if USE_AWS_S3
#    include <IO/S3Common.h>
#    include <IO/WriteBufferFromS3.h>
#endif

namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_OUTPUT_PATH;
    extern const int INTO_OUTFILE_NOT_ALLOWED;
    extern const int BAD_ARGUMENTS;
    extern const int NETWORK_ERROR;
}

OutfileTarget::OutfileTarget(std::string uri_, std::string format_, std::string compression_method_str_, int compression_level_)
    : uri(uri_), format(format_), compression_method_str(compression_method_str_), compression_level(compression_level_)
{
}

OutfileTarget::OutfileTarget(const OutfileTarget & outfile_target_)
    : uri(outfile_target_.uri)
    , format(outfile_target_.format)
    , compression_method_str(outfile_target_.compression_method_str)
    , compression_level(outfile_target_.compression_level)
{
}

std::shared_ptr<WriteBuffer> OutfileTarget::getOutfileBuffer(const ContextPtr & context, bool allow_into_local)
{
    const Poco::URI out_uri(uri);
    const String & scheme = out_uri.getScheme();
    CompressionMethod compression_method = chooseCompressionMethod(uri, compression_method_str);

    if (scheme.empty())
    {
        if (!allow_into_local)
            throw Exception("INTO OUTFILE is not allowed", ErrorCodes::INTO_OUTFILE_NOT_ALLOWED);

        out_buf_raw = std::make_unique<WriteBufferFromFile>(uri, DBMS_DEFAULT_BUFFER_SIZE, O_WRONLY | O_EXCL | O_CREAT);
    }
    else if (scheme == "tos")
    {
        if (out_uri.getQueryParameters().empty())
        {
            throw Exception("Missing access key, please check configuration.", ErrorCodes::BAD_ARGUMENTS);
        }
        out_buf_raw = std::make_unique<WriteBufferFromOwnString>();
    }
#if USE_HDFS
    else if (DB::isHdfsOrCfsScheme(scheme))
    {
        out_buf_raw = std::make_unique<WriteBufferFromHDFS>(
            uri,
            context->getHdfsConnectionParams(),
            context->getSettingsRef().max_hdfs_write_buffer_size,
            O_WRONLY,
            context->getSettingsRef().overwrite_current_file);

        // hdfs always use CompressionMethod::Gzip default
        if (compression_method_str.empty())
        {
            compression_method = CompressionMethod::Gzip;
        }
    }
#endif
#if USE_AWS_S3
    else if (scheme == "vetos")
    {
        VETosConnectionParams vetos_connect_params = VETosConnectionParams::getVETosSettingsFromContext(context);
        auto tos_uri = verifyTosURI(uri);
        std::string bucket = tos_uri.getHost();
        std::string key = getTosKeyFromURI(tos_uri);
        S3::S3Config s3_config = VETosConnectionParams::getS3Config(vetos_connect_params, bucket);
        out_buf_raw = std::make_unique<WriteBufferFromS3>(
            s3_config.create(),
            bucket,
            key,
            context->getSettingsRef().s3_min_upload_part_size,
            context->getSettingsRef().s3_max_single_part_upload_size);
    }
    // use s3 protocol to support outfile to oss
    else if (scheme == "oss")
    {
        OSSConnectionParams oss_connect_params = OSSConnectionParams::getOSSSettingsFromContext(context);
        auto oss_uri = verifyOSSURI(uri);
        std::string bucket = oss_uri.getHost();
        std::string key = getOSSKeyFromURI(oss_uri);
        S3::S3Config s3_config = OSSConnectionParams::getS3Config(oss_connect_params, bucket);
        out_buf = std::make_unique<WriteBufferFromS3>(
            s3_config.create(),
            bucket,
            key,
            context->getSettingsRef().s3_min_upload_part_size,
            context->getSettingsRef().s3_max_single_part_upload_size);
    }
    else if (isS3URIScheme(scheme))
    {
        S3::URI s3_uri(out_uri);
        String endpoint = s3_uri.endpoint.empty() ? context->getSettingsRef().s3_endpoint.toString() : s3_uri.endpoint;
        S3::S3Config s3_cfg(
            endpoint,
            context->getSettingsRef().s3_region.toString(),
            s3_uri.bucket,
            context->getSettingsRef().s3_ak_id,
            context->getSettingsRef().s3_ak_secret,
            "",
            "",
            context->getSettingsRef().s3_use_virtual_hosted_style);

        out_buf_raw = std::make_unique<WriteBufferFromS3>(
            s3_cfg.create(),
            s3_cfg.bucket,
            s3_uri.key,
            context->getSettingsRef().s3_min_upload_part_size,
            context->getSettingsRef().s3_max_single_part_upload_size);
    }
#endif
    else
    {
        throw Exception("Path: " + uri + " is illegal, scheme " + scheme + " is not supported.", ErrorCodes::ILLEGAL_OUTPUT_PATH);
    }

    out_buf = wrapWriteBufferWithCompressionMethod(std::move(out_buf_raw), compression_method, compression_level);

    return out_buf;
}

void OutfileTarget::flushFile(ContextMutablePtr context)
{
    if (auto * out_tos_buf = dynamic_cast<WriteBufferFromOwnString *>(out_buf.get()))
    {
        try
        {
            const Poco::URI out_uri(uri);
            std::string host = out_uri.getHost();
            auto port = out_uri.getPort();

            if (host.empty() || port == 0)
            {
                // choose a tos server randomly
                ServiceDiscoveryClientPtr service_discovery = std::make_shared<ServiceDiscoveryConsul>(context->getConfigRef());
                ServiceEndpoints tos_servers = service_discovery->lookupEndpoints(TOS_PSM);
                if (tos_servers.empty())
                    throw Exception("Can not find tos servers with PSM: " + String(TOS_PSM), ErrorCodes::NETWORK_ERROR);

                auto generator = std::mt19937(std::random_device{}());
                std::uniform_int_distribution<size_t> distribution(0, tos_servers.size() - 1);
                ServiceEndpoint tos_server = tos_servers.at(distribution(generator));
                host = tos_server.host;
                port = tos_server.port;
            }

            String tos_server = std::get<0>(safeNormalizeHost(host)) + ":" + std::to_string(port);

            Poco::URI tos_uri("http://" + tos_server + out_uri.getPath());
            String access_key = out_uri.getQueryParameters().at(0).second;
            HttpHeaders http_headers{{"X-Tos-Access", access_key}};

            const Settings & settings = context->getSettingsRef();
            ConnectionTimeouts timeouts(settings.http_connection_timeout, settings.http_send_timeout, settings.http_receive_timeout);

            HTTPSender http_sender(tos_uri, Poco::Net::HTTPRequest::HTTP_PUT, timeouts, http_headers);
            http_sender.send((*out_tos_buf).str());
            http_sender.handleResponse();
        }
        catch (...)
        {
            out_tos_buf->finalize();
            throw;
        }
    }
    else
    {
        out_buf->finalize();
    }
}

OutfileTargetPtr OutfileTarget::getOutfileTarget(
    const std::string & uri, const std::string & format, const std::string & compression_method_str, int compression_level)
{
    return std::make_unique<OutfileTarget>(uri, format, compression_method_str, compression_level);
}

void OutfileTarget::setOutfileCompression(
    const ASTQueryWithOutput * query_with_output, String & outfile_compression_method_str, UInt64 & outfile_compression_level)
{
    if (query_with_output->compression_method)
    {
        const auto & compression_method_node = query_with_output->compression_method->as<ASTLiteral &>();
        outfile_compression_method_str = compression_method_node.value.safeGet<std::string>();

        if (query_with_output->compression_level)
        {
            const auto & compression_level_node = query_with_output->compression_level->as<ASTLiteral &>();
            bool res = compression_level_node.value.tryGet<UInt64>(outfile_compression_level);
            if (!res)
            {
                throw Exception("Invalid compression level, must be in range or use default level without set", ErrorCodes::BAD_ARGUMENTS);
            }
        }
    }
}

bool OutfileTarget::checkOutfileWithTcpOnServer(const ContextMutablePtr & context)
{
    return context->getSettingsRef().outfile_in_server_with_tcp
        && context->getClientInfo().query_kind == ClientInfo::QueryKind::INITIAL_QUERY
        && context->getServerType() == ServerType::cnch_server;
}
}
