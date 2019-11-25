#include "templaterepo.h"

#include <sys/wait.h>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include <Poco/RegularExpression.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/PartHandler.h>
#include <Poco/Delegate.h>
#include <Poco/Zip/Compress.h>
#include <Poco/Zip/Decompress.h>
#include <Poco/Glob.h>
#include <Poco/TemporaryFile.h>
#include <Poco/Format.h>
#include <Poco/StringTokenizer.h>
#include <Poco/StreamCopier.h>
#include <Poco/DOM/DOMParser.h>
#include <Poco/DOM/Document.h>
#include <Poco/SAX/InputSource.h>
#include <Poco/DOM/DOMWriter.h>
#include <Poco/DOM/NodeList.h>
#include <Poco/DOM/Text.h>
#include <Poco/XML/XMLWriter.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Base64Decoder.h>
#include <Poco/Util/Application.h>
#include <Poco/DOM/DOMException.h>
#include <Poco/FileChannel.h>
#include <Poco/AsyncChannel.h>
#include <Poco/FormattingChannel.h>
#include <Poco/PatternFormatter.h>

#include <sqlite3.h> 
#include <Poco/Data/SQLite/Utility.h>

using Poco::Net::HTMLForm;
using Poco::Net::MessageHeader;
using Poco::Net::PartHandler;
using Poco::Net::HTTPResponse;
using Poco::RegularExpression;
using Poco::Zip::Compress;
using Poco::Zip::Decompress;
using Poco::Path;
using Poco::File;
using Poco::TemporaryFile;
using Poco::StreamCopier;
using Poco::StringTokenizer;
using Poco::XML::DOMParser;
using Poco::XML::DOMWriter;
using Poco::XML::Element;
using Poco::XML::InputSource;
using Poco::XML::NodeList;
using Poco::XML::Node;;
using Poco::Dynamic::Var;
using Poco::JSON::Object;
using Poco::DynamicStruct;
using Poco::JSON::Array;
using Poco::Util::Application;
using Poco::XML::XMLReader;
using namespace Poco::Data::Keywords;
using Poco::Data::Statement;
using Poco::Data::RecordSet;
using Poco::Data::Session;
using Poco::Data::SQLite::Utility;

using Poco::FileChannel;
using Poco::PatternFormatter;

extern "C" TemplateRepo* create_object()
{
    return new TemplateRepo;
}

extern "C" void destroy_object(TemplateRepo* templateRepo)
{
    delete templateRepo;
}

TemplateRepo::TemplateRepo()
{}

TemplateRepo::~TemplateRepo()
{}

std::string TemplateRepo::makeApiJson(std::string URL,
        bool anotherJson,
        bool yaml,
        bool showHead)
{
    if(anotherJson && yaml && showHead)
        std::cout << URL << std::endl;
    return URL;
}

void TemplateRepo::handleAPIHelp(const Poco::Net::HTTPRequest& request, std::weak_ptr<StreamSocket> _socket)
{
    LOG_DBG("Api-help request: " << request.getURI());

    const auto& app = Poco::Util::Application::instance();
    const auto ServerName = app.config().getString("server_name");
#if ENABLE_DEBUG
    std::cout << "Skip checking the server_name...";
#else
    // 檢查是否有填 server_name << restful client 依據此作為呼叫之 url
    // url 帶入 TEMPL 之 "host"
    if (app.config().getString("server_name").empty())
    {
        auto socket = _socket.lock();
        HTTPResponse response;
        response.setStatusAndReason(
                HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                "config server_name not set");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return;
    }
#endif

    auto mediaType = "text/plain";

    std::string result = Poco::format(YAMLTEMPL, ServerName);
    // TODO: Refactor this to some common handler.
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Last-Modified: " << Poco::DateTimeFormatter::format(Poco::Timestamp(), Poco::DateTimeFormat::HTTP_FORMAT) << "\r\n"
        << "Access-Control-Allow-Origin: *" << "\r\n"
        << "User-Agent: " << WOPI_AGENT_STRING << "\r\n"
        << "Content-Length: " << result.size() << "\r\n"
        << "Content-Type: " << mediaType << "\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << "\r\n"
        << result;

    auto socket = _socket.lock();
    socket->send(oss.str());
    //socket->send(read);
    socket->shutdown();
    LOG_INF("Sent api json successfully.");

}

