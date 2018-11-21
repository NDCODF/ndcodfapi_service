#include "tbl2sc.h"

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/RegularExpression.h>
#include <Poco/TemporaryFile.h>
#include <Poco/StringTokenizer.h>
#include <Poco/FileStream.h>
#include <Poco/Util/ServerApplication.h>

using Poco::Net::HTMLForm;
using Poco::Net::HTTPResponse;
using Poco::Path;
using Poco::File;
using Poco::TemporaryFile;
using Poco::StringTokenizer;
using Poco::Util::Application;

extern "C" Tbl2SC* create_object()
{
    return new Tbl2SC;
}

Tbl2SC::Tbl2SC()
{}

/// check if uri valid this ap.
bool Tbl2SC::isTbl2SCUri(std::string uri)
{
    StringTokenizer tokens(uri, "/?");
    return (tokens.count() == 3 && tokens[2] == "table2spreadsheet");
}

/// mimetype
std::string Tbl2SC::getMimeType()
{
    switch (mimetype)
    {
    case LOK_DOCTYPE_SPREADSHEET:
        return "application/vnd.oasis.opendocument.spreadsheet";
    default:
        return "application/pdf";
    }
}

/// validate for form args
bool Tbl2SC::validate(const HTMLForm &form,
                      std::weak_ptr<StreamSocket> _socket)
{
    HTTPResponse response;
    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Allow-Methods", "POST, OPTIONS");
    response.set("Access-Control-Allow-Headers",
        "Origin, X-Requested-With, Content-Type, Accept");

    auto socket = _socket.lock();

    if (!form.has("format"))
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
            "parameter format must assign");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    const std::string format = form.get("format");
    if (format != "ods" && format != "pdf")
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
            "wrong parameter format");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    if (!form.has("title"))
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
            "parameter title must assign");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    if (!form.has("content"))
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
            "parameter content must assign");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    const std::string title = form.get("title");
    std::string content = form.get("content");

    Poco::RegularExpression re(".*<table [^<]*>.*</table>.*",
        Poco::RegularExpression::RE_DOTALL);
    if (!re.match(content))
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
            "no <table>");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    return true;
}

/// convert using soffice.so
std::string Tbl2SC::outputODF(const std::string odffile,
                              const std::string format)
{
    if (odffile.empty())
    {
        return "";
    }

    std::string outfile;
    lok::Office *llo = NULL;
    try
    {
        llo = lok::lok_cpp_init(loPath.c_str());
        if (!llo)
        {
            std::cout << ": Failed to initialise LibreOfficeKit" << std::endl;
            return "";
        }
    }
    catch (const std::exception & e)
    {
        delete llo;
        std::cout << ": LibreOfficeKit threw exception (" << e.what() << ")" << std::endl;
        return "";
    }

    char *options = 0;
    lok::Document * lodoc = llo->documentLoad(odffile.c_str(), options);
    if (!lodoc)
    {
        const char * errmsg = llo->getError();
        std::cerr << ": LibreOfficeKit failed to load document (" << errmsg << ")" << std::endl;
        return "";
    }

    outfile = odffile + ".odf";
    //std::cout << outfile << std::endl;
    if (!lodoc->saveAs(outfile.c_str(), format.c_str(), options))
    {
        const char * errmsg = llo->getError();
        std::cerr << ": LibreOfficeKit failed to export (" << errmsg << ")" << std::endl;

        //Poco::File(outfile).remove(true);
        //std::cout << "remove: " << odffile << std::endl;

        delete lodoc;
        return "";
    }
    if (format == "pdf")
        mimetype = -1;
    else
    {
        lodoc = llo->documentLoad(outfile.c_str(), options);
        if (!lodoc)
            mimetype = -1;
        else
            mimetype = lodoc->getDocumentType();
    }

    return outfile;
}

/// 轉檔:
/// <table>...</table>  轉成  ods or pdf
void Tbl2SC::doConvert(const Poco::Net::HTTPRequest& request,
                       Poco::MemoryInputStream& message,
                       std::weak_ptr<StreamSocket> _socket)
{
    HTMLForm form(request, message);
    if (!validate(form, _socket))
        return;

    const std::string format = form.get("format");
    const std::string title = form.get("title");
    const std::string content = form.get("content");

    const std::string templ = R"MULTILINE(
<!doctype html>
<html lang="zh-tw">
<head>
    <title>%s</title>
    <meta charset="UTF-8">
    <meta http-equiv="X-UA-Compatible" content="IE=edge">
</head>
<body>
%s
</body>
</html>
)MULTILINE";
    auto buf = Poco::format(templ, title, content);
    //std::cout << buf <<std::endl;

    auto sourcefile = TemporaryFile::tempName() + ".xls";

    Poco::File f(sourcefile);
    f.createFile();

    Poco::FileOutputStream fos(sourcefile, std::ios::binary);
    fos << buf;
    fos.close();
    std::cout << sourcefile << std::endl;

    HTTPResponse response;
    auto socket = _socket.lock();

    Process::PID pid = fork();
    if (pid < 0)
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
            "error running table2spreadsheet");
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
            socket->send(response);
            socket->shutdown();
            _exit(Application::EXIT_SOFTWARE);
            return;
        }
        else if (pid > 0)
        {
            _exit(Application::EXIT_SOFTWARE);
            return;
        }
        else
        {
            auto outputfile = outputODF(sourcefile, format);
            if (outputfile.empty())
            {
                response.setStatusAndReason(
                    HTTPResponse::HTTP_BAD_REQUEST, "convert error");
                socket->send(response);
                socket->shutdown();
                _exit(Application::EXIT_SOFTWARE);
                return;
            }

            response.set("Access-Control-Allow-Origin", "*");
            response.set("Access-Control-Allow-Methods",
                "POST, OPTIONS");
            response.set("Access-Control-Allow-Headers",
                "Origin, X-Requested-With, Content-Type, Accept");

            HttpHelper::sendFile(socket, outputfile,
                getMimeType(), response);

            Poco::File(outputfile).remove(true);
            std::cout << "remove: " << outputfile << std::endl;

            _exit(Application::EXIT_SOFTWARE);
        }
    }
    else
    {
        //std::cout << "call from parent" << std::endl;
        waitpid(pid, NULL, 0); // 父程序呼叫waitpid(), 等待子程序終結,並捕獲返回狀態
    }
}
