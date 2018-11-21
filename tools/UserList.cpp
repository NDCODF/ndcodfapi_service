/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "config.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/FilePartSource.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/KeyConsoleHandler.h>
#include <Poco/Net/AcceptCertificateHandler.h>

#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SecureServerSocket.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Util/ServerApplication.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/ConsoleCertificateHandler.h>
#include <Poco/Net/SSLManager.h>

#include <Poco/StreamCopier.h>
#include <Poco/Thread.h>
#include <Poco/URI.h>
#include <Poco/StringTokenizer.h>
#include <Poco/Util/Application.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Data/SQLite/Connector.h>

#include "Common.hpp"
#include "Protocol.hpp"
#include "Util.hpp"

const int PORT = 9982;
const std::string ConfigFile = LOOLWSD_CONFIGDIR "/loolwsd.xml";
//std::string ConfigFile = "loolwsd.xml";

using namespace LOOLProtocol;

using Poco::Net::HTMLForm;
using Poco::Net::HTTPClientSession;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPResponse;
using Poco::Runnable;
using Poco::Thread;
using Poco::URI;
using Poco::Util::Application;
using Poco::Util::HelpFormatter;
using Poco::Util::Option;
using Poco::Util::OptionSet;
using namespace Poco::Data::Keywords;
using Poco::Data::Statement;
using Poco::Data::RecordSet;

class MyRequestHandler : public Poco::Net::HTTPRequestHandler
{
public:

    std::string getUsers(std::string doc_id)
    {
        Poco::Data::SQLite::Connector::registerConnector();
        const std::string dbfile = Poco::Path::temp() + "/docid.sqlite";
        if (!Poco::File(dbfile).exists())
        {
            return "[]";
        }

        Poco::Data::Session session("SQLite", dbfile);
        
        Statement select(session);
        std::string jsondata;
        select << "SELECT json FROM data WHERE doc_id=?", into(jsondata), use(doc_id);
        while (!select.done())
        {
            select.execute();
            std::cout << select.toString() << std::endl;
            break;
        }
        std::cout << doc_id << jsondata << std::endl;
        return jsondata;
    }

    virtual void handleRequest(HTTPServerRequest &req, HTTPServerResponse &resp)
    {
        if (req.getURI() == "/lool/get_users")
        {
            std::cout << "Post request: [" << req.getURI() << "]" << std::endl;
            Poco::StringTokenizer tokens(req.getURI(), "/?");
            for(size_t idx = 0; idx < tokens.count(); idx ++)
            {
                fprintf(stderr, "token[%zu] = %s\n", tokens.count(), tokens[idx].c_str());
            }

            resp.set("Access-Control-Allow-Origin", "*");
            resp.set("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            resp.set("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");
            HTMLForm form(req, req.stream());
            const std::string doc_id = (form.has("doc_id") ? form.get("doc_id") : "");

            if (doc_id.empty())
                resp.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
            else
                resp.setStatusAndReason(HTTPResponse::HTTP_OK);
            
            std::ostream& out = resp.send();
            out << getUsers(doc_id);
            out.flush();
            
            std::cout << doc_id << std::endl;
        }
        else
        {
            resp.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
            std::ostream& out = resp.send();
            out.flush();
        }
    }

private:
    static int count;
};

int MyRequestHandler::count = 0;

class MyRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
{
public:
  virtual Poco::Net::HTTPRequestHandler* createRequestHandler(const HTTPServerRequest &)
  {
    return new MyRequestHandler;
  }
};

/// Simple command-line tool for file format conversion.
class Tool: public Poco::Util::ServerApplication
{
public:
    Tool();

protected:
    int  main(const std::vector<std::string>& args) override;

    bool SSLTermination;
    bool SSLEnabled;

#if ENABLE_SSL
    void initializeSSL(void);
#endif
    /// Returns the value of the specified application configuration,
    /// of the default, if one doesn't exist.
    template<typename T>
    static
    T getConfigValue(const std::string& name, const T def)
    {
        return getConfigValue(Application::instance().config(), name, def);
    }