bool TemplateRepo::isTemplateRepoHelpUri(std::string URL)
{

    if(URL == "/lool/templaterepo/api")
        return true;
    else
        return false;
}

void TemplateRepo::getInfoFile(std::weak_ptr<StreamSocket> _socket)
{
    std::string infoFilePath = "/usr/share/NDCODFAPI/ODFReport/templates/repo/myfile.json";
    HTTPResponse response;
    auto socket = _socket.lock();
    std::string mimeType = "application/json";

    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Allow-Methods", "GET, OPTIONS");
    response.set("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");
    HttpHelper::sendFile(socket, infoFilePath, mimeType, response);
    socket->shutdown();

}

void TemplateRepo::downloadAllTemplates(std::weak_ptr<StreamSocket> _socket)
{
    std::string zipFilePath = zip_ALL_FILE();
    HTTPResponse response;
    auto socket = _socket.lock();
    std::string mimeType = "application/zip";

    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Allow-Methods", "GET, OPTIONS");
    response.set("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");
    HttpHelper::sendFile(socket, zipFilePath, mimeType, response);
    socket->shutdown();
}

void TemplateRepo::syncTemplates(std::weak_ptr<StreamSocket> _socket,
        const Poco::Net::HTTPRequest& request,
        Poco::MemoryInputStream& message)
{
    Object::Ptr object;

    /*
     * Swagger's CORS would send OPTIONS first to check if the server allow CROS, So Check First OPTIONS and allow 
     */
    if (request.getMethod() == HTTPRequest::HTTP_OPTIONS)
    {
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Last-Modified: " << Poco::DateTimeFormatter::format(Poco::Timestamp(), Poco::DateTimeFormat::HTTP_FORMAT) << "\r\n"
            << "Access-Control-Allow-Origin: *" << "\r\n"
            << "Access-Control-Allow-Headers: Origin, X-Requested-With, Content-Type, Accept" << "\r\n"
            << "User-Agent: " << WOPI_AGENT_STRING << "\r\n"
            << "Content-Type: application/json; charset=utf-8\r\n"
            << "X-Content-Type-Options: nosniff\r\n"
            << "\r\n";
        auto socket = _socket.lock();
        socket->send(oss.str());
        socket->shutdown();
        return;
    }
    if (request.getContentType() == "application/json")
    {
        std::string line, data;
        std::istream &iss(message);
        while (!iss.eof())
        {
            std::getline(iss, line);
            data += line;
        }
        // 解析 request body to json
        std::string jstr = data;

        Poco::JSON::Parser jparser;
        Var result;

        // Parse data to PocoJSON 
        try{
            result = jparser.parse(jstr);
            object = result.extract<Object::Ptr>();
        }
        catch (Poco::Exception& e)
        {
            std::cerr << e.displayText() << std::endl;
            std::string rrr = "JSON Error\n";
            std::ostringstream oss;
            oss << "HTTP/1.1 401 JSON ERROR\r\n"
                << "Last-Modified: " << Poco::DateTimeFormatter::format(Poco::Timestamp(), Poco::DateTimeFormat::HTTP_FORMAT) << "\r\n"
                << "Access-Control-Allow-Origin: *" << "\r\n"
                << "User-Agent: " << WOPI_AGENT_STRING << "\r\n"
                << "Content-Length: " << rrr.size() << "\r\n"
                << "Content-Type: application/json; charset=utf-8\r\n"
                << "X-Content-Type-Options: nosniff\r\n"
                << "\r\n"
                << rrr;
            auto socket = _socket.lock();
            socket->send(oss.str());
            socket->shutdown();
            return;
        }
    }
    else
    {
        std::string rrr = "Content Type Error";
        std::ostringstream oss;
        oss << "HTTP/1.1 401 Error\r\n"
            << "Last-Modified: " << Poco::DateTimeFormatter::format(Poco::Timestamp(), Poco::DateTimeFormat::HTTP_FORMAT) << "\r\n"
            << "Access-Control-Allow-Origin: *" << "\r\n"
            << "Access-Control-Allow-Headers: Origin, X-Requested-With, Content-Type, Accept" << "\r\n"
            << "User-Agent: " << WOPI_AGENT_STRING << "\r\n"
            << "Content-Length: " << rrr.size() << "\r\n"
            << "Content-Type: application/json; charset=utf-8\r\n"
            << "X-Content-Type-Options: nosniff\r\n"
            << "\r\n"
            << rrr;
        auto socket = _socket.lock();
        socket->send(oss.str());
        socket->shutdown();
        return;
    }

    std::string zipFilePath = zip_DIFF_FILE(object);
    std::cout << zipFilePath << std::endl;
    HTTPResponse response;
    auto socket = _socket.lock();
    std::string mimeType = "application/octet-stream";

    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Allow-Methods", "POST, OPTIONS");
    response.set("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");
    response.set("Content-Disposition", "attachment; filename=\"sync.zip\"");
    HttpHelper::sendFile(socket, zipFilePath, mimeType, response);
    socket->shutdown();
}

void createDirectory(std::string filePath)
{
    Poco::File dir(Poco::Path(filePath, Poco::Path::PATH_UNIX));
    dir.createDirectories();
}

Object::Ptr JSON_FROM_FILE(){
    std::ifstream inFile;
    std::string line;
    inFile.open("/usr/share/NDCODFAPI/ODFReport/templates/repo/myfile.json");
    while(std::getline(inFile, line))
    {
        std::cout << line << "\n";
    }
    Poco::JSON::Parser parser;
    Var result = parser.parse(line);
    Object::Ptr object;
    object = result.extract<Object::Ptr>();
    return object;
}
std::string zip_ALL_FILE()
{
    //Create temp dir
    //
    //
    std::string templatePath = "/usr/share/NDCODFAPI/ODFReport/templates/repo/";

    std::string extra2;
    extra2 = TemporaryFile::tempName();
    createDirectory(extra2);
    Object::Ptr object;
    object = JSON_FROM_FILE();
    for(auto it = object->begin();it!=object->end();it++){
        createDirectory(extra2 + "/" + it->first);
        Array::Ptr templArray = object->getArray(it->first);
        std::string extraPath = extra2 + "/" + it->first + "/";
        for(auto tp = templArray->begin();tp!=templArray->end();tp++)
        {
            Object::Ptr oData = tp->extract<Object::Ptr>();
            std::string endpt = oData->getValue<std::string>("endpt");
            std::string docname = oData->getValue<std::string>("docname");
            std::string extname = oData->getValue<std::string>("extname");
            std::string oFilePath = templatePath + endpt + "." + extname;
            std::string nFilePath = extra2 + "/" + it->first + "/" + docname + "." + extname;
            std::string oFileName = endpt + "." + extname;
            std::string nFileName = docname + "." + extname;
            Poco::File oFile(templatePath+endpt + "." + extname);
            oFile.copyTo(extraPath);
            Poco::File nFile(extraPath + oFileName);
            nFile.renameTo(extraPath + nFileName);
        }

    }

    //zip the dir
    const auto zip2 = extra2 + ".zip";

    std::ofstream out(zip2, std::ios::binary);
    Compress c(out, true);

    c.addRecursive(extra2);
    c.close();
    return zip2;
}