    /// Reads and processes path entries with the given property
    /// from the configuration.
    /// Converts relative paths to absolute.
    std::string getPathFromConfig(const std::string& property) const
    {
        auto path = config().getString(property);
        if (path.empty() && config().hasProperty(property + "[@default]"))
        {
            // Use the default value if empty and a default provided.
            path = config().getString(property + "[@default]");
        }

        // Reconstruct absolute path if relative.
        if (!Poco::Path(path).isAbsolute() &&
            config().hasProperty(property + "[@relative]") &&
            config().getBool(property + "[@relative]"))
        {
            path = Poco::Path(Application::instance().commandPath()).parent().append(path).toString();
        }

        return path;
    }
};

Tool::Tool()
{
}

#if ENABLE_SSL
void Tool::initializeSSL()
{
    const auto ssl_cert_file_path = getPathFromConfig("ssl.cert_file_path");
    const auto ssl_key_file_path = getPathFromConfig("ssl.key_file_path");
    const auto ssl_ca_file_path = getPathFromConfig("ssl.ca_file_path");

    std::cout << "SSL Cert file: " << ssl_cert_file_path << std::endl;
    std::cout << "SSL Key file: " << ssl_key_file_path << std::endl;
    std::cout << "SSL CA file: " << ssl_ca_file_path << std::endl;

    Poco::Crypto::initializeCrypto();

    Poco::Net::initializeSSL();
    Poco::Net::Context::Params sslParams;
    sslParams.certificateFile = ssl_cert_file_path;
    sslParams.privateKeyFile = ssl_key_file_path;
    sslParams.caLocation = ssl_ca_file_path;
    // Don't ask clients for certificate
    sslParams.verificationMode = Poco::Net::Context::VERIFY_NONE;

    Poco::SharedPtr<Poco::Net::PrivateKeyPassphraseHandler> consoleHandler = new Poco::Net::KeyConsoleHandler(true);
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> invalidCertHandler = new Poco::Net::ConsoleCertificateHandler(true);

    Poco::Net::Context::Ptr sslContext = new Poco::Net::Context(Poco::Net::Context::SERVER_USE, sslParams);
    Poco::Net::SSLManager::instance().initializeServer(consoleHandler, invalidCertHandler, sslContext);

    // Init client
    Poco::Net::Context::Params sslClientParams;
    // TODO: Be more strict and setup SSL key/certs for owncloud server and us
    sslClientParams.verificationMode = Poco::Net::Context::VERIFY_NONE;

    Poco::SharedPtr<Poco::Net::PrivateKeyPassphraseHandler> consoleClientHandler = new Poco::Net::KeyConsoleHandler(false);
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> invalidClientCertHandler = new Poco::Net::AcceptCertificateHandler(false);

    Poco::Net::Context::Ptr sslClientContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, sslClientParams);
    Poco::Net::SSLManager::instance().initializeClient(consoleClientHandler, invalidClientCertHandler, sslClientContext);
}
#endif

int Tool::main(const std::vector<std::string>& /*args*/)
{
    std::cout << "run..." << std::endl;

    // Load default configuration files, if present.
    if (loadConfiguration(PRIO_DEFAULT) == 0)
    {
        // Fallback to the LOOLWSD_CONFIGDIR or --config-file path.
        loadConfiguration(ConfigFile, PRIO_DEFAULT);
    }
#if ENABLE_SSL
    SSLEnabled = Application::instance().config().getBool("ssl.enable");
#else
    SSLEnabled = false;
#endif

#if ENABLE_SSL
    //SSLTermination = getConfigValue<bool>("ssl.termination", true);
    SSLTermination = Application::instance().config().getBool("ssl.termination");
#else
    SSLTermination = false;
#endif

#if ENABLE_SSL
    if (SSLEnabled || SSLTermination)
        initializeSSL();
#endif

    Poco::Net::ServerSocket socket;
    if (SSLEnabled || SSLTermination)
        socket = Poco::Net::SecureServerSocket(PORT);
    else
        socket = Poco::Net::ServerSocket(PORT);

    Poco::Net::HTTPServer s(new MyRequestHandlerFactory, socket, new Poco::Net::HTTPServerParams);

    s.start();
    std::cout << std::endl << "Server started" << std::endl;

    waitForTerminationRequest();  // wait for CTRL-C or kill

    std::cout << std::endl << "Shutting down..." << std::endl;
    s.stop();

    return Application::EXIT_OK;
}

int main(int argc, char** argv)
{
    Tool app;
    return app.run(argc, argv);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