std::string zip_DIFF_FILE(Object::Ptr object)
{
    //Create temp dir
    std::string templatePath = "/usr/share/NDCODFAPI/ODFReport/templates/repo/";

    std::string extra2;
    extra2 = TemporaryFile::tempName();
    createDirectory(extra2);
    for(auto it = object->begin();it!=object->end();it++){
        createDirectory(extra2 + "/" + it->first);
        Array::Ptr templArray = object->getArray(it->first);
        std::string extraPath = extra2 + "/" + it->first + "/";
        for(auto tp = templArray->begin();tp!=templArray->end();tp++)
        {
            Object::Ptr oData = tp->extract<Object::Ptr>();
            std::string endpt = oData->getValue<std::string>("endpt");
            std::string docname = oData->getValue<std::string>("docname");
            std::string extname = oData->getValue<std::string>("extname");
            std::string oFilePath = templatePath + endpt + "." + extname;
            std::string nFilePath = extra2 + "/" + it->first + "/" + docname + "." + extname;
            std::string oFileName = endpt + "." + extname;
            std::string nFileName = docname + "." + extname;
            Poco::File oFile(templatePath+endpt + "." + extname);
            oFile.copyTo(extraPath);
            Poco::File nFile(extraPath + oFileName);
            nFile.renameTo(extraPath + nFileName);
        }

    }

    //zip the dir
    const auto zip2 = extra2 + ".zip";

    std::ofstream out(zip2, std::ios::binary);
    Compress c(out, true);

    c.addRecursive(extra2);
    c.close();
    return zip2;
}
void TemplateRepo::doTemplateRepo(std::weak_ptr<StreamSocket> _socket, const Poco::Net::HTTPRequest& request, Poco::MemoryInputStream& message)
{

    HTTPResponse response;
    auto socket = _socket.lock();

    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Allow-Methods", "POST, OPTIONS");
    response.set("Access-Control-Allow-Headers",
            "Origin, X-Requested-With, Content-Type, Accept");

    Process::PID pid = fork();
    if (pid < 0)
    {
        response.setStatusAndReason
            (HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
             "error loading mergeodf");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        _exit(Application::EXIT_SOFTWARE);
        return;
    }
    else if (pid == 0)
    {
        if ((pid = fork()) < 0)
        {
            response.setStatusAndReason(
                    HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                    "error loading mergeodf");
            response.setContentLength(0);
            socket->send(response);
            socket->shutdown();
            _exit(Application::EXIT_SOFTWARE);
            return;
        }
        else if (pid > 0)
        {
            if (request.getMethod() == HTTPRequest::HTTP_GET &&
                    request.getURI()=="/lool/templaterepo/list")
            {
                getInfoFile(socket);
            }
            else if (request.getMethod() == HTTPRequest::HTTP_GET &&
                     request.getURI()=="/lool/templaterepo/download")
            {
                downloadAllTemplates(socket);
            }
            else if ((request.getMethod() == HTTPRequest::HTTP_POST ||
                      request.getMethod() == HTTPRequest::HTTP_OPTIONS) &&
                     request.getURI()=="/lool/templaterepo/sync")
            {
                syncTemplates(socket, request, message);
            }
            else if (request.getMethod() == HTTPRequest::HTTP_GET &&
                     isTemplateRepoHelpUri(request.getURI()))
            {  // /lool/tempalterepo/api
                handleAPIHelp(request, socket);
            }
            else
            {
                response.setStatusAndReason(
                        HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                        "No such Route");
                response.setContentLength(0);
                socket->send(response);
                socket->shutdown();
                _exit(Application::EXIT_SOFTWARE);
                return;
            }
            _exit(Application::EXIT_SOFTWARE);
            return;
        }
        else
        {
            _exit(Application::EXIT_SOFTWARE);
        }
    }
    else
    {
        std::cout << "call from parent" << std::endl;
        waitpid(pid, NULL, 0); // 父程序呼叫waitpid(), 等待子程序終結,並捕獲返回狀態
    }
}
